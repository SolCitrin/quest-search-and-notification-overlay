#pragma once

#include <cstdint>
#include <string>

// core (0.17.0): the Notifications tab. Every notification of this game session, newest first, so a player can read
// one they missed. Kept in memory only: nothing goes into the save, and it is gone when the game closes.
// Cap kCap entries; the oldest drop off.
namespace QSO::History
{
    // 300: a long session's worth of ordinary lines (a few per minute), and still a whole 80-line flood plus what came
    // before it. Costs at most a few hundred KB, and the page gets the JSON only when the tab is opened.
    constexpr std::size_t kCap = 300;

    // Main thread. a_fields: a quest box's JSON members without braces (as Boxes::PushQuest). a_how: "box" or
    // "vanilla" (shown as the game's own text: iBoxStyle=0 or no page).
    void AddQuest(const std::string& a_fields, const char* a_how);
    // Any thread (the capture hook). a_seq: the capture's line number (0 = none). a_how: the capture's fate
    // ("box", "vanilla-disabled", "vanilla-B612", "vanilla-not-ready").
    void AddLine(std::uint64_t a_seq, const std::string& a_text, const char* a_how);
    // Main thread: a line that will not be shown on screen (folded into a "+N more" box).
    void MarkFolded(std::uint64_t a_seq);

    // Main thread: {"cap":300,"total":N,"entries":[...]} newest first; each entry {"n","real","game","kind":"quest"|"game",
    // "how", "folded", then the quest fields or "text"}.
    std::string Json();
}
