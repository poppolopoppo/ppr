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
                -std::cos(pitch) * std::cos(yaw)
            };
        }

        // Per-key modulate modifiers. These deliberately do NOT bake dt (unlike the
        // modulate(floatN) overloads): events carry pure direction/delta and dt is
        // applied exactly once in update(). Static storage keeps the function_ref
        // captured by the returned InputModifierEvent valid for the controller's lifetime.
        constexpr auto kMoveForward = [](TimeSpan) noexcept { return float3{0.0f, 0.0f, 1.0f}; };
        constexpr auto kMoveBack = [](TimeSpan) noexcept { return float3{0.0f, 0.0f, -1.0f}; };
        constexpr auto kMoveLeft = [](TimeSpan) noexcept { return float3{-1.0f, 0.0f, 0.0f}; };
        constexpr auto kMoveRight = [](TimeSpan) noexcept { return float3{1.0f, 0.0f, 0.0f}; };
        constexpr auto kMoveUp = [](TimeSpan) noexcept { return float3{0.0f, 1.0f, 0.0f}; };
        constexpr auto kMoveDown = [](TimeSpan) noexcept { return float3{0.0f, -1.0f, 0.0f}; };
        constexpr auto kSpeedUp = [](TimeSpan) noexcept { return 0.001f; };
        constexpr auto kSpeedDown = [](TimeSpan) noexcept { return -0.001f; };
        constexpr auto kSpeedFast = [](TimeSpan) noexcept { return 0.01f; };
        constexpr auto kSpeedSlow = [](TimeSpan) noexcept { return -0.01f; };
        constexpr auto kFovIn = [](TimeSpan) noexcept { return 0.01f; };

        constexpr auto kFovInSmall = [](TimeSpan) noexcept { return 0.001f; };
        constexpr auto kFovOutSmall = [](TimeSpan) noexcept { return -0.001f; };
    }

    void Camera::setView(const float4x4 &view) noexcept {
        m_current.view = view;
        recomputeViewProjection();
        ++m_camera_version;
    }

    void Camera::setProjection(const float4x4 &projection) noexcept {
        m_current.projection = projection;
        recomputeViewProjection();
        ++m_camera_version;
    }

    void Camera::recomputeViewProjection() noexcept {
        m_current.viewProjection = m_current.view * m_current.projection;
        m_current.inverseViewProjection = inverse(m_current.viewProjection);
    }

    void Camera::updateModel(const CameraModel &model) noexcept {
        m_current.position = model.position;
        const float3 forward = forwardFromAngles(model.yaw, model.pitch);
        const float3 up{0.0f, 1.0f, 0.0f};
        m_current.view = lookAt(model.position, model.position + forward, up);
        recomputeViewProjection();
        m_pending_cut = model.cameraCut;
        ++m_camera_version;
    }

    void Camera::setViewportSize(const int2 &size) noexcept {
        m_current.viewportSize = float2{static_cast<float>(size.x), static_cast<float>(size.y)};
        ++m_camera_version;
    }

    void Camera::setPosition(const float3 &position) noexcept {
        m_current.position = position;
        ++m_camera_version;
    }

    // ------------------------------------------------------------------
    // Frustum
    // ------------------------------------------------------------------

    Frustum::Frustum() noexcept {
        std::fill_n(m_planes, 6, float4{0.0f, 0.0f, 0.0f, 0.0f});
        std::fill_n(m_corners, 8, float3{zero_v});
        m_bbox_min = float3{zero_v};
        m_bbox_max = float3{zero_v};
    }

    Frustum::Frustum(const float4x4 &viewProjection, rhi::EProjectionConvention convention) noexcept {
        std::fill_n(m_planes, 6, float4{0.0f, 0.0f, 0.0f, 0.0f});
        std::fill_n(m_corners, 8, float3{zero_v});
        m_bbox_min = float3{zero_v};
        m_bbox_max = float3{zero_v};
        setMatrix(viewProjection, convention);
    }

    void Frustum::setMatrix(const float4x4 &viewProjection, rhi::EProjectionConvention convention) noexcept {
        // Single-arg overload computes inverse internally. Callers that already hold
        // the inverse (or have a singular VP) should use the two-arg overload to avoid
        // a redundant inverse and to prevent NaN propagation from a singular matrix.
        setMatrix(viewProjection, inverse(viewProjection), convention);
    }

    void Frustum::setMatrix(const float4x4 &viewProjection, const float4x4 &inverseViewProjection, rhi::EProjectionConvention convention) noexcept {
        m_convention = convention;
        // Gribb-Hartmann plane extraction from view-projection matrix.
        // Each plane: row3 ± rowN, normalized.
        const float4 row0 = viewProjection[0];
        const float4 row1 = viewProjection[1];
        const float4 row2 = viewProjection[2];
        const float4 row3 = viewProjection[3];

        auto normalizePlane = [](float4 p) noexcept {
            const float len_sq = p.x * p.x + p.y * p.y + p.z * p.z;
            if (len_sq < 1e-12f) {
                return float4{0.0f, 0.0f, 0.0f, std::numeric_limits<float>::max()};
            }
            const float len = std::sqrt(len_sq);
            p.x /= len;
            p.y /= len;
            p.z /= len;
            p.w /= len;
            return p;
        };

        m_planes[static_cast<u8>(EFrustumPlane::Left)] = normalizePlane(row3 + row0);
        m_planes[static_cast<u8>(EFrustumPlane::Right)] = normalizePlane(row3 - row0);
        m_planes[static_cast<u8>(EFrustumPlane::Bottom)] = normalizePlane(row3 + row1);
        m_planes[static_cast<u8>(EFrustumPlane::Top)] = normalizePlane(row3 - row1);
        m_planes[static_cast<u8>(EFrustumPlane::Near)] = normalizePlane(row3 + row2);
        m_planes[static_cast<u8>(EFrustumPlane::Far)] = normalizePlane(row3 - row2);

        // Compute 8 corners by intersecting plane combinations.
        // For brevity, use a simplified approach: compute corners from the
        // view-projection matrix by transforming NDC cube corners.
        const float z_near = m_convention == rhi::EProjectionConvention::D3D ? 0.0f : -1.0f;
        const float z_far = 1.0f;
        const float3 ndc_corners[8] = {
            float3{-1.0f, -1.0f, z_near}, // NearLeftBottom
            float3{1.0f, -1.0f, z_near}, // NearRightBottom
            float3{1.0f, 1.0f, z_near}, // NearRightTop
            float3{-1.0f, 1.0f, z_near}, // NearLeftTop
            float3{-1.0f, -1.0f, z_far}, // FarLeftBottom
            float3{1.0f, -1.0f, z_far}, // FarRightBottom
            float3{1.0f, 1.0f, z_far}, // FarRightTop
            float3{-1.0f, 1.0f, z_far}, // FarLeftTop
        };

        const float4x4 &invVP = inverseViewProjection;
        m_bbox_min = float3{std::numeric_limits<float>::max()};
        m_bbox_max = float3{std::numeric_limits<float>::lowest()};

        std::size_t idx = 0;
        for (float3 &corner: m_corners) {
            const float4 clip = float4{ndc_corners[idx], 1.0f};
            const float4 world = clip * invVP;
            // Guard against non-finite invVP (singular/degenerate matrix): NaN fails
            // the != 0 comparison, so finiteness must be checked explicitly.
            const bool usable = std::isfinite(world.x) && std::isfinite(world.y)
                                && std::isfinite(world.z) && std::isfinite(world.w) && world.w != 0.0f;
            const float3 pos = usable
                                   ? float3{world.x, world.y, world.z} / world.w
                                   : float3{zero_v};
            corner = pos;
            if (usable) {
                m_bbox_min = float3{
                    std::min(m_bbox_min.x, pos.x),
                    std::min(m_bbox_min.y, pos.y),
                    std::min(m_bbox_min.z, pos.z)
                };
                m_bbox_max = float3{
                    std::max(m_bbox_max.x, pos.x),
                    std::max(m_bbox_max.y, pos.y),
                    std::max(m_bbox_max.z, pos.z)
                };
            }
            ++idx;
        }
    }

    EContainmentType Frustum::contains(float3 point) const noexcept {
        for (const float4 &p: m_planes) {
            const float dist = dot(float3{p.x, p.y, p.z}, point) + p.w;
            if (dist < 0.0f)
                return EContainmentType::Outside;
        }
        return EContainmentType::Inside;
    }

    bool Frustum::intersects(const float3 &box_min, const float3 &box_max) const noexcept {
        // AABB vs frustum: for each plane, find the "positive vertex" (the corner
        // of the AABB farthest in the direction of the plane normal). If that
        // vertex is outside the plane, the AABB is fully outside.do
        for (const float4 &p: m_planes) {
            const float3 normal{p.x, p.y, p.z};
            const float3 positive_vertex{
                normal.x > 0.0f ? box_max.x : box_min.x,
                normal.y > 0.0f ? box_max.y : box_min.y,
                normal.z > 0.0f ? box_max.z : box_min.z
            };
            const float dist = dot(normal, positive_vertex) + p.w;
            if (dist < 0.0f)
                return false;
        }
        return true;
    }

    void Camera::update(TimeSpan dt) noexcept {
        const float dt_s = dtSeconds(dt);
        if (!m_pending_cut) {
            m_velocity = dt_s > 0.0f ? (m_current.position - m_previous.position) * (1.0f / dt_s) : float3{zero_v};
            m_previous.position = m_current.position;
        } else {
            // Camera cut: zero velocity and re-baseline the previous position so the
            // teleport delta never leaks into the next frame's velocity.
            m_velocity = float3{zero_v};
            m_previous.position = m_current.position;
            m_pending_cut = false;
        }
    }

    const float4x4 &Camera::view() const noexcept { return m_current.view; }
    const float4x4 &Camera::projection() const noexcept { return m_current.projection; }
    const float4x4 &Camera::viewProjection() const noexcept { return m_current.viewProjection; }
    const float4x4 &Camera::inverseViewProjection() const noexcept { return m_current.inverseViewProjection; }
    const float3 &Camera::position() const noexcept { return m_current.position; }
    float3 Camera::velocity() const noexcept { return m_velocity; }
    const float2 &Camera::viewportSize() const noexcept { return m_current.viewportSize; }
    u64 Camera::cameraVersion() const noexcept { return m_camera_version; }

    float3 Camera::worldToClip(const float3& world) const noexcept {
        const float4 clip = float4{world, 1.0f} * m_current.viewProjection;
        const float w = clip.w != 0.0f ? clip.w : 1.0f;
        return float3{clip.x / w, clip.y / w, clip.z / w};
    }

    float3 Camera::clipToWorld(const float4 &clip) const noexcept {
        const float4 world = clip * m_current.inverseViewProjection;
        const float w = world.w != 0.0f ? world.w : 1.0f;
        return float3{world.x / w, world.y / w, world.z / w};
    }

    float3 Camera::clientToWorld(const float2& client, float z_ndc) const noexcept {
        const float2 size = m_current.viewportSize;
        PPR_ASSERT(size.x > 0.0f && size.y > 0.0f);
        const float w = size.x != 0.0f ? size.x : 1.0f;
        const float h = size.y != 0.0f ? size.y : 1.0f;
        const float ndc_x = (client.x / w) * 2.0f - 1.0f;
        const float ndc_y = (client.y / h) * 2.0f - 1.0f;
        return clipToWorld(float4{ndc_x, ndc_y, z_ndc, 1.0f});
    }

    float3 Camera::screenToWorld(const float2& screen, const int2& framebuffer_size, float z_ndc) const noexcept {
        PPR_ASSERT(framebuffer_size.x > 0 && framebuffer_size.y > 0);
        const float2 size = m_current.viewportSize;
        const float2 fb{static_cast<float>(framebuffer_size.x), static_cast<float>(framebuffer_size.y)};
        const float2 origin = (size - fb) * 0.5f;
        return clientToWorld(screen - origin, z_ndc);
    }

    float2 Camera::worldToScreen(const float3& world, const int2& framebuffer_size) const noexcept {
        PPR_ASSERT(framebuffer_size.x > 0 && framebuffer_size.y > 0);
        const float3 ndc = worldToClip(world);
        const float2 size = m_current.viewportSize;
        const float2 fb{static_cast<float>(framebuffer_size.x), static_cast<float>(framebuffer_size.y)};
        const float2 origin = (size - fb) * 0.5f;
        const float2 client{(ndc.x * 0.5f + 0.5f) * size.x, (ndc.y * 0.5f + 0.5f) * size.y};
        return client + origin;
    }

    std::pair<float3, float3> Camera::clientToWorldRay(const float2& client) const noexcept {
        const float3 near_point = clientToWorld(client, -1.0f);
        const float3 far_point = clientToWorld(client, 1.0f);
        const float3 direction = far_point - near_point;
        const float len = std::sqrt(dot(direction, direction));
        return {near_point, len > 0.0f ? direction * (1.0f / len) : float3{0.0f, 0.0f, 1.0f}};
    }

    // ------------------------------------------------------------------
    // FreeCameraController
    // ------------------------------------------------------------------

    FreeCameraController::FreeCameraController() noexcept
        : m_move_action{std::make_unique<InputAction>("CameraMove", EInputValueType::axis_3d, EInputActionFlags::none)},
          m_rotate_action{std::make_unique<InputAction>("CameraRotate", EInputValueType::axis_2d, EInputActionFlags::none)},
          m_speed_action{std::make_unique<InputAction>("CameraSpeed", EInputValueType::axis_1d, EInputActionFlags::none)},
          m_fov_action{std::make_unique<InputAction>("CameraFov", EInputValueType::axis_1d, EInputActionFlags::none)},
          m_look_action{std::make_unique<InputAction>("CameraLook", EInputValueType::digital, EInputActionFlags::none)} {
        m_move_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onMoveStarted_(event, key); });
        m_move_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onMoveCompleted_(event, key); });
        m_move_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept { onMoveTriggered_(event, key); });

        m_rotate_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onRotateStarted_(event, key); });
        m_rotate_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onRotateCompleted_(event, key); });
        m_rotate_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept { onRotateTriggered_(event, key); });

        m_speed_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onSpeedStarted_(event, key); });
        m_speed_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onSpeedCompleted_(event, key); });

        m_fov_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onFovAccumulate_(event, key); });
        m_fov_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept { onFovAccumulate_(event, key); });

        m_look_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onLookStarted_(event, key); });
        m_look_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onLookCompleted_(event, key); });
    }

    void FreeCameraController::activate(IInputService &, Camera &camera) {
        m_camera = &camera;
    }

    void FreeCameraController::deactivate() noexcept {
        m_camera = nullptr;
        m_move_sum = float3{zero_v};

        m_rotate_sum = float2{zero_v};
        m_rotate_accum = float2{zero_v};
        m_gamepad_move_last = float3{zero_v};
        m_gamepad_rotate_last = float2{zero_v};
        m_speed_sum = 0.0f;
        m_fov_accum = 0.0f;
        m_move_contrib.clear();
        m_rotate_contrib.clear();
        m_speed_contrib.clear();
        m_fov_contrib.clear();
    }

    void FreeCameraController::setDeviceType(rhi::DeviceType device_type) noexcept {
        m_device_type = device_type;
    }

    void FreeCameraController::provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept {
        const auto bind = [&](const InputKey &key, const InputAction &action, InputModifierEvent modifier) {
            InputActionKeyMapping &mapping = out_mapping.mapKey(SharedInputAction{&action}, key);
            mapping.m_modifier = std::move(modifier);
        };

        // CameraMove — digital directions (camera-local: X=right, Y=up, Z=forward) + left stick.
        bind(InputKey::w, *m_move_action, InputAction::modulate(kMoveForward));
        bind(InputKey::s, *m_move_action, InputAction::modulate(kMoveBack));
        bind(InputKey::a, *m_move_action, InputAction::modulate(kMoveLeft));
        bind(InputKey::d, *m_move_action, InputAction::modulate(kMoveRight));
        bind(InputKey::up_arrow, *m_move_action, InputAction::modulate(kMoveForward));
        bind(InputKey::down_arrow, *m_move_action, InputAction::modulate(kMoveBack));
        bind(InputKey::left_arrow, *m_move_action, InputAction::modulate(kMoveLeft));
        bind(InputKey::right_arrow, *m_move_action, InputAction::modulate(kMoveRight));
        bind(InputKey::page_up, *m_move_action, InputAction::modulate(kMoveUp));
        bind(InputKey::page_down, *m_move_action, InputAction::modulate(kMoveDown));
        bind(InputKey::gamepad_dpad_up, *m_move_action, InputAction::modulate(kMoveUp));
        bind(InputKey::gamepad_dpad_down, *m_move_action, InputAction::modulate(kMoveDown));
        bind(InputKey::gamepad_left_2d, *m_move_action, [sens = m_gamepad_sensitivity.x](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float3{sens, sens, sens});
        });

        // CameraRotate — Q/E digital yaw, mouse + right stick analog.
        bind(InputKey::q, *m_rotate_action, [sens = m_mouse_sensitivity.x * 2.0f](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{-sens, 0.0f});
        });
        bind(InputKey::e, *m_rotate_action, [sens = m_mouse_sensitivity.x * 2.0f](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{sens, 0.0f});
        });
        bind(InputKey::mouse_2d, *m_rotate_action, [sens = m_mouse_sensitivity](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(sens);
        });
        bind(InputKey::gamepad_right_2d, *m_rotate_action, [sens = m_gamepad_sensitivity.y](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{1.0f, -1.0f} * sens);
        });

        // CameraSpeed — shift/ctrl + shoulders.
        bind(InputKey::left_shift, *m_speed_action, InputAction::modulate(kSpeedUp));
        bind(InputKey::left_control, *m_speed_action, InputAction::modulate(kSpeedDown));
        bind(InputKey::gamepad_left_shoulder, *m_speed_action, InputAction::modulate(kSpeedSlow));
        bind(InputKey::gamepad_right_shoulder, *m_speed_action, InputAction::modulate(kSpeedFast));

        // CameraFov — wheel + add/subtract.
        bind(InputKey::mouse_wheel_axis_y, *m_fov_action, InputAction::modulate(kFovIn));
        bind(InputKey::add, *m_fov_action, InputAction::modulate(kFovInSmall));
        bind(InputKey::subtract, *m_fov_action, InputAction::modulate(kFovOutSmall));

        // CameraLook — optional digital gate, no modifier (no-op in v1).
        out_mapping.mapKey(SharedInputAction{m_look_action.get()}, InputKey::left_mouse_button);
    }

    void FreeCameraController::onMoveStarted_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto axis = event.getAxis3DValue()) {
            if (m_move_contrib.insert({key, axis->m_absolute}).second) {
                m_move_sum += axis->m_absolute;
            }
        }
    }

    void FreeCameraController::onMoveCompleted_([[maybe_unused]] const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto it = m_move_contrib.find(key); it != m_move_contrib.end()) {
            m_move_sum -= it->second;
            m_move_contrib.erase(key);
        }
    }

    void FreeCameraController::onMoveTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::gamepad_left_2d) {
            if (const auto axis = event.getAxis3DValue()) {
                m_move_sum -= m_gamepad_move_last;
                m_gamepad_move_last = axis->m_absolute;
                m_move_sum += m_gamepad_move_last;
            }
        }
    }

    void FreeCameraController::onRotateStarted_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto axis = event.getAxis2DValue()) {
            if (m_rotate_contrib.insert({key, axis->m_absolute}).second) {
                m_rotate_sum += axis->m_absolute;
            }
        }
    }

    void FreeCameraController::onRotateCompleted_([[maybe_unused]] const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto it = m_rotate_contrib.find(key); it != m_rotate_contrib.end()) {
            m_rotate_sum -= it->second;
            m_rotate_contrib.erase(key);
        }
    }

    void FreeCameraController::onRotateTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::mouse_2d || key == InputKey::gamepad_right_2d) {
            if (const auto axis = event.getAxis2DValue()) {
                if (key == InputKey::gamepad_right_2d) {
                    m_rotate_sum -= m_gamepad_rotate_last;
                    m_gamepad_rotate_last = axis->m_absolute;
                    m_rotate_sum += m_gamepad_rotate_last;
                } else {
                    m_rotate_accum += axis->m_relative;
                }
            }
        }
    }

    void FreeCameraController::onSpeedStarted_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto axis = event.getAxis1DValue()) {
            if (m_speed_contrib.insert({key, axis->m_absolute}).second) {
                m_speed_sum += axis->m_absolute;
            }
        }
    }

    void FreeCameraController::onSpeedCompleted_([[maybe_unused]] const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto it = m_speed_contrib.find(key); it != m_speed_contrib.end()) {
            m_speed_sum -= it->second;
            m_speed_contrib.erase(key);
        }
    }

    void FreeCameraController::onFovAccumulate_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto axis = event.getAxis1DValue()) {
            if (key == InputKey::mouse_wheel_axis_y) {
                m_fov_accum += axis->m_relative;
            } else {
                if (m_fov_contrib.insert({key, axis->m_absolute}).second) {
                    m_fov_accum += axis->m_absolute;
                }
            }
        }
    }

    void FreeCameraController::onLookStarted_(const InputActionEvent &event [[maybe_unused]], const InputKey &key [[maybe_unused]]) noexcept {
        m_look_active = true;
    }

    void FreeCameraController::onLookCompleted_(const InputActionEvent &event [[maybe_unused]], const InputKey &key [[maybe_unused]]) noexcept {
        m_look_active = false;
    }

    void FreeCameraController::update(TimeSpan dt) noexcept {
        if (m_camera == nullptr)
            return;
        const float dts = dtSeconds(dt);

        // Compute target rotation from input.
        const float2 rotate = m_look_active
                                  ? (m_rotate_sum * m_rotate_speed * dts + m_rotate_accum * m_rotate_speed)
                                  : (m_rotate_accum * m_rotate_speed);
        const float target_yaw = m_yaw + rotate.x;
        const float target_pitch = clamp(m_pitch + rotate.y, -1.5f, 1.5f);

        // Compute target position from input.
        const float3 forward = forwardFromAngles(target_yaw, target_pitch);
        const float3 right = normalize(cross(forward, m_up));
        const float speed = m_move_speed * (1.0f + m_speed_sum);
        float3 digital_local = m_move_sum;
        if (dot(digital_local, digital_local) > 0.0f)
            digital_local = normalize(digital_local);
        const float3 target_eye = m_eye + (digital_local.x * right + digital_local.y * m_up + digital_local.z * forward) * speed * dts;

        // Apply inertia/smoothing: lerp current toward target.
        const bool has_move_input = dot(m_move_sum, m_move_sum) > 0.0f;
        const bool has_rotate_input = m_look_active || dot(m_rotate_sum, m_rotate_sum) > 0.0f || dot(m_rotate_accum, m_rotate_accum) > 0.0f;
        if (has_move_input) {
            const float pos_alpha = 1.0f - std::exp(-dts / std::max(m_position_inertia, 1e-6f));
            m_eye = lerp(m_eye, target_eye, pos_alpha);
        }
        if (has_rotate_input) {
            const float rot_alpha = 1.0f - std::exp(-dts / std::max(m_rotation_inertia, 1e-6f));
            m_yaw = lerp(m_yaw, target_yaw, rot_alpha);
            m_pitch = lerp(m_pitch, target_pitch, rot_alpha);
        }

        // Apply FOV changes.
        if (m_fov_accum != 0.0f) {
            m_fov = clamp(m_fov - m_fov_accum, m_fov_min, m_fov_max);
            m_fov_accum = 0.0f;
            const float2 &size = m_camera->viewportSize();
            const float aspect = size.y != 0.0f ? size.x / size.y : 1.0f;
            m_camera->setProjection(rhi::getPerspectiveMatrix(m_device_type, m_fov, aspect, 0.1f, 1000.0f));
        }

        m_camera->updateModel(CameraModel{m_eye, m_yaw, m_pitch});

        m_rotate_accum = float2{zero_v};
    }

    // ------------------------------------------------------------------
    // PanCameraController
    // ------------------------------------------------------------------

    PanCameraController::PanCameraController() noexcept {
        m_move_action = std::make_unique<InputAction>("CameraMove", EInputValueType::axis_3d, EInputActionFlags::none);
        m_rotate_action = std::make_unique<InputAction>("CameraRotate", EInputValueType::axis_2d, EInputActionFlags::none);
        m_speed_action = std::make_unique<InputAction>("CameraSpeed", EInputValueType::axis_1d, EInputActionFlags::none);
        m_fov_action = std::make_unique<InputAction>("CameraFov", EInputValueType::axis_1d, EInputActionFlags::none);
        m_look_action = std::make_unique<InputAction>("CameraLook", EInputValueType::digital, EInputActionFlags::none);

        m_move_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onMoveStarted_(event, key); });
        m_move_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onMoveCompleted_(event, key); });
        m_move_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept { onMoveTriggered_(event, key); });

        m_rotate_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onRotateStarted_(event, key); });
        m_rotate_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onRotateCompleted_(event, key); });
        m_rotate_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept { onRotateTriggered_(event, key); });

        m_speed_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onSpeedStarted_(event, key); });
        m_speed_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onSpeedCompleted_(event, key); });

        m_fov_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onZoomAccumulate_(event, key); });
        m_fov_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept { onZoomAccumulate_(event, key); });

        m_look_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onLookStarted_(event, key); });
        m_look_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onLookCompleted_(event, key); });
    }

    void PanCameraController::activate(IInputService &, Camera &camera) {
        m_camera = &camera;
        m_camera->setProjection(rhi::getOrthoMatrix(m_device_type, 10.0f * m_zoom, 10.0f * m_zoom));
    }

    void PanCameraController::deactivate() noexcept {
        m_camera = nullptr;
        m_move_sum = float3{zero_v};

        m_rotate_sum = float2{zero_v};
        m_rotate_accum = float2{zero_v};
        m_gamepad_move_last = float3{zero_v};
        m_gamepad_rotate_last = float2{zero_v};
        m_speed_sum = 0.0f;
        m_zoom_accum = 0.0f;
        m_move_contrib.clear();
        m_rotate_contrib.clear();
        m_speed_contrib.clear();
    }

    void PanCameraController::setDeviceType(rhi::DeviceType device_type) noexcept {
        m_device_type = device_type;
        if (m_camera) {
            m_camera->setProjection(rhi::getOrthoMatrix(m_device_type, 10.0f * m_zoom, 10.0f * m_zoom));
        }
    }

    void PanCameraController::provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept {
        const auto bind = [&](const InputKey &key, const InputAction &action, InputModifierEvent modifier) {
            InputActionKeyMapping &mapping = out_mapping.mapKey(SharedInputAction{&action}, key);
            mapping.m_modifier = std::move(modifier);
        };

        // CameraMove — digital directions (camera-local: X=right, Y=up, Z=forward) + left stick.
        bind(InputKey::w, *m_move_action, InputAction::modulate(kMoveForward));
        bind(InputKey::s, *m_move_action, InputAction::modulate(kMoveBack));
        bind(InputKey::a, *m_move_action, InputAction::modulate(kMoveLeft));
        bind(InputKey::d, *m_move_action, InputAction::modulate(kMoveRight));
        bind(InputKey::up_arrow, *m_move_action, InputAction::modulate(kMoveForward));
        bind(InputKey::down_arrow, *m_move_action, InputAction::modulate(kMoveBack));
        bind(InputKey::left_arrow, *m_move_action, InputAction::modulate(kMoveLeft));
        bind(InputKey::right_arrow, *m_move_action, InputAction::modulate(kMoveRight));
        bind(InputKey::page_up, *m_move_action, InputAction::modulate(kMoveUp));
        bind(InputKey::page_down, *m_move_action, InputAction::modulate(kMoveDown));
        bind(InputKey::gamepad_dpad_up, *m_move_action, InputAction::modulate(kMoveUp));
        bind(InputKey::gamepad_dpad_down, *m_move_action, InputAction::modulate(kMoveDown));
        bind(InputKey::gamepad_left_2d, *m_move_action, [sens = m_gamepad_sensitivity.x](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float3{sens, sens, sens});
        });

        // CameraRotate — Q/E digital yaw, mouse + right stick analog.
        bind(InputKey::q, *m_rotate_action, [sens = m_mouse_sensitivity.x * 2.0f](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{-sens, 0.0f});
        });
        bind(InputKey::e, *m_rotate_action, [sens = m_mouse_sensitivity.x * 2.0f](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{sens, 0.0f});
        });
        bind(InputKey::mouse_2d, *m_rotate_action, [sens = m_mouse_sensitivity](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(sens);
        });
        bind(InputKey::gamepad_right_2d, *m_rotate_action, [sens = m_gamepad_sensitivity.y](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{1.0f, -1.0f} * sens);
        });

        // CameraSpeed — shift/ctrl + shoulders.
        bind(InputKey::left_shift, *m_speed_action, InputAction::modulate(kSpeedUp));
        bind(InputKey::left_control, *m_speed_action, InputAction::modulate(kSpeedDown));
        bind(InputKey::gamepad_left_shoulder, *m_speed_action, InputAction::modulate(kSpeedSlow));
        bind(InputKey::gamepad_right_shoulder, *m_speed_action, InputAction::modulate(kSpeedFast));

        // CameraZoom — wheel + add/subtract.
        bind(InputKey::mouse_wheel_axis_y, *m_fov_action, InputAction::modulate(kFovIn));
        bind(InputKey::add, *m_fov_action, InputAction::modulate(kFovInSmall));
        bind(InputKey::subtract, *m_fov_action, InputAction::modulate(kFovOutSmall));

        // CameraLook — optional digital gate, no modifier (no-op in v1).
        out_mapping.mapKey(SharedInputAction{m_look_action.get()}, InputKey::left_mouse_button);
    }

    void PanCameraController::onMoveStarted_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto axis = event.getAxis3DValue()) {
            if (m_move_contrib.insert({key, axis->m_absolute}).second) {
                m_move_sum += axis->m_absolute;
            }
        }
    }

    void PanCameraController::onMoveCompleted_([[maybe_unused]] const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto it = m_move_contrib.find(key); it != m_move_contrib.end()) {
            m_move_sum -= it->second;
            m_move_contrib.erase(key);
        }
    }

    void PanCameraController::onMoveTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::gamepad_left_2d) {
            if (const auto axis = event.getAxis3DValue()) {
                m_move_sum -= m_gamepad_move_last;
                m_gamepad_move_last = axis->m_absolute;
                m_move_sum += m_gamepad_move_last;
            }
        }
    }

    void PanCameraController::onRotateStarted_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto axis = event.getAxis2DValue()) {
            if (m_rotate_contrib.insert({key, axis->m_absolute}).second) {
                m_rotate_sum += axis->m_absolute;
            }
        }
    }

    void PanCameraController::onRotateCompleted_([[maybe_unused]] const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto it = m_rotate_contrib.find(key); it != m_rotate_contrib.end()) {
            m_rotate_sum -= it->second;
            m_rotate_contrib.erase(key);
        }
    }

    void PanCameraController::onRotateTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::mouse_2d || key == InputKey::gamepad_right_2d) {
            if (const auto axis = event.getAxis2DValue()) {
                if (key == InputKey::gamepad_right_2d) {
                    m_rotate_sum -= m_gamepad_rotate_last;
                    m_gamepad_rotate_last = axis->m_absolute;
                    m_rotate_sum += m_gamepad_rotate_last;
                } else {
                    m_rotate_accum += axis->m_relative;
                }
            }
        }
    }

    void PanCameraController::onSpeedStarted_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto axis = event.getAxis1DValue()) {
            if (m_speed_contrib.insert({key, axis->m_absolute}).second) {
                m_speed_sum += axis->m_absolute;
            }
        }
    }

    void PanCameraController::onSpeedCompleted_([[maybe_unused]] const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto it = m_speed_contrib.find(key); it != m_speed_contrib.end()) {
            m_speed_sum -= it->second;
            m_speed_contrib.erase(key);
        }
    }

    void PanCameraController::onZoomAccumulate_(const InputActionEvent &event, [[maybe_unused]] const InputKey &key) noexcept {
        if (const auto axis = event.getAxis1DValue()) {
            m_zoom_accum += axis->m_relative;
        }
    }

    void PanCameraController::onLookStarted_(const InputActionEvent &, [[maybe_unused]] const InputKey &) noexcept {
        m_look_active = true;
    }

    void PanCameraController::onLookCompleted_(const InputActionEvent &, [[maybe_unused]] const InputKey &) noexcept {
        m_look_active = false;
    }

    void PanCameraController::update(TimeSpan dt) noexcept {
        if (m_camera == nullptr)
            return;
        const float dts = dtSeconds(dt);

        const float speed = m_pan_speed * (1.0f + m_speed_sum);
        // Digital (held keys) = rate → dt-scaled; analog (mouse/stick deltas) = displacement → NOT dt-scaled.
        float2 rotate = m_rotate_sum * m_rotate_speed * dts;
        if (m_look_active) {
            rotate += m_rotate_accum * m_rotate_speed;
        }
        m_yaw += rotate.x;
        m_pitch = clamp(m_pitch + rotate.y, -1.5f, 1.5f);
        m_zoom = clamp(m_zoom + m_zoom_accum, 0.1f, 10.0f);

        const float3 forward = forwardFromAngles(m_yaw, m_pitch);
        const float3 right = normalize(cross(forward, m_up));

        // Digital held keys → rate (dt-scaled), normalized direction.
        float3 digital_local = m_move_sum;
        if (dot(digital_local, digital_local) > 0.0f)
            digital_local = normalize(digital_local);
        const float3 digital_world = (digital_local.x * right + digital_local.y * m_up + digital_local.z * forward) * speed * dts;

        m_eye += digital_world;
        m_camera->setProjection(rhi::getOrthoMatrix(m_device_type, 10.0f * m_zoom, 10.0f * m_zoom));
        m_camera->updateModel(CameraModel{m_eye, m_yaw, m_pitch});

        m_rotate_accum = float2{zero_v};
        m_zoom_accum = 0.0f;
    }

    // ------------------------------------------------------------------
    // DummyCameraController
    // ------------------------------------------------------------------

    void DummyCameraController::activate(IInputService &, Camera &camera) noexcept {
        m_camera = &camera;
    }

    void DummyCameraController::deactivate() noexcept {
        m_camera = nullptr;
    }

    void DummyCameraController::update(TimeSpan) noexcept {
    }

    void DummyCameraController::setDeviceType(rhi::DeviceType) noexcept {
    }

    void DummyCameraController::provideInputActionKeyMappings(InputMapping &) const noexcept {
    }

    // ------------------------------------------------------------------
    // OrbitCameraController
    // ------------------------------------------------------------------

    OrbitCameraController::OrbitCameraController() noexcept {
        m_rotate_action = std::make_unique<InputAction>("CameraRotate", EInputValueType::axis_2d, EInputActionFlags::none);
        m_zoom_action = std::make_unique<InputAction>("CameraZoom", EInputValueType::axis_1d, EInputActionFlags::none);
        m_look_action = std::make_unique<InputAction>("CameraLook", EInputValueType::digital, EInputActionFlags::none);

        m_rotate_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onRotateStarted_(event, key); });
        m_rotate_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onRotateCompleted_(event, key); });
        m_rotate_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept { onRotateTriggered_(event, key); });

        m_zoom_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onZoomStarted_(event, key); });
        m_zoom_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onZoomCompleted_(event, key); });
        m_zoom_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept { onZoomTriggered_(event, key); });

        m_look_action->setStarted([this](const InputActionEvent &event, const InputKey &key) noexcept { onLookStarted_(event, key); });
        m_look_action->setCompleted([this](const InputActionEvent &event, const InputKey &key) noexcept { onLookCompleted_(event, key); });
    }

    void OrbitCameraController::activate(IInputService &, Camera &camera) {
        m_camera = &camera;
    }

    void OrbitCameraController::deactivate() noexcept {
        m_camera = nullptr;
        m_rotate_sum = float2{zero_v};
        m_rotate_accum = float2{zero_v};
        m_zoom_sum = 0.0f;
        m_zoom_accum = 0.0f;
        m_rotate_contrib.clear();
        m_zoom_contrib.clear();
    }

    void OrbitCameraController::setDeviceType(rhi::DeviceType device_type) noexcept {
        m_device_type = device_type;
    }

    void OrbitCameraController::provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept {
        const auto bind = [&](const InputKey &key, const InputAction &action, InputModifierEvent modifier) {
            InputActionKeyMapping &mapping = out_mapping.mapKey(SharedInputAction{&action}, key);
            mapping.m_modifier = std::move(modifier);
        };

        // CameraRotate — Q/E digital yaw, mouse analog (when LMB held).
        bind(InputKey::q, *m_rotate_action, [sens = m_mouse_sensitivity.x * 2.0f](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{-sens, 0.0f});
        });
        bind(InputKey::e, *m_rotate_action, [sens = m_mouse_sensitivity.x * 2.0f](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{sens, 0.0f});
        });
        bind(InputKey::mouse_2d, *m_rotate_action, [sens = m_mouse_sensitivity](TimeSpan, InputValue &output) noexcept {
            output = output.modulate(sens);
        });

        // CameraZoom — wheel + add/subtract.
        bind(InputKey::mouse_wheel_axis_y, *m_zoom_action, InputAction::modulate(kFovIn));
        bind(InputKey::add, *m_zoom_action, InputAction::modulate(kFovInSmall));
        bind(InputKey::subtract, *m_zoom_action, InputAction::modulate(kFovOutSmall));

        // CameraLook — LMB gate for mouse-rotate.
        out_mapping.mapKey(SharedInputAction{m_look_action.get()}, InputKey::left_mouse_button);
    }

    void OrbitCameraController::onRotateStarted_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto axis = event.getAxis2DValue()) {
            if (m_rotate_contrib.insert({key, axis->m_absolute}).second) {
                m_rotate_sum += axis->m_absolute;
            }
        }
    }

    void OrbitCameraController::onRotateCompleted_([[maybe_unused]] const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto it = m_rotate_contrib.find(key); it != m_rotate_contrib.end()) {
            m_rotate_sum -= it->second;
            m_rotate_contrib.erase(key);
        }
    }

    void OrbitCameraController::onRotateTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::mouse_2d) {
            if (const auto axis = event.getAxis2DValue()) {
                m_rotate_accum += axis->m_relative;
            }
        }
    }

    void OrbitCameraController::onZoomStarted_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto axis = event.getAxis1DValue()) {
            if (m_zoom_contrib.insert({key, axis->m_absolute}).second) {
                m_zoom_sum += axis->m_absolute;
            }
        }
    }

    void OrbitCameraController::onZoomCompleted_([[maybe_unused]] const InputActionEvent &event, const InputKey &key) noexcept {
        if (const auto it = m_zoom_contrib.find(key); it != m_zoom_contrib.end()) {
            m_zoom_sum -= it->second;
            m_zoom_contrib.erase(key);
        }
    }

    void OrbitCameraController::onZoomTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::mouse_wheel_axis_y) {
            if (const auto axis = event.getAxis1DValue()) {
                m_zoom_accum += axis->m_relative;
            }
        }
    }

    void OrbitCameraController::onLookStarted_(const InputActionEvent &, const InputKey &) noexcept {
        m_look_active = true;
    }

    void OrbitCameraController::onLookCompleted_(const InputActionEvent &, const InputKey &) noexcept {
        m_look_active = false;
    }

    void OrbitCameraController::update(TimeSpan dt) noexcept {
        if (m_camera == nullptr)
            return;
        const float dts = dtSeconds(dt);

        // Apply rotate: mouse delta when look active, otherwise Q/E or accumulated.
        const float2 rotate = m_look_active
                                  ? (m_rotate_sum * m_rotate_speed * dts + m_rotate_accum * m_rotate_speed)
                                  : (m_rotate_accum * m_rotate_speed);
        m_yaw += rotate.x;
        m_pitch = clamp(m_pitch + rotate.y, -1.5f, 1.5f);

        // Apply zoom: keyboard rate is dt-scaled; mouse wheel displacement is applied directly.
        m_distance = clamp(m_distance - m_zoom_sum * m_zoom_speed * dts - m_zoom_accum, m_min_distance, m_max_distance);

        // Compute camera position from target + distance + angles.
        const float3 offset = forwardFromAngles(m_yaw, m_pitch) * m_distance;
        const float3 eye = m_target + offset;

        m_camera->updateModel(CameraModel{eye, m_yaw, m_pitch, false});

        m_rotate_accum = float2{zero_v};
        m_zoom_accum = 0.0f;
    }

    CameraService::CameraService() noexcept = default;

    void CameraService::deactivateController() noexcept {
        if (m_input) {
            m_input->removeGlobalInputMapping(m_controller_mapping);
        }
        if (m_controller)
            m_controller->deactivate();
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
        if (m_controller) {
            if (m_input)
                m_input->removeGlobalInputMapping(m_controller_mapping);
            m_controller->deactivate();
            m_controller_mapping.clearAllMappings();
        }
        m_controller = std::move(controller);
        if (m_controller && m_input) {
            m_controller->activate(*m_input, m_camera);
            m_controller->setDeviceType(m_device_type);
            m_controller->provideInputActionKeyMappings(m_controller_mapping);
            m_input->addGlobalInputMapping(SharedInputMapping{&m_controller_mapping});
        }
    }

    Camera &CameraService::camera() noexcept { return m_camera; }

    void CameraService::update(TimeSpan dt) noexcept {
        if (m_controller)
            m_controller->update(dt);
        m_camera.update(dt);
    }

    void CameraService::setPerspectiveProjection(float fov, float aspect, float near_, float far_) noexcept {
        m_camera.setProjection(rhi::getPerspectiveMatrix(m_device_type, fov, aspect, near_, far_));
    }

    void CameraService::setOrthoProjection(float width, float height) noexcept {
        m_camera.setProjection(rhi::getOrthoMatrix(m_device_type, width, height));
    }
}
