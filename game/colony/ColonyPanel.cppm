export module game.colony.panel;

import engine.core;
import game.colony.driver;
import std;

export namespace pP::colony {
    /// Read-only presentation counts supplied by the renderer/caller.
    /// Path counters ride the same struct (appended): the panel never
    /// imports the pathfinding module.
    struct ColonyPanelCounts final {
        u32 m_visible_chunks{};
        u32 m_visible_tiles{};
        u32 m_paths{};
        u32 m_partials{};
        u32 m_blocked{};
        u32 m_errands{};
    };

    /// Draws the compact colony status overlay; this panel never changes state.
    void drawColonyPanel(const ColonyDriver &driver, ColonyPanelCounts counts);
}
