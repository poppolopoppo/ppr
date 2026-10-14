export module game.colony.smoke;

import engine.core;
import game.colony.driver;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // smoke options
    // ------------------------------------------------------------------

    /// Typed smoke options, resolved at the game invocation boundary from
    /// the shared startup settings. Replaces the former second argv scan
    /// for `--gpu-validation`; there is exactly one source of CLI semantics.
    struct SmokeOptions {
        bool m_gpu_validation{false};
    };

    // ------------------------------------------------------------------
    // smoke api
    // ------------------------------------------------------------------

    /// Run one headless, offscreen colony frame and verify a generated cell by GPU readback.
    [[nodiscard]] std::error_code runColonySmoke(const SmokeOptions &options);

    /// Blocking offscreen renderer diagnostic; never an interactive publication path.
    [[nodiscard]] std::error_code captureWorldGpuCloseups(
        std::unique_ptr<ColonyDriver> driver, double spawn_x_m, double floor_y_m, std::string identity,
        const std::filesystem::path &directory);
}
