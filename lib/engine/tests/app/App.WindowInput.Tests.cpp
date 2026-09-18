module;
#include "pP/UnitTest.h"

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
                return std::ranges::any_of(m_contexts, [&](const SharedInputContext &stored) noexcept {
                    return stored.get() == &context;
                });
            }

            void addInputContext(SharedInputContext context) override {
                m_contexts.push_back(std::move(context));
            }

            bool removeInputContext(const InputContext &context) override {
                const auto it = std::ranges::find_if(m_contexts, [&](const SharedInputContext &stored) noexcept {
                    return stored.get() == &context;
                });
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

            void postKeyboardKeyPressed(const InputContext &context, const EKeyboardKey key, const bool pressed) override {
                m_keyboard.postKeyboardKeyPressed(TimeSpan{}, context, key, pressed);
            }

            void postMouseButtonPressed(const InputContext &context, const EMouseButton button, const bool pressed) override {
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

        PPR_UNIT_TEST(mouse_keeps_client_space_with_nonzero_window_origin) {
            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{400, 300}, .m_window_size = int2{800, 600}}
            };
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

        PPR_UNIT_TEST(char_posted_after_poll_clear_survives_to_consumer) {
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

        PPR_UNIT_TEST(reset_clears_pending_characters) {
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
                m_foreground.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
                    return EInputMessageResponse::unhandled;
                });
                m_background.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
                    return EInputMessageResponse::unhandled;
                });
            }

            ~TapHarness() {
                if (m_latch) {
                    std::ignore = m_latch->shutdown(m_context);
                    std::ignore = m_context.removeInputListener(m_foreground);
                }
            }

            [[nodiscard]] std::error_code attach() {
                m_context.addInputListener(
                    safe_ptr<InputListener>{&m_foreground},
                    static_cast<int>(EInputListenerPriority::ui));
                m_latch = std::make_unique<InputBackgroundLatch>(
                    static_cast<int>(EInputListenerPriority::ui),
                    safe_ptr<InputListener>{&m_background},
                    static_cast<int>(EInputListenerPriority::player),
                    static_cast<int>(EInputListenerPriority::detector));
                return m_latch->initialize(m_context, safe_ptr<const InputListener>{&m_foreground});
            }

            void post(const EMouseButton button, const EInputMessageEvent event) {
                const std::optional<InputKey> key = InputKey::from(button);
                PPR_TEST_ASSERT(key.has_value());
                const InputMessage message{
                    *key,
                    InputValue{InputDigital{event == EInputMessageEvent::pressed or event == EInputMessageEvent::repeat}},
                    InputDeviceID{0u},
                    event
                };
                std::ignore = m_context.postKeyEvent(TimeSpan{}, message);
            }
        };

        PPR_UNIT_TEST(drag_latch_initial_state_disengaged) {
            InputListener foreground{};
            foreground.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
                return EInputMessageResponse::unhandled;
            });
            InputListener background{};
            background.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
                return EInputMessageResponse::unhandled;
            });
            // No default ctor by design: priorities + background owner are
            // required up front. Pre-attach the latch is disengaged.
            const InputBackgroundLatch latch(
                static_cast<int>(EInputListenerPriority::ui),
                safe_ptr<InputListener>{&background},
                static_cast<int>(EInputListenerPriority::player),
                static_cast<int>(EInputListenerPriority::detector));
            PPR_TEST_ASSERT(not latch.isEngaged());
        };

        PPR_UNIT_TEST(drag_latch_background_press_engages_left) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::left, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        PPR_UNIT_TEST(drag_latch_middle_button_engages) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::middle, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::middle, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        // Arrival-equals-background: the detector observes only presses the ui
        // tier left unhandled, so every observed press engages with no origin query.
        PPR_UNIT_TEST(drag_latch_arrival_always_engages_no_origin_query) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
            tap.post(EMouseButton::left, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
        };

        PPR_UNIT_TEST(drag_latch_right_and_thumb_ignored) {
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

        PPR_UNIT_TEST(drag_latch_rmb_independent_of_latch) {
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

        PPR_UNIT_TEST(drag_latch_overlap_survives_single_release) {
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

        PPR_UNIT_TEST(drag_latch_release_without_press_resets) {
            TapHarness tap{};
            PPR_TEST_ASSERT(not tap.attach());
            tap.post(EMouseButton::left, EInputMessageEvent::released);
            PPR_TEST_ASSERT(not tap.m_latch->isEngaged());
            tap.post(EMouseButton::left, EInputMessageEvent::pressed);
            PPR_TEST_ASSERT(tap.m_latch->isEngaged());
        };

        PPR_UNIT_TEST(drag_latch_reset_restores_disengaged) {
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
        PPR_UNIT_TEST(drag_latch_tap_state_reset_clears_stale_hold) {
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
        PPR_UNIT_TEST(imgui_nav_signal_evidence) {
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

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}
            };
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
                    std::println("[nav-evidence] text: capturekb=1 navactive=1 activeid=0 (focus primes nav; typing activates)");
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
                std::println("[nav-evidence] plain-focus: capturekb={} navactive={} activeid={} (recorded)",
                    plain_io.WantCaptureKeyboard ? 1 : 0,
                    plain_io.NavActive ? 1 : 0,
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
        PPR_UNIT_TEST(imgui_background_click_returns_keys_to_player) {
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

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}
            };
            window.m_framebuffer_size = int2{800, 600};

            InputAction player_move{"PlayerMove", EInputValueType::digital};
            InputMapping player_mapping{"Player"};
            player_mapping.mapInputKey(SharedInputAction{&player_move}, InputKey::w);
            InputListener player_listener{};
            player_listener.addInputMapping(SharedInputMapping{&player_mapping}, 0);
            int move_count = 0;
            player_move.setTriggered([&move_count](const InputActionEvent &, const InputKey &) noexcept {
                ++move_count;
            });
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
                    InputKey::w,
                    InputValue{InputDigital{true}},
                    InputDeviceID{0u},
                    EInputMessageEvent::pressed
                };

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
                    InputKey::mouse_2d,
                    InputValue{InputAxis2D{float2{750.0f, 550.0f}, float2{0.0f, 0.0f}}},
                    InputDeviceID{0u},
                    EInputMessageEvent::axis
                };
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, void_move) == EInputMessageResponse::handled);
                const InputMessage void_click{
                    InputKey::left_mouse_button,
                    InputValue{InputDigital{true}},
                    InputDeviceID{0u},
                    EInputMessageEvent::pressed
                };
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, void_click) == EInputMessageResponse::handled);
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
        PPR_UNIT_TEST(imgui_gamepad_follows_nav_active) {
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

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}
            };
            window.m_framebuffer_size = int2{800, 600};

            InputAction player_pad{"PlayerPad", EInputValueType::digital};
            InputMapping player_mapping{"Player"};
            player_mapping.mapInputKey(SharedInputAction{&player_pad}, InputKey::gamepad_face_button_bottom);
            InputListener player_listener{};
            player_listener.addInputMapping(SharedInputMapping{&player_mapping}, 0);
            int pad_count = 0;
            player_pad.setTriggered([&pad_count](const InputActionEvent &, const InputKey &) noexcept {
                ++pad_count;
            });
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
                    InputKey::gamepad_face_button_bottom,
                    InputValue{InputDigital{true}},
                    InputDeviceID{0u},
                    EInputMessageEvent::pressed
                };

                // Background (no nav): UI yields, player receives.
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, pad_press) == EInputMessageResponse::consumed);
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
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, pad_press) == EInputMessageResponse::consumed);
                PPR_TEST_ASSERT(pad_count == 1);
                ImGui::Render();

                // Nav-off passthrough: gamepad nav bit cleared, focused window
                // or not, the pad always reaches the player.
                ImGui::SetCurrentContext(imgui_context);
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
                PPR_TEST_ASSERT(not ui->update(frame, viewport));
                PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, pad_press) == EInputMessageResponse::consumed);
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
        PPR_UNIT_TEST(imgui_wheel_hover_split) {
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

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}
            };
            window.m_framebuffer_size = int2{800, 600};

            InputAction camera_fov{"CameraFov", EInputValueType::axis_1d};
            InputMapping camera_mapping{"Camera"};
            camera_mapping.mapInputKey(SharedInputAction{&camera_fov}, InputKey::mouse_wheel_axis_y);
            InputListener camera_listener{};
            camera_listener.addInputMapping(SharedInputMapping{&camera_mapping}, 0);
            int fov_count = 0;
            camera_fov.setTriggered([&fov_count](const InputActionEvent &, const InputKey &) noexcept {
                ++fov_count;
            });
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
                    InputKey::mouse_wheel_axis_y,
                    InputValue{InputAxis1D{0.0f, 1.0f}},
                    InputDeviceID{0u},
                    EInputMessageEvent::axis
                };

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
        PPR_UNIT_TEST(imgui_consume_flags_reset_on_shutdown) {
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

            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}
            };
            window.m_framebuffer_size = int2{800, 600};

            InputAction player_move{"PlayerMove", EInputValueType::digital};
            InputMapping player_mapping{"Player"};
            player_mapping.mapInputKey(SharedInputAction{&player_move}, InputKey::w);
            InputListener player_listener{};
            player_listener.addInputMapping(SharedInputMapping{&player_mapping}, 0);
            int move_count = 0;
            player_move.setTriggered([&move_count](const InputActionEvent &, const InputKey &) noexcept {
                ++move_count;
            });
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
                    InputKey::w,
                    InputValue{InputDigital{true}},
                    InputDeviceID{0u},
                    EInputMessageEvent::pressed
                };
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

            [[nodiscard]] std::error_code boot() { return initialize(); }
            [[nodiscard]] std::error_code teardown() { return shutdown(); }
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

        struct MockGraphicsService : IService {
            int m_tag{7};
        };
    }

    PPR_UNIT_TEST(renderer_triangle_reinit_ok) {
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

    PPR_UNIT_TEST(imgui_live_shutdown_idempotent) {
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
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}
        };
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

    PPR_UNIT_TEST(app_init_rollback_on_window_create_failure) {
        AppLifecycle::WindowCreateFailApp test_app{"WindowCreateRollback"};
        PPR_TEST_ASSERT(test_app.boot() == std::make_error_code(std::errc::no_such_device_or_address));
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.getServices().tryGet<IWindowService>().isValid());
    };

    PPR_UNIT_TEST(app_init_rollback_on_input_connect_failure) {
        AppLifecycle::InputConnectFailApp test_app{"InputConnectRollback"};
        PPR_TEST_ASSERT(test_app.boot() == std::make_error_code(std::errc::not_connected));
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.getServices().tryGet<IInputService>().isValid());
    };

    PPR_UNIT_TEST(app_teardown_double_shutdown_no_refire) {
        AppLifecycle::CountingApp test_app{"DoubleShutdown"};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(test_app.m_shutdowns == 2);
    };

    PPR_UNIT_TEST(app_teardown_reports_subsystem_failure_but_completes) {
        AppLifecycle::PartialFailApp test_app{"PartialTeardown"};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_TEST_ASSERT(test_app.teardown() == std::make_error_code(std::errc::io_error));
        PPR_TEST_ASSERT(not test_app.teardown());
    };

    PPR_UNIT_TEST(app_preserves_pre_registered_graphics_services) {
        AppLifecycle::HeadlessApp test_app{"PreRegisteredGraphics"};
        AppLifecycle::MockGraphicsService graphics{};
        PPR_TEST_ASSERT(test_app.getServices().insert(safe_ptr<AppLifecycle::MockGraphicsService>{&graphics}));
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_DEFER { PPR_TEST_ASSERT(not test_app.teardown()); };
        const auto resolved = test_app.getServices().tryGet<AppLifecycle::MockGraphicsService>();
        PPR_TEST_ASSERT(resolved.isValid());
        PPR_TEST_ASSERT(resolved.get() == &graphics);
        // Teardown inverse-of-setup: detach the pre-registered service before
        // scope exit so no safe_ptr outlives its stack owner.
        PPR_TEST_ASSERT(test_app.getServices().erase(graphics));
    };
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest window_input = UnitTest::Named("window_input") / [](UnitTest::IRun &_) -> void {
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
            detail::WindowInput::imgui_nav_signal_evidence,
            detail::WindowInput::imgui_background_click_returns_keys_to_player,
            detail::WindowInput::imgui_gamepad_follows_nav_active,
            detail::WindowInput::imgui_wheel_hover_split,
            detail::WindowInput::imgui_consume_flags_reset_on_shutdown,
        });
    };
} // namespace pP::tests

namespace pP::tests {
    extern const UnitTest renderer_triangle_reinit_ok = detail::renderer_triangle_reinit_ok;
    extern const UnitTest imgui_live_shutdown_idempotent = detail::imgui_live_shutdown_idempotent;
    extern const UnitTest app_init_rollback_on_window_create_failure = detail::app_init_rollback_on_window_create_failure;
    extern const UnitTest app_init_rollback_on_input_connect_failure = detail::app_init_rollback_on_input_connect_failure;
    extern const UnitTest app_teardown_double_shutdown_no_refire = detail::app_teardown_double_shutdown_no_refire;
    extern const UnitTest app_teardown_reports_subsystem_failure_but_completes = detail::app_teardown_reports_subsystem_failure_but_completes;
    extern const UnitTest app_preserves_pre_registered_graphics_services = detail::app_preserves_pre_registered_graphics_services;
} // namespace pP::tests
