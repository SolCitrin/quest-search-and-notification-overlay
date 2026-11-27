#pragma once

#include <cstdint>
#include <string>

// core (0.16.0): ONE box stack for every notification box, quest boxes (quests part) and game lines (capture part).
// The hold rules are HNR 0.2.0's, tested in VanillaTestingList 2026-11-24, now applied to every box:
// - while the J overlay, Dialogue, the journal, a loading screen, the Fader or any pausing / cursor / modal menu is
//   open, new boxes wait here (FIFO, cap 50, the rest folded into one "+N more" box); boxes already on screen pause;
// - after a loading screen or a fade, the release waits 2 s more;
// - a box is DELAYED, NEVER DROPPED. At the box limit the page lets new boxes wait and hurries the oldest one out
//   once it has been up 3 s, and then one box per second while others wait (0.16.1; boxes.js MIN_MS / PACE_MS).
// Everything here runs on the MAIN thread, except Ready() and LinesActive().
namespace QSO::Boxes
{
    // kDataLoaded, after View::Initialize: watch menus (and count the ones open right now, e.g. the main menu).
    void Initialize();

    // Any thread: the page can show boxes.
    bool Ready() noexcept;

    // A quest box: its JSON members without braces ("kind":"near","head":"...","quest":"...", ...). False = the page
    // cannot show boxes (no Prisma UI, or not loaded yet): the caller uses the game's own notification.
    bool PushQuest(std::string a_fields);
    // A game line the capture part took (raw Flash-HTML text). The hook already decided the page is ready.
    void PushLine(std::uint64_t a_seq, std::string a_text);

    // The J overlay opened / closed (the view is shown while it is open; boxes wait).
    void SetPanelOpen(bool a_open);
    // The page reported "nothing on screen or waiting" for box id a_id.
    void OnIdle(std::uint64_t a_id);

    // The capture part installed its hook (kDataLoaded). With the hook in and bCaptureEnabled on, game lines become
    // boxes, and quest messages always use boxes too (the Style choice is greyed out on the Settings page).
    void SetCaptureHooked(bool a_hooked) noexcept;
    bool CaptureHooked() noexcept;
    // Any thread: hooked and switched on right now.
    bool LinesActive() noexcept;

    // 0.16.1: B612 NotificationManager is installed (capture part, kDataLoaded). Its lines stay the game's own text,
    // and the Settings page says so only when this is true.
    void SetB612Present(bool a_present) noexcept;
    bool B612Present() noexcept;
}
