#pragma once

#include <string>

// core (0.16.0): the one Prisma UI view of the plugin. Prisma UI's docs say "Create only one PrismaView per plugin", so
// the J overlay (quests part) and the notification boxes (core Boxes) share this page. Commands from the page come in
// through one JS listener, `qsoCommand`; the core handles its own ("toasts:idle:", "log:", "settings:") and hands the
// rest to the handler a part registered.
namespace QSO::View
{
    // Main thread, true = the command was handled.
    using CommandHandler = bool (*)(const std::string&);

    // kDataLoaded, main thread: creates the (hidden) view. a_page = "QuestSearchOverlay/index.html" etc.
    void Initialize(const char* a_page);
    void SetCommandHandler(CommandHandler a_handler);

    // Any thread: Prisma UI answered and the page has loaded.
    bool Ready() noexcept;
    // Main thread: the view exists (the page may still be loading).
    bool Exists() noexcept;

    // Main thread. Each does nothing without a view.
    void Call(const char* a_function, const std::string& a_argument);
    void SetShown(bool a_shown);
    bool Focus();
    void Unfocus();
    bool OtherViewHasFocus();
}
