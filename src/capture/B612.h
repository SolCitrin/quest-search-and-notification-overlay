#pragma once

// B612 NotificationManager (Nexus 127701; Feeding Perk Notification depends on it). Mods register lines with
// it inside the HUD movie; B612 then hides them or turns them into mod events, working on vanilla's MessageArray. A line
// we take never reaches that array, so a line B612 claims is passed on to vanilla untouched
// and B612 handles it exactly as without this capture.
//
// The data layout and the matching rule (facts, not code) are from Notification Tweaks' notes on B612 1.0.27:
// `_root.b612_NotificationManager.NotificationManager_mc.searchItems`, an array of {text, before, after, callback};
// a non-empty `text` is an exact match, otherwise `before` and `after` must match the start and the end; first match wins.
namespace QSO::B612
{
    // HUD thread (the hook's thread: it is the one running HUDMenu::ProcessMessage). Reads the items fresh from the HUD
    // movie (mods add them at any time) and returns true if a_line matches one. False when B612 is absent.
    bool Claims(std::string_view a_line, bool a_logItems);

    // 0.16.1, kDataLoaded (main thread): is B612 installed? Yes if b612.esp is loaded or its notification swf is in
    // Data/Interface (loose; MO2's virtual Data counts). Logged, and told to the box stack for the Settings page, which
    // mentions B612 only when it is there. Needs no hook, so it runs whether game notifications are on or off.
    bool Detect();
}
