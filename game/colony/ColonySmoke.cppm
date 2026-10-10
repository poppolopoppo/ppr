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
}
