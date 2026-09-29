export module game.colony.smoke;

import engine.core;
import std;

export namespace pP::colony {
    /// Run one headless, offscreen colony frame and verify a generated cell by GPU readback.
    [[nodiscard]] std::error_code runColonySmoke(std::span<const char *const> argv);
    /// Slice 6 gate: 100-agent sustained driver + translator.submit +
    /// GridPass-upload load (600 fixed-1/60 ticked frames, stderr
    /// diagnostics, driver/translator timing CSVs). Default `--smoke`
    /// behavior is unchanged: this runs only under `--load100`.
    [[nodiscard]] Expected<void> runLoad100();
}
