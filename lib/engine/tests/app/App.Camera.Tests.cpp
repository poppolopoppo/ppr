module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import std;

namespace pP::tests::detail {
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

    PPR_UNIT_TEST (look_at_canonical) {
        const float4x4 view = float4x4::lookat(float3{0.0f, 0.0f, 1.0f}, float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 1.0f, 0.0f});
        const float4x4 expected{
            float4{1.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 1.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 1.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 1.0f}
        };
        PPR_TEST_ASSERT(matEq(view, expected));
    };

    PPR_UNIT_TEST (look_at_eye_to_origin) {
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

    PPR_UNIT_TEST (look_at_target_distance) {
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

    PPR_UNIT_TEST (look_at_orthonormal_bis) {
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

    PPR_UNIT_TEST (viewport_size) {
        Camera cam;
        cam.updateModel(std::chrono::milliseconds{16}, CameraModel{}, testViewport(int2{1920, 1080}));
        PPR_TEST_ASSERT(cam.getSnapshot().m_viewport_size.x == 1920.0f);
        PPR_TEST_ASSERT(cam.getSnapshot().m_viewport_size.y == 1080.0f);
        PPR_TEST_ASSERT(std::abs(cam.getSnapshot().m_aspect_ratio - 1920.0f / 1080.0f) < kEps);
    };

    PPR_UNIT_TEST (velocity) {
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
    PPR_UNIT_TEST (cut_velocity) {
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

    // Phase 1: Camera-constructed frustum pins the unjittered VP remapped to
    // D3D [0,1] depth (not adapter isolation only).
    PPR_UNIT_TEST (frustum_from_update_model) {
        Camera cam;
        CameraModel model{};
        model.m_z_near = 0.1f;
        model.m_z_far = 100.0f;
        cam.updateModel(std::chrono::milliseconds{16}, model, testViewport(int2{800, 600}));

        const CameraSnapshot &snap = cam.getSnapshot();
        const Frustum expected = makeZeroToOneFrustum(snap.m_view_projection);
        for (std::size_t i = 0; i < 6; ++i) {
            PPR_TEST_ASSERT(distance(snap.m_frustum.clip[i], expected.clip[i]) < kEps);
        }

        const Frustum &frustum = cam.getFrustum();
        PPR_TEST_ASSERT(frustum.isVisible(Box{float3{0.0f, 0.0f, 1.0f}, 0.1f}));
        PPR_TEST_ASSERT(not frustum.isVisible(Box{float3{0.0f, 0.0f, 0.05f}, 0.01f}));
        PPR_TEST_ASSERT(not frustum.isVisible(Box{float3{10.0f, 0.0f, 1.0f}, 0.1f}));
        PPR_TEST_ASSERT(not frustum.isVisible(Box{float3{0.0f, 0.0f, 101.0f}, 0.1f}));

        // Jitter must not move the frustum: it stays pinned to unjittered VP.
        const float3 ray_origin_before = cam.getRayFrustum().origin;
        const float3 ray_point_before = cam.getRayFrustum().point[0];
        std::array<float2, 2> samples{float2{2.0f, 1.0f}, float2{-2.0f, -1.0f}};
        cam.setJitterSamples(CameraJitterSamples{std::span<const float2>(samples)});
        cam.updateModel(std::chrono::milliseconds{16}, model, testViewport(int2{800, 600}));
        const Frustum rejittered = makeZeroToOneFrustum(cam.getSnapshot().m_view_projection);
        for (std::size_t i = 0; i < 6; ++i) {
            PPR_TEST_ASSERT(distance(cam.getFrustum().clip[i], rejittered.clip[i]) < kEps);
        }
        PPR_TEST_ASSERT(cam.getFrustum().isVisible(Box{float3{0.0f, 0.0f, 1.0f}, 0.1f}));
        // Ray frustum shares the same unjittered-VP source, so it stays stable under jitter.
        PPR_TEST_ASSERT(distance(cam.getRayFrustum().origin, ray_origin_before) < kEps);
        PPR_TEST_ASSERT(distance(cam.getRayFrustum().point[0], ray_point_before) < kEps);
    };

    // Phase 1: engaged-but-empty jitter behaves as no jitter â€” no crash
    // (never revision % 0), cut signaled, jitter phase sane.
    PPR_UNIT_TEST (empty_jitter) {
        Camera cam;
        const Viewport viewport = testViewport(int2{800, 600});
        cam.updateModel(std::chrono::seconds{1}, CameraModel{}, viewport);

        std::array<float2, 2> samples{float2{1.0f, 0.0f}, float2{-1.0f, 0.0f}};
        cam.setJitterSamples(CameraJitterSamples{std::span<const float2>(samples)});
        cam.updateModel(std::chrono::seconds{1}, CameraModel{}, viewport);
        PPR_TEST_ASSERT(std::abs(cam.getJitter().x) > kEps);

        cam.setJitterSamples(CameraJitterSamples{std::span<const float2>{}});
        cam.updateModel(std::chrono::seconds{1}, CameraModel{}, viewport);

        const CameraSnapshot &snap = cam.getSnapshot();
        PPR_TEST_ASSERT(std::abs(snap.m_jitter.x) < kEps);
        PPR_TEST_ASSERT(std::abs(snap.m_jitter.y) < kEps);
        PPR_TEST_ASSERT(snap.m_has_camera_cut);
        PPR_TEST_ASSERT(cam.getRevision() == 0u);
    };

    PPR_UNIT_TEST (mode_accessors) {
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

    PPR_UNIT_TEST (state_accessors) {
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

    PPR_UNIT_TEST (basis_accessors) {
        Camera cam;
        CameraModel model{};
        // NOTE: normalize â€” rotateXYZ's raw output is not guaranteed unit
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

    PPR_UNIT_TEST (inverse_accessors) {
        Camera cam;
        CameraModel model{};
        model.m_origin = float3{0.0f, 0.0f, 5.0f};
        cam.updateModel(std::chrono::milliseconds{16}, model, testViewport(int2{800, 600}));
        const CameraSnapshot &snap = cam.getSnapshot();
        PPR_TEST_ASSERT(matEq(cam.getInvertView(), inverse(snap.m_view)));
        PPR_TEST_ASSERT(matEq(cam.getInvertProjection(), inverse(snap.m_projection)));
        PPR_TEST_ASSERT(matEq(cam.getInvertViewProjection(), inverse(snap.m_view_projection)));
    };

    // W flies forward (+Z): a single 250ms press moves ~0.65 units at the 3 m/s base rate.
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

    PPR_UNIT_TEST (free_camera_held_key_rate) {
        FreeCameraController ctrl;
        ctrl.setPositionInertia(100000.0f);
        InputMapping mapping{"FreeCameraHeldKey"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const InputMessage press{InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, press);

        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        const float first_z = model.m_origin.z;
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        PPR_TEST_ASSERT(std::abs(first_z - 3.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(model.m_origin.z - 6.0f) < kEps);
    };

    PPR_UNIT_TEST (free_camera_partial_key_release) {
        FreeCameraController ctrl;
        ctrl.setPositionInertia(100000.0f);
        InputMapping mapping{"FreeCameraPartialRelease"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const InputMessage forward{InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        const InputMessage right{InputKey::d, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, forward);
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, right);

        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        const InputMessage release_right{InputKey::d, InputValue{InputDigital{false}}, InputDeviceID{0u}, EInputMessageEvent::released};
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, release_right);
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        PPR_TEST_ASSERT(std::abs(model.m_origin.x - 3.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(model.m_origin.z - 6.0f) < kEps);
    };

    PPR_UNIT_TEST (free_camera_input_reset) {
        FreeCameraController ctrl;
        ctrl.setPositionInertia(100000.0f);
        ctrl.setRotationInertia(100000.0f);
        InputMapping mapping{"FreeCameraInputReset"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const InputMessage forward{InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        const InputMessage look{InputKey::right_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, forward);
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, look);

        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        const float3 origin_before_reset = model.m_origin;
        const Quaternion basis_before_reset = model.m_basis;
        ctrl.resetInputState();

        const InputMessage mouse{
            InputKey::mouse_2d, InputValue{InputAxis2D{.m_absolute = float2{1.0f, 0.0f}, .m_relative = float2{1.0f, 0.0f}}}, InputDeviceID{0u},
            EInputMessageEvent::axis
        };
        // Reset cleared held-key rates and the look gate, so pointer motion
        // must neither drift the origin nor rotate the basis.
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, mouse);
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        PPR_TEST_ASSERT(distance(model.m_origin, origin_before_reset) < kEps);
        PPR_TEST_ASSERT(dot(model.m_basis, basis_before_reset) > 1.0f - kEps);
    };

    // Pointer yaw needs the look gate: identical mouse motion leaves the basis
    // untouched without look held, and yaws once RMB (the look key) is held.
    // Quantitative pin: 100px * 0.0025 sensitivity * 1.8 heading ~= 0.45 rad.
    PPR_UNIT_TEST (free_camera_look_press_gates_pointer_yaw) {
        FreeCameraController ctrl;
        ctrl.setRotationInertia(100000.0f);
        InputMapping mapping{"FreeCameraLookGatedYaw"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const InputMessage mouse{
            InputKey::mouse_2d,
            InputValue{InputAxis2D{.m_absolute = float2{100.0f, 0.0f}, .m_relative = float2{100.0f, 0.0f}}},
            InputDeviceID{0u},
            EInputMessageEvent::axis
        };
        const InputMessage look{InputKey::right_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        CameraModel model{};
        // Disengaged: no look held, motion ignored.
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, mouse);
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        const Quaternion disengaged_basis = model.m_basis;

        // Engaged: RMB held opens the look gate, same motion yaws.
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, look);
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, mouse);
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        const float3 mouse_fwd = quaternionTransform(model.m_basis, math::axis_z);
        const float mouse_angle = std::acos(std::clamp(mouse_fwd.z, -1.0f, 1.0f));
        PPR_TEST_ASSERT(dot(model.m_basis, disengaged_basis) < 1.0f - kEps);
        PPR_TEST_ASSERT(std::abs(mouse_angle - 0.45f) < 5e-2f);
    };

    PPR_UNIT_TEST (camera_mouse_wheel_impulses) {
        const auto mouse_rotation = [](const TimeSpan dt) {
            FreeCameraController ctrl;
            ctrl.setRotationInertia(100000.0f);
            InputMapping mapping{"FreeCameraMouseImpulse"};
            ctrl.provideInputActionKeyMappings(mapping);
            InputListener listener;
            listener.addInputMapping(SharedInputMapping{&mapping}, 0);
            const InputMessage look{InputKey::right_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
            const InputMessage mouse{
                InputKey::mouse_2d, InputValue{InputAxis2D{.m_absolute = float2{1.0f, 0.0f}, .m_relative = float2{1.0f, 0.0f}}}, InputDeviceID{0u},
                EInputMessageEvent::axis
            };
            (void) listener.postKeyEvent(std::chrono::milliseconds{1}, look);
            (void) listener.postKeyEvent(std::chrono::milliseconds{1}, mouse);
            CameraModel model{};
            ctrl.updateCameraModel(dt, model);
            const Quaternion first_rotation = model.m_basis;
            ctrl.updateCameraModel(dt, model);
            PPR_TEST_ASSERT(dot(first_rotation, model.m_basis) > 1.0f - kEps);
            return first_rotation;
        };

        PPR_TEST_ASSERT(dot(mouse_rotation(std::chrono::milliseconds{1}), mouse_rotation(std::chrono::milliseconds{100})) > 1.0f - kEps);

        const auto wheel_translation = [](const TimeSpan dt) {
            PanCameraController ctrl;
            ctrl.setPositionInertia(100000.0f);
            InputMapping mapping{"PanCameraWheelImpulse"};
            ctrl.provideInputActionKeyMappings(mapping);
            InputListener listener;
            listener.addInputMapping(SharedInputMapping{&mapping}, 0);
            const InputMessage wheel{
                InputKey::mouse_wheel_axis_y, InputValue{InputAxis1D{.m_absolute = 1.0f, .m_relative = 1.0f}}, InputDeviceID{0u}, EInputMessageEvent::axis
            };
            (void) listener.postKeyEvent(std::chrono::milliseconds{1}, wheel);
            CameraModel model{};
            ctrl.updateCameraModel(dt, model);
            const float first_z = model.m_origin.z;
            ctrl.updateCameraModel(dt, model);
            PPR_TEST_ASSERT(std::abs(model.m_origin.z - first_z) < kEps);
            return first_z;
        };

        PPR_TEST_ASSERT(std::abs(wheel_translation(std::chrono::milliseconds{1}) - wheel_translation(std::chrono::milliseconds{100})) < kEps);
    };

    PPR_UNIT_TEST (free_camera_gamepad_rate) {
        FreeCameraController ctrl;
        ctrl.setPositionInertia(100000.0f);
        InputMapping mapping{"FreeCameraGamepadRate"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        InputContext context;
        context.addInputListener(safe_ptr{&listener}, 0);
        GamepadDevice device{InputDeviceID{0u}};
        const TimeSpan dt{std::chrono::seconds{1}};
        device.postGamepadAxis2DMoved(dt, context, EGamepadAxis::left_stick, float2{0.0f, 1.0f});

        CameraModel model{};
        ctrl.updateCameraModel(dt, model);
        ctrl.updateCameraModel(dt, model);
        PPR_TEST_ASSERT(std::abs(model.m_origin.z - 3.6f) < kEps);

        device.postGamepadAxis2DMoved(dt, context, EGamepadAxis::left_stick, float2{0.0f, device.m_left_stick.m_dead_zone / 2.0f});
        ctrl.updateCameraModel(dt, model);
        PPR_TEST_ASSERT(std::abs(model.m_origin.z - 3.6f) < kEps);

        device.postGamepadAxis2DMoved(dt, context, EGamepadAxis::left_stick, float2{0.0f, device.m_left_stick.m_dead_zone / 2.0f});
        ctrl.updateCameraModel(dt, model);
        PPR_TEST_ASSERT(std::abs(model.m_origin.z - 3.6f) < kEps);
    };

    // Focus/device-disconnect path: resetInputState clears retained per-key
    // rates and transient impulses, so held keys stop moving the camera.
    PPR_UNIT_TEST (free_camera_reset_clears_held_key) {
        FreeCameraController ctrl;
        ctrl.setPositionInertia(100000.0f);
        InputMapping mapping{"FreeCameraResetHeldKey"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const InputMessage press{InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, press);

        // Reset before any update: retained rate is dropped, no movement.
        ctrl.resetInputState();
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        PPR_TEST_ASSERT(distance(model.m_origin, float3{zero_v}) < kEps);

        // Press again, move once, reset: second frame must not drift.
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, press);
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        PPR_TEST_ASSERT(std::abs(model.m_origin.z - 3.0f) < kEps);
        ctrl.resetInputState();
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        PPR_TEST_ASSERT(std::abs(model.m_origin.z - 3.0f) < kEps);
    };

    // Wheel FOV is a transient relative impulse: one frame applies, the next
    // frame without new events does not.
    PPR_UNIT_TEST (free_camera_fov_wheel_transient) {
        FreeCameraController ctrl;
        ctrl.setPositionInertia(100000.0f);
        InputMapping mapping{"FreeCameraFovWheel"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const InputMessage wheel{
            InputKey::mouse_wheel_axis_y, InputValue{InputAxis1D{.m_absolute = 1.0f, .m_relative = 1.0f}}, InputDeviceID{0u},
            EInputMessageEvent::axis
        };
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, wheel);
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        const float first_fov = model.m_fov;
        PPR_TEST_ASSERT(std::abs(first_fov - std::numbers::pi_v<float> / 3.0f) > 1e-6f);
        ctrl.updateCameraModel(std::chrono::milliseconds{16}, model);
        PPR_TEST_ASSERT(std::abs(model.m_fov - first_fov) < kEps);
    };

    // lookAt(eye, target, up) positions the camera and faces the target.
    PPR_UNIT_TEST (free_camera_look_at_target) {
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
    PPR_UNIT_TEST (free_camera_look_at_heading_pitch) {
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
        (void) listener.postKeyEvent(std::chrono::milliseconds{250}, mouse(100.0f));
        ctrl.updateCameraModel(std::chrono::milliseconds{250}, model);
        PPR_TEST_ASSERT(distance(quaternionTransform(model.m_basis, math::axis_z), base_forward) < 1e-6f);

        // Holding RMB gates pointer rotation.
        const InputMessage look{InputKey::right_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) listener.postKeyEvent(std::chrono::milliseconds{250}, look);
        (void) listener.postKeyEvent(std::chrono::milliseconds{250}, mouse(100.0f));
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
            InputValue{InputAxis2D{.m_absolute = float2{0.0f, 100.0f}, .m_relative = float2{0.0f, 100.0f}}},
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
        PPR_TEST_ASSERT(distance(ctrl.getTranslateSpeed(), float3{3.0f, 3.0f, 3.0f}) < kEps);
        PPR_TEST_ASSERT(distance(ctrl.getRotateSpeed(), float2{1.8f, 1.2f}) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.getPositionInertia() - 8.0f) < kEps);
        ctrl.setPositionInertia(1.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.getPositionInertia() - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.getRotationInertia() - 8.0f) < kEps);
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
    // NOTE: axis-aligned plane â€” axis inputs are exactly unit, and
    // isNormalized's default (10x float eps) covers oblique normalize() output.
    PPR_UNIT_TEST (pan_camera_parallel_plane) {
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
    PPR_UNIT_TEST (orbit_camera_look_at_and_radius) {
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

    // Phase 2: primed held input is frame-partition invariant. Equal wall time
    // at 30/60/120Hz converges within tolerance, and motion is meaningful
    // (the old pow(dt, 1/s) contract stalled near zero post-priming).
    PPR_UNIT_TEST (free_camera_primed_held_key_partition_invariant) {
        const auto run_at = [](const int steps, const TimeSpan step) {
            FreeCameraController ctrl;
            InputMapping mapping{"FreeCameraPrimedPartition"};
            ctrl.provideInputActionKeyMappings(mapping);
            InputListener listener;
            listener.addInputMapping(SharedInputMapping{&mapping}, 0);
            CameraModel model{};
            ctrl.updateCameraModel(std::chrono::milliseconds{16}, model); // prime filters
            const InputMessage press{InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
            (void) listener.postKeyEvent(std::chrono::milliseconds{1}, press);
            for (int i = 0; i < steps; ++i) {
                ctrl.updateCameraModel(step, model);
            }
            return model.m_origin.z;
        };

        const float at30 = run_at(30, TimeSpan{std::chrono::microseconds{33333}});
        const float at60 = run_at(60, TimeSpan{std::chrono::microseconds{16667}});
        const float at120 = run_at(120, TimeSpan{std::chrono::microseconds{8333}});
        // Meaningful motion after 1s of held input (not stalled near zero).
        PPR_TEST_ASSERT(at30 > 0.5f);
        PPR_TEST_ASSERT(at60 > 0.5f);
        PPR_TEST_ASSERT(at120 > 0.5f);
        // Equal wall time converges regardless of partitioning.
        PPR_TEST_ASSERT(std::abs(at30 - at60) < 5e-2f);
        PPR_TEST_ASSERT(std::abs(at60 - at120) < 5e-2f);
        PPR_TEST_ASSERT(std::abs(at30 - at120) < 5e-2f);
    };

    // Retune pin: E-key yaw runs at ~1.8 rad/s and pointer gain is
    // ~0.0045 rad/px heading (0.0025 px sensitivity x 1.8 rad/s).
    PPR_UNIT_TEST (free_camera_retuned_rates) {
        FreeCameraController ctrl;
        PPR_TEST_ASSERT(distance(ctrl.getRotateSpeed(), float2{1.8f, 1.2f}) < kEps);
        PPR_TEST_ASSERT(distance(ctrl.m_mouse_sensitivity, float2{0.0025f, 0.0025f}) < 1e-6f);
        PPR_TEST_ASSERT(std::abs(ctrl.m_mouse_sensitivity.x * ctrl.getRotateSpeed().x - 0.0045f) < 1e-6f);

        // Keyboard yaw: holding E for 1s (inertia pinned) yaws ~1.8 rad.
        ctrl.setRotationInertia(100000.0f);
        InputMapping mapping{"FreeCameraRetunedYaw"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        const InputMessage press{InputKey::e, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        (void) listener.postKeyEvent(std::chrono::milliseconds{1}, press);
        CameraModel model{};
        ctrl.updateCameraModel(std::chrono::seconds{1}, model);
        const float3 yawed = quaternionTransform(model.m_basis, math::axis_z);
        const float yaw_angle = std::acos(std::clamp(yawed.z, -1.0f, 1.0f));
        PPR_TEST_ASSERT(std::abs(yaw_angle - 1.8f) < 5e-2f);

        // Mouse gain: 100px with RMB held yaws ~0.45 rad (100 x 0.0045).
        FreeCameraController mouse_ctrl;
        mouse_ctrl.setRotationInertia(100000.0f);
        InputMapping mouse_mapping{"FreeCameraRetunedMouse"};
        mouse_ctrl.provideInputActionKeyMappings(mouse_mapping);
        InputListener mouse_listener;
        mouse_listener.addInputMapping(SharedInputMapping{&mouse_mapping}, 0);
        const InputMessage look{InputKey::right_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        const InputMessage mouse{
            InputKey::mouse_2d,
            InputValue{InputAxis2D{.m_absolute = float2{100.0f, 0.0f}, .m_relative = float2{100.0f, 0.0f}}},
            InputDeviceID{0u},
            EInputMessageEvent::axis
        };
        (void) mouse_listener.postKeyEvent(std::chrono::milliseconds{1}, look);
        (void) mouse_listener.postKeyEvent(std::chrono::milliseconds{1}, mouse);
        CameraModel mouse_model{};
        mouse_ctrl.updateCameraModel(std::chrono::milliseconds{16}, mouse_model);
        const float3 mouse_fwd = quaternionTransform(mouse_model.m_basis, math::axis_z);
        const float mouse_angle = std::acos(std::clamp(mouse_fwd.z, -1.0f, 1.0f));
        PPR_TEST_ASSERT(std::abs(mouse_angle - 0.45f) < 5e-2f);
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
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest camera = UnitTest::Named("camera") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::camera_model,
            detail::viewport_size,
            detail::velocity,
            detail::cut_velocity,
            detail::frustum_from_update_model,
            detail::empty_jitter,
            detail::zero_to_one_frustum_near_plane,
            detail::look_at_canonical,
            detail::look_at_eye_to_origin,
            detail::look_at_target_distance,
            detail::look_at_orthonormal_bis,
            detail::math_inverse_identity,
            detail::math_inverse_involution,
            detail::mode_accessors,
            detail::state_accessors,
            detail::basis_accessors,
            detail::inverse_accessors,
            detail::free_camera_translate_moves_origin,
            detail::free_camera_held_key_rate,
            detail::free_camera_partial_key_release,
            detail::free_camera_input_reset,
            detail::free_camera_look_press_gates_pointer_yaw,
            detail::camera_mouse_wheel_impulses,
            detail::free_camera_gamepad_rate,
            detail::free_camera_reset_clears_held_key,
            detail::free_camera_fov_wheel_transient,
            detail::free_camera_look_at_target,
            detail::free_camera_look_at_heading_pitch,
            detail::free_camera_teleport_skips_delta,
            detail::free_camera_mouse_look_gate,
            detail::free_camera_qe_oppose,
            detail::free_camera_heading_pitch_wiring,
            detail::free_camera_accessors,
            detail::free_camera_retuned_rates,
            detail::pan_camera_key_directions,
            detail::pan_camera_parallel_plane,
            detail::orbit_camera_look_at_and_radius,
            detail::free_camera_primed_held_key_partition_invariant,
        });
    };
} // namespace pP::tests
