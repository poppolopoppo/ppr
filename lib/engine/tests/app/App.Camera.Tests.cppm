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
        const float4x4 view = lookAt(float3{0.0f, 0.0f, 5.0f}, float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 1.0f, 0.0f});
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
        ctrl.update(std::chrono::milliseconds{16});

        const float3 initial = cam.position();
        const InputMessage msg{
            InputKey::w,
            InputValue{InputDigital{true}},
            TimeSpan{},
            InputDeviceID{0u},
            EInputMessageEvent::pressed};
        if (input.m_listener) {
            (void)input.m_listener->postKeyEvent(msg);
        }
        ctrl.update(std::chrono::milliseconds{16});
        PPR_TEST_ASSERT(std::isfinite(cam.position().x));
        PPR_TEST_ASSERT(distance(cam.position(), initial) > kEps);
        ctrl.deactivate();
    };

    PPR_UNIT_TEST(lookat_canonical) {
        const float4x4 view = lookAt(float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 0.0f, -1.0f}, float3{0.0f, 1.0f, 0.0f});
        const float4x4 expected{
            float4{-1.0f, 0.0f, 0.0f, 0.0f},
            float4{ 0.0f, 1.0f, 0.0f, 0.0f},
            float4{ 0.0f, 0.0f, -1.0f, 0.0f},
            float4{ 0.0f, 0.0f, 0.0f, 1.0f}};
        PPR_TEST_ASSERT(matEq(view, expected));
    };

    PPR_UNIT_TEST(lookat_eye_to_origin) {
        const float3 eye{1.0f, 2.0f, 3.0f};
        const float3 target{4.0f, 5.0f, 6.0f};
        const float3 up{0.0f, 1.0f, 0.0f};
        const float4x4 view = lookAt(eye, target, up);
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
        const float4x4 view = lookAt(eye, target, up);
        const float4 target_view = float4{target, 1.0f} * view;
        const float d = distance(eye, target);
        PPR_TEST_ASSERT(std::abs(target_view.x) < kEps);
        PPR_TEST_ASSERT(std::abs(target_view.y) < kEps);
        PPR_TEST_ASSERT(std::abs(target_view.z - d) < kEps);
        PPR_TEST_ASSERT(std::abs(target_view.w - 1.0f) < kEps);
    };

    PPR_UNIT_TEST(lookat_orthonormal_bis) {
        const float4x4 view = lookAt(float3{1.0f, 2.0f, 3.0f}, float3{4.0f, 5.0f, 6.0f}, float3{0.0f, 1.0f, 0.0f});
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
        ctrl.update(std::chrono::milliseconds{16});

        const float3 initial = cam.position();
        const InputMessage msg{
            InputKey::w,
            InputValue{InputDigital{true}},
            TimeSpan{},
            InputDeviceID{0u},
            EInputMessageEvent::pressed};
        if (input.m_listener) {
            (void)input.m_listener->postKeyEvent(msg);
        }
        ctrl.update(std::chrono::milliseconds{16});
        PPR_TEST_ASSERT(std::isfinite(cam.position().x));
        PPR_TEST_ASSERT(distance(cam.position(), initial) > kEps);
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

    PPR_UNIT_TEST(app_camera) {
        _.recurse({
            camera_model,
            camera_service_init,
            camera_viewport_size,
            camera_velocity,
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
        });
    };
}
