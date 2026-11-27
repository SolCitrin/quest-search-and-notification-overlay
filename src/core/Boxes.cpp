#include "Boxes.h"
#include "History.h"
#include "Settings.h"
#include "View.h"

#include <deque>
#include <set>
#include <thread>

// Merged in 0.16.0 from QSO 0.11.0 Overlay.cpp (the box half: one view shown only while a box is up, since a shown Prisma
// view is drawn every frame over everything; the blocking-menu set; stale blockers dropped; the page's idle handshake)
// and HNR 0.2.0 Boxes.cpp (the stricter hold: Fader too, the 2 s settle after a load or fade, the FIFO with its cap).
namespace QSO::Boxes
{
    namespace
    {
        bool                  viewShown = false;
        bool                  pausedSent = false;
        bool                  panelOpen = false;    // the J overlay is open
        bool                  pagePending = false;  // the page has boxes on screen or waiting
        std::uint64_t         boxSeq = 0;           // id of the newest box sent to the page
        std::set<std::string> blockers;             // open menus that hold the boxes
        bool                  settling = false;     // the 2 s after a loading screen / fade
        std::uint32_t         settleGen = 0;
        std::atomic<bool>     captureHooked{ false };
        std::atomic<bool>     b612Present{ false };

        struct Held
        {
            bool          line;  // a game line (else a quest box)
            std::uint64_t seq;   // the capture's line number (lines only)
            std::string   payload;
        };
        std::deque<Held>      held;
        std::uint32_t         overflow = 0;
        constexpr std::size_t kHoldCap = 50;

        bool Verbose() { return Settings::Get().captureVerbose; }

        bool IsLoadOrFade(std::string_view a_name)
        {
            return a_name == RE::LoadingMenu::MENU_NAME || a_name == RE::FaderMenu::MENU_NAME;
        }

        // QSO 0.11.0's set plus the Fader Menu (HNR M1: vanilla lost a line during a load door's fade). Dialogue (user
        // decision) and the Journal are in the named set. The HUD and the cursor never count.
        bool Blocks(const std::string& a_name)
        {
            static const std::set<std::string_view> named = { RE::LoadingMenu::MENU_NAME, RE::FaderMenu::MENU_NAME,
                RE::DialogueMenu::MENU_NAME, RE::Console::MENU_NAME, RE::MapMenu::MENU_NAME, RE::InventoryMenu::MENU_NAME,
                RE::MagicMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME, RE::BarterMenu::MENU_NAME, RE::GiftMenu::MENU_NAME,
                RE::StatsMenu::MENU_NAME, RE::TweenMenu::MENU_NAME, RE::JournalMenu::MENU_NAME, RE::BookMenu::MENU_NAME,
                RE::LockpickingMenu::MENU_NAME, RE::CraftingMenu::MENU_NAME, RE::TrainingMenu::MENU_NAME,
                RE::SleepWaitMenu::MENU_NAME, RE::MessageBoxMenu::MENU_NAME, RE::FavoritesMenu::MENU_NAME,
                RE::LevelUpMenu::MENU_NAME, RE::RaceSexMenu::MENU_NAME, RE::MainMenu::MENU_NAME, RE::TutorialMenu::MENU_NAME,
                "PrismaUI_FocusMenu"sv, "CustomMenu"sv };
            if (a_name == RE::HUDMenu::MENU_NAME || a_name == RE::CursorMenu::MENU_NAME) {
                return false;
            }
            if (named.contains(a_name)) {
                return true;
            }
            const auto ui = RE::UI::GetSingleton();
            const auto menu = ui ? ui->GetMenu(a_name) : nullptr;
            using F = RE::IMenu::Flag;
            return menu && menu->menuFlags.any(F::kPausesGame, F::kUsesCursor, F::kModal);
        }

        bool Holding() { return panelOpen || !blockers.empty() || settling; }

        std::string HoldList()
        {
            std::string out = panelOpen ? "the J overlay" : "";
            for (const auto& b : blockers) {
                out += out.empty() ? b : ", " + b;
            }
            if (settling) {
                out += out.empty() ? "2 s after loading/fade" : ", 2 s after loading/fade";
            }
            return out.empty() ? "nothing" : out;
        }

        // A menu whose close event never came would hold the boxes forever: drop blockers that are not open now.
        void DropStaleBlockers()
        {
            const auto ui = RE::UI::GetSingleton();
            if (!ui) {
                return;
            }
            for (auto it = blockers.begin(); it != blockers.end();) {
                if (!ui->IsMenuOpen(*it)) {
                    logger::warn("Boxes: '{}' was still counted as open but is not; no longer holding for it.", *it);
                    it = blockers.erase(it);
                } else {
                    ++it;
                }
            }
        }

        void UpdateVisibility()
        {
            if (!View::Exists()) {
                return;
            }
            const bool paused = Holding();
            if (paused != pausedSent) {
                pausedSent = paused;
                View::Call("qsoToastPause", paused ? "1" : "0");
            }
            const bool want = panelOpen || (pagePending && !paused);
            if (want != viewShown) {
                View::SetShown(want);
                viewShown = want;
                if (Verbose()) {
                    logger::info("Boxes: view {}.", want ? "shown" : "hidden");
                }
            }
        }

        std::string JsonString(std::string_view a_s)
        {
            std::string out = "\"";
            for (const char c : a_s) {
                switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    } else {
                        out += c;  // UTF-8 bytes pass through (HNR M1: lines arrive as UTF-8)
                    }
                }
            }
            return out + "\"";
        }

        // Position, size and the box limit are shared by both kinds (one stack); the seconds are per kind.
        void SendQuest(const std::string& a_fields)
        {
            const auto& o = Settings::Get();
            View::Call("qsoToast", std::format(R"({{"id":{},"pos":{},"secs":{},"min":{},"scale":{},"max":{},{}}})", ++boxSeq, o.boxPosition,
                                       o.boxSeconds, o.minBoxSeconds, o.boxScale, o.maxBoxes, a_fields));
            pagePending = true;
        }

        // The text is sent raw; the page turns it into DOM nodes itself (font colours, b, i, u, br kept; every other tag
        // dropped; never innerHTML).
        void SendLine(const std::string& a_text, bool a_fold)
        {
            const auto& o = Settings::Get();
            View::Call("qsoToast", std::format(R"({{"id":{},"pos":{},"secs":{},"min":{},"scale":{},"max":{},"box":"game","fold":{},"text":{}}})",
                                       ++boxSeq, o.boxPosition, o.lineSeconds, o.minBoxSeconds, o.boxScale, o.maxBoxes, a_fold ? "true" : "false",
                                       JsonString(a_text)));
            pagePending = true;
        }

        void Release(const std::string& a_why)
        {
            if (Holding() || (held.empty() && !overflow)) {
                UpdateVisibility();
                return;
            }
            // Unpause the page FIRST (0.16.0 fix; HNR 0.2.0 sent the backlog to a still-paused page): a paused page lets
            // everything wait, and with 50 held + the "+N more" box it would fold the OLDEST held line into a second
            // "+1 more" box. Unpaused, it shows up to the box limit at once and the rest wait under its own cap of 50.
            UpdateVisibility();
            const auto lines = std::ranges::count_if(held, [](const Held& h) { return h.line; });
            logger::info("Boxes: released {} held box(es) ({} game line(s), {} quest box(es)){} ({}).", held.size(), lines,
                held.size() - lines, overflow ? std::format(" + {} folded", overflow) : "", a_why);
            for (const auto& h : held) {
                if (h.line) {
                    SendLine(h.payload, false);
                    logger::info("Capture: #{} shown as box {} (released).", h.seq, boxSeq);
                } else {
                    SendQuest(h.payload);
                }
            }
            held.clear();
            if (overflow) {
                SendLine(std::format("+{} more notifications, not shown (all in " QSO_FILE_STEM ".log)", overflow), true);
                overflow = 0;
            }
            UpdateVisibility();
        }

        void EndSettle(std::uint32_t a_gen)
        {
            if (a_gen != settleGen || !settling) {
                return;
            }
            settling = false;
            Release("2 s after loading/fade");
        }

        // Shown now or held. a_line: a game line (a_seq = the capture's line number).
        void Push(bool a_line, std::uint64_t a_seq, std::string a_payload)
        {
            if (!blockers.empty()) {
                DropStaleBlockers();
            }
            if (Holding()) {
                if (held.size() >= kHoldCap) {
                    ++overflow;
                    if (a_line) {
                        History::MarkFolded(a_seq);  // still listed in the Notifications tab, marked "not shown"
                    }
                    logger::info("{} held, queue full ({} boxes): folded into the \"+N more\" box (holding for: {}).",
                        a_line ? std::format("Capture: #{}", a_seq) : "Boxes: a quest box", kHoldCap, HoldList());
                } else {
                    held.push_back({ a_line, a_seq, std::move(a_payload) });
                    logger::info("{} held ({} waiting; holding for: {}).", a_line ? std::format("Capture: #{}", a_seq) : "Boxes: a quest box",
                        held.size(), HoldList());
                }
                UpdateVisibility();
                return;
            }
            if (!held.empty() || overflow) {
                Release("blockers turned out stale");  // older boxes first: keep the order
            }
            if (a_line) {
                SendLine(a_payload, false);
                logger::info("Capture: #{} shown as box {}.", a_seq, boxSeq);
            } else {
                SendQuest(a_payload);
            }
            UpdateVisibility();
        }

        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!a_event || a_event->menuName.empty()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                std::string name(a_event->menuName.c_str());
                const bool  opening = a_event->opening;
                SKSE::GetTaskInterface()->AddTask([name, opening] {
                    if (opening) {
                        if (!Blocks(name)) {
                            return;
                        }
                        blockers.insert(name);
                        if (IsLoadOrFade(name)) {
                            settling = false;
                            ++settleGen;  // a release scheduled by an earlier load is void now
                        }
                        if (Verbose()) {
                            logger::info("Boxes: {} opened (holding for: {}).", name, HoldList());
                        }
                    } else {
                        if (!blockers.erase(name)) {
                            return;
                        }
                        if (IsLoadOrFade(name) && std::ranges::none_of(blockers, [](const std::string& b) { return IsLoadOrFade(b); })) {
                            settling = true;
                            const auto gen = ++settleGen;
                            std::thread([gen] {
                                std::this_thread::sleep_for(std::chrono::seconds(2));
                                SKSE::GetTaskInterface()->AddTask([gen] { EndSettle(gen); });
                            }).detach();
                        }
                        if (Verbose()) {
                            logger::info("Boxes: {} closed (still holding for: {}).", name, HoldList());
                        }
                        Release(std::format("{} closed", name));
                    }
                    UpdateVisibility();
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        MenuSink g_menuSink;
    }

    void Initialize()
    {
        static bool done = false;
        if (done || !View::Exists()) {
            return;
        }
        done = true;
        if (const auto ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
            // Menus already open now (the main menu at kDataLoaded) never send us an "opened" event.
            for (const auto& [name, entry] : ui->menuMap) {
                if (entry.menu && entry.menu->OnStack() && name.c_str() && Blocks(name.c_str())) {
                    blockers.insert(name.c_str());
                }
            }
        }
        logger::info("Boxes: one stack for quest boxes and game lines; holding at start for: {}.", HoldList());
    }

    bool Ready() noexcept { return View::Ready(); }

    bool PushQuest(std::string a_fields)
    {
        if (!View::Ready()) {
            return false;
        }
        History::AddQuest(a_fields, "box");
        Push(false, 0, std::move(a_fields));
        return true;
    }

    void PushLine(std::uint64_t a_seq, std::string a_text) { Push(true, a_seq, std::move(a_text)); }

    void SetPanelOpen(bool a_open)
    {
        panelOpen = a_open;
        if (a_open) {
            UpdateVisibility();
        } else {
            Release("the J overlay closed");  // waits on for the focus menu's own close event, if it is still open
        }
    }

    void OnIdle(std::uint64_t a_id)
    {
        if (a_id == boxSeq) {  // an older "idle" that crossed a newer box on the way is ignored
            pagePending = false;
            UpdateVisibility();
        }
    }

    void SetCaptureHooked(bool a_hooked) noexcept { captureHooked = a_hooked; }
    bool CaptureHooked() noexcept { return captureHooked.load(); }
    bool LinesActive() noexcept { return captureHooked.load() && Settings::CaptureOn(); }
    void SetB612Present(bool a_present) noexcept { b612Present = a_present; }
    bool B612Present() noexcept { return b612Present.load(); }
}
