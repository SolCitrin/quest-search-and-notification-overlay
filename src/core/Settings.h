#pragma once

#include <cstdint>
#include <string>

namespace QSO::Settings
{
    struct Values
    {
        // true: the overlay opens on whatever key the player binds to Journal in the Controls menu (that key no longer
        // opens the vanilla journal from gameplay). false: the vanilla Journal key is left alone and the overlay
        // opens on `hotkey` instead.
        bool          interceptJournalKey = true;
        bool          openOnMap = true;  // 0.12.0: the same key opens the overlay on the world map too
        std::uint32_t hotkey = 0;  // DirectInput scan code, used only when interceptJournalKey is false; 0 = none
        bool          showCompleted = false;  // the Completed filter starts switched on
        bool          rememberSearch = false;  // keep the search text and filters between opens
        int           maxObjectives = 3;  // current objectives shown under each quest (1..10)
        bool          groupHeadings = true;  // 0.13.0: the list is grouped under quest-type headings (Main, Daedric, ...)
        // Nearby quests (0.10.0)
        bool          notifyArrive = true;   // "2 quests here in Riften" when you enter a town / dungeon with work there
        bool          notifyLeave = true;    // "Leaving Riften: 2 objectives still here (...)" when you leave it
        bool          notifyNear = true;     // 0.10.4: "Nearby: <objective> (150 m)" when you come within nearbyRadius
        int           nearbyRadius = 300;    // metres
        bool          nearbyMisc = true;     // count Misc quests (radiant favours etc.) in those messages
        int           nearbyCooldown = 90;   // seconds before the same message for the same place can show again
        // Notifications (0.11.0)
        bool          notifyUpdates = true;  // a quest update whose new objective is here / within the radius
        int           boxStyle = 1;          // 1 = QSO boxes, 0 = the game's own top-left text (as before 0.11.0)
        int           boxPosition = 1;       // 0 top-left, 1 top-right, 2 middle-left, 3 middle-right, 4 bottom-left, 5 bottom-right
        int           boxSeconds = 8;        // 1..60
        int           boxScale = 100;        // percent, 50..200
        int           maxBoxes = 5;          // 1..10 on screen at once (quest boxes and game lines: one stack)
        int           minBoxSeconds = 3;     // 0.17.0: 1..30, least time on screen before a waiting box may push one off
        // Game notifications (0.16.0, the merged HUD Notification Replacer; INI section [Capture])
        bool          captureEnabled = false;  // [Capture] bEnabled: the game's white lines become boxes. The hook is put in
                                               // at startup only if this is on then; switching it off works at once.
        bool          captureVerbose = false;  // [Capture] bCaptureVerbose (INI only): extra log lines (holds, view)
        int           lineSeconds = 6;         // [Capture] iLineSeconds, 1..60: each game line's box
    };

    // Reads Data/SKSE/Plugins/QuestSearchOverlay.ini (QSO_FILE_STEM.ini) (missing file or keys = defaults), then the player's own choices
    // from the in-game Settings page (QuestSearchOverlay_Settings.ini next to the log, in My Games). Call once at
    // plugin load.
    void Load();
    const Values& Get();
    // Any thread (the capture hook runs on the HUD's thread): bCaptureEnabled right now.
    bool CaptureOn() noexcept;

    // The Settings page (0.11.0). "key=value;key=value" with the INI key names; unknown keys are ignored. Applies at
    // once and writes the player's file. Returns how many keys were taken.
    int ApplyFromPage(const std::string& a_text);
    // Forget the player's file: back to what QuestSearchOverlay.ini says.
    void ResetToIni();
    // The keys the Settings page can change, as a JSON object for the page.
    std::string PageJson();
}
