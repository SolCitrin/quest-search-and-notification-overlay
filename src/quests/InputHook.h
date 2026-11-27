#pragma once

namespace QSO::InputHook
{
    // Hooks the call where BSInputDeviceManager hands each frame's input events to every
    // listener (the game's menu/player controls AND every SKSE input sink). Same call site
    // Wheeler, dMenu, PhotoMode and others hook: RELOCATION_ID(67315, 68617) + 0x7B.
    // Returns false (and installs nothing) if the call site does not look like a call.
    bool Install();

    // Logs Journal Menu open/close (shows in the log that Esc still opens it). Call after kDataLoaded.
    void RegisterMenuWatcher();

    // DirectInput scan code bound to Journal in gameplay (0x24 = J by default; 0xFF if none). Read live, so a
    // rebind in the Controls menu is followed.
    std::uint32_t CurrentJournalKey();
}
