#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace QSO::Settings
{
    namespace
    {
        Values g_values;
        Values g_fromIni;  // what QuestSearchOverlay.ini alone says (for "Reset")
        std::atomic<bool> g_captureOn{ false };  // mirror of g_values.captureEnabled for the hook's thread

        void Mirror() { g_captureOn.store(g_values.captureEnabled); }

        // The keys the in-game Settings page may change (0.11.0), in the order they are written.
        constexpr const char* kPageKeys[] = { "bGroupHeadings", "bOpenOnMap", "bNotifyArrive", "bNotifyLeave", "bNotifyNearby", "bNotifyUpdates",
            "bNearbyIncludeMisc", "iNearbyRadius", "iNearbyCooldown", "iBoxStyle", "iBoxPosition", "iBoxSeconds", "iBoxScale",
            "iMaxBoxes", "iMinBoxSeconds", "bCaptureEnabled", "iLineSeconds" };

        std::string Trim(std::string a_s)
        {
            const auto first = a_s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                return {};
            }
            const auto last = a_s.find_last_not_of(" \t\r\n");
            return a_s.substr(first, last - first + 1);
        }

        std::string Lower(std::string a_s)
        {
            std::ranges::transform(a_s, a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return a_s;
        }

        // Accepts decimal or 0x-prefixed hex (scan codes are usually written in hex).
        bool ParseInt(const std::string& a_text, long& a_out)
        {
            try {
                std::size_t used = 0;
                a_out = std::stol(a_text, &used, 0);
                return used == a_text.size();
            } catch (...) {
                return false;
            }
        }

        int Clamp(long a_v, long a_lo, long a_hi) { return static_cast<int>(std::clamp(a_v, a_lo, a_hi)); }

        // a_key lower case. False = unknown key.
        bool ApplyKey(Values& a_v, const std::string& a_key, long a_value)
        {
            if (a_key == "binterceptjournalkey") {
                a_v.interceptJournalKey = a_value != 0;
            } else if (a_key == "bgroupheadings") {
                a_v.groupHeadings = a_value != 0;
            } else if (a_key == "bopenonmap") {
                a_v.openOnMap = a_value != 0;
            } else if (a_key == "ihotkey") {
                a_v.hotkey = a_value > 0 && a_value < 0x100 ? static_cast<std::uint32_t>(a_value) : 0;
            } else if (a_key == "bshowcompleted") {
                a_v.showCompleted = a_value != 0;
            } else if (a_key == "bremembersearch") {
                a_v.rememberSearch = a_value != 0;
            } else if (a_key == "imaxobjectives") {
                a_v.maxObjectives = Clamp(a_value, 1, 10);
            } else if (a_key == "bnotifyarrive") {
                a_v.notifyArrive = a_value != 0;
            } else if (a_key == "bnotifyleave") {
                a_v.notifyLeave = a_value != 0;
            } else if (a_key == "bnotifynearby") {
                a_v.notifyNear = a_value != 0;
            } else if (a_key == "inearbyradius") {
                a_v.nearbyRadius = Clamp(a_value, 50, 5000);
            } else if (a_key == "bnotifyhold") {
                logger::info("Settings: bNotifyHold is gone since 0.10.4 (hold messages were replaced by distance); ignored.");
            } else if (a_key == "bnearbyincludemisc") {
                a_v.nearbyMisc = a_value != 0;
            } else if (a_key == "inearbycooldown") {
                a_v.nearbyCooldown = Clamp(a_value, 0, 3600);
            } else if (a_key == "bnotifyupdates") {
                a_v.notifyUpdates = a_value != 0;
            } else if (a_key == "iboxstyle") {
                a_v.boxStyle = a_value != 0 ? 1 : 0;
            } else if (a_key == "iboxposition") {
                a_v.boxPosition = Clamp(a_value, 0, 5);
            } else if (a_key == "iboxseconds") {
                a_v.boxSeconds = Clamp(a_value, 1, 60);
            } else if (a_key == "iboxscale") {
                a_v.boxScale = Clamp(a_value, 50, 200);
            } else if (a_key == "imaxboxes") {
                a_v.maxBoxes = Clamp(a_value, 1, 10);
            } else if (a_key == "iminboxseconds") {
                a_v.minBoxSeconds = Clamp(a_value, 1, 30);
            } else if (a_key == "bcaptureenabled") {
                a_v.captureEnabled = a_value != 0;
            } else if (a_key == "bcaptureverbose") {
                a_v.captureVerbose = a_value != 0;
            } else if (a_key == "ilineseconds") {
                a_v.lineSeconds = Clamp(a_value, 1, 60);
            } else {
                return false;
            }
            return true;
        }

        void ReadFile(const std::filesystem::path& a_path, Values& a_v, bool a_pageKeysOnly)
        {
            std::ifstream in(a_path);
            if (!in) {
                logger::info("Settings: {} not found{}.", a_path.string(), a_pageKeysOnly ? "" : ", using defaults");
                return;
            }
            std::string line;
            std::string section;
            int         lineNo = 0;
            while (std::getline(in, line)) {
                ++lineNo;
                if (const auto comment = line.find_first_of(";#"); comment != std::string::npos) {
                    line.erase(comment);
                }
                line = Trim(line);
                if (!line.empty() && line.front() == '[') {
                    section = Lower(Trim(line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1)));
                    continue;
                }
                if (line.empty()) {
                    continue;
                }
                const auto eq = line.find('=');
                long       value = 0;
                if (eq == std::string::npos || !ParseInt(Trim(line.substr(eq + 1)), value)) {
                    logger::warn("Settings: {} line {} ignored ('{}').", a_path.filename().string(), lineNo, line);
                    continue;
                }
                auto key = Lower(Trim(line.substr(0, eq)));
                if (section == "capture" && key == "benabled") {
                    key = "bcaptureenabled";  // 0.16.0: [Capture] bEnabled is the page's bCaptureEnabled
                }
                if (a_pageKeysOnly && std::ranges::none_of(kPageKeys, [&](const char* k) { return Lower(k) == key; })) {
                    logger::warn("Settings: '{}' in {} is not a Settings-page key; ignored.", key, a_path.filename().string());
                    continue;
                }
                if (!ApplyKey(a_v, key, value)) {
                    logger::warn("Settings: unknown key '{}' on line {} of {}.", key, lineNo, a_path.filename().string());
                }
            }
            logger::info("Settings: read {}.", a_path.string());
        }

        std::filesystem::path UserFile()
        {
            const auto dir = logger::log_directory();
            return dir ? *dir / QSO_FILE_STEM "_Settings.ini" : std::filesystem::path();
        }

        long ValueOf(const Values& a_v, std::string_view a_key)
        {
            if (a_key == "bGroupHeadings") return a_v.groupHeadings;
            if (a_key == "bOpenOnMap") return a_v.openOnMap;
            if (a_key == "bNotifyArrive") return a_v.notifyArrive;
            if (a_key == "bNotifyLeave") return a_v.notifyLeave;
            if (a_key == "bNotifyNearby") return a_v.notifyNear;
            if (a_key == "bNotifyUpdates") return a_v.notifyUpdates;
            if (a_key == "bNearbyIncludeMisc") return a_v.nearbyMisc;
            if (a_key == "iNearbyRadius") return a_v.nearbyRadius;
            if (a_key == "iNearbyCooldown") return a_v.nearbyCooldown;
            if (a_key == "iBoxStyle") return a_v.boxStyle;
            if (a_key == "iBoxPosition") return a_v.boxPosition;
            if (a_key == "iBoxSeconds") return a_v.boxSeconds;
            if (a_key == "iBoxScale") return a_v.boxScale;
            if (a_key == "iMaxBoxes") return a_v.maxBoxes;
            if (a_key == "iMinBoxSeconds") return a_v.minBoxSeconds;
            if (a_key == "bCaptureEnabled") return a_v.captureEnabled;
            if (a_key == "iLineSeconds") return a_v.lineSeconds;
            return 0;
        }

        void WriteUserFile()
        {
            const auto path = UserFile();
            if (path.empty()) {
                logger::warn("Settings: no My Games folder; the Settings page choices last until the game closes.");
                return;
            }
            const auto    temp = std::filesystem::path(path).concat(".tmp");
            std::ofstream out(temp, std::ios::trunc);
            if (!out) {
                logger::error("Settings: cannot write {}.", temp.string());
                return;
            }
            out << "; " QSO_DISPLAY_NAME ": your choices from the in-game Settings page (J, gear button).\n"
                   "; They override Data/SKSE/Plugins/" QSO_FILE_STEM ".ini. Delete this file to go back to the INI.\n"
                   "[Settings page]\n";
            for (const auto* k : kPageKeys) {
                out << k << '=' << ValueOf(g_values, k) << '\n';
            }
            out.close();
            std::error_code ec;
            std::filesystem::rename(temp, path, ec);
            if (ec) {
                logger::error("Settings: cannot replace {} ({}).", path.string(), ec.message());
            }
        }

        void LogValues(const char* a_when)
        {
            const auto& v = g_values;
            logger::info("Settings ({}): bInterceptJournalKey={} bOpenOnMap={} bGroupHeadings={} iHotkey=0x{:02X} bShowCompleted={} bRememberSearch={} "
                         "iMaxObjectives={} bNotifyArrive={} bNotifyLeave={} bNotifyNearby={} iNearbyRadius={} "
                         "bNearbyIncludeMisc={} iNearbyCooldown={} bNotifyUpdates={} iBoxStyle={} iBoxPosition={} "
                         "iBoxSeconds={} iBoxScale={} iMaxBoxes={} iMinBoxSeconds={} [Capture] bEnabled={} bCaptureVerbose={} iLineSeconds={}",
                a_when, v.interceptJournalKey, v.openOnMap, v.groupHeadings, v.hotkey, v.showCompleted, v.rememberSearch, v.maxObjectives, v.notifyArrive,
                v.notifyLeave, v.notifyNear, v.nearbyRadius, v.nearbyMisc, v.nearbyCooldown, v.notifyUpdates, v.boxStyle,
                v.boxPosition, v.boxSeconds, v.boxScale, v.maxBoxes, v.minBoxSeconds, v.captureEnabled, v.captureVerbose, v.lineSeconds);
        }
    }

    void Load()
    {
        ReadFile("Data/SKSE/Plugins/" QSO_FILE_STEM ".ini", g_values, false);
        g_fromIni = g_values;
        if (const auto user = UserFile(); !user.empty() && std::filesystem::exists(user)) {
            ReadFile(user, g_values, true);
        }
        Mirror();
        LogValues("loaded");
        if (!g_values.interceptJournalKey && g_values.hotkey == 0) {
            logger::warn("Settings: bInterceptJournalKey=0 and no iHotkey set: nothing opens the overlay.");
        }
    }

    const Values& Get() { return g_values; }
    bool          CaptureOn() noexcept { return g_captureOn.load(); }

    int ApplyFromPage(const std::string& a_text)
    {
        int         taken = 0;
        std::size_t at = 0;
        while (at <= a_text.size()) {
            const auto end = std::min(a_text.find(';', at), a_text.size());
            const auto pair = a_text.substr(at, end - at);
            at = end + 1;
            const auto eq = pair.find('=');
            long       value = 0;
            if (eq == std::string::npos || !ParseInt(Trim(pair.substr(eq + 1)), value)) {
                continue;
            }
            const auto key = Lower(Trim(pair.substr(0, eq)));
            if (std::ranges::none_of(kPageKeys, [&](const char* k) { return Lower(k) == key; })) {
                logger::warn("Settings page sent '{}', not a page key; ignored.", key);
                continue;
            }
            if (ApplyKey(g_values, key, value)) {
                ++taken;
            }
        }
        if (taken) {
            const bool wasOn = g_captureOn.load();
            Mirror();
            if (wasOn != g_values.captureEnabled) {
                logger::info("Capture: switched {} on the Settings page{}.", g_values.captureEnabled ? "ON" : "OFF",
                    g_values.captureEnabled ? " (works at once if the hook is in, else after a restart)" : ": lines go to the game's own text at once");
            }
            WriteUserFile();
            LogValues("Settings page");
        }
        return taken;
    }

    void ResetToIni()
    {
        g_values = g_fromIni;
        Mirror();
        if (const auto user = UserFile(); !user.empty()) {
            std::error_code ec;
            std::filesystem::remove(user, ec);
        }
        LogValues("reset to the INI");
    }

    std::string PageJson()
    {
        std::string out = "{";
        bool        first = true;
        for (const auto* k : kPageKeys) {
            out += std::format(R"({}"{}":{})", first ? "" : ",", k, ValueOf(g_values, k));
            first = false;
        }
        return out + "}";
    }
}
