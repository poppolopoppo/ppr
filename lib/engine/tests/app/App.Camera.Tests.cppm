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

    struct StubInputService : IInputService {
        KeyboardState m_keyboard{};
        MouseState m_mouse{};
        GamepadState m_gamepad{};
        SharedInputListener m_listener{};

        [[nodiscard]] const KeyboardState &getKeyboard() const noexcept override { return m_keyboard; }
        [[nodiscard]] const MouseState &getMouse() const noexcept override { return m_mouse; }
        [[nodiscard]] const GamepadState &getGamepad(int) const noexcept override { return m_gamepad; }
        [[nodiscard]] SharedInputDevice getInputDevice(const InputDeviceID &) const noexcept override { return SharedInputDevice{}; }
        [[nodiscard]] std::error_code enumerateInputDevices(Collector<SharedInputDevice>) const noexcept override { return default_value_v; }
        [[nodiscard]] std::error_code supportedInputKeys(Collector<InputKey>) const override { return default_value_v; }
        [[nodiscard]] std::error_code postInputMessages(TimeSpan) override { return default_value_v; }
        void resetInputState() noexcept override {}
        [[nodiscard]] bool hasInputListener(const InputListener &) const noexcept override { return false; }
        void pushInputListener(SharedInputListener listener) override { m_listener = listener; }
        bool popInputListener(const InputListener &) override { m_listener = nullptr; return true; }
        [[nodiscard]] bool hasGlobalInputMapping(const InputMapping &) const noexcept override { return false; }
        void addGlobalInputMapping(SharedInputMapping, int) override {}
        bool removeGlobalInputMapping(const InputMapping &) override { return false; }
        [[nodiscard]] DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event) override { return {}; }
        [[nodiscard]] DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event) override { return {}; }
        [[nodiscard]] TriggerCallback::Handle whenActionStarted(TriggerCallback::Event) override { return {}; }
        [[nodiscard]] TriggerCallback::Handle whenActionTriggered(TriggerCallback::Event) override { return {}; }
        [[nodiscard]] TriggerCallback::Handle whenActionCompleted(TriggerCallback::Event) override { return {}; }
        [[nodiscard]] UnhandledKeyCallback::Handle whenUnhandledKey(UnhandledKeyCallback::Event) override { return {}; }
        [[nodiscard]] UpdateCallback::Handle whenBeforeUpdated(UpdateCallback::Event) override { return {}; }
        [[nodiscard]] UpdateCallback::Handle whenAfterUpdated(UpdateCallback::Event) override { return {}; }
    };

    PPR_UNIT_TEST(camera_model) {
        Camera cam;
        const float4x4 view = makeLookAtMatrix(float3{0.0f, 0.0f, 5.0f}, float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 1.0f, 0.0f});
        const float4x4 proj = rhi::getPerspectiveMatrix(rhi::DeviceType::D3D12, 1.0f, 1.0f, 0.1f, 100.0f);
        cam.setView(view);
        cam.setProjection(proj);
        PPR_TEST_ASSERT(matEq(cam.view(), view));
        PPR_TEST_ASSERT(matEq(cam.projection(), proj));
        PPR_TEST_ASSERT(matEq(cam.viewProjection(), view * proj));
        PPR_TEST_ASSERT(matEq(cam.inverseViewProjection(), inverse(view * proj)));
        cam.setPosition(float3{1.0f, 2.0f, 3.0f});
        cam.update(TimeSpan{});
        PPR_TEST_ASSERT(std::abs(cam.position().x - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.position().y - 2.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.position().z - 3.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().x) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().y) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().z) < kEps);
    };

    PPR_UNIT_TEST(camera_service_init) {
        StubInputService input;
        CameraService svc;
        std::error_code ec = svc.initialize(input);
        PPR_TEST_ASSERT(!ec);
        PPR_TEST_ASSERT(not matEq(svc.camera().projection(), float4x4{}));
        svc.update(std::chrono::milliseconds{16});
        auto ctrl = std::make_unique<PanCameraController>();
        svc.setController(std::move(ctrl));
    };

    PPR_UNIT_TEST(replay_round_trip) {
        InputReplay r;
        r.setMode(EInputReplayMode::record);
        r.injectKey(InputKey::w, true);
        r.injectCursorDelta(float2{1.0f, 2.0f});
        r.injectWheel(3.0f);
        r.injectGamepadStick(0, float2{0.5f, 0.5f});
        r.injectGamepadButton(EGamepadButton::button0, true);
        r.injectGamepadTrigger(1, 0.25f);
        r.stopRecording();
        auto rec = r.recording();
        PPR_TEST_ASSERT(rec.size() == 6u);
        InputReplay r2;
        r2.loadRecording(rec);
        r2.setMode(EInputReplayMode::replay);
        int count = 0;
        InputListener listener;
        listener.setRawKeyCallback([&count](const InputMessage &) noexcept { ++count; });
        r2.pushInputListener(safe_ptr<InputListener>{&listener});
        std::ignore = r2.postInputMessages(TimeSpan{});
        PPR_TEST_ASSERT(count == 6);
        std::ignore = r2.popInputListener(listener);
    };

    PPR_UNIT_TEST(free_camera_activate) {
        StubInputService input;
        Camera cam;
        FreeCameraController ctrl;
        ctrl.activate(input, cam);
        // Speed up position convergence so a single frame moves the camera measurably
        // (the default 0.15 inertia converges at ~1e-12/frame).
        ctrl.setPositionInertia(2.0f);
        CameraModel model = cam.currentState().model;
        ctrl.updateCamera(std::chrono::milliseconds{16}, model);
        cam.updateModel(model, int2{1920, 1080});

        InputMapping mapping{"FreeCameraTest"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addMapping(SharedInputMapping{&mapping}, 0);

        const float3 initial = cam.position();
        const InputMessage msg{
            InputKey::w,
            InputValue{InputDigital{true}},
            TimeSpan{},
            InputDeviceID{0u},
            EInputMessageEvent::pressed};
        (void)listener.postKeyEvent(msg);
        model = cam.currentState().model;
        ctrl.updateCamera(std::chrono::milliseconds{16}, model);
        cam.updateModel(model, int2{1920, 1080});
        PPR_TEST_ASSERT(std::isfinite(cam.position().x));
        PPR_TEST_ASSERT(distance(cam.position(), initial) > kEps);
        // W moves the camera forward. At identity rotation, forward = +Z (CameraModel default).
        PPR_TEST_ASSERT(cam.position().z > initial.z);
        ctrl.deactivate();
    };

    PPR_UNIT_TEST(lookat_canonical) {
        const float4x4 view = makeLookAtMatrix(float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 0.0f, -1.0f}, float3{0.0f, 1.0f, 0.0f});
        const float4x4 expected{
            float4{ 1.0f, 0.0f, 0.0f, 0.0f},
            float4{ 0.0f, 1.0f, 0.0f, 0.0f},
            float4{ 0.0f, 0.0f, 1.0f, 0.0f},
            float4{ 0.0f, 0.0f, 0.0f, 1.0f}};
        PPR_TEST_ASSERT(matEq(view, expected));
    };

    PPR_UNIT_TEST(lookat_eye_to_origin) {
        const float3 eye{1.0f, 2.0f, 3.0f};
        const float3 target{4.0f, 5.0f, 6.0f};
        const float3 up{0.0f, 1.0f, 0.0f};
        const float4x4 view = makeLookAtMatrix(eye, target, up);
        const float4 eye_view = float4{eye, 1.0f} * view;
        PPR_TEST_ASSERT(std::abs(eye_view.x) < kEps);
        PPR_TEST_ASSERT(std::abs(eye_view.y) < kEps);
        PPR_TEST_ASSERT(std::abs(eye_view.z) < kEps);
        PPR_TEST_ASSERT(std::abs(eye_view.w - 1.0f) < kEps);
    };

    PPR_UNIT_TEST(lookat_target_distance) {
        const float3 eye{0.0f, 0.0f, 5.0f};
        const float3 target{0.0f, 0.0f, 0.0f};
        const float3 up{0.0f, 1.0f, 0.0f};
        const float4x4 view = makeLookAtMatrix(eye, target, up);
        const float4 target_view = float4{target, 1.0f} * view;
        const float d = distance(eye, target);
        PPR_TEST_ASSERT(std::abs(target_view.x) < kEps);
        PPR_TEST_ASSERT(std::abs(target_view.y) < kEps);
        PPR_TEST_ASSERT(std::abs(target_view.z + d) < kEps);
        PPR_TEST_ASSERT(std::abs(target_view.w - 1.0f) < kEps);
    };

    PPR_UNIT_TEST(lookat_orthonormal_bis) {
        const float4x4 view = makeLookAtMatrix(float3{1.0f, 2.0f, 3.0f}, float3{4.0f, 5.0f, 6.0f}, float3{0.0f, 1.0f, 0.0f});
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

    PPR_UNIT_TEST(camera_viewport_size) {
        Camera cam;
        cam.setViewportSize(int2{1920, 1080});
        PPR_TEST_ASSERT(cam.viewportSize().x == 1920.0f);
        PPR_TEST_ASSERT(cam.viewportSize().y == 1080.0f);
        cam.setViewportSize(int2{0, 0});
        PPR_TEST_ASSERT(cam.viewportSize().x == 0.0f);
        PPR_TEST_ASSERT(cam.viewportSize().y == 0.0f);
    };

    PPR_UNIT_TEST(camera_service_projection) {
        StubInputService input;
        CameraService svc;
        PPR_TEST_ASSERT(!svc.initialize(input));
        svc.setDeviceType(rhi::DeviceType::Vulkan);
        svc.setPerspectiveProjection(1.0f, 1.5f, 0.5f, 200.0f);
        PPR_TEST_ASSERT(not matEq(svc.camera().projection(), float4x4{}));
        svc.setOrthoProjection(20.0f, 20.0f);
        PPR_TEST_ASSERT(not matEq(svc.camera().projection(), float4x4{}));
        svc.setViewportSize(int2{800, 600});
        PPR_TEST_ASSERT(svc.camera().viewportSize().x == 800.0f);
        PPR_TEST_ASSERT(svc.camera().viewportSize().y == 600.0f);
        svc.update(std::chrono::milliseconds{16});
        PPR_TEST_ASSERT(std::isfinite(svc.camera().position().x));
        svc.deactivateController();
    };

    PPR_UNIT_TEST(pan_camera_controller) {
        StubInputService input;
        Camera cam;
        PanCameraController ctrl;
        ctrl.activate(input, cam);
        PPR_TEST_ASSERT(not matEq(cam.projection(), float4x4{}));
        CameraModel model = cam.currentState().model;
        ctrl.updateCamera(std::chrono::milliseconds{16}, model);

        InputMapping mapping{"PanCameraTest"};
        ctrl.provideInputActionKeyMappings(mapping);
        InputListener listener;
        listener.addMapping(SharedInputMapping{&mapping}, 0);

        const float3 initial = cam.position();
        const InputMessage msg{
            InputKey::w,
            InputValue{InputDigital{true}},
            TimeSpan{},
            InputDeviceID{0u},
            EInputMessageEvent::pressed};
        (void)listener.postKeyEvent(msg);
        model = cam.currentState().model;
        ctrl.updateCamera(std::chrono::milliseconds{16}, model);
        PPR_TEST_ASSERT(std::isfinite(cam.position().x));
        PPR_TEST_ASSERT(distance(cam.position(), initial) > kEps);
        // W should move the camera forward (toward the look target). At yaw=0, pitch=0,
        // forward = (0,0,-1), so W moves the eye in -Z direction.
        PPR_TEST_ASSERT(cam.position().z < initial.z);
        ctrl.deactivate();
    };

    PPR_UNIT_TEST(replay_recording_control) {
        InputReplay r;
        r.setMode(EInputReplayMode::record);
        r.startRecording();
        r.injectKey(InputKey::w, true);
        r.injectKey(InputKey::s, false);
        PPR_TEST_ASSERT(r.recording().size() == 2u);
        r.stopRecording();
        r.clearRecording();
        PPR_TEST_ASSERT(r.recording().empty());
    };

    PPR_UNIT_TEST(replay_injection) {
        InputReplay r;
        r.setMode(EInputReplayMode::replay);
        int count = 0;
        InputListener listener;
        listener.setRawKeyCallback([&count](const InputMessage &) noexcept { ++count; });
        r.pushInputListener(safe_ptr<InputListener>{&listener});
        r.injectKey(InputKey::w, true);
        r.injectCursorDelta(float2{1.0f, 2.0f});
        r.injectWheel(3.0f);
        r.injectGamepadStick(0, float2{0.5f, 0.5f});
        r.injectGamepadButton(EGamepadButton::button0, true);
        r.injectGamepadTrigger(1, 0.25f);
        PPR_TEST_ASSERT(r.recording().empty());
        std::ignore = r.postInputMessages(TimeSpan{});
        PPR_TEST_ASSERT(count == 6);
        std::ignore = r.popInputListener(listener);
    };

    PPR_UNIT_TEST(replay_decorator) {
        StubInputService parent;
        InputReplay r;
        r.setParent(safe_ptr<IInputService>{&parent});
        r.setMode(EInputReplayMode::record);
        PPR_TEST_ASSERT(parent.m_listener != nullptr);
        r.detachParent();
        PPR_TEST_ASSERT(parent.m_listener == nullptr);
    };

    PPR_UNIT_TEST(math_inverse_identity) {
        const float4x4 identity{
            float4{1.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 1.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 1.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 1.0f}};
        PPR_TEST_ASSERT(matEq(inverse(identity), identity));
        const float4x4 scale{
            float4{2.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 3.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 4.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 1.0f}};
        PPR_TEST_ASSERT(matEq(inverse(scale) * scale, identity));
    };

    PPR_UNIT_TEST(math_inverse_involution) {
        const float4x4 m{
            float4{1.0f, 2.0f, 3.0f, 0.0f},
            float4{0.0f, 1.0f, 4.0f, 0.0f},
            float4{5.0f, 0.0f, 1.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 1.0f}};
        PPR_TEST_ASSERT(matEq(inverse(inverse(m)), m));
    };

    PPR_UNIT_TEST(camera_velocity) {
        Camera cam;
        cam.setPosition(float3{0.0f, 0.0f, 0.0f});
        cam.update(std::chrono::seconds{1});
        PPR_TEST_ASSERT(std::abs(cam.velocity().x) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().y) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().z) < kEps);
        cam.setPosition(float3{1.0f, 2.0f, 3.0f});
        cam.update(std::chrono::seconds{1});
        PPR_TEST_ASSERT(std::abs(cam.velocity().x - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().y - 2.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().z - 3.0f) < kEps);
    };

    // Camera cuts are opt-in (teleport/reset only): a cut frame zeroes velocity and
    // re-baselines history so the teleport delta never leaks into later frames.
    PPR_UNIT_TEST(camera_cut_velocity) {
        Camera cam;
        cam.setPosition(float3{0.0f, 0.0f, 0.0f});
        cam.update(std::chrono::seconds{1});

        // Normal movement produces velocity.
        cam.updateModel(CameraModel{.position = float3{1.0f, 0.0f, 0.0f}, .cameraCut = false}, int2{1920, 1080});
        cam.update(std::chrono::seconds{1});
        PPR_TEST_ASSERT(std::abs(cam.velocity().x - 1.0f) < kEps);

        // A cut frame zeroes velocity despite the position jump.
        cam.updateModel(CameraModel{.position = float3{5.0f, 0.0f, 0.0f}, .cameraCut = true}, int2{1920, 1080});
        cam.update(std::chrono::seconds{1});
        PPR_TEST_ASSERT(std::abs(cam.velocity().x) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().y) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().z) < kEps);

        // Tracking resumes from the post-cut baseline: no teleport spike.
        cam.updateModel(CameraModel{.position = float3{6.0f, 0.0f, 0.0f}, .cameraCut = false}, int2{1920, 1080});
        cam.update(std::chrono::seconds{1});
        PPR_TEST_ASSERT(std::abs(cam.velocity().x - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().y) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.velocity().z) < kEps);
    };

    // A singular view-projection must not poison planes/corners/bbox with NaN.
    PPR_UNIT_TEST(frustum_degenerate_matrix) {
        Frustum frustum;
        const float4x4 singular{
            float4{0.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 0.0f}};
        frustum.setMatrix(singular, rhi::EProjectionConvention::D3D);

        constexpr EFrustumPlane kPlanes[]{
            EFrustumPlane::Near, EFrustumPlane::Far,
            EFrustumPlane::Left, EFrustumPlane::Right,
            EFrustumPlane::Top, EFrustumPlane::Bottom};
        PPR_TEST_ASSERT(std::ranges::all_of(kPlanes, [&](const EFrustumPlane p) noexcept {
            const float4 &pl = frustum.plane(p);
            return std::isfinite(pl.x) && std::isfinite(pl.y)
                && std::isfinite(pl.z) && std::isfinite(pl.w);
        }));

        constexpr EFrustumCorner kCorners[]{
            EFrustumCorner::NearLeftTop, EFrustumCorner::NearLeftBottom,
            EFrustumCorner::NearRightBottom, EFrustumCorner::NearRightTop,
            EFrustumCorner::FarLeftTop, EFrustumCorner::FarLeftBottom,
            EFrustumCorner::FarRightBottom, EFrustumCorner::FarRightTop};
        PPR_TEST_ASSERT(std::ranges::all_of(kCorners, [&](const EFrustumCorner c) noexcept {
            const float3 cr = frustum.corner(c);
            return std::isfinite(cr.x) && std::isfinite(cr.y) && std::isfinite(cr.z);
        }));

        PPR_TEST_ASSERT(std::isfinite(frustum.boundingMin().x));
        PPR_TEST_ASSERT(std::isfinite(frustum.boundingMax().x));
    };

    PPR_UNIT_TEST(camera_mode_accessors) {
        Camera cam;
        PPR_TEST_ASSERT(cam.mode() == ECameraProjection::Perspective);
        cam.setMode(ECameraProjection::Orthographic);
        PPR_TEST_ASSERT(cam.mode() == ECameraProjection::Orthographic);
        cam.setMode(ECameraProjection::Perspective);
        PPR_TEST_ASSERT(cam.mode() == ECameraProjection::Perspective);
    };

    PPR_UNIT_TEST(camera_state_accessors) {
        Camera cam;
        // Default state: both current and previous are zero-positioned.
        PPR_TEST_ASSERT(distance(cam.currentState().model.position, float3{zero_v}) < kEps);
        PPR_TEST_ASSERT(distance(cam.previousState().model.position, float3{zero_v}) < kEps);

        // updateModel applies to currentState; previousState is untouched until update().
        const CameraModel model{
            .position = float3{1.0f, 2.0f, 3.0f},
            .right = float3{1.0f, 0.0f, 0.0f},
            .up = float3{0.0f, 1.0f, 0.0f},
            .forward = float3{0.0f, 0.0f, 1.0f},
            .fov = 1.0f,
            .zNear = 0.5f,
            .zFar = 500.0f,
            .cameraCut = false,
        };
        cam.updateModel(model, int2{800, 600});
        PPR_TEST_ASSERT(distance(cam.currentState().model.position, float3{1.0f, 2.0f, 3.0f}) < kEps);
        PPR_TEST_ASSERT(distance(cam.previousState().model.position, float3{zero_v}) < kEps);
        PPR_TEST_ASSERT(cam.viewportSize().x == 800.0f);
        PPR_TEST_ASSERT(cam.viewportSize().y == 600.0f);

        // update() re-baselines previous to current.
        cam.update(std::chrono::milliseconds{16});
        PPR_TEST_ASSERT(distance(cam.previousState().model.position, float3{1.0f, 2.0f, 3.0f}) < kEps);
    };

    PPR_UNIT_TEST(camera_basis_accessors) {
        Camera cam;
        const CameraModel model{
            .position = float3{0.0f, 0.0f, 5.0f},
            .right = float3{1.0f, 0.0f, 0.0f},
            .up = float3{0.0f, 1.0f, 0.0f},
            .forward = float3{0.0f, 0.0f, -1.0f},
            .fov = 1.0f,
            .zNear = 0.1f,
            .zFar = 100.0f,
            .cameraCut = false,
        };
        cam.updateModel(model, int2{1920, 1080});
        PPR_TEST_ASSERT(distance(cam.up(), model.up) < kEps);
        PPR_TEST_ASSERT(distance(cam.forward(), model.forward) < kEps);
        PPR_TEST_ASSERT(distance(cam.right(), model.right) < kEps);
        PPR_TEST_ASSERT(distance(cam.position(), model.position) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.zNear() - 0.1f) < kEps);
        PPR_TEST_ASSERT(std::abs(cam.zFar() - 100.0f) < kEps);
        // Frustum is derived from the view-projection; must be finite.
        PPR_TEST_ASSERT(std::isfinite(cam.frustum().boundingMin().x));
        PPR_TEST_ASSERT(std::isfinite(cam.frustum().boundingMax().x));
    };

    PPR_UNIT_TEST(camera_inverse_accessors) {
        Camera cam;
        const float4x4 view = makeLookAtMatrix(float3{0.0f, 0.0f, 5.0f}, float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 1.0f, 0.0f});
        const float4x4 proj = rhi::getPerspectiveMatrix(rhi::DeviceType::D3D12, 1.0f, 1.0f, 0.1f, 100.0f);
        cam.setView(view);
        cam.setProjection(proj);
        PPR_TEST_ASSERT(matEq(cam.invertView(), inverse(view)));
        PPR_TEST_ASSERT(matEq(cam.invertProjection(), inverse(proj)));
        PPR_TEST_ASSERT(matEq(cam.invertViewProjection(), inverse(view * proj)));
        // Legacy alias matches the new accessor.
        PPR_TEST_ASSERT(matEq(cam.inverseViewProjection(), cam.invertViewProjection()));
    };

    // The PPE-parity FreeCameraController writes basis vectors from the rotation quaternion:
    // right/up/forward are the quaternion-transformed local axes. The up must be the transformed
    // local up (not PPE's world_up - transformed_up formula).
    PPR_UNIT_TEST(camera_free_look_basis_convention) {
        StubInputService input;
        Camera cam;
        FreeCameraController ctrl;
        ctrl.activate(input, cam);
        ctrl.lookAt(float3{0.0f, 0.0f, 5.0f}, 0.5f, 0.25f);
        CameraModel model = cam.currentState().model;
        ctrl.updateCamera(std::chrono::milliseconds{16}, model);
        const Quaternion rotation = ctrl.rotation();
        PPR_TEST_ASSERT(distance(model.right, quaternionTransform(rotation, float3{1.0f, 0.0f, 0.0f})) < kEps);
        PPR_TEST_ASSERT(distance(model.up, quaternionTransform(rotation, float3{0.0f, 1.0f, 0.0f})) < kEps);
        PPR_TEST_ASSERT(distance(model.forward, quaternionTransform(rotation, float3{0.0f, 0.0f, 1.0f})) < kEps);
        ctrl.deactivate();
    };

    // A teleport sets m_b_teleported, which skips delta consumption for that frame.
    PPR_UNIT_TEST(camera_free_look_teleport_skips_delta) {
        StubInputService input;
        Camera cam;
        FreeCameraController ctrl;
        ctrl.activate(input, cam);
        const float3 eye{1.0f, 2.0f, 3.0f};
        ctrl.lookAt(eye, 0.5f, 0.25f, true); // teleport
        ctrl.translate(float3{10.0f, 0.0f, 0.0f});
        ctrl.rotate(float2{1.0f, 1.0f});
        CameraModel model = cam.currentState().model;
        ctrl.updateCamera(std::chrono::milliseconds{16}, model);
        // Teleport skips delta consumption: position stays at the teleport eye.
        PPR_TEST_ASSERT(distance(model.position, eye) < kEps);
        ctrl.deactivate();
    };

    // translate()/rotate() accumulate deltas consumed by updateCamera.
    PPR_UNIT_TEST(camera_free_look_translate_rotate_helpers) {
        StubInputService input;
        Camera cam;
        FreeCameraController ctrl;
        ctrl.activate(input, cam);
        CameraModel model = cam.currentState().model;

        // Translate accumulates into the position analog (identity rotation → world delta = local delta).
        ctrl.translate(float3{1.0f, 2.0f, 3.0f});
        ctrl.updateCamera(std::chrono::milliseconds{16}, model);
        PPR_TEST_ASSERT(distance(model.position, float3{1.0f, 2.0f, 3.0f}) < 1e-3f);

        // Rotate accumulates into the rotation analog.
        ctrl.rotate(float2{0.1f, 0.2f});
        ctrl.updateCamera(std::chrono::milliseconds{16}, model);
        const Quaternion expected = makeYawPitchRollQuaternion(0.1f, 0.2f, 0.0f);
        const Quaternion q = ctrl.rotation();
        PPR_TEST_ASSERT(std::abs(q.x - expected.x) < 1e-3f);
        PPR_TEST_ASSERT(std::abs(q.y - expected.y) < 1e-3f);
        PPR_TEST_ASSERT(std::abs(q.z - expected.z) < 1e-3f);
        PPR_TEST_ASSERT(std::abs(q.w - expected.w) < 1e-3f);
        ctrl.deactivate();
    };

    // lookAt(eye, target, up) sets the position and orients the camera toward the target.
    PPR_UNIT_TEST(camera_free_look_lookAt_target) {
        StubInputService input;
        Camera cam;
        FreeCameraController ctrl;
        ctrl.activate(input, cam);
        const float3 eye{0.0f, 0.0f, 5.0f};
        const float3 target{0.0f, 0.0f, 0.0f};
        ctrl.lookAt(eye, target, float3{0.0f, 1.0f, 0.0f});
        PPR_TEST_ASSERT(distance(ctrl.position(), eye) < kEps);
        // The camera looks toward the target: forward points from eye to target (not away).
        CameraModel model = cam.currentState().model;
        ctrl.updateCamera(std::chrono::milliseconds{16}, model);
        const float3 expected_forward = normalize(target - eye);
        PPR_TEST_ASSERT(distance(model.forward, expected_forward) < kEps);
        PPR_TEST_ASSERT(distance(model.position, eye) < kEps);
        ctrl.deactivate();
    };

    // lookAt(eye, heading, pitch) sets the position and the yaw-pitch-roll rotation.
    PPR_UNIT_TEST(camera_free_look_lookAt_heading_pitch) {
        StubInputService input;
        Camera cam;
        FreeCameraController ctrl;
        ctrl.activate(input, cam);
        const float3 eye{1.0f, 2.0f, 3.0f};
        constexpr float kHeading = 0.5f;
        constexpr float kPitch = 0.25f;
        ctrl.lookAt(eye, kHeading, kPitch);
        PPR_TEST_ASSERT(distance(ctrl.position(), eye) < kEps);
        const Quaternion expected = makeYawPitchRollQuaternion(kHeading, kPitch, 0.0f);
        const Quaternion q = ctrl.rotation();
        PPR_TEST_ASSERT(std::abs(q.x - expected.x) < kEps);
        PPR_TEST_ASSERT(std::abs(q.y - expected.y) < kEps);
        PPR_TEST_ASSERT(std::abs(q.z - expected.z) < kEps);
        PPR_TEST_ASSERT(std::abs(q.w - expected.w) < kEps);
        ctrl.deactivate();
    };

    // All 31 PPE-parity accessors return the configured values.
    PPR_UNIT_TEST(camera_free_look_accessors) {
        FreeCameraController ctrl;

        // Speed accessors.
        PPR_TEST_ASSERT(std::abs(ctrl.forwardSpeed() - 1.0f) < kEps);
        ctrl.setForwardSpeed(2.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.forwardSpeed() - 2.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.strafeSpeed() - 1.0f) < kEps);
        ctrl.setStrafeSpeed(3.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.strafeSpeed() - 3.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.upwardSpeed() - 1.0f) < kEps);
        ctrl.setUpwardSpeed(4.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.upwardSpeed() - 4.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.headingSpeed() - 10.0f) < kEps);
        ctrl.setHeadingSpeed(5.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.headingSpeed() - 5.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.pitchSpeed() - 10.0f) < kEps);
        ctrl.setPitchSpeed(6.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.pitchSpeed() - 6.0f) < kEps);

        // Range accessors.
        const float2 fov_range = ctrl.fovMinMax();
        PPR_TEST_ASSERT(std::abs(fov_range.x - std::numbers::pi_v<float> / 15.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(fov_range.y - 5.0f * std::numbers::pi_v<float> / 7.0f) < kEps);
        ctrl.setFovMinMax(float2{0.1f, 3.0f});
        PPR_TEST_ASSERT(std::abs(ctrl.fovMinMax().x - 0.1f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.fovMinMax().y - 3.0f) < kEps);
        const float2 speed_range = ctrl.speedMultiplierMinMax();
        PPR_TEST_ASSERT(std::abs(speed_range.x - 0.1f) < kEps);
        PPR_TEST_ASSERT(std::abs(speed_range.y - 50.0f) < kEps);
        ctrl.setSpeedMultiplierMinMax(float2{0.5f, 10.0f});
        PPR_TEST_ASSERT(std::abs(ctrl.speedMultiplierMinMax().x - 0.5f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.speedMultiplierMinMax().y - 10.0f) < kEps);

        // Sensitivity accessors.
        PPR_TEST_ASSERT(std::abs(ctrl.mouseSensitivity().x - 0.05f) < kEps);
        ctrl.setMouseSensitivity(float2{0.1f, 0.2f});
        PPR_TEST_ASSERT(std::abs(ctrl.mouseSensitivity().x - 0.1f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.mouseSensitivity().y - 0.2f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.gamepadSensitivity().x - 0.3f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.gamepadSensitivity().y - 0.1f) < kEps);
        ctrl.setGamepadSensitivity(float2{0.5f, 0.5f});
        PPR_TEST_ASSERT(std::abs(ctrl.gamepadSensitivity().x - 0.5f) < kEps);

        // Inertia accessors (map to the analog sensitivity).
        PPR_TEST_ASSERT(std::abs(ctrl.positionInertia() - 0.15f) < kEps);
        ctrl.setPositionInertia(1.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.positionInertia() - 1.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.rotationInertia() - 0.15f) < kEps);
        ctrl.setRotationInertia(1.0f);
        PPR_TEST_ASSERT(std::abs(ctrl.rotationInertia() - 1.0f) < kEps);

        // State accessors.
        PPR_TEST_ASSERT(distance(ctrl.position(), float3{zero_v}) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.fov() - std::numbers::pi_v<float> / 3.0f) < kEps);
        PPR_TEST_ASSERT(std::abs(ctrl.speedMultiplier() - 1.0f) < kEps);
        PPR_TEST_ASSERT(!ctrl.isTeleported());

        // Input action accessors.
        PPR_TEST_ASSERT(&ctrl.fovInput() != nullptr);
        PPR_TEST_ASSERT(&ctrl.lookInput() != nullptr);
        PPR_TEST_ASSERT(&ctrl.moveInput() != nullptr);
        PPR_TEST_ASSERT(&ctrl.rotateInput() != nullptr);
        PPR_TEST_ASSERT(&ctrl.speedInput() != nullptr);
    };

    PPR_UNIT_TEST(app_camera) {
        _.recurse({
            camera_model,
            camera_service_init,
            camera_viewport_size,
            camera_velocity,
            camera_cut_velocity,
            frustum_degenerate_matrix,
            camera_service_projection,
            replay_round_trip,
            replay_recording_control,
            replay_injection,
            replay_decorator,
            free_camera_activate,
            pan_camera_controller,
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
            camera_free_look_basis_convention,
            camera_free_look_teleport_skips_delta,
            camera_free_look_translate_rotate_helpers,
            camera_free_look_lookAt_target,
            camera_free_look_lookAt_heading_pitch,
            camera_free_look_accessors,
        });
    };
}
