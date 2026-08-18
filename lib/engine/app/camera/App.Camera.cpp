module;

#include "pP/Macros.h"

module engine.app;

import :camera;
import engine.core;
import engine.math;
import engine.rhi;
import std;

namespace pP {
    namespace {
        [[nodiscard]] constexpr float dtSeconds(TimeSpan dt) noexcept {
            return std::chrono::duration<float>(dt).count();
        }

        [[nodiscard]] constexpr float3 forwardFromAngles(float yaw, float pitch) noexcept {
            return float3{
                std::cos(pitch) * std::sin(yaw),
                std::sin(pitch),
                -std::cos(pitch) * std::cos(yaw)};
        }
    }

    void Camera::setView(const float4x4 &view) noexcept {
        m_view = view;
        ++m_camera_version;
    }

    void Camera::setProjection(const float4x4 &projection) noexcept {
        m_projection = projection;
        m_view_projection = m_view * m_projection;
        m_inverse_view_projection = pP::inverse(m_view_projection);
        ++m_camera_version;
    }

    void Camera::recomputeViewProjection() noexcept {
        m_view_projection = m_view * m_projection;
        m_inverse_view_projection = pP::inverse(m_view_projection);
    }

    void Camera::setViewportSize(const int2 &size) noexcept {
        m_viewport_size = float2{static_cast<float>(size.x), static_cast<float>(size.y)};
        ++m_camera_version;
    }

    void Camera::setPosition(const float3 &position) noexcept {
        m_position = position;
        ++m_camera_version;
    }

    void Camera::update(TimeSpan dt) noexcept {
        const float dt_s = dtSeconds(dt);
        m_velocity = dt_s > 0.0f ? (m_position - m_prev_position) * (1.0f / dt_s) : float3{zero_v};
        m_prev_position = m_position;
    }

    const float4x4 &Camera::view() const noexcept { return m_view; }
    const float4x4 &Camera::projection() const noexcept { return m_projection; }
    const float4x4 &Camera::viewProjection() const noexcept { return m_view_projection; }
    const float4x4 &Camera::inverseViewProjection() const noexcept { return m_inverse_view_projection; }
    const float3 &Camera::position() const noexcept { return m_position; }
    float3 Camera::velocity() const noexcept { return m_velocity; }
    const float2 &Camera::viewportSize() const noexcept { return m_viewport_size; }
    u64 Camera::cameraVersion() const noexcept { return m_camera_version; }

    void FreeCameraController::activate(IInputService &input, Camera &camera) {
        m_input = safe_ptr<IInputService>{&input};
        m_camera = &camera;
        m_listener.setRawKeyCallback([this](const InputMessage &msg) noexcept { onMessage_(msg); });
        input.pushInputListener(safe_ptr<InputListener>{&m_listener});
    }

    void FreeCameraController::deactivate() noexcept {
        if (m_input) {
            m_input->popInputListener(m_listener);
            m_input = nullptr;
        }
        m_camera = nullptr;
    }

    void FreeCameraController::setDeviceType(rhi::DeviceType device_type) noexcept {
        m_device_type = device_type;
    }

    void FreeCameraController::onMessage_(const InputMessage &msg) noexcept {
        if (msg.m_device_id == InputDeviceID{0u}) {
            if (msg.m_value.index() == 0u) {
                const bool down = std::get<InputDigital>(msg.m_value) == InputDigital{true};
                const auto *key = std::get_if<EKeyboardKey>(&msg.m_key.m_code);
                if (!key) return;
                if (*key == EKeyboardKey::w) m_forward = down;
                else if (*key == EKeyboardKey::s) m_back = down;
                else if (*key == EKeyboardKey::a) m_left = down;
                else if (*key == EKeyboardKey::d) m_right = down;
                else if (*key == EKeyboardKey::e) m_up_key = down;
                else if (*key == EKeyboardKey::q) m_down_key = down;
            }
        } else if (msg.m_device_id == InputDeviceID{1u}) {
            if (msg.isAxis() && msg.m_value.index() == 2u) {
                m_look_delta += std::get<InputAxis2D>(msg.m_value).m_relative;
            }
        }
    }

    void FreeCameraController::update(TimeSpan dt) noexcept {
        if (m_camera == nullptr) return;
        const float dts = dtSeconds(dt);
        m_yaw += m_look_delta.x * 0.01f;
        m_pitch = pP::clamp(m_pitch + m_look_delta.y * 0.01f, -1.5f, 1.5f);
        m_look_delta = float2{zero_v};

        const float3 forward = forwardFromAngles(m_yaw, m_pitch);
        const float3 right = normalize(pP::cross(forward, m_up));

        float3 move{0.0f, 0.0f, 0.0f};
        if (m_forward) move += forward;
        if (m_back) move -= forward;
        if (m_right) move += right;
        if (m_left) move -= right;
        if (m_up_key) move += m_up;
        if (m_down_key) move -= m_up;
        if (dot2(move) > 0.0f) move = normalize(move);

        m_eye += move * m_move_speed * dts;
        const float4x4 view = pP::lookAt(m_eye, m_eye + forward, m_up);
        m_camera->setView(view);
        m_camera->setPosition(m_eye);
        m_camera->recomputeViewProjection();
    }

    void PanCameraController::activate(IInputService &input, Camera &camera) {
        m_input = safe_ptr<IInputService>{&input};
        m_camera = &camera;
        m_listener.setRawKeyCallback([this](const InputMessage &msg) noexcept { onMessage_(msg); });
        input.pushInputListener(safe_ptr<InputListener>{&m_listener});
        m_camera->setProjection(rhi::getOrthoMatrix(m_device_type, 10.0f * m_zoom, 10.0f * m_zoom));
    }

    void PanCameraController::deactivate() noexcept {
        if (m_input) {
            m_input->popInputListener(m_listener);
            m_input = nullptr;
        }
        m_camera = nullptr;
    }

    void PanCameraController::setDeviceType(rhi::DeviceType device_type) noexcept {
        m_device_type = device_type;
        if (m_camera) {
            m_camera->setProjection(rhi::getOrthoMatrix(m_device_type, 10.0f * m_zoom, 10.0f * m_zoom));
        }
    }

    void PanCameraController::onMessage_(const InputMessage &msg) noexcept {
        if (msg.m_device_id == InputDeviceID{0u}) {
            if (msg.m_value.index() == 0u) {
                const bool down = std::get<InputDigital>(msg.m_value) == InputDigital{true};
                const auto *key = std::get_if<EKeyboardKey>(&msg.m_key.m_code);
                if (!key) return;
                if (*key == EKeyboardKey::w) m_forward = down;
                else if (*key == EKeyboardKey::s) m_back = down;
                else if (*key == EKeyboardKey::a) m_left = down;
                else if (*key == EKeyboardKey::d) m_right = down;
            }
        } else if (msg.m_device_id == InputDeviceID{1u}) {
            if (msg.isAxis()) {
                if (msg.m_value.index() == 2u) {
                    m_look_delta += std::get<InputAxis2D>(msg.m_value).m_relative;
                } else if (msg.m_value.index() == 1u) {
                    const float wheel = std::get<InputAxis1D>(msg.m_value).m_relative;
                    m_zoom = pP::clamp(m_zoom * (1.0f + wheel * 0.1f), 0.1f, 10.0f);
                }
            }
        }
    }

    void PanCameraController::update(TimeSpan dt) noexcept {
        if (m_camera == nullptr) return;
        const float dts = dtSeconds(dt);
        m_yaw += m_look_delta.x * 0.01f;
        m_pitch = pP::clamp(m_pitch + m_look_delta.y * 0.01f, -1.5f, 1.5f);
        m_look_delta = float2{zero_v};

        const float3 forward = forwardFromAngles(m_yaw, m_pitch);
        const float3 right = normalize(pP::cross(forward, m_up));

        float3 move{0.0f, 0.0f, 0.0f};
        if (m_forward) move += forward;
        if (m_back) move -= forward;
        if (m_right) move += right;
        if (m_left) move -= right;
        if (dot2(move) > 0.0f) move = normalize(move);

        m_eye += move * m_pan_speed * dts;
        const float4x4 view = pP::lookAt(m_eye, m_eye + forward, m_up);
        m_camera->setView(view);
        m_camera->setPosition(m_eye);
        m_camera->setProjection(rhi::getOrthoMatrix(m_device_type, 10.0f * m_zoom, 10.0f * m_zoom));
    }

    CameraService::CameraService() noexcept = default;

    void CameraService::deactivateController() noexcept {
        if (m_controller) m_controller->deactivate();
    }

    CameraService::~CameraService() noexcept {
        deactivateController();
    }

    void CameraService::setDeviceType(rhi::DeviceType type) noexcept {
        m_device_type = type;
        const float2 &size = m_camera.viewportSize();
        const float aspect = size.y != 0.0f ? size.x / size.y : 1.0f;
        setPerspectiveProjection(m_fov, aspect, m_near, m_far);
        if (m_controller) {
            m_controller->setDeviceType(type);
        }
    }

    void CameraService::setViewportSize(const int2 &size) noexcept {
        m_camera.setViewportSize(size);
        const float aspect = size.y != 0 ? static_cast<float>(size.x) / static_cast<float>(size.y) : 1.0f;
        setPerspectiveProjection(m_fov, aspect, m_near, m_far);
    }

    std::error_code CameraService::initialize(IInputService &input) {
        m_input = safe_ptr<IInputService>{&input};
        m_fov = 60.0f * std::numbers::pi_v<float> / 180.0f;
        m_near = 0.1f;
        m_far = 1000.0f;
        setPerspectiveProjection(m_fov, 16.0f / 9.0f, m_near, m_far);
        auto free_cam = std::make_unique<FreeCameraController>();
        setController(std::move(free_cam));
        return default_value_v;
    }

    void CameraService::setController(std::unique_ptr<ICameraController> &&controller) {
        if (m_controller) m_controller->deactivate();
        m_controller = std::move(controller);
        if (m_controller && m_input) {
            m_controller->activate(*m_input, m_camera);
            m_controller->setDeviceType(m_device_type);
        }
    }

    Camera &CameraService::camera() noexcept { return m_camera; }

    void CameraService::update(TimeSpan dt) noexcept {
        if (m_controller) m_controller->update(dt);
        m_camera.update(dt);
    }

    void CameraService::setPerspectiveProjection(float fov, float aspect, float near_, float far_) noexcept {
        m_camera.setProjection(rhi::getPerspectiveMatrix(m_device_type, fov, aspect, near_, far_));
    }

    void CameraService::setOrthoProjection(float width, float height) noexcept {
        m_camera.setProjection(rhi::getOrthoMatrix(m_device_type, width, height));
    }
}
