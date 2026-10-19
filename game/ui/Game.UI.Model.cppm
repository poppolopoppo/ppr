module;

export module game.ui.model;

import engine.core;
import engine.sim;
import game.colony.driver;
import game.colony.panel;
import std;

export namespace pP::game::ui {
    // ------------------------------------------------------------------
    // driver presentation adapter
    // ------------------------------------------------------------------

    /// Reuses snapshot storage and borrows current names/rows only for the
    /// following synchronous UI call. The driver remains owned by the host.
    void captureColonyPresentation(
        colony::ColonyPresentation &out, const colony::ColonyDriver &driver, std::optional<sim::GlobalCellPos> hover,
        std::optional<sim::GlobalCellPos> inspected);
}
