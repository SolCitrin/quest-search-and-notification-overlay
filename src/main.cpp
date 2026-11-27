// Quest Search and Notification Overlay (QSNO), 0.16.0: Quest Search Overlay + the merged HUD Notification Replacer.
// Three parts (CMake QSO_PARTS): core (view, box stack, hold rules, settings, log) + quests (the J overlay, nearby and
// town messages, notes, pins) + capture (the game's white notification lines as boxes). QSO_PARTS=all builds all three
// into QuestSearchOverlay.dll; QSO_PARTS=notify builds core + capture into HUDNotificationReplacer.dll (not shipped).
#include "Boxes.h"
#include "Settings.h"
#include "View.h"

#if QSO_PART_QUESTS
#    include "InputHook.h"
#    include "Nearby.h"
#    include "Overlay.h"
#    include "Pins.h"
#endif
#include "B612.h"
#include "Capture.h"

namespace
{
    void SetupLog()
    {
        auto dir = logger::log_directory();
        if (!dir) {
            return;
        }
        auto path = *dir / QSO_FILE_STEM ".log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        log->set_pattern("[%H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(std::move(log));
    }

    void InstallCapture()
    {
        if (!QSO::Settings::Get().captureEnabled) {
            logger::info("Capture: game notifications are OFF ([Capture] bEnabled=0 at startup): no hook; the game shows its own "
                         "lines. Switching it on takes effect after a restart.");
            return;
        }
        if (!QSO::View::Exists()) {
            logger::warn("Capture: no view, so no hook either; the game shows its own lines.");
            return;
        }
        QSO::Capture::Install();
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
#if QSO_PART_QUESTS
            QSO::Overlay::OnGameLoaded();
            QSO::Nearby::OnGameLoaded();
#endif
            if (QSO::Boxes::CaptureHooked()) {
                QSO::Capture::OnGameLoaded();
            }
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            QSO::View::Initialize(QSO_VIEW_PAGE);  // the page loads while the game finishes starting
            QSO::Boxes::Initialize();
#if QSO_PART_QUESTS
            // 0.3.0: hook at kDataLoaded, not at plugin load. A later write_call wraps the earlier ones, so hooking last
            // puts us OUTSIDE mods that hooked the same input call while loading (SKSE Menu Framework, Wheeler, ...):
            // we see keys first and can keep them from those mods while the overlay is open.
            if (!QSO::InputHook::Install()) {
                logger::warn("Running without the input hook.");
            }
            QSO::InputHook::RegisterMenuWatcher();
            QSO::Overlay::Initialize();
            QSO::Nearby::Install();
#endif
            // Hook here, not at plugin load: by now every plugin has done its load-time patching, so the byte check
            // sees the call site as the game will run it, and a site someone else already took makes us fall back.
            QSO::Boxes::SetB612Present(QSO::B612::Detect());
            InstallCapture();
            logger::info("Data loaded; view, box stack{} and capture ({}) set up.",
                QSO_PART_QUESTS ? ", input hook, Journal Menu watcher, overlay" : "",
                QSO::Boxes::CaptureHooked() ? "hooked" : "not hooked");
            break;
        default:
            break;
        }
    }
}

SKSEPluginInfo(
    .Version = REL::Version{ 1, 0, 0, 0 },
    .Name = QSO_PLUGIN_NAME ""sv,
    .Author = "SolCitrin"sv,
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    // Only Skyrim 1.6 and later (Anniversary Edition builds, including 1.7) are supported. Older runtimes (SE 1.5.x)
    // and Skyrim VR are refused here, before anything reads game memory or the Address Library. The runtime version
    // comes from SKSE itself; the log folder lookup does not touch the game either.
    if (const auto runtime = a_skse->RuntimeVersion(); runtime < REL::Version{ 1, 6, 0, 0 }) {
        SetupLog();
        logger::error("{} {} needs Skyrim 1.6 or later (Anniversary Edition builds); this game is {}. "
                      "Not loading: SE 1.5.x and Skyrim VR are not supported.",
            QSO_DISPLAY_NAME, QSO_VERSION_STRING, runtime.string());
        return false;
    }

    SKSE::Init(a_skse);
    SetupLog();
    logger::info("{} {} loading (game {}; parts: {}).", QSO_DISPLAY_NAME, QSO_VERSION_STRING, REL::Module::get().version().string(),
        QSO_PART_QUESTS ? "core + quests + capture" : "core + capture");

    QSO::Settings::Load();
#if QSO_PART_QUESTS
    QSO::Pins::Install();  // 0.13.0: pinned quests in the co-save (callbacks must be set during plugin load)
#endif
    if (QSO::Settings::Get().captureEnabled) {
        QSO::Capture::LogEarlyBytes();
    }

    SKSE::AllocTrampoline(64);  // reserved now; the hooks themselves are written at kDataLoaded (input 14, capture 14)

    if (const auto messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }
    return true;
}
