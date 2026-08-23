module;

#include "pP/Macros.h"

module engine.app;

import :viewport.camera;
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

        constexpr float kPitchLimit = 1.5f;

        [[nodiscard]] constexpr float clampPitch(float p) noexcept {
            return std::clamp(p, -kPitchLimit, kPitchLimit);
        }

        [[nodiscard]] CameraModel cameraModelFromAngles(const float3 &eye, float yaw, float pitch, bool camera_cut, float fov = std::numbers::pi_v<float> / 3.0f,
                                                        float z_near = 0.01f, float z_far = 10000.0f) noexcept {
            const float3 forward = forwardFromAngles(yaw, pitch);
            const float3 right_raw = cross(float3{0.0f, 1.0f, 0.0f}, forward);
            const float3 right = dot(right_raw, right_raw) > epsilon_v
                                     ? normalize(right_raw)
                                     : float3{1.0f, 0.0f, 0.0f};
            const float3 up = cross(forward, right);
            return CameraModel{
                .position = eye,
                .right = right,
                .up = up,
                .forward = forward,
                .fov = fov,
                .zNear = z_near,
                .zFar = z_far,
                .cameraCut = camera_cut
            };
        }

        // Viewport size for updateModel: round-trips the camera's stored float2 size
        // through int2, falling back to a sane default before the first resize.
        [[nodiscard]] int2 cameraViewportSize(const Camera &camera) noexcept {
            const float2 size = camera.viewportSize();
            if (size.x > 0.0f && size.y > 0.0f) {
                return int2{static_cast<int>(size.x), static_cast<int>(size.y)};
            }
            return int2{1920, 1080};
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
        m_viewProjection = m_current.view * m_current.projection;
        m_current.invertViewProjection = inverse(m_viewProjection);
        m_current.invertView = inverse(m_current.view);
        m_current.invertProjection = inverse(m_current.projection);
        // Both projection modes currently share the D3D depth convention; the mode
        // dispatch is where a Vulkan/OpenGL convention would be selected.
        const rhi::EProjectionConvention convention = rhi::EProjectionConvention::D3D;
        m_current.frustum.setMatrix(m_viewProjection, m_current.invertViewProjection, convention);
    }

    void Camera::updateModel(const CameraModel &model, const int2 &viewportSize) noexcept {
        m_viewportSize = float2{static_cast<float>(viewportSize.x), static_cast<float>(viewportSize.y)};
        m_current.model = model;
        m_current.view = makeLookAtMatrix(model.position, model.position + model.forward, model.up);
        recomputeViewProjection();
        m_pending_cut = model.cameraCut;
        ++m_camera_version;
    }

    void Camera::setViewportSize(const int2 &size) noexcept {
        m_viewportSize = float2{static_cast<float>(size.x), static_cast<float>(size.y)};
        ++m_camera_version;
    }

    void Camera::setPosition(const float3 &position) noexcept {
        m_current.model.position = position;
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
                return float4{0.0f, 0.0f, 0.0f, max_v};
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
        m_bbox_min = float3{max_v};
        m_bbox_max = float3{min_v};

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
        // vertex is outside the plane, the AABB is fully outside.
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
        if (not m_pending_cut) {
            m_velocity = dt_s > 0.0f ? (m_current.model.position - m_previous.model.position) * (1.0f / dt_s) : float3{zero_v};
            m_previous.model.position = m_current.model.position;
        } else {
            // Camera cut: zero velocity and re-baseline the previous position so the
            // teleport delta never leaks into the next frame's velocity.
            m_velocity = float3{zero_v};
            m_previous.model.position = m_current.model.position;
            m_pending_cut = false;
        }
    }

    void Camera::setMode(ECameraProjection value) noexcept { m_mode = value; }

    float3 Camera::worldToClip(const float3 &world) const noexcept {
        const float4 clip = float4{world, 1.0f} * m_viewProjection;
        const float w = clip.w != 0.0f ? clip.w : 1.0f;
        return float3{clip.x / w, clip.y / w, clip.z / w};
    }

    float3 Camera::clipToWorld(const float4 &clip) const noexcept {
        const float4 world = clip * m_current.invertViewProjection;
        const float w = world.w != 0.0f ? world.w : 1.0f;
        return float3{world.x / w, world.y / w, world.z / w};
    }

    float3 Camera::clientToWorld(const float2 &client, float z_ndc) const noexcept {
        const float2 size = m_viewportSize;
        PPR_ASSERT(size.x > 0.0f && size.y > 0.0f);
        const float w = size.x != 0.0f ? size.x : 1.0f;
        const float h = size.y != 0.0f ? size.y : 1.0f;
        const float ndc_x = (client.x / w) * 2.0f - 1.0f;
        const float ndc_y = (client.y / h) * 2.0f - 1.0f;
        return clipToWorld(float4{ndc_x, ndc_y, z_ndc, 1.0f});
    }

    float3 Camera::screenToWorld(const float2 &screen, const int2 &framebuffer_size, float z_ndc) const noexcept {
        PPR_ASSERT(framebuffer_size.x > 0 && framebuffer_size.y > 0);
        const float2 size = m_viewportSize;
        const float2 fb{static_cast<float>(framebuffer_size.x), static_cast<float>(framebuffer_size.y)};
        const float2 origin = (size - fb) * 0.5f;
        return clientToWorld(screen - origin, z_ndc);
    }

    float2 Camera::worldToScreen(const float3 &world, const int2 &framebuffer_size) const noexcept {
        PPR_ASSERT(framebuffer_size.x > 0 && framebuffer_size.y > 0);
        const float3 ndc = worldToClip(world);
        const float2 size = m_viewportSize;
        const float2 fb{static_cast<float>(framebuffer_size.x), static_cast<float>(framebuffer_size.y)};
        const float2 origin = (size - fb) * 0.5f;
        const float2 client{(ndc.x * 0.5f + 0.5f) * size.x, (ndc.y * 0.5f + 0.5f) * size.y};
        return client + origin;
    }

    std::pair<float3, float3> Camera::clientToWorldRay(const float2 &client) const noexcept {
        const float3 near_point = clientToWorld(client, -1.0f);
        const float3 far_point = clientToWorld(client, 1.0f);
        const float3 direction = far_point - near_point;
        const float len = std::sqrt(dot(direction, direction));
        return {near_point, len > 0.0f ? direction * (1.0f / len) : float3{0.0f, 0.0f, 1.0f}};
    }

    void ICameraController::teleport(const float3 &, const float3 &) noexcept {
        // Default no-op: only OrbitCameraController supports eye+target teleport.
    }

    // ------------------------------------------------------------------
    // FreeCameraController
    // ------------------------------------------------------------------

    FreeCameraController::FreeCameraController() // NOLINT(*-use-equals-default)
        : m_move_action{std::make_unique<InputAction>("CameraMove", EInputValueType::axis_3d, EInputActionFlags::none)},
          m_rotate_action{std::make_unique<InputAction>("CameraRotate", EInputValueType::axis_2d, EInputActionFlags::none)},
          m_speed_action{std::make_unique<InputAction>("CameraSpeed", EInputValueType::axis_1d, EInputActionFlags::none)},
          m_fov_action{std::make_unique<InputAction>("CameraFov", EInputValueType::axis_1d, EInputActionFlags::none)},
          m_look_action{std::make_unique<InputAction>("CameraLook", EInputValueType::digital, EInputActionFlags::none)},
          m_position_analog{float3{zero_v}, 0.15f},
          m_rotation_analog{Quaternion{0.0f, 0.0f, 0.0f, 1.0f}, 0.15f},
          m_fov_analog{std::numbers::pi_v<float> / 3.0f, 0.8f},
          m_speed_analog{1.0f, 0.8f} {
        m_fov_action->setTriggered([this](const InputActionEvent &event, const InputKey &) noexcept {
            if (const auto axis = event.getAxis1DValue()) {
                m_fov_analog.addClamp(axis->m_relative, m_fov_min_max.x, m_fov_min_max.y);
            }
        });
        m_look_action->setTriggered([this](const InputActionEvent &event, const InputKey &) noexcept {
            m_b_mouse_look = event.getDigitalValue().has_value() && *event.getDigitalValue();
        });
        m_move_action->setTriggered([this](const InputActionEvent &event, const InputKey &) noexcept {
            if (const auto axis = event.getAxis3DValue()) {
                translate(axis->m_absolute);
            }
        });
        m_rotate_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept {
            if (const auto axis = event.getAxis2DValue()) {
                if (key != InputKey::mouse_2d)
                    rotate(axis->m_absolute);
                else if (m_b_mouse_look)
                    rotate(axis->m_relative);
            }
        });
        m_speed_action->setTriggered([this](const InputActionEvent &event, const InputKey &) noexcept {
            if (const auto axis = event.getAxis1DValue()) {
                m_speed_analog.addClamp(axis->m_relative, m_speed_multiplier_range.x, m_speed_multiplier_range.y);
            }
        });
    }

    void FreeCameraController::activate(IInputService &, Camera &camera) {
        m_camera = &camera;
    }

    void FreeCameraController::deactivate() noexcept {
        m_camera = nullptr;
        m_position_analog.reset(float3{zero_v});
        m_rotation_analog.reset(Quaternion{0.0f, 0.0f, 0.0f, 1.0f});
        m_fov_analog.reset(std::numbers::pi_v<float> / 3.0f);
        m_speed_analog.reset(1.0f);
    }

    void FreeCameraController::translate(const float3 &delta) noexcept {
        m_delta_position += delta;
    }

    void FreeCameraController::rotate(const float2 &delta) noexcept {
        m_delta_rotation += delta;
    }

    void FreeCameraController::lookAt(const float3 &eye, const float3 &target, const float3 &up, bool teleport) noexcept {
        // Build the rotation from the corrected basis so local +Z maps to the camera's
        // forward (toward the target), not the lookAt matrix's zaxis (which points from
        // target to eye — the camera's backward).
        const float3 dir = target - eye;
        const float3 forward = dot(dir, dir) > epsilon_t<float>
                                   ? normalize(dir)
                                   : float3{0.0f, 0.0f, 1.0f};
        const float3 right = normalize(cross(up, forward));
        const float3 corrected_up = cross(forward, right);
        const float3x3 rotation_matrix = float3x3{right, corrected_up, forward};
        const Quaternion rotation = makeQuaternionFromRotationMatrix(rotation_matrix);
        if (teleport) {
            m_b_teleported = true;
            m_position_analog.reset(eye);
            m_rotation_analog.reset(rotation);
        } else {
            m_position_analog.setRaw(eye);
            m_rotation_analog.setRaw(rotation);
        }
    }

    void FreeCameraController::lookAt(const float3 &eye, float heading, float pitch, bool teleport) noexcept {
        if (teleport) {
            m_b_teleported = true;
            m_position_analog.reset(eye);
            m_rotation_analog.reset(makeYawPitchRollQuaternion(heading, pitch, 0.0f));
        } else {
            m_position_analog.setRaw(eye);
            m_rotation_analog.setRaw(makeYawPitchRollQuaternion(heading, pitch, 0.0f));
        }
    }

    void FreeCameraController::teleport(const float3 &eye, float yaw, float pitch) noexcept {
        lookAt(eye, yaw, pitch, true);
    }

    void FreeCameraController::teleport(const float3 &eye, const float3 &target) noexcept {
        lookAt(eye, target, float3{0.0f, 1.0f, 0.0f}, true);
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

        // CameraLook — LMB gate for mouse-rotate.
        out_mapping.mapKey(SharedInputAction{m_look_action.get()}, InputKey::left_mouse_button);
    }

    void FreeCameraController::updateCamera(TimeSpan dt, CameraModel &model) noexcept {
        if (m_camera == nullptr)
            return;

        m_fov_analog.update(dt);
        m_speed_analog.update(dt);

        if (not m_b_teleported && dot(m_delta_rotation, m_delta_rotation) > 0.0f) {
            const Quaternion delta_q = makeYawPitchRollQuaternion(m_delta_rotation.x, m_delta_rotation.y, 0.0f);
            m_rotation_analog.setRaw(delta_q * m_rotation_analog.raw());
        }
        m_rotation_analog.update(dt);

        if (not m_b_teleported && dot(m_delta_position, m_delta_position) > 0.0f) {
            const float3 local_delta = m_delta_position * m_speed_analog.filtered();
            const float3 world_delta = quaternionTransform(m_rotation_analog.raw(), local_delta);
            m_position_analog.add(world_delta);
        }
        m_position_analog.update(dt);

        const Quaternion rotation = m_rotation_analog.filtered();
        model.position = m_position_analog.filtered();
        model.right = quaternionTransform(rotation, float3{1.0f, 0.0f, 0.0f});
        model.up = quaternionTransform(rotation, float3{0.0f, 1.0f, 0.0f});
        model.forward = quaternionTransform(rotation, float3{0.0f, 0.0f, 1.0f});
        model.fov = m_fov_analog.filtered();
        model.cameraCut = m_b_teleported;

        m_b_mouse_look = false;
        m_b_teleported = false;
        m_delta_position = float3{zero_v};
        m_delta_rotation = float2{zero_v};
    }

    void FreeCameraController::setForwardSpeed(float value) noexcept { m_forward_speed = value; }
    void FreeCameraController::setStrafeSpeed(float value) noexcept { m_strafe_speed = value; }
    void FreeCameraController::setUpwardSpeed(float value) noexcept { m_upward_speed = value; }
    void FreeCameraController::setHeadingSpeed(float value) noexcept { m_heading_speed = value; }
    void FreeCameraController::setPitchSpeed(float value) noexcept { m_pitch_speed = value; }
    void FreeCameraController::setFovMinMax(float2 value) noexcept { m_fov_min_max = value; }
    void FreeCameraController::setSpeedMultiplierMinMax(float2 value) noexcept { m_speed_multiplier_range = value; }
    void FreeCameraController::setMouseSensitivity(float2 value) noexcept { m_mouse_sensitivity = value; }
    void FreeCameraController::setGamepadSensitivity(float2 value) noexcept { m_gamepad_sensitivity = value; }
    float3 FreeCameraController::position() const noexcept { return m_position_analog.filtered(); }
    Quaternion FreeCameraController::rotation() const noexcept { return m_rotation_analog.filtered(); }
    float FreeCameraController::fov() const noexcept { return m_fov_analog.filtered(); }
    float FreeCameraController::speedMultiplier() const noexcept { return m_speed_analog.filtered(); }
    float FreeCameraController::positionInertia() const noexcept { return m_position_analog.sensitivity(); }
    void FreeCameraController::setPositionInertia(float value) noexcept { m_position_analog.setSensitivity(value); }
    float FreeCameraController::rotationInertia() const noexcept { return m_rotation_analog.sensitivity(); }
    void FreeCameraController::setRotationInertia(float value) noexcept { m_rotation_analog.setSensitivity(value); }

    // ------------------------------------------------------------------
    // PanCameraController
    // ------------------------------------------------------------------

    PanCameraController::PanCameraController() // NOLINT(*-use-equals-default)
        : m_position_analog{float3{zero_v}, 0.15f},
          m_rotation_analog{Quaternion{0.0f, 0.0f, 0.0f, 1.0f}, 0.15f},
          m_zoom_analog{0.0f, 0.8f} {
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
        m_position_analog.reset(float3{zero_v});
        m_rotation_analog.reset(Quaternion{0.0f, 0.0f, 0.0f, 1.0f});
        m_zoom_analog.reset(0.0f);
        m_speed_sum = 0.0f;
        m_speed_contrib.clear();
    }

    void PanCameraController::setDeviceType(rhi::DeviceType device_type) noexcept {
        m_device_type = device_type;
        if (m_camera) {
            m_camera->setProjection(rhi::getOrthoMatrix(m_device_type, 10.0f * m_zoom, 10.0f * m_zoom));
        }
    }

    void PanCameraController::teleport(const float3 &eye, float yaw, float pitch) noexcept {
        m_eye = eye;
        m_yaw = yaw;
        m_pitch = clampPitch(pitch);
        m_look_active = false;
        m_speed_sum = 0.0f;
        m_speed_contrib.clear();
        if (m_camera) {
            m_camera->updateModel(cameraModelFromAngles(m_eye, m_yaw, m_pitch, true), cameraViewportSize(*m_camera));
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

    void PanCameraController::onMoveStarted_(const InputActionEvent &event, const InputKey &key [[maybe_unused]]) noexcept {
        if (const auto axis = event.getAxis3DValue()) {
            m_position_analog.add(axis->m_absolute);
        }
    }

    void PanCameraController::onMoveCompleted_(const InputActionEvent &event, const InputKey &key [[maybe_unused]]) noexcept {
        if (const auto axis = event.getAxis3DValue()) {
            m_position_analog.add(-axis->m_absolute);
        }
    }

    void PanCameraController::onMoveTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::gamepad_left_2d) {
            if (const auto axis = event.getAxis3DValue()) {
                m_position_analog.add(axis->m_relative);
            }
        }
    }

    void PanCameraController::onRotateStarted_(const InputActionEvent &event, const InputKey &key [[maybe_unused]]) noexcept {
        if (const auto axis = event.getAxis2DValue()) {
            m_rotation_analog.add(makeYawPitchRollQuaternion(axis->m_absolute.x, axis->m_absolute.y, 0.0f));
        }
    }

    void PanCameraController::onRotateCompleted_(const InputActionEvent &event, const InputKey &key [[maybe_unused]]) noexcept {
        if (const auto axis = event.getAxis2DValue()) {
            m_rotation_analog.add(makeYawPitchRollQuaternion(-axis->m_absolute.x, -axis->m_absolute.y, 0.0f));
        }
    }

    void PanCameraController::onRotateTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::mouse_2d || key == InputKey::gamepad_right_2d) {
            if (const auto axis = event.getAxis2DValue()) {
                m_rotation_analog.add(makeYawPitchRollQuaternion(axis->m_relative.x, axis->m_relative.y, 0.0f));
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
            m_zoom_analog.add(axis->m_relative);
        }
    }

    void PanCameraController::onLookStarted_(const InputActionEvent &, [[maybe_unused]] const InputKey &) noexcept {
        m_look_active = true;
    }

    void PanCameraController::onLookCompleted_(const InputActionEvent &, [[maybe_unused]] const InputKey &) noexcept {
        m_look_active = false;
    }

    void PanCameraController::updateCamera(TimeSpan dt, [[maybe_unused]] CameraModel &model) noexcept {
        if (m_camera == nullptr)
            return;
        m_position_analog.update(dt);
        m_rotation_analog.update(dt);
        m_zoom_analog.update(dt);
        const float3 position_delta = m_position_analog.delta();
        const Quaternion rotation_delta = m_rotation_analog.delta();
        const float zoom_delta = m_zoom_analog.delta();
        const float dts = dtSeconds(dt);

        const float speed = m_pan_speed * (1.0f + m_speed_sum);
        // Digital (held keys) = rate → dt-scaled; analog (mouse/stick deltas) = displacement → NOT dt-scaled.
        const float2 rotation_yawpitch = quaternionToYawPitch(rotation_delta);
        const float2 rotate = rotation_yawpitch * m_rotate_speed * dts;
        m_yaw += rotate.x;
        m_pitch = clamp(m_pitch + rotate.y, -1.5f, 1.5f);
        m_zoom = clamp(m_zoom + zoom_delta, 0.1f, 10.0f);

        const float3 forward = forwardFromAngles(m_yaw, m_pitch);
        const float3 right_raw = cross(forward, m_up);
        const float3 right = dot(right_raw, right_raw) > epsilon_t<float>
                                 ? normalize(right_raw)
                                 : float3{1.0f, 0.0f, 0.0f};

        // Digital held keys → rate (dt-scaled), normalized direction.
        float3 digital_local = position_delta;
        if (dot(digital_local, digital_local) > 0.0f)
            digital_local = normalize(digital_local);
        const float3 digital_world = (digital_local.x * right + digital_local.y * m_up + digital_local.z * forward) * speed * dts;

        m_eye += digital_world;
        m_camera->setProjection(rhi::getOrthoMatrix(m_device_type, 10.0f * m_zoom, 10.0f * m_zoom));
        m_camera->updateModel(cameraModelFromAngles(m_eye, m_yaw, m_pitch, false), cameraViewportSize(*m_camera));
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

    void DummyCameraController::updateCamera(TimeSpan, CameraModel &) noexcept {
    }

    void DummyCameraController::setDeviceType(rhi::DeviceType) noexcept {
    }

    void DummyCameraController::teleport(const float3 &, float, float) noexcept {
        // No-op
    }

    void DummyCameraController::provideInputActionKeyMappings(InputMapping &) const noexcept {
    }

    // ------------------------------------------------------------------
    // OrbitCameraController
    // ------------------------------------------------------------------

    OrbitCameraController::OrbitCameraController() // NOLINT(*-use-equals-default)
        : m_rotate_analog{float2{zero_v}, 0.15f},
          m_zoom_analog{0.0f, 0.8f} {
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
        m_rotate_analog.reset(float2{zero_v});
        m_zoom_analog.reset(0.0f);
    }

    void OrbitCameraController::setDeviceType(rhi::DeviceType device_type) noexcept {
        m_device_type = device_type;
    }

    void OrbitCameraController::teleport(const float3 &eye, const float3 &target) noexcept {
        m_target = target;
        const float3 offset = eye - target;
        m_distance = std::sqrt(dot(offset, offset));
        m_distance = clamp(m_distance, m_min_distance, m_max_distance);
        // Derive yaw/pitch from offset direction
        const float3 dir = m_distance > 0.0f ? offset * (1.0f / m_distance) : float3{0.0f, 0.0f, -1.0f};
        m_pitch = std::asin(clamp(dir.y, -1.0f, 1.0f));
        m_yaw = std::atan2(dir.x, -dir.z);
        m_look_active = false;
        if (m_camera) {
            const float3 final_offset = forwardFromAngles(m_yaw, m_pitch) * m_distance;
            const float3 final_eye = m_target + final_offset;
            m_camera->updateModel(cameraModelFromAngles(final_eye, m_yaw, m_pitch, true), cameraViewportSize(*m_camera));
        }
    }

    void OrbitCameraController::teleport(const float3 &, float, float) noexcept {
        // No-op: Orbit teleport is defined by eye+target.
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

    void OrbitCameraController::onRotateStarted_(const InputActionEvent &event, const InputKey &key [[maybe_unused]]) noexcept {
        if (const auto axis = event.getAxis2DValue()) {
            m_rotate_analog.add(axis->m_absolute);
        }
    }

    void OrbitCameraController::onRotateCompleted_(const InputActionEvent &event, const InputKey &key [[maybe_unused]]) noexcept {
        if (const auto axis = event.getAxis2DValue()) {
            m_rotate_analog.add(-axis->m_absolute);
        }
    }

    void OrbitCameraController::onRotateTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::mouse_2d) {
            if (const auto axis = event.getAxis2DValue()) {
                m_rotate_analog.add(axis->m_relative);
            }
        }
    }

    void OrbitCameraController::onZoomStarted_(const InputActionEvent &event, const InputKey &key [[maybe_unused]]) noexcept {
        if (const auto axis = event.getAxis1DValue()) {
            m_zoom_analog.add(axis->m_absolute);
        }
    }

    void OrbitCameraController::onZoomCompleted_(const InputActionEvent &event, const InputKey &key [[maybe_unused]]) noexcept {
        if (const auto axis = event.getAxis1DValue()) {
            m_zoom_analog.add(-axis->m_absolute);
        }
    }

    void OrbitCameraController::onZoomTriggered_(const InputActionEvent &event, const InputKey &key) noexcept {
        if (key == InputKey::mouse_wheel_axis_y) {
            if (const auto axis = event.getAxis1DValue()) {
                m_zoom_analog.add(axis->m_relative);
            }
        }
    }

    void OrbitCameraController::onLookStarted_(const InputActionEvent &, const InputKey &) noexcept {
        m_look_active = true;
    }

    void OrbitCameraController::onLookCompleted_(const InputActionEvent &, const InputKey &) noexcept {
        m_look_active = false;
    }

    void OrbitCameraController::updateCamera(TimeSpan dt, [[maybe_unused]] CameraModel &model) noexcept {
        if (m_camera == nullptr)
            return;
        m_rotate_analog.update(dt);
        m_zoom_analog.update(dt);
        const float2 rotate_delta = m_rotate_analog.delta();
        const float zoom_delta = m_zoom_analog.delta();
        const float dts = dtSeconds(dt);

        // Apply rotate: mouse delta when look active, otherwise Q/E or accumulated.
        const float2 rotate = m_look_active ? rotate_delta * m_rotate_speed * dts : float2{zero_v};
        m_yaw += rotate.x;
        m_pitch = clamp(m_pitch + rotate.y, -1.5f, 1.5f);

        // Apply zoom: keyboard rate is dt-scaled; mouse wheel displacement is applied directly.
        m_distance = clamp(m_distance - zoom_delta * m_zoom_speed * dts, m_min_distance, m_max_distance);

        // Compute camera position from target + distance + angles.
        const float3 offset = forwardFromAngles(m_yaw, m_pitch) * m_distance;
        const float3 eye = m_target + offset;

        m_camera->updateModel(cameraModelFromAngles(eye, m_yaw, m_pitch, false), cameraViewportSize(*m_camera));
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
        CameraModel model = m_camera.currentState().model;
        if (m_controller)
            m_controller->updateCamera(dt, model);
        m_camera.updateModel(model, cameraViewportSize(m_camera));
        m_camera.update(dt);
    }

    void CameraService::setPerspectiveProjection(float fov, float aspect, float near_, float far_) noexcept {
        m_camera.setProjection(rhi::getPerspectiveMatrix(m_device_type, fov, aspect, near_, far_));
    }

    void CameraService::setOrthoProjection(float width, float height) noexcept {
        m_camera.setProjection(rhi::getOrthoMatrix(m_device_type, width, height));
    }

    void CameraService::teleport(const float3 &eye, float yaw, float pitch) noexcept {
        if (m_controller) {
            // Try Free/Pan signature first
            m_controller->teleport(eye, yaw, pitch);
        }
    }

    void CameraService::teleport(const float3 &eye, const float3 &target) noexcept {
        if (m_controller) {
            // Try Orbit signature
            m_controller->teleport(eye, target);
        }
    }
}
