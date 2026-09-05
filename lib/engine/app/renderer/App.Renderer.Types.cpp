module;
module engine.app;

import :renderer.types;
import :window.viewport;

import engine.core;
import engine.math;
import engine.rhi;
import std;

namespace pP {
    std::optional<RenderView> makeRenderView(const Viewport &viewport, const int2 &target_extent) noexcept {
        const PixelRect &window_rect = viewport.getWindowRect();
        const PixelRect &client_rect = viewport.getClientRect();

        if (window_rect.m_extent.x <= 0 || window_rect.m_extent.y <= 0) [[unlikely]] {
            return std::nullopt;
        }
        if (client_rect.m_extent.x <= 0 || client_rect.m_extent.y <= 0) [[unlikely]] {
            return std::nullopt;
        }
        if (target_extent.x <= 0 || target_extent.y <= 0) [[unlikely]] {
            return std::nullopt;
        }

        // Absolute client rect is screen-space: translate to window-local without
        // the normalizeClient family (frame-mixed for nonzero window origins).
        const float2 local_origin = vector_cast<float>(client_rect.m_origin - window_rect.m_origin);
        const float2 client_extent = vector_cast<float>(client_rect.m_extent);

        // DPI scale: window units to framebuffer (target) units.
        const float2 scale = vector_cast<float>(target_extent) / vector_cast<float>(window_rect.m_extent);

        const float2 scaled_min = local_origin * scale;
        const float2 scaled_extent = client_extent * scale;
        const float2 target = vector_cast<float>(target_extent);

        const float2 clipped_min{std::max(scaled_min.x, 0.0f), std::max(scaled_min.y, 0.0f)};
        const float2 clipped_max{
            std::min(scaled_min.x + scaled_extent.x, target.x),
            std::min(scaled_min.y + scaled_extent.y, target.y),
        };
        if (clipped_min.x >= clipped_max.x || clipped_min.y >= clipped_max.y) [[unlikely]] {
            return std::nullopt;
        }

        const int2 pixel_min = floorToInt(clipped_min);
        const int2 pixel_max = ceilToInt(clipped_max);

        RenderView view{};
        view.m_viewport = rhi::Viewport{
            .originX = static_cast<float>(pixel_min.x),
            .originY = static_cast<float>(pixel_min.y),
            .extentX = static_cast<float>(pixel_max.x - pixel_min.x),
            .extentY = static_cast<float>(pixel_max.y - pixel_min.y),
            .minZ = 0.0f,
            .maxZ = 1.0f,
        };
        view.m_scissor = rhi::ScissorRect{
            .minX = static_cast<u32>(pixel_min.x),
            .minY = static_cast<u32>(pixel_min.y),
            .maxX = static_cast<u32>(pixel_max.x),
            .maxY = static_cast<u32>(pixel_max.y),
        };
        return view;
    }
}
