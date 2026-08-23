module;

#include "pP/Macros.h"

export module engine.app:viewport.client;

import engine.core;
import engine.math;
import engine.rhi;
import std;

import :viewport.camera;
import :viewport;

export namespace pP {
    // ------------------------------------------------------------------
    // ViewportClient — binds a camera and viewport to a controller
    // ------------------------------------------------------------------

    class ViewportClient {
    public:
        ViewportClient() noexcept = default;
        ViewportClient(Camera &camera, const int2 &clientRect, EViewportFlags flags = EViewportFlags::None) noexcept;

        [[nodiscard]] Camera &camera() noexcept {
            PPR_ASSERT(m_camera != nullptr);
            return *m_camera;
        }
        [[nodiscard]] const Camera &camera() const noexcept {
            PPR_ASSERT(m_camera != nullptr);
            return *m_camera;
        }
        [[nodiscard]] Viewport &viewport() noexcept { return m_viewport; }
        [[nodiscard]] const Viewport &viewport() const noexcept { return m_viewport; }

        [[nodiscard]] const int2 &clientRect() const noexcept { return m_viewport.clientRect(); }
        void setClientRect(const int2 &value) noexcept;

        void update(TimeSpan dt, ICameraController &controller, const std::optional<int2> &clientRect = std::nullopt) noexcept;

    private:
        safe_ptr<Camera> m_camera{};
        Viewport m_viewport{};
    };
}
