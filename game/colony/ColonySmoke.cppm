export module game.colony.smoke;

import engine.core;
import engine.rhi;
import game.colony.driver;
import game.colony.content;
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

    /// Handcrafted bounded reference scene through real RC receivers, GridPass
    /// and spatial/HDR display. Uses the selected catalog's prepared appearance;
    /// does not generate or admit a full simulation world. Blocking diagnostic.
    /// Failure may leave partial diagnostic files for inspection.
    /// qualified_fp64 is an explicit caller assertion of current-device/session
    /// RTE64 qualification, not discovery; false preserves strict FP32.
    [[nodiscard]] std::error_code captureLightingScene(
        std::shared_ptr<const ContentPresentation> presentation, std::string profile,
        const std::filesystem::path &directory, bool qualified_fp64 = false);
}

// ------------------------------------------------------------------
// shared implementation-only capture readback
// ------------------------------------------------------------------

namespace pP::colony {
    [[nodiscard]] std::error_code writeCaptureReadback(
        rhi::IDevice &device, rhi::ITexture &target, const std::filesystem::path &path);
}
