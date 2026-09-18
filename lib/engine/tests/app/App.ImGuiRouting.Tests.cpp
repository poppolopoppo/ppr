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
    namespace ImGuiRouting {
        // Minimal IInputService for live-UIService leaves in this file: the
        // leaf drives keys via InputContext::postKeyEvent directly, so the
        // post* bodies only forward to member devices. Mirrors the proven
        // WindowInput::FakeInputService; file-local per test-group convention.
        struct StubInputService final : IInputService {
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
    }

    // Routing order: an ImGui-style listener (raw callback only, no mappings)
    // at -1000 on the window context still observes a scene-mapped key before
    // the scene listener at 0 consumes it. Mirrors Application init order
    // (ImGui registered first) without instantiating the RHI-backed service.
    PPR_UNIT_TEST(imgui_first_on_window_context_observes_scene_mapped_key) {
        // Listeners outlive the context: it holds safe_ptrs to them.
        InputListener imgui_listener{};
        int raw_count = 0;
        imgui_listener.setRawKeyCallback([&](const TimeSpan, const InputMessage &) noexcept {
            ++raw_count;
            return EInputMessageResponse::unhandled;
        });

        InputAction scene_action{"SceneMove", EInputValueType::digital};
        InputMapping scene_mapping{"Scene"};
        scene_mapping.mapInputKey(SharedInputAction{&scene_action}, InputKey::w);
        InputListener scene_listener{};
        scene_listener.addInputMapping(SharedInputMapping{&scene_mapping}, 0);
        int scene_triggered = 0;
        scene_action.setTriggered([&scene_triggered](const InputActionEvent &, const InputKey &) noexcept {
            ++scene_triggered;
        });

        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&imgui_listener}, -1000);
        window_context.addInputListener(safe_ptr<InputListener>{&scene_listener}, 0);

        const InputMessage msg{
            InputKey::w,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::pressed
        };
        const EInputMessageResponse response = window_context.postKeyEvent(TimeSpan{}, msg);

        PPR_TEST_ASSERT(raw_count == 1);
        PPR_TEST_ASSERT(scene_triggered == 1);
        PPR_TEST_ASSERT(response == EInputMessageResponse::consumed);
    };

    // Background-drag detector observes at detector(1) before player(2) with camera mapping and never consumes.
    PPR_UNIT_TEST(detector_observes_before_player_mapping_and_never_consumes) {
        // Contract values, int-identical with the pre-Phase-2 anonymous enum.
        PPR_TEST_ASSERT(static_cast<int>(EInputListenerPriority::ui) == 0);
        PPR_TEST_ASSERT(static_cast<int>(EInputListenerPriority::detector) == 1);
        PPR_TEST_ASSERT(static_cast<int>(EInputListenerPriority::player) == 2);
        PPR_TEST_ASSERT(static_cast<int>(EInputMappingPriority::camera) == 1);

        // The statics below must carry the look-button identities.
        const EMouseButton *left_code = std::get_if<EMouseButton>(&InputKey::left_mouse_button.m_code);
        PPR_TEST_ASSERT(left_code != nullptr);
        PPR_TEST_ASSERT(*left_code == EMouseButton::left);
        const EMouseButton *middle_code = std::get_if<EMouseButton>(&InputKey::middle_mouse_button.m_code);
        PPR_TEST_ASSERT(middle_code != nullptr);
        PPR_TEST_ASSERT(*middle_code == EMouseButton::middle);

        // Foreground stands in for the ui listener: present but uninterested,
        // so the press reaches the latch-owned detector. Mirrors production
        // (ApplicationEditor): foreground added manually at ui, background
        // player registered by the latch at player, detector owned and
        // registered internally at detector.
        InputListener foreground{};
        foreground.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });

        InputAction camera_rotate{"CameraRotate", EInputValueType::digital};
        InputMapping camera_mapping{"Camera"};
        camera_mapping.mapInputKey(SharedInputAction{&camera_rotate}, InputKey::left_mouse_button);
        InputListener background{};
        background.addInputMapping(SharedInputMapping{&camera_mapping}, static_cast<int>(EInputMappingPriority::camera));
        // The detector tier runs before the player mapping: arrival already latched.
        std::unique_ptr<InputBackgroundLatch> latch{};
        bool detector_ran_before_camera = false;
        int camera_triggered = 0;
        camera_rotate.setTriggered([&](const InputActionEvent &, const InputKey &) noexcept {
            detector_ran_before_camera = latch != nullptr and latch->isEngaged();
            ++camera_triggered;
        });

        // Listeners outlive the context: it holds safe_ptrs to them, and the
        // latch shuts down detector-first (production shutdown order).
        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::pressed
        };
        const EInputMessageResponse response = window_context.postKeyEvent(TimeSpan{}, left_press);

        PPR_TEST_ASSERT(detector_ran_before_camera);
        PPR_TEST_ASSERT(camera_triggered == 1);
        PPR_TEST_ASSERT(response == EInputMessageResponse::consumed);
        PPR_TEST_ASSERT(latch->isEngaged());

        const InputMessage left_release{
            InputKey::left_mouse_button,
            InputValue{InputDigital{false}},
            InputDeviceID{0u},
            EInputMessageEvent::released
        };
        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_release);
        PPR_TEST_ASSERT(not latch->isEngaged());

        PPR_TEST_ASSERT(not latch->shutdown(window_context));
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));
    };

    // Arrival-equals-background: a ui mapping with its consume flag set
    // (captured press, WantCaptureMouseUnlessPopupClose) returns consumed, so
    // the detector never observes the press and the actuator stays idle. With
    // the flag clear (background press) the ui returns handled, the detector
    // observes before the player mapping, and arrival alone proves the
    // external origin — the callback queries no UI state on the hot path.
    PPR_UNIT_TEST(captured_press_starves_actuator_background_press_reaches_actuator) {
        InputAction ui_click{"UiClick", EInputValueType::digital};
        InputMapping ui_mapping{"Ui"};
        ui_mapping.mapInputKey(SharedInputAction{&ui_click}, InputKey::left_mouse_button);
        InputListener foreground{};
        foreground.addInputMapping(SharedInputMapping{&ui_mapping}, static_cast<int>(EInputMappingPriority::camera));

        InputAction camera_rotate{"CameraRotate", EInputValueType::digital};
        InputMapping camera_mapping{"Camera"};
        camera_mapping.mapInputKey(SharedInputAction{&camera_rotate}, InputKey::left_mouse_button);
        InputListener background{};
        background.addInputMapping(SharedInputMapping{&camera_mapping}, static_cast<int>(EInputMappingPriority::camera));

        // Listeners outlive the context: it holds safe_ptrs to them. The
        // foreground (ui) is added manually; the latch registers its internal
        // detector plus the background player, and shuts them down
        // detector-first (production order).
        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::pressed
        };
        const InputMessage left_release{
            InputKey::left_mouse_button,
            InputValue{InputDigital{false}},
            InputDeviceID{0u},
            EInputMessageEvent::released
        };

        // Captured: ui consume flag set → consumed at ui, detector starved.
        ui_click.setConsumeInput(true);
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, left_press) == EInputMessageResponse::consumed);
        PPR_TEST_ASSERT(not latch->isEngaged());

        // Background: flag clear → handled at ui, detector observes, player consumes.
        ui_click.setConsumeInput(false);
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, left_press) == EInputMessageResponse::consumed);
        PPR_TEST_ASSERT(latch->isEngaged());

        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_release);
        PPR_TEST_ASSERT(not latch->isEngaged());

        PPR_TEST_ASSERT(not latch->shutdown(window_context));
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));
    };

    // Release-over-captured-UI edge: a background press engages the hold, but
    // a release consumed at ui never reaches the detector, so the hold goes
    // stale. The blur/disconnect backstop (resetInputState, cf.
    // ApplicationEditor::onMainWindowFocused_) clears it — the release alone
    // cannot.
    PPR_UNIT_TEST(release_consumed_by_ui_leaves_hold_until_reset_backstop) {
        InputAction ui_click{"UiClick", EInputValueType::digital};
        InputMapping ui_mapping{"Ui"};
        ui_mapping.mapInputKey(SharedInputAction{&ui_click}, InputKey::left_mouse_button);
        InputListener foreground{};
        foreground.addInputMapping(SharedInputMapping{&ui_mapping}, static_cast<int>(EInputMappingPriority::camera));

        InputListener background{};
        background.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });

        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::pressed
        };
        const InputMessage left_release{
            InputKey::left_mouse_button,
            InputValue{InputDigital{false}},
            InputDeviceID{0u},
            EInputMessageEvent::released
        };

        ui_click.setConsumeInput(false);
        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_press);
        PPR_TEST_ASSERT(latch->isEngaged());

        // Release captured at ui: consumed there, detector misses it, hold goes stale.
        ui_click.setConsumeInput(true);
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, left_release) == EInputMessageResponse::consumed);
        PPR_TEST_ASSERT(latch->isEngaged());

        // Blur/disconnect backstop clears the stale hold.
        latch->resetInputState();
        PPR_TEST_ASSERT(not latch->isEngaged());

        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_release);
        PPR_TEST_ASSERT(not latch->isEngaged());

        PPR_TEST_ASSERT(not latch->shutdown(window_context));
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));
    };

    // Shutdown detaches the detector before the player listener (cf.
    // ApplicationEditor::shutdown): after the detector detach the player
    // still receives keys; after full detach the context reports unhandled.
    PPR_UNIT_TEST(shutdown_detaches_detector_before_player) {
        int detector_seen = 0;
        InputListener detector{};
        detector.setRawKeyCallback([&](TimeSpan, const InputMessage &) noexcept {
            ++detector_seen;
            return EInputMessageResponse::unhandled;
        });

        InputAction scene_action{"SceneMove", EInputValueType::digital};
        InputMapping scene_mapping{"Scene"};
        scene_mapping.mapInputKey(SharedInputAction{&scene_action}, InputKey::w);
        InputListener player{};
        player.addInputMapping(SharedInputMapping{&scene_mapping}, static_cast<int>(EInputMappingPriority::camera));
        int scene_triggered = 0;
        scene_action.setTriggered([&scene_triggered](const InputActionEvent &, const InputKey &) noexcept {
            ++scene_triggered;
        });

        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&detector}, static_cast<int>(EInputListenerPriority::detector));
        window_context.addInputListener(safe_ptr<InputListener>{&player}, static_cast<int>(EInputListenerPriority::player));
        PPR_TEST_ASSERT(window_context.hasInputListener(detector));
        PPR_TEST_ASSERT(window_context.hasInputListener(player));

        const InputMessage move{
            InputKey::w,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::pressed
        };
        std::ignore = window_context.postKeyEvent(TimeSpan{}, move);
        PPR_TEST_ASSERT(detector_seen == 1);
        PPR_TEST_ASSERT(scene_triggered == 1);

        // Detector first, mirroring production teardown.
        PPR_TEST_ASSERT(window_context.removeInputListener(detector));
        PPR_TEST_ASSERT(not window_context.hasInputListener(detector));
        PPR_TEST_ASSERT(window_context.hasInputListener(player));
        std::ignore = window_context.postKeyEvent(TimeSpan{}, move);
        PPR_TEST_ASSERT(detector_seen == 1);
        PPR_TEST_ASSERT(scene_triggered == 2);

        PPR_TEST_ASSERT(window_context.removeInputListener(player));
        PPR_TEST_ASSERT(not window_context.hasInputListener(player));
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, move) == EInputMessageResponse::unhandled);
        PPR_TEST_ASSERT(detector_seen == 1);
        PPR_TEST_ASSERT(scene_triggered == 2);
    };

    // The background-drag predicate is the single shared gate for the ui
    // probe, the detector actuator, and the controller latch: left/middle only.
    PPR_UNIT_TEST(background_drag_predicate_covers_left_and_middle_only) {
        PPR_TEST_ASSERT(InputBackgroundLatch::isBackgroundDragButton(EMouseButton::left));
        PPR_TEST_ASSERT(InputBackgroundLatch::isBackgroundDragButton(EMouseButton::middle));
        PPR_TEST_ASSERT(not InputBackgroundLatch::isBackgroundDragButton(EMouseButton::right));
        PPR_TEST_ASSERT(not InputBackgroundLatch::isBackgroundDragButton(EMouseButton::thumb0));
        PPR_TEST_ASSERT(not InputBackgroundLatch::isBackgroundDragButton(EMouseButton::thumb1));
    };

    // State-path repeat dedupe: repeats must not inflate the routing-owned
    // latch, so pressed + N repeats + a single release fully clears it. Also
    // covers the latch teardown: shutdown unregisters detector-first and
    // drains the context to unhandled, but preserves counts (no auto-reset) —
    // the blur/disconnect backstop owns clearing.
    PPR_UNIT_TEST(state_path_repeat_hold_single_release_clears) {
        InputListener foreground{};
        foreground.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });
        InputListener background{};
        background.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });

        // Listeners outlive the context: it holds safe_ptrs to them, and the
        // latch shuts down detector-first (production shutdown order). The
        // detector itself is latch-owned, so its registration is observable
        // only through engagement, not through hasInputListener.
        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->validate());
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));
        PPR_TEST_ASSERT(window_context.hasInputListener(background));

        const InputMessage left_press{
            InputKey::left_mouse_button,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::pressed
        };
        const InputMessage left_repeat{
            InputKey::left_mouse_button,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::repeat
        };
        const InputMessage left_release{
            InputKey::left_mouse_button,
            InputValue{InputDigital{false}},
            InputDeviceID{0u},
            EInputMessageEvent::released
        };

        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_press);
        PPR_TEST_ASSERT(latch->isEngaged());

        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_repeat);
        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_repeat);
        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_repeat);
        // Repeats never inflate the hold: a single release still clears it.
        PPR_TEST_ASSERT(latch->isEngaged());

        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_release);
        PPR_TEST_ASSERT(not latch->isEngaged());

        // Mid-hold teardown: shutdown unregisters detector-first and drains
        // to unhandled, but preserves the hold — clearing stays with the
        // blur/disconnect backstop.
        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_press);
        PPR_TEST_ASSERT(latch->isEngaged());
        PPR_TEST_ASSERT(not latch->shutdown(window_context));
        PPR_TEST_ASSERT(not window_context.hasInputListener(background));
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, left_press) == EInputMessageResponse::unhandled);
        PPR_TEST_ASSERT(latch->isEngaged());
        latch->resetInputState();
        PPR_TEST_ASSERT(not latch->isEngaged());

        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, left_press) == EInputMessageResponse::unhandled);

        // Second shutdown reports not_connected: nothing left to detach.
        PPR_TEST_ASSERT(latch->shutdown(window_context) == std::make_error_code(std::errc::not_connected));
    };

    // Legacy co-located topology (pre-fix production): the detector shared the
    // player priority, so intra-tier order was address-defined (the
    // PriorityPair tiebreak compares listener addresses) and fragile. The
    // latch now owns the detector and validate() requires the strict
    // ui < detector < player ordering, so the co-located topology cannot be
    // constructed: this leaf locks in that rejection. Deterministic
    // detector-before-player order at detector(1) < player(2) stays covered
    // by the detector@1 leaves above; consumed-starves holds without any
    // ordering assumption via captured_press_starves above.
    PPR_UNIT_TEST(colocated_detector_priority_rejected_by_validate) {
        InputListener background{};
        background.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });

        // detector == player: legacy co-location, rejected.
        const InputBackgroundLatch colocated(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::player));
        PPR_TEST_ASSERT(colocated.validate() == std::make_error_code(std::errc::invalid_argument));

        // initialize() with that topology registers nothing and reports the
        // validation failure.
        InputListener foreground{};
        foreground.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });
        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto bad = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::player));
        PPR_TEST_ASSERT(bad->initialize(window_context, safe_ptr<const InputListener>{&foreground})
            == std::make_error_code(std::errc::invalid_argument));
        PPR_TEST_ASSERT(not window_context.hasInputListener(background));
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));
    };

    // validate() contract: background owner present and strict
    // ui < detector < player ordering. Anything else is invalid_argument.
    PPR_UNIT_TEST(latch_validate_rejects_null_background_and_bad_ordering) {
        constexpr int ui = static_cast<int>(EInputListenerPriority::ui);
        constexpr int detector = static_cast<int>(EInputListenerPriority::detector);
        constexpr int player = static_cast<int>(EInputListenerPriority::player);

        InputListener background{};
        background.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });

        const InputBackgroundLatch null_background{ui, safe_ptr<InputListener>{}, player, detector};
        PPR_TEST_ASSERT(null_background.validate() == std::make_error_code(std::errc::invalid_argument));

        const InputBackgroundLatch foreground_at_detector{detector, safe_ptr<InputListener>{&background}, player, detector};
        PPR_TEST_ASSERT(foreground_at_detector.validate() == std::make_error_code(std::errc::invalid_argument));

        const InputBackgroundLatch detector_at_background{ui, safe_ptr<InputListener>{&background}, detector, detector};
        PPR_TEST_ASSERT(detector_at_background.validate() == std::make_error_code(std::errc::invalid_argument));

        const InputBackgroundLatch valid{ui, safe_ptr<InputListener>{&background}, player, detector};
        PPR_TEST_ASSERT(not valid.validate());
    };

    // initialize() contract: null foreground rejected, double-connect
    // rejected, bad-validate topology registers nothing, and a good topology
    // registers the background (detector registration is latch-internal, so
    // observable only through engagement).
    PPR_UNIT_TEST(latch_initialize_rejects_null_double_and_bad_validate) {
        InputListener foreground{};
        foreground.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });
        InputListener background{};
        background.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });

        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(latch->initialize(window_context, safe_ptr<const InputListener>{})
            == std::make_error_code(std::errc::invalid_argument));
        PPR_TEST_ASSERT(not window_context.hasInputListener(background));

        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));
        PPR_TEST_ASSERT(window_context.hasInputListener(background));
        PPR_TEST_ASSERT(latch->initialize(window_context, safe_ptr<const InputListener>{&foreground})
            == std::make_error_code(std::errc::already_connected));

        PPR_TEST_ASSERT(not latch->shutdown(window_context));
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));

        auto bad = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::player));
        PPR_TEST_ASSERT(bad->initialize(window_context, safe_ptr<const InputListener>{&foreground})
            == std::make_error_code(std::errc::invalid_argument));
        PPR_TEST_ASSERT(not window_context.hasInputListener(background));
    };

    // shutdown() contract: missing parts aggregate not_connected (a fresh
    // latch, and a second shutdown after a full one), and a full shutdown
    // drains the context to unhandled. Detector-first removal mirrors
    // production teardown; engagement retention is covered by the state_path
    // leaf above.
    PPR_UNIT_TEST(latch_shutdown_reports_not_connected_and_drains_to_unhandled) {
        InputListener foreground{};
        foreground.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });
        InputListener background{};
        background.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });

        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::pressed
        };
        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_press);
        PPR_TEST_ASSERT(latch->isEngaged());

        PPR_TEST_ASSERT(not latch->shutdown(window_context));
        PPR_TEST_ASSERT(not window_context.hasInputListener(background));
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, left_press) == EInputMessageResponse::unhandled);
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, left_press) == EInputMessageResponse::unhandled);
        PPR_TEST_ASSERT(latch->shutdown(window_context) == std::make_error_code(std::errc::not_connected));

        auto fresh = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(fresh->shutdown(window_context) == std::make_error_code(std::errc::not_connected));
    };

    // notifyPress/notifyRelease contract (direct, latch-internal counting):
    // release-without-press is a no-op, same-button overlap needs one release
    // per press, buttons track independently, non-drag buttons never engage.
    PPR_UNIT_TEST(latch_notify_press_release_tracks_per_button_overlap) {
        InputListener background{};
        InputBackgroundLatch latch(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch.isEngaged());

        latch.notifyRelease(EMouseButton::left);
        PPR_TEST_ASSERT(not latch.isEngaged());

        latch.notifyPress(EMouseButton::left);
        PPR_TEST_ASSERT(latch.isEngaged());
        latch.notifyPress(EMouseButton::left);
        latch.notifyRelease(EMouseButton::left);
        PPR_TEST_ASSERT(latch.isEngaged());
        latch.notifyRelease(EMouseButton::left);
        PPR_TEST_ASSERT(not latch.isEngaged());

        latch.notifyPress(EMouseButton::left);
        latch.notifyPress(EMouseButton::middle);
        latch.notifyRelease(EMouseButton::left);
        PPR_TEST_ASSERT(latch.isEngaged());
        latch.notifyRelease(EMouseButton::middle);
        PPR_TEST_ASSERT(not latch.isEngaged());

        latch.notifyPress(EMouseButton::right);
        PPR_TEST_ASSERT(not latch.isEngaged());
        latch.notifyRelease(EMouseButton::right);
        PPR_TEST_ASSERT(not latch.isEngaged());

        latch.notifyPress(EMouseButton::left);
        latch.resetInputState();
        PPR_TEST_ASSERT(not latch.isEngaged());
    };

    // resetInputState() blur path via posted events (cf.
    // ApplicationEditor::onMainWindowFocused_): a press engages, the backstop
    // clears without any release, and the late release is a no-op.
    PPR_UNIT_TEST(latch_reset_input_state_clears_blur_stale_hold) {
        InputListener foreground{};
        foreground.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });
        InputListener background{};
        background.setRawKeyCallback([](const TimeSpan, const InputMessage &) noexcept {
            return EInputMessageResponse::unhandled;
        });

        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button,
            InputValue{InputDigital{true}},
            InputDeviceID{0u},
            EInputMessageEvent::pressed
        };
        const InputMessage left_release{
            InputKey::left_mouse_button,
            InputValue{InputDigital{false}},
            InputDeviceID{0u},
            EInputMessageEvent::released
        };
        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_press);
        PPR_TEST_ASSERT(latch->isEngaged());
        latch->resetInputState();
        PPR_TEST_ASSERT(not latch->isEngaged());
        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_release);
        PPR_TEST_ASSERT(not latch->isEngaged());

        PPR_TEST_ASSERT(not latch->shutdown(window_context));
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));
    };

    // Drain-latch lock-in: the m_pending_deselect latch set by the ui probe on
    // a background-button press is drained by update (post-NewFrame via
    // clearWindowFocus), so a focused window loses focus WITHOUT an explicit
    // clearWindowFocus call. Contrast sub-case: a captured press sets no latch,
    // so focus is retained. The latch effect is observable here through focus
    // state and key flow, not through the private flag.
    PPR_UNIT_TEST(pending_deselect_drain_clears_focus_without_explicit_clear) {
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

        ImGuiRouting::StubInputService inputs{};
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

            // Focus a text field: keys are captured until a background click deselects.
            // NOTE: SetNextWindow* needs an open frame — update first.
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            auto *imgui_context = static_cast<ImGuiContext *>(ui->getContext());
            PPR_TEST_ASSERT(imgui_context != nullptr);
            ImGui::SetCurrentContext(imgui_context);
            ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
            ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
            PPR_TEST_ASSERT(ImGui::Begin("drain_probe"));
            ImGui::SetKeyboardFocusHere();
            char probe_text[64]{};
            std::ignore = ImGui::InputText("##probe", probe_text, sizeof(probe_text));
            ImGui::End();
            ImGui::Render();
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            // Plain window focus captures keys (WantCaptureKeyboard), though
            // the text field itself only activates on typing.
            PPR_TEST_ASSERT(ImGui::GetIO().WantCaptureKeyboard);

            const InputMessage w_press{
                InputKey::w,
                InputValue{InputDigital{true}},
                InputDeviceID{0u},
                EInputMessageEvent::pressed
            };
            PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, w_press) == EInputMessageResponse::consumed);
            PPR_TEST_ASSERT(move_count == 0);
            // Flush the key event alone: ImGui trickles queued mouse input
            // behind key/button changes in the same flush.
            ImGui::Render();
            PPR_TEST_ASSERT(not ui->update(frame, viewport));

            // Captured sub-case: hover inside the window, so the press is
            // captured — the probe sets no latch and the update below retains focus.
            // Hover is per-frame: re-submit the window in this same open frame and
            // feed the position directly (cf. wheel_hover_split).
            PPR_TEST_ASSERT(ImGui::Begin("drain_probe"));
            ImGui::End();
            ImGui::GetIO().AddMousePosEvent(100.0f, 100.0f);
            ImGui::Render();
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            PPR_TEST_ASSERT(ImGui::GetIO().WantCaptureMouseUnlessPopupClose);
            PPR_TEST_ASSERT(ui->hasMouseCaptureUnlessPopupClose());
            const InputMessage captured_click{
                InputKey::left_mouse_button,
                InputValue{InputDigital{true}},
                InputDeviceID{0u},
                EInputMessageEvent::pressed
            };
            PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, captured_click) == EInputMessageResponse::consumed);
            const InputMessage captured_release{
                InputKey::left_mouse_button,
                InputValue{InputDigital{false}},
                InputDeviceID{0u},
                EInputMessageEvent::released
            };
            std::ignore = routed.m_context.postKeyEvent(TimeSpan{}, captured_release);
            ImGui::Render();
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            PPR_TEST_ASSERT(ImGui::GetIO().WantCaptureKeyboard);
            PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, w_press) == EInputMessageResponse::consumed);
            PPR_TEST_ASSERT(move_count == 0);
            // Flush the deferred button release before feeding the next position.
            ImGui::Render();
            PPR_TEST_ASSERT(not ui->update(frame, viewport));

            // Background sub-case: void point, no widget — the press is handled
            // (BOTH vote) and arms the latch; the update below drains it, so
            // focus clears with no explicit clearWindowFocus call anywhere.
            const InputMessage void_move{
                InputKey::mouse_2d,
                InputValue{InputAxis2D{float2{750.0f, 550.0f}, float2{0.0f, 0.0f}}},
                InputDeviceID{0u},
                EInputMessageEvent::axis
            };
            PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, void_move) == EInputMessageResponse::handled);
            ImGui::Render();
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            // The engine cursor mapping delivered the void position, so nothing
            // is hovered and the probe observes background.
            PPR_TEST_ASSERT(ImGui::GetIO().MousePos.x == 750.0f);
            PPR_TEST_ASSERT(ImGui::GetIO().MousePos.y == 550.0f);
            PPR_TEST_ASSERT(not ui->hasMouseCaptureUnlessPopupClose());
            const InputMessage void_click{
                InputKey::left_mouse_button,
                InputValue{InputDigital{true}},
                InputDeviceID{0u},
                EInputMessageEvent::pressed
            };
            PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, void_click) == EInputMessageResponse::handled);
            const InputMessage void_release{
                InputKey::left_mouse_button,
                InputValue{InputDigital{false}},
                InputDeviceID{0u},
                EInputMessageEvent::released
            };
            std::ignore = routed.m_context.postKeyEvent(TimeSpan{}, void_release);
            ImGui::Render();
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            ImGui::Render();
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            // Latch drained: focus auto-cleared, keys flow to the player again.
            PPR_TEST_ASSERT(not ImGui::GetIO().WantCaptureKeyboard);
            PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, w_press) == EInputMessageResponse::consumed);
            PPR_TEST_ASSERT(move_count == 1);

            PPR_TEST_ASSERT(not ui->shutdown());
            PPR_TEST_ASSERT(not ui->shutdown());
            std::ignore = ui.release();
            PPR_TEST_ASSERT(not routed.shutdown());
        }
        std::ignore = window.release();
    };
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest imgui_routing = UnitTest::Named("imgui_routing") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::imgui_first_on_window_context_observes_scene_mapped_key,
            detail::detector_observes_before_player_mapping_and_never_consumes,
            detail::captured_press_starves_actuator_background_press_reaches_actuator,
            detail::release_consumed_by_ui_leaves_hold_until_reset_backstop,
            detail::shutdown_detaches_detector_before_player,
            detail::background_drag_predicate_covers_left_and_middle_only,
            detail::state_path_repeat_hold_single_release_clears,
            detail::colocated_detector_priority_rejected_by_validate,
            detail::latch_validate_rejects_null_background_and_bad_ordering,
            detail::latch_initialize_rejects_null_double_and_bad_validate,
            detail::latch_shutdown_reports_not_connected_and_drains_to_unhandled,
            detail::latch_notify_press_release_tracks_per_button_overlap,
            detail::latch_reset_input_state_clears_blur_stale_hold,
            detail::pending_deselect_drain_clears_focus_without_explicit_clear,
        });
    };
} // namespace pP::tests
