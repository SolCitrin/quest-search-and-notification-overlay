#include "Overlay.h"
#include "InputHook.h"
#include "Nearby.h"
#include "Pins.h"
#include "QuestReader.h"
#include "Settings.h"

#include "Boxes.h"
#include "View.h"

namespace QSO::Overlay
{
    namespace
    {
        std::atomic<bool>     open{ false };
        std::atomic<unsigned> blocked{ 0 };
        std::uint32_t         loadId = 0;  // bumped on every save load / new game

        // 0.16.0: the view, the box stack and its hold rules moved to core (View.cpp, Boxes.cpp), shared with the
        // capture part's game lines. This file keeps the J overlay and its commands.

        // Sends the current list to the page without resetting its search box or filters.
        void Refresh()
        {
            if (!open.load() || !View::Exists()) {
                return;
            }
            const auto snapshot = Quests::Read();
            View::Call("qsoRefresh", Quests::ToJson(snapshot));
        }

        // Runs when the Papyrus call finished (or failed). The VM may run it while the overlay is still open or,
        // if the VM is paused by the overlay, only after it closed: both are logged so the test shows which.
        class TrackDone : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            TrackDone(std::uint32_t a_formID, bool a_on) : formID(a_formID), on(a_on) {}
            void operator()(RE::BSScript::Variable) override
            {
                const auto id = formID;
                const auto want = on;
                SKSE::GetTaskInterface()->AddTask([id, want] {
                    const auto quest = RE::TESForm::LookupByID<RE::TESQuest>(id);
                    const bool now = quest && quest->IsActive();
                    logger::info("Track call finished for {:08X}: asked {}, quest is now {} (overlay {}).", id,
                        want ? "track" : "untrack", now ? "TRACKED" : "not tracked", open.load() ? "still open" : "closed");
                    Refresh();
                });
            }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::uint32_t formID;
            bool          on;
        };

        // "track:<8 hex digits>:<1|0>" from the page. Same engine path as the journal's own toggle: Papyrus
        // Quest.SetActive (a native function), dispatched through the VM on the main thread.
        void Track(const std::string& a_text)
        {
            std::uint32_t formID = 0;
            int           on = -1;
            if (std::sscanf(a_text.c_str(), "track:%8x:%d", &formID, &on) != 2 || (on != 0 && on != 1)) {
                logger::warn("Bad track command '{}'.", a_text);
                return;
            }
            const auto quest = RE::TESForm::LookupByID<RE::TESQuest>(formID);
            if (!quest) {
                logger::warn("Track: {:08X} is not a quest.", formID);
                return;
            }
            if (quest->IsCompleted() || !quest->IsRunning()) {
                logger::info("Track: '{}' {:08X} is completed or not running; ignored.", quest->GetFullName(), formID);
                Refresh();
                return;
            }
            const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
            const auto handle = policy ? policy->GetHandleForObject(RE::FormType::Quest, quest) : 0;
            if (!handle || handle == policy->EmptyHandle()) {
                logger::error("Track: no Papyrus handle for '{}' {:08X}; not changed.", quest->GetFullName(), formID);
                Refresh();
                return;
            }
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{ new TrackDone(formID, on == 1) };
            const auto args = RE::MakeFunctionArguments(static_cast<bool>(on == 1));
            const bool sent = vm->DispatchMethodCall(handle, "Quest", "SetActive", args, callback);
            logger::info("Track: '{}' {:08X} -> {} ({}).", quest->GetFullName(), formID, on ? "track" : "untrack",
                sent ? "sent to Papyrus" : "Papyrus REFUSED the call");
            if (!sent) {
                Refresh();
            }
        }

        void UntrackAll()
        {
            const auto snapshot = Quests::Read();
            int        sent = 0;
            for (const auto& e : snapshot.entries) {
                if (e.active) {
                    Track(std::format("track:{:08X}:0", e.formID));
                    ++sent;
                }
            }
            logger::info("Untrack all: {} quest(s) sent.", sent);
            if (sent == 0) {
                Refresh();
            }
        }

        // "map:<quest formID hex>:<instance>:<objective index>" from the page. The target is recomputed now (the
        // page's copy may be stale), the overlay closes, and on the next frame the journal's own ShowTargetOnMap
        // opens the world map centred on it, exactly as the vanilla journal's "Show on Map" does.
        void ShowOnMap(const std::string& a_text)
        {
            std::uint32_t formID = 0;
            unsigned      instance = 0;
            unsigned      index = 0;
            if (std::sscanf(a_text.c_str(), "map:%8x:%u:%u", &formID, &instance, &index) != 3 || index > 0xFFFF) {
                logger::warn("Bad map command '{}'.", a_text);
                return;
            }
            const auto quest = RE::TESForm::LookupByID<RE::TESQuest>(formID);
            const auto name = quest ? std::string(quest->GetFullName()) : std::string("?");
            const auto controls = RE::ControlMap::GetSingleton();
            if (controls && !controls->IsMenuControlsEnabled()) {
                logger::info("Show on map: '{}' {:08X} refused, menu controls are disabled right now.", name, formID);
                View::Call("qsoNotice", "The map cannot be opened right now.");
                return;
            }
            const auto target = Quests::FindMapTarget(formID, instance, static_cast<std::uint16_t>(index));
            if (!target) {
                logger::info("Show on map: '{}' {:08X} objective {} has no map target any more.", name, formID, index);
                View::Call("qsoNotice", "This objective has nothing to show on the map.");
                Refresh();
                return;
            }
            const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(target);
            logger::info("Show on map: '{}' {:08X} objective {} -> reference {:08X} ({}).", name, formID, index, target,
                ref && ref->GetBaseObject() && ref->GetBaseObject()->GetName() ? ref->GetBaseObject()->GetName() : "?");
            // 0.12.0: opened from the world map, the map is still open underneath; the journal's handler then sends the
            // open map an update (recentre in place) instead of opening it.
            const auto ui = RE::UI::GetSingleton();
            const bool mapOpen = ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
            Close(mapOpen ? "Show on map, map already open" : "Show on map");
            SKSE::GetTaskInterface()->AddTask([target, mapOpen] {
                const bool sent = Quests::ShowOnMap(target);
                logger::info("Show on map: {} for reference {:08X}.",
                    !sent   ? "FAILED (handler faulted or not AE)" :
                    mapOpen ? "open map recentred through the journal's handler" :
                              "map opened through the journal's handler",
                    target);
            });
        }

        // Three sample boxes for the Settings page's "Show test notifications": the overlay closes so they show as in
        // play, at the chosen position, size and duration.
        void TestToasts()
        {
            const auto key = Quests::JsonString(Nearby::KeyLabel());
            Close("Settings page: test notifications");
            const char* samples[] = {
                R"("kind":"arrive","head":"Here in Whiterun","quest":"Test: Hunt and Gather","line":"Bring venison to Anoriath","where":"The Drunken Huntsman")",
                R"("kind":"near","head":"Nearby · 150 m","quest":"Test: Bleak Falls Barrow","line":"Retrieve the Dragonstone","where":"Bleak Falls Barrow")",
                R"("kind":"updated","head":"Updated · here in Whiterun","quest":"Test: The Golden Claw","line":"Talk to Lucan Valerius","where":"Riverwood Trader")",
            };
            const bool boxes = Settings::Get().boxStyle != 0 || Boxes::LinesActive();
            for (const auto* s : samples) {
                if (!boxes || !Toast(std::string(s) + R"(,"key":)" + key)) {
                    RE::SendHUDMessage::ShowHUDMessage("Quest Search and Notification Overlay: test notification (vanilla style)");
                }
            }
            // 0.16.0: with game notifications on, one real game line too: it goes through the game's own notification
            // call, so the hook catches it like any other line and it lands in the same stack.
            if (Boxes::LinesActive()) {
                RE::SendHUDMessage::ShowHUDMessage("Test: a game notification, caught from the game's own top-left line");
            }
            logger::info("Settings page: 3 test notifications sent ({} style){}.", boxes ? "QSO boxes" : "vanilla",
                Boxes::LinesActive() ? " + 1 game line through the capture hook" : "");
        }

        // The quests part's commands (core View handles toasts:idle, log: and settings:). Main thread.
        bool OnCommand(const std::string& text)
        {
            if (text == "toasttest") {
                TestToasts();
            } else if (text == "close") {
                Close("page asked (Esc or J outside the search box)");
            } else if (text.starts_with("track:")) {
                Track(text);
            } else if (text.starts_with("pin:") || text.starts_with("hide:")) {
                Pins::Command(text);
            } else if (text.starts_with("map:")) {
                ShowOnMap(text);
            } else if (text == "untrackall") {
                UntrackAll();
            } else {
                return false;
            }
            return true;
        }
    }

    void Initialize()
    {
        View::SetCommandHandler(OnCommand);
        if (!View::Exists()) {
            logger::error("No view: J will keep writing the quest list to the log instead.");
        }
    }

    bool IsOpen() noexcept { return open.load(); }

    void OnGameLoaded()
    {
        ++loadId;
        Quests::ClearCache();
    }

    void CountBlocked(unsigned a_events) noexcept { blocked += a_events; }

    void Open()
    {
        static bool firstDump = true;
        const auto  snapshot = Quests::Read();
        Quests::DumpToLog(snapshot, firstDump);  // full rows once per session, then summary lines only
        firstDump = false;

        if (!View::Ready()) {
            logger::warn("Overlay not ready (view {}, page loaded {}); quest list written to the log only.", View::Exists(),
                View::Ready());
            RE::SendHUDMessage::ShowHUDMessage("Quest Search and Notification Overlay: overlay not ready, see log");
            return;
        }
        if (open.load()) {
            return;
        }
        if (View::OtherViewHasFocus()) {
            logger::info("Overlay not opened: another PrismaUI view has focus.");
            return;
        }
        blocked.store(0);
        open.store(true);  // set before Focus so no keystroke slips through between the two
        Boxes::SetPanelOpen(true);  // shows the view and holds the boxes
        if (!View::Focus()) {
            open.store(false);
            Boxes::SetPanelOpen(false);
            logger::error("PrismaUI refused focus; overlay closed again.");
            return;
        }
        // {"build":..,"journalKey":<DirectInput scan code>,"list":{"valid":..,"problem":..,"quests":[...]}}
        const auto& opt = Settings::Get();
        const auto  data = std::format(
            R"({{"build":"{}","loadId":{},"journalKey":{},"options":{{"showCompleted":{},"rememberSearch":{},"maxObjectives":{}}},"settings":{},"captureHooked":{},"b612":{},"pins":{},"hidden":{},"list":{}}})",
            QSO_VERSION_STRING, loadId, InputHook::CurrentJournalKey(), opt.showCompleted, opt.rememberSearch, opt.maxObjectives,
            Settings::PageJson(), Boxes::CaptureHooked(), Boxes::B612Present(), Pins::Json(), Pins::HiddenJson(), Quests::ToJson(snapshot));
        View::Call("qsoOpened", data);
        logger::info("Overlay opened with {} quests ({} bytes of list data sent to the page).", snapshot.entries.size(),
            data.size());
    }

    void Close(const char* a_reason)
    {
        if (!open.load()) {
            return;
        }
        View::Call("qsoClosed", "");
        View::Unfocus();
        open.store(false);
        Boxes::SetPanelOpen(false);  // hidden again unless boxes are waiting (they resume once the focus menu has closed)
        logger::info("Overlay closed ({}); {} keyboard input events (down + every held frame + up + typed character, not "
                     "presses) were kept from the game and other mods while it was open.",
            a_reason, blocked.load());
    }

    bool Toast(const std::string& a_fields) { return Boxes::PushQuest(a_fields); }
}
