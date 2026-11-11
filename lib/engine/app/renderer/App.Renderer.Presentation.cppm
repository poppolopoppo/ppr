export module engine.app:renderer.presentation;

import :renderer.types;

import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // resolution and presentation values
    // ------------------------------------------------------------------
    // The window extent W, integer pixelization P and resolved content R are
    // independent of the multisampled reconstruction extent. R is the only
    // valid post-processing/history/statistics region; B may carry padding.

    struct PresentationGrid {
        int2 m_window{}, m_resolved{}, m_raster{}, m_backing{};
        u32 m_pixel_scale{1u};
    };

    /// Checked W/P/R/S/B arithmetic for a positive framebuffer. The caller
    /// suspends scene work for a zero framebuffer instead of fabricating an
    /// extent. A zero window extent or a pixel scale outside 1..8 is rejected.
    [[nodiscard]] Expected<PresentationGrid> makePresentationGrid(int2 framebuffer, u32 pixel_scale);

    // ------------------------------------------------------------------
    // resolved scene binding and post settings
    // ------------------------------------------------------------------

    /// Immutable read binding to a resolved scene image. Strong ownership keeps
    /// the allocation alive; it does not alone exclude another GPU writer. The
    /// producer and the consuming submission order the backing through its last
    /// read. `m_valid` names the content region inside `m_backing`.
    struct ResolvedHdrView {
        rhi::ComPtr<rhi::ITextureView> m_view;
        GridWorldIdentity m_world;
        int2 m_backing{zero_v}, m_valid{zero_v};
    };

    struct PostProcessSettings {
        float m_exposure_ev{0.0f}, m_adaptation_tau{0.7f};
        float m_ev_min{-4.0f}, m_ev_max{4.0f};
        float m_bloom_intensity{0.125f}, m_bloom_threshold{1.0f};
        bool m_auto_exposure{true}, m_bloom_enabled{true};
    };

    /// Finite fields, a positive adaptation time constant, an ordered EV clamp
    /// and non-negative bloom controls. These are physical pass preconditions;
    /// the application owns its narrower user-control domains.
    [[nodiscard]] std::error_code validatePostProcessSettings(const PostProcessSettings &settings);
}
