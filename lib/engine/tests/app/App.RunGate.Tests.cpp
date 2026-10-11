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

        // ora-23 measurement-only boot-phase timing (PERMANENT, silent by
        // default): PPR_RUNGATE_BOOT_TIMING=1 enables one stderr line per
        // boot phase (phase name + elapsed ms). File idiom is std::fprintf
        // to stderr (see the [lane-gate:*] evidence summaries); no
        // PPR_LOG category exists in this file, so env-gated fprintf keeps
        // it silent by default without new members or hot-path allocation
        // (one env check per boot/first-submit, boot paths only).
        [[nodiscard]] bool runGateBootTimingEnabled() noexcept {
#ifdef _WIN32
            char *stored = nullptr;
            const bool enabled = ::_dupenv_s(&stored, nullptr, "PPR_RUNGATE_BOOT_TIMING") == 0 and
                                 stored != nullptr and
                                 stored[0] == '1' and
                                 stored[1] == '\0';
            std::free(stored);
            return enabled;
#else
            const char *value = std::getenv("PPR_RUNGATE_BOOT_TIMING");
            return value != nullptr and value[0] == '1' and value[1] == '\0';
#endif
        }

        [[nodiscard]] long long runGateBootTimingMsSince(const std::chrono::steady_clock::time_point since) noexcept {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since)
                .count();
        }

        void runGateBootTimingLogIf(const bool enabled, const char *phase, const long long ms) noexcept {
            if (not enabled) {
                return;
            }
            try {
                std::fprintf(stderr, "[run-gate:boot-timing] %s %lld ms\n", phase, ms);
            } catch (...) {
            }
        }

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
            std23::move_only_function<std::error_code(TimeSpan)> m_on_wait{};

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

            [[nodiscard]] std::error_code waitNativeEvents(const TimeSpan timeout) override {
                if (m_on_wait) {
                    return m_on_wait(timeout);
                }
                return IPlatform::waitNativeEvents(timeout);
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
                std::error_code m_set_monitor_error{};
                IWindowService::ECursorMode m_cursor_mode{IWindowService::ECursorMode::normal};
                bool m_close_requested{false};
                std::string m_clipboard{};
                bool m_display_enabled{false};
                bool m_monitor_present{true};
                Monitor m_display_monitor{
                    MonitorHandle{}, "scripted", VideoMode{int2{1920, 1080}, int3{}, 60}, int2{}, int2{}, true};
                WindowDisplayGeometry m_display_geometry{int2{20, 30}, int2{800, 600}, int2{1600, 1200}};
                Array<std::thread::id> m_display_threads{};
                Array<std::error_code> m_display_errors{};
                u32 m_display_calls{0u};

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

                [[nodiscard]] std::error_code enumerateMonitors(
                    Collector<SharedMonitor> collect) const noexcept override {
                    if (m_display_enabled and m_monitor_present) {
                        return collect(SharedMonitor{&m_display_monitor});
                    }
                    return default_value_v;
                }

                [[nodiscard]] SharedMonitor getPrimaryMonitor() const noexcept override {
                    return {};
                }

                [[nodiscard]] std::error_code enumerateMonitorVideoModes(
                    const Monitor &, Collector<VideoMode> collect) const noexcept override {
                    if (m_display_enabled and m_monitor_present) {
                        return collect(m_display_monitor.m_video_mode);
                    }
                    return default_value_v;
                }

                bool supportsDisplayWindowChanges() const noexcept override {
                    return m_display_enabled;
                }
                bool supportsExclusiveRefresh() const noexcept override {
                    return m_display_enabled;
                }
                Expected<WindowDisplayGeometry> displayWindowGeometry(const Window &) const override {
                    return m_display_geometry;
                }
                Expected<WindowDisplayGeometry> configureDisplayWindow(
                    const Window &window, const Monitor *monitor, const int2 &position, const int2 &size,
                    std::optional<u32> refresh) override {
                    m_display_threads.push_back(std::this_thread::get_id());
                    const u32 call = m_display_calls++;
                    if (call < m_display_errors.size() and m_display_errors[call]) {
                        return std::unexpected{m_display_errors[call]};
                    }
                    m_display_geometry = {
                        position, size, int2{size.x * 2, size.y * 2}, refresh, monitor != nullptr,
                        monitor != nullptr ? monitor->m_name : ""};
                    if (window.m_handle == m_owner->m_window.m_handle) {
                        m_owner->m_window.m_window_position = position;
                        m_owner->m_window.m_window_size = size;
                        m_owner->m_window.m_framebuffer_size = m_display_geometry.m_framebuffer_size;
                    }
                    return m_display_geometry;
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

                [[nodiscard]] std::error_code setWindowMonitor(
                    const Window &, const Monitor &, const int2 &, const int2 &) override {
                    return m_set_monitor_error;
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

        PPR_UNIT_TEST (monitor_change_and_revert_report_operational_errors) {
            ScriptedPlatform platform{};
            Monitor monitor{MonitorHandle{}, "scripted", VideoMode{}, int2{}, int2{}, false};
            IWindowService &windows = *platform.getWindowService();
            const int2 position{0, 0};
            const int2 size{800, 600};

            PPR_TEST_ASSERT(not windows.setWindowMonitor(platform.m_window, monitor, position, size));

            const std::error_code apply_error = std::make_error_code(std::errc::io_error);
            platform.m_window_service->m_set_monitor_error = apply_error;
            const std::error_code apply_result = windows.setWindowMonitor(
                platform.m_window, monitor, position, int2{1920, 1080});
            PPR_TEST_ASSERT(apply_result == apply_error);

            // Rollback uses the same operational boundary, not a notification
            // callback: its failure remains available to the transaction caller.
            const std::error_code revert_error = std::make_error_code(std::errc::state_not_recoverable);
            platform.m_window_service->m_set_monitor_error = revert_error;
            const std::error_code revert_result = windows.setWindowMonitor(platform.m_window, monitor, position, size);
            PPR_TEST_ASSERT(revert_result == revert_error);
        };

        // The injected clock must be fully constructed BEFORE the
        // Application base (which holds a non-owning view of it): a base
        // class is initialized before the derived Editor base, unlike a
        // member. ScriptedPlatform has no such constraint (factory call).
        struct HarnessClock {
            FakeClock m_clock{};
        };

        struct WindowTransactionHarness final : HarnessClock, Application {
            ScriptedPlatform *m_scripted{};
            WindowTransactionHarness()
                : HarnessClock{},
                  Application(
                      ApplicationDomain{.m_is_interactive = false, .m_needs_user_interface = false}, "display-window",
                      {}, std::make_unique<ScriptedPlatform>(), HarnessClock::m_clock) {
                m_scripted = static_cast<ScriptedPlatform *>(const_cast<IPlatform *>(&getPlatform()));
                auto &service = *m_scripted->m_window_service;
                service.m_display_enabled = true;
                service.m_main_window = SharedWindow{&m_scripted->m_window};
            }
            ~WindowTransactionHarness() noexcept override {
                std::ignore = shutdown();
            }
            std::error_code boot() {
                const std::error_code error = initialize();
                if (not error) {
                    bindDisplaySurface_(m_scripted->m_window.m_handle, 7u);
                }
                return error;
            }
            void executor(RenderSurfaceExecutor execute) {
                renderLane()->setSurfaceExecutor(std::move(execute));
            }
            void frameExecutor(RenderFrameExecutor execute) {
                renderLane()->setExecutor(std::move(execute));
            }
            std::error_code queueFrame(const u64 id) {
                auto packet = std::make_shared<FramePacket>();
                packet->m_frame_id = id;
                return renderLane()->submitFrame(RenderFrameRequest{.m_packet = std::move(packet)});
            }
            std::error_code laneAdmission() {
                return getRenderer().checkFrameAdmission();
            }
            std::error_code pump() {
                return pumpRenderCompletions_();
            }
            std::error_code synchronize() {
                bool ran = false;
                const auto error = renderLane()->runOnLaneSync([]() -> std::error_code { return {}; }, &ran);
                return ran ? error : std::make_error_code(std::errc::not_connected);
            }
            std::error_code synchronize(RenderLaneOperation operation, bool &ran) {
                return renderLane()->runOnLaneSync(std::move(operation), &ran);
            }
            void recreate() {
                bindDisplaySurface_(m_scripted->m_window.m_handle, 99u);
            }
            std::error_code close() {
                return shutdown();
            }
        };

        config::Resolved<DisplaySettings> fullscreenCandidate() {
            config::Resolved<DisplaySettings> candidate{};
            candidate.m_value.m_window_mode = EDisplayWindowMode::fullscreen;
            candidate.m_value.m_monitor = "scripted";
            candidate.m_value.m_width = 1920u;
            candidate.m_value.m_height = 1080u;
            candidate.m_value.m_refresh_hz = 60u;
            return candidate;
        }

        PPR_UNIT_TEST (throwing_native_pump_and_wait_shutdown_retains_first_error) {
            for (const bool throw_first: {false, true}) {
                std::binary_semaphore release{0};
                bool released = false;
                u32 waits = 0u;
                WindowTransactionHarness test_app{};
                PPR_TEST_ASSERT(not test_app.boot());
                PPR_DEFER {
                    if (not released) {
                        release.release();
                    }
                    std::ignore = test_app.close();
                };
                const std::error_code first = std::make_error_code(std::errc::io_error);
                test_app.setGpuTeardownEntryHookForTest([] { throw std::bad_alloc{}; });
                test_app.m_scripted->m_pump_error = first;
                test_app.m_scripted->m_on_pump = [throw_first, first](ScriptedPlatform &, u32) {
                    if (throw_first) {
                        throw std::system_error{first};
                    }
                };
                test_app.m_scripted->m_on_wait = [&](TimeSpan) -> std::error_code {
                    ++waits;
                    if (not released) {
                        released = true;
                        release.release();
                    }
                    throw std::system_error{std::make_error_code(std::errc::timed_out)};
                };
                bool ran = false;
                PPR_TEST_ASSERT(not test_app.synchronize(
                    [&]() -> std::error_code {
                        release.acquire();
                        return {};
                    },
                    ran));
                PPR_TEST_ASSERT(ran);
                PPR_TEST_ASSERT(waits >= 1u);
                PPR_TEST_ASSERT(test_app.m_scripted->m_pumps >= 1u);
                PPR_TEST_ASSERT(test_app.close() == first);
                PPR_TEST_ASSERT(test_app.gpuTeardownDisposition() == GpuQuiescence::kIdleProven);
                PPR_TEST_ASSERT(not test_app.getServices().tryGet<IRhiService>().isValid());
                const Array<std::string> expected{"quiescence", "idle-proven", "renderer", "rhi", "platform"};
                const auto stages = test_app.gpuTeardownStageLog();
                PPR_TEST_ASSERT(stages.size() == expected.size());
                for (u32 index = 0u; index < expected.size(); ++index) {
                    PPR_TEST_ASSERT(stages[index] == expected[index]);
                }
                test_app.m_scripted->m_on_pump = {};
                test_app.m_scripted->m_on_wait = {};
                test_app.m_scripted->m_pump_error = {};
                PPR_TEST_ASSERT(not test_app.boot());
                PPR_TEST_ASSERT(not test_app.close());
            }
        };

        PPR_UNIT_TEST (shutdown_drops_queued_frames_before_backend_quarantine) {
            std::binary_semaphore entered{0};
            std::binary_semaphore release{0};
            bool released = false;
            std::atomic<u32> executions{};
            WindowTransactionHarness test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                if (not released) {
                    release.release();
                }
                std::ignore = test_app.close();
            };
            test_app.frameExecutor([&](const RenderFrameRequest &, RenderFrameResult &) -> std::error_code {
                const auto execution = executions.fetch_add(1u);
                if (execution == 0u) {
                    entered.release();
                    if (not release.try_acquire_for(std::chrono::seconds{10})) {
                        return std::make_error_code(std::errc::timed_out);
                    }
                }
                return test_app.laneAdmission();
            });
            PPR_TEST_ASSERT(not test_app.queueFrame(1u));
            PPR_TEST_ASSERT(entered.try_acquire_for(std::chrono::seconds{10}));
            PPR_TEST_ASSERT(not test_app.queueFrame(2u));

            // The teardown handoff is posted before its native wait releases
            // the executing frame. The second accepted frame stays queued.
            test_app.m_scripted->m_on_wait = [&](TimeSpan) -> std::error_code {
                if (not released) {
                    released = true;
                    release.release();
                }
                return {};
            };
            PPR_TEST_ASSERT(not test_app.close());
            PPR_TEST_ASSERT(released);
            PPR_TEST_ASSERT(executions.load() == 1u);
            PPR_TEST_ASSERT(test_app.gpuTeardownDisposition() == GpuQuiescence::kIdleProven);
            test_app.m_scripted->m_on_wait = {};
        };

        PPR_UNIT_TEST (display_window_mode_ack_while_suspended_and_revert) {
            WindowTransactionHarness test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            std::binary_semaphore entered{0}, release{0};
            bool released = false;
            PPR_DEFER {
                if (not released) {
                    release.release();
                }
                std::ignore = test_app.close();
            };
            std::atomic<u32> calls{0u};
            int2 requested{};
            test_app.executor([&](const RenderSurfaceRequest &request, RenderSurfaceResult &result) {
                const u32 call = calls.fetch_add(1u);
                requested = request.m_settings.m_extent.value_or(int2{});
                if (call == 0u) {
                    entered.release();
                    release.acquire();
                }
                result.m_applied = {8u + call, requested, true, true};
            });
            const auto prior = test_app.displayController().resolved();
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(fullscreenCandidate()));
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(entered.try_acquire_for(std::chrono::seconds{30}));
            PPR_TEST_ASSERT(test_app.displayController().admissionSuspended());
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(
                test_app.m_scripted->m_window_service->m_display_threads.front() == std::this_thread::get_id());
            release.release();
            released = true;
            PPR_TEST_ASSERT(not test_app.synchronize());
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(requested.x == 3840 and requested.y == 2160);
            PPR_TEST_ASSERT(test_app.displayController().committed().m_window_mode == EDisplayWindowMode::fullscreen);
            PPR_TEST_ASSERT(not test_app.displayController().admissionSuspended());
            // Exclusive fullscreen can be iconified while a timed trial expires.
            // Native zero sizes must not erase the acknowledged rollback baseline.
            test_app.m_scripted->m_window.m_iconified = true;
            auto &native_geometry = test_app.m_scripted->m_window_service->m_display_geometry;
            native_geometry.m_size = int2{0, 0};
            native_geometry.m_framebuffer_size = int2{0, 0};
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(prior));
            const auto revert_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            while (test_app.displayController().committed().m_window_mode != EDisplayWindowMode::windowed) {
                PPR_TEST_ASSERT(std::chrono::steady_clock::now() < revert_deadline);
                PPR_TEST_ASSERT(not test_app.pump());
                PPR_TEST_ASSERT(not test_app.synchronize());
                PPR_TEST_ASSERT(not test_app.pump());
            }
            PPR_TEST_ASSERT(test_app.displayController().committed().m_window_mode == EDisplayWindowMode::windowed);
            PPR_TEST_ASSERT(requested.x == 1600 and requested.y == 1200);
            PPR_TEST_ASSERT(test_app.m_scripted->m_window_service->m_display_calls == 2u);
            PPR_TEST_ASSERT(not test_app.close());
        };

        PPR_UNIT_TEST (display_window_minimized_revert_failure_preserves_errors) {
            WindowTransactionHarness test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                std::ignore = test_app.close();
            };
            test_app.executor([](const RenderSurfaceRequest &request, RenderSurfaceResult &result) {
                result.m_applied = {8u, *request.m_settings.m_extent, true, true};
            });
            const auto prior = test_app.displayController().resolved();
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(fullscreenCandidate()));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            while (test_app.displayController().committed().m_window_mode != EDisplayWindowMode::fullscreen) {
                PPR_TEST_ASSERT(std::chrono::steady_clock::now() < deadline);
                PPR_TEST_ASSERT(not test_app.pump());
                PPR_TEST_ASSERT(not test_app.synchronize());
                PPR_TEST_ASSERT(not test_app.pump());
            }
            auto &service = *test_app.m_scripted->m_window_service;
            test_app.m_scripted->m_window.m_iconified = true;
            service.m_display_geometry.m_size = int2{0, 0};
            service.m_display_geometry.m_framebuffer_size = int2{0, 0};
            const auto apply_error = std::make_error_code(std::errc::io_error);
            const auto restore_error = std::make_error_code(std::errc::permission_denied);
            service.m_display_errors.push_back(std::error_code{});
            service.m_display_errors.push_back(apply_error);
            service.m_display_errors.push_back(restore_error);
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(prior));
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(test_app.displayController().restoring());
            PPR_TEST_ASSERT(test_app.pump() == apply_error);
            PPR_TEST_ASSERT(test_app.displayController().applied().m_restoration_error == restore_error);
            PPR_TEST_ASSERT(service.m_display_calls == 3u);
            PPR_TEST_ASSERT(test_app.displayController().transactionState() == EDisplayTransaction::degraded);
            PPR_TEST_ASSERT(test_app.displayController().transactionError() == apply_error);
            PPR_TEST_ASSERT(test_app.displayController().committed().m_window_mode == EDisplayWindowMode::fullscreen);
            PPR_TEST_ASSERT(test_app.close() == apply_error);
        };

        PPR_UNIT_TEST (display_window_stale_generation_never_applies_native_mode) {
            WindowTransactionHarness test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(fullscreenCandidate()));
            test_app.recreate();
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(test_app.m_scripted->m_window_service->m_display_calls == 0u);
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(not test_app.close());
        };

        PPR_UNIT_TEST (display_window_failed_apply_and_restore_are_honest) {
            WindowTransactionHarness test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            const auto apply_error = std::make_error_code(std::errc::io_error);
            const auto restore_error = std::make_error_code(std::errc::permission_denied);
            auto &service = *test_app.m_scripted->m_window_service;
            service.m_display_errors.push_back(apply_error);
            service.m_display_errors.push_back(restore_error);
            std::atomic<u32> executions{0u};
            test_app.executor([&](const RenderSurfaceRequest &, RenderSurfaceResult &) { executions.fetch_add(1u); });
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(fullscreenCandidate()));
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(test_app.displayController().restoring());
            PPR_TEST_ASSERT(test_app.pump() == apply_error);
            PPR_TEST_ASSERT(service.m_display_calls == 2u and executions.load() == 0u);
            PPR_TEST_ASSERT(test_app.displayController().transactionState() == EDisplayTransaction::degraded);
            PPR_TEST_ASSERT(test_app.displayController().applied().m_restoration_error == restore_error);
            PPR_TEST_ASSERT(not test_app.displayController().applied().m_available);
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(test_app.close() == apply_error);
        };

        PPR_UNIT_TEST (display_window_reenumeration_rejects_removed_monitor_before_apply) {
            WindowTransactionHarness test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(fullscreenCandidate()));
            auto &service = *test_app.m_scripted->m_window_service;
            service.m_monitor_present = false;
            test_app.executor([](const RenderSurfaceRequest &request, RenderSurfaceResult &result) {
                result.m_applied = {8u, *request.m_settings.m_extent, true, true};
            });
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(test_app.displayController().restoring());
            PPR_TEST_ASSERT(service.m_display_calls == 0u);
            const auto rejection_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            while (test_app.displayController().transactionState() != EDisplayTransaction::idle) {
                PPR_TEST_ASSERT(std::chrono::steady_clock::now() < rejection_deadline);
                PPR_TEST_ASSERT(not test_app.pump());
                PPR_TEST_ASSERT(not test_app.synchronize());
                PPR_TEST_ASSERT(not test_app.pump());
            }
            PPR_TEST_ASSERT(test_app.displayController().transactionState() == EDisplayTransaction::idle);
            PPR_TEST_ASSERT(
                test_app.displayController().transactionError() == std::make_error_code(std::errc::no_such_device));
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(not test_app.close());
        };

        PPR_UNIT_TEST (display_window_surface_failure_restores_native_geometry) {
            WindowTransactionHarness test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            std::atomic<u32> executions{0u};
            int2 restored_extent{};
            test_app.executor([&](const RenderSurfaceRequest &request, RenderSurfaceResult &result) {
                const u32 call = executions.fetch_add(1u);
                result.m_applied = {8u + call, *request.m_settings.m_extent, true, true};
                if (call == 0u) {
                    result.m_error = std::make_error_code(std::errc::io_error);
                } else {
                    restored_extent = *request.m_settings.m_extent;
                }
            });
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(fullscreenCandidate()));
            const auto restoration_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            // Executor entry alone does not publish the restored geometry or its acknowledgement.
            while (executions.load() != 2u or
                   test_app.displayController().transactionState() != EDisplayTransaction::idle) {
                PPR_TEST_ASSERT(std::chrono::steady_clock::now() < restoration_deadline);
                PPR_TEST_ASSERT(not test_app.pump());
                PPR_TEST_ASSERT(not test_app.synchronize());
                PPR_TEST_ASSERT(not test_app.pump());
            }
            PPR_TEST_ASSERT(executions.load() == 2u);
            PPR_TEST_ASSERT(restored_extent.x == 1600 and restored_extent.y == 1200);
            const auto geometry = test_app.displayWindowGeometry();
            PPR_TEST_ASSERT(geometry.has_value() and not geometry->m_fullscreen);
            PPR_TEST_ASSERT(geometry->m_position.x == 20 and geometry->m_position.y == 30);
            PPR_TEST_ASSERT(test_app.displayController().transactionState() == EDisplayTransaction::idle);
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(
                test_app.displayController().transactionError() == std::make_error_code(std::errc::io_error));
            PPR_TEST_ASSERT(not test_app.close());
        };

        PPR_UNIT_TEST (display_window_stale_ack_cannot_publish_recreated_window) {
            WindowTransactionHarness test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            std::binary_semaphore entered{0}, release{0};
            bool released = false;
            PPR_DEFER {
                if (not released) {
                    release.release();
                }
                std::ignore = test_app.close();
            };
            test_app.executor([&](const RenderSurfaceRequest &request, RenderSurfaceResult &result) {
                entered.release();
                release.acquire();
                result.m_applied = {8u, *request.m_settings.m_extent, true, true};
            });
            PPR_TEST_ASSERT(not test_app.requestDisplaySettings(fullscreenCandidate()));
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(entered.try_acquire_for(std::chrono::seconds{30}));
            test_app.recreate();
            release.release();
            released = true;
            PPR_TEST_ASSERT(not test_app.synchronize());
            PPR_TEST_ASSERT(not test_app.pump());
            PPR_TEST_ASSERT(test_app.displayController().resolved().m_revision == 0u);
            PPR_TEST_ASSERT(test_app.displayController().committed().m_window_mode == EDisplayWindowMode::windowed);
            PPR_TEST_ASSERT(test_app.m_scripted->m_window_service->m_display_calls == 1u);
            PPR_TEST_ASSERT(not test_app.close());
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
            u32 m_control_polls{};

            [[nodiscard]] std::error_code serviceControls_() override {
                ++m_control_polls;
                if (m_on_controls) {
                    m_on_controls();
                }
                return {};
            }

            using Application::enableFrameAdmissionObservations;
            using Application::frameAdmissionObservations;
            std23::move_only_function<void()> m_on_controls{};

            explicit SaturatingHarness(
                const std::string_view name, GpuTeardownFailureHandler handler = nullptr,
                const ITimerClock *const clock = nullptr)
                : HarnessClock{}, ApplicationEditor(
                                      name, std::span<const char *const>{}, std::make_unique<ScriptedPlatform>(),
                                      clock ? *clock : HarnessClock::m_clock, handler) {
                // SaturatingHarness drives GridPass/TrianglePass/Renderer
                // from the test thread (scripted platform, offscreen
                // target): the default render lane would make every direct
                // renderToTexture fail closed with operation_not_permitted
                // (same failure mode as EditorShutdownHarness).
                setRenderLaneEnabled(false);
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
                const bool boot_timing = runGateBootTimingEnabled();
                const auto boot_t0 = std::chrono::steady_clock::now();

                const auto app_t0 = std::chrono::steady_clock::now();
                const std::error_code app_ec = Application::initialize();
                runGateBootTimingLogIf(boot_timing, "saturating.boot.app_initialize", runGateBootTimingMsSince(app_t0));
                if (app_ec) {
                    return app_ec;
                }

                const safe_ptr<IRhiService> rhi = getServices().get<IRhiService>();
                const safe_ptr<IShaderService> shaders = getServices().get<IShaderService>();
                PPR_ASSERT(rhi.isValid());
                PPR_ASSERT(shaders.isValid());
                const auto grid_t0 = std::chrono::steady_clock::now();
                const std::error_code grid_ec = m_grid.initialize(*rhi, *shaders, getContentDir().path());
                runGateBootTimingLogIf(
                    boot_timing, "saturating.boot.grid_initialize", runGateBootTimingMsSince(grid_t0));
                if (grid_ec) {
                    return grid_ec;
                }
                const auto tri_t0 = std::chrono::steady_clock::now();
                const std::error_code tri_ec = m_triangles.initialize(*rhi, *shaders, getContentDir().path());
                runGateBootTimingLogIf(
                    boot_timing, "saturating.boot.triangles_initialize", runGateBootTimingMsSince(tri_t0));
                if (tri_ec) {
                    return tri_ec;
                }

                // Triangle content (mirrors the fence growth test): one mesh,
                // one fallback material, submitted every admitted frame.
                const mesh::StaticMeshVertex verts[] = {
                    {.m_position = {0.0f, 0.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                    {.m_position = {1.0f, 0.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                    {.m_position = {0.0f, 1.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                };
                const u32 indices[] = {0u, 1u, 2u};
                const auto upload_t0 = std::chrono::steady_clock::now();
                const Expected<TriangleBagHandle> bag = m_triangles.uploadMesh(
                    std::span<const mesh::StaticMeshVertex>{verts}, std::span<const u32>{indices});
                runGateBootTimingLogIf(boot_timing, "saturating.boot.upload_mesh", runGateBootTimingMsSince(upload_t0));
                if (not bag.has_value()) {
                    return bag.error();
                }
                m_tri_bag = *bag;
                const TextureHandle no_textures[] = {
                    TextureHandle{}, TextureHandle{}, TextureHandle{}, TextureHandle{}};
                const auto pack_t0 = std::chrono::steady_clock::now();
                const Expected<MaterialHandle> material = m_triangles.packMaterial(
                    mesh::MaterialAsset{}, std::span<const TextureHandle>{no_textures});
                runGateBootTimingLogIf(boot_timing, "saturating.boot.pack_material", runGateBootTimingMsSince(pack_t0));
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
                const auto target_t0 = std::chrono::steady_clock::now();
                const std::error_code target_ec = make_error_code(
                    rhi->getDevice().createTexture(target_desc, nullptr, m_target.writeRef()));
                runGateBootTimingLogIf(
                    boot_timing, "saturating.boot.create_target", runGateBootTimingMsSince(target_t0));
                if (target_ec) {
                    return target_ec;
                }

                m_passes_live = true;
                m_booted = true;
                runGateBootTimingLogIf(boot_timing, "saturating.boot.total", runGateBootTimingMsSince(boot_t0));
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
                const bool is_first_submit = (m_renders == 1u);
                std::chrono::steady_clock::time_point submit_t0{};
                if (is_first_submit) {
                    submit_t0 = std::chrono::steady_clock::now();
                }
                const FenceCompletionValue previously_submitted = renderer.lastSubmittedFenceValue();
                if (const std::error_code ec = renderer.renderToTexture(*m_target, {grid_draws, tri_draws})) {
                    return ec;
                }
                if (is_first_submit) {
                    // Env check only on the first submit: steady frames pay a
                    // single integer compare, never an env allocation.
                    runGateBootTimingLogIf(
                        runGateBootTimingEnabled(), "saturating.first_submit.render_to_texture",
                        runGateBootTimingMsSince(submit_t0));
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
                // Mirror production: retain the first error but continue
                // teardown so owned GPU state is always released.
                std::error_code first_err{};
                if (const std::error_code ec = prepareGpuTeardown_()) {
                    // A failed prepare still releases what it can below only
                    // on a safe disposition; UNKNOWN fail-stops instead.
                    if (not first_err) {
                        first_err = ec;
                    }
                    if (gpuTeardownDisposition() == GpuQuiescence::kUnknown) {
                        failStopOnUnknownGpuTeardown_(ec);
                    }
                }
                m_target.setNull();
                if (m_passes_live) {
                    m_passes_live = false;
                    // First-error retain, never an early return: a pass
                    // failure must not skip the remaining passes or the
                    // Editor/RHI shutdown below while m_shutdown_done latches.
                    if (const std::error_code ec = m_grid.shutdown()) {
                        if (not first_err) {
                            first_err = ec;
                        }
                    }
                    if (const std::error_code ec = m_triangles.shutdown()) {
                        if (not first_err) {
                            first_err = ec;
                        }
                    }
                }
                if (const std::error_code ec = ApplicationEditor::shutdown()) {
                    if (not first_err) {
                        first_err = ec;
                    }
                }
                return first_err;
            }

            [[nodiscard]] std::error_code teardown() {
                return shutdown();
            }
        };

        // --- Lane-gate offscreen harness ------------------------------------
        // Item-4 evidence (Gate-2 item 4): real Application::run() admission
        // against a LANE-OWNED renderer with real offscreen submissions held
        // GPU-pending. Unlike SaturatingHarness (main-thread inline submits),
        // every GPU touch here runs ON the lane: Grid/Triangle pass
        // initialize, per-frame update/apply/encode, the 64x64 offscreen
        // target create/release, and the retirement-gate hold/open all ride
        // runOnLaneSync or the installed lane executor. Main only builds the
        // owned packet plus plain-value staged payloads and moves them
        // across the lane transports.
        //
        // Base-only Application::initialize (same pin as SaturatingHarness:
        // the Editor window/viewport/UI stages stay null), but the lane is
        // left ENABLED — headless=false comes from the injected Editor ctor
        // (App.Application.Editor.cpp:69-80), so Application::initialize
        // starts the RenderLane and the Renderer initializes on it.
        //
        // Construction accounting: update() counts the admitted-frame
        // construction at its START (before Application::update), stages only
        // values (camera snapshot, tile values, instance values); render()
        // tags the request with the live reservation identity
        // (reservationEpoch_ + frame id), submits via the lane transports,
        // and notes the queued half via noteReservationQueued_. Settlement
        // rides the production hook: the override counts and chains
        // ApplicationEditor::onLaneFrameConsumed_, so a submitted credit
        // releases only when the lane-published retirement frontier covers
        // its fence value. A transport-busy submit (never expected: the
        // admission gate holds construction at two) still settles as a
        // dropped slot so the ledger can never strand a charge.
        //
        // Retirement gate: boot() holds the renderer sweep lane-side, so the
        // first two submissions settle CPU-side yet stay GPU-pending; the
        // test opens the gate lane-side via openRetirementGate(). The
        // paused-worker rig parks the second frame inside the lane executor
        // on a binary semaphore with a 10s try bound (fail-loud, never an
        // unbounded park); shutdown() releases it best-effort so teardown
        // can never hang behind a stuck worker.
        struct LaneOffscreenHarness : HarnessClock, ApplicationEditor {
            ScriptedPlatform *m_scripted{nullptr};

            Camera m_camera{};
            GridPass m_grid{};
            TrianglePass m_triangles{};
            TriangleBagHandle m_tri_bag{};
            MaterialHandle m_tri_material{};
            rhi::ComPtr<rhi::ITexture> m_target{};
            bool m_booted{false};
            bool m_shutdown_done{false};
            std::error_code m_shutdown_result{};

            // Admitted-frame construction count (update() entry, before any
            // super work), successful lane submits, lane settlements seen.
            u32 m_constructions{0u};
            u32 m_renders{0u};
            u32 m_queued{0u};
            u32 m_consumed{0u};
            // Fix-19 bounded-lane refusals: update() refuses a fourth
            // construction (the 2-credit ledger legitimately admits one
            // while the third is still in flight; the settlement-hook exit
            // only fires when the third result drains) and settles the
            // refused, never-queued reservation through the base hook at
            // refusal time — never the three-frame counter below. The
            // paired render skips without queuing when the flag is set.
            // Workload bound only: refusal counts are diagnostic (logged per
            // run below), never admission proof — admission is proven by the
            // constructions/consumed/updates counts plus the observed
            // blocked state, never by these counts.
            u32 m_construction_refusals{0u};
            bool m_refused_update_pending{false};
            // Refused slots settled through the base hook (never counted in
            // m_consumed); the three intended settlements are captured for
            // the per-frame error/drop/submit asserts below.
            u32 m_refused_settled{0u};
            Array<std::error_code> m_consumed_errors{};
            Array<bool> m_consumed_dropped{};
            Array<FenceCompletionValue> m_consumed_submit_values{};

            // Owned per-frame payload staged by update(), moved into the
            // lane request by render(). Main-side only; the lane never
            // touches these members (it owns copies via the transport).
            FramePacket m_pending_packet{};
            GridPass::GridStagedFrame m_pending_grid{};
            TrianglePass::TriangleStagedFrame m_pending_tri{};
            bool m_pending_valid{false};

            // Paused-worker rig (lane reads, main arms/releases).
            std::atomic<bool> m_pause_second{false};
            std::atomic<bool> m_pause_entered{false};
            std::atomic<bool> m_pause_timed_out{false};
            std::binary_semaphore m_pause_gate{0};
            // Lane-confined execution count (second execution == frame two).
            // Written only by the lane executor, never read on main.
            u64 m_lane_executions{0u};

            explicit LaneOffscreenHarness(const std::string_view name, GpuTeardownFailureHandler handler = nullptr)
                : HarnessClock{}, ApplicationEditor(
                                      name, std::span<const char *const>{}, std::make_unique<ScriptedPlatform>(),
                                      HarnessClock::m_clock, handler) {
                m_scripted = static_cast<ScriptedPlatform *>(const_cast<IPlatform *>(&getPlatform()));
            }

            ~LaneOffscreenHarness() noexcept override = default;

            // Same base-only pin as SaturatingHarness::initialize: run()'s
            // virtual initialize() must not reach the Editor window stages.
            // The lane stays enabled (no setRenderLaneEnabled(false)), so
            // the Renderer below is lane-owned from boot on.
            [[nodiscard]] std::error_code initialize() override {
                if (m_booted) {
                    return default_value_v;
                }
                return Application::initialize();
            }

            [[nodiscard]] std::error_code boot() {
                const bool boot_timing = runGateBootTimingEnabled();
                const auto boot_t0 = std::chrono::steady_clock::now();

                const auto app_t0 = std::chrono::steady_clock::now();
                const std::error_code app_ec = Application::initialize();
                runGateBootTimingLogIf(boot_timing, "lane.boot.app_initialize", runGateBootTimingMsSince(app_t0));
                if (app_ec) {
                    return app_ec;
                }
                const safe_ptr<IRhiService> rhi = getServices().get<IRhiService>();
                const safe_ptr<IShaderService> shaders = getServices().get<IShaderService>();
                PPR_ASSERT(rhi.isValid());
                PPR_ASSERT(shaders.isValid());
                const std::filesystem::path content = getContentDir().path();
                if (renderLane() == nullptr) {
                    return std::make_error_code(std::errc::not_connected);
                }
                // Pass + target bring-up ON the lane (affinity from birth),
                // with the retirement gate held so the first two submits
                // stay GPU-pending until the test opens it lane-side.
                bool ran = false;
                const auto lane_bringup_t0 = std::chrono::steady_clock::now();
                const std::error_code lane_ec = renderLane()->runOnLaneSync(
                    [this, rhi, shaders, content, boot_timing]() -> std::error_code {
                        const auto grid_t0 = std::chrono::steady_clock::now();
                        const std::error_code grid_ec = m_grid.initialize(*rhi, *shaders, content);
                        runGateBootTimingLogIf(
                            boot_timing, "lane.boot.grid_initialize", runGateBootTimingMsSince(grid_t0));
                        if (grid_ec) {
                            return grid_ec;
                        }
                        const auto tri_t0 = std::chrono::steady_clock::now();
                        const std::error_code tri_ec = m_triangles.initialize(*rhi, *shaders, content);
                        runGateBootTimingLogIf(
                            boot_timing, "lane.boot.triangles_initialize", runGateBootTimingMsSince(tri_t0));
                        if (tri_ec) {
                            return tri_ec;
                        }
                        const mesh::StaticMeshVertex verts[] = {
                            {.m_position = {0.0f, 0.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                            {.m_position = {1.0f, 0.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                            {.m_position = {0.0f, 1.0f, 0.0f}, .m_normal = {0.0f, 0.0f, 1.0f}},
                        };
                        const u32 indices[] = {0u, 1u, 2u};
                        const auto upload_t0 = std::chrono::steady_clock::now();
                        const Expected<TriangleBagHandle> bag = m_triangles.uploadMesh(
                            std::span<const mesh::StaticMeshVertex>{verts}, std::span<const u32>{indices});
                        runGateBootTimingLogIf(
                            boot_timing, "lane.boot.upload_mesh", runGateBootTimingMsSince(upload_t0));
                        if (not bag.has_value()) {
                            return bag.error();
                        }
                        m_tri_bag = *bag;
                        const TextureHandle no_textures[] = {
                            TextureHandle{}, TextureHandle{}, TextureHandle{}, TextureHandle{}};
                        const auto pack_t0 = std::chrono::steady_clock::now();
                        const Expected<MaterialHandle> material = m_triangles.packMaterial(
                            mesh::MaterialAsset{}, std::span<const TextureHandle>{no_textures});
                        runGateBootTimingLogIf(
                            boot_timing, "lane.boot.pack_material", runGateBootTimingMsSince(pack_t0));
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
                        target_desc.label = "lane-gate target";
                        const auto target_t0 = std::chrono::steady_clock::now();
                        const std::error_code target_ec = make_error_code(
                            rhi->getDevice().createTexture(target_desc, nullptr, m_target.writeRef()));
                        runGateBootTimingLogIf(
                            boot_timing, "lane.boot.create_target", runGateBootTimingMsSince(target_t0));
                        if (target_ec) {
                            return target_ec;
                        }
                        getRenderer().setTestHoldSweep(true);
                        return default_value_v;
                    },
                    &ran);
                runGateBootTimingLogIf(
                    boot_timing, "lane.boot.lane_bringup", runGateBootTimingMsSince(lane_bringup_t0));
                if (not ran) {
                    return lane_ec ? lane_ec : std::make_error_code(std::errc::not_connected);
                }
                if (lane_ec) {
                    return lane_ec;
                }
                // Per-frame offscreen executor ON the lane: staged-payload
                // apply, encode to the offscreen target, submit-value
                // capture with pass stamping. Services were captured at the
                // startup boundary above; nothing is resolved here.
                renderLane()->setExecutor(
                    [this](const RenderFrameRequest &request, RenderFrameResult &result) -> std::error_code {
                        ++m_lane_executions;
                        if (m_pause_second.load(std::memory_order_acquire) and m_lane_executions == 2u) {
                            // Paused-worker state: the second frame has been
                            // admitted and picked up, but stalls here before
                            // any submit — still charged, never settled.
                            // Bounded park: a missed release fails the test
                            // loudly instead of hanging the worker forever.
                            m_pause_entered.store(true, std::memory_order_release);
                            if (not m_pause_gate.try_acquire_for(std::chrono::seconds{10})) {
                                m_pause_timed_out.store(true, std::memory_order_release);
                                return std::make_error_code(std::errc::timed_out);
                            }
                        }
                        if (not request.m_packet) [[unlikely]] {
                            result.m_dropped = true;
                            return std::make_error_code(std::errc::invalid_argument);
                        }
                        const FramePacket &packet = *request.m_packet;
                        if (const std::error_code ec = m_triangles.update(packet.m_delta_time, packet.m_camera)) {
                            return ec;
                        }
                        if (const std::error_code ec = m_grid.update(packet.m_delta_time, packet.m_camera)) {
                            return ec;
                        }
                        if (const std::error_code ec = m_grid.applyStagedFrame(request.m_grid_stage)) {
                            return ec;
                        }
                        if (const std::error_code ec = m_triangles.applyStagedFrame(request.m_triangle_stage)) {
                            return ec;
                        }
                        Renderer &renderer = getRenderer();
                        DrawSubmission grid_draws{m_grid};
                        grid_draws.m_viewport = rhi::Viewport{.extentX = 64.0f, .extentY = 64.0f};
                        grid_draws.m_scissor = rhi::ScissorRect{.maxX = 64u, .maxY = 64u};
                        DrawSubmission tri_draws{m_triangles};
                        tri_draws.m_viewport = rhi::Viewport{.extentX = 64.0f, .extentY = 64.0f};
                        tri_draws.m_scissor = rhi::ScissorRect{.maxX = 64u, .maxY = 64u};
                        const bool is_first_submit = (m_lane_executions == 1u);
                        std::chrono::steady_clock::time_point submit_t0{};
                        if (is_first_submit) {
                            submit_t0 = std::chrono::steady_clock::now();
                        }
                        const FenceCompletionValue previously_submitted = renderer.lastSubmittedFenceValue();
                        if (const std::error_code ec = renderer.renderToTexture(*m_target, {grid_draws, tri_draws})) {
                            return ec;
                        }
                        if (is_first_submit) {
                            // Env check only on the first lane submit: later
                            // frames pay one integer compare, never an env
                            // allocation on the lane.
                            runGateBootTimingLogIf(
                                runGateBootTimingEnabled(), "lane.first_submit.render_to_texture",
                                runGateBootTimingMsSince(submit_t0));
                        }
                        const FenceCompletionValue accepted = renderer.lastSubmittedFenceValue();
                        if (accepted != previously_submitted and accepted != kFenceValueNone) {
                            result.m_submit_value = accepted;
                            m_grid.notifyFrameSubmitted(accepted);
                            m_triangles.notifyFrameSubmitted(accepted);
                        }
                        return default_value_v;
                    });
                m_booted = true;
                runGateBootTimingLogIf(boot_timing, "lane.boot.total", runGateBootTimingMsSince(boot_t0));
                return default_value_v;
            }

            // Opens the retirement gate ON the lane (affinity-safe: the
            // hold flag lives on the lane-owned renderer). Main-side only;
            // never call from inside the lane executor, and never from
            // inside the platform pump callback (the handoff wait slices
            // re-enter the pump — open the gate from the test body between
            // pumps instead, while the run loop owns the pump on its own
            // thread).
            [[nodiscard]] std::error_code openRetirementGate(bool *out_ran = nullptr) {
                if (renderLane() == nullptr) {
                    if (out_ran != nullptr) {
                        *out_ran = false;
                    }
                    return std::make_error_code(std::errc::not_connected);
                }
                // Park the wait-pump slice for this handoff: the run loop
                // owns the platform pump on its worker, so a handoff wait
                // slice must never pump concurrently with it (shared drain
                // scratch, platform counters, and settlement hooks are all
                // single-threaded). The parked lane still acks promptly, and
                // the later shutdown join stays bounded without the slice —
                // the scripted platform needs no servicing.
                renderLane()->setWaitPump({});
                return renderLane()->runOnLaneSync(
                    [this]() -> std::error_code {
                        // Release the hold only: retirement plus frontier
                        // publication ride the ordinary production sweep
                        // (the handoff's own kWake trips the lane's
                        // post-control sweep in this same iteration), so
                        // admission reopens through the real queue-gate
                        // held-retirement path — never a fixture-driven
                        // direct retire outside the sweep.
                        getRenderer().setTestHoldSweep(false);
                        // Bounded completion kick (Gate-2 item 4 stall):
                        // the backend parks submitted work until a wait (or
                        // a further submit) observes completion, and
                        // admission holds at two so no further submit can
                        // drive the submit-time poll — poll-alone never
                        // advances the frontier. Wait the youngest
                        // submitted value lane-side (affinity-safe); the
                        // post-control sweep in this same iteration then
                        // retires + publishes via production code. Scoped
                        // to the gate path only; steady-state sweeps stay
                        // poll-only.
                        const auto retire_t0 = std::chrono::steady_clock::now();
                        std::ignore = getRenderer().waitForFenceValue(getRenderer().lastSubmittedFenceValue());
                        runGateBootTimingLogIf(
                            runGateBootTimingEnabled(), "lane.first_sweep.wait_for_fence",
                            runGateBootTimingMsSince(retire_t0));
                        return default_value_v;
                    },
                    out_ran);
            }

            [[nodiscard]] std::error_code update(const TimeSpan dt) override {
                // Derived pre-super construction marker: counted at the
                // START, so a blocked third frame never reaches the count.
                // Fix-19 bound: refuse a fourth construction (see the member
                // note); the paired render() skips and counts stay at three.
                // No exit here: the run keeps pumping until the third-result
                // settlement hook below ends it. The admission probe already
                // charged this frame's reservation, so settle the refused,
                // never-queued slot through the base contract now (dropped,
                // unsubmitted) — never the three-frame counter — so no
                // credit strands past teardown.
                if (m_constructions >= 3u or m_consumed >= 3u) {
                    ++m_construction_refusals;
                    m_pending_valid = false;
                    m_refused_update_pending = true;
                    RenderFrameResult refused{};
                    refused.m_frame_id = getFrameIndex();
                    refused.m_dropped = true;
                    refused.m_reservation_epoch = reservationEpoch_();
                    refused.m_reservation_frame_id = getFrameIndex();
                    ApplicationEditor::onLaneFrameConsumed_(refused);
                    ++m_refused_settled;
                    return default_value_v;
                }
                ++m_constructions;
                if (const std::error_code ec = Application::update(dt)) {
                    return ec;
                }
                // Camera + staging are plain values only: no pass is touched
                // on main (pass affinity is the lane). The packet carries
                // the snapshot the lane executes against.
                CameraModel model{};
                model.m_camera_mode = ECameraProjection::orthographic;
                m_camera.updateModel(dt, model, Viewport{PixelRect{int2{0, 0}, int2{64, 64}}, ViewportLayout{}});
                m_pending_packet = FramePacket{};
                m_pending_packet.m_frame_id = getFrameIndex();
                m_pending_packet.m_delta_time = dt;
                m_pending_packet.m_total_elapsed = getTimerManager().getTotalElapsed();
                m_pending_packet.m_camera = m_camera.getSnapshot();
                m_pending_packet.m_viewport = Viewport{PixelRect{int2{0, 0}, int2{64, 64}}, ViewportLayout{}};
                m_pending_packet.m_framebuffer_size = int2{64, 64};
                m_pending_grid = GridPass::GridStagedFrame{};
                m_pending_grid.m_tiles.push_back(
                    GridTileSubmission{
                        .m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u});
                m_pending_tri = TrianglePass::TriangleStagedFrame{};
                m_pending_tri.m_instances.push_back(
                    TrianglePass::SubmittedInstance{
                        .m_bag = m_tri_bag, .m_material = m_tri_material, .m_model = identity_v});
                m_pending_valid = true;
                return default_value_v;
            }

            [[nodiscard]] std::error_code render() override {
                // Fix-19 paired skip: the refused update above staged
                // nothing and already settled its reservation through the
                // base hook — return without counting or queuing (never an
                // app failure, never a second settle).
                if (m_refused_update_pending) {
                    m_refused_update_pending = false;
                    return default_value_v;
                }
                ++m_renders;
                if (const std::error_code ec = Application::render()) {
                    return ec;
                }
                if (not m_pending_valid) [[unlikely]] {
                    return std::make_error_code(std::errc::not_connected);
                }
                if (renderLane() == nullptr) [[unlikely]] {
                    return std::make_error_code(std::errc::not_connected);
                }
                const u64 frame_id = m_pending_packet.m_frame_id;
                RenderFrameRequest request{};
                try {
                    request.m_packet = std::make_shared<const FramePacket>(m_pending_packet);
                    request.m_grid_stage = std::move(m_pending_grid);
                    request.m_triangle_stage = std::move(m_pending_tri);
                } catch (const std::bad_alloc &) {
                    return std::make_error_code(std::errc::not_enough_memory);
                } catch (...) {
                    return std::make_error_code(std::errc::state_not_recoverable);
                }
                m_pending_valid = false;
                // Tag the frame with its construction reservation identity
                // so the lane result settles exactly this record.
                request.m_reservation_epoch = reservationEpoch_();
                request.m_reservation_frame_id = frame_id;
                RenderFrameResult dropped{};
                dropped.m_frame_id = frame_id;
                dropped.m_reservation_epoch = reservationEpoch_();
                dropped.m_reservation_frame_id = frame_id;
                if (const std::error_code submit_err = renderLane()->submitFrame(std::move(request))) {
                    // Bounded fallback, never an app failure on capacity:
                    // the slot still settles (never-applied) so the credit
                    // is never stranded.
                    dropped.m_dropped = true;
                    if (submit_err == std::make_error_code(std::errc::device_or_resource_busy)) {
                        onLaneFrameConsumed_(dropped);
                        return default_value_v;
                    }
                    dropped.m_error = submit_err;
                    onLaneFrameConsumed_(dropped);
                    return submit_err;
                }
                // Successful transport handoff: the queued half owns the
                // frame until its lane settlement arrives.
                noteReservationQueued_(frame_id);
                ++m_queued;
                return default_value_v;
            }

            void onLaneFrameConsumed_(const RenderFrameResult &result) override {
                ++m_consumed;
                m_consumed_errors.push_back(result.m_error);
                m_consumed_dropped.push_back(result.m_dropped);
                m_consumed_submit_values.push_back(result.m_submit_value);
                ApplicationEditor::onLaneFrameConsumed_(result);
                // Exit from the settlement hook itself: when the third
                // result is consumed the run ends before another admission
                // probe can construct a fourth frame (never from a later
                // pump observing m_consumed).
                if (m_consumed >= 3u) {
                    requestExit();
                }
            }

            [[nodiscard]] std::error_code shutdown() override {
                if (m_shutdown_done) {
                    return m_shutdown_result;
                }
                m_shutdown_result = shutdownOnce_();
                m_shutdown_done = true;
                return m_shutdown_result;
            }

            [[nodiscard]] std::error_code shutdownOnce_() {
                // Best-effort unpark: a test that failed before releasing
                // the paused worker must not hang the lane stop below.
                m_pause_second.store(false, std::memory_order_release);
                m_pause_gate.release();
                // Mirror production: retain the first error but continue
                // teardown so owned GPU state is always released.
                std::error_code first_err{};
                if (const std::error_code ec = prepareGpuTeardown_()) {
                    if (not first_err) {
                        first_err = ec;
                    }
                    if (gpuTeardownDisposition() == GpuQuiescence::kUnknown) {
                        failStopOnUnknownGpuTeardown_(ec);
                    }
                }
                // Target + pass release ON the lane (affinity-safe), before
                // the Editor shutdown below stops the worker and releases
                // the renderer. Skipped only when the lane never started.
                if (renderLaneOwnsRenderer_()) {
                    bool acknowledged = false;
                    std::error_code lane_err{};
                    try {
                        lane_err = renderLane()->runOnLaneSync(
                            [this]() -> std::error_code {
                                // First-error retain inside the handoff: a
                                // grid failure must not skip the triangles
                                // shutdown (target → grid → triangles order).
                                std::error_code lane_first_err{};
                                m_target.setNull();
                                if (const std::error_code ec = m_grid.shutdown()) {
                                    if (not lane_first_err) {
                                        lane_first_err = ec;
                                    }
                                }
                                if (const std::error_code ec = m_triangles.shutdown()) {
                                    if (not lane_first_err) {
                                        lane_first_err = ec;
                                    }
                                }
                                return lane_first_err;
                            },
                            &acknowledged);
                    } catch (...) {
                        lane_err = std::make_error_code(std::errc::state_not_recoverable);
                    }
                    if (not acknowledged) {
                        failStopOnUnknownGpuTeardown_(lane_err);
                    }
                    // A lane-release failure is retained but teardown
                    // continues into Editor shutdown (renderer → RHI below).
                    if (lane_err and not first_err) {
                        first_err = lane_err;
                    }
                } else {
                    m_target.setNull();
                    if (const std::error_code ec = m_grid.shutdown()) {
                        if (not first_err) {
                            first_err = ec;
                        }
                    }
                    if (const std::error_code ec = m_triangles.shutdown()) {
                        if (not first_err) {
                            first_err = ec;
                        }
                    }
                }
                if (const std::error_code ec = ApplicationEditor::shutdown()) {
                    if (not first_err) {
                        first_err = ec;
                    }
                }
                return first_err;
            }

            [[nodiscard]] std::error_code teardown() {
                return shutdown();
            }
        };

        // --- Shared lane-gate phase protocol ------------------------------
        // ONE fixture phase protocol for the lane-gate tests below
        // (Finding 2: the two duplicated per-test latch replicas — parked /
        // timed-out atoms plus a binary semaphore plus a hand-rolled worker
        // plus a manual join each — are removed; this struct is the only
        // park/release/join machinery). The run worker parks (bounded,
        // fail-loud) once it observes the blocked state; the test thread
        // takes gate ownership and opens the retirement gate only on an
        // acknowledged park, then releases and joins. A timeout on either
        // side cancels the handoff (the gate is never opened behind a
        // resumed worker; granted ownership excludes worker resumption on
        // every path including timeouts — an owned expiry parks until the
        // explicit release below), releases the park, and joins before
        // teardown — run_done stays purely informational and is never a
        // lifetime barrier.
        //
        // Bespoke-latch note: the engine Signal/IEvent primitives expose
        // only an unbounded wait() (Core.Concurrency.Event.cppm), which
        // cannot fail loud on a stuck worker — a fixture rig must bound
        // every park. Hence atomics plus a timed binary_semaphore here; no
        // new production primitive, fixture-local only.
        struct LaneGatePhase {
            // Worker-observed blocked state, reported exactly once.
            std::atomic<bool> m_parked{false};
            // Bounded park expired before the test thread released.
            std::atomic<bool> m_timed_out{false};
            // Test thread authorized resume (exactly once).
            std::atomic<bool> m_released{false};
            // Worker consumed the release (woke via acquire, not timeout).
            std::atomic<bool> m_release_acked{false};
            // Test thread abandoned the handoff (no gate open past this).
            std::atomic<bool> m_cancelled{false};
            // run() returned on the worker. Informational only — never a
            // lifetime barrier: teardown is ordered strictly after join().
            std::atomic<bool> m_run_done{false};
            std::binary_semaphore m_gate{0};
            // Gate-operation ownership (F2): the test thread holds this
            // across openRetirementGate() so a worker timeout can never
            // resume application access concurrently with the gate. The
            // flag and the timeout/cancel latches are always decided under
            // m_serial_mutex — never check-then-act across it.
            mutable std::mutex m_serial_mutex{};
            bool m_gate_owned{false};
            // Bounded park, configurable for the deterministic
            // timeout-overlap test below (production default 30s).
            std::chrono::milliseconds m_park_bound{std::chrono::seconds{30}};
            std::thread m_worker{};
            std::error_code m_run_ec{std::make_error_code(std::errc::state_not_recoverable)};
            bool m_run_threw{false};

            // Fail-loud bounds: the worker park covers the worst-case gate
            // handoff (Finding 2 cites ~30s); the test-side wait only
            // watches the parked flag, which lands in milliseconds when
            // healthy. Both expire loudly, never silently.
            static constexpr std::chrono::seconds kWaitBound{30};

            LaneGatePhase() = default;
            LaneGatePhase(const LaneGatePhase &) = delete;
            LaneGatePhase &operator=(const LaneGatePhase &) = delete;

            // Scope-bound join (Finding 2): the destructor cancels any
            // outstanding park, releases the worker, and joins — the phase
            // is declared AFTER the PPR_DEFER teardown guard, so teardown
            // can only run once the worker is gone, never behind an
            // in-flight lane access. Never detaches, even when an assertion
            // below throws past the explicit join.
            ~LaneGatePhase() {
                cancel();
                join();
            }

            void startRun(LaneOffscreenHarness &harness) {
                m_worker = std::thread{[this, &harness] {
                    try {
                        m_run_ec = harness.run();
                    } catch (...) {
                        m_run_threw = true;
                    }
                    m_run_done.store(true, std::memory_order_release);
                }};
            }

            // Run-thread side: park once the blocked state is observed.
            // Exactly-once (the first observer parks; later pumps proceed).
            // Returns true when woken by the test thread's release.
            // Invariant (F2): granted gate ownership excludes worker
            // resumption on EVERY path including timeouts. A bounded wait
            // may record a deadline failure, but it can never revoke
            // exclusion: while the test thread owns the gate the worker
            // parks until ownership is explicitly relinquished (release) or
            // abandoned (cancel wakes via release) — never resumes
            // concurrently with openRetirementGate(). A genuinely stuck gate
            // is a fail-stop watchdog abort, never a resumption.
            bool parkWorker() {
                bool expected = false;
                if (not m_parked.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    return true;
                }
                {
                    std::lock_guard<std::mutex> lock(m_serial_mutex);
                    if (m_cancelled.load(std::memory_order_acquire)) {
                        return false;
                    }
                }
                // Single bounded wait: the only timed layer. Ownership
                // decides the expiry below; there is no second timed layer.
                const bool released = m_gate.try_acquire_for(m_park_bound);
                if (released) {
                    const bool ack = not m_cancelled.load(std::memory_order_acquire) and
                                     m_released.load(std::memory_order_acquire);
                    m_release_acked.store(ack, std::memory_order_release);
                    return ack;
                }
                // Bounded-wait expiry serialization (F2): an unowned (or
                // abandoned) expiry latches timed_out and returns; an owned
                // expiry keeps parking until the owner relinquishes (release)
                // or abandons (cancel wakes via release) — without latching
                // timed_out and without resuming into the owned window.
                {
                    std::lock_guard<std::mutex> lock(m_serial_mutex);
                    if (m_cancelled.load(std::memory_order_acquire)) {
                        return false;
                    }
                    if (not m_gate_owned) {
                        m_timed_out.store(true, std::memory_order_release);
                        return false;
                    }
                }
                m_gate.acquire();
                const bool ack = not m_cancelled.load(std::memory_order_acquire) and
                                 m_released.load(std::memory_order_acquire);
                m_release_acked.store(ack, std::memory_order_release);
                return ack;
            }

            // Test-thread gate ownership (F2): take before
            // openRetirementGate(), release after. The take checks the
            // timeout/cancel latches and the ownership flag under the same
            // mutex the worker's timeout path decides under, so the gate
            // never opens behind a resumed worker. The test thread holds
            // ownership across the whole gate call; the worker parks until
            // the release below instead of resuming inside this window.
            [[nodiscard]] bool tryTakeGateOwnership() {
                std::lock_guard<std::mutex> lock(m_serial_mutex);
                if (m_gate_owned or
                    m_cancelled.load(std::memory_order_acquire) or
                    m_timed_out.load(std::memory_order_acquire)) {
                    return false;
                }
                m_gate_owned = true;
                return true;
            }

            void releaseGateOwnership() {
                std::lock_guard<std::mutex> lock(m_serial_mutex);
                m_gate_owned = false;
            }

            void setParkBoundForTest(const std::chrono::milliseconds bound) noexcept {
                m_park_bound = bound;
            }

            // Test-thread side: bounded wait for the worker's park.
            [[nodiscard]] bool waitForPark() {
                const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
                while (not m_parked.load(std::memory_order_acquire) and
                       not m_run_done.load(std::memory_order_acquire) and
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return m_parked.load(std::memory_order_acquire);
            }

            [[nodiscard]] bool timedOut() const {
                return m_timed_out.load(std::memory_order_acquire);
            }

            [[nodiscard]] bool releaseAcked() const {
                return m_release_acked.load(std::memory_order_acquire);
            }

            [[nodiscard]] bool runDone() const {
                return m_run_done.load(std::memory_order_acquire);
            }

            // Test-thread abandon: never open the gate past this point;
            // wake a possibly-parked worker so join() cannot hang.
            void cancel() {
                m_cancelled.store(true, std::memory_order_release);
                release();
            }

            // Test-thread authorize resume (exactly once; the semaphore
            // count from a second call wakes nobody, never an extra park).
            void release() {
                bool expected = false;
                if (m_released.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    m_gate.release();
                }
            }

            void join() {
                if (m_worker.joinable()) {
                    // A parked worker must never hang the join: ensure the
                    // gate is open before waiting. Harmless post-release.
                    release();
                    m_worker.join();
                }
            }
        };

        PPR_UNIT_TEST (lane_gate_blocks_third_until_retirement) {
            // Item-4 run-gate evidence: two real offscreen submissions that
            // are CPU-settled but GPU-pending (held sweep) keep the third
            // admitted frame from ever being constructed across >=20 busy
            // pumps; opening the retirement gate lane-side retires them and
            // the third frame constructs, submits, and settles exactly once.
            // The run loop owns the platform pump on its worker; once the
            // blocked state is observed the worker parks on the shared
            // LaneGatePhase above, the gate opens from the test thread, the
            // phase releases, and the worker is joined before teardown.
            LaneOffscreenHarness harness{"RunGateLaneBlock"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            // Declared after the teardown guard: LaneGatePhase joins the
            // worker in its destructor, so teardown strictly follows the
            // join even when an assertion below throws past it.
            LaneGatePhase phase{};
            ScriptedPlatform &platform = *harness.m_scripted;
            // Failure-only backstop for the worker spin: the acknowledged
            // phase protocol below answers within milliseconds, so the
            // watchdog only fires on a genuine stall (and fails the test).
            platform.m_watchdog_pumps = 100000u;

            u32 first_third_pump = 0u;
            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (harness.m_constructions >= 3u and first_third_pump == 0u) {
                    first_third_pump = pump;
                }
                // Acknowledged phase protocol (see LaneGatePhase): on the
                // first observation of the blocked state — two CPU-settled
                // still-charged submissions (third never constructed) across
                // >=20 busy pumps — park the worker until the test thread
                // opens the retirement gate and releases the phase below.
                // The park is bounded: a missed release trips the phase
                // timeout (an explicit failure below), never a silent
                // successful endRun(). Exit rides the settlement hook
                // (third result consumed), never a later pump observing
                // m_consumed.
                if (harness.m_constructions == 2u and
                    harness.m_consumed >= 2u and
                    self.m_pumps - self.m_updates >= 20u) {
                    phase.parkWorker();
                }
            };

            phase.startRun(harness);
            // Bounded wait for the worker's park, then open the gate from
            // this (test) thread: the lane's post-control sweep retires and
            // publishes in that same handoff iteration, so the worker's very
            // next probe after the phase release admits the third frame.
            // The handoff runs ONLY on an acknowledged park: a timeout (or
            // a run that ended without parking) cancels it — the gate stays
            // closed, the worker is released and joined, and teardown runs
            // only after the join. run_done stays informational (it only
            // feeds the wait and the exit reason below), never a barrier;
            // the watchdog above stays the only other exit, and it fails.
            const bool parked = phase.waitForPark();
            bool gate_opened = false;
            std::error_code gate_ec{};
            bool gate_ran = false;
            // Gate ownership serializes against the worker's timeout resume
            // (F2): the ownership flag and the timeout latch are decided
            // under the same mutex, so the gate never opens behind a
            // resumed worker and a timed-out worker never resumes inside
            // this owned window — it re-parks until the release below.
            if (parked and not phase.runDone() and phase.tryTakeGateOwnership()) {
                try {
                    gate_ec = harness.openRetirementGate(&gate_ran);
                } catch (...) {
                    gate_ec = std::make_error_code(std::errc::state_not_recoverable);
                }
                gate_opened = true;
                phase.releaseGateOwnership();
            }
            if (gate_opened) {
                phase.release();
            } else {
                phase.cancel();
            }
            phase.join();

            // Permanent one-line evidence summary (always on, pass or fail):
            // future flakes carry their counts instead of a bare assert.
            // Refusals stay what Fix-19 made them — a workload bound logged
            // here, never admission proof (see the member note).
            std::string exit_reason{"pass"};
            if (phase.m_run_threw) {
                exit_reason = "run-threw";
            } else if (phase.m_run_ec) {
                exit_reason = "run-ec";
            } else if (platform.m_watchdog_fired) {
                exit_reason = "watchdog";
            } else if (phase.timedOut()) {
                exit_reason = "phase-timeout";
            } else if (not parked) {
                exit_reason = "gate-wait-timeout";
            } else if (not phase.releaseAcked()) {
                exit_reason = "release-unacked";
            } else if (not gate_opened or gate_ec or not gate_ran) {
                exit_reason = "gate-not-opened";
            } else if (harness.m_constructions != 3u or harness.m_consumed != 3u) {
                exit_reason = "count-mismatch";
            }
            std::fprintf(
                stderr,
                "[lane-gate:block] constructions=%u consumed=%u queued=%u renders=%u refusals=%u"
                " refused_settled=%u pumps=%u updates=%u gate=%d ran=%d watchdog=%d reason=%s"
                " runec=%d(%s)\n",
                harness.m_constructions, harness.m_consumed, harness.m_queued, harness.m_renders,
                harness.m_construction_refusals, harness.m_refused_settled, platform.m_pumps, platform.m_updates,
                gate_opened ? 1 : 0, gate_ran ? 1 : 0, platform.m_watchdog_fired ? 1 : 0, exit_reason.c_str(),
                phase.m_run_ec.value(), phase.m_run_ec.message().c_str());

            PPR_TEST_ASSERT(not phase.m_run_threw);
            PPR_TEST_ASSERT(not phase.m_run_ec);
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(not phase.timedOut());
            PPR_TEST_ASSERT(parked);
            PPR_TEST_ASSERT(phase.releaseAcked());
            // Exactly three constructions, submits, and settlements: the
            // third waited for the lane retirement, then ran exactly once.
            // Refusals stay informational (logged above): a clean
            // three-frame run needs no fourth construction, so zero is a
            // valid outcome.
            PPR_TEST_ASSERT(harness.m_constructions == 3u);
            PPR_TEST_ASSERT(harness.m_renders == 3u);
            PPR_TEST_ASSERT(harness.m_queued == 3u);
            PPR_TEST_ASSERT(harness.m_consumed == 3u);
            // Every intended frame settled error-free, non-dropped, with an
            // accepted submit value (captured in the settlement hook above;
            // refused slots settle through the base hook only and never land
            // here).
            PPR_TEST_ASSERT(harness.m_consumed_errors.size() == 3u);
            PPR_TEST_ASSERT(harness.m_consumed_dropped.size() == 3u);
            PPR_TEST_ASSERT(harness.m_consumed_submit_values.size() == 3u);
            for (const std::error_code &ec: harness.m_consumed_errors) {
                PPR_TEST_ASSERT(not ec);
            }
            for (const bool dropped: harness.m_consumed_dropped) {
                PPR_TEST_ASSERT(not dropped);
            }
            for (const FenceCompletionValue value: harness.m_consumed_submit_values) {
                PPR_TEST_ASSERT(value != kFenceValueNone);
            }
            PPR_TEST_ASSERT(platform.m_updates == 3u);
            // The gate opened once, lane-side, after the observed blocked
            // state — and the third construction came only after it (busy
            // pumps held the count at two).
            PPR_TEST_ASSERT(gate_opened);
            PPR_TEST_ASSERT(not gate_ec);
            PPR_TEST_ASSERT(gate_ran);
            PPR_TEST_ASSERT(first_third_pump != 0u);
            // Blocking spanned >=20 busy pumps (admitted frames excluded).
            PPR_TEST_ASSERT(platform.m_pumps - platform.m_updates >= 20u);
            // Real submissions rode the lane (fence value observed through
            // the production diagnostics mirror) and teardown proved idle.
            PPR_TEST_ASSERT(harness.renderDiagnostics().m_last_submit_value != kFenceValueNone);
            PPR_TEST_ASSERT(harness.gpuTeardownDisposition() == GpuQuiescence::kIdleProven);
        };

        PPR_UNIT_TEST (lane_gate_paused_worker_blocks_third) {
            // Paused-worker variant: the second frame is admitted and picked
            // up by the lane but stalls inside the executor before any
            // submit (charged, never settled) while the first stays
            // GPU-pending — the third construction is blocked across >=20
            // busy pumps. Releasing the worker settles frame two; opening
            // the retirement gate retires both; the third then runs once.
            // Like its sibling, the run loop owns the pump on its worker;
            // once the blocked state is observed the worker parks on the
            // shared LaneGatePhase above, the gate opens from the test
            // thread, the phase releases, and the worker is joined before
            // teardown.
            LaneOffscreenHarness harness{"RunGateLanePaused"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            // Declared after the teardown guard: LaneGatePhase joins the
            // worker in its destructor, so teardown strictly follows the
            // join even when an assertion below throws past it.
            LaneGatePhase phase{};
            harness.m_pause_second.store(true, std::memory_order_release);
            ScriptedPlatform &platform = *harness.m_scripted;
            // Failure-only backstop for the worker spin (see sibling):
            // answered in milliseconds when healthy, fail-loud on a
            // genuine stall.
            platform.m_watchdog_pumps = 100000u;

            u32 block_start_pump = 0u;
            u32 release_pump = 0u;
            bool pause_released = false;
            platform.m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                if (not pause_released and
                    harness.m_pause_entered.load(std::memory_order_acquire) and
                    harness.m_constructions >= 2u) {
                    if (block_start_pump == 0u) {
                        block_start_pump = pump;
                    } else if (pump - block_start_pump >= 20u) {
                        harness.m_pause_gate.release();
                        pause_released = true;
                        release_pump = pump;
                    }
                }
                // Acknowledged phase protocol (see sibling and
                // LaneGatePhase): on the first observation of the blocked
                // state — pause released, two CPU-settled still-charged
                // submissions (third never constructed) across >=20 busy
                // pumps — park the worker until the test thread opens the
                // retirement gate and releases the phase. Bounded park,
                // never a silent successful endRun(); exit rides the
                // settlement hook.
                if (pause_released and
                    harness.m_constructions == 2u and
                    harness.m_consumed >= 2u and
                    self.m_pumps - self.m_updates >= 20u) {
                    phase.parkWorker();
                }
            };

            phase.startRun(harness);
            // Bounded wait for the worker's park, then open the gate from
            // this (test) thread: the lane's post-control sweep retires and
            // publishes in that same handoff iteration, so the worker's very
            // next probe after the phase release admits the third frame.
            // The handoff runs ONLY on an acknowledged park: a timeout (or
            // a run that ended without parking) cancels it — the gate stays
            // closed, the worker is released and joined, and teardown runs
            // only after the join. run_done stays informational (it only
            // feeds the wait and the exit reason below), never a barrier;
            // the watchdog above stays the only other exit, and it fails.
            const bool parked = phase.waitForPark();
            bool gate_opened = false;
            std::error_code gate_ec{};
            bool gate_ran = false;
            // Gate ownership serializes against the worker's timeout resume
            // (F2): the ownership flag and the timeout latch are decided
            // under the same mutex, so the gate never opens behind a
            // resumed worker and a timed-out worker never resumes inside
            // this owned window — it re-parks until the release below.
            if (parked and not phase.runDone() and phase.tryTakeGateOwnership()) {
                try {
                    gate_ec = harness.openRetirementGate(&gate_ran);
                } catch (...) {
                    gate_ec = std::make_error_code(std::errc::state_not_recoverable);
                }
                gate_opened = true;
                phase.releaseGateOwnership();
            }
            if (gate_opened) {
                phase.release();
            } else {
                phase.cancel();
            }
            phase.join();

            // Permanent one-line evidence summary (always on, pass or fail):
            // future flakes carry their counts instead of a bare assert.
            // Refusals stay what Fix-19 made them — a workload bound logged
            // here, never admission proof (see the member note).
            std::string exit_reason{"pass"};
            if (phase.m_run_threw) {
                exit_reason = "run-threw";
            } else if (phase.m_run_ec) {
                exit_reason = "run-ec";
            } else if (platform.m_watchdog_fired) {
                exit_reason = "watchdog";
            } else if (harness.m_pause_timed_out.load(std::memory_order_acquire)) {
                exit_reason = "pause-timeout";
            } else if (phase.timedOut()) {
                exit_reason = "phase-timeout";
            } else if (not parked) {
                exit_reason = "gate-wait-timeout";
            } else if (not phase.releaseAcked()) {
                exit_reason = "release-unacked";
            } else if (not pause_released) {
                exit_reason = "pause-never-released";
            } else if (not gate_opened or gate_ec or not gate_ran) {
                exit_reason = "gate-not-opened";
            } else if (harness.m_constructions != 3u or harness.m_consumed != 3u) {
                exit_reason = "count-mismatch";
            }
            std::fprintf(
                stderr,
                "[lane-gate:paused] constructions=%u consumed=%u queued=%u renders=%u refusals=%u"
                " refused_settled=%u pumps=%u updates=%u paused=%d span=%u gate=%d ran=%d watchdog=%d reason=%s\n",
                harness.m_constructions, harness.m_consumed, harness.m_queued, harness.m_renders,
                harness.m_construction_refusals, harness.m_refused_settled, platform.m_pumps, platform.m_updates,
                pause_released ? 1 : 0, release_pump - block_start_pump, gate_opened ? 1 : 0, gate_ran ? 1 : 0,
                platform.m_watchdog_fired ? 1 : 0, exit_reason.c_str());

            PPR_TEST_ASSERT(not phase.m_run_threw);
            PPR_TEST_ASSERT(not phase.m_run_ec);
            PPR_TEST_ASSERT(not platform.m_watchdog_fired);
            PPR_TEST_ASSERT(not harness.m_pause_timed_out.load(std::memory_order_acquire));
            PPR_TEST_ASSERT(not phase.timedOut());
            PPR_TEST_ASSERT(parked);
            PPR_TEST_ASSERT(phase.releaseAcked());
            PPR_TEST_ASSERT(pause_released);
            PPR_TEST_ASSERT(pause_released);
            PPR_TEST_ASSERT(harness.m_constructions == 3u);
            PPR_TEST_ASSERT(harness.m_renders == 3u);
            PPR_TEST_ASSERT(harness.m_queued == 3u);
            PPR_TEST_ASSERT(harness.m_consumed == 3u);
            // Every intended frame settled error-free, non-dropped, with an
            // accepted submit value (captured in the settlement hook above;
            // refused slots settle through the base hook only and never land
            // here).
            PPR_TEST_ASSERT(harness.m_consumed_errors.size() == 3u);
            PPR_TEST_ASSERT(harness.m_consumed_dropped.size() == 3u);
            PPR_TEST_ASSERT(harness.m_consumed_submit_values.size() == 3u);
            for (const std::error_code &ec: harness.m_consumed_errors) {
                PPR_TEST_ASSERT(not ec);
            }
            for (const bool dropped: harness.m_consumed_dropped) {
                PPR_TEST_ASSERT(not dropped);
            }
            for (const FenceCompletionValue value: harness.m_consumed_submit_values) {
                PPR_TEST_ASSERT(value != kFenceValueNone);
            }
            PPR_TEST_ASSERT(platform.m_updates == 3u);
            // The worker stalled (never timed out), the block spanned >=20
            // busy pumps, and the gate opened lane-side before the third.
            PPR_TEST_ASSERT(harness.m_pause_entered.load(std::memory_order_acquire));
            PPR_TEST_ASSERT(release_pump - block_start_pump >= 20u);
            PPR_TEST_ASSERT(gate_opened);
            PPR_TEST_ASSERT(not gate_ec);
            PPR_TEST_ASSERT(gate_ran);
            PPR_TEST_ASSERT(platform.m_pumps - platform.m_updates >= 20u);
            PPR_TEST_ASSERT(harness.renderDiagnostics().m_last_submit_value != kFenceValueNone);
            PPR_TEST_ASSERT(harness.gpuTeardownDisposition() == GpuQuiescence::kIdleProven);
        };

        PPR_UNIT_TEST (rejected_frame_does_not_inherit_previous_submit_fate) {
            // F1 stale-fate regression: a successful SUBMIT followed by a
            // pre-admission rejection in the same fence epoch. The rejected
            // frame must NOT be charged against the earlier frame's fence
            // value, and each settlement pairs exactly one record (two
            // distinct accepted values, one explicit unsubmitted).
            SaturatingHarness harness{"RunGateStaleFate"};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            Renderer &renderer = harness.getRenderer();
            rhi::ITexture &target = *harness.m_target;
            // Hold the sweep so both submits stay outstanding: the third
            // admission then fails pre-admission (capacity-busy) on the
            // same epoch, with no fence movement past it.
            renderer.setTestHoldSweep(true);
            const FenceCompletionValue before_a = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(not renderer.renderToTexture(target, {}));
            PPR_TEST_ASSERT(renderer.lastSubmitSettlement() == FrameSettlement::kSubmitted);
            const FenceCompletionValue value_a = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(value_a != kFenceValueNone and value_a > before_a);
            const FenceCompletionValue before_b = value_a;
            PPR_TEST_ASSERT(not renderer.renderToTexture(target, {}));
            PPR_TEST_ASSERT(renderer.lastSubmitSettlement() == FrameSettlement::kSubmitted);
            const FenceCompletionValue value_b = renderer.lastSubmittedFenceValue();
            PPR_TEST_ASSERT(value_b != kFenceValueNone and value_b > before_b);
            const FenceCompletionValue before_c = value_b;
            const std::error_code busy_err = renderer.renderToTexture(target, {});
            PPR_TEST_ASSERT(busy_err == std::make_error_code(std::errc::device_or_resource_busy));
            // THIS invocation reports definitely-unsubmitted: the fence did
            // not move and the settlement is kRejected — never the previous
            // frame's kSubmitted.
            PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == before_c);
            PPR_TEST_ASSERT(renderer.lastSubmitSettlement() == FrameSettlement::kRejected);
            // Production classification (mirrors the Editor fate guard):
            // only strictly-greater advancement reports an accepted
            // submission, so the rejected frame charges nothing.
            FenceCompletionValue charged_c{kFenceValueNone};
            bool quarantined_c = false;
            const FenceCompletionValue after_c = renderer.lastSubmittedFenceValue();
            const FrameSettlement fate_c = renderer.lastSubmitSettlement();
            if (fate_c == FrameSettlement::kSubmitted and after_c != kFenceValueNone and after_c > before_c) {
                charged_c = after_c;
            } else if (fate_c == FrameSettlement::kSubmitFailed) {
                quarantined_c = true;
            }
            PPR_TEST_ASSERT(charged_c == kFenceValueNone);
            PPR_TEST_ASSERT(not quarantined_c);
            // Each settlement pairs exactly one record: two distinct
            // accepted values for the two submits, and no aliasing of the
            // rejected frame onto either.
            PPR_TEST_ASSERT(value_a != value_b);
            PPR_TEST_ASSERT(charged_c != value_a);
            PPR_TEST_ASSERT(charged_c != value_b);
            // Drain through production calls so teardown meets idle proof.
            renderer.setTestHoldSweep(false);
            std::ignore = renderer.waitForFenceValue(value_b);
            std::ignore = renderer.retireCompletedFrames();
        };

        PPR_UNIT_TEST (lane_gate_timeout_overlapping_gate_entry_serialized) {
            // F2 timeout-race regression (deterministic): force the
            // worker's single bounded-park timeout to fire strictly inside
            // the test thread's owned gate window — and hold ownership past
            // BOTH stacked bounds of the old two-layer wait (150ms bound vs
            // ~360ms hold with wide margins). Lane-access epochs on both
            // sides must be disjoint — the owned timeout parks until the
            // explicit release below instead of resuming concurrently with
            // the gate — and the post-gate release wakes it with an
            // acknowledgement, never a latched timeout.
            LaneGatePhase phase{};
            phase.setParkBoundForTest(std::chrono::milliseconds{150});
            using SteadyClock = std::chrono::steady_clock;
            const SteadyClock::time_point epoch = SteadyClock::now();
            const auto millis_since = [&](const SteadyClock::time_point point) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(point - epoch).count();
            };
            std::atomic<bool> worker_returned{false};
            std::atomic<long long> worker_resumed_ms{-1};
            std::atomic<long long> gate_enter_ms{-1};
            std::atomic<long long> gate_exit_ms{-1};
            bool worker_ack = false;
            std::thread worker{[&] {
                worker_ack = phase.parkWorker();
                worker_resumed_ms.store(millis_since(SteadyClock::now()), std::memory_order_release);
                worker_returned.store(true, std::memory_order_release);
            }};
            // Unconditional release-and-join cleanup BEFORE any throwing
            // assert below: a scheduling delay past the worker timeout can
            // throw out of tryTakeGateOwnership while this thread stays
            // joinable (terminate without this guard).
            PPR_DEFER {
                phase.cancel();
                if (worker.joinable()) {
                    worker.join();
                }
            };
            // Explicit park-entry coordination (no sleep-inferred entry):
            // the worker signals m_parked at parkWorker entry; observe it
            // (bounded, fail-loud) before owning the gate.
            PPR_TEST_ASSERT(phase.waitForPark());
            // Own the gate across BOTH configured wait bounds (150ms bound,
            // hold ~360ms with wide margins): the single bounded wait expires
            // strictly inside this owned window, and the worker must still
            // park until the release below — never resume inside it.
            PPR_TEST_ASSERT(phase.tryTakeGateOwnership());
            gate_enter_ms.store(millis_since(SteadyClock::now()), std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds{350});
            gate_exit_ms.store(millis_since(SteadyClock::now()), std::memory_order_release);
            phase.releaseGateOwnership();
            phase.release();
            const SteadyClock::time_point deadline = SteadyClock::now() + std::chrono::seconds{10};
            while (not worker_returned.load(std::memory_order_acquire) and SteadyClock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            worker.join();
            PPR_TEST_ASSERT(worker_returned.load(std::memory_order_acquire));
            // No concurrent access: the worker resumed at or after the
            // gate exit, never inside the owned window — and the wake was
            // the post-gate release (acknowledged), not an independent
            // timeout resume (which would also have latched timedOut).
            const long long resumed = worker_resumed_ms.load(std::memory_order_acquire);
            const long long entered = gate_enter_ms.load(std::memory_order_acquire);
            const long long exited = gate_exit_ms.load(std::memory_order_acquire);
            PPR_TEST_ASSERT(entered >= 0 and exited > entered and resumed >= 0);
            PPR_TEST_ASSERT(not (resumed >= entered and resumed < exited));
            PPR_TEST_ASSERT(worker_ack);
            PPR_TEST_ASSERT(phase.releaseAcked());
            PPR_TEST_ASSERT(not phase.timedOut());
        };

        PPR_UNIT_TEST (admission_observations_report_skips_with_explicit_time) {
            struct ManualClock final : ITimerClock {
                TimePoint m_now{};
                TimePoint now() const noexcept override {
                    return m_now;
                }
            } clock;
            for (const bool enabled: {false, true}) {
                SaturatingHarness harness{"AdmissionObservations", nullptr, &clock};
                PPR_TEST_ASSERT(not harness.boot());
                PPR_DEFER {
                    std::ignore = harness.teardown();
                };
                harness.enableFrameAdmissionObservations(enabled);
                harness.m_on_controls = [&] { clock.m_now += std::chrono::milliseconds{1}; };
                auto &renderer = harness.getRenderer();
                renderer.setTestHoldSweep(true);
                harness.m_scripted->m_on_pump = [&](ScriptedPlatform &self, const u32 pump) {
                    clock.m_now += std::chrono::milliseconds{10};
                    if (pump == 5u) {
                        renderer.setTestHoldSweep(false);
                    }
                    if (harness.m_updates >= 3u) {
                        self.endRun();
                    }
                };
                PPR_TEST_ASSERT(not harness.run());
                PPR_TEST_ASSERT(not harness.m_scripted->m_watchdog_fired);
                PPR_TEST_ASSERT(harness.m_updates == 3u);
                const auto &observed = harness.frameAdmissionObservations();
                if (enabled) {
                    PPR_TEST_ASSERT(observed.m_attempts >= harness.m_updates + observed.m_skipped);
                    PPR_TEST_ASSERT(observed.m_skipped > 0u);
                    PPR_TEST_ASSERT(observed.m_max_consecutive_skips > 0u);
                    PPR_TEST_ASSERT(observed.m_max_consecutive_skips <= observed.m_skipped);
                    PPR_TEST_ASSERT(observed.m_max_controls == std::chrono::milliseconds{1});
                    // Uncapped tick only reads this clock; native pumping is outside it.
                    PPR_TEST_ASSERT(observed.m_max_clock == TimeSpan{});
                    PPR_TEST_ASSERT(observed.m_max_total >= observed.m_max_clock);
                    const auto &interval = observed.m_last_interval;
                    PPR_TEST_ASSERT(interval.m_skipped > 0u);
                    PPR_TEST_ASSERT(interval.m_skipped <= observed.m_skipped);
                    PPR_TEST_ASSERT(
                        interval.m_suspended_skips + interval.m_credit_skips + interval.m_other_skips ==
                        interval.m_skipped);
                    // The explicit clock advances once in controls for each
                    // attempt, and once in native collection for each skip.
                    PPR_TEST_ASSERT(interval.m_admission == (interval.m_skipped + 1u) * std::chrono::milliseconds{1});
                    PPR_TEST_ASSERT(interval.m_busy_wait == interval.m_skipped * std::chrono::milliseconds{10});
                    PPR_TEST_ASSERT(interval.m_busy_service == TimeSpan{});
                } else {
                    PPR_TEST_ASSERT(observed.m_attempts == 0u and observed.m_skipped == 0u);
                    PPR_TEST_ASSERT(observed.m_max_total == TimeSpan{});
                    PPR_TEST_ASSERT(observed.m_last_interval.m_skipped == 0u);
                    PPR_TEST_ASSERT(observed.m_last_interval.m_admission == TimeSpan{});
                }
            }
        };

        PPR_UNIT_TEST (paced_credit_retries_preserve_elapsed_deadline) {
            struct ManualClock final : ITimerClock {
                TimePoint m_now{};
                TimePoint now() const noexcept override {
                    return m_now;
                }
            } clock;
            SaturatingHarness harness{"PacedCreditRetries", nullptr, &clock};
            PPR_TEST_ASSERT(not harness.boot());
            PPR_DEFER {
                std::ignore = harness.teardown();
            };
            harness.enableFrameAdmissionObservations(true);
            harness.m_on_controls = [&] { clock.m_now += std::chrono::milliseconds{1}; };
            auto &renderer = harness.getRenderer();
            renderer.setTestHoldSweep(true);
            u32 busy_collections{};
            harness.m_on_rendered = [&](SaturatingHarness &self) {
                if (self.m_renders == 2u) {
                    // Make GPU readiness deterministic; only the explicit
                    // sweep hold keeps both reservations occupied afterward.
                    PPR_TEST_ASSERT(not renderer.waitForFenceValue(renderer.lastSubmittedFenceValue()));
                    self.setTargetFrameRate(100);
                }
            };
            harness.m_scripted->m_on_pump = [&](ScriptedPlatform &self, const u32) {
                clock.m_now += std::chrono::milliseconds{1};
                if (harness.m_updates == 2u and harness.frameAdmissionObservations().m_skipped > 0u) {
                    if (++busy_collections == 3u) {
                        renderer.setTestHoldSweep(false);
                    }
                }
                if (harness.m_updates >= 3u) {
                    self.endRun();
                }
            };
            PPR_TEST_ASSERT(not harness.run());
            PPR_TEST_ASSERT(not harness.m_scripted->m_watchdog_fired);
            PPR_TEST_ASSERT(harness.m_updates == 3u and harness.m_renders == 3u);
            const auto &interval = harness.frameAdmissionObservations().m_last_interval;
            PPR_TEST_ASSERT(interval.m_skipped == 3u);
            PPR_TEST_ASSERT(interval.m_admission < std::chrono::milliseconds{20});
            PPR_TEST_ASSERT(interval.m_busy_wait == std::chrono::milliseconds{3});
            PPR_TEST_ASSERT(harness.m_control_polls > harness.m_updates);
            TimeSpan admitted_total{};
            for (const auto dt: harness.m_update_dts) {
                admitted_total += dt;
            }
            PPR_TEST_ASSERT(admitted_total == harness.getTimerManager().getTotalElapsed());
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
            PPR_TEST_ASSERT(harness.m_control_polls > harness.m_updates);
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
                    // Confirmed removal AFTER the first admitted frame:
                    // retire backend work on the owner thread BEFORE
                    // injecting synthetic loss. slang-rhi parks submitted
                    // command buffers until a submit-time poll observes
                    // completion, and the removal-confirmed shutdown path
                    // performs zero backend waits (production must not
                    // submit after removal), so unretired parked refs
                    // would survive to destroyRHI and trip
                    // m_liveDeviceCount. Wait the accepted value, sweep,
                    // then drive the queue poll — all proven completion,
                    // never new device work after the loss below.
                    // Best-effort (never fails the zero-client-work
                    // assertion): a drive failure still injects the loss.
                    try {
                        Renderer &renderer = self.getRenderer();
                        std::ignore = renderer.waitForFenceValue(self.m_last_accepted);
                        std::ignore = renderer.retireCompletedFrames();
                        renderer.driveQueueRetirementForTest();
                    } catch (...) {
                    }
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
            // Healthy-backend drain BEFORE injecting synthetic loss (mirror
            // the terminal_probe fixture): boot uploads (TrianglePass
            // fallback texture initial data) submit async and park in the
            // backend queue until a submit-time poll observes completion, but
            // notifyDeviceLost drops the queue ref without waiting — an
            // undrained backend retains device refs to destroyRHI (which
            // asserts m_liveDeviceCount == 0). Wait the submitted value,
            // sweep, then drive the queue poll — proven completion only,
            // never new device work. Test precondition: each step asserts
            // success, so a drain failure fails loudly here and the loss
            // below never injects on top of undrained uploads.
            PPR_TEST_ASSERT(not renderer.waitForFenceValue(renderer.lastSubmittedFenceValue()));
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            renderer.driveQueueRetirementForTest();
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
            PPR_TEST_ASSERT(not lumped.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 424242u}));
            PPR_DEFER {
                std::ignore = lumped.shutdown();
            };
            const u64 tick_before = lumped.tickCount();
            PPR_TEST_ASSERT(not lumped.update(std::chrono::milliseconds{50}));
            PPR_TEST_ASSERT(lumped.tickCount() == tick_before + 3u);

            colony::ColonyDriver split{};
            PPR_TEST_ASSERT(not split.initLegacyFixture(colony::ColonyDriverDesc{.m_seed = 424242u}));
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
            // Healthy-backend drain BEFORE injecting confirmed removal
            // (mirror the terminal_probe fixture): boot uploads (TrianglePass
            // fallback texture initial data) submit async and park in the
            // backend queue until a submit-time poll observes completion, but
            // removal-confirmed teardown abandons the fence without waiting
            // (App.Renderer.cpp:364-368) — an undrained backend retains
            // boot-upload/m_commandBuffersInFlight device refs to a later
            // destroyRHI (which asserts m_liveDeviceCount == 0). Wait the
            // submitted value, sweep, then drive the queue poll — proven
            // completion only, never new device work. Test precondition:
            // each step asserts success, so a drain failure fails loudly here
            // and the removal below never injects on top of undrained
            // uploads.
            PPR_TEST_ASSERT(not renderer.waitForFenceValue(renderer.lastSubmittedFenceValue()));
            PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
            renderer.driveQueueRetirementForTest();
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
            for (const std::error_code original: {std::error_code{}, std::make_error_code(std::errc::io_error)}) {
                SaturatingHarness harness{"RunGateDiagFail"};
                PPR_TEST_ASSERT(not harness.boot());
                PPR_DEFER {
                    std::ignore = harness.teardown();
                };
                ScriptedPlatform &platform = *harness.m_scripted;
                harness.setGpuTeardownEntryHookForTest([] { throw std::bad_alloc{}; });
                platform.m_on_pump = [&](ScriptedPlatform &, const u32 pump) {
                    if (pump >= 2u) {
                        harness.requestExit(original);
                    }
                };
                const std::error_code result = harness.run();
                PPR_TEST_ASSERT(result == original);
                PPR_TEST_ASSERT(not platform.m_watchdog_fired);
                PPR_TEST_ASSERT(harness.gpuTeardownDisposition() == GpuQuiescence::kIdleProven);
                const std::span<const std::string> stages = harness.gpuTeardownStageLog();
                const Array<std::string> expected{
                    "quiescence", "idle-proven", "scene", "ui", "renderer", "rhi", "platform"};
                PPR_TEST_ASSERT(stages.size() == expected.size());
                for (u32 i = 0u; i < expected.size(); ++i) {
                    PPR_TEST_ASSERT(stages[i] == expected[i]);
                }
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
                // Inline death regression: the child drives Grid/Triangle
                // passes plus Renderer::renderToTexture from this (test)
                // thread, so the lane stays off — lane-owned init would
                // fail closed with operation_not_permitted before UNKNOWN.
                setRenderLaneEnabled(false);
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
            // Deterministic diagnostic throw (armed by the parent via
            // PPR_GATE1_B1_DIAG_THROW): the hook genuinely throws INSIDE
            // the production-protected prepareGpuTeardown_ hook boundary
            // (which swallows hook exceptions and falls through to
            // classification), so diagnostic-exception resilience is
            // actually exercised on the UNKNOWN path below.
            editor.setGpuTeardownEntryHookForTest([&] {
                writeDeathMarker("quiescence-entry");
                // Owners alive, nothing released yet: passes still hold
                // their stamped slots and completion is below both values.
                const bool alive = maxSlotValue(grid.slotCompletionValues()) == second and
                                   maxSlotValue(triangles.slotCompletionValues()) == second and
                                   renderer.completedFenceValue() < first;
                writeDeathMarker(alive ? "owners-alive" : "owners-dead");
                if (diagThrowArmed()) {
                    writeDeathMarker("diag-throw-armed");
                    throw std::runtime_error{"ppr-gate1-b1-diag-throw"};
                }
            });

            // Real shutdown WITHOUT pre-drain → UNKNOWN → fail-stop.
            // Unreachable past this call: no normal return, no destruction.
            // An escaping product exception must NEVER manufacture the
            // expected fatal status nor release the UNKNOWN resources:
            // die with a distinct marker/status so the parent fails
            // loudly instead of mistaking it for the proven fail-stop.
            try {
                std::ignore = editor.teardown();
            } catch (...) {
                writeDeathMarker("exception-escaped");
                std::_Exit(kUnknownGpuCompletionFatalExit + 1);
            }
            writeDeathMarker("normal-return");
            // Release the child-owned GPU refs BEFORE any RHI destroy path
            // on this (misconfigured, non-fail-stop) return: the target
            // holds a strong device ref and RHI::destroy aborts on live
            // children.
            try {
                target.setNull();
            } catch (...) {
            }
            try {
                std::ignore = grid.shutdown();
            } catch (...) {
            }
            try {
                std::ignore = triangles.shutdown();
            } catch (...) {
            }
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
            // B1'b deterministic diagnostic throw on UNKNOWN: the child's
            // quiescence-entry hook genuinely throws INSIDE the
            // production-protected hook boundary (proven via markers) and
            // the child must still die with the explicit fail-stop status
            // (asserted result, not merely exit), with NO release,
            // normal-return, or destruction markers.
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
            detail::RunGate::monitor_change_and_revert_report_operational_errors,
            detail::RunGate::display_window_mode_ack_while_suspended_and_revert,
            detail::RunGate::display_window_minimized_revert_failure_preserves_errors,
            detail::RunGate::display_window_stale_generation_never_applies_native_mode,
            detail::RunGate::display_window_failed_apply_and_restore_are_honest,
            detail::RunGate::display_window_reenumeration_rejects_removed_monitor_before_apply,
            detail::RunGate::display_window_surface_failure_restores_native_geometry,
            detail::RunGate::display_window_stale_ack_cannot_publish_recreated_window,
            detail::RunGate::admission_observations_report_skips_with_explicit_time,
            detail::RunGate::paced_credit_retries_preserve_elapsed_deadline,
            detail::RunGate::saturated_run_preserves_time_and_input,
            detail::RunGate::terminal_probe_runs_zero_client_work,
            detail::RunGate::first_error_precedence_clock_over_terminal,
            detail::RunGate::busy_collection_failure_latches_first,
            detail::RunGate::sim_catch_up_preserves_skipped_time,
            detail::RunGate::healthy_teardown_releases_in_order,
            detail::RunGate::throwing_native_pump_and_wait_shutdown_retains_first_error,
            detail::RunGate::shutdown_drops_queued_frames_before_backend_quarantine,
            detail::RunGate::removal_confirmed_preserves_first_error,
            detail::RunGate::focus_loss_gain_ordered_across_busy,
            detail::RunGate::two_runs_reestablish_quiescence_independently,
            detail::RunGate::healthy_teardown_survives_diagnostic_failure,
            detail::RunGate::lane_gate_blocks_third_until_retirement,
            detail::RunGate::lane_gate_paused_worker_blocks_third,
            detail::RunGate::rejected_frame_does_not_inherit_previous_submit_fate,
            detail::RunGate::lane_gate_timeout_overlapping_gate_entry_serialized,
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
