#include "History.h"

#include <deque>
#include <mutex>

namespace QSO::History
{
    namespace
    {
        struct Entry
        {
            std::uint64_t n;       // running number in this game session
            std::uint64_t seq;     // capture line number (game lines), 0 = none
            bool          quest;   // a quest box (payload = its JSON members), else a game line (payload = raw text)
            bool          folded;  // never shown on screen: folded into "+N more"
            std::string   how;
            std::string   real;    // "20:14:05"
            std::string   game;    // "8:14 PM, 17 Last Seed"
            std::string   payload;
        };

        std::mutex        g_lock;
        std::deque<Entry> g_entries;  // oldest first
        std::uint64_t     g_total = 0;

        std::string JsonString(std::string_view a_s)
        {
            std::string out = "\"";
            for (const char c : a_s) {
                switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    } else {
                        out += c;
                    }
                }
            }
            return out + "\"";
        }

        std::string RealClock()
        {
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm    t{};
            localtime_s(&t, &now);
            return std::format("{:02}:{:02}:{:02}", t.tm_hour, t.tm_min, t.tm_sec);
        }

        // Plain reads of the calendar's globals (no game call), so any thread may do it.
        std::string GameClock()
        {
            try {
                const auto cal = RE::Calendar::GetSingleton();
                if (!cal || !cal->gameHour || !cal->gameDay || !cal->gameMonth) {
                    return {};
                }
                const auto hour = cal->GetHour();
                auto       h = static_cast<int>(hour);
                const auto m = static_cast<int>((hour - static_cast<float>(h)) * 60.0f);
                const char* ampm = h >= 12 ? "PM" : "AM";
                h %= 12;
                if (h == 0) {
                    h = 12;
                }
                static constexpr const char* kMonths[] = { "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand", "Second Seed",
                    "Midyear", "Sun's Height", "Last Seed", "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star" };
                const auto month = cal->GetMonth();
                return std::format("{}:{:02} {}, {} {}", h, m, ampm, static_cast<int>(cal->GetDay()), month < 12 ? kMonths[month] : "?");
            } catch (...) {
                return {};
            }
        }

        void Add(Entry a_entry)
        {
            a_entry.real = RealClock();
            a_entry.game = GameClock();
            std::scoped_lock lock(g_lock);
            a_entry.n = ++g_total;
            g_entries.push_back(std::move(a_entry));
            while (g_entries.size() > kCap) {
                g_entries.pop_front();
            }
        }
    }

    void AddQuest(const std::string& a_fields, const char* a_how)
    {
        Add({ 0, 0, true, false, a_how ? a_how : "box", {}, {}, a_fields });
    }

    void AddLine(std::uint64_t a_seq, const std::string& a_text, const char* a_how)
    {
        Add({ 0, a_seq, false, false, a_how ? a_how : "box", {}, {}, a_text });
    }

    void MarkFolded(std::uint64_t a_seq)
    {
        if (!a_seq) {
            return;
        }
        std::scoped_lock lock(g_lock);
        for (auto it = g_entries.rbegin(); it != g_entries.rend(); ++it) {
            if (!it->quest && it->seq == a_seq) {
                it->folded = true;
                break;
            }
        }
    }

    std::string Json()
    {
        std::scoped_lock lock(g_lock);
        std::string      out = std::format(R"({{"cap":{},"total":{},"entries":[)", kCap, g_total);
        bool             first = true;
        for (auto it = g_entries.rbegin(); it != g_entries.rend(); ++it) {
            out += std::format(R"({}{{"n":{},"real":"{}","game":{},"kind":"{}","how":{},"folded":{},)", first ? "" : ",", it->n,
                it->real, JsonString(it->game), it->quest ? "quest" : "game", JsonString(it->how), it->folded ? "true" : "false");
            out += it->quest ? it->payload : std::format(R"("text":{})", JsonString(it->payload));
            out += "}";
            first = false;
        }
        return out + "]}";
    }
}
