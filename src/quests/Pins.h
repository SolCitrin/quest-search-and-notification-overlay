#pragma once

#include <string>

// 0.13.0 pinned quests and 0.14.0 hidden quests, kept per save in the SKSE co-save (the .skse file next to each .ess).
namespace QSO::Pins
{
    // Registers the co-save callbacks. Call from SKSEPluginLoad (SKSE accepts them only during plugin load).
    void Install();

    // "pin:<formID hex>:<1|0>" or (0.14.0) "hide:<formID hex>:<1|0>" from the page. False = malformed command.
    bool Command(const std::string& a_text);

    // The pinned quest FormIDs as a JSON array of 8-digit hex strings, e.g. ["0001A2B3","FE012800"].
    std::string Json();
    // 0.14.0: the hidden quest FormIDs, same format.
    std::string HiddenJson();

    // 0.14.0: a hidden quest is silent (no notification boxes, not counted as nearby). Any thread.
    bool IsHidden(RE::FormID a_quest);
}
