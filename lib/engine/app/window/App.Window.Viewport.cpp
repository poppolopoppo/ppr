module;
#include "pP/Macros.h"
module engine.app;

import :window.viewport;
import :window.handle;

import engine.core;
import engine.math;
import std;

namespace pP {
    // ------------------------------------------------------------------
    // simple rectangle struct to hold both integral and floating point data
    // ------------------------------------------------------------------

    template<math::details::TArithmetic T>
    float BasicRect<T>::getAspectRatio() const noexcept {
        PPR_ASSERT(m_extent.y > epsilon_v<T>);
        return static_cast<float>(m_extent.x) / static_cast<float>(m_extent.y);
    }

    template struct BasicRect<int>;
    template struct BasicRect<float>;

    // ------------------------------------------------------------------
    // Viewport layout -> policy when parent window is resized
    // ------------------------------------------------------------------

    PixelRect ViewportLayout::clientRect(const PixelRect &window_rect) const noexcept {
        return std::visit(overloaded(
            [&](FullWindow) noexcept -> PixelRect {
                return window_rect;
            },
            [&](const Centered &centered) noexcept -> PixelRect {
                // NOTE: component-wise; mango int-vector / and - don't cover
                // generic vectors on this toolchain (same as operator==).
                const int2 half{centered.m_extent.x / 2, centered.m_extent.y / 2};
                const int2 center{
                    window_rect.m_origin.x + window_rect.m_extent.x / 2,
                    window_rect.m_origin.y + window_rect.m_extent.y / 2,
                };
                return {
                    int2{center.x - half.x, center.y - half.y},
                    centered.m_extent
                };
            },
            [&](const WindowRect &client_rect) noexcept -> PixelRect {
                return client_rect;
            },
            [&](const NormalizedWindowRect &normalized_rect) noexcept -> PixelRect {
                const float2 window_origin{
                    static_cast<float>(window_rect.m_origin.x) + 0.5f,
                    static_cast<float>(window_rect.m_origin.y) + 0.5f
                };
                const float2 window_extent{
                    static_cast<float>(window_rect.m_extent.x),
                    static_cast<float>(window_rect.m_extent.y)
                };
                return {
                    int2{
                        static_cast<int>(std::round(window_origin.x + window_extent.x * normalized_rect.m_origin.x)),
                        static_cast<int>(std::round(window_origin.y + window_extent.y * normalized_rect.m_origin.y))
                    },
                    int2{
                        static_cast<int>(std::round(window_extent.x * normalized_rect.m_extent.x)),
                        static_cast<int>(std::round(window_extent.y * normalized_rect.m_extent.y))
                    }
                };
            }
        ), m_variant);
    }

    // ------------------------------------------------------------------
    // Viewport with client-rect coordinate transforms and window attachment
    // ------------------------------------------------------------------

    // Window size here, not framebuffer size: DPI scaling and framebuffer-space
    // translation belong to P2 makeRenderView, not to geometry.
    Viewport::Viewport(const Window &window, const ViewportLayout &layout) noexcept
        : Viewport(PixelRect{window.m_window_position, window.m_window_size}, layout) {
    }

    Viewport::Viewport(const PixelRect &window_rect, const ViewportLayout &layout) noexcept
        : m_window_rect(window_rect),
          m_client_rect(layout.clientRect(window_rect)) {
        if (m_window_rect.m_extent.x <= 0 or m_window_rect.m_extent.y <= 0) [[unlikely]] {
            m_client_rect = PixelRect{m_window_rect.m_origin, int2{zero_v}};
        } else {
            if (m_client_rect.m_extent.x < 0) [[unlikely]] {
                m_client_rect.setWidth(0);
            }
            if (m_client_rect.m_extent.y < 0) [[unlikely]] {
                m_client_rect.setHeight(0);
            }
        }
    }

    NormalizedRect Viewport::getNormalizedClientRect() const noexcept {
        if (m_window_rect.m_extent.x <= 0 or m_window_rect.m_extent.y <= 0) [[unlikely]] {
            return NormalizedRect{};
        }
        // Pixel-center convention, mirrored with clientRect(): the forward map
        // lerps from window_origin + 0.5f over raw extents, so the inverse
        // subtracts the same biased origin and divides by the same raw extents.
        const float2 window_origin{
            static_cast<float>(m_window_rect.m_origin.x) + 0.5f,
            static_cast<float>(m_window_rect.m_origin.y) + 0.5f
        };
        const float2 window_extent{
            static_cast<float>(m_window_rect.m_extent.x),
            static_cast<float>(m_window_rect.m_extent.y)
        };
        const float2 client_origin{
            static_cast<float>(m_client_rect.m_origin.x),
            static_cast<float>(m_client_rect.m_origin.y)
        };
        const float2 client_extent{
            static_cast<float>(m_client_rect.m_extent.x),
            static_cast<float>(m_client_rect.m_extent.y)
        };
        return {
            (client_origin - window_origin) / window_extent,
            client_extent / window_extent
        };
    }

    int2 Viewport::screenToWindow(const int2 &screen_pos) const noexcept {
        return screen_pos - m_window_rect.m_origin;
    }

    int2 Viewport::windowToClient(const int2 &window_pos) const noexcept {
        // Client rect is stored screen-space; rebase to window-local first.
        return window_pos - (getClientRect().m_origin - getWindowRect().m_origin);
    }

    int2 Viewport::clientToWindow(const int2 &client_pos) const noexcept {
        return client_pos + (getClientRect().m_origin - getWindowRect().m_origin);
    }

    int2 Viewport::windowToScreen(const int2 &window_pos) const noexcept {
        return window_pos + m_window_rect.m_origin;
    }

    float2 Viewport::normalizeClient(const int2 &client_pos) const noexcept {
        return getClientRect().normalizeClamp(client_pos);
    }

    int2 Viewport::denormalizeClient(const float2 &client_uv) const noexcept {
        return getClientRect().denormalizeClamp(client_uv);
    }

    float2 Viewport::normalizeWindow(const int2 &window_pos) const noexcept {
        return getWindowRect().normalizeClamp(window_pos);
    }

    int2 Viewport::denormalizeWindow(const float2 &window_uv) const noexcept {
        return getWindowRect().denormalizeClamp(window_uv);
    }

    // ------------------------------------------------------------------
    // WindowViewport -> viewport client associated to a window, handles updates
    // ------------------------------------------------------------------

    WindowViewport::WindowViewport(SharedWindow window, ViewportLayout layout) noexcept
        : m_window(std::move(window))
          , m_viewport(PixelRect{})
          , m_layout(std::move(layout)) {
        PPR_ASSERT(m_window.isValid());

        updateFromWindow();
    }

    void WindowViewport::setLayout(ViewportLayout layout) noexcept {
        m_layout = std::move(layout);
        updateFromWindow();
    }

    bool WindowViewport::updateFromWindow() noexcept {
        const Viewport old_viewport = m_viewport;
        m_viewport = Viewport{*m_window, m_layout};

        if (old_viewport != m_viewport) [[unlikely]] {
            ++m_viewport_revision;
            return true;
        }
        return false;
    }
}
