#pragma once

// The ShowMessage hook: every plain HUD notification line is logged, then taken for our boxes (M2) or left to vanilla.
namespace QSO::Capture
{
    // Plugin load: logs the call-site bytes as the exe has them before other plugins' kDataLoaded work.
    void LogEarlyBytes();
    // kDataLoaded: checks the ShowMessage call site and hooks it, or falls back to HUDMenu vtable slot 0x4.
    void Install();
    // kPostLoadGame / kNewGame: the next captured line dumps B612's patterns again (they register per load).
    void OnGameLoaded();
}
