module;
#include "pP/UnitTest.h"

#include <cstdio>
#include <cstdlib>

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.mesh;
import engine.rhi;
import engine.shader;
import engine.sim;
import std;

import game.colony.driver;

namespace pP::tests::detail {
    namespace RunGate {
        // ------------------------------------------------------------------
        // B1'/B2'/B3' run-level evidence: real Application::run() through a
        // derived Editor fixture with an injected clock and a scripted
        // platform. One reusable harness serves admission (B2'), fail-stop
        // (B1'), and saturation (B3') coverage.
        //
        // Fixture shape: ApplicationEditor-derived (Editor type + real
        // camera/passes), Application::initialize only — pinned by the
        // SaturatingHarness::initialize override below, because run()
        // dispatches initialize() virtually and the Editor stages would
        // otherwise dereference platform window state (no native window —
        // windowed stages stay null; ImGui is therefore NOT exercised here
        // and stays an honestly-manual item), offscreen target owned by the
        // fixture and released post-quiescence pre-RHI-shutdown, finite
        // watchdog pump cap (scripted exit must win; the watchdog firing
        // fails the test).
        //
        // Service audit for every resolution on the exercised run-loop paths
        // (run/probe/update/render, harness update/render, pumpNativeEvents,
        // batch dispatch, finishRun_/teardown):
        // - IInputService: resolved (busy-pump staging, admitted
        //   poll/dispatch, batch replay). PROVIDED (scripted InputService).
        // - IWindowService: resolved only by the pinned-away Editor stages
        //   plus the focus-dispatch service sink. PROVIDED (scripted
        //   WindowService; monitors absent, should-close false).
        // - IPlayerService: never resolved (Editor domain m_needs_presence
        //   = false, so production creates none either). Kept null.
        // - IShaderService/IRhiService/Renderer: real singletons/objects
        //   from Application::initialize; resolved by boot() and the
        //   harness passes. No fake.
        // - IUIService (ImGui): created only by the pinned-away Editor
        //   stages; null here, and every production use is member-guarded.
        // - ServicesStore window/player lookups: no exercised production
        //   path performs one, so the scripted platform registers nothing
        //   (mirrors the previous input-service-only registration shape).
        // ------------------------------------------------------------------

        constexpr TimeSpan kFrameStep = std::chrono::milliseconds{16};

        struct FakeClock final : ITimerClock {
            mutable u64 m_calls{0u};
            TimePoint m_epoch{};

            [[nodiscard]] TimePoint now() const noexcept override {
                // Exactly one live now() per frame (uncapped pacer never
                // sleeps; TimerManager::tick samples once): every frame's dt
                // is exactly kFrameStep, so carries are exact multiples.
                return m_epoch + (m_calls++ * kFrameStep);
            }
        };

        struct ScriptedPlatform final : IPlatform {
            safe_ptr<Application> m_application{};

            u32 m_pumps{0u};
            u32 m_updates{0u};
            u32 m_device_polls{0u};
            u32 m_dispatches{0u};
            TimeSpan m_device_time{};

            PendingInputBatch m_batch{};
            Array<u32> m_batch_sizes{};
            Array<u32> m_update_pumps{};
            InputContext m_context{};
            InputListener m_listener{};
            KeyboardDevice m_keyboard{InputDeviceID{1u}};
            MouseDevice m_mouse{InputDeviceID{2u}};
            GamepadDevice m_gamepad{InputDeviceID{3u}};
            Window m_window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(2)},
                WindowModel{
                    .m_title = "scripted",
                    .m_window_size = int2{800, 600},
                }};
            Array<std::string> m_log{};

            // Per-pump script hook (stages arrivals, opens gates, ends the
            // run). Runs inside pumpNativeEvents on BOTH busy and admitted
            // frames.
            std23::move_only_function<void(ScriptedPlatform &platform, u32 pump)> m_on_pump{};
            std::error_code m_pump_error{};

            u32 m_watchdog_pumps{1000u};
            bool m_watchdog_fired{false};

            ScriptedPlatform() {
                m_window.m_framebuffer_size = int2{800, 600};
                m_context.addInputListener(safe_ptr<InputListener>{&m_listener}, 0);
                m_listener.setRawKeyCallback([this](const TimeSpan, const InputMessage &) -> EInputMessageResponse {
                    m_log.push_back("key");
                    return EInputMessageResponse::handled;
                });
                m_listener.setCharacterInputCallback([this](const hal::native::char_t) -> EInputMessageResponse {
                    m_log.push_back("char");
                    return EInputMessageResponse::handled;
                });
                m_input_service = std::make_unique<InputService>(this);
                m_window_service = std::make_unique<WindowService>(this);
            }

            ~ScriptedPlatform() {
                // Discard-before-destroy: drop every collected record (and
                // the listener registration) before members unwind.
                m_context.clearInputListeners();
                m_batch.clear();
                std::ignore = m_window.release();
            }

            [[nodiscard]] std::error_code initialize(Application &application) override {
                m_application.reset(&application);
                return default_value_v;
            }

            [[nodiscard]] std::error_code shutdown(Application &) override {
                m_batch.clear();
                m_input_service->m_collecting = false;
                m_application.reset();
                return default_value_v;
            }

            [[nodiscard]] std::string_view getPlatformName() const noexcept override {
                return "Scripted";
            }

            [[nodiscard]] platform::Version getPlatformVersion() const noexcept override {
                return {.m_major = 0, .m_minor = 0, .m_revision = 0};
            }

            [[nodiscard]] safe_ptr<Application> getApplication() const noexcept override {
                return m_application;
            }

            [[nodiscard]] safe_ptr<IInputService> getInputService() const noexcept override {
                return safe_ptr<IInputService>{m_input_service.get()};
            }

            [[nodiscard]] safe_ptr<IWindowService> getWindowService() const noexcept override {
                // Scripted window backend (FakeFrameInput pattern, like the
                // input service above): owns every created window, tracks
                // main/focused, fires the service-level broadcasts the
                // admitted dispatch replays through. Monitors stay absent
                // (headless: no refresh-rate retarget, uncapped pacer) and
                // should-close stays false (exit only via the app latch).
                return safe_ptr<IWindowService>{m_window_service.get()};
            }

            [[nodiscard]] safe_ptr<IPlayerService> getPlayerService() const noexcept override {
                // Domain-absent by design: the Editor domain sets
                // m_needs_presence = false, so production creates no player
                // service either, and no exercised run-loop path resolves
                // one (audit in the fixture note below). Kept null.
                return {};
            }

            // B2' collection only: stage scripted arrivals, serve close/exit
            // via the app latch. NEVER advances device time, polls devices,
            // dispatches batches, or opens UI frames. Collection stages
            // through the owned input service (collecting flag below),
            // exactly like window callbacks stage into GlfwInput's batch.
            [[nodiscard]] std::error_code pumpNativeEvents() override {
                ++m_pumps;
                m_input_service->m_collecting = true;
                PPR_DEFER {
                    m_input_service->m_collecting = false;
                };
                if (m_on_pump) {
                    m_on_pump(*this, m_pumps);
                }
                m_batch_sizes.push_back(safe_narrowing(m_batch.size()));
                if (m_pumps >= m_watchdog_pumps and m_application.isValid() and not m_application->isExitRequested()) {
                    m_watchdog_fired = true;
                    m_application->requestExit();
                }
                return m_pump_error;
            }

            // Admitted frames only: poll-once (single device-time advance
            // through the input service) → collect (one pump) →
            // dispatch-once (input replay + deferred control halves, in
            // collection order).
            [[nodiscard]] std::error_code update(const TimeSpan dt) override {
                ++m_updates;
                if (const std::error_code ec = m_input_service->pollInputDevices(dt)) {
                    return ec;
                }
                if (const std::error_code ec = pumpNativeEvents()) {
                    return ec;
                }
                m_update_pumps.push_back(m_pumps);

                ++m_dispatches;
                PendingFocusApplier focus_applier =
                    [this](const safe_ptr<Window> window, const bool focused) -> std::error_code {
                    if (not window.isValid()) [[unlikely]] {
                        return std::make_error_code(std::errc::invalid_argument);
                    }
                    // Deferred client half, mirroring the platform applier:
                    // window-level broadcast now, then the service-level
                    // sink (empty headless — success), in collection order.
                    // B2'c: the CAPTURED value goes to BOTH — never the
                    // live flag, which may already hold a later arrival.
                    window->m_when_focused(*window, focused);
                    if (const std::error_code ec = m_window_service->notifyFocused(*window, focused)) {
                        return ec;
                    }
                    m_log.push_back(focused ? "focus:1" : "focus:0");
                    return default_value_v;
                };
                PendingDeviceApplier device_applier =
                    [this](const safe_ptr<const IInputDevice> device, const bool connected) -> std::error_code {
                    if (not device.isValid()) [[unlikely]] {
                        return std::make_error_code(std::errc::invalid_argument);
                    }
                    // Deferred client half: fire the service broadcast (empty
                    // headless — success) and record the ordered delivery.
                    if (connected) {
                        if (const std::error_code ec = m_input_service->notifyConnected(*device)) {
                            return ec;
                        }
                    } else {
                        if (const std::error_code ec = m_input_service->notifyDisconnected(*device)) {
                            return ec;
                        }
                    }
                    m_log.push_back(connected ? "device:1" : "device:0");
                    return default_value_v;
                };
                return m_batch.dispatch(m_keyboard, m_mouse, dt, std::move(focus_applier), std::move(device_applier));
            }

            void stageKey(const EKeyboardKey key, const bool pressed) {
                m_input_service->postKeyboardKeyPressed(m_context, key, pressed);
            }

            void stageChar(const hal::native::char_t codepoint) {
                m_input_service->postKeyboardCharacterInput(m_context, codepoint);
            }

            void stageFocus(const bool focused) {
                // Immediate platform half (mirrors the GLFW focus callback):
                // flags run synchronously so the deferred dispatch observes
                // them; the client broadcast replays at dispatch, in order.
                m_window.m_focused = focused;
                m_batch.postFocus(safe_ptr<Window>{&m_window}, focused);
            }

            void stageDevice(const bool connected) {
                const safe_ptr<const IInputDevice> device{&m_keyboard};
                m_batch.postDeviceConnection(device, connected);
            }

            // Scripted input service (FakeFrameInput pattern from
            // App.Frame.Tests.cpp): the platform owns the input backend
            // exactly like GlfwPlatform owns GlfwInput, so every collection
            // path that resolves the service — including the B2' busy pumps,
            // which stage without advancing devices or dispatching — has a
            // real target. Staging and device advancement run on the same
            // platform-owned members the assertions observe (one shared
            // truth); this type is only the interface over them.
            struct InputService final : IInputService {
                ScriptedPlatform *m_owner{nullptr};
                DeviceCallback m_when_connected{};
                DeviceCallback m_when_disconnected{};
                bool m_collecting{false};

                explicit InputService(ScriptedPlatform *owner) noexcept : m_owner(owner) {
                }

                [[nodiscard]] const KeyboardDevice &getKeyboard() const noexcept override {
                    return m_owner->m_keyboard;
                }

                [[nodiscard]] const MouseDevice &getMouse() const noexcept override {
                    return m_owner->m_mouse;
                }

                [[nodiscard]] const GamepadDevice &getGamepad(int) const noexcept override {
                    return m_owner->m_gamepad;
                }

                [[nodiscard]] SharedInputDevice getInputDeviceByID(
                    const InputDeviceID &device_id) const noexcept override {
                    if (device_id == m_owner->m_keyboard.m_device_id) {
                        return safe_ptr<const IInputDevice>{&m_owner->m_keyboard};
                    }
                    if (device_id == m_owner->m_mouse.m_device_id) {
                        return safe_ptr<const IInputDevice>{&m_owner->m_mouse};
                    }
                    if (device_id == m_owner->m_gamepad.m_device_id) {
                        return safe_ptr<const IInputDevice>{&m_owner->m_gamepad};
                    }
                    return {};
                }

                [[nodiscard]] std::error_code enumerateInputDevices(
                    Collector<SharedInputDevice> each_device) const noexcept override {
                    const SharedInputDevice devices[] = {
                        safe_ptr<const IInputDevice>{&m_owner->m_keyboard},
                        safe_ptr<const IInputDevice>{&m_owner->m_mouse},
                        safe_ptr<const IInputDevice>{&m_owner->m_gamepad},
                    };
                    return each_device.append(devices);
                }

                [[nodiscard]] std::error_code enumerateInputKeysSupported(
                    Collector<InputKey> supports_key) const override {
                    if (const std::error_code ec = m_owner->m_keyboard.enumerateSupportedInputKeys(supports_key)) {
                        return ec;
                    }
                    if (const std::error_code ec = m_owner->m_mouse.enumerateSupportedInputKeys(supports_key)) {
                        return ec;
                    }
                    return m_owner->m_gamepad.enumerateSupportedInputKeys(supports_key);
                }

                [[nodiscard]] InputContext &getGlobalInputContext() noexcept override {
                    return m_owner->m_context;
                }

                [[nodiscard]] bool hasInputContext(const InputContext &context) const noexcept override {
                    return &context == &m_owner->m_context;
                }

                void addInputContext(SharedInputContext) override {
                }

                bool removeInputContext(const InputContext &) override {
                    return false;
                }

                void assignInputContextToDevice(InputDeviceID, SharedInputContext) override {
                }

                void clearInputContextDeviceAssignments() override {
                }

                [[nodiscard]] std::error_code enumerateInputContexts(
                    Collector<InputContext> each_context) const override {
                    return each_context(m_owner->m_context);
                }

                [[nodiscard]] std::error_code enumerateInputContextDeviceAssignments(
                    Collector<IInputDevice, InputContext>) const override {
                    return default_value_v;
                }

                // Once-per-client-frame advancement: device time, transient
                // clears, exactly like the platform poll it replaces below.
                [[nodiscard]] std::error_code pollInputDevices(const TimeSpan dt) override {
                    m_owner->m_device_time += dt;
                    ++m_owner->m_device_polls;
                    if (const std::error_code ec = m_owner->m_keyboard.pollInputMessages(dt)) {
                        return ec;
                    }
                    return m_owner->m_mouse.pollInputMessages(dt);
                }

                void resetInputDevices() noexcept override {
                    m_owner->m_keyboard.resetInputState();
                    m_owner->m_mouse.resetInputState();
                    m_owner->m_gamepad.resetInputState();
                }

                void postKeyboardCharacterInput(
                    const InputContext &context, const hal::native::char_t codepoint) override {
                    if (m_collecting) {
                        m_owner->m_batch.postChar(safe_ptr<const InputContext>{&context}, codepoint);
                        return;
                    }
                    m_owner->m_keyboard.postKeyboardCharacterInput(context, codepoint);
                }

                void postKeyboardKeyPressed(
                    const InputContext &context, const EKeyboardKey key, const bool pressed) override {
                    if (m_collecting) {
                        m_owner->m_batch.postKey(safe_ptr<const InputContext>{&context}, key, pressed);
                        return;
                    }
                    m_owner->m_keyboard.postKeyboardKeyPressed(TimeSpan{}, context, key, pressed);
                }

                void postMouseButtonPressed(
                    const InputContext &context, const EMouseButton button, const bool pressed) override {
                    if (m_collecting) {
                        m_owner->m_batch.postMouseButton(safe_ptr<const InputContext>{&context}, button, pressed);
                        return;
                    }
                    m_owner->m_mouse.postMouseButtonPressed(TimeSpan{}, context, button, pressed);
                }

                void postMouseCursorPosition(const InputContext &context, const float2 &absolute_pos) override {
                    if (m_collecting) {
                        m_owner->m_batch.postMouseCursor(safe_ptr<const InputContext>{&context}, absolute_pos);
                        return;
                    }
                    m_owner->m_mouse.postMouseCursorPosition(TimeSpan{}, context, absolute_pos);
                }

                void postMouseScrollWheel(const InputContext &context, const float2 &delta) override {
                    if (m_collecting) {
                        m_owner->m_batch.postMouseScroll(safe_ptr<const InputContext>{&context}, delta);
                        return;
                    }
                    m_owner->m_mouse.postMouseScrollWheel(TimeSpan{}, context, delta);
                }

                [[nodiscard]] DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event on_connected) override {
                    return m_when_connected.add(std::move(on_connected));
                }

                [[nodiscard]] DeviceCallback::Handle whenDeviceDisconnected(
                    DeviceCallback::Event on_disconnected) override {
                    return m_when_disconnected.add(std::move(on_disconnected));
                }

                [[nodiscard]] std::error_code notifyConnected(const IInputDevice &device) {
                    return m_when_connected(device);
                }

                [[nodiscard]] std::error_code notifyDisconnected(const IInputDevice &device) {
                    return m_when_disconnected(device);
                }
            };

            // Scripted window backend: owns every created window (fabricated
            // non-null handles — unique per window, never presented, never
            // registered with the renderer), tracks main/focused, and fires
            // the service-level broadcasts the admitted dispatch replays
            // through. Monitor halves stay absent (headless: no refresh-rate
            // retarget, so the pacer stays uncapped and FakeClock steps stay
            // exact); manipulations update the plain window model in place.
            struct WindowService final : IWindowService {
                ScriptedPlatform *m_owner{nullptr};

                Array<std::unique_ptr<Window>> m_windows{};
                SharedWindow m_main_window{};
                SharedWindow m_focused_window{};

                MonitorCallback m_when_monitor_connected{};
                MonitorCallback m_when_monitor_disconnected{};
                WindowCallback m_when_created{};
                WindowCallback m_when_destroyed{};
                WindowCallback m_when_closed{};
                WindowFocusedCallback m_when_focused{};
                WindowIconifiedCallback m_when_iconified{};
                WindowMovedCallback m_when_moved{};
                WindowResizedCallback m_when_resized{};
                WindowScaledCallback m_when_scaled{};

                // Observability (additive: no B2' assertion reads these;
                // they prove the window lifecycle ran exactly once).
                u32 m_creates{0u};
                u32 m_destroys{0u};
                u32 m_set_mains{0u};
                u32 m_polls{0u};
                IWindowService::ECursorMode m_cursor_mode{IWindowService::ECursorMode::normal};
                bool m_close_requested{false};
                std::string m_clipboard{};

                explicit WindowService(ScriptedPlatform *owner) noexcept : m_owner(owner) {
                }

                ~WindowService() override {
                    // Discard-before-destroy (same discipline as the
                    // platform): release every fabricated handle so the
                    // Window destructor assert (m_handle == nullptr) never
                    // fires on a window the test forgot to destroy.
                    for (const std::unique_ptr<Window> &owned: m_windows) {
                        std::ignore = owned->release();
                    }
                }

                [[nodiscard]] std::error_code pollEvents() override {
                    ++m_polls;
                    return default_value_v;
                }

                [[nodiscard]] std::error_code waitEvents() override {
                    ++m_polls;
                    return default_value_v;
                }

                [[nodiscard]] std::error_code enumerateMonitors(Collector<SharedMonitor>) const noexcept override {
                    return default_value_v;
                }

                [[nodiscard]] SharedMonitor getPrimaryMonitor() const noexcept override {
                    return {};
                }

                [[nodiscard]] std::error_code enumerateMonitorVideoModes(
                    const Monitor &, Collector<VideoMode>) const noexcept override {
                    return default_value_v;
                }

                void setMonitorGamma(const Monitor &, const float) noexcept override {
                }

                [[nodiscard]] MonitorCallback::Handle whenMonitorConnected(
                    MonitorCallback::Event on_connected) noexcept override {
                    return m_when_monitor_connected.add(std::move(on_connected));
                }

                [[nodiscard]] MonitorCallback::Handle whenMonitorDisconnected(
                    MonitorCallback::Event on_disconnected) noexcept override {
                    return m_when_monitor_disconnected.add(std::move(on_disconnected));
                }

                [[nodiscard]] std::error_code createWindow(
                    WindowModel &&definition, safe_ptr<Window> *window_write_ref) override {
                    if (window_write_ref == nullptr) [[unlikely]] {
                        return std::make_error_code(std::errc::invalid_argument);
                    }
                    const auto tag = static_cast<std::uintptr_t>(m_windows.size() + 1u);
                    auto window = std::make_unique<Window>(
                        WindowHandle{reinterpret_cast<void *>(0x57A10000u + tag)},
                        NativeWindowHandle{reinterpret_cast<void *>(0x0A71CE000u + tag)}, std::move(definition));
                    window->m_framebuffer_size = window->m_window_size;
                    window_write_ref->reset(window.get());
                    m_windows.push_back(std::move(window));
                    ++m_creates;
                    if (const std::error_code err = m_when_created(**window_write_ref)) [[unlikely]] {
                        std::ignore = destroyWindow(std::move(*window_write_ref));
                        window_write_ref->reset();
                        return err;
                    }
                    return default_value_v;
                }

                [[nodiscard]] std::error_code destroyWindow(SharedWindow &&window) override {
                    if (not window.isValid()) [[unlikely]] {
                        return std::make_error_code(std::errc::invalid_argument);
                    }
                    const auto it = std::ranges::find_if(
                        m_windows,
                        [&](const std::unique_ptr<Window> &owned) noexcept -> bool { return owned.get() == &*window; });
                    if (it == m_windows.end()) [[unlikely]] {
                        return std::make_error_code(std::errc::invalid_argument);
                    }
                    const bool was_main = (m_main_window == window);
                    const bool was_focused = (m_focused_window == window);
                    std::unique_ptr<Window> extracted = std::move(*it);
                    m_windows.erase(it);
                    window = nullptr;
                    if (was_main) {
                        m_main_window.reset();
                    }
                    if (was_focused) {
                        m_focused_window.reset();
                    }
                    // Release the fabricated handle before the unique_ptr
                    // destroys the Window (destructor asserts null handle).
                    std::ignore = extracted->release();
                    ++m_destroys;
                    return m_when_destroyed(*extracted);
                }

                [[nodiscard]] const SharedWindow &getFocusedWindow() const noexcept override {
                    return m_focused_window;
                }

                [[nodiscard]] const SharedWindow &getMainWindow() const noexcept override {
                    return m_main_window;
                }

                SharedWindow setMainWindow(SharedWindow window) override {
                    PPR_ASSERT(
                        not window.isValid() or
                        std::ranges::find_if(m_windows, [&](const std::unique_ptr<Window> &owned) noexcept -> bool {
                            return owned.get() == &*window;
                        }) != m_windows.end());
                    SharedWindow old_main_window{m_main_window};
                    m_main_window = std::move(window);
                    ++m_set_mains;
                    return old_main_window;
                }

                [[nodiscard]] SharedMonitor getWindowFullscreenMonitor(const Window &) const noexcept override {
                    return {};
                }

                void setWindowMonitor(const Window &, const Monitor &, const int2 &, const int2 &) override {
                }

                [[nodiscard]] bool getWindowShouldClose(const Window &) const noexcept override {
                    return false;
                }

                void setWindowShouldClose(const Window &, const bool value) override {
                    m_close_requested = value;
                }

                void setWindowCursorMode(const Window &, const ECursorMode mode) override {
                    m_cursor_mode = mode;
                }

                void moveWindow(const Window &window, const int2 &position) override {
                    const_cast<Window &>(window).m_window_position = position;
                }

                void resizeWindow(const Window &window, const int2 &size) override {
                    Window &mutable_window = const_cast<Window &>(window);
                    mutable_window.m_window_size = size;
                    mutable_window.m_framebuffer_size = size;
                }

                void renameWindow(const Window &window, const std::string_view &title) override {
                    const_cast<Window &>(window).m_title = std::string{title};
                }

                void showWindow(const Window &window) override {
                    const_cast<Window &>(window).m_visible = true;
                }

                void hideWindow(const Window &window) override {
                    const_cast<Window &>(window).m_visible = false;
                }

                void iconifyWindow(const Window &window) override {
                    const_cast<Window &>(window).m_iconified = true;
                }

                void restoreWindow(const Window &window) override {
                    const_cast<Window &>(window).m_iconified = false;
                }

                void focusWindow(const Window &window) override {
                    Window &mutable_window = const_cast<Window &>(window);
                    mutable_window.m_focused = true;
                    m_focused_window = SharedWindow{&mutable_window};
                }

                void swapWindowBuffers(const Window &) override {
                }

                [[nodiscard]] void *getWindowNativeHandle(const Window &window) const noexcept override {
                    return window.m_native;
                }

                void setWindowClipboardString(const Window &, const std::string_view &text) override {
                    m_clipboard = std::string{text};
                }

                [[nodiscard]] std::string_view getWindowClipboardString(const Window &) const noexcept override {
                    return m_clipboard;
                }

                [[nodiscard]] WindowCallback::Handle whenWindowCreated(
                    WindowCallback::Event on_created) noexcept override {
                    return m_when_created.add(std::move(on_created));
                }

                [[nodiscard]] WindowCallback::Handle whenWindowDestroyed(
                    WindowCallback::Event on_destroyed) noexcept override {
                    return m_when_destroyed.add(std::move(on_destroyed));
                }

                [[nodiscard]] WindowCallback::Handle whenWindowClosed(
                    WindowCallback::Event on_closed) noexcept override {
                    return m_when_closed.add(std::move(on_closed));
                }

                [[nodiscard]] WindowFocusedCallback::Handle whenWindowFocused(
                    WindowFocusedCallback::Event on_focused) noexcept override {
                    return m_when_focused.add(std::move(on_focused));
                }

                [[nodiscard]] WindowIconifiedCallback::Handle whenWindowIconified(
                    WindowIconifiedCallback::Event on_iconified) noexcept override {
                    return m_when_iconified.add(std::move(on_iconified));
                }

                [[nodiscard]] WindowMovedCallback::Handle whenWindowMoved(
                    WindowMovedCallback::Event on_moved) noexcept override {
                    return m_when_moved.add(std::move(on_moved));
                }

                [[nodiscard]] WindowResizedCallback::Handle whenWindowResized(
                    WindowResizedCallback::Event on_resized) noexcept override {
                    return m_when_resized.add(std::move(on_resized));
                }

                [[nodiscard]] WindowScaledCallback::Handle whenWindowScaled(
                    WindowScaledCallback::Event on_scaled) noexcept override {
                    return m_when_scaled.add(std::move(on_scaled));
                }

                [[nodiscard]] std::error_code notifyFocused(const Window &window, const bool focused) {
                    return m_when_focused(window, focused);
                }
            };

            std::unique_ptr<InputService> m_input_service{};
            std::unique_ptr<WindowService> m_window_service{};

            void endRun() {
                if (m_application.isValid()) {
                    m_application->requestExit();
                }
            }
        };

        // The injected clock must be fully constructed BEFORE the
        // Application base (which holds a non-owning view of it): a base
        // class is initialized before the derived Editor base, unlike a
        // member. ScriptedPlatform has no such constraint (factory call).
        struct HarnessClock {
            FakeClock m_clock{};
        };

        // Derived Editor fixture: real camera + real Grid/Triangle passes,
        // Editor-shaped update/render (camera → passes → offscreen submit +
        // acceptance stamping), real run(). No window/viewport/UI (null).
        struct SaturatingHarness : HarnessClock, ApplicationEditor {
            ScriptedPlatform *m_scripted{nullptr};

            Camera m_camera{};
            GridPass m_grid{};
            TrianglePass m_triangles{};
            TriangleBagHandle m_tri_bag{};
            MaterialHandle m_tri_material{};
            rhi::ComPtr<rhi::ITexture> m_target{};
            bool m_passes_live{false};
            // Set by a successful boot(): run()'s initialize() above must
            // not re-initialize (see the single-init guard there).
            bool m_booted{false};
            // Idempotent-shutdown record: the first shutdown() runs the body
            // and records its outcome; re-entry (the test scope-exit
            // teardown after run()'s finishRun_ shutdown) re-reports it
            // without touching released owners again.
            bool m_shutdown_done{false};
            std::error_code m_shutdown_result{};

            u32 m_updates{0u};
            u32 m_renders{0u};
            Array<TimeSpan> m_update_dts{};
            FenceCompletionValue m_last_accepted{kFenceValueNone};
            std::error_code m_update_error{};
            std::error_code m_render_error{};
            std23::move_only_function<void(SaturatingHarness &harness)> m_on_rendered{};

            explicit SaturatingHarness(const std::string_view name, GpuTeardownFailureHandler handler = nullptr)
                : HarnessClock{}, ApplicationEditor(
                                      name, std::span<const char *const>{}, std::make_unique<ScriptedPlatform>(),
                                      HarnessClock::m_clock, handler) {
                // The platform is owned by the base; keep a view for the script.
                m_scripted = static_cast<ScriptedPlatform *>(const_cast<IPlatform *>(&getPlatform()));
            }

            // Matches the application base: never throws (members unwind
            // best-effort, like the other harnesses' ignore discipline).
            ~SaturatingHarness() noexcept override = default;

            // Root-cause pin (saturated_run abort): run() calls initialize()
            // virtually, which would otherwise reach
            // ApplicationEditor::initialize — real window/viewport/UI stages
            // that unconditionally dereference the window service
            // (App.Application.Editor.cpp:167,469). This fixture is
            // Application::initialize-only by design (offscreen target,
            // scripted platform, no native window), exactly like boot()
            // below: pin the base here so run() exercises the same base-only
            // initialization the B2' assertions observe. Editor member stages
            // stay null and take their production null paths at shutdown
            // (proven precedent: EditorShutdownHarness in
            // App.FrameFence.Tests.cpp); the scripted window service above
            // still honors the IPlatform contract for anything that resolves
            // it (notably the focus-dispatch service sink).
            //
            // Single-init guard (ASan heap-buffer-overflow WRITE in
            // Renderer::setTestHoldSweep): Application::initialize()
            // unconditionally replaces m_renderer (App.Application.cpp:392),
            // so a second initialization inside run() would destroy the
            // boot-time renderer out from under the test's armed seams and
            // bound references, silently drop the hold (no saturation), and
            // leave every later renderer touch a use-after-free. boot()
            // owns initialization; run() must not re-initialize.
            [[nodiscard]] std::error_code initialize() override {
                if (m_booted) {
                    return default_value_v;
                }
                return Application::initialize();
            }

            [[nodiscard]] std::error_code boot() {
                if (const std::error_code ec = Application::initialize()) {
                    return ec;
                }

                const safe_ptr<IRhiService> rhi = getServices().get<IRhiService>();
                const safe_ptr<IShaderService> shaders = getServices().get<IShaderService>();
                PPR_ASSERT(rhi.isValid());
                PPR_ASSERT(shaders.isValid());
                if (const std::error_code ec = m_grid.initialize(*rhi, *shaders, getContentDir().path())) {
                    return ec;
                }
                if (const std::error_code ec = m_triangles.initialize(*rhi, *shaders, getContentDir().path())) {
                    return ec;
                }

                // Triangle content (mirrors the fence growth test): one mesh,
                // one fallback material, submitted every admitted frame.
                const mesh::StaticMeshVertex verts[] = {
                    {.m_position = {0.0f, 0.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                    {.m_position = {1.0f, 0.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                    {.m_position = {0.0f, 1.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                };
                const u32 indices[] = {0u, 1u, 2u};
                const Expected<TriangleBagHandle> bag = m_triangles.uploadMesh(
                    std::span<const mesh::StaticMeshVertex>{verts}, std::span<const u32>{indices});
                if (not bag.has_value()) {
                    return bag.error();
                }
                m_tri_bag = *bag;
                const TextureHandle no_textures[] = {
                    TextureHandle{}, TextureHandle{}, TextureHandle{}, TextureHandle{}};
                const Expected<MaterialHandle> material = m_triangles.packMaterial(
                    mesh::MaterialAsset{}, std::span<const TextureHandle>{no_textures});
                if (not material.has_value()) {
                    return material.error();
                }
                m_tri_material = *material;

                rhi::TextureDesc target_desc{};
                target_desc.type = rhi::TextureType::Texture2D;
                target_desc.size = {64u, 64u, 1u};
                target_desc.arrayLength = 1u;
                target_desc.mipCount = 1u;
                target_desc.format = rhi::Format::RGBA8Unorm;
                target_desc.sampleCount = 1u;
                target_desc.memoryType = rhi::MemoryType::DeviceLocal;
                target_desc.usage = rhi::TextureUsage::RenderTarget;
                target_desc.defaultState = rhi::ResourceState::RenderTarget;
                target_desc.label = "run-gate target";
                if (const std::error_code ec = make_error_code(
                        rhi->getDevice().createTexture(target_desc, nullptr, m_target.writeRef()))) {
                    return ec;
                }

                m_passes_live = true;
                m_booted = true;
                return default_value_v;
            }

            [[nodiscard]] CameraSnapshot snapshot_() {
                CameraModel model{};
                model.m_camera_mode = ECameraProjection::orthographic;
                m_camera.updateModel(
                    kFrameStep, model, Viewport{PixelRect{int2{0, 0}, int2{800, 600}}, ViewportLayout{}});
                return m_camera.getSnapshot();
            }

            [[nodiscard]] std::error_code update(const TimeSpan dt) override {
                // Derived pre-super work marker: counted BEFORE super runs.
                ++m_updates;
                m_update_dts.push_back(dt);
                if (m_update_error) {
                    return m_update_error;
                }
                if (const std::error_code ec = Application::update(dt)) {
                    return ec;
                }

                const CameraSnapshot snapshot = snapshot_();
                if (const std::error_code ec = m_triangles.update(dt, snapshot)) {
                    return ec;
                }
                if (const std::error_code ec = m_grid.update(dt, snapshot)) {
                    return ec;
                }
                const GridTileSubmission staged[] = {
                    {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
                };
                if (const std::error_code ec = m_grid.submitTiles(staged)) {
                    return ec;
                }
                return m_triangles.submitInstance(m_tri_bag, m_tri_material, identity_v);
            }

            [[nodiscard]] std::error_code render() override {
                ++m_renders;
                if (m_render_error) {
                    return m_render_error;
                }
                if (const std::error_code ec = Application::render()) {
                    return ec;
                }
                Renderer &renderer = getRenderer();
                DrawSubmission grid_draws{m_grid};
                grid_draws.m_viewport = rhi::Viewport{.extentX = 64.0f, .extentY = 64.0f};
                grid_draws.m_scissor = rhi::ScissorRect{.maxX = 64u, .maxY = 64u};
                DrawSubmission tri_draws{m_triangles};
                tri_draws.m_viewport = rhi::Viewport{.extentX = 64.0f, .extentY = 64.0f};
                tri_draws.m_scissor = rhi::ScissorRect{.maxX = 64u, .maxY = 64u};
                const FenceCompletionValue previously_submitted = renderer.lastSubmittedFenceValue();
                if (const std::error_code ec = renderer.renderToTexture(*m_target, {grid_draws, tri_draws})) {
                    return ec;
                }
                const FenceCompletionValue accepted = renderer.lastSubmittedFenceValue();
                if (accepted != previously_submitted and accepted != kFenceValueNone) {
                    m_last_accepted = accepted;
                    m_grid.notifyFrameSubmitted(accepted);
                    m_triangles.notifyFrameSubmitted(accepted);
                }
                if (m_on_rendered) {
                    m_on_rendered(*this);
                }
                return default_value_v;
            }

            [[nodiscard]] std::error_code shutdown() override {
                if (m_shutdown_done) {
                    // Idempotent re-entry: the test scope-exit teardown runs
                    // after run()'s finishRun_ shutdown already released
                    // every owner (renderer included). Re-report the recorded
                    // outcome without touching released objects again — in
                    // particular, a recorded RemovalConfirmed disposition
                    // must not re-enter Editor::shutdown's notifyDeviceLost
                    // on the destroyed renderer (DeferredReleaseQueue::drainAll
                    // on garbage). First-error semantics preserved: the
                    // recorded result is returned unchanged.
                    return m_shutdown_result;
                }
                m_shutdown_result = shutdownOnce_();
                m_shutdown_done = true;
                return m_shutdown_result;
            }

            [[nodiscard]] std::error_code shutdownOnce_() {
                // Explicit offscreen-target release post-quiescence (owners
                // alive, idle proven) pre-RHI-shutdown: the texture holds a
                // strong device ref and RHI::destroy aborts on live children.
                if (const std::error_code ec = prepareGpuTeardown_()) {
                    // A failed prepare still releases what it can below only
                    // on a safe disposition; UNKNOWN fail-stops instead.
                    if (gpuTeardownDisposition() == GpuQuiescence::kUnknown) {
                        failStopOnUnknownGpuTeardown_(ec);
                    }
                }
                m_target.setNull();
                if (m_passes_live) {
                    m_passes_live = false;
                    if (const std::error_code ec = m_grid.shutdown()) {
                        return ec;
                    }
                    if (const std::error_code ec = m_triangles.shutdown()) {
                        return ec;
                    }
                }
                return ApplicationEditor::shutdown();
            }

            [[nodiscard]] std::error_code teardown() {
                return shutdown();
            }
        };

        PPR_UNIT_TEST (saturated_run_preserves_time_and_input) {
            // B2' admission + B3' saturation through real run(): 2 admitted
            // frames, K busy frames (collection grows, update/render/input-
            // dispatch counts frozen), gate opens, exact effective_dt with
            // ordered exactly-once delivery and single advancements.
            SaturatingHarness harness{"RunGateSaturation"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            ScriptedPlatform &platform = *harness.m_scripted;
            Renderer &renderer = harness.getRenderer();

            // No scope-exit hold release: run()'s shutdown destroys the
            // renderer (Application::shutdown resets m_renderer), so any
            // later touch of this reference is a use-after-free. The seam
            // needs no manual reset — the mid-run script releases it at pump
            // 5, and Renderer::shutdown clears it unconditionally
            // (App.Renderer.cpp:381) on every path.
            renderer.setTestHoldSweep(true);

            // Script per pump index: admitted pumps stage input; busy pumps
            // keep staging (collection must grow); pump 5 opens the gate;
            // the run ends after the third admitted update.
            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                switch (pump) {
                    case 1u: self.stageKey(EKeyboardKey::a, true); break;
                    case 2u:
                        self.stageKey(EKeyboardKey::a, false);
                        self.stageFocus(false);
                        self.stageDevice(true);
                        break;
                    case 3u:
                    case 4u:
                    case 5u: self.stageChar(static_cast<hal::native::char_t>(u'a' + pump)); break;
                    default: break;
                }
                if (pump == 5u) {
                    renderer.setTestHoldSweep(false);
                }
                if (harness.m_updates >= 3u) {
                    self.endRun();
                }
            };

            PPR_TEST_ASSERT(not harness.run());
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);

            // Positive execution around saturation: admitted, busy, admitted.
            PPR_TEST_ASSERT(harness.m_updates == 3u);
            PPR_TEST_ASSERT(harness.m_renders == 3u);
            PPR_TEST_ASSERT(platform.m_updates == 3u);
            PPR_TEST_ASSERT(platform.m_dispatches == 3u);
            PPR_TEST_ASSERT(platform.m_device_polls == 3u);
            PPR_TEST_ASSERT(platform.m_pumps > platform.m_updates);
            PPR_TEST_ASSERT(platform.m_update_pumps.size() == 3u);
            // Collection grew across the busy pumps (no dispatch, no drop):
            // pump indices are 1-based; sizes[2..4] cover busy pumps 3..5.
            PPR_TEST_ASSERT(platform.m_batch_sizes.size() == platform.m_pumps);
            PPR_TEST_ASSERT(platform.m_batch_sizes[2] < platform.m_batch_sizes[4]);
            // Exact carry: the third admitted dt is (busy + 1) steps, where
            // busy counts admitted-update pump gaps minus the admitted one.
            const u32 busy_frames = (platform.m_update_pumps[2] - platform.m_update_pumps[1]) - 1u;
            PPR_TEST_ASSERT(busy_frames >= 1u);
            PPR_TEST_ASSERT(harness.m_update_dts.size() == 3u);
            PPR_TEST_ASSERT(harness.m_update_dts[0] == kFrameStep);
            PPR_TEST_ASSERT(harness.m_update_dts[1] == kFrameStep);
            PPR_TEST_ASSERT(harness.m_update_dts[2] == (busy_frames + 1u) * kFrameStep);
            // Ordered exactly-once delivery across the pumps (input replay
            // plus deferred focus/hotplug halves, in collection order).
            const Array<std::string> expected_log{"key", "key", "focus:0", "device:1", "char", "char", "char"};
            PPR_TEST_ASSERT(platform.m_log.size() >= expected_log.size());
            for (u32 i = 0u; i < expected_log.size(); ++i) {
                PPR_TEST_ASSERT(platform.m_log[i] == expected_log[i]);
            }
            // Time advanced every frame (clock) and once per admitted frame
            // on devices (single advancement, never on busy — proven by
            // m_device_polls == m_updates above). Each advancement carries
            // that update's effective_dt, INCLUDING the busy carry on the
            // third update (that preservation is the point of B2', mirroring
            // production device polling): device time equals the admitted dt
            // sum, not m_updates single steps.
            PPR_TEST_ASSERT(harness.getTimerManager().getTotalElapsed() == platform.m_pumps * kFrameStep);
            TimeSpan admitted_total{};
            for (const TimeSpan dt: harness.m_update_dts) {
                admitted_total += dt;
            }
            PPR_TEST_ASSERT(platform.m_device_time == admitted_total);
            // Fence frontier advanced (credits returned after the open).
            // Post-shutdown the renderer is destroyed by
            // Application::shutdown, so the ledger cannot be queried
            // anymore; the drain is proven instead by the clean run()
            // return plus the idle-proven disposition (Renderer::shutdown
            // waits the last submission, sweeps, and drains on that path),
            // while the harness-captured frontier proves submissions
            // happened.
            PPR_TEST_ASSERT(harness.m_last_accepted != kFenceValueNone);
            PPR_TEST_ASSERT(harness.gpuTeardownDisposition() == GpuQuiescence::kIdleProven);
        };

        PPR_UNIT_TEST (terminal_probe_runs_zero_client_work) {
            // Terminal injection after the first admitted frame: the next
            // probe leaves BEFORE carry/update/render — zero update/render
            // calls on the terminal frame, including derived pre-super work
            // — and run() reports the terminal code.
            SaturatingHarness harness{"RunGateTerminal"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            ScriptedPlatform &platform = *harness.m_scripted;

            harness.m_on_rendered = [](SaturatingHarness &self) noexcept {
                if (self.m_renders == 1u) {
                    // Confirmed removal AFTER the first admitted frame.
                    self.getRenderer().notifyDeviceLost();
                }
            };
            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (pump >= 10u) {
                    self.endRun();
                }
            };

            PPR_TEST_ASSERT(harness.run() == std::make_error_code(std::errc::no_such_device_or_address));
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(harness.m_updates == 1u);
            PPR_TEST_ASSERT(harness.m_renders == 1u);
            PPR_TEST_ASSERT(platform.m_updates == 1u);
        };

        PPR_UNIT_TEST (first_error_precedence_clock_over_terminal) {
            // First-error combo: a clock (timer-callback) error is observed
            // before the probe, so it wins over the pre-injected terminal —
            // run() reports the clock error, never overwritten.
            SaturatingHarness harness{"RunGatePrecedence"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            ScriptedPlatform &platform = *harness.m_scripted;
            Renderer &renderer = harness.getRenderer();

            harness.getTimerManager().schedule(harness.m_clock.now(), [](const TimePoint) -> std::error_code {
                return std::make_error_code(std::errc::io_error);
            });
            renderer.notifyDeviceLost();
            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (pump >= 10u) {
                    self.endRun();
                }
            };

            PPR_TEST_ASSERT(harness.run() == std::make_error_code(std::errc::io_error));
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(harness.m_updates == 0u);
            PPR_TEST_ASSERT(harness.m_renders == 0u);
        };

        PPR_UNIT_TEST (busy_collection_failure_latches_first) {
            // Busy-branch collection failure: latched for run()'s return,
            // with update/render frozen from the start.
            SaturatingHarness harness{"RunGateBusyError"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            ScriptedPlatform &platform = *harness.m_scripted;
            Renderer &renderer = harness.getRenderer();
            // No scope-exit hold release (see the saturated test): the
            // renderer is destroyed by run()'s shutdown, and
            // Renderer::shutdown clears the seam unconditionally.
            renderer.setTestHoldSweep(true);

            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (pump >= 3u) {
                    platform.m_pump_error = std::make_error_code(std::errc::io_error);
                }
                if (pump >= 10u) {
                    self.endRun();
                }
            };

            PPR_TEST_ASSERT(harness.run() == std::make_error_code(std::errc::io_error));
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(harness.m_updates == 2u);
            PPR_TEST_ASSERT(harness.m_renders == 2u);
        };

        PPR_UNIT_TEST (sim_catch_up_preserves_skipped_time) {
            // Real ColonyDriver: one lumped effective_dt (dt + carry) ticks
            // the same fixed-step slices as the split frames would have —
            // GPU backpressure never loses sim time (5-slice clamp bounds a
            // single frame; the remainder stays in the defer backlog).
            colony::ColonyDriver lumped{};
            PPR_TEST_ASSERT(not lumped.init(colony::ColonyDriverDesc{.m_seed = 424242u}));
            PPR_DEFER {
                std::ignore = lumped.shutdown();
            };
            const u64 tick_before = lumped.tickCount();
            PPR_TEST_ASSERT(not lumped.update(std::chrono::milliseconds{50}));
            PPR_TEST_ASSERT(lumped.tickCount() == tick_before + 3u);

            colony::ColonyDriver split{};
            PPR_TEST_ASSERT(not split.init(colony::ColonyDriverDesc{.m_seed = 424242u}));
            PPR_DEFER {
                std::ignore = split.shutdown();
            };
            for (u32 i = 0u; i < 50u; ++i) {
                PPR_TEST_ASSERT(not split.update(std::chrono::milliseconds{1}));
            }
            PPR_TEST_ASSERT(split.tickCount() == lumped.tickCount());
        };

        PPR_UNIT_TEST (healthy_teardown_releases_in_order) {
            // Healthy path: normal return, every owner released, each
            // lifecycle stage exactly once with releases after idle proof.
            SaturatingHarness harness{"RunGateHealthy"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            ScriptedPlatform &platform = *harness.m_scripted;
            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (pump >= 2u) {
                    self.endRun();
                }
            };
            PPR_TEST_ASSERT(not harness.run());
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(harness.m_updates == 2u);

            const std::span<const std::string> stages = harness.gpuTeardownStageLog();
            const Array<std::string> expected{
                "quiescence", "idle-proven", "scene", "ui", "renderer", "rhi", "platform"};
            PPR_TEST_ASSERT(stages.size() == expected.size());
            for (u32 i = 0u; i < expected.size(); ++i) {
                PPR_TEST_ASSERT(stages[i] == expected[i]);
            }
        };

        PPR_UNIT_TEST (removal_confirmed_preserves_first_error) {
            // Removal-confirmed quiescence releases without touching the
            // device AND preserves the original error ahead of the removal
            // error (first-error precedence through finishRun_).
            SaturatingHarness harness{"RunGateRemoval"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            ScriptedPlatform &platform = *harness.m_scripted;
            Renderer &renderer = harness.getRenderer();
            harness.m_update_error = std::make_error_code(std::errc::io_error);
            // No scope-exit injection reset (same lifetime rule as the hold
            // seam above): the renderer is destroyed by run()'s shutdown,
            // and Renderer::shutdown clears the injected failure
            // unconditionally (App.Renderer.cpp:380) on every path.
            renderer.setInjectedQuiescenceFailureForTest(std::make_error_code(std::errc::no_such_device_or_address));

            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (pump >= 5u) {
                    self.endRun();
                }
            };

            // The update error (first) wins over the removal error (later).
            PPR_TEST_ASSERT(harness.run() == std::make_error_code(std::errc::io_error));
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(harness.gpuTeardownDisposition() == GpuQuiescence::kRemovalConfirmed);
            bool saw_removal = false;
            for (const std::string &stage: harness.gpuTeardownStageLog()) {
                saw_removal = saw_removal or stage == "removal-confirmed";
            }
            PPR_TEST_ASSERT(saw_removal);
        };

        // --- B2'c focus final-state closure ---------------------------------
        // Loss→gain collected across busy iterations: EACH subscriber
        // (window-level + service-level) receives false,true in order
        // exactly once. The live flag ends true, so replaying it instead of
        // the captured record would corrupt the first event.
        PPR_UNIT_TEST (focus_loss_gain_ordered_across_busy) {
            SaturatingHarness harness{"RunGateFocusOrder"};
            PPR_TEST_ASSERT(not harness.boot());
            ScriptedPlatform &platform = *harness.m_scripted;
            Renderer &renderer = harness.getRenderer();

            Array<bool> window_seen{};
            Array<bool> service_seen{};
            // Named subscriber storage: both sinks keep non-owning
            // function_ref views, so the lambdas must outlive run() — a
            // temporary subscribed here would dangle at the end of the full
            // expression and fire use-after-scope on a later admitted
            // dispatch. The captured arrays are test-scope locals, live
            // through run() and the assertions below.
            auto window_focus_recorder = [&](const Window &, const bool focused) { window_seen.push_back(focused); };
            auto service_focus_recorder = [&](const Window &, const bool focused) -> std::error_code {
                service_seen.push_back(focused);
                return default_value_v;
            };
            platform.m_window.m_when_focused.subscribe(window_focus_recorder);
            auto service_focus_handle = platform.m_window_service->whenWindowFocused(service_focus_recorder);
            PPR_DEFER {
                platform.m_window.m_when_focused.reset();
            };
            PPR_DEFER {
                std::ignore = harness.teardown();
            };

            // Hold the sweep so both focus arrivals queue across busy
            // iterations (collection grows, no dispatch), then open the gate
            // so one admitted frame replays both in collection order.
            renderer.setTestHoldSweep(true);
            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (pump == 3u) {
                    self.stageFocus(false);
                }
                if (pump == 4u) {
                    self.stageFocus(true);
                }
                if (pump == 5u) {
                    renderer.setTestHoldSweep(false);
                }
                if (harness.m_updates >= 4u) {
                    self.endRun();
                }
            };

            PPR_TEST_ASSERT(not harness.run());
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            // The live flag ends true: a subscriber fed from live state
            // would observe true,true — the assertions below discriminate.
            PPR_TEST_ASSERT(platform.m_window.m_focused == true);
            PPR_TEST_ASSERT(window_seen.size() == 2u);
            PPR_TEST_ASSERT(window_seen[0] == false);
            PPR_TEST_ASSERT(window_seen[1] == true);
            PPR_TEST_ASSERT(service_seen.size() == 2u);
            PPR_TEST_ASSERT(service_seen[0] == false);
            PPR_TEST_ASSERT(service_seen[1] == true);
        };

        // --- B1'a two-run epoch closure -----------------------------------
        // Two runs on ONE application: run 1 establishes idle; run 2
        // performs new GPU work under a held sweep (outstanding at
        // teardown) and must INDEPENDENTLY establish quiescence — the stage
        // log is a fresh epoch (quiescence + idle-proven re-recorded), never
        // a reuse of run 1's authorization.
        PPR_UNIT_TEST (two_runs_reestablish_quiescence_independently) {
            SaturatingHarness harness{"RunGateTwoRuns"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            ScriptedPlatform &platform = *harness.m_scripted;

            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (pump >= 2u) {
                    self.endRun();
                }
            };
            PPR_TEST_ASSERT(not harness.run());
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(harness.gpuTeardownDisposition() == GpuQuiescence::kIdleProven);
            {
                const std::span<const std::string> stages = harness.gpuTeardownStageLog();
                PPR_TEST_ASSERT(stages.size() == 7u);
                PPR_TEST_ASSERT(stages[0] == "quiescence");
                PPR_TEST_ASSERT(stages[1] == "idle-proven");
            }
            PPR_TEST_ASSERT(harness.m_last_accepted != kFenceValueNone);
            const u32 updates_run1 = harness.m_updates;

            // Fresh epoch on the same object: reopen the single-init guards
            // so run()'s initialize() re-acquires GPU resources (exercising
            // the initialize()-top reset; the run()-top reset covers the
            // pinned-initialize path on its own).
            harness.m_shutdown_done = false;
            harness.m_booted = false;
            PPR_TEST_ASSERT(not harness.boot());
            Renderer &renderer2 = harness.getRenderer();
            // Hold the sweep for the whole run: every run-2 submit stays
            // outstanding at teardown, so idle-proven below comes from a
            // genuine quiescence wait, not the frame-boundary sweep.
            renderer2.setTestHoldSweep(true);
            platform.m_pumps = 0u;
            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (pump >= 4u) {
                    self.endRun();
                }
            };
            PPR_TEST_ASSERT(not harness.run());
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(harness.m_updates > updates_run1);
            PPR_TEST_ASSERT(harness.m_last_accepted != kFenceValueNone);
            PPR_TEST_ASSERT(harness.gpuTeardownDisposition() == GpuQuiescence::kIdleProven);
            {
                const std::span<const std::string> stages = harness.gpuTeardownStageLog();
                PPR_TEST_ASSERT(stages.size() == 7u);
                PPR_TEST_ASSERT(stages[0] == "quiescence");
                PPR_TEST_ASSERT(stages[1] == "idle-proven");
            }
        };

        // --- B1'b best-effort diagnostics closure --------------------------
        // A throwing quiescence-entry diagnostic must not fail a proven-idle
        // cleanup: instrumentation is best-effort, release still completes,
        // and run() still succeeds.
        PPR_UNIT_TEST (healthy_teardown_survives_diagnostic_failure) {
            SaturatingHarness harness{"RunGateDiagFail"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            ScriptedPlatform &platform = *harness.m_scripted;
            harness.setGpuTeardownEntryHookForTest([] { throw std::runtime_error{"diagnostic failure"}; });
            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (pump >= 2u) {
                    self.endRun();
                }
            };
            PPR_TEST_ASSERT(not harness.run());
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(harness.gpuTeardownDisposition() == GpuQuiescence::kIdleProven);
            const std::span<const std::string> stages = harness.gpuTeardownStageLog();
            const Array<std::string> expected{
                "quiescence", "idle-proven", "scene", "ui", "renderer", "rhi", "platform"};
            PPR_TEST_ASSERT(stages.size() == expected.size());
            for (u32 i = 0u; i < expected.size(); ++i) {
                PPR_TEST_ASSERT(stages[i] == expected[i]);
            }
        };

        // --- B1' subprocess death test ----------------------------------
        // The child below runs ONLY when armed via the environment (the
        // parent arms it around spawnAndWait). Unarmed execution — including
        // full-suite runs — passes as a no-op so the runner never dies.

        [[nodiscard]] std::filesystem::path deathMarkerPath() {
            return std::filesystem::temp_directory_path() / "ppr-gate1-b1-markers.txt";
        }

        void writeDeathMarker(const char *line) noexcept {
            try {
                FILE *file = nullptr;
                if (::fopen_s(&file, deathMarkerPath().string().c_str(), "a") == 0 and file != nullptr) {
                    ::fputs(line, file);
                    ::fputs("\n", file);
                    ::fclose(file);
                }
            } catch (...) {
            }
        }

        void deathFailureHandler(const std::error_code quiescence_error) noexcept {
            writeDeathMarker("fail-stop-entry");
            writeDeathMarker(quiescence_error ? "unknown-recorded" : "unexpected-no-error");
        }

        struct DeathChild : ApplicationEditor {
            explicit DeathChild(const std::string_view name)
                : ApplicationEditor(name, std::span<const char *const>{}, &deathFailureHandler) {
            }

            ~DeathChild() noexcept override {
                writeDeathMarker("destroyed");
            }

            [[nodiscard]] std::error_code bootBase() {
                return Application::initialize();
            }

            [[nodiscard]] std::error_code teardown() {
                return ApplicationEditor::shutdown();
            }
        };

        [[nodiscard]] bool diagThrowArmed() noexcept {
            char *stored = nullptr;
            const bool armed = ::_dupenv_s(&stored, nullptr, "PPR_GATE1_B1_DIAG_THROW") == 0 and
                               stored != nullptr and
                               stored[0] == '1' and
                               stored[1] == '\0';
            std::free(stored);
            return armed;
        }

        // Error category whose formatting throws: any error_code::message()
        // on the fail-stop path (e.g. inside a logging retain macro) escapes
        // as if allocation failed. A marker records the attempt; absence of
        // that marker in the parent proves no formatting ran pre-fail-stop.
        struct ThrowingMessageCategory final : std::error_category {
            [[nodiscard]] const char *name() const noexcept override {
                return "ppr-gate1-throwing-message";
            }

            [[nodiscard]] std::string message(int) const override {
                writeDeathMarker("message-formatted");
                throw std::bad_alloc{};
            }
        };

        [[nodiscard]] const ThrowingMessageCategory &throwingMessageCategory() noexcept {
            static const ThrowingMessageCategory category{};
            return category;
        }

        [[nodiscard]] bool messageThrowArmed() noexcept {
            char *stored = nullptr;
            const bool armed = ::_dupenv_s(&stored, nullptr, "PPR_GATE1_B1_MESSAGE_THROW") == 0 and
                               stored != nullptr and
                               stored[0] == '1' and
                               stored[1] == '\0';
            std::free(stored);
            return armed;
        }

        void runUnknownCompletionDeathChild() {
            // Resource-bearing Editor fixture: base-init (platform + RHI +
            // renderer live) plus real Grid/Triangle passes with staged
            // tiles and two stamped offscreen submits held pending (no
            // pre-drain); the injected io_error drain failure has NO
            // removal marking (healthy fence) → UNKNOWN → fail-stop.
            DeathChild editor{"Gate1B1DeathChild"};
            PPR_TEST_ASSERT(not editor.bootBase());

            const safe_ptr<IRhiService> rhi = editor.getServices().get<IRhiService>();
            const safe_ptr<IShaderService> shaders = editor.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(rhi.isValid());
            PPR_TEST_ASSERT(shaders.isValid());
            rhi::IDevice &device = rhi->getDevice();

            GridPass grid{};
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shaders, editor.getContentDir().path()));
            TrianglePass triangles{};
            PPR_TEST_ASSERT(not triangles.initialize(*rhi, *shaders, editor.getContentDir().path()));

            Camera camera{};
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            camera.updateModel(kFrameStep, model, Viewport{PixelRect{int2{0, 0}, int2{800, 600}}, ViewportLayout{}});
            PPR_TEST_ASSERT(not grid.update(kFrameStep, camera.getSnapshot()));
            PPR_TEST_ASSERT(not triangles.update(kFrameStep, camera.getSnapshot()));
            const GridTileSubmission staged[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
            };
            PPR_TEST_ASSERT(not grid.submitTiles(staged));

            rhi::TextureDesc target_desc{};
            target_desc.type = rhi::TextureType::Texture2D;
            target_desc.size = {64u, 64u, 1u};
            target_desc.arrayLength = 1u;
            target_desc.mipCount = 1u;
            target_desc.format = rhi::Format::RGBA8Unorm;
            target_desc.sampleCount = 1u;
            target_desc.memoryType = rhi::MemoryType::DeviceLocal;
            target_desc.usage = rhi::TextureUsage::RenderTarget;
            target_desc.defaultState = rhi::ResourceState::RenderTarget;
            target_desc.label = "b1 death target";
            rhi::ComPtr<rhi::ITexture> target{};
            PPR_TEST_ASSERT(not make_error_code(device.createTexture(target_desc, nullptr, target.writeRef())));

            Renderer &renderer = editor.getRenderer();
            renderer.setTestHoldSweep(true);
            DrawSubmission grid_draws{grid};
            grid_draws.m_viewport = rhi::Viewport{.extentX = 64.0f, .extentY = 64.0f};
            grid_draws.m_scissor = rhi::ScissorRect{.maxX = 64u, .maxY = 64u};
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {grid_draws}));
            const FenceCompletionValue first = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {grid_draws}));
            const FenceCompletionValue second = renderer.lastSubmittedFenceValue();
            grid.notifyFrameSubmitted(second);
            triangles.notifyFrameSubmitted(second);
            // Truly pending: hold suppresses the sweep, so completion is
            // still below both accepted values at quiescence entry.
            PPR_TEST_ASSERT(renderer.completedFenceValue() < first);
            PPR_TEST_ASSERT(first < second);

            // Injected drain failure WITHOUT removal marking (healthy fence
            // → UNKNOWN). The message-throw variant (armed by the parent via
            // PPR_GATE1_B1_MESSAGE_THROW) additionally makes error-message
            // formatting throw, so any allocating diagnostic left on the
            // fail-stop path would escape without _Exit(0xB1).
            if (messageThrowArmed()) {
                writeDeathMarker("message-throw-armed");
                renderer.setInjectedQuiescenceFailureForTest(std::error_code{42, throwingMessageCategory()});
            } else {
                renderer.setInjectedQuiescenceFailureForTest(std::make_error_code(std::errc::io_error));
            }
            editor.setGpuTeardownEntryHookForTest([&] {
                writeDeathMarker("quiescence-entry");
                // Owners alive, nothing released yet: passes still hold
                // their stamped slots and completion is below both values.
                const bool alive = maxSlotValue(grid.slotCompletionValues()) == second and
                                   maxSlotValue(triangles.slotCompletionValues()) == second and
                                   renderer.completedFenceValue() < first;
                writeDeathMarker(alive ? "owners-alive" : "owners-dead");
                // Deterministic diagnostic failure (armed by the parent via
                // PPR_GATE1_B1_DIAG_THROW): best-effort instrumentation must
                // not gate the UNKNOWN fail-stop below.
                if (diagThrowArmed()) {
                    writeDeathMarker("diag-throw-armed");
                    throw std::runtime_error{"b1 diagnostic failure"};
                }
            });

            // Real shutdown WITHOUT pre-drain → UNKNOWN → fail-stop.
            // Unreachable past this call: no normal return, no destruction.
            std::ignore = editor.teardown();
            writeDeathMarker("normal-return");
            writeDeathMarker("release-begun");
        }

        PPR_UNIT_TEST (unknown_completion_child_fatal) {
            char *stored = nullptr;
            const bool armed = ::_dupenv_s(&stored, nullptr, "PPR_GATE1_B1_DEATH_ARMED") == 0 and
                               stored != nullptr and
                               stored[0] == '1' and
                               stored[1] == '\0';
            std::free(stored);
            if (not armed) {
                return;
            }
            runUnknownCompletionDeathChild();
        };

        void setDeathEnv(const char *value) {
#ifdef _WIN32
            const std::string armed = value != nullptr ? value : "0";
            ::_putenv_s("PPR_GATE1_B1_DEATH_ARMED", armed.c_str());
#else
            if (value != nullptr) {
                ::setenv("PPR_GATE1_B1_DEATH_ARMED", value, 1);
            } else {
                ::unsetenv("PPR_GATE1_B1_DEATH_ARMED");
            }
#endif
        }

        void setDiagThrowEnv(const char *value) {
#ifdef _WIN32
            const std::string armed = value != nullptr ? value : "0";
            ::_putenv_s("PPR_GATE1_B1_DIAG_THROW", armed.c_str());
#else
            if (value != nullptr) {
                ::setenv("PPR_GATE1_B1_DIAG_THROW", value, 1);
            } else {
                ::unsetenv("PPR_GATE1_B1_DIAG_THROW");
            }
#endif
        }

        void setMessageThrowEnv(const char *value) {
#ifdef _WIN32
            const std::string armed = value != nullptr ? value : "0";
            ::_putenv_s("PPR_GATE1_B1_MESSAGE_THROW", armed.c_str());
#else
            if (value != nullptr) {
                ::setenv("PPR_GATE1_B1_MESSAGE_THROW", value, 1);
            } else {
                ::unsetenv("PPR_GATE1_B1_MESSAGE_THROW");
            }
#endif
        }

        PPR_UNIT_TEST (unknown_completion_fail_stops_process) {
            // Subprocess death test: the armed child hits UNKNOWN at final
            // teardown and must die with the explicit fatal status after
            // recording quiescence/UNKNOWN/fatal-entry — with NO release,
            // normal-return, or destruction markers.
            std::error_code fs_ec{};
            std::filesystem::remove(deathMarkerPath(), fs_ec);

            // Resolve the executable BEFORE arming: the path lookup may
            // throw while spawnAndWait itself is noexcept, so the armed flag
            // can never leak into a full-suite run on a spawn failure.
            const std::filesystem::path exe_path = hal::process::currentExecutablePath();
            setDeathEnv("1");
            const std::vector<std::string> child_args{
                "--child-run", "--run-test", "app/run_gate/unknown_completion_child_fatal"};
            const int exit_code = hal::process::spawnAndWait(exe_path, child_args);
            setDeathEnv(nullptr);

            PPR_TEST_ASSERT(exit_code == kUnknownGpuCompletionFatalExit);

            std::string markers{};
            FILE *markers_file = nullptr;
            if (::fopen_s(&markers_file, deathMarkerPath().string().c_str(), "r") == 0 and markers_file != nullptr) {
                char line[256];
                while (::fgets(line, sizeof(line), markers_file) != nullptr) {
                    markers += line;
                }
                ::fclose(markers_file);
            }
            PPR_TEST_ASSERT(markers.find("quiescence-entry") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("owners-alive") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("unknown-recorded") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("fail-stop-entry") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("normal-return") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("release-begun") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("destroyed") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("owners-dead") == std::string::npos);
        };

        PPR_UNIT_TEST (unknown_completion_message_throw_still_fail_stops) {
            // Logging-boundary closure: direct shutdown with UNKNOWN plus a
            // deterministic failure during error-message formatting. The
            // child must still die with the explicit fail-stop status (no
            // release/destruction markers), and the absent
            // "message-formatted" marker proves no allocating diagnostic ran
            // on the path before _Exit(0xB1).
            std::error_code fs_ec{};
            std::filesystem::remove(deathMarkerPath(), fs_ec);

            const std::filesystem::path exe_path = hal::process::currentExecutablePath();
            setDeathEnv("1");
            setMessageThrowEnv("1");
            const std::vector<std::string> child_args{
                "--child-run", "--run-test", "app/run_gate/unknown_completion_child_fatal"};
            const int exit_code = hal::process::spawnAndWait(exe_path, child_args);
            setMessageThrowEnv(nullptr);
            setDeathEnv(nullptr);

            PPR_TEST_ASSERT(exit_code == kUnknownGpuCompletionFatalExit);

            std::string markers{};
            FILE *markers_file = nullptr;
            if (::fopen_s(&markers_file, deathMarkerPath().string().c_str(), "r") == 0 and markers_file != nullptr) {
                char line[256];
                while (::fgets(line, sizeof(line), markers_file) != nullptr) {
                    markers += line;
                }
                ::fclose(markers_file);
            }
            // Armed (custom-category failure injected), fail-stop result
            // identical to the clean path, and crucially no formatting ran.
            PPR_TEST_ASSERT(markers.find("message-throw-armed") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("message-formatted") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("quiescence-entry") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("owners-alive") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("unknown-recorded") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("fail-stop-entry") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("normal-return") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("release-begun") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("destroyed") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("owners-dead") == std::string::npos);
        };

        PPR_UNIT_TEST (unknown_completion_diag_fail_still_fail_stops) {
            // B1'b deterministic diagnostic failure on UNKNOWN: the child's
            // quiescence-entry hook throws AND the failure is recorded via
            // markers — the child must still die with the explicit
            // fail-stop status (asserted result, not merely exit), with NO
            // release, normal-return, or destruction markers.
            std::error_code fs_ec{};
            std::filesystem::remove(deathMarkerPath(), fs_ec);

            const std::filesystem::path exe_path = hal::process::currentExecutablePath();
            setDeathEnv("1");
            setDiagThrowEnv("1");
            const std::vector<std::string> child_args{
                "--child-run", "--run-test", "app/run_gate/unknown_completion_child_fatal"};
            const int exit_code = hal::process::spawnAndWait(exe_path, child_args);
            setDiagThrowEnv(nullptr);
            setDeathEnv(nullptr);

            PPR_TEST_ASSERT(exit_code == kUnknownGpuCompletionFatalExit);

            std::string markers{};
            FILE *markers_file = nullptr;
            if (::fopen_s(&markers_file, deathMarkerPath().string().c_str(), "r") == 0 and markers_file != nullptr) {
                char line[256];
                while (::fgets(line, sizeof(line), markers_file) != nullptr) {
                    markers += line;
                }
                ::fclose(markers_file);
            }
            // Deterministic proof the failure was active, then the same
            // fail-stop result as the clean path.
            PPR_TEST_ASSERT(markers.find("diag-throw-armed") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("quiescence-entry") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("owners-alive") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("unknown-recorded") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("fail-stop-entry") != std::string::npos);
            PPR_TEST_ASSERT(markers.find("normal-return") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("release-begun") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("destroyed") == std::string::npos);
            PPR_TEST_ASSERT(markers.find("owners-dead") == std::string::npos);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest run_gate = UnitTest::Named("run_gate") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::RunGate::saturated_run_preserves_time_and_input,
            detail::RunGate::terminal_probe_runs_zero_client_work,
            detail::RunGate::first_error_precedence_clock_over_terminal,
            detail::RunGate::busy_collection_failure_latches_first,
            detail::RunGate::sim_catch_up_preserves_skipped_time,
            detail::RunGate::healthy_teardown_releases_in_order,
            detail::RunGate::removal_confirmed_preserves_first_error,
            detail::RunGate::focus_loss_gain_ordered_across_busy,
            detail::RunGate::two_runs_reestablish_quiescence_independently,
            detail::RunGate::healthy_teardown_survives_diagnostic_failure,
            detail::RunGate::unknown_completion_child_fatal,
            detail::RunGate::unknown_completion_fail_stops_process,
            detail::RunGate::unknown_completion_diag_fail_still_fail_stops,
            detail::RunGate::unknown_completion_message_throw_still_fail_stops,
        });
    };

    const UnitTest &run_gateTests() noexcept {
        return run_gate;
    }
} // namespace pP::tests
