#include "View.h"
#include "Boxes.h"
#include "History.h"
#include "Settings.h"

#include "PrismaUI_API.h"

namespace QSO::View
{
    namespace
    {
        PRISMA_UI_API::IVPrismaUI1* api = nullptr;
        PrismaView                  view = 0;
        std::atomic<bool>           domReady{ false };
        CommandHandler              handler = nullptr;

        void OnDomReady(PrismaView)
        {
            // May run under Prisma UI's own locks: no API calls here, only the flag and a task.
            domReady.store(true);
            SKSE::GetTaskInterface()->AddTask([] { logger::info("Page loaded (DOM ready): boxes and the overlay can show now."); });
        }

        void OnCommand(const char* a_text)
        {
            const std::string text = a_text ? a_text : "";
            SKSE::GetTaskInterface()->AddTask([text] {
                unsigned long long idle = 0;
                if (text.starts_with("toasts:idle:") && std::sscanf(text.c_str(), "toasts:idle:%llu", &idle) == 1) {
                    Boxes::OnIdle(idle);
                } else if (text == "history") {
                    Call("qsoHistory", History::Json());  // 0.17.0: the Notifications tab asks when it opens
                } else if (text.starts_with("log:")) {
                    logger::info("Page: {}", text.substr(4));
                } else if (text == "settings:reset") {
                    Settings::ResetToIni();
                    Call("qsoSettings", Settings::PageJson());
                } else if (text.starts_with("settings:")) {
                    Settings::ApplyFromPage(text.substr(9));
                } else if (!handler || !handler(text)) {
                    logger::warn("The page sent an unknown command '{}'.", text);
                }
            });
        }
    }

    void Initialize(const char* a_page)
    {
        if (api) {
            return;
        }
        api = static_cast<PRISMA_UI_API::IVPrismaUI1*>(PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1));
        if (!api) {
            logger::error("Prisma UI not available: no overlay and no boxes (the game's own notifications are used).");
            return;
        }
        view = api->CreateView(a_page, OnDomReady);
        if (!view) {
            logger::error("Prisma UI refused to create the view {}.", a_page);
            api = nullptr;
            return;
        }
        api->Hide(view);
        api->RegisterJSListener(view, "qsoCommand", OnCommand);
        logger::info("View {} created (Prisma UI V1 API); the overlay and every notification box share it.", a_page);
    }

    void SetCommandHandler(CommandHandler a_handler) { handler = a_handler; }

    bool Ready() noexcept { return api && view && domReady.load(); }
    bool Exists() noexcept { return api && view; }

    void Call(const char* a_function, const std::string& a_argument)
    {
        if (api && view) {
            api->InteropCall(view, a_function, a_argument.c_str());
        }
    }

    void SetShown(bool a_shown)
    {
        if (api && view) {
            a_shown ? api->Show(view) : api->Hide(view);
        }
    }

    bool Focus() { return api && view && api->Focus(view, true, false); }

    void Unfocus()
    {
        if (api && view) {
            api->Unfocus(view);
        }
    }

    bool OtherViewHasFocus() { return api && api->HasAnyActiveFocus(); }
}
