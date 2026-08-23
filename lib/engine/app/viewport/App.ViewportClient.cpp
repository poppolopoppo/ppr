module;
#include "pP/Macros.h"
module engine.app;
import :viewport.client;
import :viewport.camera;
import :viewport;
import engine.core;
import engine.math;
import engine.rhi;
import std;

namespace pP {
    ViewportClient::ViewportClient(Camera &camera, const int2 &clientRect, EViewportFlags flags) noexcept
        : m_camera(&camera), m_viewport(clientRect, flags) {}

    void ViewportClient::setClientRect(const int2 &value) noexcept {
        m_viewport.setClientRect(value);
    }

    void ViewportClient::update(TimeSpan dt, ICameraController &controller, const std::optional<int2> &clientRect) noexcept {
        PPR_ASSERT(m_camera != nullptr);
        if (clientRect) {
            m_viewport.setClientRect(*clientRect);
        }
        CameraModel model = m_camera->currentState().model;
        controller.updateCamera(dt, model);
        m_camera->updateModel(model, m_viewport.clientRect());
    }
}
