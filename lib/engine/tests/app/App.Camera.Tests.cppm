module;

#include "pP/Macros.h"
#include "pP/UnitTest.h"

export module engine.tests.app:camera;

import engine.app;
import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP::tests {
    constexpr float kEps = 1e-4f;

    [[nodiscard]] bool matEq(const float4x4 &a, const float4x4 &b) noexcept {
        return std::ranges::equal(
            std::span<const float, 16>(a.data(), 16),
            std::span<const float, 16>(b.data(), 16),
            [](float x, float y) noexcept { return std::abs(x - y) <= kEps; });
    }

    [[nodiscard]] Viewport testViewport(const int2 extent) noexcept {
        return Viewport{PixelRect{int2{0, 0}, extent}, ViewportLayout{}};
    }

    PPR_UNIT_TEST (camera_model) {
        Camera cam;
        CameraModel model{};
        model.m_origin = float3{1.0f, 2.0f, 3.0f};
        cam.updateModel(std::chrono::milliseconds{16}, model, testViewport(int2{800, 600}));

        const CameraSnapshot &snap = cam.getSnapshot();
        const float3 expected_forward = quaternionTransform(model.m_basis, math::axis_z);
        const float3 expected_up = quaternionTransform(model.m_basis, math::axis_y);
        const float4x4 expected_view = float4x4::lookat(model.m_origin + expected_forward, model.m_origin, expected_up);
        const float4x4 expected_proj = rhi::getPerspectiveMatrix(model.m_fov, 800.0f / 600.0f, model.m_z_near, model.m_z_far);
        PPR_TEST_ASSERT(matEq(snap.m_view, expected_view));
        PPR_TEST_ASSERT(matEq(snap.m_projection, expected_proj));
        PPR_TEST_ASSERT(matEq(snap.m_view_projection, expected_view * expected_proj));
        PPR_TEST_ASSERT(matEq(snap.m_invert_view_projection, inverse(expected_view * expected_proj)));
        PPR_TEST_ASSERT(distance(snap.m_origin, model.m_origin) < kEps);
    };

    PPR_UNIT_TEST (lookat_canonical) {
        const float4x4 view = float4x4::lookat(float3{0.0f, 0.0f, 1.0f}, float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 1.0f, 0.0f});
        const float4x4 expected{
            float4{1.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 1.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 1.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 1.0f}
        };
        PPR_TEST_ASSERT(matEq(view, expected));
    };

    PPR_UNIT_TEST (lookat_eye_to_origin) {
        const float3 eye{1.0f, 2.0f, 3.0f};
        const float3 target{4.0f, 5.0f, 6.0f};
        const float3 up{0.0f, 1.0f, 0.0f};
        const float4x4 view = float4x4::lookat(target, eye, up);
        const float4 eye_view = float4{eye, 1.0f} * view;
        PPR_TEST_ASSERT(std::abs(eye_view.x) < kEps);
        PPR_TEST_ASSERT(std::abs(eye_view.y) < kEps);
        PPR_TEST_ASSERT(std::abs(eye_view.z) < kEps);
        PPR_TEST_ASSERT(std::abs(eye_view.w - 1.0f) < kEps);
    };

    PPR_UNIT_TEST (lookat_target_distance) {
        const float3 eye{0.0f, 0.0f, -5.0f};
        const float3 target{0.0f, 0.0f, 0.0f};
        const float3 up{0.0f, 1.0f, 0.0f};
        const float4x4 view = float4x4::lookat(target, eye, up);
        const float4 target_view = float4{target, 1.0f} * view;
        const float d = distance(eye, target);
        PPR_TEST_ASSERT(std::abs(target_view.x) < kEps);
        PPR_TEST_ASSERT(std::abs(target_view.y) < kEps);
        PPR_TEST_ASSERT(std::abs(target_view.z - d) < kEps);
        PPR_TEST_ASSERT(std::abs(target_view.w - 1.0f) < kEps);
    };

    PPR_UNIT_TEST (lookat_orthonormal_bis) {
        const float4x4 view = float4x4::lookat(float3{4.0f, 5.0f, 6.0f}, float3{1.0f, 2.0f, 3.0f}, float3{0.0f, 1.0f, 0.0f});
        const float4 x = view[0];
        const float4 y = view[1];
        const float4 z = view[2];
        PPR_TEST_ASSERT(std::abs(length(x) - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(length(y) - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(length(z) - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(dot(x, y)) < kEps);
        PPR_TEST_ASSERT(std::abs(dot(x, z)) < kEps);
        PPR_TEST_ASSERT(std::abs(dot(y, z)) < kEps);
    };

    PPR_UNIT_TEST (camera_viewport_size) {
        Camera cam;
        cam.updateModel(std::chrono::milliseconds{16}, CameraModel{}, testViewport(int2{1920, 1080}));
        PPR_TEST_ASSERT(cam.getSnapshot().m_viewport_size.x == 1920.0f);
        PPR_TEST_ASSERT(cam.getSnapshot().m_viewport_size.y == 1080.0f);
        PPR_TEST_ASSERT(std::abs(cam.getSnapshot().m_aspect_ratio - 1920.0f / 1080.0f) < kEps);
    };

    PPR_UNIT_TEST (camera_velocity) {
        Camera cam;
        const Viewport viewport = testViewport(int2{800, 600});
        cam.updateModel(std::chrono::seconds{1}, CameraModel{}, viewport);
        // First frame is a cut: velocity is zero.
        PPR_TEST_ASSERT(distance(cam.getCameraVelocity(), float3{zero_v}) < kEps);

        CameraModel moved{};
        moved.m_origin = float3{1.0f, 2.0f, 3.0f};
        cam.updateModel(std::chrono::seconds{1}, moved, viewport);
        PPR_TEST_ASSERT(distance(cam.getCameraVelocity(), float3{1.0f, 2.0f, 3.0f}) < kEps);
    };

    // Camera cuts are opt-in (teleport/reset only): a cut frame zeroes velocity and
    // re-baselines history so the teleport delta never leaks into later frames.
    PPR_UNIT_TEST (camera_cut_velocity) {
        Camera cam;
        const Viewport viewport = testViewport(int2{800, 600});
        cam.updateModel(std::chrono::seconds{1}, CameraModel{}, viewport);

        // Normal movement produces velocity.
        CameraModel moved{};
        moved.m_origin = float3{1.0f, 0.0f, 0.0f};
        cam.updateModel(std::chrono::seconds{1}, moved, viewport);
        PPR_TEST_ASSERT(std::abs(cam.getCameraVelocity().x - 1.0f) < kEps);

        // A cut frame zeroes velocity despite the position jump.
        cam.signalCameraCutNextFrame();
        CameraModel jumped{};
        jumped.m_origin = float3{5.0f, 0.0f, 0.0f};
        cam.updateModel(std::chrono::seconds{1}, jumped, viewport);
        PPR_TEST_ASSERT(distance(cam.getCameraVelocity(), float3{zero_v}) < kEps);

        // Tracking resumes from the post-cut baseline: no teleport spike.
        CameraModel resumed{};
        resumed.m_origin = float3{6.0f, 0.0f, 0.0f};
        cam.updateModel(std::chrono::seconds{1}, resumed, viewport);
        PPR_TEST_ASSERT(std::abs(cam.getCameraVelocity().x - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.getCameraVelocity().y) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.getCameraVelocity().z) < kEps);
    };

    PPR_UNIT_TEST (zero_to_one_frustum_near_plane) {
        const float4x4 projection = rhi::getPerspectiveMatrix(1.0f, 1.0f, 0.1f, 100.0f);
        const Frustum frustum = makeZeroToOneFrustum(projection);
        PPR_TEST_ASSERT(frustum.isVisible(Box{float3{0.0f, 0.0f, 1.0f}, 0.1f}));
        PPR_TEST_ASSERT(not frustum.isVisible(Box{float3{0.0f, 0.0f, 0.05f}, 0.01f}));
        PPR_TEST_ASSERT(not frustum.isVisible(Box{float3{10.0f, 0.0f, 1.0f}, 0.1f}));
        PPR_TEST_ASSERT(not frustum.isVisible(Box{float3{0.0f, 0.0f, 101.0f}, 0.1f}));

        const float4 near_clip = float4{float3{0.0f, 0.0f, 0.1f}, 1.0f} * projection;
        const float4 far_clip = float4{float3{0.0f, 0.0f, 100.0f}, 1.0f} * projection;
        const float4 upper_clip = float4{float3{0.0f, 1.0f, 1.0f}, 1.0f} * projection;
        PPR_TEST_ASSERT(near_clip.w > 0.0f);
        PPR_TEST_ASSERT(far_clip.w > 0.0f);
        PPR_TEST_ASSERT(std::abs(near_clip.z / near_clip.w) < kEps);
        PPR_TEST_ASSERT(std::abs(far_clip.z / far_clip.w - 1.0f) < kEps);
        PPR_TEST_ASSERT(upper_clip.y / upper_clip.w > 0.0f);
    };

    PPR_UNIT_TEST (camera_mode_accessors) {
        // NOTE: getCameraMode reflects the last committed model; the pending
        // mode lands on the next updateModel.
        Camera cam;
        const Viewport mode_viewport = testViewport(int2{800, 600});
        PPR_TEST_ASSERT(cam.getCameraMode() == ECameraProjection::perspective);
        cam.setCameraMode(ECameraProjection::orthographic);
        cam.updateModel(std::chrono::milliseconds{16}, CameraModel{}, mode_viewport);
        PPR_TEST_ASSERT(cam.getCameraMode() == ECameraProjection::orthographic);
        cam.setCameraMode(ECameraProjection::perspective);
        cam.updateModel(std::chrono::milliseconds{16}, CameraModel{}, mode_viewport);
        PPR_TEST_ASSERT(cam.getCameraMode() == ECameraProjection::perspective);
    };

    PPR_UNIT_TEST (camera_state_accessors) {
        Camera cam;
        const Viewport viewport = testViewport(int2{800, 600});
        // Default state: zero-positioned with no previous snapshot yet.
        PPR_TEST_ASSERT(distance(cam.getSnapshot().m_origin, float3{zero_v}) < kEps);
        PPR_TEST_ASSERT(not cam.getPreviousSnapshot().has_value());

        // updateModel applies to current; previous captures the pre-update state.
        CameraModel model{};
        model.m_origin = float3{1.0f, 2.0f, 3.0f};
        model.m_fov = 1.0f;
        model.m_z_near = 0.5f;
        model.m_z_far = 500.0f;
        cam.updateModel(std::chrono::milliseconds{16}, model, viewport);
        PPR_TEST_ASSERT(distance(cam.getSnapshot().m_origin, float3{1.0f, 2.0f, 3.0f}) < kEps);
        PPR_TEST_ASSERT(distance(cam.getPreviousSnapshot()->m_origin, float3{zero_v}) < kEps);
        PPR_TEST_ASSERT(cam.getSnapshot().m_viewport_size.x == 800.0f);
        PPR_TEST_ASSERT(cam.getSnapshot().m_viewport_size.y == 600.0f);

        // A second update re-baselines previous to the first snapshot.
        cam.updateModel(std::chrono::milliseconds{16}, model, viewport);
        PPR_TEST_ASSERT(distance(cam.getPreviousSnapshot()->m_origin, float3{1.0f, 2.0f, 3.0f}) < kEps);
    };

    PPR_UNIT_TEST (camera_basis_accessors) {
        Camera cam;
        CameraModel model{};
        // NOTE: normalize — rotateXYZ's raw output is not guaranteed unit
        // for every angle triple, and updateModel requires a unit basis.
        model.m_basis = normalize(Quaternion::rotateXYZ(0.0f, 1.0f, 0.0f));
        model.m_origin = float3{0.0f, 0.0f, 5.0f};
        cam.updateModel(std::chrono::milliseconds{16}, model, testViewport(int2{1920, 1080}));
        PPR_TEST_ASSERT(distance(cam.getUp(), quaternionTransform(model.m_basis, math::axis_y)) < kEps);
        PPR_TEST_ASSERT(distance(cam.getForward(), quaternionTransform(model.m_basis, math::axis_z)) < kEps);
        PPR_TEST_ASSERT(distance(cam.getRight(), quaternionTransform(model.m_basis, math::axis_x)) < kEps);
        PPR_TEST_ASSERT(distance(cam.getOrigin(), model.m_origin) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.getNearZ() - model.m_z_near) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.getFarZ() - model.m_z_far) < kEps);
    };

    PPR_UNIT_TEST (camera_inverse_accessors) {
        Camera cam;
        CameraModel model{};
        model.m_origin = float3{0.0f, 0.0f, 5.0f};
        cam.updateModel(std::chrono::milliseconds{16}, model, testViewport(int2{800, 600}));
        const CameraSnapshot &snap = cam.getSnapshot();
        PPR_TEST_ASSERT(matEq(cam.getInvertView(), inverse(snap.m_view)));
        PPR_TEST_ASSERT(matEq(cam.getInvertProjection(), inverse(snap.m_projection)));
        PPR_TEST_ASSERT(matEq(cam.getInvertViewProjection(), inverse(snap.m_view_projection)));
    };

    // W flies forward (+Z): a single 250ms press moves a quarter unit.
    PPR_UNIT_TEST (free_camera_translate_moves_origin) {
        FreeCameraController ctrl;
        InputMapping mapping{"FreeCameraMove"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const InputMessage press{InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) listener.postKeyEvent(std::chrono::milliseconds{250}, press);
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::milliseconds{250}, model);
        PPR_TEST_ASSERT(model.m_origin.z > 1e-2f);
        PPR_TEST_ASSERT(std::abs(model.m_origin.x) < kEps);
        PPR_TEST_ASSERT(std::abs(model.m_origin.y) < kEps);
        PPR_TEST_ASSERT(not model.m_has_camera_cut);
    };

    // lookAt(eye, target, up) positions the camera and faces the target.
    PPR_UNIT_TEST (free_camera_lookAt_target) {
        FreeCameraController ctrl;
        const float3 eye{0.0f, 0.0f, -5.0f};
        const float3 target{0.0f, 0.0f, 0.0f};
        ctrl.lookAt(eye, target, float3{0.0f, 1.0f, 0.0f}, true);
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        PPR_TEST_ASSERT(distance(model.m_origin, eye) < kEps);
        // eye=(0,0,-5) faces target=(0,0,0): forward is exactly +Z.
        const float3 fwd = quaternionTransform(model.m_basis, math::axis_z);
        PPR_TEST_ASSERT(std::abs(fwd.x) < 1e-3f);
        PPR_TEST_ASSERT(std::abs(fwd.y) < 1e-3f);
        PPR_TEST_ASSERT(std::abs(fwd.z - 1.0f) < 1e-3f);
        PPR_TEST_ASSERT(model.m_has_camera_cut);
    };

    // lookAt(eye, heading, pitch) applies the exact yaw-pitch rotation.
    PPR_UNIT_TEST (free_camera_lookAt_heading_pitch) {
        FreeCameraController ctrl;
        const float3 eye{1.0f, 2.0f, 3.0f};
        ctrl.lookAt(eye, 0.5f, 0.25f, true);
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        PPR_TEST_ASSERT(distance(model.m_origin, eye) < kEps);
        PPR_TEST_ASSERT(dot(model.m_basis, Quaternion::rotateXYZ(0.25f, 0.5f, 0.0f)) > 1.0f - 1e-6f);
        PPR_TEST_ASSERT(model.m_has_camera_cut);
    };

    // A teleport skips delta consumption for that frame.
    PPR_UNIT_TEST (free_camera_teleport_skips_delta) {
        FreeCameraController ctrl;
        InputMapping mapping{"FreeCameraTeleport"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const float3 eye{1.0f, 2.0f, 3.0f};
        ctrl.lookAt(eye, 0.5f, 0.25f, true);
        const InputMessage press{InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) listener.postKeyEvent(std::chrono::milliseconds{16}, press);
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        // Teleport skips delta consumption: position stays at the teleport eye.
        PPR_TEST_ASSERT(distance(model.m_origin, eye) < kEps);
        PPR_TEST_ASSERT(model.m_has_camera_cut);
    };

    // Pointer motion rotates only while the look button is held (RMB).
    PPR_UNIT_TEST (free_camera_mouse_look_gate) {
        FreeCameraController ctrl;
        ctrl.setRotationInertia(2.0f);
        ctrl.lookAt(float3{0.0f, 0.0f, -5.0f}, float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 1.0f, 0.0f}, true);
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        const float3 base_forward = quaternionTransform(model.m_basis, math::axis_z);

        InputMapping mapping{"FreeCameraMouse"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const auto mouse = [](const float x) {
            return InputMessage{
                InputKey::mouse_2d,
                InputValue{InputAxis2D{.m_absolute = float2{x, 0.0f}, .m_relative = float2{x, 0.0f}}},
                InputDeviceID{0u},
                EInputMessageEvent::axis
            };
        };

        // Without the look button held, pointer motion does not rotate.
        (void) listener.postKeyEvent(std::chrono::milliseconds{250}, mouse(1.0f));
        ctrl.updateCameraModel(std::chrono::milliseconds{250}, model);
        PPR_TEST_ASSERT(distance(quaternionTransform(model.m_basis, math::axis_z), base_forward) < 1e-6f);

        // Holding RMB gates pointer rotation.
        const InputMessage look{InputKey::right_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) listener.postKeyEvent(std::chrono::milliseconds{250}, look);
        (void) listener.postKeyEvent(std::chrono::milliseconds{250}, mouse(1.0f));
        ctrl.updateCameraModel(std::chrono::milliseconds{250}, model);
        PPR_TEST_ASSERT(distance(quaternionTransform(model.m_basis, math::axis_z), base_forward) > 1e-2f);
    };

    // Q/E oppose: Q yaws one way, E yaws the other, with equal magnitude.
    // Pins the -float2(1,0)/+float2(1,0) rotation wiring (both keys fed the
    // same +speed before, so Q and E rotated identically).
    PPR_UNIT_TEST (free_camera_qe_oppose) {
        const auto yawForward = [](const InputKey key) {
            FreeCameraController ctrl;
            InputMapping mapping{"FreeCameraQEOppose"};
            ctrl.provideInputActionKeyMappings(mapping);
            InputListener listener;
            listener.addInputMapping(SharedInputMapping{&mapping}, 0);
            const InputMessage press{key, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
            (void) listener.postKeyEvent(std::chrono::milliseconds{250}, press);
            CameraModel model{};
            ctrl.updateCameraModel(std::chrono::milliseconds{250}, model);
            return quaternionTransform(model.m_basis, math::axis_z);
        };

        const float3 fwd_q = yawForward(InputKey::q);
        const float3 fwd_e = yawForward(InputKey::e);
        // Both keys yaw measurably, in opposite directions, symmetrically.
        PPR_TEST_ASSERT(std::abs(fwd_q.x) > 1e-1f);
        PPR_TEST_ASSERT(std::abs(fwd_e.x) > 1e-1f);
        PPR_TEST_ASSERT(fwd_q.x * fwd_e.x < -1e-2f);
        PPR_TEST_ASSERT(std::abs(std::abs(fwd_q.x) - std::abs(fwd_e.x)) < 1e-3f);
        PPR_TEST_ASSERT(std::abs(fwd_q.z - fwd_e.z) < 1e-3f);
        // Pure yaw: no pitch leak into Y.
        PPR_TEST_ASSERT(std::abs(fwd_q.y) < 1e-3f);
        PPR_TEST_ASSERT(std::abs(fwd_e.y) < 1e-3f);
    };

    // Heading/pitch wiring: pure-heading input yaws (moves X, not Y) and
    // pure-pitch input pitches (moves Y, not X). Pins the
    // rotateXYZ(pitch, heading, roll) argument order: swapped arguments
    // would turn E-key input into pitch and pointer-Y input into yaw.
    PPR_UNIT_TEST (free_camera_heading_pitch_wiring) {
        const auto driveKey = [](const InputKey key) {
            FreeCameraController ctrl;
            InputMapping mapping{"FreeCameraHeadingWiring"};
            ctrl.provideInputActionKeyMappings(mapping);
            InputListener listener;
            listener.addInputMapping(SharedInputMapping{&mapping}, 0);
            const InputMessage press{key, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
            (void) listener.postKeyEvent(std::chrono::milliseconds{250}, press);
            CameraModel model{};
            ctrl.updateCameraModel(std::chrono::milliseconds{250}, model);
            return quaternionTransform(model.m_basis, math::axis_z);
        };

        // E feeds absolute.x only, i.e. pure heading: yaw moves, pitch does not.
        const float3 yaw_fwd = driveKey(InputKey::e);
        PPR_TEST_ASSERT(std::abs(yaw_fwd.x) > 1e-1f);
        PPR_TEST_ASSERT(std::abs(yaw_fwd.y) < 1e-3f);

        // Pointer Y (with RMB held) feeds relative.y only, i.e. pure pitch.
        FreeCameraController pitch_ctrl;
        InputMapping pitch_mapping{"FreeCameraPitchWiring"};
        pitch_ctrl.provideInputActionKeyMappings(pitch_mapping);
        InputListener pitch_listener;
        pitch_listener.addInputMapping(SharedInputMapping{&pitch_mapping}, 0);
        const InputMessage look{InputKey::right_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) pitch_listener.postKeyEvent(std::chrono::milliseconds{250}, look);
        const InputMessage mouse{
            InputKey::mouse_2d,
            InputValue{InputAxis2D{.m_absolute = float2{0.0f, 1.0f}, .m_relative = float2{0.0f, 1.0f}}},
            InputDeviceID{0u},
            EInputMessageEvent::axis
        };
        (void) pitch_listener.postKeyEvent(std::chrono::milliseconds{250}, mouse);
        CameraModel pitch_model{};
        pitch_ctrl.updateCameraModel(std::chrono::milliseconds{250}, pitch_model);
        const float3 pitch_fwd = quaternionTransform(pitch_model.m_basis, math::axis_z);
        PPR_TEST_ASSERT(std::abs(pitch_fwd.y) > 1e-2f);
        PPR_TEST_ASSERT(std::abs(pitch_fwd.x) < 1e-3f);
    };

    // Real controller accessors return the configured values.
    PPR_UNIT_TEST (free_camera_accessors) {
        FreeCameraController ctrl;
        PPR_TEST_ASSERT(distance(ctrl.getPosition(), float3{zero_v}) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.getFov() - std::numbers::pi_v<float> / 3.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.getSpeedMultiplier() - 1.0f) < kEps);
        PPR_TEST_ASSERT(distance(ctrl.getTranslateSpeed(), float3{1.0f, 1.0f, 1.0f}) < kEps);
        PPR_TEST_ASSERT(distance(ctrl.getRotateSpeed(), float2{10.0f, 10.0f}) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.getPositionInertia() - 0.15f) < kEps);
        ctrl.setPositionInertia(1.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.getPositionInertia() - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.getRotationInertia() - 0.15f) < kEps);
        ctrl.setRotationInertia(1.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.getRotationInertia() - 1.0f) < kEps);
        PPR_TEST_ASSERT(not ctrl.hasTeleported());
        ctrl.lookAt(float3{1.0f, 2.0f, 3.0f}, 0.0f, 0.0f, true);
        PPR_TEST_ASSERT(ctrl.hasTeleported());
        PPR_TEST_ASSERT(distance(ctrl.getPosition(), float3{1.0f, 2.0f, 3.0f}) < kEps);
    };

    // Pan maps WASD to plane axes (W/S vertical, A/D horizontal) and Q/E to depth.
    PPR_UNIT_TEST (pan_camera_key_directions) {
        const auto displacement = [](const InputKey key) {
            PanCameraController ctrl;
            InputMapping mapping{"PanCameraDirections"};
            ctrl.provideInputActionKeyMappings(mapping);
            InputListener listener;
            listener.addInputMapping(SharedInputMapping{&mapping}, 0);
            const InputMessage press{key, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
            (void) listener.postKeyEvent(std::chrono::milliseconds{250}, press);
            CameraModel model{};
            ctrl.updateCameraModel(std::chrono::milliseconds{250}, model);
            return model.m_origin;
        };

        PPR_TEST_ASSERT(displacement(InputKey::w).y > 1e-2f);
        PPR_TEST_ASSERT(displacement(InputKey::s).y < -1e-2f);
        PPR_TEST_ASSERT(displacement(InputKey::a).x < -1e-2f);
        PPR_TEST_ASSERT(displacement(InputKey::d).x > 1e-2f);
        PPR_TEST_ASSERT(displacement(InputKey::q).z < -1e-2f);
        PPR_TEST_ASSERT(displacement(InputKey::e).z > 1e-2f);
        PPR_TEST_ASSERT(displacement(InputKey::gamepad_dpad_left).x < -1e-2f);
        PPR_TEST_ASSERT(displacement(InputKey::gamepad_dpad_right).x > 1e-2f);
    };

    // The pan pose stays on its plane with the plane basis.
    // NOTE: axis-aligned plane — axis inputs are exactly unit, and
    // isNormalized's default (10x float eps) covers oblique normalize() output.
    PPR_UNIT_TEST(pan_camera_parallel_plane) {
        PanCameraController ctrl;
        const float3 normal{0.0f, 0.0f, -1.0f};
        const float3 point{2.0f, -1.0f, 0.5f};
        ctrl.setParallelPlane(normal, float3{0.0f, 1.0f, 0.0f}, true);
        const float3 eye{point.x + normal.x * 7.0f + 1.0f, point.y + normal.y * 7.0f, point.z + normal.z * 7.0f - 1.0f};
        ctrl.translate(eye, true);
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::milliseconds{250}, model);
        PPR_TEST_ASSERT(distance(model.m_origin, eye) < 1e-3f);
        PPR_TEST_ASSERT(model.m_has_camera_cut);
        const float3 forward = quaternionTransform(model.m_basis, math::axis_z);
        PPR_TEST_ASSERT(std::abs(forward.x + normal.x) < 1e-3f);
        PPR_TEST_ASSERT(std::abs(forward.y + normal.y) < 1e-3f);
        PPR_TEST_ASSERT(std::abs(forward.z + normal.z) < 1e-3f);
    };

    // Orbit keeps target/radius: lookAt fixes both, setOrbitRadius re-seats the eye.
    PPR_UNIT_TEST (orbit_camera_lookAt_and_radius) {
        OrbitCameraController ctrl;
        const float3 eye{4.0f, 3.0f, -2.0f};
        const float3 target{-1.0f, 1.0f, 5.0f};
        ctrl.lookAt(eye, target, true);
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        PPR_TEST_ASSERT(distance(model.m_origin, eye) < 1e-3f);
        PPR_TEST_ASSERT(distance(ctrl.getOrbitTarget(), target) < kEps);
        PPR_TEST_ASSERT(distance(model.m_origin, target) > 2.0f);
        PPR_TEST_ASSERT(model.m_has_camera_cut);

        ctrl.setOrbitRadius(2.0f, true);
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        PPR_TEST_ASSERT(std::abs(distance(model.m_origin, target) - 2.0f) < 1e-3f);
        PPR_TEST_ASSERT(distance(ctrl.getOrbitTarget(), target) < kEps);
        PPR_TEST_ASSERT(model.m_has_camera_cut);
    };

    PPR_UNIT_TEST (math_inverse_identity) {
        const float4x4 identity{
            float4{1.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 1.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 1.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 1.0f}
        };
        PPR_TEST_ASSERT(matEq(inverse(identity), identity));
        const float4x4 scale{
            float4{2.0f, 0.0f, 3.0f, 0.0f},
            float4{0.0f, 3.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 4.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 1.0f}
        };
        PPR_TEST_ASSERT(matEq(inverse(scale) * scale, identity));
    };

    PPR_UNIT_TEST (math_inverse_involution) {
        const float4x4 m{
            float4{1.0f, 2.0f, 3.0f, 0.0f},
            float4{0.0f, 1.0f, 4.0f, 0.0f},
            float4{5.0f, 0.0f, 1.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 1.0f}
        };
        PPR_TEST_ASSERT(matEq(inverse(inverse(m)), m));
    };

    PPR_UNIT_TEST (app_camera){
        _.recurse({
            camera_model,
            camera_viewport_size,
            camera_velocity,
            camera_cut_velocity,
            zero_to_one_frustum_near_plane,
            lookat_canonical,
            lookat_eye_to_origin,
            lookat_target_distance,
            lookat_orthonormal_bis,
            math_inverse_identity,
            math_inverse_involution,
            camera_mode_accessors,
            camera_state_accessors,
            camera_basis_accessors,
            camera_inverse_accessors,
            free_camera_translate_moves_origin,
            free_camera_lookAt_target,
            free_camera_lookAt_heading_pitch,
            free_camera_teleport_skips_delta,
            free_camera_mouse_look_gate,
            free_camera_qe_oppose,
            free_camera_heading_pitch_wiring,
            free_camera_accessors,
            pan_camera_key_directions,
            pan_camera_parallel_plane,
            orbit_camera_lookAt_and_radius,
        });

    };
}
