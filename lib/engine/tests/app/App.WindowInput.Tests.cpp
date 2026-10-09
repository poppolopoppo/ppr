module;
#include "pP/UnitTest.h"

#include <cstdio>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import engine.shader;
import imgui;
import imgui_internal;
import std;

namespace pP::tests::detail {
    namespace SharedWindowInputGpu {
        // Shared process device for the window_input GPU leaves.
        //
        // Shape mirrors Asset.GpuFixture SharedGpu (Holder + mutex + refcount
        // + acquire/release + service accessors); adapted: no Application is
        // booted — these leaves address the IRhiService/IShaderService process
        // singletons directly, so the device is shared by construction and the
        // Holder owns only the boot/teardown pairing. The window_input group
        // acquires once around its GPU leaves (CPU-only leaves hold no refs,
        // cf. the Asset render umbrella); per-leaf Renderer/Window/UI state
        // stays leaf-local and tears down in inverse order before the group
        // release, so every safe_ptr dies before the device it observes.
        // initialize() on the live pair is a verified no-op (RHI.cpp:235,
        // Shader.cpp:170), so the per-leaf initialize asserts kept below are
        // known-state gates, not rebuilds.
        //
        // Sharing stops at the group boundary: renderer_triangle_reinit_ok,
        // renderer_off_thread_fails_closed, imgui_live_shutdown_idempotent and
        // imgui_live_same_object_reinit are standalone top-level registrations
        // with no shared scope in this file, so they keep private devices. A
        // sticky boot-once holder would share them, but that is a different
        // protocol.
        struct Holder {
            std::mutex m_mutex{};
            unsigned m_refs = 0u;
        };

        [[nodiscard]] Holder &holder() {
            static Holder s_holder{};
            return s_holder;
        }

        [[nodiscard]] std::error_code acquire() {
            Holder &state = holder();
            const std::lock_guard lock{state.m_mutex};
            if (state.m_refs == 0u) {
                const auto shader = IShaderService::get();
                const auto rhi = IRhiService::get();
                std::ignore = rhi->shutdown();
                std::ignore = shader->shutdown();
                if (const std::error_code err = shader->initialize()) {
                    return err;
                }
                if (const std::error_code err = rhi->initialize(rhi::DeviceType::Default, *shader)) {
                    std::ignore = shader->shutdown();
                    return err;
                }
            }
            ++state.m_refs;
            return default_value_v;
        }

        [[nodiscard]] std::error_code release() {
            Holder &state = holder();
            const std::lock_guard lock{state.m_mutex};
            if (state.m_refs == 0u) {
                return default_value_v;
            }
            PPR_ASSERT(state.m_refs > 0u);
            if (--state.m_refs == 0u) {
                const auto shader = IShaderService::get();
                const auto rhi = IRhiService::get();
                const std::error_code rhi_err = rhi->shutdown();
                const std::error_code shader_err = shader->shutdown();
                if (rhi_err) {
                    return rhi_err;
                }
                return shader_err;
            }
            return default_value_v;
        }

        [[nodiscard]] safe_ptr<IRhiService> rhi() {
            return IRhiService::get();
        }

        [[nodiscard]] safe_ptr<IShaderService> shader() {
            return IShaderService::get();
        }

        void resetBetweenLeaves() {
            // Explicit state-reset between leaves (not a rebuild): the first
            // GPU leaf in a fresh process observes a null ImGui current, so
            // every leaf starts from that same known state instead of
            // inheriting a dangling context from the previous leaf's
            // released UI service. Each leaf sets its own context after its
            // ui->initialize; the service-validity asserts gate the live
            // device the group acquire owns.
            const auto shader = IShaderService::get();
            const auto rhi = IRhiService::get();
            PPR_TEST_ASSERT(shader.isValid());
            PPR_TEST_ASSERT(rhi.isValid());
            ImGui::SetCurrentContext(nullptr);
        }
    } // namespace SharedWindowInputGpu

    namespace WindowInput {
        struct FakeInputService final : IInputService {
            KeyboardDevice m_keyboard{InputDeviceID{1u}};
            MouseDevice m_mouse{InputDeviceID{2u}};
            GamepadDevice m_gamepad{InputDeviceID{3u}};
            InputContext m_global{};
            std::vector<SharedInputContext> m_contexts{};
            float2 m_last_cursor{0.0f, 0.0f};

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

            [[nodiscard]] bool hasInputContext(const InputContext &context) const noexcept override {
                if (&m_global == &context) {
                    return true;
                }
                return std::ranges::any_of(
                    m_contexts, [&](const SharedInputContext &stored) noexcept { return stored.get() == &context; });
            }

            void addInputContext(SharedInputContext context) override {
                m_contexts.push_back(std::move(context));
            }

            bool removeInputContext(const InputContext &context) override {
                const auto it = std::ranges::find_if(
                    m_contexts, [&](const SharedInputContext &stored) noexcept { return stored.get() == &context; });
                if (it == m_contexts.end()) {
                    return false;
                }
                m_contexts.erase(it);
                return true;
            }

            void assignInputContextToDevice(InputDeviceID, SharedInputContext) override {
            }

            void clearInputContextDeviceAssignments() override {
                m_contexts.clear();
            }

            [[nodiscard]] std::error_code enumerateInputContexts(Collector<InputContext>) const override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code enumerateInputContextDeviceAssignments(
                Collector<IInputDevice, InputContext>) const override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code pollInputDevices(TimeSpan) override {
                return default_value_v;
            }

            void resetInputDevices() noexcept override {
                m_keyboard.resetInputState();
                m_mouse.resetInputState();
                m_gamepad.resetInputState();
            }

            void postKeyboardCharacterInput(const InputContext &context, const hal::native::char_t codepoint) override {
                m_keyboard.postKeyboardCharacterInput(context, codepoint);
            }

            void postKeyboardKeyPressed(
                const InputContext &context, const EKeyboardKey key, const bool pressed) override {
                m_keyboard.postKeyboardKeyPressed(TimeSpan{}, context, key, pressed);
            }

            void postMouseButtonPressed(
                const InputContext &context, const EMouseButton button, const bool pressed) override {
                m_mouse.postMouseButtonPressed(TimeSpan{}, context, button, pressed);
            }

            void postMouseCursorPosition(const InputContext &context, const float2 &absolute_pos) override {
                m_last_cursor = absolute_pos;
                m_mouse.postMouseCursorPosition(TimeSpan{}, context, absolute_pos);
            }

            void postMouseScrollWheel(const InputContext &context, const float2 &delta) override {
                m_mouse.postMouseScrollWheel(TimeSpan{}, context, delta);
            }

            [[nodiscard]] DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event) override {
                return {};
            }

            [[nodiscard]] DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event) override {
                return {};
            }
        };

        struct ImGuiDrawable final {
            IUIService &m_service;

            [[nodiscard]] std::error_code render(const DrawContext &draw_context) {
                return m_service.render(draw_context);
            }
        };

        PPR_UNIT_TEST (mouse_keeps_client_space_with_nonzero_window_origin) {
            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{400, 300}, .m_window_size = int2{800, 600}}};
            {
                // NOTE: production ctor split (inputs-only + initialize(window));
                // migrated from the removed 2-arg form. Shutdown before scope
                // exit: the dtor asserts the window is detached.
                WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
                PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
                const float2 client{10.0f, 20.0f};
                window.m_when_mouse_moved(window, client);

                PPR_TEST_ASSERT(inputs.m_last_cursor.x == client.x && inputs.m_last_cursor.y == client.y);

                const float2 &stored = inputs.m_mouse.m_cursor_pos.m_raw.m_absolute;
                PPR_TEST_ASSERT(stored.x == client.x && stored.y == client.y);
                PPR_TEST_ASSERT(not routed.shutdown());
            }
            std::ignore = window.release();
        };

        PPR_UNIT_TEST (char_posted_after_poll_clear_survives_to_consumer) {
            KeyboardDevice device{InputDeviceID{0u}};
            InputContext context{};

            // Frame start: transient clear runs before window-event dispatch.
            PPR_TEST_ASSERT(not device.pollInputMessages(TimeSpan{}));
            // Dispatch: character arrives via window event.
            device.postKeyboardCharacterInput(context, static_cast<hal::native::char_t>('a'));
            // Consumer (ImGui::NewFrame) observes it in the same frame.
            PPR_TEST_ASSERT(device.m_character_inputs.size() == 1u);
            // Next frame start clears it exactly once.
            PPR_TEST_ASSERT(not device.pollInputMessages(TimeSpan{}));
            PPR_TEST_ASSERT(device.m_character_inputs.empty());
        };

        PPR_UNIT_TEST (reset_clears_pending_characters) {
            KeyboardDevice device{InputDeviceID{0u}};
            InputContext context{};

            device.postKeyboardCharacterInput(context, static_cast<hal::native::char_t>('b'));
            PPR_TEST_ASSERT(device.m_character_inputs.size() == 1u);

            device.resetInputState();
            PPR_TEST_ASSERT(device.m_character_inputs.empty());
        };

        // Routing-owned background-drag transition table —
        // per-button latch for LEFT+MIDDLE only, driven through the latch-owned
        // detector. Harness mirrors production (ApplicationEditor): the
        // foreground listener is added manually at ui, the background player
        // listener is owned by the test but registered by the latch at player,
        // and the detector is owned and registered internally at detector.
        // Only isEngaged() is observable; counts stay private. The dtor
        // detaches best-effort (shutdown aggregates not_connected when the
        // test already detached).
        struct TapHarness {
            InputListener m_foreground{};
            InputListener m_background{};
            std::unique_ptr<InputBackgroundLatch> m_latch{};
            InputContext m_context{};

            TapHarness() {
                m_foreground.setRawKeyCallback(
                    [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });
                m_background.setRawKeyCallback(
                    [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });
            }

            ~TapHarness() {
                if (m_latch) {
                    std::ignore = m_latch->shutdown(m_context);
                    std::ignore = m_context.removeInputListener(m_foreground);
                }
            }

            [[nodiscard]] std::error_code attach() {
                m_context.addInputListener(
                    safe_ptr<InputListener>{&m_foreground}, static_cast<int>(EInputListenerPriority::ui));
                m_latch = std::make_unique<InputBackgroundLatch>(
                    static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&m_background},
                    static_cast<int>(EInputListenerPriority::player),
                    static_cast<int>(EInputListenerPriority::detector));
                return m_latch->initialize(m_context, safe_ptr<const InputListener>{&m_foreground});
            }

            void post(const EMouseButton button, const EInputMessageEvent event) {
                const std::optional<InputKey> key = InputKey::from(button);
                PPR_TEST_ASSERT(key.has_value());
                const InputMessage message{
                    *key,
                    InputValue{
                        InputDigital{event == EInputMessageEvent::pressed or event == EInputMessageEvent::repeat}},
                    InputDeviceID{0u}, event};
                std::ignore = m_context.postKeyEvent(TimeSpan{}, message);
            }
        };

        PPR_UNIT_TEST (drag_latch_initial_state_disengaged) {
            InputListener foreground{};
            foreground.setRawKeyCallback(
                [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });
            InputListener background{};
            background.setRawKeyCallback(
                [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });
            // No default ctor by design: priorities + background owner are
            // required up front. Pre-attach the latch is disengaged.
            const InputBackgroundLatch latch(
                static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
                static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
            PPR_TEST_ASSERT(not latch.isEngaged());
        };

        PPR_UNIT_TEST (drag_latch_background_press_engages_left) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::left, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        PPR_UNIT_TEST (drag_latch_middle_button_engages) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::middle, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::middle, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        // Arrival-equals-background: the detector observes only presses the ui
        // tier left unhandled, so every observed press engages with no origin query.
        PPR_UNIT_TEST (drag_latch_arrival_always_engages_no_origin_query) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::left, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        PPR_UNIT_TEST (drag_latch_right_and_thumb_ignored) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::right, EInputMessageEvent::pressed);
            tap.post(EMouseButton::thumb0, EInputMessageEvent::pressed);
            tap.post(EMouseButton::thumb1, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
            tap.post(EMouseButton::right, EInputMessageEvent::released);
            tap.post(EMouseButton::thumb0, EInputMessageEvent::released);
            tap.post(EMouseButton::thumb1, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
            // Look buttons unaffected by non-look traffic.
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
        };

        PPR_UNIT_TEST (drag_latch_rmb_independent_of_latch) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            tap.post(EMouseButton::right, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::right, EInputMessageEvent::released);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::left, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        PPR_UNIT_TEST (drag_latch_overlap_survives_single_release) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            tap.post(EMouseButton::middle, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::left, EInputMessageEvent::released);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::middle, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        PPR_UNIT_TEST (drag_latch_release_without_press_resets) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
        };

        PPR_UNIT_TEST (drag_latch_reset_restores_disengaged) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            tap.post(EMouseButton::middle, EInputMessageEvent::pressed);
            tap.m_latch->resetInputState();
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        // Blur/disconnect backstop entry (cf.
        // ApplicationEditor::onMainWindowFocused_): resetInputState clears a
        // stale hold even though no release was ever observed.
        PPR_UNIT_TEST (drag_latch_tap_state_reset_clears_stale_hold) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.m_latch->resetInputState();
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
            tap.post(EMouseButton::left, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        // G1-3 EVIDENCE (broadcast Phase 1): live Nav/capture signal values
        // across focus states. Asserts only code-proven facts; the table below
        // is recorded to stderr and frozen into broadcast-input-routing.md.
        PPR_UNIT_TEST (imgui_nav_signal_evidence) {
            // Shared device (group acquire); the initialize pair is a no-op known-state gate, not a rebuild.
            SharedWindowInputGpu::resetBetweenLeaves();
            const auto shader = SharedWindowInputGpu::shader();
            const auto rhi = SharedWindowInputGpu::rhi();
            PPR_TEST_ASSERT(not shader->initialize());
            PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
            window.m_framebuffer_size = int2{800, 600};
            {
                WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
                PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
                auto ui = ui::createImGuiService();
                PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));

                WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
                std::ignore = viewport.updateFromWindow();
                const TimeSpan frame{std::chrono::milliseconds{16}};

                auto *imgui_context = static_cast<ImGuiContext *>(ui->getContext());
                PPR_TEST_ASSERT(imgui_context != nullptr);
                ImGui::SetCurrentContext(imgui_context);

                // Init defaults: our init sets both nav bits (ImGui.cpp:244).
                const ImGuiIO &init_io = ImGui::GetIO();
                PPR_TEST_ASSERT((init_io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0);
                PPR_TEST_ASSERT((init_io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0);
                PPR_TEST_ASSERT(init_io.ConfigNavCaptureKeyboard);
                PPR_TEST_ASSERT(not init_io.NavActive);
                std::println("[nav-evidence] init: nav_kb=1 nav_gp=1 capnavkb=1 navactive=0");

                // Empty frame: nothing focused, nothing captured.
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                const ImGuiIO &empty_io = ImGui::GetIO();
                PPR_TEST_ASSERT(not empty_io.WantCaptureKeyboard);
                PPR_TEST_ASSERT(not empty_io.WantCaptureMouse);
                PPR_TEST_ASSERT(not empty_io.WantCaptureMouseUnlessPopupClose);
                PPR_TEST_ASSERT(not empty_io.NavActive);
                PPR_TEST_ASSERT(ImGui::GetCurrentContext()->ActiveId == 0);
                std::println("[nav-evidence] empty: capturekb=0 capturemouse=0 unlesspopup=0 navactive=0 activeid=0");
                ImGui::EndFrame();

                // Text focus: ActiveId branch captures, independent of nav.
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(ImGui::Begin("evidence_text"));
                ImGui::SetKeyboardFocusHere();
                char probe_text[64]{};
                std::ignore = ImGui::InputText("##probe", probe_text, sizeof(probe_text));
                ImGui::End();
                ImGui::Render();
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                const ImGuiIO &text_io = ImGui::GetIO();
                PPR_TEST_ASSERT(text_io.WantCaptureKeyboard);
                // Capture here rides the NavActive branch (focus primes nav;
                // ActiveId lands only once real typing starts) — the recorded
                // ActiveId below documents, not asserts, that.
                PPR_TEST_ASSERT(text_io.NavActive);
                if (ImGui::GetCurrentContext()->ActiveId != 0) {
                    std::println("[nav-evidence] text: capturekb=1 navactive=1 activeid!=0");
                } else {
                    std::println(
                        "[nav-evidence] text: capturekb=1 navactive=1 activeid=0 (focus primes nav; typing activates)");
                }
                ImGui::Render();

                // Plain window focus (no widgets): solution to S3 — does ANY
                // focused window capture? Values recorded, focus sanity
                // asserted; routing verdict follows in the keyboard tests.
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(ImGui::Begin("evidence_plain"));
                ImGui::SetWindowFocus();
                const bool plain_focused = ImGui::IsWindowFocused();
                ImGui::End();
                PPR_TEST_ASSERT(plain_focused);
                ImGui::Render();
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                const ImGuiIO &plain_io = ImGui::GetIO();
                std::println(
                    "[nav-evidence] plain-focus: capturekb={} navactive={} activeid={} (recorded)",
                    plain_io.WantCaptureKeyboard ? 1 : 0, plain_io.NavActive ? 1 : 0,
                    static_cast<unsigned long long>(ImGui::GetCurrentContext()->ActiveId));
                ImGui::Render();

                // Nav ladder (no new widgets; NavWindow persists): with
                // NavEnableKeyboard cleared, the gamepad branch alone must
                // sustain NavActive for the focused window. Clearing both bits
                // drops NavActive — nav-off defaults to player for the
                // gamepad gate, and keyboard keeps only ActiveId/modal.
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(ImGui::GetIO().NavActive);
                if (ImGui::GetIO().WantCaptureKeyboard) {
                    std::println("[nav-evidence] navoff-kb: navactive=1 capturekb=1 (unexpected: record for Gate 1)");
                } else {
                    // Reference-doc line 101 live: gamepad-only nav does NOT
                    // set WantCaptureKeyboard without the keyboard bit — the
                    // split is by design (keys flow; gamepad stays captured
                    // via NavActive + NavEnableGamepad).
                    std::println("[nav-evidence] navoff-kb: navactive=1 capturekb=0 (split by design)");
                }
                ImGui::EndFrame();
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(not ImGui::GetIO().NavActive);
                PPR_TEST_ASSERT(not ImGui::GetIO().WantCaptureKeyboard);
                std::println("[nav-evidence] navoff-both: navactive=0 capturekb=0 (nav-off defaults to player)");

                PPR_TEST_ASSERT(not ui->shutdown());
                PPR_TEST_ASSERT(not ui->shutdown());
                std::ignore = ui.release();
                PPR_TEST_ASSERT(not routed.shutdown());
            }
            std::ignore = window.release();
        };

        // Broadcast Phase 1 (intended behavior): background keys yield handled
        // so a player listener downstream receives them; a focused text field
        // consumes them; a background click clears focus (S4) and keys flow
        // again next frame. The LMB press itself is handled (BOTH vote).
        PPR_UNIT_TEST (imgui_background_click_returns_keys_to_player) {
            // Shared device (group acquire); the initialize pair is a no-op known-state gate, not a rebuild.
            SharedWindowInputGpu::resetBetweenLeaves();
            const auto shader = SharedWindowInputGpu::shader();
            const auto rhi = SharedWindowInputGpu::rhi();
            PPR_TEST_ASSERT(not shader->initialize());
            PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
            window.m_framebuffer_size = int2{800, 600};

            InputAction player_move{"PlayerMove", EInputValueType::digital};
            InputMapping player_mapping{"Player"};
            player_mapping.mapInputKey(SharedInputAction{&player_move}, InputKey::w);
            InputListener player_listener{};
            player_listener.addInputMapping(SharedInputMapping{&player_mapping}, 0);
            int move_count = 0;
            player_move.setTriggered(
                [&move_count](const InputActionEvent &, const InputKey &) noexcept { ++move_count; });
            {
                WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
                PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
                routed.m_context.addInputListener(safe_ptr<InputListener>{&player_listener}, 2);
                auto ui = ui::createImGuiService();
                PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));

                WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
                std::ignore = viewport.updateFromWindow();
                const TimeSpan frame{std::chrono::milliseconds{16}};
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                ImGui::EndFrame();

                const InputMessage w_press{
                    InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};

                // Background: UI yields (handled) so the consuming player
                // receives (response consumed by the player, not the UI).
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, w_press) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(move_count == 1);

                // Focus a text field: UI consumes, player stays silent.
                // NOTE: SetNextWindow* needs an open frame — update first.
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                auto *imgui_context = static_cast<ImGuiContext *>(ui->getContext());
                PPR_TEST_ASSERT(imgui_context != nullptr);
                ImGui::SetCurrentContext(imgui_context);
                ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
                ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
                PPR_TEST_ASSERT(ImGui::Begin("keys_probe"));
                ImGui::SetKeyboardFocusHere();
                char probe_text[64]{};
                std::ignore = ImGui::InputText("##probe", probe_text, sizeof(probe_text));
                ImGui::End();
                ImGui::Render();
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, w_press) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(move_count == 1);
                ImGui::Render();

                // Background click (void point, no widget): press itself is
                // handled (BOTH). Stock ImGui does NOT clear window focus on
                // void clicks (verified: no FocusWindow(NULL) on void path) —
                // deselect is app-side, so the test performs the editor's
                // deselect step explicitly (Phase 2 detector territory).
                // Afterwards keys flow again next frame (S4).
                const InputMessage void_move{
                    InputKey::mouse_2d, InputValue{InputAxis2D{float2{750.0f, 550.0f}, float2{0.0f, 0.0f}}},
                    InputDeviceID{0u}, EInputMessageEvent::axis};
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, void_move) == EInputMessageResponse::handled);
                const InputMessage void_click{
                    InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u},
                    EInputMessageEvent::pressed};
                PPR_TEST_ASSERT(
                    routed.m_context.postKeyEvent(TimeSpan{}, void_click) == EInputMessageResponse::handled);
                // App-side deselect needs an open frame: update opens it (the
                // void click is processed here), clear focus, close, refresh.
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(not ui->hasMouseCaptureUnlessPopupClose());
                PPR_TEST_ASSERT(not ui->clearWindowFocus()); // production drain: editor post-UI boundary
                ImGui::Render();
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                // Focus cleared: UI yields again, the consuming player takes it.
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, w_press) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(move_count == 2);

                PPR_TEST_ASSERT(not ui->shutdown());
                PPR_TEST_ASSERT(not ui->shutdown());
                std::ignore = ui.release();
                PPR_TEST_ASSERT(not routed.shutdown());
            }
            std::ignore = window.release();
        };

        // Broadcast Phase 1 (intended behavior): gamepad buttons follow
        // NavActive + NavEnableGamepad — background (no nav) flows to the
        // player (S6 sink fixed); a focused window captures; clearing the
        // gamepad nav bit passes everything through (nav-off passthrough).
        PPR_UNIT_TEST (imgui_gamepad_follows_nav_active) {
            // Shared device (group acquire); the initialize pair is a no-op known-state gate, not a rebuild.
            SharedWindowInputGpu::resetBetweenLeaves();
            const auto shader = SharedWindowInputGpu::shader();
            const auto rhi = SharedWindowInputGpu::rhi();
            PPR_TEST_ASSERT(not shader->initialize());
            PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
            window.m_framebuffer_size = int2{800, 600};

            InputAction player_pad{"PlayerPad", EInputValueType::digital};
            InputMapping player_mapping{"Player"};
            player_mapping.mapInputKey(SharedInputAction{&player_pad}, InputKey::gamepad_face_button_bottom);
            InputListener player_listener{};
            player_listener.addInputMapping(SharedInputMapping{&player_mapping}, 0);
            int pad_count = 0;
            player_pad.setTriggered([&pad_count](const InputActionEvent &, const InputKey &) noexcept { ++pad_count; });
            {
                WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
                PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
                routed.m_context.addInputListener(safe_ptr<InputListener>{&player_listener}, 2);
                auto ui = ui::createImGuiService();
                PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));

                WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
                std::ignore = viewport.updateFromWindow();
                const TimeSpan frame{std::chrono::milliseconds{16}};
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                ImGui::EndFrame();

                const InputMessage pad_press{
                    InputKey::gamepad_face_button_bottom, InputValue{InputDigital{true}}, InputDeviceID{0u},
                    EInputMessageEvent::pressed};

                // Background (no nav): UI yields, player receives.
                PPR_TEST_ASSERT(
                    routed.m_context.postKeyEvent(TimeSpan{}, pad_press) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(pad_count == 1);

                // Focused window (nav active): UI consumes, player silent.
                // NOTE: Begin needs an open frame — update first.
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                auto *imgui_context = static_cast<ImGuiContext *>(ui->getContext());
                PPR_TEST_ASSERT(imgui_context != nullptr);
                ImGui::SetCurrentContext(imgui_context);
                PPR_TEST_ASSERT(ImGui::Begin("pad_probe"));
                ImGui::SetWindowFocus();
                ImGui::End();
                ImGui::Render();
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(ImGui::GetIO().NavActive);
                PPR_TEST_ASSERT(
                    routed.m_context.postKeyEvent(TimeSpan{}, pad_press) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(pad_count == 1);
                ImGui::Render();

                // Nav-off passthrough: gamepad nav bit cleared, focused window
                // or not, the pad always reaches the player.
                ImGui::SetCurrentContext(imgui_context);
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(
                    routed.m_context.postKeyEvent(TimeSpan{}, pad_press) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(pad_count == 2);

                PPR_TEST_ASSERT(not ui->shutdown());
                PPR_TEST_ASSERT(not ui->shutdown());
                std::ignore = ui.release();
                PPR_TEST_ASSERT(not routed.shutdown());
            }
            std::ignore = window.release();
        };

        // Broadcast Phase 1 lock-in (intended behavior): wheel is hover-split
        // — background flows to the fov listener, a hovered widget consumes.
        PPR_UNIT_TEST (imgui_wheel_hover_split) {
            // Shared device (group acquire); the initialize pair is a no-op known-state gate, not a rebuild.
            SharedWindowInputGpu::resetBetweenLeaves();
            const auto shader = SharedWindowInputGpu::shader();
            const auto rhi = SharedWindowInputGpu::rhi();
            PPR_TEST_ASSERT(not shader->initialize());
            PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
            window.m_framebuffer_size = int2{800, 600};

            InputAction camera_fov{"CameraFov", EInputValueType::axis_1d};
            InputMapping camera_mapping{"Camera"};
            camera_mapping.mapInputKey(SharedInputAction{&camera_fov}, InputKey::mouse_wheel_axis_y);
            InputListener camera_listener{};
            camera_listener.addInputMapping(SharedInputMapping{&camera_mapping}, 0);
            int fov_count = 0;
            camera_fov.setTriggered([&fov_count](const InputActionEvent &, const InputKey &) noexcept { ++fov_count; });
            {
                WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
                PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
                routed.m_context.addInputListener(safe_ptr<InputListener>{&camera_listener}, 1);
                auto ui = ui::createImGuiService();
                PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));

                WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
                std::ignore = viewport.updateFromWindow();
                const TimeSpan frame{std::chrono::milliseconds{16}};
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                ImGui::EndFrame();

                const InputMessage wheel{
                    InputKey::mouse_wheel_axis_y, InputValue{InputAxis1D{0.0f, 1.0f}}, InputDeviceID{0u},
                    EInputMessageEvent::axis};

                // Background: UI yields, camera fov receives.
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, wheel) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(fov_count == 1);

                // Hovered widget: UI consumes, camera stays silent.
                // NOTE: SetNextWindow* needs an open frame — update first.
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                auto *imgui_context = static_cast<ImGuiContext *>(ui->getContext());
                PPR_TEST_ASSERT(imgui_context != nullptr);
                ImGui::SetCurrentContext(imgui_context);
                ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
                ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
                PPR_TEST_ASSERT(ImGui::Begin("wheel_probe"));
                ImGui::End();
                ImGui::GetIO().AddMousePosEvent(100.0f, 100.0f);
                ImGui::Render();
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(ImGui::GetIO().WantCaptureMouseUnlessPopupClose);
                PPR_TEST_ASSERT(ui->hasMouseCaptureUnlessPopupClose());
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, wheel) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(fov_count == 1);

                PPR_TEST_ASSERT(not ui->shutdown());
                PPR_TEST_ASSERT(not ui->shutdown());
                std::ignore = ui.release();
                PPR_TEST_ASSERT(not routed.shutdown());
            }
            std::ignore = window.release();
        };

        // Broadcast Phase 1 teardown leaf: shutdown resets consume flags to
        // non-consuming (defaults to player), so a re-initialized service
        // routes to the player before its first update.
        PPR_UNIT_TEST (imgui_consume_flags_reset_on_shutdown) {
            // Shared device (group acquire); the initialize pair is a no-op known-state gate, not a rebuild.
            SharedWindowInputGpu::resetBetweenLeaves();
            const auto shader = SharedWindowInputGpu::shader();
            const auto rhi = SharedWindowInputGpu::rhi();
            PPR_TEST_ASSERT(not shader->initialize());
            PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
            window.m_framebuffer_size = int2{800, 600};

            InputAction player_move{"PlayerMove", EInputValueType::digital};
            InputMapping player_mapping{"Player"};
            player_mapping.mapInputKey(SharedInputAction{&player_move}, InputKey::w);
            InputListener player_listener{};
            player_listener.addInputMapping(SharedInputMapping{&player_mapping}, 0);
            int move_count = 0;
            player_move.setTriggered(
                [&move_count](const InputActionEvent &, const InputKey &) noexcept { ++move_count; });
            {
                WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
                PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
                routed.m_context.addInputListener(safe_ptr<InputListener>{&player_listener}, 2);
                auto ui = ui::createImGuiService();
                PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));

                WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
                std::ignore = viewport.updateFromWindow();
                const TimeSpan frame{std::chrono::milliseconds{16}};

                // First life ends with consuming flags (focused text).
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                auto *imgui_context = static_cast<ImGuiContext *>(ui->getContext());
                PPR_TEST_ASSERT(imgui_context != nullptr);
                ImGui::SetCurrentContext(imgui_context);
                PPR_TEST_ASSERT(ImGui::Begin("reset_probe"));
                ImGui::SetKeyboardFocusHere();
                char probe_text[64]{};
                std::ignore = ImGui::InputText("##probe", probe_text, sizeof(probe_text));
                ImGui::End();
                ImGui::Render();
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(ImGui::GetIO().WantCaptureKeyboard);
                ImGui::Render();

                const InputMessage w_press{
                    InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, w_press) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(move_count == 0);

                PPR_TEST_ASSERT(not ui->shutdown());
                PPR_TEST_ASSERT(not routed.shutdown());

                // Second life, no update yet: reset flags route to the player.
                PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
                PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, w_press) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(move_count == 1);

                PPR_TEST_ASSERT(not ui->shutdown());
                PPR_TEST_ASSERT(not ui->shutdown());
                std::ignore = ui.release();
                PPR_TEST_ASSERT(not routed.shutdown());
            }
            std::ignore = window.release();
        };
    }


    namespace AppLifecycle {
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

            [[nodiscard]] std::error_code boot() {
                return initialize();
            }
            [[nodiscard]] std::error_code teardown() {
                return shutdown();
            }
        };

        struct WindowCreateFailApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

        protected:
            [[nodiscard]] std::error_code initialize() override {
                if (const auto ec = Application::initialize()) {
                    return ec;
                }
                return std::make_error_code(std::errc::no_such_device_or_address);
            }
        };

        struct InputConnectFailApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

        protected:
            [[nodiscard]] std::error_code initialize() override {
                if (const auto ec = Application::initialize()) {
                    return ec;
                }
                return std::make_error_code(std::errc::not_connected);
            }
        };

        struct CountingApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

            int m_shutdowns{0};

        protected:
            [[nodiscard]] std::error_code shutdown() override {
                ++m_shutdowns;
                return Application::shutdown();
            }
        };

        struct PartialFailApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

            bool m_fail_once{true};

        protected:
            [[nodiscard]] std::error_code shutdown() override {
                const auto ec = Application::shutdown();
                if (m_fail_once) {
                    m_fail_once = false;
                    if (not ec) {
                        return std::make_error_code(std::errc::io_error);
                    }
                }
                return ec;
            }
        };

        struct ExitAfterFramesApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

            int m_frames{0};
            int m_exit_after{3};
            std::error_code m_clause{};

        protected:
            [[nodiscard]] std::error_code update([[maybe_unused]] const TimeSpan dt) override {
                if (++m_frames >= m_exit_after) {
                    requestExit(m_clause);
                }
                return std::error_code{};
            }
        };

        struct ThrowingExitMessageCategory final : std::error_category {
            const char *name() const noexcept override {
                return "throwing-exit-message";
            }

            std::string message(int) const override {
                throw std::bad_alloc{};
            }
        };

        const ThrowingExitMessageCategory kThrowingExitMessageCategory{};

        struct MockGraphicsService : IService {
            int m_tag{7};
        };
    }

    PPR_UNIT_TEST (renderer_triangle_reinit_ok) {
        // Private device: standalone registration outside the window_input shared scope; full rebuild kept.
        const auto shader = IShaderService::get();
        const auto rhi = IRhiService::get();
        std::ignore = rhi->shutdown();
        std::ignore = shader->shutdown();
        PPR_DEFER {
            std::ignore = rhi->shutdown();
            std::ignore = shader->shutdown();
        };
        PPR_TEST_ASSERT(not shader->initialize());
        PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));
        {
            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_TEST_ASSERT(not renderer.shutdown());
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_TEST_ASSERT(not renderer.shutdown());
        }
        {
            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.shutdown());
            PPR_TEST_ASSERT(not pass.shutdown());
        }
    };

    PPR_UNIT_TEST (renderer_off_thread_fails_closed) {
        // Private device: standalone registration outside the window_input shared scope; full rebuild kept.
        const auto shader = IShaderService::get();
        const auto rhi = IRhiService::get();
        std::ignore = rhi->shutdown();
        std::ignore = shader->shutdown();
        PPR_DEFER {
            std::ignore = rhi->shutdown();
            std::ignore = shader->shutdown();
        };
        PPR_TEST_ASSERT(not shader->initialize());
        PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));

        Window window{
            WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
        window.m_framebuffer_size = int2{800, 600};

        rhi::TextureDesc target_desc{};
        target_desc.type = rhi::TextureType::Texture2D;
        target_desc.size = {64u, 64u, 1u};
        target_desc.arrayLength = 1u;
        target_desc.mipCount = 1u;
        target_desc.format = rhi::Format::RGBA8Unorm;
        target_desc.memoryType = rhi::MemoryType::DeviceLocal;
        target_desc.usage = rhi::TextureUsage::RenderTarget;
        target_desc.defaultState = rhi::ResourceState::RenderTarget;
        rhi::ComPtr<rhi::ITexture> target{};
        PPR_TEST_ASSERT(not make_error_code(rhi->getDevice().createTexture(target_desc, nullptr, target.writeRef())));

        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*rhi));

        const std::error_code kNotPermitted = std::make_error_code(std::errc::operation_not_permitted);
        std::error_code render_ec{};
        std::error_code to_texture_ec{};
        std::error_code wait_ec{};
        std::error_code present_ec{};
        std::error_code destroy_ec{};
        std::error_code shutdown_ec{};
        std::jthread worker([&](std::stop_token) {
            render_ec = renderer.render(string_literal{std::in_place, "off-thread render"}, rhi::RenderPassDesc{}, {});
            to_texture_ec = renderer.renderToTexture(*target, {});
            wait_ec = renderer.waitOnHost();
            present_ec = renderer.renderAndPresent(window, {});
            destroy_ec = renderer.destroyWindowSurface(window);
            shutdown_ec = renderer.shutdown();
        });
        worker.join();
        PPR_TEST_ASSERT(render_ec == kNotPermitted);
        PPR_TEST_ASSERT(to_texture_ec == kNotPermitted);
        PPR_TEST_ASSERT(wait_ec == kNotPermitted);
        PPR_TEST_ASSERT(present_ec == kNotPermitted);
        PPR_TEST_ASSERT(destroy_ec == kNotPermitted);
        PPR_TEST_ASSERT(shutdown_ec == kNotPermitted);
        PPR_TEST_ASSERT(not renderer.shutdown());

        // Uninitialized renderer never captures an owner, so off-thread
        // shutdown succeeds instead of locking itself out.
        Renderer fresh{};
        std::error_code fresh_ec{std::make_error_code(std::errc::state_not_recoverable)};
        std::jthread drifter([&](std::stop_token) { fresh_ec = fresh.shutdown(); });
        drifter.join();
        PPR_TEST_ASSERT(not fresh_ec);

        std::ignore = window.release();
    };

    PPR_UNIT_TEST (imgui_live_shutdown_idempotent) {
        // Private device: standalone registration outside the window_input shared scope; full rebuild kept.
        const auto shader = IShaderService::get();
        const auto rhi = IRhiService::get();
        std::ignore = rhi->shutdown();
        std::ignore = shader->shutdown();
        PPR_DEFER {
            std::ignore = rhi->shutdown();
            std::ignore = shader->shutdown();
        };
        PPR_TEST_ASSERT(not shader->initialize());
        PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));

        WindowInput::FakeInputService inputs{};
        Window window{
            WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
        window.m_framebuffer_size = int2{800, 600};
        {
            WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
            PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
            auto ui = ui::createImGuiService();
            PPR_TEST_ASSERT(ui != nullptr);
            PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));
            PPR_TEST_ASSERT(ui->getContext() != nullptr);
            PPR_TEST_ASSERT(not ui->shutdown());
            PPR_TEST_ASSERT(not ui->shutdown());
            // ImGuiService::~ImGuiService ENSUREs a live context, so a shut-down
            // instance cannot be destroyed while assert hooks are installed; the
            // emptied shell is released here. Dtor ownership stays engine-side.
            std::ignore = ui.release();
            PPR_TEST_ASSERT(not routed.shutdown());
        }
        std::ignore = window.release();
    };

    PPR_UNIT_TEST (imgui_live_same_object_reinit) {
        // Private device: standalone registration outside the window_input shared scope; full rebuild kept.
        const auto shader = IShaderService::get();
        const auto rhi = IRhiService::get();
        std::ignore = rhi->shutdown();
        std::ignore = shader->shutdown();
        PPR_DEFER {
            std::ignore = rhi->shutdown();
            std::ignore = shader->shutdown();
        };
        PPR_TEST_ASSERT(not shader->initialize());
        PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));

        WindowInput::FakeInputService inputs{};
        Window window{
            WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
        window.m_framebuffer_size = int2{800, 600};
        {
            WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
            PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
            auto ui = ui::createImGuiService();
            PPR_TEST_ASSERT(ui != nullptr);

            rhi::TextureDesc target_desc{};
            target_desc.type = rhi::TextureType::Texture2D;
            target_desc.size = {64u, 64u, 1u};
            target_desc.arrayLength = 1u;
            target_desc.mipCount = 1u;
            target_desc.format = rhi::Format::RGBA8Unorm;
            target_desc.memoryType = rhi::MemoryType::DeviceLocal;
            target_desc.usage = rhi::TextureUsage::RenderTarget;
            target_desc.defaultState = rhi::ResourceState::RenderTarget;
            rhi::ComPtr<rhi::ITexture> target{};
            PPR_TEST_ASSERT(
                not make_error_code(rhi->getDevice().createTexture(target_desc, nullptr, target.writeRef())));

            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
            WindowInput::ImGuiDrawable drawable{*ui};

            PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));
            PPR_TEST_ASSERT(not ui->update(TimeSpan{}, viewport));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {drawable}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not ui->shutdown());

            PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));
            PPR_TEST_ASSERT(not ui->update(TimeSpan{}, viewport));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {drawable}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not ui->shutdown());
            PPR_TEST_ASSERT(not renderer.shutdown());
            PPR_TEST_ASSERT(not routed.shutdown());
        }
        std::ignore = window.release();
    };

    PPR_UNIT_TEST (app_init_rollback_on_window_create_failure) {
        AppLifecycle::WindowCreateFailApp test_app{"WindowCreateRollback"};
        PPR_TEST_ASSERT(test_app.boot() == std::make_error_code(std::errc::no_such_device_or_address));
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.getServices().tryGet<IWindowService>().isValid());
    };

    PPR_UNIT_TEST (app_init_rollback_on_input_connect_failure) {
        AppLifecycle::InputConnectFailApp test_app{"InputConnectRollback"};
        PPR_TEST_ASSERT(test_app.boot() == std::make_error_code(std::errc::not_connected));
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.getServices().tryGet<IInputService>().isValid());
    };

    PPR_UNIT_TEST (app_teardown_double_shutdown_no_refire) {
        AppLifecycle::CountingApp test_app{"DoubleShutdown"};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(test_app.m_shutdowns == 2);
    };

    PPR_UNIT_TEST (app_teardown_reports_subsystem_failure_but_completes) {
        AppLifecycle::PartialFailApp test_app{"PartialTeardown"};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_TEST_ASSERT(test_app.teardown() == std::make_error_code(std::errc::io_error));
        PPR_TEST_ASSERT(not test_app.teardown());
    };

    PPR_UNIT_TEST (app_preserves_pre_registered_graphics_services) {
        AppLifecycle::HeadlessApp test_app{"PreRegisteredGraphics"};
        AppLifecycle::MockGraphicsService graphics{};
        PPR_TEST_ASSERT(test_app.getServices().insert(safe_ptr<AppLifecycle::MockGraphicsService>{&graphics}));
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_DEFER {
            PPR_TEST_ASSERT(not test_app.teardown());
        };
        const auto resolved = test_app.getServices().tryGet<AppLifecycle::MockGraphicsService>();
        PPR_TEST_ASSERT(resolved.isValid());
        PPR_TEST_ASSERT(resolved.get() == &graphics);
        // Teardown inverse-of-setup: detach the pre-registered service before
        // scope exit so no safe_ptr outlives its stack owner.
        PPR_TEST_ASSERT(test_app.getServices().erase(graphics));
    };

    PPR_UNIT_TEST (app_empty_request_exit_signals_exit_without_error) {
        // Empty requestExit is graceful exit, not cancellation: the run loop
        // observes termination while the lifecycle records no error and fires
        // no event. Fails on the A2-broken tree (empty cancel is a no-op, so
        // no termination is observable); single-threaded, no timing.
        AppLifecycle::HeadlessApp test_app{"EmptyExitSignal"};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_DEFER {
            PPR_TEST_ASSERT(not test_app.teardown());
        };
        PPR_TEST_ASSERT(not test_app.isExitRequested());
        test_app.requestExit();
        PPR_TEST_ASSERT(test_app.isExitRequested());
        PPR_TEST_ASSERT(not test_app.getLifecycle()->error());
        PPR_TEST_ASSERT(not test_app.getLifecycle()->pollEvent());
    };

    PPR_UNIT_TEST (app_error_request_exit_latches_cause) {
        // Nonzero requestExit keeps latching the cause and emitting as
        // before: the loop observes termination AND run() reports the cause.
        const std::error_code expected = std::make_error_code(std::errc::operation_canceled);
        AppLifecycle::HeadlessApp test_app{"ErrorExitSignal"};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_DEFER {
            PPR_TEST_ASSERT(not test_app.teardown());
        };
        test_app.requestExit(expected);
        PPR_TEST_ASSERT(test_app.isExitRequested());
        PPR_TEST_ASSERT(test_app.getLifecycle()->error() == expected);
        PPR_TEST_ASSERT(test_app.getLifecycle()->pollEvent());
    };

    PPR_UNIT_TEST (app_empty_request_exit_run_returns_success) {
        // End-to-end through run(): the frame-3 empty exit terminates the
        // loop after exactly 3 frames and run() returns success. run() owns
        // cleanup via its shutdown defer, so no teardown follows. Pre-fix
        // this spins forever — the hang IS the bug; the signal test above is
        // the clean failing-then-passing proof.
        AppLifecycle::ExitAfterFramesApp test_app{"EmptyExitRun"};
        // A zero-valued custom category is still graceful, but message construction throws when logging is enabled.
        test_app.m_clause = std::error_code{0, AppLifecycle::kThrowingExitMessageCategory};
        PPR_TEST_ASSERT(not test_app.run());
        PPR_TEST_ASSERT(test_app.m_frames == 3);
        // The latch survives shutdown; only initialize() re-arms it, so a
        // repeated run() starts unrequested.
        PPR_TEST_ASSERT(test_app.isExitRequested());
    };

    PPR_UNIT_TEST (app_error_request_exit_run_reports_cause) {
        // End-to-end through run(): the frame-3 error exit terminates the
        // loop and run() reports the latched cause.
        const std::error_code expected = std::make_error_code(std::errc::operation_canceled);
        AppLifecycle::ExitAfterFramesApp test_app{"ErrorExitRun"};
        test_app.m_clause = expected;
        PPR_TEST_ASSERT(test_app.run() == expected);
        PPR_TEST_ASSERT(test_app.m_frames == 3);

        AppLifecycle::ExitAfterFramesApp throwing_app{"ThrowingErrorExitRun"};
        const std::error_code throwing_cause{42, AppLifecycle::kThrowingExitMessageCategory};
        throwing_app.m_clause = throwing_cause;
        PPR_TEST_ASSERT(throwing_app.run() == throwing_cause);
        PPR_TEST_ASSERT(throwing_app.m_frames == 3);
        // No post-run isExitRequested() assert: error exits latch through
        // the lifecycle object, which shutdown() resets — the latch only
        // observes a live run, and run()'s return already proves the cause.
    };

    PPR_UNIT_TEST (telemetry_unwritable_output_preserves_run_result, UnitTest::fork) {
        // Isolate process-wide stdout and logger policy from all other leaves.
        PPR_TEST_ASSERT(not Log::flush(true));
        const auto silent_writer = [](const Log::Entry &) noexcept {};
        auto policy = Log::setWriterPolicy(silent_writer);
        PPR_DEFER {
            std::ignore = Log::setWriterPolicy(std::move(policy));
        };
        std::ignore = std::fflush(stdout);
#ifdef _WIN32
        const int output = ::_fileno(stdout);
        const int saved = ::_dup(output);
        std::FILE *read_only_file = nullptr;
        const int open_error = ::fopen_s(&read_only_file, "NUL", "r");
        const int read_only = open_error == 0 and read_only_file != nullptr ? ::_fileno(read_only_file) : -1;
        auto duplicate = [](const int from, const int to) { return ::_dup2(from, to); };
        auto close = [](const int descriptor) { return ::_close(descriptor); };
#else
        const int output = ::fileno(stdout);
        const int saved = ::dup(output);
        const int read_only = ::open("/dev/null", O_RDONLY);
        auto duplicate = [](const int from, const int to) { return ::dup2(from, to); };
        auto close = [](const int descriptor) { return ::close(descriptor); };
#endif
        PPR_DEFER {
            if (saved >= 0) {
                std::ignore = std::fflush(stdout);
                std::ignore = duplicate(saved, output);
                std::ignore = close(saved);
                std::clearerr(stdout);
            }
#ifdef _WIN32
            if (read_only_file != nullptr) {
                std::fclose(read_only_file);
            }
#else
            if (read_only >= 0) {
                std::ignore = close(read_only);
            }
#endif
        };
        PPR_TEST_ASSERT(saved >= 0 and read_only >= 0);
        PPR_TEST_ASSERT(duplicate(read_only, output) >= 0);

        const std::error_code cause = std::make_error_code(std::errc::io_error);
        AppLifecycle::ExitAfterFramesApp graceful{"UnwritableTelemetryGraceful"};
        const std::error_code graceful_result = graceful.run();
        AppLifecycle::ExitAfterFramesApp failed{"UnwritableTelemetryFailure"};
        failed.m_clause = cause;
        const std::error_code failed_result = failed.run();
        const bool output_failed = std::fflush(stdout) == EOF or std::ferror(stdout) != 0;

        PPR_TEST_ASSERT(duplicate(saved, output) >= 0);
        std::clearerr(stdout);
        PPR_TEST_ASSERT(output_failed);
        PPR_TEST_ASSERT(not graceful_result);
        PPR_TEST_ASSERT(failed_result == cause);
        PPR_TEST_ASSERT(frameTelemetryMainPending() == 0u);
    };
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest window_input = UnitTest::Named("window_input") / [](UnitTest::IRun &_) -> void {
        // CPU-only leaves hold no SharedWindowInputGpu refs and never boot a device.
        _.recurse({
            detail::WindowInput::mouse_keeps_client_space_with_nonzero_window_origin,
            detail::WindowInput::char_posted_after_poll_clear_survives_to_consumer,
            detail::WindowInput::reset_clears_pending_characters,
            detail::WindowInput::drag_latch_initial_state_disengaged,
            detail::WindowInput::drag_latch_background_press_engages_left,
            detail::WindowInput::drag_latch_middle_button_engages,
            detail::WindowInput::drag_latch_arrival_always_engages_no_origin_query,
            detail::WindowInput::drag_latch_right_and_thumb_ignored,
            detail::WindowInput::drag_latch_rmb_independent_of_latch,
            detail::WindowInput::drag_latch_overlap_survives_single_release,
            detail::WindowInput::drag_latch_release_without_press_resets,
            detail::WindowInput::drag_latch_reset_restores_disengaged,
            detail::WindowInput::drag_latch_tap_state_reset_clears_stale_hold,
            detail::telemetry_unwritable_output_preserves_run_result,
        });
        // Shared-device scope: one boot serves every GPU leaf below (each
        // observes it through resetBetweenLeaves plus the initialize no-op
        // gate); the release tears it down after the last leaf, so no
        // leaf-local safe_ptr outlives the device.
        PPR_TEST_ASSERT(not detail::SharedWindowInputGpu::acquire());
        _.recurse({
            detail::WindowInput::imgui_nav_signal_evidence,
            detail::WindowInput::imgui_background_click_returns_keys_to_player,
            detail::WindowInput::imgui_gamepad_follows_nav_active,
            detail::WindowInput::imgui_wheel_hover_split,
            detail::WindowInput::imgui_consume_flags_reset_on_shutdown,
        });
        PPR_TEST_ASSERT(not detail::SharedWindowInputGpu::release());
    };

    const UnitTest &window_inputTests() noexcept {
        return window_input;
    }
} // namespace pP::tests

namespace pP::tests {
    const UnitTest renderer_triangle_reinit_ok = detail::renderer_triangle_reinit_ok;

    const UnitTest &renderer_triangle_reinit_okTests() noexcept {
        return renderer_triangle_reinit_ok;
    }

    const UnitTest renderer_off_thread_fails_closed = detail::renderer_off_thread_fails_closed;

    const UnitTest &renderer_off_thread_fails_closedTests() noexcept {
        return renderer_off_thread_fails_closed;
    }

    const UnitTest imgui_live_shutdown_idempotent = detail::imgui_live_shutdown_idempotent;

    const UnitTest &imgui_live_shutdown_idempotentTests() noexcept {
        return imgui_live_shutdown_idempotent;
    }

    const UnitTest imgui_live_same_object_reinit = detail::imgui_live_same_object_reinit;

    const UnitTest &imgui_live_same_object_reinitTests() noexcept {
        return imgui_live_same_object_reinit;
    }

    const UnitTest app_init_rollback_on_window_create_failure = detail::app_init_rollback_on_window_create_failure;

    const UnitTest &app_init_rollback_on_window_create_failureTests() noexcept {
        return app_init_rollback_on_window_create_failure;
    }

    const UnitTest app_init_rollback_on_input_connect_failure = detail::app_init_rollback_on_input_connect_failure;

    const UnitTest &app_init_rollback_on_input_connect_failureTests() noexcept {
        return app_init_rollback_on_input_connect_failure;
    }

    const UnitTest app_teardown_double_shutdown_no_refire = detail::app_teardown_double_shutdown_no_refire;

    const UnitTest &app_teardown_double_shutdown_no_refireTests() noexcept {
        return app_teardown_double_shutdown_no_refire;
    }

    const UnitTest app_teardown_reports_subsystem_failure_but_completes =
        detail::app_teardown_reports_subsystem_failure_but_completes;

    const UnitTest &app_teardown_reports_subsystem_failure_but_completesTests() noexcept {
        return app_teardown_reports_subsystem_failure_but_completes;
    }

    const UnitTest app_preserves_pre_registered_graphics_services =
        detail::app_preserves_pre_registered_graphics_services;

    const UnitTest &app_preserves_pre_registered_graphics_servicesTests() noexcept {
        return app_preserves_pre_registered_graphics_services;
    }

    const UnitTest app_empty_request_exit_signals_exit_without_error =
        detail::app_empty_request_exit_signals_exit_without_error;

    const UnitTest &app_empty_request_exit_signals_exit_without_errorTests() noexcept {
        return app_empty_request_exit_signals_exit_without_error;
    }

    const UnitTest app_error_request_exit_latches_cause = detail::app_error_request_exit_latches_cause;

    const UnitTest &app_error_request_exit_latches_causeTests() noexcept {
        return app_error_request_exit_latches_cause;
    }

    const UnitTest app_empty_request_exit_run_returns_success = detail::app_empty_request_exit_run_returns_success;

    const UnitTest &app_empty_request_exit_run_returns_successTests() noexcept {
        return app_empty_request_exit_run_returns_success;
    }

    const UnitTest app_error_request_exit_run_reports_cause = detail::app_error_request_exit_run_reports_cause;

    const UnitTest &app_error_request_exit_run_reports_causeTests() noexcept {
        return app_error_request_exit_run_reports_cause;
    }
} // namespace pP::tests
