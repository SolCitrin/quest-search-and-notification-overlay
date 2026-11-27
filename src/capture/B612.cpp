#include "B612.h"

namespace
{
    // The HUD movie resolves AS2 paths without regard to case; spelled as B612's own Papyrus script does.
    constexpr const char* kItemsPath = "_root.b612_NotificationManager.NotificationManager_mc.searchItems";

    struct Item
    {
        std::optional<std::string> text;
        std::optional<std::string> before;
        std::optional<std::string> after;
    };

    std::optional<std::string> Member(const RE::GFxValue& a_obj, const char* a_name)
    {
        RE::GFxValue v;
        if (!a_obj.GetMember(a_name, &v)) {
            return std::nullopt;
        }
        if (v.IsString()) {
            return std::string(v.GetString() ? v.GetString() : "");
        }
        if (v.IsStringW()) {
            // Rare; keep ASCII, mark the rest (B612 lines are ASCII in practice).
            std::string out;
            for (const wchar_t* w = v.GetStringW(); w && *w; ++w) {
                out += *w < 0x80 ? static_cast<char>(*w) : '?';
            }
            return out;
        }
        return std::nullopt;
    }

    bool Matches(const Item& a_item, std::string_view a_line)
    {
        if (a_item.text && !a_item.text->empty()) {
            return *a_item.text == a_line;
        }
        // An item without text goes by before/after; an undefined `after` never matches (B612's own isMatch).
        return a_item.before && a_item.after && a_line.starts_with(*a_item.before) && a_line.ends_with(*a_item.after);
    }

    std::string Show(const std::optional<std::string>& a_s) { return a_s ? std::format("\"{}\"", *a_s) : "undefined"; }
}

namespace QSO::B612
{
    bool Detect()
    {
        const auto data = RE::TESDataHandler::GetSingleton();
        const bool esp = data && data->LookupModByName("b612.esp");
        std::error_code ec;
        const bool swf = std::filesystem::exists("Data/Interface/b612_notificationmanager.swf", ec);
        logger::info("Capture: B612 {} (b612.esp {}, Interface/b612_notificationmanager.swf {}).", esp || swf ? "installed" : "not installed",
            esp ? "loaded" : "not loaded", swf ? "found" : "not found");
        return esp || swf;
    }

    bool Claims(std::string_view a_line, bool a_logItems)
    {
        const auto ui = RE::UI::GetSingleton();
        const auto hud = ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr;
        if (!hud || !hud->uiMovie) {
            if (a_logItems) {
                logger::info("Capture: B612: HUD movie not available; no line passed through for B612.");
            }
            return false;
        }
        RE::GFxValue array;
        if (!hud->uiMovie->GetVariable(&array, kItemsPath) || !array.IsArray()) {
            if (a_logItems) {
                logger::info("Capture: B612: {} not found (B612 not installed or not in this HUD); no line is passed through for it.", kItemsPath);
            }
            return false;
        }
        const auto size = array.GetArraySize();
        if (a_logItems) {
            logger::info("Capture: B612: {} item(s) registered; lines matching them go to vanilla (B612 handles them):", size);
        }
        bool claimed = false;
        for (std::uint32_t i = 0; i < size; ++i) {
            RE::GFxValue entry;
            if (!array.GetElement(i, &entry) || !entry.IsObject()) {
                continue;
            }
            Item item{ Member(entry, "text"), Member(entry, "before"), Member(entry, "after") };
            if (a_logItems) {
                logger::info("Capture: B612:   [{}] text={} before={} after={} callback={}", i, Show(item.text), Show(item.before), Show(item.after),
                    Show(Member(entry, "callback")));
            }
            if (!claimed && Matches(item, a_line)) {
                claimed = true;
                if (!a_logItems) {
                    return true;
                }
            }
        }
        return claimed;
    }
}
