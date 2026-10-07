module;
#include "pP/UnitTest.h"

#include <cstring>

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
    }

    // Routing order: an ImGui-style listener (raw callback only, no mappings)
    // at -1000 on the window context still observes a scene-mapped key before
    // the scene listener at 0 consumes it. Mirrors Application init order
    // (ImGui registered first) without instantiating the RHI-backed service.
    PPR_UNIT_TEST (imgui_first_on_window_context_observes_scene_mapped_key) {
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
        scene_action.setTriggered(
            [&scene_triggered](const InputActionEvent &, const InputKey &) noexcept { ++scene_triggered; });

        InputContext window_context{};
        window_context.addInputListener(safe_ptr<InputListener>{&imgui_listener}, -1000);
        window_context.addInputListener(safe_ptr<InputListener>{&scene_listener}, 0);

        const InputMessage msg{
            InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
        const EInputMessageResponse response = window_context.postKeyEvent(TimeSpan{}, msg);

        PPR_TEST_ASSERT(raw_count == 1);
        PPR_TEST_ASSERT(scene_triggered == 1);
        PPR_TEST_ASSERT(response == EInputMessageResponse::consumed);
    };

    // Background-drag detector observes at detector(1) before player(2) with camera mapping and never consumes.
    PPR_UNIT_TEST (detector_observes_before_player_mapping_and_never_consumes) {
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
        foreground.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });

        InputAction camera_rotate{"CameraRotate", EInputValueType::digital};
        InputMapping camera_mapping{"Camera"};
        camera_mapping.mapInputKey(SharedInputAction{&camera_rotate}, InputKey::left_mouse_button);
        InputListener background{};
        background.addInputMapping(
            SharedInputMapping{&camera_mapping}, static_cast<int>(EInputMappingPriority::camera));
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
        window_context.addInputListener(
            safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u},
            EInputMessageEvent::pressed};
        const EInputMessageResponse response = window_context.postKeyEvent(TimeSpan{}, left_press);

        PPR_TEST_ASSERT(detector_ran_before_camera);
        PPR_TEST_ASSERT(camera_triggered == 1);
        PPR_TEST_ASSERT(response == EInputMessageResponse::consumed);
        PPR_TEST_ASSERT(latch->isEngaged());

        const InputMessage left_release{
            InputKey::left_mouse_button, InputValue{InputDigital{false}}, InputDeviceID{0u},
            EInputMessageEvent::released};
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
    PPR_UNIT_TEST (captured_press_starves_actuator_background_press_reaches_actuator) {
        InputAction ui_click{"UiClick", EInputValueType::digital};
        InputMapping ui_mapping{"Ui"};
        ui_mapping.mapInputKey(SharedInputAction{&ui_click}, InputKey::left_mouse_button);
        InputListener foreground{};
        foreground.addInputMapping(SharedInputMapping{&ui_mapping}, static_cast<int>(EInputMappingPriority::camera));

        InputAction camera_rotate{"CameraRotate", EInputValueType::digital};
        InputMapping camera_mapping{"Camera"};
        camera_mapping.mapInputKey(SharedInputAction{&camera_rotate}, InputKey::left_mouse_button);
        InputListener background{};
        background.addInputMapping(
            SharedInputMapping{&camera_mapping}, static_cast<int>(EInputMappingPriority::camera));

        // Listeners outlive the context: it holds safe_ptrs to them. The
        // foreground (ui) is added manually; the latch registers its internal
        // detector plus the background player, and shuts them down
        // detector-first (production order).
        InputContext window_context{};
        window_context.addInputListener(
            safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u},
            EInputMessageEvent::pressed};
        const InputMessage left_release{
            InputKey::left_mouse_button, InputValue{InputDigital{false}}, InputDeviceID{0u},
            EInputMessageEvent::released};

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
    PPR_UNIT_TEST (release_consumed_by_ui_leaves_hold_until_reset_backstop) {
        InputAction ui_click{"UiClick", EInputValueType::digital};
        InputMapping ui_mapping{"Ui"};
        ui_mapping.mapInputKey(SharedInputAction{&ui_click}, InputKey::left_mouse_button);
        InputListener foreground{};
        foreground.addInputMapping(SharedInputMapping{&ui_mapping}, static_cast<int>(EInputMappingPriority::camera));

        InputListener background{};
        background.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });

        InputContext window_context{};
        window_context.addInputListener(
            safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u},
            EInputMessageEvent::pressed};
        const InputMessage left_release{
            InputKey::left_mouse_button, InputValue{InputDigital{false}}, InputDeviceID{0u},
            EInputMessageEvent::released};

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
    PPR_UNIT_TEST (shutdown_detaches_detector_before_player) {
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
        scene_action.setTriggered(
            [&scene_triggered](const InputActionEvent &, const InputKey &) noexcept { ++scene_triggered; });

        InputContext window_context{};
        window_context.addInputListener(
            safe_ptr<InputListener>{&detector}, static_cast<int>(EInputListenerPriority::detector));
        window_context.addInputListener(
            safe_ptr<InputListener>{&player}, static_cast<int>(EInputListenerPriority::player));
        PPR_TEST_ASSERT(window_context.hasInputListener(detector));
        PPR_TEST_ASSERT(window_context.hasInputListener(player));

        const InputMessage move{
            InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
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
    PPR_UNIT_TEST (background_drag_predicate_covers_left_and_middle_only) {
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
    PPR_UNIT_TEST (state_path_repeat_hold_single_release_clears) {
        InputListener foreground{};
        foreground.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });
        InputListener background{};
        background.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });

        // Listeners outlive the context: it holds safe_ptrs to them, and the
        // latch shuts down detector-first (production shutdown order). The
        // detector itself is latch-owned, so its registration is observable
        // only through engagement, not through hasInputListener.
        InputContext window_context{};
        window_context.addInputListener(
            safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->validate());
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));
        PPR_TEST_ASSERT(window_context.hasInputListener(background));

        const InputMessage left_press{
            InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u},
            EInputMessageEvent::pressed};
        const InputMessage left_repeat{
            InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::repeat};
        const InputMessage left_release{
            InputKey::left_mouse_button, InputValue{InputDigital{false}}, InputDeviceID{0u},
            EInputMessageEvent::released};

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
    PPR_UNIT_TEST (colocated_detector_priority_rejected_by_validate) {
        InputListener background{};
        background.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });

        // detector == player: legacy co-location, rejected.
        const InputBackgroundLatch colocated(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::player));
        PPR_TEST_ASSERT(colocated.validate() == std::make_error_code(std::errc::invalid_argument));

        // initialize() with that topology registers nothing and reports the
        // validation failure.
        InputListener foreground{};
        foreground.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });
        InputContext window_context{};
        window_context.addInputListener(
            safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto bad = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::player));
        PPR_TEST_ASSERT(
            bad->initialize(window_context, safe_ptr<const InputListener>{&foreground}) ==
            std::make_error_code(std::errc::invalid_argument));
        PPR_TEST_ASSERT(not window_context.hasInputListener(background));
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));
    };

    // validate() contract: background owner present and strict
    // ui < detector < player ordering. Anything else is invalid_argument.
    PPR_UNIT_TEST (latch_validate_rejects_null_background_and_bad_ordering) {
        constexpr int ui = static_cast<int>(EInputListenerPriority::ui);
        constexpr int detector = static_cast<int>(EInputListenerPriority::detector);
        constexpr int player = static_cast<int>(EInputListenerPriority::player);

        InputListener background{};
        background.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });

        const InputBackgroundLatch null_background{ui, safe_ptr<InputListener>{}, player, detector};
        PPR_TEST_ASSERT(null_background.validate() == std::make_error_code(std::errc::invalid_argument));

        const InputBackgroundLatch foreground_at_detector{
            detector, safe_ptr<InputListener>{&background}, player, detector};
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
    PPR_UNIT_TEST (latch_initialize_rejects_null_double_and_bad_validate) {
        InputListener foreground{};
        foreground.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });
        InputListener background{};
        background.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });

        InputContext window_context{};
        window_context.addInputListener(
            safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(
            latch->initialize(window_context, safe_ptr<const InputListener>{}) ==
            std::make_error_code(std::errc::invalid_argument));
        PPR_TEST_ASSERT(not window_context.hasInputListener(background));

        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));
        PPR_TEST_ASSERT(window_context.hasInputListener(background));
        PPR_TEST_ASSERT(
            latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}) ==
            std::make_error_code(std::errc::already_connected));

        PPR_TEST_ASSERT(not latch->shutdown(window_context));
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));

        auto bad = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::player));
        PPR_TEST_ASSERT(
            bad->initialize(window_context, safe_ptr<const InputListener>{&foreground}) ==
            std::make_error_code(std::errc::invalid_argument));
        PPR_TEST_ASSERT(not window_context.hasInputListener(background));
    };

    // shutdown() contract: missing parts aggregate not_connected (a fresh
    // latch, and a second shutdown after a full one), and a full shutdown
    // drains the context to unhandled. Detector-first removal mirrors
    // production teardown; engagement retention is covered by the state_path
    // leaf above.
    PPR_UNIT_TEST (latch_shutdown_reports_not_connected_and_drains_to_unhandled) {
        InputListener foreground{};
        foreground.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });
        InputListener background{};
        background.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });

        InputContext window_context{};
        window_context.addInputListener(
            safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u},
            EInputMessageEvent::pressed};
        std::ignore = window_context.postKeyEvent(TimeSpan{}, left_press);
        PPR_TEST_ASSERT(latch->isEngaged());

        PPR_TEST_ASSERT(not latch->shutdown(window_context));
        PPR_TEST_ASSERT(not window_context.hasInputListener(background));
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, left_press) == EInputMessageResponse::unhandled);
        PPR_TEST_ASSERT(window_context.removeInputListener(foreground));
        PPR_TEST_ASSERT(window_context.postKeyEvent(TimeSpan{}, left_press) == EInputMessageResponse::unhandled);
        PPR_TEST_ASSERT(latch->shutdown(window_context) == std::make_error_code(std::errc::not_connected));

        auto fresh = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(fresh->shutdown(window_context) == std::make_error_code(std::errc::not_connected));
    };

    // notifyPress/notifyRelease contract (direct, latch-internal counting):
    // release-without-press is a no-op, same-button overlap needs one release
    // per press, buttons track independently, non-drag buttons never engage.
    PPR_UNIT_TEST (latch_notify_press_release_tracks_per_button_overlap) {
        InputListener background{};
        InputBackgroundLatch latch(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
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
    PPR_UNIT_TEST (latch_reset_input_state_clears_blur_stale_hold) {
        InputListener foreground{};
        foreground.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });
        InputListener background{};
        background.setRawKeyCallback(
            [](const TimeSpan, const InputMessage &) noexcept { return EInputMessageResponse::unhandled; });

        InputContext window_context{};
        window_context.addInputListener(
            safe_ptr<InputListener>{&foreground}, static_cast<int>(EInputListenerPriority::ui));
        auto latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr<InputListener>{&background},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));
        PPR_TEST_ASSERT(not latch->initialize(window_context, safe_ptr<const InputListener>{&foreground}));

        const InputMessage left_press{
            InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u},
            EInputMessageEvent::pressed};
        const InputMessage left_release{
            InputKey::left_mouse_button, InputValue{InputDigital{false}}, InputDeviceID{0u},
            EInputMessageEvent::released};
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
    PPR_UNIT_TEST (pending_deselect_drain_clears_focus_without_explicit_clear) {
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
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
        window.m_framebuffer_size = int2{800, 600};

        InputAction player_move{"PlayerMove", EInputValueType::digital};
        InputMapping player_mapping{"Player"};
        player_mapping.mapInputKey(SharedInputAction{&player_move}, InputKey::w);
        InputListener player_listener{};
        player_listener.addInputMapping(SharedInputMapping{&player_mapping}, 0);
        int move_count = 0;
        player_move.setTriggered([&move_count](const InputActionEvent &, const InputKey &) noexcept { ++move_count; });
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
                InputKey::w, InputValue{InputDigital{true}}, InputDeviceID{0u}, EInputMessageEvent::pressed};
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
                InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u},
                EInputMessageEvent::pressed};
            PPR_TEST_ASSERT(
                routed.m_context.postKeyEvent(TimeSpan{}, captured_click) == EInputMessageResponse::consumed);
            const InputMessage captured_release{
                InputKey::left_mouse_button, InputValue{InputDigital{false}}, InputDeviceID{0u},
                EInputMessageEvent::released};
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
                InputKey::mouse_2d, InputValue{InputAxis2D{float2{750.0f, 550.0f}, float2{0.0f, 0.0f}}},
                InputDeviceID{0u}, EInputMessageEvent::axis};
            PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, void_move) == EInputMessageResponse::handled);
            ImGui::Render();
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            // The engine cursor mapping delivered the void position, so nothing
            // is hovered and the probe observes background.
            PPR_TEST_ASSERT(ImGui::GetIO().MousePos.x == 750.0f);
            PPR_TEST_ASSERT(ImGui::GetIO().MousePos.y == 550.0f);
            PPR_TEST_ASSERT(not ui->hasMouseCaptureUnlessPopupClose());
            const InputMessage void_click{
                InputKey::left_mouse_button, InputValue{InputDigital{true}}, InputDeviceID{0u},
                EInputMessageEvent::pressed};
            PPR_TEST_ASSERT(routed.m_context.postKeyEvent(TimeSpan{}, void_click) == EInputMessageResponse::handled);
            const InputMessage void_release{
                InputKey::left_mouse_button, InputValue{InputDigital{false}}, InputDeviceID{0u},
                EInputMessageEvent::released};
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

    // --- Detached-UI contract (Finding 5 + Finding 3 coverage) -----------
    // The build/encode boundary carries ONLY owned DetachedUiDrawData.
    // These leaves pin the client-side contract without a GPU encode:
    // snapshot ownership across live churn and context destruction,
    // selective callback rejection (unknown dropped, known reset flagged),
    // and font publication/replacement under overlap with same-generation
    // retransmission until acknowledgement.
    namespace DetachedUi {
        bool g_foreign_callback_called = false;

        void foreignDrawCallback(const ImDrawList *, const ImDrawCmd *) {
            g_foreign_callback_called = true;
        }

        // Known-reset identity for the detach-boundary leaf below. The
        // harness runs with no renderer backend, so PlatformIO holds no
        // reset callback by default — this stub is installed as
        // DrawCallback_ResetRenderState so the reset sub-case exercises
        // the production equality filter with a valid non-null pointer.
        // Like the foreign callback above, it must never be invoked on
        // the build path (converted to a flag, never called).
        bool g_reset_callback_called = false;

        void detachedResetCallback(const ImDrawList *, const ImDrawCmd *) {
            g_reset_callback_called = true;
        }

        // Field-wise vertex equality (mango float2 equality yields a mask,
        // not bool, so component compare; also padding-proof, unlike a
        // bulk memcmp over memcpy-filled vertices).
        [[nodiscard]] bool sameVertices(const Array<DetachedUiVertex> &lhs, const Array<DetachedUiVertex> &rhs) {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (u32 i = 0u; i < lhs.size(); ++i) {
                if (lhs[i].m_position.x != rhs[i].m_position.x or
                    lhs[i].m_position.y != rhs[i].m_position.y or
                    lhs[i].m_uv.x != rhs[i].m_uv.x or
                    lhs[i].m_uv.y != rhs[i].m_uv.y or
                    lhs[i].m_color != rhs[i].m_color) {
                    return false;
                }
            }
            return true;
        }
    }

    // Snapshot lifetime across the build/encode boundary: the published
    // frame is a deep, owned copy — later live frames with different
    // content, and even live-context destruction, leave it identical. The
    // encode lane therefore never observes torn or recycled live memory,
    // whenever it runs.
    PPR_UNIT_TEST (detached_snapshot_outlives_live_state) {
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
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
        window.m_framebuffer_size = int2{800, 600};

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

        // Frame 1 with content: the snapshotted payload.
        PPR_TEST_ASSERT(not ui->update(frame, viewport));
        ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
        PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
        ImGui::TextUnformatted("detached snapshot payload");
        ImGui::End();
        DetachedUiDrawData snapshot{};
        PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(snapshot));
        PPR_TEST_ASSERT(snapshot.m_has_content);
        PPR_TEST_ASSERT(snapshot.m_vertices.size() != 0u);
        PPR_TEST_ASSERT(snapshot.m_commands.size() != 0u);
        // Frozen clone at build time: any aliasing of live draw-list
        // memory would diverge below once live frames churn.
        const DetachedUiDrawData frozen = snapshot;

        // Churn: further live frames with different-length content plus
        // discarded builds; the snapshot must not move.
        for (u32 round = 0u; round < 4u; ++round) {
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
            ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
            PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
            for (u32 line = 0u; line <= round; ++line) {
                ImGui::Text("churn line %u", line);
            }
            ImGui::End();
            DetachedUiDrawData discarded{};
            PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(discarded));
        }
        PPR_TEST_ASSERT(snapshot.m_vertices.size() == frozen.m_vertices.size());
        PPR_TEST_ASSERT(DetachedUi::sameVertices(snapshot.m_vertices, frozen.m_vertices));
        PPR_TEST_ASSERT(std::ranges::equal(snapshot.m_indices, frozen.m_indices));
        PPR_TEST_ASSERT(snapshot.m_commands.size() == frozen.m_commands.size());
        PPR_TEST_ASSERT(
            std::memcmp(
                snapshot.m_commands.data(), frozen.m_commands.data(),
                snapshot.m_commands.size() * sizeof(DetachedUiCommand)) == 0);

        // Live-context destruction: the snapshot stays fully readable
        // (an aliasing snapshot would be use-after-free here).
        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(snapshot.m_vertices.size() == frozen.m_vertices.size());
        PPR_TEST_ASSERT(DetachedUi::sameVertices(snapshot.m_vertices, frozen.m_vertices));
        PPR_TEST_ASSERT(snapshot.m_commands.size() == frozen.m_commands.size());
        PPR_TEST_ASSERT(
            std::memcmp(
                snapshot.m_commands.data(), frozen.m_commands.data(),
                snapshot.m_commands.size() * sizeof(DetachedUiCommand)) == 0);

        PPR_TEST_ASSERT(not ui->shutdown());
        std::ignore = ui.release();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // Callback rejection/reset at the detach boundary: an unknown
    // client-context callback never crosses to the lane (never invoked,
    // no command emitted), while the known render-only reset crosses as a
    // flag honored at its encoded position — selective, never wholesale.
    // Both AddCallback inputs below are valid non-null function pointers:
    // imgui asserts (callback != 0), so a null callback must never reach
    // a real ImDrawList. Rejection happens in OUR builder
    // (buildDetachedUiFrame), before anything reaches imgui.
    PPR_UNIT_TEST (detached_build_rejects_foreign_callbacks_and_flags_reset) {
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
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
        window.m_framebuffer_size = int2{800, 600};

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
        DetachedUi::g_foreign_callback_called = false;
        DetachedUi::g_reset_callback_called = false;

        // The harness runs with no renderer backend, so PlatformIO holds
        // no reset identity by default (null — passing it to AddCallback
        // would trip imgui's callback != 0 assert). Install the test reset
        // stub as the known reset so the reset sub-case below exercises
        // the production equality filter (UserCallback ==
        // DrawCallback_ResetRenderState) with a valid non-null pointer.
        // Restored before teardown while the context is still live.
        auto &platform_io = ImGui::GetPlatformIO();
        const auto saved_reset = platform_io.DrawCallback_ResetRenderState;
        platform_io.DrawCallback_ResetRenderState = &DetachedUi::detachedResetCallback;
        PPR_TEST_ASSERT(platform_io.DrawCallback_ResetRenderState == &DetachedUi::detachedResetCallback);
        PPR_TEST_ASSERT(&DetachedUi::foreignDrawCallback != &DetachedUi::detachedResetCallback);

        // Baseline frame: empty window (background only, no text and no
        // callbacks). Text-free on purpose: consecutive text draws merge
        // into one command, so only the background command gives all three
        // frames below an identical merge-stable baseline to compare
        // against.
        PPR_TEST_ASSERT(not ui->update(frame, viewport));
        ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
        PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
        ImGui::End();
        DetachedUiDrawData baseline{};
        PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(baseline));
        PPR_TEST_ASSERT(baseline.m_has_content);
        const std::size_t baseline_commands = baseline.m_commands.size();
        PPR_TEST_ASSERT(baseline_commands != 0u);

        // Foreign callback mid-draw: rejected — the build succeeds, the
        // callback never runs on the build path, and no command crosses
        // for it (count matches the callback-free baseline exactly).
        PPR_TEST_ASSERT(not ui->update(frame, viewport));
        ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
        PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
        ImGui::GetWindowDrawList()->AddCallback(&DetachedUi::foreignDrawCallback, nullptr);
        ImGui::End();
        DetachedUiDrawData rejected{};
        PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(rejected));
        PPR_TEST_ASSERT(not DetachedUi::g_foreign_callback_called);
        PPR_TEST_ASSERT(rejected.m_has_content);
        PPR_TEST_ASSERT(rejected.m_commands.size() == baseline_commands);
        for (const DetachedUiCommand &cmd: rejected.m_commands) {
            PPR_TEST_ASSERT(not cmd.m_reset_render_state);
        }

        // Known render-only reset mid-draw: crosses as exactly one
        // reset-flag command on top of the unchanged baseline draws. Uses
        // the installed stub directly: non-null (never trips imgui's
        // callback != 0 assert) and equal to the PlatformIO identity, so
        // the production filter honors it.
        PPR_TEST_ASSERT(not ui->update(frame, viewport));
        ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
        PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
        ImGui::GetWindowDrawList()->AddCallback(&DetachedUi::detachedResetCallback, nullptr);
        ImGui::End();
        DetachedUiDrawData flagged{};
        PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(flagged));
        PPR_TEST_ASSERT(not DetachedUi::g_reset_callback_called);
        PPR_TEST_ASSERT(flagged.m_has_content);
        PPR_TEST_ASSERT(flagged.m_commands.size() == baseline_commands + 1u);
        u32 reset_flags = 0u;
        for (const DetachedUiCommand &cmd: flagged.m_commands) {
            if (cmd.m_reset_render_state) {
                ++reset_flags;
            }
        }
        PPR_TEST_ASSERT(reset_flags == 1u);

        // Restore the harness identity while the context is still live
        // (no backend owned it, so this is typically null again).
        platform_io.DrawCallback_ResetRenderState = saved_reset;

        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        std::ignore = ui.release();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // Font publication/replacement under overlap (Finding 3 + Finding 5):
    // init installs generation 1 on the GPU, so steady-state builds at
    // scale 1 carry no publication; a 2x scale change then stages a newer
    // generation, and a second pre-acknowledgement build retransmits the
    // SAME generation (identical pixels) — the first post-scale
    // publication dropped (never encoded), so a dropped frame can never
    // lose a rebuild.
    // NOTE (production lane): the D3D12 debug layer reports
    // ResourceBarrier warnings on imgui_font_atlas (same subresource in
    // separate Barrier Descs) during service init's font upload
    // (copyBufferToTexture + setTextureState). No one-line merge exists
    // in the test-visible upload path — the split is emitted below the
    // IUIService boundary — so it is left for the production lane.
    PPR_UNIT_TEST (detached_font_publication_retransmits_same_generation_until_ack) {
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
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
        window.m_framebuffer_size = int2{800, 600};

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

        // Init installs generation 1 on the GPU (createFontTexture_ stages
        // and installs it together with the frozen TexID), so steady-state
        // builds carry no publication until a scale change opens the
        // overlap window.
        PPR_TEST_ASSERT(ui->installedFontVersion() == 1u);

        // Build A at scale 1: steady state — no scale change and nothing
        // unacknowledged, so no publication (version 0 with empty pixels
        // means "keep the installed atlas").
        PPR_TEST_ASSERT(not ui->update(frame, viewport));
        ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
        PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
        ImGui::TextUnformatted("font publication probe");
        ImGui::End();
        DetachedUiDrawData first{};
        PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(first));
        PPR_TEST_ASSERT(first.m_font_version == 0u);
        PPR_TEST_ASSERT(first.m_font_pixels.empty());
        PPR_TEST_ASSERT(ui->installedFontVersion() == 1u);

        // Build B at the same scale, still with nothing unacknowledged:
        // still silent — a second build mints nothing while the staged
        // version equals the installed one (either frame dropped loses
        // nothing because neither carries a publication).
        PPR_TEST_ASSERT(not ui->update(frame, viewport));
        ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
        PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
        ImGui::TextUnformatted("font publication probe");
        ImGui::End();
        DetachedUiDrawData second{};
        PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(second));
        PPR_TEST_ASSERT(second.m_font_version == 0u);
        PPR_TEST_ASSERT(second.m_font_pixels.empty());
        PPR_TEST_ASSERT(ui->installedFontVersion() == 1u);

        // Replacement: a scale change to 2x stages a newer generation —
        // still pre-acknowledgement (installed stays 1), so the overlap
        // window opens here.
        window.m_content_scale = float2{2.0f, 2.0f};
        PPR_TEST_ASSERT(not ui->update(frame, viewport));
        ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
        PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
        ImGui::TextUnformatted("font publication probe");
        ImGui::End();
        DetachedUiDrawData third{};
        PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(third));
        PPR_TEST_ASSERT(third.m_font_version != 0u);
        PPR_TEST_ASSERT(third.m_font_version == ui->installedFontVersion() + 1u);
        PPR_TEST_ASSERT(third.m_font_pixels.size() != 0u);
        PPR_TEST_ASSERT(third.m_font_extent.x > 0 and third.m_font_extent.y > 0);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 1u);

        // Build D before any acknowledgement: same 2x scale with the
        // staged generation still unacknowledged — retransmits the SAME
        // version with identical pixels (never mints a fresh generation
        // for an unacknowledged retransmit).
        PPR_TEST_ASSERT(not ui->update(frame, viewport));
        ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
        PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
        ImGui::TextUnformatted("font publication probe");
        ImGui::End();
        DetachedUiDrawData fourth{};
        PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(fourth));
        PPR_TEST_ASSERT(fourth.m_font_version == third.m_font_version);
        PPR_TEST_ASSERT(fourth.m_font_version > second.m_font_version);
        PPR_TEST_ASSERT(std::ranges::equal(fourth.m_font_pixels, third.m_font_pixels));
        PPR_TEST_ASSERT(
            fourth.m_font_extent.x == third.m_font_extent.x and fourth.m_font_extent.y == third.m_font_extent.y);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 1u);
        // third is dropped here (never encoded): fourth alone stays
        // self-sufficient, proven by its full pixel carriage above.

        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        std::ignore = ui.release();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest imgui_routing = UnitTest::Named("imgui_routing") / [](UnitTest::IRun &_) -> void {
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
            detail::detached_snapshot_outlives_live_state,
            detail::detached_build_rejects_foreign_callbacks_and_flags_reset,
            detail::detached_font_publication_retransmits_same_generation_until_ack,
        });
    };

    const UnitTest &imgui_routingTests() noexcept {
        return imgui_routing;
    }
} // namespace pP::tests
