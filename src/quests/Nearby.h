#pragma once

#include <string>

namespace QSO::Nearby
{
    // Listens for the player's location changes (TESActorLocationChangeEvent). Call once at kDataLoaded.
    void Install();

    // A save was loaded or a new game started: forget where the player was and check the new place.
    void OnGameLoaded();

    // The Journal key's name for the messages ("J", "F3", ... or "your journal key").
    std::string KeyLabel();
}
