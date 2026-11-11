module;
#include "pP/Macros.h"
module engine.app;

import :renderer.presentation;

import engine.core;
import engine.math;
import engine.rhi;
import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(Presentation, debug, none)

    namespace {
        constexpr u32 kMinPixelScale{1u};
        constexpr u32 kMaxPixelScale{8u};
    }

    // ------------------------------------------------------------------
    // resolution and presentation values
    // ------------------------------------------------------------------

    Expected<PresentationGrid> makePresentationGrid(const int2 framebuffer, const u32 pixel_scale) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Presentation, pixel_scale >= kMinPixelScale and pixel_scale <= kMaxPixelScale
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Presentation, framebuffer.x > 0 and framebuffer.y > 0 ? std::error_code{}
                                                                  : std::make_error_code(std::errc::invalid_argument));

        // Each axis is independent so one odd axis never stretches the other.
        // The intermediate arithmetic is wide enough that only the resolve
        // backing can overflow an i32 for an extreme window.
        const auto axis = [pixel_scale](const i32 window, i32 &resolved, i32 &raster, i32 &backing) {
            const i64 w = window;
            const i64 p = pixel_scale;
            const i64 r = (w + p - 1) / p;
            const i64 s = (r + 1) / 2;
            const i64 b = 2 * s;
            PPR_RETURN_ERROR_ON_FAIL(
                Presentation, b <= std::numeric_limits<i32>::max() ? std::error_code{}
                                                                   : std::make_error_code(std::errc::value_too_large));
            resolved = static_cast<i32>(r);
            raster = static_cast<i32>(s);
            backing = static_cast<i32>(b);
            return std::error_code{};
        };

        PresentationGrid grid{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Presentation, axis(framebuffer.x, grid.m_resolved.x, grid.m_raster.x, grid.m_backing.x));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Presentation, axis(framebuffer.y, grid.m_resolved.y, grid.m_raster.y, grid.m_backing.y));
        grid.m_window = framebuffer;
        grid.m_pixel_scale = pixel_scale;
        return grid;
    }

    // ------------------------------------------------------------------
    // post settings
    // ------------------------------------------------------------------

    std::error_code validatePostProcessSettings(const PostProcessSettings &settings) {
        PPR_RETURN_ERROR_ON_FAIL(
            Presentation, std::isfinite(settings.m_exposure_ev) and
                                  settings.m_exposure_ev >= -16.0f and
                                  settings.m_exposure_ev <= 16.0f and
                                  std::isfinite(settings.m_adaptation_tau) and
                                  std::isfinite(settings.m_ev_min) and
                                  std::isfinite(settings.m_ev_max) and
                                  std::isfinite(settings.m_bloom_intensity) and
                                  std::isfinite(settings.m_bloom_threshold)
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));

        PPR_RETURN_ERROR_ON_FAIL(
            Presentation, settings.m_adaptation_tau > 0.0f and settings.m_ev_min <= settings.m_ev_max
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            Presentation, settings.m_bloom_intensity >= 0.0f and settings.m_bloom_threshold >= 0.0f
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));

        return {};
    }
}
