#include "InputHook.h"
#include "Overlay.h"
#include "QuestReader.h"
#include "Settings.h"

namespace QSO::InputHook
{
    namespace
    {
        // True from a swallowed J press until its release, so the held/up events of the
        // same press are swallowed too and the game never sees half a key press.
        std::atomic<bool>          g_swallowing{ false };
        std::atomic<std::uint32_t> g_interceptCount{ 0 };
        std::atomic<bool>          g_firstDispatchLogged{ false };
        std::atomic<bool>          g_lastOnMap{ false };  // where the last intercepted press happened (for the log)
        std::atomic<bool>          g_mapKeysLogged{ false };

        // The key the player has bound to the Journal action in a context. Read from the control map each time, so
        // a remapped key works (default J = 0x24). 0xFF = not bound.
        std::uint32_t JournalKeyIn(RE::ControlMap::InputContextID a_context)
        {
            const auto controlMap = RE::ControlMap::GetSingleton();
            const auto userEvents = RE::UserEvents::GetSingleton();
            if (!controlMap || !userEvents) {
                return 0xFF;
            }
            return controlMap->GetMappedKey(userEvents->journal.c_str(), RE::INPUT_DEVICE::kKeyboard, a_context);
        }

        // 0.12.0: the world map has its own "Journal" row in the control map (context kMap). Use it on the map, and
        // the gameplay key if the map row is unbound.
        std::uint32_t JournalKey(bool a_onMap)
        {
            if (a_onMap) {
                const auto mapKey = JournalKeyIn(RE::ControlMap::InputContextID::kMap);
                if (mapKey != 0xFF && mapKey != 0) {
                    return mapKey;
                }
            }
            return JournalKeyIn(RE::ControlMap::InputContextID::kGameplay);
        }

        // The key that opens the overlay: the player's Journal binding, or iHotkey when the INI de-couples them.
        std::uint32_t TriggerKey(bool a_onMap)
        {
            const auto& s = Settings::Get();
            if (s.interceptJournalKey) {
                return JournalKey(a_onMap);
            }
            return s.hotkey ? s.hotkey : 0xFF;
        }

        // The ControlMap context stack, or nullptr if it looks wrong (a layout mismatch): then J stays vanilla
        // instead of reading through a junk pointer.
        const RE::BSTArray<RE::ControlMap::InputContextID>* ContextStack()
        {
            static_assert(sizeof(RE::ControlMap) == 0x130, "build must define SKYRIM_SUPPORT_AE (AE layout)");
            const auto controlMap = RE::ControlMap::GetSingleton();
            if (!controlMap) {
                return nullptr;
            }
            const auto& stack = controlMap->contextPriorityStack;
            const auto  size = stack.size();
            if (size == 0 || size > 64 || stack.data() == nullptr) {
                static std::atomic<bool> warned{ false };
                if (size != 0 && !warned.exchange(true)) {
                    logger::error("ControlMap context stack looks invalid (size {}); J left vanilla.", size);
                }
                return nullptr;
            }
            return &stack;
        }

        // Only take over J during normal gameplay. Anywhere else (menus, console, dialogue,
        // main menu, loading) the key passes through untouched.
        bool InGameplay()
        {
            const auto ui = RE::UI::GetSingleton();
            const auto controlMap = RE::ControlMap::GetSingleton();
            const auto player = RE::PlayerCharacter::GetSingleton();
            if (!ui || !controlMap || !player || !player->Is3DLoaded()) {
                return false;
            }
            if (ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
                return false;
            }
            const auto stack = ContextStack();
            return stack && stack->back() == RE::ControlMap::InputContextID::kGameplay;
        }

        // 0.12.0: the world map is open and is what takes input right now: no console, conversation, journal,
        // loading screen or other menu (another Prisma UI page included) on top of it. The map pauses the game, so
        // InGameplay() is false there. bOpenOnMap=0 turns this off.
        bool InMap()
        {
            if (!Settings::Get().openOnMap) {
                return false;
            }
            const auto ui = RE::UI::GetSingleton();
            const auto player = RE::PlayerCharacter::GetSingleton();
            if (!ui || !player || !player->Is3DLoaded() || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
                return false;
            }
            if (ui->IsMenuOpen(RE::Console::MENU_NAME) || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::JournalMenu::MENU_NAME)) {
                return false;
            }
            const auto stack = ContextStack();
            return stack && stack->back() == RE::ControlMap::InputContextID::kMap;
        }

        bool MapOpen()
        {
            const auto ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
        }

        void OnJournalKeyIntercepted()
        {
            const auto n = ++g_interceptCount;
            const bool onMap = g_lastOnMap.load();
            logger::info("Overlay key (scan code 0x{:02X}, {}) pressed {} (#{}); the game did not see it.", TriggerKey(onMap),
                Settings::Get().interceptJournalKey ? "the Journal binding" : "iHotkey", onMap ? "on the map" : "in gameplay", n);
            if (onMap && !g_mapKeysLogged.exchange(true)) {
                logger::info("Journal key rows: map 0x{:02X}, gameplay 0x{:02X} (0xFF = unbound; on the map the map row "
                             "is used, the gameplay row only if the map row is unbound).",
                    JournalKeyIn(RE::ControlMap::InputContextID::kMap), JournalKeyIn(RE::ControlMap::InputContextID::kGameplay));
            }
            // Milestone 3: open the overlay (it also writes the quest list to the log).
            Overlay::Open();
        }

        // Unlinks events the game must not see. Returns true if an intercept happened.
        bool FilterEvents(RE::InputEvent** a_events)
        {
            bool       intercepted = false;
            const bool overlayOpen = Overlay::IsOpen();
            const auto gameKey = TriggerKey(false);
            const auto mapKey = Settings::Get().openOnMap ? TriggerKey(true) : 0xFFu;
            if (!overlayOpen && gameKey == 0xFF && mapKey == 0xFF) {
                return false;
            }
            unsigned   blockedNow = 0;

            RE::InputEvent* prev = nullptr;
            RE::InputEvent* event = *a_events;
            while (event) {
                RE::InputEvent* const next = event->next;
                bool                  drop = false;

                if (overlayOpen) {
                    // Milestone 3: while the overlay is open, no keyboard event reaches the game, SKSE's Papyrus
                    // key events or any mod hooked inside us. The page still gets typing via PrismaUI's window hook.
                    // Mouse events pass: PrismaUI drives its cursor from them.
                    const auto type = event->GetEventType();
                    if (type == RE::INPUT_EVENT_TYPE::kChar ||
                        (type == RE::INPUT_EVENT_TYPE::kButton && event->GetDevice() == RE::INPUT_DEVICE::kKeyboard)) {
                        drop = true;
                        ++blockedNow;
                        if (type == RE::INPUT_EVENT_TYPE::kButton) {
                            const auto button = static_cast<RE::ButtonEvent*>(event);
                            if (button->GetIDCode() == 0x01 && button->IsDown()) {  // Esc
                                SKSE::GetTaskInterface()->AddTask([] { Overlay::Close("Esc"); });
                            }
                        }
                    }
                } else if (event->GetEventType() == RE::INPUT_EVENT_TYPE::kButton &&
                    event->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                    const auto button = static_cast<RE::ButtonEvent*>(event);
                    const auto code = button->GetIDCode();
                    if (code != 0xFF && (code == gameKey || code == mapKey)) {
                        if (button->IsDown()) {
                            const bool inGame = code == gameKey && InGameplay();
                            const bool onMap = !inGame && code == mapKey && InMap();
                            if (inGame || onMap) {
                                g_lastOnMap = onMap;
                                g_swallowing = true;
                                drop = true;
                                intercepted = true;
                            }
                        } else if (g_swallowing) {
                            drop = true;  // held or released part of a swallowed press
                            if (!button->IsPressed()) {
                                g_swallowing = false;
                            }
                        }
                    }
                }

                if (drop) {
                    if (prev) {
                        prev->next = next;
                    } else {
                        *a_events = next;
                    }
                } else {
                    prev = event;
                }
                event = next;
            }
            if (blockedNow != 0) {
                Overlay::CountBlocked(blockedNow);
            }
            return intercepted;
        }

        struct DispatchInputEvent
        {
            static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent** a_events)
            {
                if (!g_firstDispatchLogged.exchange(true)) {
                    logger::info("Input dispatch hook is live (first call seen).");
                }
                if (a_events && *a_events && FilterEvents(a_events)) {
                    // Runs on the main thread already, but defer UI work until after dispatch.
                    SKSE::GetTaskInterface()->AddTask(OnJournalKeyIntercepted);
                }
                func(a_dispatcher, a_events);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // Logs Journal Menu opening/closing, so the test log shows whether Esc still opens it
        // and J no longer does.
        class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            static MenuWatcher* GetSingleton()
            {
                static MenuWatcher singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (a_event && a_event->menuName == RE::JournalMenu::MENU_NAME) {
                    logger::info("Journal Menu {}.", a_event->opening ? "opened" : "closed");
                } else if (a_event && a_event->menuName == RE::MapMenu::MENU_NAME) {
                    logger::info("Map Menu {}{}.", a_event->opening ? "opened" : "closed",
                        Overlay::IsOpen() ? " (the overlay is open)" : "");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    bool Install()
    {
        REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(67315, 68617), REL::Relocate(0x7B, 0x7B) };
        const auto opcode = *reinterpret_cast<const std::uint8_t*>(target.address());
        if (opcode != 0xE8) {
            logger::critical("Input dispatch site {:X} holds opcode {:02X}, not a CALL (E8). "
                             "Hook NOT installed; J behaves as vanilla.",
                target.address() - REL::Module::get().base(), opcode);
            return false;
        }

        auto& trampoline = SKSE::GetTrampoline();
        DispatchInputEvent::func = trampoline.write_call<5>(target.address(), DispatchInputEvent::thunk);
        logger::info("Input dispatch hook installed at SkyrimSE+{:X}; previous target SkyrimSE+{:X} "
                     "(a different value from the vanilla one means another mod hooked first and we chain to it).",
            target.address() - REL::Module::get().base(),
            DispatchInputEvent::func.address() - REL::Module::get().base());
        return true;
    }

    void RegisterMenuWatcher()
    {
        if (const auto ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatcher::GetSingleton());
        }
    }

    std::uint32_t CurrentJournalKey() { return TriggerKey(Settings::Get().openOnMap && MapOpen()); }
}
