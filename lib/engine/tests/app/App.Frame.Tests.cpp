module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import std;

namespace pP::tests::detail {
    namespace FrameStages {
        constexpr ApplicationDomain kHeadlessDomain{
            .m_is_headless = true,
            .m_is_interactive = false,
            .m_needs_presence = false,
            .m_needs_rendering = false,
            .m_needs_user_interface = false,
        };

        struct HeadlessApp : Application {
            explicit HeadlessApp(const std::string_view name)
                : Application(kHeadlessDomain, name, std::span<const char *const>{}) {
            }

            // Test-seam construction: scripted platform + injected clock
            // (mirrors the production Application seam). The clock outlives
            // the application (non-owning view).
            HeadlessApp(const std::string_view name, std::unique_ptr<IPlatform> platform, const ITimerClock &clock)
                : Application(kHeadlessDomain, name, std::span<const char *const>{}, std::move(platform), clock) {
            }

            [[nodiscard]] std::error_code boot() {
                return initialize();
            }
            [[nodiscard]] std::error_code teardown() {
                return shutdown();
            }
        };

        // P0 completed-pump boundary, tested through the IInputService
        // interface (established FakeInputService pattern from
        // App.WindowInput.Tests.cpp — GLFW concretes are not visible to
        // tests) but dispatching through the PRODUCTION PendingInputBatch:
        // native pumps collect owned records without client dispatch;
        // device advancement + exactly-once batch dispatch happen at the
        // defined update point.
        struct FakeFrameInput final : IInputService {
            KeyboardDevice m_keyboard{InputDeviceID{1u}};
            MouseDevice m_mouse{InputDeviceID{2u}};
            GamepadDevice m_gamepad{InputDeviceID{3u}};
            InputContext m_global{};

            TimePoint m_timestamp{};
            TimeSpan m_delta_time{};
            u32 m_device_polls{0u};
            u32 m_native_pumps{0u};

            // Production collection/dispatch path under test.
            PendingInputBatch m_batch{};
            bool m_collecting{false};

            [[nodiscard]] const KeyboardDevice &getKeyboard() const noexcept override {
                return m_keyboard;
            }

            [[nodiscard]] const MouseDevice &getMouse() const noexcept override {
                return m_mouse;
            }

            [[nodiscard]] const GamepadDevice &getGamepad(int) const noexcept override {
                return m_gamepad;
            }

            [[nodiscard]] SharedInputDevice getInputDeviceByID(const InputDeviceID &) const noexcept override {
                return {};
            }

            [[nodiscard]] std::error_code enumerateInputDevices(Collector<SharedInputDevice>) const noexcept override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code enumerateInputKeysSupported(Collector<InputKey>) const override {
                return default_value_v;
            }

            [[nodiscard]] InputContext &getGlobalInputContext() noexcept override {
                return m_global;
            }

            [[nodiscard]] bool hasInputContext(const InputContext &) const noexcept override {
                return false;
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

            [[nodiscard]] std::error_code enumerateInputContexts(Collector<InputContext>) const override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code enumerateInputContextDeviceAssignments(
                Collector<IInputDevice, InputContext>) const override {
                return default_value_v;
            }

            // Once-per-client-frame advancement: device time, transient
            // clears, and gamepad sampling happen exactly here.
            [[nodiscard]] std::error_code pollInputDevices(const TimeSpan dt) override {
                m_timestamp += dt;
                m_delta_time = dt;
                ++m_device_polls;
                if (const std::error_code ec = m_keyboard.pollInputMessages(dt)) {
                    return ec;
                }
                return m_mouse.pollInputMessages(dt);
            }

            void resetInputDevices() noexcept override {
                m_keyboard.resetInputState();
                m_mouse.resetInputState();
                m_gamepad.resetInputState();
            }

            void postKeyboardCharacterInput(const InputContext &context, const hal::native::char_t codepoint) override {
                if (m_collecting) {
                    m_batch.postChar(safe_ptr<const InputContext>{&context}, codepoint);
                    return;
                }
                m_keyboard.postKeyboardCharacterInput(context, codepoint);
            }

            void postKeyboardKeyPressed(
                const InputContext &context, const EKeyboardKey key, const bool pressed) override {
                if (m_collecting) {
                    m_batch.postKey(safe_ptr<const InputContext>{&context}, key, pressed);
                    return;
                }
                m_keyboard.postKeyboardKeyPressed(TimeSpan{}, context, key, pressed);
            }

            void postMouseButtonPressed(
                const InputContext &context, const EMouseButton button, const bool pressed) override {
                if (m_collecting) {
                    m_batch.postMouseButton(safe_ptr<const InputContext>{&context}, button, pressed);
                    return;
                }
                m_mouse.postMouseButtonPressed(TimeSpan{}, context, button, pressed);
            }

            void postMouseCursorPosition(const InputContext &context, const float2 &absolute_pos) override {
                if (m_collecting) {
                    m_batch.postMouseCursor(safe_ptr<const InputContext>{&context}, absolute_pos);
                    return;
                }
                m_mouse.postMouseCursorPosition(TimeSpan{}, context, absolute_pos);
            }

            void postMouseScrollWheel(const InputContext &context, const float2 &delta) override {
                if (m_collecting) {
                    m_batch.postMouseScroll(safe_ptr<const InputContext>{&context}, delta);
                    return;
                }
                m_mouse.postMouseScrollWheel(TimeSpan{}, context, delta);
            }

            [[nodiscard]] DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event) override {
                return {};
            }

            [[nodiscard]] DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event) override {
                return {};
            }

            // One native pump carrying externally-arrived events: collect
            // into the owned production batch, dispatch nothing. Mirrors
            // GlfwPlatform::pumpNativeEvents (flag toggled around collection)
            // with the test acting as the window-callback event source.
            void beginNativePump() {
                m_collecting = true;
            }

            void endNativePump() {
                m_collecting = false;
                ++m_native_pumps;
            }

            // The defined update point: exactly-once ordered replay through
            // the real devices, then clear. Mirrors
            // GlfwInput::dispatchCollectedBatches.
            [[nodiscard]] std::error_code dispatchBatches() {
                return m_batch.dispatch(m_keyboard, m_mouse, m_delta_time);
            }
        };

        PPR_UNIT_TEST (native_pump_without_devices_is_repeatable_noop) {
            FakeFrameInput inputs{};
            inputs.beginNativePump();
            inputs.endNativePump();
            inputs.beginNativePump();
            inputs.endNativePump();
            PPR_TEST_ASSERT(inputs.m_native_pumps == 2u);
            PPR_TEST_ASSERT(inputs.m_device_polls == 0u);
            PPR_TEST_ASSERT(inputs.m_timestamp == TimePoint{});
            PPR_TEST_ASSERT(inputs.m_batch.empty());
        };

        PPR_UNIT_TEST (posted_text_survives_native_pumps) {
            FakeFrameInput inputs{};
            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not inputs.pollInputDevices(kDt));

            // Immediate post (defined update point) still buffers directly.
            inputs.postKeyboardCharacterInput(inputs.m_global, static_cast<hal::native::char_t>('x'));
            PPR_TEST_ASSERT(inputs.m_keyboard.m_character_inputs.size() == 1u);

            // Native-only collection never clears transient input and never
            // advances device state on its own.
            inputs.beginNativePump();
            inputs.endNativePump();
            PPR_TEST_ASSERT(inputs.m_keyboard.m_character_inputs.size() == 1u);
            PPR_TEST_ASSERT(inputs.m_device_polls == 1u);
            PPR_TEST_ASSERT(inputs.m_timestamp == TimePoint{} + kDt);
        };

        PPR_UNIT_TEST (device_time_advances_exactly_once_per_poll) {
            FakeFrameInput inputs{};
            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not inputs.pollInputDevices(kDt));
            PPR_TEST_ASSERT(inputs.m_timestamp == TimePoint{} + kDt);
            PPR_TEST_ASSERT(inputs.m_delta_time == kDt);

            // A second poll re-advances time and clears transient text, so
            // callers must poll once per client frame and use native-only
            // pumps for any extra collection while waiting.
            inputs.postKeyboardCharacterInput(inputs.m_global, static_cast<hal::native::char_t>('y'));
            PPR_TEST_ASSERT(not inputs.pollInputDevices(kDt));
            PPR_TEST_ASSERT(inputs.m_timestamp == TimePoint{} + kDt + kDt);
            PPR_TEST_ASSERT(inputs.m_keyboard.m_character_inputs.empty());
        };

        PPR_UNIT_TEST (batched_multi_pump_dispatches_once_in_order) {
            // Teardown is the inverse of setup: the listener is declared
            // FIRST so the input fake (whose context holds a safe_ptr to
            // the listener) is destroyed first, releasing the observer
            // before its owner dies. The reverse order trips
            // "safe_object destroyed with live reference".
            InputListener listener{};
            FakeFrameInput inputs{};
            InputContext &routed = inputs.m_global;
            Array<InputMessage> messages{};
            listener.setRawKeyCallback([&](const TimeSpan, const InputMessage &message) -> EInputMessageResponse {
                messages.push_back(message);
                return EInputMessageResponse::handled;
            });
            routed.addInputListener(safe_ptr{&listener}, 0);

            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not inputs.pollInputDevices(kDt));

            // Pump 1 (native callbacks arrive): char + press are collected
            // into the owned production batch — no client callback runs, the
            // device char buffer stays empty, device time is untouched.
            inputs.beginNativePump();
            inputs.postKeyboardCharacterInput(routed, static_cast<hal::native::char_t>('a'));
            inputs.postKeyboardKeyPressed(routed, EKeyboardKey::space, true);
            PPR_TEST_ASSERT(messages.empty());
            PPR_TEST_ASSERT(inputs.m_keyboard.m_character_inputs.empty());
            PPR_TEST_ASSERT(inputs.m_batch.size() == 2u);
            inputs.endNativePump();

            // Pump 2: the release joins the same completed batch.
            inputs.beginNativePump();
            inputs.postKeyboardKeyPressed(routed, EKeyboardKey::space, false);
            inputs.endNativePump();
            PPR_TEST_ASSERT(messages.empty());
            PPR_TEST_ASSERT(inputs.m_native_pumps == 2u);
            PPR_TEST_ASSERT(inputs.m_device_polls == 1u);
            PPR_TEST_ASSERT(inputs.m_timestamp == TimePoint{} + kDt);

            // Defined update point: exactly-once ordered replay through the
            // real devices — press then release, char buffered in order.
            PPR_TEST_ASSERT(not inputs.dispatchBatches());
            PPR_TEST_ASSERT(messages.size() == 2u);
            PPR_TEST_ASSERT(messages[0u].isPressed());
            PPR_TEST_ASSERT(messages[1u].isReleased());
            PPR_TEST_ASSERT(inputs.m_keyboard.m_character_inputs.size() == 1u);
            PPR_TEST_ASSERT(inputs.m_batch.empty());

            // Replaying the consumed batch dispatches nothing: no invented
            // release edges, no replayed one-shot actions.
            PPR_TEST_ASSERT(not inputs.dispatchBatches());
            PPR_TEST_ASSERT(messages.size() == 2u);
        };

        PPR_UNIT_TEST (press_release_pair_survives_to_dispatch) {
            KeyboardDevice device{InputDeviceID{0u}};
            InputListener listener{};
            InputContext context{};
            Array<InputMessage> messages{};
            listener.setRawKeyCallback([&](const TimeSpan, const InputMessage &message) -> EInputMessageResponse {
                messages.push_back(message);
                return EInputMessageResponse::handled;
            });
            context.addInputListener(safe_ptr{&listener}, 0);

            // A same-poll press/release dispatches exactly once each; no
            // pump in between can split or duplicate the pair.
            device.postKeyboardKeyPressed(TimeSpan{}, context, EKeyboardKey::space, true);
            device.postKeyboardKeyPressed(TimeSpan{}, context, EKeyboardKey::space, false);
            PPR_TEST_ASSERT(messages.size() == 2u);
            PPR_TEST_ASSERT(messages[0u].isPressed());
            PPR_TEST_ASSERT(messages[1u].isReleased());
        };

        // F3 exception safety: a throwing character-input callback escapes the
        // production batch dispatch (postCharacterInput is non-noexcept), but
        // the dispatch guard still releases every pending record — the batch
        // is empty afterwards, so teardown destroys the context with no live
        // safe_ptr observers stranded.
        PPR_UNIT_TEST (throwing_char_dispatch_releases_batch_before_teardown) {
            // Teardown is the inverse of setup (same declaration order as the
            // multi-pump test above): the listener outlives the input fake
            // whose batch observed its context.
            InputListener listener{};
            FakeFrameInput inputs{};
            InputContext &routed = inputs.m_global;
            listener.setCharacterInputCallback(
                [&](hal::native::char_t) -> EInputMessageResponse { throw std::runtime_error{"F3 listener failure"}; });
            routed.addInputListener(safe_ptr{&listener}, 0);

            const TimeSpan kDt = std::chrono::milliseconds{16};
            PPR_TEST_ASSERT(not inputs.pollInputDevices(kDt));

            inputs.beginNativePump();
            inputs.postKeyboardCharacterInput(routed, static_cast<hal::native::char_t>('q'));
            PPR_TEST_ASSERT(inputs.m_batch.size() == 1u);
            inputs.endNativePump();

            bool threw = false;
            try {
                std::ignore = inputs.dispatchBatches();
            } catch (const std::runtime_error &) {
                threw = true;
            }
            PPR_TEST_ASSERT(threw);
            PPR_TEST_ASSERT(inputs.m_batch.empty());

            // Exactly-once retained: redispatching the released batch is a
            // silent no-op, inventing no callbacks.
            PPR_TEST_ASSERT(not inputs.dispatchBatches());
        };

        // P0 owned packet: copies at capture time, immune to later live
        // mutation; zero-size surfaces are an explicit non-render outcome.
        PPR_UNIT_TEST (packet_owns_camera_viewport_and_window_copies) {
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
            window.m_framebuffer_size = int2{800, 600};
            PPR_DEFER {
                std::ignore = window.release();
            };

            WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
            std::ignore = viewport.updateFromWindow();

            Camera camera{ECameraProjection::perspective};
            const TimeSpan kDt = std::chrono::milliseconds{16};
            const TimeSpan kElapsed = std::chrono::milliseconds{160};
            FramePacket packet = makeFramePacket(
                7u, kDt, kElapsed, camera.getSnapshot(), viewport.getViewport(), viewport.getViewportRevision(),
                window);
            PPR_TEST_ASSERT(packet.m_frame_id == 7u);
            PPR_TEST_ASSERT(packet.m_delta_time == kDt);
            PPR_TEST_ASSERT(packet.m_total_elapsed == kElapsed);
            PPR_TEST_ASSERT(packet.m_camera.m_revision == camera.getRevision());
            PPR_TEST_ASSERT(packet.m_viewport == viewport.getViewport());
            PPR_TEST_ASSERT(packet.m_window_title == window.m_title);
            PPR_TEST_ASSERT(hasRenderTarget(packet));

            // Live mutation after capture leaves the packet untouched.
            camera.setCameraMode(ECameraProjection::orthographic);
            PPR_TEST_ASSERT(packet.m_camera.m_camera_mode == ECameraProjection::perspective);
        };

        PPR_UNIT_TEST (packet_reports_zero_size_as_non_rendering) {
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{0, 0}, .m_window_size = int2{0, 0}}};
            window.m_framebuffer_size = int2{0, 0};
            PPR_DEFER {
                std::ignore = window.release();
            };

            WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
            std::ignore = viewport.updateFromWindow();

            Camera camera{ECameraProjection::perspective};
            const FramePacket packet = makeFramePacket(
                1u, TimeSpan{}, TimeSpan{}, camera.getSnapshot(), viewport.getViewport(),
                viewport.getViewportRevision(), window);
            PPR_TEST_ASSERT(not hasRenderTarget(packet));
        };

        // F1 consumption: render preparation runs on the packet-owned
        // snapshot, so a post-capture producer mutation cannot alter what
        // the frame prepares. Mirrors the production feed order (capture,
        // stage passes from the packet, mutate live, plan from both).
        PPR_UNIT_TEST (render_preparation_consumes_packet_not_live_camera) {
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
            window.m_framebuffer_size = int2{800, 600};
            PPR_DEFER {
                std::ignore = window.release();
            };

            WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
            std::ignore = viewport.updateFromWindow();

            const TimeSpan kDt = std::chrono::milliseconds{16};
            Camera camera{};
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            camera.updateModel(kDt, model, viewport.getViewport());

            // Capture, then stage the passes from the packet (production
            // order; update() is init-free CPU staging).
            FramePacket packet = makeFramePacket(
                7u, kDt, kDt, camera.getSnapshot(), viewport.getViewport(), viewport.getViewportRevision(), window);
            GridPass grid{};
            TrianglePass triangle{};
            PPR_TEST_ASSERT(not grid.update(kDt, packet.m_camera));
            PPR_TEST_ASSERT(not triangle.update(kDt, packet.m_camera));

            // Post-capture producer mutation: teleport far outside the world.
            // (Its effect is proven below: the live plan culls the tile the
            // packet plan still sees.)
            model.m_origin = float3{100000.0f, 0.0f, 0.0f};
            camera.updateModel(kDt, model, viewport.getViewport());

            // Preparation proof on the CPU planner: the packet plan still
            // sees the home tile, while the mutated live snapshot culls it.
            const GridTileSubmission submissions[] = {
                {.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 3u},
            };
            const Expected<GridTilePlan> packet_plan = GridPass::planTiles(packet.m_camera, submissions);
            PPR_TEST_ASSERT(packet_plan.has_value());
            PPR_TEST_ASSERT(packet_plan->m_tiles.size() == 1u);
            PPR_TEST_ASSERT(packet_plan->m_tiles[0u].m_chunk_id == 0u);

            const Expected<GridTilePlan> live_plan = GridPass::planTiles(camera.getSnapshot(), submissions);
            PPR_TEST_ASSERT(live_plan.has_value());
            PPR_TEST_ASSERT(live_plan->m_tiles.empty());
        };

        // P0 admission policy: focus/background/uncapped transitions
        // re-evaluate the pacer target every frame via getTargetFrameTime().
        PPR_UNIT_TEST (target_time_reflects_focus_and_rate) {
            HeadlessApp test_app{"TargetPolicy"};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());

            test_app.setTargetFrameRate(60);
            const TimeSpan k60 = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 60.0});
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k60);

            test_app.setBackgroundPriority(true);
            const TimeSpan kBackground = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 5.0});
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == kBackground);

            test_app.setBackgroundPriority(false);
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k60);

            test_app.setTargetFrameRate(0);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            test_app.setTargetFrameRate(-5);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
        };

        // Phase 3 pacing policy: DisplaySettings commits map to the §4.1
        // foreground target plus the vsync half; the approved default is
        // follow-vsync (no CPU cap). Legacy setters keep working alongside.
        PPR_UNIT_TEST (display_pacing_modes_map_to_foreground_and_vsync) {
            HeadlessApp test_app{"PacingModes"};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };
            PPR_TEST_ASSERT(test_app.displayController().committed().m_pacing == EFramePacing::follow_vsync);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            PPR_TEST_ASSERT(test_app.presentationVsyncRequested());

            DisplaySettings uncapped{};
            uncapped.m_pacing = EFramePacing::uncapped;
            test_app.setDisplaySettings(uncapped);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            PPR_TEST_ASSERT(not test_app.presentationVsyncRequested());

            DisplaySettings paced{};
            paced.m_pacing = EFramePacing::paced;
            paced.m_cap_hz = 30u;
            paced.m_paced_vsync = false;
            test_app.setDisplaySettings(paced);
            const TimeSpan k30 = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 30.0});
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k30);
            PPR_TEST_ASSERT(not test_app.presentationVsyncRequested());

            paced.m_paced_vsync = true;
            test_app.setDisplaySettings(paced);
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k30);
            PPR_TEST_ASSERT(test_app.presentationVsyncRequested());
        };

        // Phase 3 overlay (§4.2): background priority floors the effective
        // target at 200ms, preserves a slower explicit cap instead of
        // speeding it up to 5fps, and restores the foreground mode on
        // refocus. Vsync never toggles on focus changes.
        PPR_UNIT_TEST (display_unfocused_overlay_floors_and_preserves_slow_caps) {
            HeadlessApp test_app{"PacingOverlay"};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };
            const TimeSpan kFloor{std::chrono::milliseconds{200}};

            test_app.setBackgroundPriority(true);
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == kFloor);

            DisplaySettings slow{};
            slow.m_pacing = EFramePacing::paced;
            slow.m_cap_hz = 1u;
            test_app.setDisplaySettings(slow);
            const TimeSpan k1Fps = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0});
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k1Fps);
            PPR_TEST_ASSERT(test_app.presentationVsyncRequested());

            test_app.setBackgroundPriority(false);
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k1Fps);
            PPR_TEST_ASSERT(test_app.presentationVsyncRequested());
        };

        // Both opt-outs disable the overlay: display.throttle_unfocused and
        // the legacy profiling/bounded-run switch leave the foreground
        // target untouched while background priority is active.
        PPR_UNIT_TEST (display_throttle_opt_outs_disable_overlay) {
            HeadlessApp test_app{"PacingOptOut"};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };
            DisplaySettings no_throttle{};
            no_throttle.m_throttle_unfocused = false;
            test_app.setDisplaySettings(no_throttle);
            PPR_TEST_ASSERT(not test_app.unfocusedThrottleEnabled());
            test_app.setBackgroundPriority(true);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());

            DisplaySettings throttled{};
            test_app.setDisplaySettings(throttled);
            PPR_TEST_ASSERT(test_app.unfocusedThrottleEnabled());
            test_app.setNoFocusThrottle(true);
            PPR_TEST_ASSERT(test_app.getNoFocusThrottle());
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            test_app.setNoFocusThrottle(false);
            const TimeSpan kFloor{std::chrono::milliseconds{200}};
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == kFloor);
        };

        // Telemetry view (§5.1): requested policy, foreground/effective
        // targets, vsync, and the throttle reason in one copyable snapshot.
        PPR_UNIT_TEST (display_view_reports_policy_and_throttle_state) {
            HeadlessApp test_app{"PacingView"};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };
            DisplaySettings paced{};
            paced.m_pacing = EFramePacing::paced;
            paced.m_cap_hz = 60u;
            test_app.setDisplaySettings(paced);
            test_app.setBackgroundPriority(true);

            const DisplayView view = test_app.displayView();
            const TimeSpan k60 = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 60.0});
            const TimeSpan kFloor{std::chrono::milliseconds{200}};
            PPR_TEST_ASSERT(view.m_committed.m_pacing == EFramePacing::paced);
            PPR_TEST_ASSERT(view.m_foreground == k60);
            PPR_TEST_ASSERT(view.m_effective == kFloor);
            PPR_TEST_ASSERT(view.m_vsync);
            PPR_TEST_ASSERT(view.m_background_priority);
            PPR_TEST_ASSERT(view.m_throttle_active);

            // Legacy setters override the live target while the committed
            // policy is retained separately: a paced-1fps foreground stays
            // 1fps under the active overlay instead of speeding up to 5fps.
            test_app.setTargetFrameRate(1);
            const TimeSpan k1Fps = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0});
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k1Fps);
            const DisplayView legacy = test_app.displayView();
            PPR_TEST_ASSERT(legacy.m_committed.m_pacing == EFramePacing::paced);
            PPR_TEST_ASSERT(legacy.m_committed.m_cap_hz == 60u);
            PPR_TEST_ASSERT(legacy.m_foreground == k1Fps);
            PPR_TEST_ASSERT(legacy.m_effective == k1Fps);
            PPR_TEST_ASSERT(legacy.m_effective == test_app.getTargetFrameTime());
        };

        // Setter coverage: manually supplied settings update foreground
        // pacing and requested presentation state after initialization.
        PPR_UNIT_TEST (display_settings_setter_updates_pacing_and_presentation_request) {
            HeadlessApp test_app{"DisplaySetter"};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };
            DisplaySettings display{};
            display.m_pacing = EFramePacing::paced;
            display.m_cap_hz = 30u;
            display.m_paced_vsync = false;
            test_app.setDisplaySettings(display);
            const TimeSpan k30 = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 30.0});
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k30);
            PPR_TEST_ASSERT(not test_app.presentationVsyncRequested());
            PPR_TEST_ASSERT(test_app.unfocusedThrottleEnabled());
            const DisplayView view = test_app.displayView();
            PPR_TEST_ASSERT(view.m_foreground == k30);
            PPR_TEST_ASSERT(view.m_effective == k30);
            PPR_TEST_ASSERT(not view.m_vsync);

            display.m_paced_vsync = true;
            test_app.setDisplaySettings(display);
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k30);
            PPR_TEST_ASSERT(test_app.presentationVsyncRequested());
            PPR_TEST_ASSERT(test_app.displayView().m_vsync);
        };

        // Pre-initialization presentation boundary: requested startup state
        // is available before boot and retained through headless initialization.
        // Actual surface presentation and runtime switching are not exercised here.
        PPR_UNIT_TEST (display_settings_before_initialization_retain_presentation_request) {
            for (const bool vsync: {false, true}) {
                HeadlessApp test_app{"StartupPresentation"};
                DisplaySettings display{};
                display.m_pacing = EFramePacing::paced;
                display.m_cap_hz = 30u;
                display.m_paced_vsync = vsync;
                test_app.setDisplaySettings(display);
                const TimeSpan k30 = std::chrono::duration_cast<TimeSpan>(TimeDuration{1.0 / 30.0});
                PPR_TEST_ASSERT(test_app.presentationVsyncRequested() == vsync);
                PPR_TEST_ASSERT(test_app.displayView().m_vsync == vsync);
                PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k30);

                PPR_TEST_ASSERT(not test_app.boot());
                PPR_DEFER {
                    PPR_TEST_ASSERT(not test_app.teardown());
                };
                PPR_TEST_ASSERT(test_app.presentationVsyncRequested() == vsync);
                PPR_TEST_ASSERT(test_app.displayView().m_vsync == vsync);
                PPR_TEST_ASSERT(test_app.getTargetFrameTime() == k30);
            }
        };
        // wait/pump boundary the pacer sleep primitive drives
        // (pacerSleepSlice_ pumps then waits natively per slice), so a zero
        // count proves no sleep slice executed. The manual clock is
        // controlled: the application holds a non-owning view of it, so the
        // clock outlives the application.
        struct PaceSpyClock final : ITimerClock {
            TimePoint m_now{};

            [[nodiscard]] TimePoint now() const noexcept override {
                return m_now;
            }
        };

        struct PaceSpyPlatform final : IPlatform {
            u32 m_pumps{0u};
            u32 m_waits{0u};
            TimeSpan m_waited{};
            safe_ptr<Application> m_application{};

            [[nodiscard]] std::error_code initialize(Application &application) override {
                m_application.reset(&application);
                return default_value_v;
            }

            [[nodiscard]] std::error_code shutdown(Application &) override {
                m_application.reset();
                return default_value_v;
            }

            [[nodiscard]] std::string_view getPlatformName() const noexcept override {
                return "PaceSpy";
            }

            [[nodiscard]] platform::Version getPlatformVersion() const noexcept override {
                return {.m_major = 0, .m_minor = 0, .m_revision = 0};
            }

            [[nodiscard]] safe_ptr<Application> getApplication() const noexcept override {
                return m_application;
            }

            [[nodiscard]] safe_ptr<IInputService> getInputService() const noexcept override {
                return {};
            }

            [[nodiscard]] safe_ptr<IWindowService> getWindowService() const noexcept override {
                return {};
            }

            [[nodiscard]] safe_ptr<IPlayerService> getPlayerService() const noexcept override {
                return {};
            }

            [[nodiscard]] std::error_code update(const TimeSpan) override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code pumpNativeEvents() override {
                ++m_pumps;
                return default_value_v;
            }

            [[nodiscard]] std::error_code waitNativeEvents(const TimeSpan timeout) override {
                ++m_waits;
                m_waited += timeout;
                return default_value_v;
            }
        };

        // F2 admission (behavioral, load-independent): an exit latched
        // before the clock stage stops the wait before any pacer sleep slice
        // executes — no pump, no native wait, no admitted frame time. The
        // controlled clock never advances, so dt is exactly zero; the spy
        // above proves zero slices ran. Holds under any load — this
        // behavioral assertion replaces the former wall-clock latency leaf
        // (removed: shared-machine jitter breaks fixed wall-clock bounds).
        PPR_UNIT_TEST (preset_exit_stops_clock_admission) {
            PaceSpyClock clock{};
            auto platform = std::make_unique<PaceSpyPlatform>();
            PaceSpyPlatform *const spy = platform.get();

            struct ClockApp final : HeadlessApp {
                using HeadlessApp::HeadlessApp;
                using Application::advanceClock_;
            };

            ClockApp test_app{"ClockStop", std::move(platform), clock};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };
            test_app.setBackgroundPriority(true);

            test_app.requestExit();
            TimeSpan dt{std::chrono::seconds{9}};
            PPR_TEST_ASSERT(not test_app.advanceClock_(&dt));
            PPR_TEST_ASSERT(spy->m_pumps == 0u);
            PPR_TEST_ASSERT(spy->m_waits == 0u);
            PPR_TEST_ASSERT(spy->m_waited == TimeSpan{});
            PPR_TEST_ASSERT(dt == TimeSpan{});
        };

        // Blur → enable → refocus: blurring while throttling is disabled
        // preserves the raw focus loss, so enabling throttle_unfocused
        // while still blurred activates the overlay without another focus
        // event; refocus restores the foreground target. The focus→priority
        // mapping lives in the production callback; this test pins overlay
        // behavior through the same public setBackgroundPriority calls.
        PPR_UNIT_TEST (editor_blur_enable_refocus_activates_throttle) {
            PaceSpyClock clock{};
            HeadlessApp test_app{"BlurEnableRefocus", std::make_unique<PaceSpyPlatform>(), clock};
            DisplaySettings no_throttle{};
            no_throttle.m_throttle_unfocused = false;
            test_app.setDisplaySettings(no_throttle);
            test_app.setBackgroundPriority(true);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            PPR_TEST_ASSERT(not test_app.displayView().m_throttle_active);
            PPR_TEST_ASSERT(test_app.displayView().m_vsync);

            DisplaySettings throttled{};
            test_app.setDisplaySettings(throttled);
            const TimeSpan kFloor{std::chrono::milliseconds{200}};
            PPR_TEST_ASSERT(test_app.getTargetFrameTime() == kFloor);
            PPR_TEST_ASSERT(test_app.displayView().m_throttle_active);
            PPR_TEST_ASSERT(test_app.displayView().m_effective == kFloor);
            PPR_TEST_ASSERT(test_app.displayView().m_vsync);

            test_app.setBackgroundPriority(false);
            PPR_TEST_ASSERT(not test_app.getTargetFrameTime().has_value());
            PPR_TEST_ASSERT(not test_app.displayView().m_throttle_active);
            PPR_TEST_ASSERT(test_app.displayView().m_vsync);
        };

        struct RunCleanupApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

            int m_frames{0};
            int m_exit_after{3};
            std::error_code m_clause{};
            std::error_code m_shutdown_error{};
            bool m_fail_shutdown{false};

        protected:
            [[nodiscard]] std::error_code update([[maybe_unused]] const TimeSpan dt) override {
                if (++m_frames >= m_exit_after) {
                    requestExit(m_clause);
                }
                return std::error_code{};
            }

            [[nodiscard]] std::error_code shutdown() override {
                const std::error_code ec = Application::shutdown();
                if (ec) {
                    return ec;
                }
                if (m_fail_shutdown) {
                    return m_shutdown_error;
                }
                return std::error_code{};
            }
        };

        // Fused cleanup: a shutdown failure on a graceful exit must become
        // run()'s return value (previously unreachable behind the defer).
        PPR_UNIT_TEST (run_returns_shutdown_error_on_graceful_exit) {
            RunCleanupApp test_app{"RunCleanupGraceful"};
            test_app.m_fail_shutdown = true;
            test_app.m_shutdown_error = std::make_error_code(std::errc::io_error);
            PPR_TEST_ASSERT(test_app.run() == std::make_error_code(std::errc::io_error));
            PPR_TEST_ASSERT(test_app.m_frames == 3);
        };

        // First error wins: the loop cause outranks a later shutdown error.
        PPR_UNIT_TEST (run_keeps_first_error_across_shutdown_failure) {
            RunCleanupApp test_app{"RunCleanupCause"};
            test_app.m_clause = std::make_error_code(std::errc::operation_canceled);
            test_app.m_fail_shutdown = true;
            test_app.m_shutdown_error = std::make_error_code(std::errc::io_error);
            PPR_TEST_ASSERT(test_app.run() == std::make_error_code(std::errc::operation_canceled));
            PPR_TEST_ASSERT(test_app.m_frames == 3);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest frame_stages = UnitTest::Named("frame_stages") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::FrameStages::native_pump_without_devices_is_repeatable_noop,
            detail::FrameStages::posted_text_survives_native_pumps,
            detail::FrameStages::device_time_advances_exactly_once_per_poll,
            detail::FrameStages::batched_multi_pump_dispatches_once_in_order,
            detail::FrameStages::press_release_pair_survives_to_dispatch,
            detail::FrameStages::throwing_char_dispatch_releases_batch_before_teardown,
            detail::FrameStages::packet_owns_camera_viewport_and_window_copies,
            detail::FrameStages::packet_reports_zero_size_as_non_rendering,
            detail::FrameStages::render_preparation_consumes_packet_not_live_camera,
            detail::FrameStages::target_time_reflects_focus_and_rate,
            detail::FrameStages::display_pacing_modes_map_to_foreground_and_vsync,
            detail::FrameStages::display_unfocused_overlay_floors_and_preserves_slow_caps,
            detail::FrameStages::display_throttle_opt_outs_disable_overlay,
            detail::FrameStages::display_view_reports_policy_and_throttle_state,
            detail::FrameStages::display_settings_setter_updates_pacing_and_presentation_request,
            detail::FrameStages::display_settings_before_initialization_retain_presentation_request,
            detail::FrameStages::editor_blur_enable_refocus_activates_throttle,
            detail::FrameStages::preset_exit_stops_clock_admission,
            detail::FrameStages::run_returns_shutdown_error_on_graceful_exit,
            detail::FrameStages::run_keeps_first_error_across_shutdown_failure,
        });
    };

    const UnitTest &frame_stagesTests() noexcept {
        return frame_stages;
    }
} // namespace pP::tests
