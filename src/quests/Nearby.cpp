#include "Nearby.h"
#include "Boxes.h"
#include "History.h"
#include "InputHook.h"
#include "Overlay.h"
#include "Pins.h"
#include "QuestReader.h"
#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <tuple>

// Nearby quests. Goal: remind the player of quests they could do where they are.
//   Towns (0.10.0): when the player's "area" changes (Quests::AreaOf: a town/city/settlement, else the dungeon or site
//   under the hold; walking into a house inside a town does not change it):
//     - leaving an area with open objectives there -> "Leaving Riften: 2 objectives still here (The Raid, ...)"
//     - arriving in an area with open objectives   -> "2 quests here in Riften (press J)"
//   Distance (0.10.4, replaces the hold messages): outdoors, an open objective whose target is within iNearbyRadius
//   metres -> "Nearby: Hunt and Gather: Retrieve Wylandriah's Spoon (150 m)". Once per approach: it can come again
//   only after the player has been more than 1.5x the radius away. An indoor target counts as its way out (door).
//   Checks run on every location change AND every cell the player enters (0.10.4: walking out through a walled
//   city's gate changes only the worldspace, which fires no location change).
//   Quest updates (0.11.0): a quest's stage changes, and a new open objective of it is in this town or within the
//   radius -> "Updated \u00b7 here in Whiterun" / "Updated \u00b7 120 m".
// Tracked and untracked quests both count. 0.11.0: each quest is its own box (Overlay::Toast, in the overlay's page),
// or with iBoxStyle=0 the game's own top-left notification as before. Messages raised while a loading screen is up
// are held until 2 s after it closes (the page also holds boxes while any menu is open). Objectives with no quest
// marker have no target, so they cannot be counted (same as Show on map).
namespace QSO::Nearby
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // Main thread only.
        std::uint32_t g_area = 0;  // the player's current area as last seen
        std::string   g_areaName;
        std::uint32_t g_location = 0;  // last location and worldspace seen, for the log
        std::uint32_t g_world = 0;
        // (place, 0 leave area / 1 arrive area) -> when that message was last shown
        std::map<std::pair<std::uint32_t, int>, Clock::time_point> g_lastShown;
        // Objectives already reminded of on this approach: (quest, instance, objective index).
        std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint16_t>> g_reminded;

        // One message: the game-notification text (iBoxStyle=0, or no page) and the same thing as one box per quest
        // (JSON members for Overlay::Toast).
        struct Message
        {
            std::string              vanilla;
            std::vector<std::string> boxes;
        };

        // Loading screens: messages raised behind one are held and shown 2 s after it closes.
        bool                 g_loading = false;
        Clock::time_point    g_holdUntil{};
        std::vector<Message> g_pending;
        std::uint32_t        g_loadGeneration = 0;

        // Quest updates (0.11.0): open objectives per quest instance as last seen, and the quests whose stage changed
        // since the last update check.
        using QuestKey = std::pair<std::uint32_t, std::uint32_t>;
        std::map<QuestKey, std::set<std::uint16_t>> g_baseline;
        bool                                        g_baselineValid = false;
        std::set<std::uint32_t>                     g_stageQuests;
        std::uint32_t                               g_stageGeneration = 0;

        std::atomic<bool> g_checkQueued{ false };

        bool CoolingDown(std::uint32_t a_area, int a_kind)
        {
            const auto seconds = Settings::Get().nearbyCooldown;
            const auto key = std::make_pair(a_area, a_kind);
            const auto now = Clock::now();
            const auto it = g_lastShown.find(key);
            if (seconds > 0 && it != g_lastShown.end() && now - it->second < std::chrono::seconds(seconds)) {
                return true;
            }
            g_lastShown[key] = now;
            return false;
        }

        std::string KeyName(std::uint32_t a_scan)
        {
            static const char* letters[0x40] = {};
            static bool        init = false;
            if (!init) {
                const char* row1 = "QWERTYUIOP";
                const char* row2 = "ASDFGHJKL";
                const char* row3 = "ZXCVBNM";
                static char buf[0x40][2] = {};
                auto        put = [&](std::uint32_t a_start, const char* a_keys) {
                    for (std::uint32_t i = 0; a_keys[i]; ++i) {
                        buf[a_start + i][0] = a_keys[i];
                        letters[a_start + i] = buf[a_start + i];
                    }
                };
                put(0x10, row1);
                put(0x1E, row2);
                put(0x2C, row3);
                init = true;
            }
            if (a_scan < 0x40 && letters[a_scan]) {
                return letters[a_scan];
            }
            if (a_scan >= 0x3B && a_scan <= 0x44) {
                return std::format("F{}", a_scan - 0x3A);
            }
            if (a_scan == 0x57 || a_scan == 0x58) {
                return std::format("F{}", a_scan - 0x4C);
            }
            return "your journal key";
        }

        // One line per quest with work in the place: the quest name, or the objective itself when there is just one
        // ("Bleak Falls Barrow: Talk to Farengar"), so it is clear WHAT is here. Unnamed quests (radiant favours) go by
        // their objective text, like the vanilla journal's Misc list.
        struct Item
        {
            std::string label;  // the 0.10.x one-line form, for the game's own notification
            std::string quest;  // box title: the quest name, or the objective for an unnamed quest
            std::string line;   // the (first) objective here; empty when it is the title already
            std::string where;  // its place ("The Drunken Huntsman")
            int         more = 0;  // further open objectives of the same quest here
        };

        struct Work
        {
            std::vector<Item> items;
            int               objectives = 0;
        };

        Item MakeItem(const Quests::Entry& a_e, const std::vector<const Quests::Objective*>& a_hits)
        {
            Item        it;
            const auto& o = *a_hits.front();
            if (a_e.name.empty()) {
                it.label = o.text;
                it.quest = o.text;
            } else {
                it.label = a_hits.size() == 1 ? a_e.name + ": " + o.text : a_e.name;
                it.quest = a_e.name;
                it.line = o.text;
            }
            it.where = o.place.empty() ? o.town : o.place;
            it.more = static_cast<int>(a_hits.size()) - 1;
            return it;
        }

        std::string BoxJson(const char* a_kind, const std::string& a_head, const Item& a_item, const std::string& a_key)
        {
            return std::format(R"("kind":"{}","head":{},"quest":{},"line":{},"where":{},"more":{},"key":{})", a_kind,
                Quests::JsonString(a_head), Quests::JsonString(a_item.quest), Quests::JsonString(a_item.line),
                Quests::JsonString(a_item.where), a_item.more, Quests::JsonString(a_key));
        }

        template <class Pred>
        Work WorkWhere(const Quests::Snapshot& a_snap, Pred a_pred)
        {
            Work w;
            const bool misc = Settings::Get().nearbyMisc;
            for (const auto& e : a_snap.entries) {
                if (e.completed || e.failed || (!misc && (e.type == 6 || e.type == 0)) || Pins::IsHidden(e.formID)) {
                    continue;
                }
                std::vector<const Quests::Objective*> hits;
                for (const auto& o : e.objectives) {
                    if (o.state == 1 && a_pred(o)) {
                        hits.push_back(&o);
                    }
                }
                if (hits.empty()) {
                    continue;
                }
                w.items.push_back(MakeItem(e, hits));
                w.objectives += static_cast<int>(hits.size());
            }
            return w;
        }

        std::string NameList(const std::vector<Item>& a_items)
        {
            std::string       out;
            const std::size_t shown = std::min<std::size_t>(a_items.size(), 3);
            for (std::size_t i = 0; i < shown; ++i) {
                out += (i ? "; " : "") + a_items[i].label;
            }
            if (a_items.size() > shown) {
                out += std::format("; and {} more", a_items.size() - shown);
            }
            return out;
        }

        // Shows a message now: as QSO boxes (one per quest), or as the game's own notification when iBoxStyle=0 or
        // the page cannot show boxes. 0.16.0: with game notifications on, always boxes (the game's own line would be
        // caught and boxed anyway; the Settings page greys the Style choice out then).
        void Deliver(const Message& a_msg, const char* a_when)
        {
            const bool boxes = (Settings::Get().boxStyle != 0 || Boxes::LinesActive()) && !a_msg.boxes.empty();
            if (boxes && Overlay::Toast(a_msg.boxes.front())) {
                for (std::size_t i = 1; i < a_msg.boxes.size(); ++i) {
                    Overlay::Toast(a_msg.boxes[i]);
                }
                logger::info("Nearby: shown{} as {} box(es): \"{}\"", a_when, a_msg.boxes.size(), a_msg.vanilla);
                return;
            }
            RE::SendHUDMessage::ShowHUDMessage(a_msg.vanilla.c_str());
            if (!Boxes::CaptureHooked()) {  // with the hook in, it lists the game's line itself
                for (const auto& b : a_msg.boxes) {
                    History::AddQuest(b, "vanilla");
                }
            }
            logger::info("Nearby: shown{} \"{}\"{}", a_when, a_msg.vanilla,
                boxes ? " (game notification: the page cannot show boxes)" : "");
        }

        void Notify(Message a_msg)
        {
            if (g_loading || Clock::now() < g_holdUntil) {
                logger::info("Nearby: held until the loading screen is gone: \"{}\"", a_msg.vanilla);
                g_pending.push_back(std::move(a_msg));
                return;
            }
            Deliver(a_msg, "");
        }

        void Show(std::uint32_t a_place, int a_kind, const std::string& a_label, Message a_msg)
        {
            if (CoolingDown(a_place, a_kind)) {
                logger::info("Nearby: {} message for '{}' skipped (shown less than {} s ago).", a_kind == 0 ? "leaving" : "arrival",
                    a_label, Settings::Get().nearbyCooldown);
                return;
            }
            Notify(std::move(a_msg));
        }

        std::string Metres(float a_units) { return std::format("{} m", static_cast<int>(std::lround(a_units / 10.0f)) * 10); }

        std::map<QuestKey, std::set<std::uint16_t>> OpenObjectives(const Quests::Snapshot& a_snap)
        {
            std::map<QuestKey, std::set<std::uint16_t>> out;
            for (const auto& e : a_snap.entries) {
                auto& open = out[{ e.formID, e.instanceID }];
                for (const auto& o : e.objectives) {
                    if (o.state == 1) {
                        open.insert(o.index);
                    }
                }
            }
            return out;
        }

        std::string LocName(RE::BGSLocation* a_loc)
        {
            return a_loc ? std::format("{:08X} '{}'", a_loc->GetFormID(), a_loc->GetFullName() ? a_loc->GetFullName() : "") : "none";
        }

        std::string WorldName(RE::TESWorldSpace* a_world)
        {
            return a_world ? std::format("{:08X} '{}'", a_world->GetFormID(), a_world->GetFullName() ? a_world->GetFullName() : "") :
                             std::string("an interior");
        }

        bool Counts(const Quests::Entry& a_entry)
        {
            return !a_entry.completed && !a_entry.failed && (Settings::Get().nearbyMisc || (a_entry.type != 6 && a_entry.type != 0)) &&
                   !Pins::IsHidden(a_entry.formID);  // 0.14.0: a hidden quest is silent
        }

        void Check(const char* a_why)
        {
            g_checkQueued = false;
            const auto player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->GetParentCell()) {
                return;
            }
            const auto& opt = Settings::Get();
            const auto  loc = player->GetCurrentLocation();
            const auto  world = player->GetWorldspace();
            const bool  outdoors = !player->GetParentCell()->IsInteriorCell();
            const auto  here = Quests::AreaOf(loc, player);
            const auto  locID = loc ? loc->GetFormID() : 0;
            const auto  worldID = world ? world->GetFormID() : 0;
            const bool  moved = locID != g_location || worldID != g_world;
            g_location = locID;
            g_world = worldID;

            const bool areaChanged = here.area != g_area;
            const bool nearOn = opt.notifyNear && outdoors;
            if (!areaChanged && !nearOn) {
                if (moved) {
                    logger::info("Nearby: now at {} in {} ({}); still area {:08X} '{}'.", LocName(loc), WorldName(world), a_why,
                        here.area, here.areaName);
                }
                return;
            }
            const auto start = Clock::now();
            const auto snap = Quests::Read(true);
            if (!snap.playerListValid) {
                logger::warn("Nearby: quest list not readable ({}); no message.", snap.problem);
                return;
            }
            const auto key = KeyLabel();
            if (!g_baselineValid) {
                g_baseline = OpenObjectives(snap);
                g_baselineValid = true;
            }

            // ---- Towns ----
            const auto oldArea = g_area;
            const auto oldName = g_areaName.empty() ? std::string("this place") : g_areaName;
            const auto newName = here.areaName.empty() ? std::string("this place") : here.areaName;
            g_area = here.area;
            g_areaName = here.areaName;
            if (areaChanged) {
                const bool leave = opt.notifyLeave && oldArea != 0;
                const bool arrive = opt.notifyArrive && here.area != 0;
                const auto left = leave ? WorkWhere(snap, [&](const Quests::Objective& o) { return o.area == oldArea; }) : Work{};
                const auto ahead = arrive ? WorkWhere(snap, [&](const Quests::Objective& o) { return o.area == here.area; }) : Work{};
                logger::info("Nearby: now at {} in {} ({}); area {:08X} '{}' -> {:08X} '{}': left behind {}, ahead {}.", LocName(loc),
                    WorldName(world), a_why, oldArea, oldName, here.area, newName, left.objectives, ahead.items.size());
                if (left.objectives) {
                    Message m{ std::format("Leaving {}: {} objective{} still here ({})", oldName, left.objectives,
                        left.objectives == 1 ? "" : "s", NameList(left.items)), {} };
                    for (const auto& it : left.items) {
                        m.boxes.push_back(BoxJson("leave", "Leaving " + oldName + " \u00b7 still here", it, key));
                    }
                    Show(oldArea, 0, oldName, std::move(m));
                }
                if (!ahead.items.empty()) {
                    const auto n = ahead.items.size();
                    Message    m{ std::format("{} quest{} here in {}: {} (press {})", n, n == 1 ? "" : "s", newName,
                        NameList(ahead.items), key), {} };
                    for (const auto& it : ahead.items) {
                        m.boxes.push_back(BoxJson("arrive", "Here in " + newName, it, key));
                    }
                    Show(here.area, 1, newName, std::move(m));
                }
            } else if (moved) {
                logger::info("Nearby: now at {} in {} ({}); still area {:08X} '{}'.", LocName(loc), WorldName(world), a_why, here.area,
                    here.areaName);
            }

            // ---- Distance (outdoors only) ----
            if (!nearOn) {
                return;
            }
            const auto radius = static_cast<float>(opt.nearbyRadius);
            struct Hit
            {
                float metres;
                Item  item;
            };
            std::vector<Hit> hits;
            for (const auto& e : snap.entries) {
                if (!Counts(e)) {
                    continue;
                }
                for (const auto& o : e.objectives) {
                    if (o.state != 1) {
                        continue;
                    }
                    const auto id = std::make_tuple(e.formID, e.instanceID, o.index);
                    if (here.area && o.area == here.area) {
                        g_reminded.insert(id);  // the town message covers it; no second reminder on the way out
                        continue;
                    }
                    if (o.distance < 0) {
                        continue;
                    }
                    if (o.distance <= radius) {
                        if (g_reminded.insert(id).second) {
                            auto item = MakeItem(e, { &o });
                            item.label = e.name.empty() ? o.text : e.name + ": " + o.text;
                            hits.push_back({ o.distance, std::move(item) });
                        }
                    } else if (o.distance > radius * 1.5f) {
                        g_reminded.erase(id);
                    }
                }
            }
            if (hits.empty()) {
                return;
            }
            std::ranges::sort(hits, {}, &Hit::metres);
            std::vector<Item> items;
            Message           m;
            for (const auto& h : hits) {
                items.push_back(h.item);
                items.back().label = std::format("{} ({})", h.item.label, Metres(h.metres));
                m.boxes.push_back(BoxJson("near", "Nearby \u00b7 " + Metres(h.metres), h.item, key));
            }
            logger::info("Nearby: {} objective(s) newly within {} m ({:.1f} ms).", hits.size(), opt.nearbyRadius,
                std::chrono::duration<double, std::milli>(Clock::now() - start).count());
            m.vanilla = hits.size() == 1 ? std::format("Nearby: {} (press {})", items.front().label, key) :
                                           std::format("{} objectives nearby: {} (press {})", hits.size(), NameList(items), key);
            Notify(std::move(m));
        }

        void QueueCheck(const char* a_why)
        {
            if (!g_checkQueued.exchange(true)) {
                SKSE::GetTaskInterface()->AddTask([a_why] { Check(a_why); });
            }
        }

        // 0.11.0: runs 1.5 s after the last quest stage change (stage fragments set the new objectives in Papyrus,
        // shortly after the stage itself). A new open objective of a changed quest that is in this town or within the
        // radius gets an "Updated" box. Objectives that appeared some other way are taken into the baseline silently.
        void UpdateCheck(std::uint32_t a_generation)
        {
            if (a_generation != g_stageGeneration) {
                return;  // another stage change came in; its own check runs later
            }
            const auto changed = std::move(g_stageQuests);
            g_stageQuests.clear();
            const auto snap = Quests::Read(true);
            if (!snap.playerListValid) {
                logger::warn("Quest update check: quest list not readable ({}).", snap.problem);
                return;
            }
            const bool compare = g_baselineValid;
            const auto before = std::move(g_baseline);
            g_baseline = OpenObjectives(snap);
            g_baselineValid = true;
            if (!compare) {
                logger::info("Quest update check: first look at the quest list; nothing to compare yet.");
                return;
            }
            const auto&       opt = Settings::Get();
            const auto        radius = static_cast<float>(opt.nearbyRadius);
            const auto        key = KeyLabel();
            const auto        hereName = snap.hereAreaName.empty() ? std::string("this place") : snap.hereAreaName;
            Message           m;
            std::vector<Item> items;
            int               fresh = 0;
            for (const auto& e : snap.entries) {
                if (!changed.contains(e.formID) || !Counts(e)) {
                    continue;
                }
                const auto                            it = before.find({ e.formID, e.instanceID });
                std::vector<const Quests::Objective*> hits;
                std::string                           head;
                for (const auto& o : e.objectives) {
                    if (o.state != 1 || (it != before.end() && it->second.contains(o.index))) {
                        continue;
                    }
                    ++fresh;
                    const bool inTown = snap.hereArea && o.area == snap.hereArea;
                    const bool close = o.distance >= 0 && o.distance <= radius;
                    if (!inTown && !close) {
                        continue;
                    }
                    if (hits.empty()) {
                        head = inTown ? "here in " + hereName : Metres(o.distance);
                    }
                    hits.push_back(&o);
                    g_reminded.insert({ e.formID, e.instanceID, o.index });  // no second "Nearby" box for it
                }
                if (hits.empty()) {
                    continue;
                }
                auto item = MakeItem(e, hits);
                item.label = std::format("{} ({})", item.label, head);
                m.boxes.push_back(BoxJson("updated", "Updated \u00b7 " + head, item, key));
                items.push_back(std::move(item));
            }
            logger::info("Quest update check: {} quest(s) changed stage, {} new open objective(s), {} here or nearby{}.",
                changed.size(), fresh, items.size(), opt.notifyUpdates ? "" : " (bNotifyUpdates=0: no message)");
            if (items.empty() || !opt.notifyUpdates) {
                return;
            }
            m.vanilla = items.size() == 1 ? std::format("Updated: {} (press {})", items.front().label, key) :
                                            std::format("{} quests updated here: {} (press {})", items.size(), NameList(items), key);
            Notify(std::move(m));
        }

        class StageSink : public RE::BSTEventSink<RE::TESQuestStageEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStageEvent* a_event, RE::BSTEventSource<RE::TESQuestStageEvent>*) override
            {
                if (!a_event) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const auto formID = a_event->formID;
                SKSE::GetTaskInterface()->AddTask([formID] {
                    g_stageQuests.insert(formID);
                    const auto generation = ++g_stageGeneration;
                    std::thread([generation] {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                        SKSE::GetTaskInterface()->AddTask([generation] { UpdateCheck(generation); });
                    }).detach();
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        StageSink g_stageSink;

        class LocationSink : public RE::BSTEventSink<RE::TESActorLocationChangeEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* a_event,
                RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override
            {
                if (a_event && a_event->actor && a_event->actor->IsPlayerRef()) {
                    QueueCheck("location changed");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        LocationSink g_sink;

        // 0.10.4: the player's cell changes. Fires on the player's own event source (one per cell entered).
        class CellSink : public RE::BSTEventSink<RE::BGSActorCellEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
            {
                if (a_event && a_event->flags.get() == RE::BGSActorCellEvent::CellFlag::kEnter) {
                    QueueCheck("cell changed");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        CellSink g_cellSink;

        void FlushPending(std::uint32_t a_generation)
        {
            if (a_generation != g_loadGeneration || g_loading) {
                return;  // another loading screen came in between; its own close flushes
            }
            for (const auto& msg : g_pending) {
                Deliver(msg, " (after the loading screen)");
            }
            g_pending.clear();
        }

        class LoadingSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!a_event || a_event->menuName != RE::LoadingMenu::MENU_NAME) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const bool opening = a_event->opening;
                SKSE::GetTaskInterface()->AddTask([opening] {
                    g_loading = opening;
                    if (opening) {
                        return;
                    }
                    g_holdUntil = Clock::now() + std::chrono::seconds(2);
                    const auto generation = ++g_loadGeneration;
                    std::thread([generation] {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
                        SKSE::GetTaskInterface()->AddTask([generation] { FlushPending(generation); });
                    }).detach();
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        LoadingSink g_loadingSink;
    }

    void Install()
    {
        const auto holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            logger::error("Nearby: no script event source; nearby messages are off.");
            return;
        }
        holder->AddEventSink<RE::TESActorLocationChangeEvent>(&g_sink);
        holder->AddEventSink<RE::TESQuestStageEvent>(&g_stageSink);
        if (const auto player = RE::PlayerCharacter::GetSingleton()) {
            player->AsBGSActorCellEventSource()->AddEventSink(&g_cellSink);
        } else {
            logger::warn("Nearby: no player yet; cell changes are not watched (gates and distance reminders need them).");
        }
        if (const auto ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_loadingSink);
        }
        const auto& v = Settings::Get();
        logger::info("Nearby: listening for location, cell and quest stage changes (arrive {}, leave {}, nearby {} within {} m, "
                     "updates {}, misc {}, cooldown {} s; {}).",
            v.notifyArrive, v.notifyLeave, v.notifyNear, v.nearbyRadius, v.notifyUpdates, v.nearbyMisc, v.nearbyCooldown,
            v.boxStyle ? "QSO boxes" : "game notifications");
    }

    void OnGameLoaded()
    {
        g_area = 0;  // no "leaving" message for the place the previous save was in
        g_areaName.clear();
        g_location = 0;
        g_world = 0;
        g_lastShown.clear();
        g_reminded.clear();
        g_pending.clear();
        g_baseline.clear();
        g_baselineValid = false;
        g_stageQuests.clear();
        ++g_stageGeneration;
        QueueCheck("game loaded");
    }

    std::string KeyLabel() { return KeyName(InputHook::CurrentJournalKey()); }
}
