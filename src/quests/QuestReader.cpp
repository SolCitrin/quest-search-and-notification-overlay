#include "QuestReader.h"
#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace QSO::Quests
{
    namespace
    {
        constexpr std::uint32_t kMaxInstances = 8192;

        // Game's own text substitution (fills <Alias=...>, <Global=...> etc.). Same IDs Compass Navigation
        // Overhaul uses on 1.6.
        //  - objective text: BGSInstancedQuestObjective::GetDisplayTextWithReplacedTags  ID 23229 / 23684
        //  - quest name:     ReplaceTagsInQuestText(BSString*, const TESQuest*, instanceID) ID 23429 / 23897
        std::string ObjectiveText(const RE::BGSInstancedQuestObjective& a_instance)
        {
            using func_t = void (*)(const RE::BGSInstancedQuestObjective*, RE::BSString&);
            static REL::Relocation<func_t> func{ RELOCATION_ID(23229, 23684) };
            RE::BSString out;
            func(&a_instance, out);
            return out.c_str() ? out.c_str() : "";
        }

        // 0.15.0: also used for quest notes (book name and text), with the quest instance the note belongs to.
        std::string FillTags(const RE::TESQuest* a_quest, std::uint32_t a_instanceID, const char* a_text)
        {
            using func_t = void (*)(RE::BSString*, const RE::TESQuest*, std::uint32_t);
            static REL::Relocation<func_t> func{ RELOCATION_ID(23429, 23897) };
            RE::BSString text(a_text ? a_text : "");
            func(&text, a_quest, a_instanceID);
            return text.c_str() ? text.c_str() : "";
        }

        std::string QuestName(const RE::TESQuest* a_quest, std::uint32_t a_instanceID)
        {
            return FillTags(a_quest, a_instanceID, a_quest->GetFullName());
        }

        // The journal's description text for a quest instance, tags replaced: TESQuest::GetJournalTextForInstance,
        // ID 24549 / 25078 (CommonLibVR "ng" branch; used on AE by the Quest-History mod). Reads the quest's current
        // journal entry (the game loads it from the .DLSTRINGS file on demand), so results are cached per stage.
        std::string JournalText(RE::TESQuest* a_quest, std::uint32_t a_instanceID)
        {
            using func_t = void (*)(RE::TESQuest*, RE::BSString&, std::uint32_t);
            static REL::Relocation<func_t> func{ RELOCATION_ID(24549, 25078) };
            RE::BSString out;
            func(a_quest, out, a_instanceID);
            return out.c_str() ? out.c_str() : "";
        }

        // ---- Show on map (0.8.0) -------------------------------------------------------------------------------
        // Read from the 1.6.1170 executable (decrypted in memory).
        // The vanilla journal (RequestObjectivesData, ID 53171) gives a SHOWN objective a "questTargetID" like this:
        //   1. PlayerCharacter quest-target lookup, ID 40513: (player, &quest, &BSTArray<TESQuestTarget*>*) -> bool.
        //      It takes the player's questTargetsLock itself.
        //   2. Journal helper ID 53179: (objective, &ctx{targets, &instancedObjective, GFxValue* out}). For the first
        //      of the objective's targets that the player's list holds, it resolves the reference, routes it into the
        //      player's worldspace (load-door chain) and validates it; then writes its form ID into *out as a number.
        // "Show on Map" then calls ShowTargetOnMap, ID 53178, with that number as argument 0. That handler reads only
        // args[0]: LookupByID -> AsReference -> handle -> show (or update) MapMenu centred on it; closes TweenMenu.
        // AE IDs only: the SE equivalents were not checked, so on SE no quest has a map target.
        struct RawGFxValue  // GFxValue layout (objectInterface, type, value); POD so it can live under __try
        {
            void*         objectInterface;
            std::uint32_t type;  // 3 = number
            std::uint32_t pad0C;
            double        number;
        };
        static_assert(sizeof(RawGFxValue) == 0x18);

        struct TargetContext  // what the journal passes to ID 53179
        {
            void*                                 targets;    // BSTArray<TESQuestTarget*>*
            const RE::BGSInstancedQuestObjective* objective;  // the helper reads objective->ownerQuest through this
            RawGFxValue*                          out;
        };

        struct RawDelegateArgs  // FxDelegateArgs layout; ShowTargetOnMap reads only the args pointer (0x28)
        {
            std::uint8_t       responseID[0x18];
            void*              handler;
            void*              movie;
            const RawGFxValue* args;
            std::uint32_t      numArgs;
            std::uint32_t      pad34;
        };
        static_assert(sizeof(RawDelegateArgs) == 0x38 && offsetof(RawDelegateArgs, args) == 0x28);

        // No C++ objects with destructors in here: SEH turns a wrong guess into "no target" instead of a crash.
        // a_actual receives the handle of the real target the helper starts from: TESQuestTarget resolve, ID 25284
        // (target, RefHandle* out, bool useCarrier, quest), as the helper calls it, for the first of the objective's
        // targets that the player's list holds (the helper's own choice).
        std::uint32_t RawMapTarget(RE::PlayerCharacter* a_player, const RE::BGSInstancedQuestObjective* a_instance,
            std::uintptr_t a_lookup, std::uintptr_t a_helper, std::uintptr_t a_resolve, std::uint32_t* a_actual)
        {
            using lookup_t = bool (*)(RE::PlayerCharacter*, RE::TESQuest**, void**);
            using helper_t = void (*)(RE::BGSQuestObjective*, TargetContext*);
            using resolve_t = std::uint32_t* (*)(RE::TESQuestTarget*, std::uint32_t*, bool, RE::TESQuest*);
            *a_actual = 0;
            __try {
                const auto    objective = a_instance->Objective;
                RE::TESQuest* quest = objective ? objective->ownerQuest : nullptr;
                if (!quest || objective->numTargets == 0) {
                    return 0;
                }
                void* targets = nullptr;
                if (!reinterpret_cast<lookup_t>(a_lookup)(a_player, &quest, &targets) || !targets) {
                    return 0;
                }
                // BSTArray<TESQuestTarget*>: data at +0x00, size at +0x10.
                const auto  listData = *reinterpret_cast<RE::TESQuestTarget* const* const*>(targets);
                const auto  listSize = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(targets) + 0x10);
                for (std::uint32_t t = 0; t < objective->numTargets && *a_actual == 0; ++t) {
                    const auto target = objective->targets[t];
                    for (std::uint32_t k = 0; k < listSize && listData; ++k) {
                        if (listData[k] == target) {
                            std::uint32_t handle = 0;
                            reinterpret_cast<resolve_t>(a_resolve)(target, &handle, true, quest);
                            *a_actual = handle;
                            break;
                        }
                    }
                }
                RawGFxValue   out{};
                TargetContext ctx{ targets, a_instance, &out };
                reinterpret_cast<helper_t>(a_helper)(objective, &ctx);
                if (out.type != 3 || !(out.number > 0)) {
                    return 0;
                }
                return static_cast<std::uint32_t>(static_cast<std::int64_t>(out.number));
            } __except (1) {
                return 0;
            }
        }

        bool RawShowOnMap(std::uint32_t a_target, std::uintptr_t a_show)
        {
            using show_t = void (*)(const RawDelegateArgs*);
            __try {
                RawGFxValue     value{ nullptr, 3, 0, static_cast<double>(a_target) };
                RawDelegateArgs args{};
                args.args = &value;
                args.numArgs = 1;
                reinterpret_cast<show_t>(a_show)(&args);
                return true;
            } __except (1) {
                return false;
            }
        }

        std::uint32_t MapTarget(const RE::BGSInstancedQuestObjective& a_instance, std::uint32_t* a_actualHandle = nullptr)
        {
            std::uint32_t actual = 0;
            if (a_actualHandle) {
                *a_actualHandle = 0;
            }
            if (!REL::Module::IsAE() || a_instance.InstanceState != RE::QUEST_OBJECTIVE_STATE::kDisplayed) {
                return 0;  // the journal only gives open (shown) objectives a target
            }
            static const auto lookup = REL::ID(40513).address();
            static const auto helper = REL::ID(53179).address();
            static const auto resolve = REL::ID(25284).address();
            const auto        player = RE::PlayerCharacter::GetSingleton();
            const auto        target = player ? RawMapTarget(player, &a_instance, lookup, helper, resolve, &actual) : 0;
            if (a_actualHandle) {
                *a_actualHandle = actual;
            }
            return target;
        }

        // ---- Objective locations (0.9.0) ----------------------------------------------------------------------
        // Location-type keywords from Skyrim.esm (form IDs read from the file).
        bool HasLocType(const RE::BGSLocation* a_loc, std::initializer_list<RE::FormID> a_ids)
        {
            for (const auto id : a_ids) {
                if (const auto kw = RE::TESForm::LookupByID<RE::BGSKeyword>(id); kw && a_loc->HasKeyword(kw)) {
                    return true;
                }
            }
            return false;
        }

        bool IsHold(const RE::BGSLocation* a_loc) { return a_loc && HasLocType(a_loc, { 0x16771 }); }
        bool IsSettlement(const RE::BGSLocation* a_loc)
        {
            return a_loc && HasLocType(a_loc, { 0x13168 /*City*/, 0x13166 /*Town*/, 0x13167 /*Settlement*/ });
        }

        struct Where
        {
            std::uint32_t location = 0;
            std::string   place;
            std::string   town;
            std::string   hold;
        };

        Where WhereIs(RE::TESObjectREFR* a_ref)
        {
            Where w;
            if (!a_ref) {
                return w;
            }
            auto loc = a_ref->GetCurrentLocation();
            if (!loc) {
                loc = a_ref->GetEditorLocation();
            }
            w.location = loc ? loc->GetFormID() : 0;
            for (int guard = 0; loc && guard < 16; loc = loc->parentLoc, ++guard) {
                const std::string name = loc->GetFullName() ? loc->GetFullName() : "";
                if (name.empty()) {
                    continue;
                }
                if (w.place.empty()) {
                    w.place = name;
                }
                if (w.town.empty() && HasLocType(loc, { 0x13168 /*City*/, 0x13166 /*Town*/, 0x13167 /*Settlement*/ })) {
                    w.town = name;
                }
                if (w.hold.empty() && HasLocType(loc, { 0x16771 /*Hold*/ })) {
                    w.hold = name;
                }
            }
            return w;
        }

        struct DescKey
        {
            std::uint32_t formID;
            std::uint32_t instanceID;
            std::uint16_t stage;
            auto          operator<=>(const DescKey&) const = default;
        };
        std::map<DescKey, std::string> g_descriptions;

        std::string OneLine(std::string a_text)
        {
            std::ranges::replace(a_text, '\n', ' ');
            std::ranges::replace(a_text, '\r', ' ');
            return a_text;
        }

        // ---- Quest notes (0.15.0) ------------------------------------------------------------------------------
        // A note = a BOOK held by one of the quest's reference
        // aliases filled "Create Reference to Object" (kCreated: every Missives quest, bounty and courier letters) or
        // "Forced Reference" (kForced). Only the static alias data and the book record are read: the note's own
        // reference is never looked at, so a note removed at turn-in cannot matter. Never Activate / equip / open the
        // book (reading a missive from the inventory raises Missives' "discard this quest?" prompt) and never touch
        // its read flag. Main thread only: TESDescription keeps just a file offset, so the text is read from the
        // plugin (or the .DLSTRINGS file) on each call; hence the per-book cache.
        constexpr std::size_t kNoteRawCap = 8192;    // bytes of raw book text kept (vanilla books can be 30 KB)
        constexpr std::size_t kNoteShownCap = 2000;  // characters shown, then "…"
        constexpr std::size_t kMaxNotesPerQuest = 3;

        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_questBooks;  // quest -> its note books
        std::unordered_map<std::uint32_t, std::string>                g_bookText;    // book -> raw text (capped)
        struct NoteKey
        {
            std::uint32_t formID;
            std::uint32_t instanceID;
            std::uint16_t stage;
            std::uint32_t book;
            auto          operator<=>(const NoteKey&) const = default;
        };
        std::map<NoteKey, std::pair<std::string, std::string>> g_noteText;  // filled name, filled + stripped text
        std::set<NoteKey>                                      g_noteHeldLogged;

        const std::vector<std::uint32_t>& QuestBooks(RE::TESQuest* a_quest)
        {
            auto [it, inserted] = g_questBooks.try_emplace(a_quest->GetFormID());
            if (!inserted) {
                return it->second;
            }
            for (const auto alias : a_quest->aliases) {
                if (!alias || alias->GetVMTypeID() != RE::BGSRefAlias::VMTYPEID) {
                    continue;
                }
                const auto       refAlias = static_cast<RE::BGSRefAlias*>(alias);
                RE::TESForm*     base = nullptr;
                // fillData is a union: which member is valid depends on fillType, so check it first.
                switch (refAlias->fillType.get()) {
                case RE::BGSBaseAlias::FILL_TYPE::kCreated:
                    base = refAlias->fillData.created.object;
                    break;
                case RE::BGSBaseAlias::FILL_TYPE::kForced:
                    if (const auto ref = refAlias->fillData.forced.forcedRef.get()) {
                        base = ref->GetBaseObject();
                    }
                    break;
                default:
                    break;
                }
                const auto book = base ? base->As<RE::TESObjectBOOK>() : nullptr;
                if (book && std::ranges::find(it->second, book->GetFormID()) == it->second.end()) {
                    it->second.push_back(book->GetFormID());
                }
            }
            return it->second;
        }

        const std::string& RawBookText(RE::TESObjectBOOK* a_book)
        {
            auto [it, inserted] = g_bookText.try_emplace(a_book->GetFormID());
            if (inserted) {
                RE::BSString out;
                static_cast<RE::TESDescription*>(a_book)->GetDescription(out, a_book);  // DESC = the book's text
                std::string text = out.c_str() ? out.c_str() : "";
                if (text.size() > kNoteRawCap) {
                    text.resize(kNoteRawCap);
                    // Do not leave half a tag for the tag filler.
                    const auto open = text.find_last_of('<');
                    if (open != std::string::npos && text.find('>', open) == std::string::npos) {
                        text.resize(open);
                    }
                }
                it->second = std::move(text);
            }
            return it->second;
        }

        // Book markup is Scaleform's HTML subset. Run AFTER the tags are filled (<Alias=...> looks like a tag).
        std::string StripBookMarkup(std::string_view a_s)
        {
            static constexpr std::pair<std::string_view, std::string_view> kEntities[] = {
                { "&lt;", "<" }, { "&gt;", ">" }, { "&amp;", "&" }, { "&quot;", "\"" }, { "&apos;", "'" }, { "&nbsp;", " " }
            };
            const auto lower = [](std::string_view a_v) {
                std::string l(a_v);
                std::ranges::transform(l, l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return l;
            };
            std::string out;
            out.reserve(a_s.size());
            for (std::size_t i = 0; i < a_s.size();) {
                const char c = a_s[i];
                if (c == '[' && lower(a_s.substr(i, 11)) == "[pagebreak]") {
                    out += "\n\n";
                    i += 11;
                    continue;
                }
                if (c == '<') {
                    const auto close = a_s.find('>', i);
                    if (close == std::string_view::npos) {
                        break;  // a broken tag at the end
                    }
                    std::string tag = lower(a_s.substr(i + 1, close - i - 1));
                    const auto  nameEnd = tag.find_first_of(" \t\r\n");
                    const auto  name = tag.substr(0, nameEnd);
                    if (name == "br" || name == "br/" || name == "p" || name == "/p" || name == "/li" || name == "ul" || name == "/ul") {
                        out += '\n';
                    } else if (name == "li") {
                        out += "\n- ";
                    }
                    i = close + 1;  // <font>, <b>, <i>, <u>, <img ...> and anything else: dropped
                    continue;
                }
                if (c == '&') {
                    bool matched = false;
                    for (const auto& [from, to] : kEntities) {
                        if (lower(a_s.substr(i, from.size())) == from) {
                            out += to;
                            i += from.size();
                            matched = true;
                            break;
                        }
                    }
                    if (matched) {
                        continue;
                    }
                }
                if (c != '\r') {
                    out += c == '\t' ? ' ' : c;
                }
                ++i;
            }
            // Tidy: trim each line, at most one blank line in a row, no blank lines at the ends.
            std::string tidy;
            tidy.reserve(out.size());
            int         newlines = 0;
            std::size_t start = 0;
            while (start <= out.size()) {
                auto end = out.find('\n', start);
                if (end == std::string::npos) {
                    end = out.size();
                }
                auto line = std::string_view(out).substr(start, end - start);
                while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
                while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
                if (line.empty()) {
                    ++newlines;
                } else {
                    if (!tidy.empty()) {
                        tidy.append(newlines >= 2 ? "\n\n" : "\n");
                    }
                    tidy.append(line);
                    newlines = 1;
                }
                start = end + 1;
            }
            return tidy;
        }

        // About a_max characters (a UTF-8 sequence or a Windows-1252 byte counts as one), cut at a space if one is
        // near, then "…".
        std::string CapText(std::string a_text, std::size_t a_max)
        {
            std::size_t chars = 0;
            std::size_t cut = std::string::npos;
            for (std::size_t i = 0; i < a_text.size(); ++i) {
                const auto c = static_cast<unsigned char>(a_text[i]);
                if ((c & 0xC0) != 0x80 || i == 0) {
                    if (chars == a_max) {
                        cut = i;
                        break;
                    }
                    ++chars;
                }
            }
            if (cut == std::string::npos) {
                return a_text;
            }
            const auto space = a_text.find_last_of(" \n", cut);
            if (space != std::string::npos && cut - space < 40) {
                cut = space;
            }
            a_text.resize(cut);
            while (!a_text.empty() && (a_text.back() == ' ' || a_text.back() == '\n')) a_text.pop_back();
            return a_text + "\xE2\x80\xA6";
        }

        std::size_t CountUnfilled(std::string_view a_text)
        {
            std::size_t n = 0;
            for (auto at = a_text.find('<'); at != std::string_view::npos; at = a_text.find('<', at + 1)) {
                const auto rest = a_text.substr(at + 1, 7);
                n += rest.starts_with("Alias") || rest.starts_with("Global") || rest.starts_with("alias") ? 1 : 0;
            }
            return n;
        }

        // Fills e.notes for one running quest instance. a_held: the player's count of each candidate book.
        void ReadNotes(Entry& a_entry, RE::TESQuest* a_quest, const std::vector<std::uint32_t>& a_books,
            const std::unordered_map<std::uint32_t, std::int32_t>& a_held, std::size_t& a_looked)
        {
            for (const auto bookID : a_books) {
                if (a_entry.notes.size() >= kMaxNotesPerQuest) {
                    break;
                }
                const auto book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookID);
                if (!book) {
                    continue;
                }
                const NoteKey key{ a_entry.formID, a_entry.instanceID, a_entry.stage, bookID };
                const auto    held = a_held.find(bookID);
                const bool    holds = held != a_held.end() && held->second > 0;
                if (!holds && !book->IsRead()) {  // spoiler guard: a note the player has not seen yet stays hidden
                    if (g_noteHeldLogged.insert(key).second) {
                        logger::info("Note held back (not carried, not read): {} [{:08X}] book {} [{:08X}].",
                            a_entry.editorID, a_entry.formID, book->GetFormEditorID() ? book->GetFormEditorID() : "", bookID);
                    }
                    continue;
                }
                auto found = g_noteText.find(key);
                if (found == g_noteText.end()) {
                    ++a_looked;
                    const auto& raw = RawBookText(book);
                    const auto  name = OneLine(FillTags(a_quest, a_entry.instanceID, book->GetFullName()));
                    const auto  filled = FillTags(a_quest, a_entry.instanceID, raw.c_str());
                    const auto  unfilled = CountUnfilled(filled);
                    auto        text = CapText(StripBookMarkup(filled), kNoteShownCap);
                    // The filled text, for checking the tags (e.g. <Alias.BaseName=...>) from the log.
                    logger::info("Note: {} [{:08X}] inst {} book {} [{:08X}] \"{}\" ({} raw bytes, {} tag(s) left unfilled, {}): {}",
                        a_entry.editorID, a_entry.formID, a_entry.instanceID, book->GetFormEditorID() ? book->GetFormEditorID() : "",
                        bookID, name, raw.size(), unfilled, holds ? "carried" : "read", OneLine(text));
                    found = g_noteText.emplace(key, std::make_pair(name, std::move(text))).first;
                }
                if (!found->second.second.empty()) {
                    a_entry.notes.push_back(Note{ bookID, found->second.first, found->second.second });
                }
            }
        }

        struct RawInstance
        {
            RE::BGSQuestObjective* objective;
            std::uint32_t          instanceID;
            std::uint32_t          state;
        };

        // Copies the player's objective array under SEH, so a wrong struct layout gives an error
        // code instead of a crash. No C++ objects with destructors may live in this function.
        int CopyInstances(const RE::BSTArray<RE::BGSInstancedQuestObjective>* a_array, RawInstance* a_out,
            std::uint32_t a_max, std::uint32_t* a_size, std::uint32_t* a_capacity)
        {
            __try {
                const auto size = a_array->size();
                const auto capacity = a_array->capacity();
                *a_size = size;
                *a_capacity = capacity;
                if (size > capacity || size > a_max) {
                    return -1;
                }
                const auto* data = a_array->data();
                if (size != 0 && !data) {
                    return -1;
                }
                for (std::uint32_t i = 0; i < size; ++i) {
                    a_out[i].objective = data[i].Objective;
                    a_out[i].instanceID = data[i].instanceID;
                    a_out[i].state = static_cast<std::uint32_t>(data[i].InstanceState);
                }
                return static_cast<int>(size);
            } __except (1 /* EXCEPTION_EXECUTE_HANDLER; CommonLib's trimmed Windows headers lack the macro */) {
                return -2;
            }
        }

        bool Shown(RE::QUEST_OBJECTIVE_STATE a_state)
        {
            using S = RE::QUEST_OBJECTIVE_STATE;
            return a_state == S::kDisplayed || a_state == S::kCompletedDisplayed || a_state == S::kFailedDisplayed;
        }

        const char* StateName(std::uint32_t a_state)
        {
            switch (a_state) {
            case 0: return "dormant";
            case 1: return "shown";
            case 2: return "done-hidden";
            case 3: return "done";
            case 4: return "failed-hidden";
            case 5: return "failed";
            default: return "?";
            }
        }

        std::string PluginOf(const RE::TESForm* a_form)
        {
            const auto file = a_form->GetFile(0);
            return file ? std::string(file->GetFilename()) : std::string("?");
        }

        // Every objective record of every quest: the reference set the player's list is checked against.
        // ---- Outdoor spots (0.10.4) --------------------------------------------------------------------------
        // A walled city's worldspace (Whiterun) uses its parent's land and coordinates (Tamriel), so positions in it
        // compare directly with positions in the parent.
        RE::TESWorldSpace* TopWorld(RE::TESWorldSpace* a_world)
        {
            for (int guard = 0; a_world && a_world->parentWorld && guard < 8; ++guard) {
                a_world = a_world->parentWorld;
            }
            return a_world;
        }

        // The reference's own spot if it stands in an exterior cell; world 0 otherwise.
        OutdoorSpot SpotIfOutdoors(RE::TESObjectREFR* a_ref)
        {
            OutdoorSpot spot;
            const auto  cell = a_ref->GetParentCell();
            if (cell && cell->IsInteriorCell()) {
                return spot;
            }
            const auto world = TopWorld(a_ref->GetWorldspace());
            if (!world) {
                return spot;
            }
            const auto pos = a_ref->GetPosition();
            spot = { world->GetFormID(), pos.x, pos.y, pos.z };
            return spot;
        }

        // Interior cell -> the outdoor spot of its way out (world 0 = none found). Load doors are persistent, so an
        // interior the player has never entered still lists them. Cached for the session; the doors do not move.
        std::unordered_map<RE::FormID, OutdoorSpot> g_exitOfCell;

        OutdoorSpot ExitOf(RE::TESObjectCELL* a_cell)
        {
            const auto id = a_cell->GetFormID();
            if (const auto it = g_exitOfCell.find(id); it != g_exitOfCell.end()) {
                return it->second;
            }
            // Breadth-first through load doors: the nearest way out in rooms wins (Jarl's quarters -> Dragonsreach ->
            // Whiterun). At most 8 rooms are opened.
            OutdoorSpot                       found;
            std::deque<RE::TESObjectCELL*>    queue{ a_cell };
            std::unordered_set<RE::FormID>    seen{ id };
            for (int opened = 0; !queue.empty() && !found.world && opened < 8; ++opened) {
                const auto cell = queue.front();
                queue.pop_front();
                std::vector<RE::NiPointer<RE::TESObjectREFR>> others;
                cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
                    const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
                    if (base && base->Is(RE::FormType::Door)) {
                        if (auto other = a_ref->extraList.GetTeleportLinkedDoor().get()) {
                            others.push_back(std::move(other));
                        }
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                for (const auto& other : others) {
                    const auto spot = SpotIfOutdoors(other.get());
                    if (spot.world) {
                        found = spot;
                        break;
                    }
                    const auto next = other->GetParentCell();
                    if (next && next->IsInteriorCell() && seen.insert(next->GetFormID()).second) {
                        queue.push_back(next);
                    }
                }
            }
            g_exitOfCell.emplace(id, found);
            return found;
        }

        float MetresBetween(const OutdoorSpot& a_a, const OutdoorSpot& a_b)
        {
            if (!a_a.world || a_a.world != a_b.world) {
                return -1.0f;
            }
            const float dx = a_a.x - a_b.x, dy = a_a.y - a_b.y, dz = a_a.z - a_b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz) / 70.0f;  // 70 game units ~ 1 metre
        }

        std::unordered_set<const RE::BGSQuestObjective*> AllQuestObjectives(std::size_t& a_questCount)
        {
            std::unordered_set<const RE::BGSQuestObjective*> set;
            a_questCount = 0;
            if (const auto data = RE::TESDataHandler::GetSingleton()) {
                for (const auto quest : data->GetFormArray<RE::TESQuest>()) {
                    if (!quest) {
                        continue;
                    }
                    ++a_questCount;
                    for (const auto objective : quest->objectives) {
                        if (objective) {
                            set.insert(objective);
                        }
                    }
                }
            }
            return set;
        }
    }

    const char* TypeName(std::uint8_t a_type)
    {
        switch (a_type) {
        case 0: return "None";
        case 1: return "Main";
        case 2: return "College";
        case 3: return "Thieves";
        case 4: return "DarkBrotherhood";
        case 5: return "Companions";
        case 6: return "Misc";
        case 7: return "Daedric";
        case 8: return "Side";
        case 9: return "CivilWar";
        case 10: return "Dawnguard";
        case 11: return "Dragonborn";
        default: return "?";
        }
    }

    OutdoorSpot OutdoorSpotOf(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref) {
            return {};
        }
        const auto spot = SpotIfOutdoors(a_ref);
        if (spot.world) {
            return spot;
        }
        const auto cell = a_ref->GetParentCell();
        return cell && cell->IsInteriorCell() ? ExitOf(cell) : OutdoorSpot{};
    }

    Area AreaOf(RE::BGSLocation* a_location, const RE::TESObjectREFR* a_where)
    {
        Area a;
        if (!a_location) {
            return a;
        }
        RE::BGSLocation* area = nullptr;
        int              guard = 0;
        for (auto loc = a_location; loc && guard < 16; loc = loc->parentLoc, ++guard) {
            if (!a.hold && IsHold(loc)) {
                a.hold = loc->GetFormID();
                a.holdName = loc->GetFullName() ? loc->GetFullName() : "";
            }
            if (!area && !a.hold && IsSettlement(loc)) {
                area = loc;  // a settlement below (or without) a hold
            }
        }
        if (!area && !IsHold(a_location)) {
            // Top-most location below the hold, and never the root of a chain with no hold (e.g. a whole DLC island).
            auto candidate = a_location;
            guard = 0;
            while (candidate->parentLoc && !IsHold(candidate->parentLoc) && candidate->parentLoc->parentLoc && guard++ < 16) {
                candidate = candidate->parentLoc;
            }
            if (!IsHold(candidate) && (candidate->parentLoc || candidate == a_location)) {
                area = candidate;
            }
        }
        if (area && a_where && HasLocType(area, { 0x13168 /*City*/ })) {
            const auto cell = a_where->GetParentCell();
            const auto world = a_where->GetWorldspace();
            if (cell && !cell->IsInteriorCell() && world && !world->parentWorld) {
                area = nullptr;  // outside the walls: the hold only
            }
        }
        if (area) {
            a.area = area->GetFormID();
            a.areaName = area->GetFullName() ? area->GetFullName() : "";
        }
        return a;
    }

    Snapshot Read(bool a_quiet)
    {
        Snapshot snap;
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            snap.problem = "no player";
            return snap;
        }

        std::size_t questCount = 0;
        const auto known = AllQuestObjectives(questCount);

        std::vector<RawInstance> raw(kMaxInstances);
        std::uint32_t            size = 0;
        std::uint32_t            capacity = 0;
        const int copied = CopyInstances(&player->GetPlayerRuntimeData().objectives, raw.data(), kMaxInstances, &size, &capacity);
        if (copied < 0) {
            snap.problem = copied == -2 ? "reading the player objective list faulted (layout mismatch?)" :
                                          std::format("player objective list looks invalid (size {}, capacity {})", size, capacity);
            return snap;
        }
        raw.resize(static_cast<std::size_t>(copied));

        // Every entry must point at a real quest objective before anything dereferences it.
        std::size_t unknown = 0;
        for (const auto& r : raw) {
            if (!known.contains(r.objective)) {
                ++unknown;
            }
        }
        if (unknown != 0) {
            snap.problem = std::format("{} of {} player objective entries do not match any quest objective (layout mismatch?)",
                unknown, raw.size());
            return snap;
        }
        snap.playerListValid = true;
        snap.instanceCount = raw.size();

        const auto hereSpot = OutdoorSpotOf(player);
        snap.herePlaced = hereSpot.world != 0;
        std::size_t distances = 0;

        // Group by (quest, instance). The array is oldest -> newest; the journal shows newest first.
        // Only objectives the journal shows count (shown / completed-shown / failed-shown): the list also keeps
        // hidden leftovers such as failed radiant favours and never-shown follower lines.
        std::map<std::pair<RE::TESQuest*, std::uint32_t>, std::size_t> index;
        for (int age = static_cast<int>(raw.size()) - 1; age >= 0; --age) {
            const auto& r = raw[static_cast<std::size_t>(age)];
            const auto  quest = r.objective->ownerQuest;
            if (!quest) {
                continue;
            }
            if (!Shown(static_cast<RE::QUEST_OBJECTIVE_STATE>(r.state))) {
                ++snap.hiddenCount;
                continue;
            }
            const auto key = std::make_pair(quest, r.instanceID);
            auto       it = index.find(key);
            if (it == index.end()) {
                Entry e;
                e.formID = quest->GetFormID();
                e.instanceID = r.instanceID;
                e.name = OneLine(QuestName(quest, r.instanceID));
                e.editorID = quest->GetFormEditorID() ? quest->GetFormEditorID() : "";
                e.plugin = PluginOf(quest);
                e.type = static_cast<std::uint8_t>(quest->GetType());
                e.active = quest->IsActive();
                e.completed = quest->IsCompleted();
                e.failed = quest->data.flags.all(RE::QuestFlag::kFailed);
                e.stage = quest->GetCurrentStageID();
                e.newestAge = age;
                snap.entries.push_back(std::move(e));
                it = index.emplace(key, snap.entries.size() - 1).first;
            }
            RE::BGSInstancedQuestObjective instance{};
            instance.Objective = r.objective;
            instance.instanceID = r.instanceID;
            instance.InstanceState = static_cast<RE::QUEST_OBJECTIVE_STATE>(r.state);
            std::uint32_t actualHandle = 0;
            const auto    target = MapTarget(instance, &actualHandle);
            snap.mapTargets += target ? 1 : 0;
            Objective o{ r.objective->index, r.state, OneLine(ObjectiveText(instance)), target };
            if (actualHandle) {
                if (const auto ref = RE::TESObjectREFR::LookupByHandle(actualHandle)) {
                    const auto where = WhereIs(ref.get());
                    o.targetRef = ref->GetFormID();
                    o.location = where.location;
                    o.place = OneLine(where.place);
                    o.town = OneLine(where.town);
                    o.hold = OneLine(where.hold);
                    snap.located += where.location ? 1 : 0;
                    const auto area = AreaOf(RE::TESForm::LookupByID<RE::BGSLocation>(where.location), ref.get());
                    o.area = area.area;
                    o.holdID = area.hold;
                    o.areaName = OneLine(area.areaName);
                    if (snap.herePlaced) {
                        o.distance = MetresBetween(hereSpot, OutdoorSpotOf(ref.get()));
                        distances += o.distance >= 0 ? 1 : 0;
                    }
                }
            }
            snap.entries[it->second].objectives.push_back(std::move(o));
        }
        // Journal descriptions, cached per (quest, instance, stage): only new or advanced quests cost a lookup.
        const auto start = std::chrono::steady_clock::now();
        std::size_t looked = 0;
        for (auto& e : snap.entries) {
            const DescKey key{ e.formID, e.instanceID, e.stage };
            auto found = g_descriptions.find(key);
            if (found == g_descriptions.end()) {
                const auto quest = RE::TESForm::LookupByID<RE::TESQuest>(e.formID);
                found = g_descriptions.emplace(key, quest ? JournalText(quest, e.instanceID) : std::string{}).first;
                ++looked;
            }
            e.description = found->second;
        }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        {
            const auto here = AreaOf(player->GetCurrentLocation(), player);
            snap.hereArea = here.area;
            snap.hereHold = here.hold;
            snap.hereAreaName = OneLine(here.areaName);
            snap.hereHoldName = OneLine(here.holdName);
        }
        if (a_quiet) {
            return snap;
        }
        // 0.15.0 quest notes: running, not completed quests only, and only notes the player carries or has read.
        const auto  notesStart = std::chrono::steady_clock::now();
        std::size_t notesLooked = 0;
        std::size_t notesShown = 0;
        {
            std::vector<std::pair<Entry*, RE::TESQuest*>> withBooks;
            std::unordered_set<RE::TESBoundObject*>       candidates;
            for (auto& e : snap.entries) {
                const auto quest = RE::TESForm::LookupByID<RE::TESQuest>(e.formID);
                if (!quest || e.completed || e.failed || !quest->IsRunning()) {
                    continue;
                }
                const auto& books = QuestBooks(quest);
                if (books.empty()) {
                    continue;
                }
                withBooks.emplace_back(&e, quest);
                for (const auto id : books) {
                    if (const auto book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(id)) {
                        candidates.insert(book);
                    }
                }
            }
            if (!withBooks.empty()) {
                std::unordered_map<std::uint32_t, std::int32_t> held;
                for (const auto& [obj, count] : player->GetInventoryCounts(
                         [&](RE::TESBoundObject& a_obj) { return candidates.contains(&a_obj); })) {
                    held[obj->GetFormID()] = count;
                }
                for (const auto& [e, quest] : withBooks) {
                    ReadNotes(*e, quest, QuestBooks(quest), held, notesLooked);
                    notesShown += e->notes.size();
                }
            }
        }
        const auto notesMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - notesStart).count();
        logger::info("Quest notes: {} shown, {} newly read ({:.1f} ms).", notesShown, notesLooked, notesMs);
        logger::info("Quest records loaded: {}; objective records: {}; journal descriptions: {} looked up, {} cached ({:.1f} ms); "
                     "{} open objective(s) with a map target, {} with a location, {} with a distance (player {}).",
            questCount, known.size(), looked, snap.entries.size() - looked, ms, snap.mapTargets, snap.located, distances,
            snap.herePlaced ? std::format("outdoors in {:08X} at {:.0f}, {:.0f}", hereSpot.world, hereSpot.x, hereSpot.y) :
                              std::string("not placed"));
        return snap;
    }

    void ClearCache()
    {
        g_descriptions.clear();
        g_exitOfCell.clear();
        g_questBooks.clear();  // forced refs resolve per save
        g_noteText.clear();
        g_noteHeldLogged.clear();
        // g_bookText stays: a book's raw text does not change with the save.
    }

    std::uint32_t FindMapTarget(std::uint32_t a_questFormID, std::uint32_t a_instanceID, std::uint16_t a_objectiveIndex)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return 0;
        }
        std::size_t questCount = 0;
        const auto  known = AllQuestObjectives(questCount);
        std::vector<RawInstance> raw(kMaxInstances);
        std::uint32_t            size = 0;
        std::uint32_t            capacity = 0;
        const int copied = CopyInstances(&player->GetPlayerRuntimeData().objectives, raw.data(), kMaxInstances, &size, &capacity);
        for (int i = 0; i < copied; ++i) {
            const auto& r = raw[static_cast<std::size_t>(i)];
            if (!known.contains(r.objective) || !r.objective->ownerQuest || r.instanceID != a_instanceID ||
                r.objective->index != a_objectiveIndex || r.objective->ownerQuest->GetFormID() != a_questFormID) {
                continue;
            }
            RE::BGSInstancedQuestObjective instance{};
            instance.Objective = r.objective;
            instance.instanceID = r.instanceID;
            instance.InstanceState = static_cast<RE::QUEST_OBJECTIVE_STATE>(r.state);
            if (const auto target = MapTarget(instance)) {
                return target;
            }
        }
        return 0;
    }

    bool ShowOnMap(std::uint32_t a_target)
    {
        if (!REL::Module::IsAE() || a_target == 0) {
            return false;
        }
        static const auto show = REL::ID(53178).address();
        return RawShowOnMap(a_target, show);
    }

    void DumpToLog(const Snapshot& a_snap, bool a_rows)
    {
        static int dumpNumber = 0;
        ++dumpNumber;
        if (!a_snap.playerListValid) {
            logger::error("Quest dump #{} FAILED: {}. Nothing was dereferenced.", dumpNumber, a_snap.problem);
            return;
        }

        std::map<std::string, int> byType;
        int                        active = 0;
        int                        completed = 0;
        for (const auto& e : a_snap.entries) {
            ++byType[TypeName(e.type)];
            active += e.active ? 1 : 0;
            completed += e.completed ? 1 : 0;
        }
        logger::info("===== Quest dump #{}: {} objective entries ({} hidden, skipped) -> {} quest rows ({} tracked, {} completed) =====",
            dumpNumber, a_snap.instanceCount, a_snap.hiddenCount, a_snap.entries.size(), active, completed);
        std::string types;
        for (const auto& [name, count] : byType) {
            types += std::format("{} {}, ", name, count);
        }
        logger::info("By type: {}", types);

        int row = 0;
        for (const auto& e : a_snap.entries) {
            if (!a_rows) {
                break;
            }
            logger::info("{:3}. [{}] {}{}{}{} | {:08X} {} | {} | stage {} | instance {}", ++row, TypeName(e.type),
                e.name.empty() ? "(no name)" : e.name, e.active ? "  <TRACKED>" : "", e.completed ? "  <COMPLETED>" : "",
                e.failed ? "  <FAILED>" : "", e.formID, e.plugin, e.editorID, e.stage, e.instanceID);
            for (const auto& o : e.objectives) {
                logger::info("       - [{} #{}] {}{}{}", StateName(o.state), o.index, o.text,
                    o.mapTarget ? std::format("  (map target {:08X})", o.mapTarget) : std::string{},
                    o.targetRef ? std::format("  (target {:08X} at '{}' / '{}' / '{}', location {:08X}; area '{}' {:08X}, hold {:08X}; {})",
                                      o.targetRef, o.place, o.town, o.hold, o.location, o.areaName, o.area, o.holdID,
                                      o.distance >= 0 ? std::format("{:.0f} m away", o.distance) : std::string("distance unknown")) :
                                  std::string{});
            }
        }

        // Cross-check: quests whose own objective records say "shown" but that are missing from the player's list,
        // and the reverse. Differences are expected for some quests; the counts tell whether the list is sane.
        std::set<std::uint32_t> fromPlayer;
        for (const auto& e : a_snap.entries) {
            fromPlayer.insert(e.formID);
        }
        std::set<std::uint32_t> fromRecords;
        if (const auto data = RE::TESDataHandler::GetSingleton()) {
            for (const auto quest : data->GetFormArray<RE::TESQuest>()) {
                if (!quest) {
                    continue;
                }
                for (const auto objective : quest->objectives) {
                    if (objective && Shown(objective->state.get())) {
                        fromRecords.insert(quest->GetFormID());
                        if (!fromPlayer.contains(quest->GetFormID())) {
                            logger::info("  Cross-check: '{}' {:08X} {} has shown objectives but is not in the player's list.",
                                quest->GetFullName(), quest->GetFormID(), PluginOf(quest));
                        }
                        break;
                    }
                }
            }
        }
        std::size_t onlyPlayer = 0;
        for (const auto id : fromPlayer) {
            onlyPlayer += fromRecords.contains(id) ? 0 : 1;
        }
        logger::info("Cross-check: {} quests in the player's list, {} quests with shown objectives in their records, "
                     "{} in the player's list only.",
            fromPlayer.size(), fromRecords.size(), onlyPlayer);
        logger::info("===== End of quest dump #{} =====", dumpNumber);
    }

    namespace
    {
        // Windows-1252 0x80..0x9F (0 = undefined there).
        constexpr char16_t kCp1252High[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030,
            0x0160, 0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC,
            0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

        // Length of a valid UTF-8 sequence starting at a_i, or 0.
        std::size_t Utf8Length(std::string_view a_s, std::size_t a_i)
        {
            const auto c = static_cast<unsigned char>(a_s[a_i]);
            std::size_t n = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c <= 0xDF ? 2 : 0;
            if (c >= 0xF5 || n == 0 || a_i + n > a_s.size()) {
                return 0;
            }
            for (std::size_t k = 1; k < n; ++k) {
                if ((static_cast<unsigned char>(a_s[a_i + k]) & 0xC0) != 0x80) {
                    return 0;
                }
            }
            return n;
        }

        void AppendString(std::string& a_out, std::string_view a_s)
        {
            a_out += '"';
            for (std::size_t i = 0; i < a_s.size(); ++i) {
                const auto c = static_cast<unsigned char>(a_s[i]);
                if (c == '"' || c == '\\') {
                    a_out += '\\';
                    a_out += static_cast<char>(c);
                } else if (c < 0x20) {
                    a_out += std::format("\\u{:04x}", c);
                } else if (c < 0x80) {
                    a_out += static_cast<char>(c);
                } else if (const auto n = Utf8Length(a_s, i)) {
                    a_out.append(a_s.substr(i, n));
                    i += n - 1;
                } else {
                    const char16_t u = c < 0xA0 ? kCp1252High[c - 0x80] : static_cast<char16_t>(c);
                    a_out += std::format("\\u{:04x}", static_cast<unsigned>(u ? u : 0xFFFD));
                }
            }
            a_out += '"';
        }
    }

    std::string JsonString(std::string_view a_text)
    {
        std::string out;
        AppendString(out, a_text);
        return out;
    }

    std::string ToJson(const Snapshot& a_snap)
    {
        std::string out = std::format(R"({{"valid":{},"problem":)", a_snap.playerListValid);
        AppendString(out, a_snap.problem);
        out += std::format(R"(,"here":{{"area":"{:08X}","hold":"{:08X}","placed":{},"radius":{},"areaName":)", a_snap.hereArea,
            a_snap.hereHold, a_snap.herePlaced, Settings::Get().nearbyRadius);
        AppendString(out, a_snap.hereAreaName);
        out += R"(,"holdName":)";
        AppendString(out, a_snap.hereHoldName);
        out += '}';
        out += R"(,"quests":[)";
        bool first = true;
        for (const auto& e : a_snap.entries) {
            out += first ? "" : ",";
            first = false;
            out += std::format(R"({{"id":"{:08X}","inst":{},"type":{},"typeName":)", e.formID, e.instanceID, e.type);
            AppendString(out, TypeName(e.type));
            out += R"(,"name":)";
            AppendString(out, e.name);
            out += R"(,"plugin":)";
            AppendString(out, e.plugin);
            out += R"(,"desc":)";
            AppendString(out, e.description);
            out += R"(,"notes":[)";
            for (std::size_t n = 0; n < e.notes.size(); ++n) {
                out += std::format(R"({}{{"book":"{:08X}","name":)", n ? "," : "", e.notes[n].book);
                AppendString(out, e.notes[n].name);
                out += R"(,"text":)";
                AppendString(out, e.notes[n].text);
                out += '}';
            }
            out += ']';
            out += std::format(R"(,"tracked":{},"completed":{},"failed":{},"objectives":[)", e.active, e.completed, e.failed);
            for (std::size_t k = 0; k < e.objectives.size(); ++k) {
                out += std::format(R"({}{{"state":{},"index":{},"map":{},)", k ? "," : "", e.objectives[k].state,
                    e.objectives[k].index, e.objectives[k].mapTarget ? "true" : "false");
                out += std::format(R"("loc":"{:08X}","place":)", e.objectives[k].location);
                AppendString(out, e.objectives[k].place);
                out += R"(,"town":)";
                AppendString(out, e.objectives[k].town);
                out += R"(,"hold":)";
                AppendString(out, e.objectives[k].hold);
                out += std::format(R"(,"area":"{:08X}","holdId":"{:08X}","dist":{:.0f},"text":)", e.objectives[k].area,
                    e.objectives[k].holdID, e.objectives[k].distance >= 0 ? e.objectives[k].distance : -1.0f);
                AppendString(out, e.objectives[k].text);
                out += '}';
            }
            out += "]}";
        }
        out += "]}";
        return out;
    }
}
