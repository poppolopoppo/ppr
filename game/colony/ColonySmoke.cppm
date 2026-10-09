export module game.colony.smoke;

import engine.core;
import std;

export namespace pP::colony {
    /// Typed smoke options, resolved at the game invocation boundary from
    /// the shared startup settings. Replaces the former second argv scan
    /// for `--gpu-validation`; there is exactly one source of CLI semantics.
    struct SmokeOptions {
        bool m_gpu_validation{false};
    };

    /// Run one headless, offscreen colony frame and verify a generated cell by GPU readback.
    [[nodiscard]] std::error_code runColonySmoke(const SmokeOptions &options);
    /// Slice 6 gate: 100-agent sustained driver + translator.submit +
    /// GridPass-upload load (600 fixed-1/60 ticked frames, stderr
    /// diagnostics, driver/translator timing CSVs). Default `--smoke`
    /// behavior is unchanged: this runs only under `--load100`.
    [[nodiscard]] Expected<void> runLoad100();
}
