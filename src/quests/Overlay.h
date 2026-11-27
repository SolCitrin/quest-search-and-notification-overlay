#pragma once

#include <string>

namespace QSO::Overlay
{
    // 0.16.0: registers the overlay's page commands with the core view (View::Initialize must have run). Main thread,
    // kDataLoaded.
    void Initialize();

    // True from Open() until Close(): the input hook keeps every keyboard event from the game and
    // other mods while this is set (the page still receives typing through PrismaUI's own window hook).
    bool IsOpen() noexcept;

    // Main thread only.
    void Open();
    void Close(const char* a_reason);

    // A save was loaded or a new game started: the page forgets what it had seen (for its NEW badges).
    void OnGameLoaded();

    // Counts keyboard events the input hook kept away from the game while the overlay was open.
    void CountBlocked(unsigned a_events) noexcept;

    // 0.11.0 notification boxes, since 0.16.0 through the core's shared stack (Boxes::PushQuest). Main thread only. a_fields: the box's JSON members without braces, e.g.
    // "kind":"near","head":"Nearby · 150 m","quest":"...","line":"...","key":"J". The page shows it (same view as the
    // overlay; the view is shown only while a box is up, and boxes wait while a menu is open). False = the page
    // cannot show boxes (no PrismaUI, or the page has not loaded): use the game's own notification instead.
    bool Toast(const std::string& a_fields);
}
