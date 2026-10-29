module;
#include "pP/UnitTest.h"

#include <cstring>

#include <slang.h>

// Atlas PNG captures for the M4 font-rasterizer harness (CPU-only encoder).
#include <mango/image/image.hpp>

// Rasterizer build selection. IMGUI_ENABLE_FREETYPE is a PUBLIC definition of
// imgui.base (cmake/external/DearImGui.cmake), so it reaches this TU through
// engine.app -> imgui. The harness uses it only as a cross-check: the loader
// identity that decides which variants run is io.Fonts->FontLoaderName, read
// at runtime.
#ifdef IMGUI_ENABLE_FREETYPE
#define PPR_M4_IMGUI_FREETYPE 1
#else
#define PPR_M4_IMGUI_FREETYPE 0
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
            PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, {}, 0));

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
            ui.reset();
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
        PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, {}, 0));

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
        ui.reset();
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
        PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, {}, 0));

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
        ui.reset();
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

        // Unwind-safe teardown (same discipline as the font_density leaf):
        // detach the input context and release the window handle before
        // ~WindowInputContext and ~Window run during unwinding, so a failed
        // PPR_TEST_ASSERT surfaces its own predicate instead of a destructor
        // abort. Registered before `routed.initialize` so an initialize
        // failure is covered too.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));

        auto ui = ui::createImGuiService();
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, {}, 0));

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

        // Replacement: a framebuffer ratio change to 2x (bake density 2.0)
        // stages a newer generation — still pre-acknowledgement (installed
        // stays 1), so the overlap window opens here.
        window.m_framebuffer_size = int2{1600, 1200};
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

        // Trip back to 1x while the 2x generation is still unacknowledged:
        // the rebake advances the content revision, and the gate keys on
        // content — not scale identity — so it mints a fresh generation
        // with the new 1x pixels instead of reusing version 1's atlas.
        window.m_framebuffer_size = int2{800, 600};
        PPR_TEST_ASSERT(not ui->update(frame, viewport));
        ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
        PPR_TEST_ASSERT(ImGui::Begin("detach_probe"));
        ImGui::TextUnformatted("font publication probe");
        ImGui::End();
        DetachedUiDrawData fifth{};
        PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(fifth));
        PPR_TEST_ASSERT(fifth.m_font_version == fourth.m_font_version + 1u);
        PPR_TEST_ASSERT(fifth.m_font_pixels.size() != 0u);
        PPR_TEST_ASSERT(fifth.m_font_extent.x > 0 and fifth.m_font_extent.y > 0);
        PPR_TEST_ASSERT(not std::ranges::equal(fifth.m_font_pixels, fourth.m_font_pixels));
        PPR_TEST_ASSERT(ui->installedFontVersion() == 1u);

        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // Shared-module refcount guard: the imgui slang module is borrowed from
    // the shader session, never owned — repeated UI initialize/shutdown
    // cycles without resetting the session or the device must all succeed
    // with stable device + global-session identities. Adopting the borrowed
    // module as owned consumes a session reference per init and eventually
    // crashes inside slang-compiler on stale module state.
    PPR_UNIT_TEST (repeated_ui_init_shutdown_preserves_module_refs) {
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

        // Stable identities: no UI cycle may disturb the session or device.
        auto *const stable_device = &rhi->getDevice();
        auto *const stable_session = shader->getGlobalSession();
        PPR_TEST_ASSERT(stable_device != nullptr);
        PPR_TEST_ASSERT(stable_session != nullptr);

        ImGuiRouting::StubInputService inputs{};
        Window window{
            WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}};
        window.m_framebuffer_size = int2{800, 600};

        WindowInputContext routed{safe_ptr<IInputService>{&inputs}};

        // Unwind-safe teardown (same discipline as the font_density leaf):
        // detach the input context and release the window handle before
        // ~WindowInputContext and ~Window run during unwinding, so a failed
        // PPR_TEST_ASSERT surfaces its own predicate instead of a destructor
        // abort. Registered before `routed.initialize` so an initialize
        // failure is covered too.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
        WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
        std::ignore = viewport.updateFromWindow();
        const TimeSpan frame{std::chrono::milliseconds{16}};

        constexpr u32 kCycles = 16u;
        for (u32 cycle = 0u; cycle < kCycles; ++cycle) {
            auto ui = ui::createImGuiService();
            // Per-cycle unwind-safe teardown: a failed PPR_TEST_ASSERT inside
            // this iteration must shut the service down before ~ImGuiService
            // runs, or its live-context assert masks the predicate.
            PPR_DEFER {
                if (ui) {
                    std::ignore = ui->shutdown();
                }
            };

            PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, {}, 0));
            PPR_TEST_ASSERT(&rhi->getDevice() == stable_device);
            PPR_TEST_ASSERT(shader->getGlobalSession() == stable_session);

            // Routing/reinit proof per cycle: the listener is registered and
            // a live frame builds on the freshly linked program.
            PPR_TEST_ASSERT(ui->getContext() != nullptr);
            PPR_TEST_ASSERT(ui->getInputListener().isValid());
            PPR_TEST_ASSERT(routed.m_context.hasInputListener(*ui->getInputListener()));
            auto *imgui_context = static_cast<ImGuiContext *>(ui->getContext());
            PPR_TEST_ASSERT(imgui_context != nullptr);
            ImGui::SetCurrentContext(imgui_context);
            PPR_TEST_ASSERT(not ui->update(frame, viewport));
            ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
            ImGui::SetNextWindowSize(ImVec2{400.0f, 300.0f});
            PPR_TEST_ASSERT(ImGui::Begin("refcount_probe"));
            ImGui::TextUnformatted("module refcount probe");
            ImGui::End();
            DetachedUiDrawData built{};
            PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(built));
            PPR_TEST_ASSERT(built.m_has_content);
            PPR_TEST_ASSERT(built.m_commands.size() != 0u);

            PPR_TEST_ASSERT(not ui->shutdown());
            PPR_TEST_ASSERT(not routed.m_context.hasInputListener(*ui->getInputListener()));
            ui.reset();
            PPR_TEST_ASSERT(&rhi->getDevice() == stable_device);
            PPR_TEST_ASSERT(shader->getGlobalSession() == stable_session);
        }

        // Final ordered teardown: context, window, then device, then session.
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
        PPR_TEST_ASSERT(not rhi->shutdown());
        PPR_TEST_ASSERT(not shader->shutdown());
    };

    // ------------------------------------------------------------------
    // target color transfer
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (detached_colors_decode_only_for_srgb_targets_before_interpolation) {
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
            WindowModel{.m_window_size = int2{128, 96}}};
        window.m_framebuffer_size = int2{128, 96};
        WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };
        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
        WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
        std::ignore = viewport.updateFromWindow();

        Renderer renderer{};
        PPR_DEFER {
            std::ignore = renderer.waitOnHost();
            std::ignore = renderer.shutdown();
        };
        PPR_TEST_ASSERT(not renderer.initialize(*rhi));

        auto ui = ui::createImGuiService();
        PPR_DEFER {
            std::ignore = renderer.waitOnHost();
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };
        PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, {}, 0));
        auto *const fence_aware = dynamic_cast<IFenceAwareUiService *>(ui.get());
        PPR_TEST_ASSERT(fence_aware != nullptr);
        auto &device = rhi->getDevice();

        // Two frames per signature reach both completion-selected slot roots;
        // returning to sRGB also checks that target changes refresh the flag.
        for (const auto format: {rhi::Format::RGBA8UnormSrgb, rhi::Format::RGBA8Unorm, rhi::Format::RGBA8UnormSrgb}) {
            rhi::TextureDesc desc{};
            desc.type = rhi::TextureType::Texture2D;
            desc.size = {128u, 96u, 1u};
            desc.arrayLength = 1u;
            desc.mipCount = 1u;
            desc.format = format;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
            desc.defaultState = rhi::ResourceState::RenderTarget;
            desc.label = "UI transfer readback";
            rhi::ComPtr<rhi::ITexture> target{};
            PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, target.writeRef())));

            for (u32 slot_frame = 0u; slot_frame < 2u; ++slot_frame) {
                PPR_TEST_ASSERT(not ui->update(std::chrono::milliseconds{16}, viewport));

                ImDrawList *const draw = ImGui::GetForegroundDrawList();
                const auto ink = ImGui::ColorConvertFloat4ToU32(pP::ui::theme::kTextPrimary.vec4());
                const auto tan = ImGui::ColorConvertFloat4ToU32(pP::ui::theme::kPanel.vec4());
                const auto translucent_ink = ImGui::ColorConvertFloat4ToU32(
                    pP::ui::theme::kTextPrimary.vec4(128.0f / 255.0f));
                draw->AddRectFilled(ImVec2{4.0f, 4.0f}, ImVec2{36.0f, 28.0f}, ink);
                draw->AddRectFilled(ImVec2{44.0f, 4.0f}, ImVec2{76.0f, 28.0f}, tan);
                draw->AddRectFilled(ImVec2{84.0f, 4.0f}, ImVec2{116.0f, 28.0f}, tan);
                draw->AddRectFilled(ImVec2{84.0f, 4.0f}, ImVec2{116.0f, 28.0f}, translucent_ink);
                draw->AddRectFilled(ImVec2{4.0f, 40.0f}, ImVec2{20.0f, 64.0f}, translucent_ink);
                draw->AddRectFilledMultiColor(
                    ImVec2{32.0f, 40.0f}, ImVec2{96.0f, 72.0f}, 0xff000000u, 0xffffffffu, 0xffffffffu, 0xff000000u);

                DetachedUiDrawData packet{};
                PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(packet));
                PPR_TEST_ASSERT(packet.m_has_content);
                const auto encode = [&](const DrawContext &context) {
                    return ui->renderDetachedUiFrame(packet, context);
                };

                const ColorAttachmentOps clear{.m_clear_color = float4{0.0f, 0.0f, 0.0f, 0.0f}};
                PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {DrawSubmission{"UI transfer", encode}}, clear));
                fence_aware->notifyFrameSubmitted(renderer.lastSubmittedFenceValue());
                PPR_TEST_ASSERT(not renderer.waitOnHost());

                shader::ComPtr<ISlangBlob> pixels{};
                rhi::SubresourceLayout layout{};
                PPR_TEST_ASSERT(
                    not make_error_code(device.readTexture(target.get(), 0u, 0u, pixels.writeRef(), &layout)));
                PPR_TEST_ASSERT(pixels.get() != nullptr and pixels->getBufferPointer() != nullptr);
                PPR_TEST_ASSERT(layout.rowPitch >= 128u * 4u);

                const auto *const bytes = static_cast<const u8 *>(pixels->getBufferPointer());
                const auto texel = [&](const u32 x, const u32 y) {
                    const std::size_t offset = static_cast<std::size_t>(layout.rowPitch) * y + x * 4u;
                    PPR_TEST_ASSERT(offset + 4u <= pixels->getBufferSize());
                    return std::array<u8, 4u>{
                        bytes[offset], bytes[offset + 1u], bytes[offset + 2u], bytes[offset + 3u]};
                };

                const auto assertTexelNear = [](const auto actual, const auto expected) {
                    for (std::size_t channel = 0u; channel < actual.size(); ++channel) {
                        PPR_TEST_ASSERT(std::abs(int(actual[channel]) - int(expected[channel])) <= 2);
                    }
                };

                assertTexelNear(texel(20u, 16u), std::array{37u, 34u, 28u, 255u});
                assertTexelNear(texel(60u, 16u), std::array{217u, 210u, 180u, 255u});
                PPR_TEST_ASSERT(std::abs(int(texel(12u, 52u)[3u]) - 128) <= 1);

                if (format == rhi::Format::RGBA8UnormSrgb) {
                    // Linear-light half coverage and a pixel-centred gradient
                    // midpoint differ substantially from byte-space blending.
                    assertTexelNear(texel(100u, 16u), std::array{161u, 155u, 133u, 255u});
                    assertTexelNear(texel(64u, 56u), std::array{189u, 189u, 189u, 255u});
                } else {
                    assertTexelNear(texel(100u, 16u), std::array{127u, 122u, 104u, 255u});
                    assertTexelNear(texel(64u, 56u), std::array{129u, 129u, 129u, 255u});
                }
            }
        }

        PPR_TEST_ASSERT(not renderer.waitOnHost());

        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();

        PPR_TEST_ASSERT(not renderer.shutdown());
    };

    // ------------------------------------------------------------------
    // static font input and bake density
    // ------------------------------------------------------------------

    // Engine-owned font bytes: read and validated (exact size, pinned
    // SHA-256) before any ImGui state exists, added with
    // ImFontConfig::FontDataOwnedByAtlas = false, and never replaced by a
    // default face on rejection. Bake density is the single scale knob: the
    // clamped framebuffer ratio drives ImFont::CurrentRasterizerDensity.
    namespace FontLoading {
        // Pinned staged artifacts (assets/fonts), the same bytes and digests
        // the production Editor font list demands.
        constexpr u64 kInterBytes = 411640u;
        constexpr u64 kMonoBytes = 273900u;
        constexpr u64 kAwesomeBytes = 414704u;

        [[nodiscard]] std::filesystem::path stagedFontRoot() {
            return hal::process::currentExecutablePath().parent_path() / "fonts";
        }

        [[nodiscard]] UIFontSource interSource(const std::filesystem::path &font_root) {
            return UIFontSource{
                .m_path = font_root / "Inter-Regular.ttf",
                .m_expected_bytes = kInterBytes,
                .m_size_pixels = 16.0f,
                .m_sha256 = sha256DigestFromHex("40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82"),
            };
        }

        /// Icon face merged into the Inter body source above, exactly as the
        /// production Editor list declares it.
        [[nodiscard]] UIFontSource awesomeSource(const std::filesystem::path &font_root) {
            return UIFontSource{
                .m_path = font_root / "Font Awesome 7 Free-Solid-900.otf",
                .m_merge_glyphs = ui::icons::kMergeRanges,
                .m_expected_bytes = kAwesomeBytes,
                .m_size_pixels = 16.0f,
                .m_sha256 = sha256DigestFromHex("daa92774861a14e9ccbccc5e73a5fba514271ba4d1bd7e5adbf952182bc00835"),
                .m_merge_into_previous = true,
            };
        }

        [[nodiscard]] UIFontSource monoSource(const std::filesystem::path &font_root) {
            return UIFontSource{
                .m_path = font_root / "JetBrainsMono-Regular.ttf",
                .m_expected_bytes = kMonoBytes,
                .m_size_pixels = 14.0f,
                .m_sha256 = sha256DigestFromHex("a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f"),
            };
        }

        [[nodiscard]] std::array<UIFontSource, 5u> conceptSources(const std::filesystem::path &font_root) {
            return {
                UIFontSource{
                    .m_path = font_root / "PixelifySans-Regular.ttf",
                    .m_expected_bytes = 93780u,
                    .m_size_pixels = 16.0f,
                    .m_sha256 = sha256DigestFromHex(
                        "5436d485d1713afa376e1744911cbbcc0849eae1eb55ec9bc34c0753993ffef9")},
                awesomeSource(font_root),
                UIFontSource{
                    .m_path = font_root / "PixelifySans-SemiBold.ttf",
                    .m_expected_bytes = 97492u,
                    .m_size_pixels = 16.0f,
                    .m_sha256 = sha256DigestFromHex(
                        "a4b54982991cd47450df317a451a7f066bf6b91aa033e105643aba2cf7bd35c3")},
                UIFontSource{
                    .m_path = font_root / "BigShouldersDisplay-Bold.ttf",
                    .m_expected_bytes = 122896u,
                    .m_size_pixels = 22.0f,
                    .m_sha256 = sha256DigestFromHex(
                        "cbfe03f89e2bb3aba487644cdaf9abc9b3eb59946efaf894dda5cd05e1690c83")},
                // The compact read-out binding: slot 0's pinned bytes again, at
                // the smaller size the production Editor list declares.
                UIFontSource{
                    .m_path = font_root / "PixelifySans-Regular.ttf",
                    .m_expected_bytes = 93780u,
                    .m_size_pixels = 12.0f,
                    .m_sha256 = sha256DigestFromHex(
                        "5436d485d1713afa376e1744911cbbcc0849eae1eb55ec9bc34c0753993ffef9")}};
        }
    } // namespace FontLoading

    // A >100-byte malformed face is a recoverable operational error: the
    // pinned ImGui path logs it (error-recovery mode), rolls the source
    // back, and returns NULL — no abort, and never a silent substitution
    // with the stock default face. The partially initialized service still
    // shuts down cleanly.
    PPR_UNIT_TEST (font_input_malformed_face_rejects_without_fallback) {
        // Deterministic 200-byte fixture: bytes 0x00..0xC7 are not a valid
        // SFNT container, so ImGui's stb source init rejects the face.
        const std::filesystem::path malformed_path = std::filesystem::temp_directory_path() /
                                                     "ppr-m2b-malformed-font-200.bin";
        PPR_DEFER {
            std::error_code ignored{};
            std::ignore = std::filesystem::remove(malformed_path, ignored);
        };
        {
            std::array<std::byte, 200u> malformed{};
            for (std::size_t index = 0u; index < malformed.size(); ++index) {
                malformed[index] = static_cast<std::byte>(index);
            }
            std::ofstream file{malformed_path, std::ios::binary | std::ios::trunc};
            PPR_TEST_ASSERT(file.is_open());
            file.write(
                reinterpret_cast<const char *>(malformed.data()), static_cast<std::streamsize>(malformed.size()));
            file.close();
            PPR_TEST_ASSERT(not file.fail());
        }
        PPR_TEST_ASSERT(std::filesystem::file_size(malformed_path) == 200u);

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

        // Unwind-safe teardown: on a failed PPR_TEST_ASSERT the input context
        // must be detached and the window handle released before
        // ~WindowInputContext and ~Window run, or their hard asserts abort
        // the process during unwinding and mask the failing predicate.
        // Registered before `routed.initialize` and before ui, so it is
        // destroyed after ~ImGuiService but before the context and window.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));

        auto ui = ui::createImGuiService();

        // Unwind-safe teardown: a failed PPR_TEST_ASSERT must surface as the
        // failing predicate, not as ~ImGuiService's live-context assert
        // terminating the process during unwinding (fail-fast).
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        const UIFontSource malformed_source{
            .m_path = malformed_path,
            .m_expected_bytes = 200u,
            .m_size_pixels = 16.0f,
            .m_sha256 = sha256DigestFromHex("1901da1c9f699b48f6b2636e65cbf73abf99d0441ef67f5c540a42f7051dec6f"),
        };
        const std::span<const UIFontSource> sources{&malformed_source, 1u};
        const std::error_code error = ui->initialize(routed, *rhi, *shader, sources, 0);

        PPR_TEST_ASSERT(error == std::errc::invalid_argument);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);

        // Partial init never attached the listener; shutdown unwinds the
        // half-built context and releases the owned bytes.
        PPR_TEST_ASSERT(not routed.m_context.hasInputListener(*ui->getInputListener()));
        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(ui->getContext() == nullptr);
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // Input range limits are checked before the file is opened: an expected
    // byte count at or below the 100-byte floor or above INT_MAX, and a
    // non-positive raster size, are rejected even though the path does not
    // exist — the range gate, not the open failure, produced the error.
    PPR_UNIT_TEST (font_input_size_limits_reject_before_file_access) {
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

        // Unwind-safe teardown: on a failed PPR_TEST_ASSERT the input context
        // must be detached and the window handle released before
        // ~WindowInputContext and ~Window run, or their hard asserts abort
        // the process during unwinding and mask the failing predicate.
        // Registered before `routed.initialize` and before ui, so it is
        // destroyed after ~ImGuiService but before the context and window.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));

        auto ui = ui::createImGuiService();

        // Unwind-safe teardown: a failed PPR_TEST_ASSERT must surface as the
        // failing predicate, not as ~ImGuiService's live-context assert
        // terminating the process during unwinding (fail-fast).
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        const std::filesystem::path absent_path = FontLoading::stagedFontRoot() / "m2b-absent-font.ttf";
        PPR_TEST_ASSERT(not std::filesystem::exists(absent_path));

        // The digest is never consulted for these rejections.
        const UIFontSource too_small{
            .m_path = absent_path, .m_expected_bytes = 100u, .m_size_pixels = 16.0f, .m_sha256 = {}};
        const std::span<const UIFontSource> small_sources{&too_small, 1u};
        const std::error_code small_error = ui->initialize(routed, *rhi, *shader, small_sources, 0);
        PPR_TEST_ASSERT(small_error == std::errc::invalid_argument);
        PPR_TEST_ASSERT(ui->getContext() == nullptr);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);

        const UIFontSource too_large{
            .m_path = absent_path,
            .m_expected_bytes = static_cast<u64>(std::numeric_limits<int>::max()) + 1u,
            .m_size_pixels = 16.0f,
            .m_sha256 = {}};
        const std::span<const UIFontSource> large_sources{&too_large, 1u};
        const std::error_code large_error = ui->initialize(routed, *rhi, *shader, large_sources, 0);
        PPR_TEST_ASSERT(large_error == std::errc::invalid_argument);
        PPR_TEST_ASSERT(ui->getContext() == nullptr);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);

        // A zero raster size is rejected at the range gate, before any file
        // access and before the digest; the declared byte count is valid, so
        // only the size gate can produce this error for the absent path.
        const UIFontSource zero_size{
            .m_path = absent_path, .m_expected_bytes = FontLoading::kInterBytes, .m_size_pixels = 0.0f, .m_sha256 = {}};
        const std::span<const UIFontSource> zero_size_sources{&zero_size, 1u};
        const std::error_code zero_size_error = ui->initialize(routed, *rhi, *shader, zero_size_sources, 0);
        PPR_TEST_ASSERT(zero_size_error == std::errc::invalid_argument);
        PPR_TEST_ASSERT(ui->getContext() == nullptr);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);

        // Negative raster sizes take the same non-positive gate.
        const UIFontSource negative_size{
            .m_path = absent_path,
            .m_expected_bytes = FontLoading::kInterBytes,
            .m_size_pixels = -1.0f,
            .m_sha256 = {}};
        const std::span<const UIFontSource> negative_size_sources{&negative_size, 1u};
        const std::error_code negative_size_error = ui->initialize(routed, *rhi, *shader, negative_size_sources, 0);
        PPR_TEST_ASSERT(negative_size_error == std::errc::invalid_argument);
        PPR_TEST_ASSERT(ui->getContext() == nullptr);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);

        PPR_TEST_ASSERT(not routed.m_context.hasInputListener(*ui->getInputListener()));
        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // A staged artifact whose bytes do not match the pinned digest is
    // rejected before any ImGui state exists: the hash gate, not the atlas
    // builder, produces the error, and no default face is substituted.
    PPR_UNIT_TEST (font_input_hash_mismatch_rejects_staged_file) {
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

        // Unwind-safe teardown: on a failed PPR_TEST_ASSERT the input context
        // must be detached and the window handle released before
        // ~WindowInputContext and ~Window run, or their hard asserts abort
        // the process during unwinding and mask the failing predicate.
        // Registered before `routed.initialize` and before ui, so it is
        // destroyed after ~ImGuiService but before the context and window.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));

        auto ui = ui::createImGuiService();

        // Unwind-safe teardown: a failed PPR_TEST_ASSERT must surface as the
        // failing predicate, not as ~ImGuiService's live-context assert
        // terminating the process during unwinding (fail-fast).
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        const std::filesystem::path font_root = FontLoading::stagedFontRoot();
        const std::filesystem::path inter_path = font_root / "Inter-Regular.ttf";
        PPR_TEST_ASSERT(std::filesystem::exists(inter_path));
        PPR_TEST_ASSERT(std::filesystem::file_size(inter_path) == FontLoading::kInterBytes);

        // Only the leading digest nibble is tampered: size, path, and bytes
        // all still match, so the hash gate alone rejects the source.
        UIFontSource tampered = FontLoading::interSource(font_root);
        tampered.m_sha256 = sha256DigestFromHex("50d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82");
        const std::span<const UIFontSource> sources{&tampered, 1u};
        const std::error_code error = ui->initialize(routed, *rhi, *shader, sources, 0);

        PPR_TEST_ASSERT(error == std::errc::invalid_argument);
        PPR_TEST_ASSERT(ui->getContext() == nullptr);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);

        PPR_TEST_ASSERT(not routed.m_context.hasInputListener(*ui->getInputListener()));
        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // A declared source whose file is absent propagates the engine I/O open
    // error (no size/hash shortcut, no substitution), and the never-started
    // service tears down cleanly.
    PPR_UNIT_TEST (font_input_missing_file_propagates_open_error_and_tears_down) {
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

        // Unwind-safe teardown: on a failed PPR_TEST_ASSERT the input context
        // must be detached and the window handle released before
        // ~WindowInputContext and ~Window run, or their hard asserts abort
        // the process during unwinding and mask the failing predicate.
        // Registered before `routed.initialize` and before ui, so it is
        // destroyed after ~ImGuiService but before the context and window.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));

        auto ui = ui::createImGuiService();

        // Unwind-safe teardown: a failed PPR_TEST_ASSERT must surface as the
        // failing predicate, not as ~ImGuiService's live-context assert
        // terminating the process during unwinding (fail-fast).
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        const std::filesystem::path font_root = FontLoading::stagedFontRoot();
        const std::filesystem::path missing_path = font_root / "Inter-Regular-absent.ttf";
        PPR_TEST_ASSERT(not std::filesystem::exists(missing_path));

        // Correct size and digest: only the file is missing.
        UIFontSource missing = FontLoading::interSource(font_root);
        missing.m_path = missing_path;
        const std::span<const UIFontSource> sources{&missing, 1u};
        const std::error_code error = ui->initialize(routed, *rhi, *shader, sources, 0);

        PPR_TEST_ASSERT(error == std::errc::no_such_file_or_directory);
        PPR_TEST_ASSERT(ui->getContext() == nullptr);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);

        PPR_TEST_ASSERT(not routed.m_context.hasInputListener(*ui->getInputListener()));
        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // A declared byte count that disagrees with the staged file's actual
    // length is rejected in both directions by the exact-size probe:
    // `readExactFontBytes` requests `expected_bytes` and proves the exact
    // size with a one-byte-longer probe (App.UI.ImGui.cpp:435-455), so a file
    // shorter than declared and a file longer than declared both return the
    // size-mismatch error and a truncated or over-long buffer never reaches
    // the parser. Only the declared count is tampered: the path, digest and
    // file are the real staged artifacts, and installedFontVersion() staying
    // 0 with a null context proves no partial install.
    PPR_UNIT_TEST (font_input_declared_length_mismatch_rejects_both_directions) {
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

        // Unwind-safe teardown: on a failed PPR_TEST_ASSERT the input context
        // must be detached and the window handle released before
        // ~WindowInputContext and ~Window run, or their hard asserts abort
        // the process during unwinding and mask the failing predicate.
        // Registered before `routed.initialize` and before ui, so it is
        // destroyed after ~ImGuiService but before the context and window.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));

        auto ui = ui::createImGuiService();

        // Unwind-safe teardown: a failed PPR_TEST_ASSERT must surface as the
        // failing predicate, not as ~ImGuiService's live-context assert
        // terminating the process during unwinding (fail-fast).
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        const std::filesystem::path font_root = FontLoading::stagedFontRoot();
        const std::filesystem::path inter_path = font_root / "Inter-Regular.ttf";
        PPR_TEST_ASSERT(std::filesystem::exists(inter_path));
        PPR_TEST_ASSERT(std::filesystem::file_size(inter_path) == FontLoading::kInterBytes);

        // File shorter than declared: the probe read transfers fewer bytes
        // than the declaration.
        UIFontSource file_shorter = FontLoading::interSource(font_root);
        file_shorter.m_expected_bytes = FontLoading::kInterBytes + 1u;
        const std::span<const UIFontSource> shorter_sources{&file_shorter, 1u};
        const std::error_code shorter_error = ui->initialize(routed, *rhi, *shader, shorter_sources, 0);
        PPR_TEST_ASSERT(shorter_error == std::errc::invalid_argument);
        PPR_TEST_ASSERT(ui->getContext() == nullptr);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);

        // File longer than declared: the probe reads one byte past the
        // declaration and transfers more than declared.
        UIFontSource file_longer = FontLoading::interSource(font_root);
        file_longer.m_expected_bytes = FontLoading::kInterBytes - 1u;
        const std::span<const UIFontSource> longer_sources{&file_longer, 1u};
        const std::error_code longer_error = ui->initialize(routed, *rhi, *shader, longer_sources, 0);
        PPR_TEST_ASSERT(longer_error == std::errc::invalid_argument);
        PPR_TEST_ASSERT(ui->getContext() == nullptr);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);

        PPR_TEST_ASSERT(not routed.m_context.hasInputListener(*ui->getInputListener()));
        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // Density is the single bake knob: the clamped framebuffer ratio drives
    // ImFont::CurrentRasterizerDensity, the 1.0 -> 2.0 step grows the packed
    // atlas and the glyph raster, the trip back to 1.0 restores the
    // density-1 raster, and a repeated 2.0 bake reproduces the same atlas.
    PPR_UNIT_TEST (font_density_rebake_grows_atlas_and_stays_stable) {
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

        // Unwind-safe teardown: on a failed PPR_TEST_ASSERT the input context
        // must be detached and the window handle released before
        // ~WindowInputContext and ~Window run, or their hard asserts abort
        // the process during unwinding and mask the failing predicate.
        // Registered before `routed.initialize` and before viewport/ui, so an
        // initialize failure is covered and the guard is destroyed after
        // viewport/ui but before ~WindowInputContext and ~Window.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
        WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
        std::ignore = viewport.updateFromWindow();
        const TimeSpan frame{std::chrono::milliseconds{16}};

        auto ui = ui::createImGuiService();
        // Unwind-safe teardown: a failed PPR_TEST_ASSERT must surface as the
        // failing predicate, not as ~ImGuiService's live-context assert
        // terminating the process during unwinding (fail-fast).
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        const std::filesystem::path font_root = FontLoading::stagedFontRoot();
        const std::array sources{FontLoading::interSource(font_root), FontLoading::monoSource(font_root)};
        PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, sources, 0));
        PPR_TEST_ASSERT(ui->installedFontVersion() == 1u);

        auto *const imgui_context = static_cast<ImGuiContext *>(ui->getContext());
        PPR_TEST_ASSERT(imgui_context != nullptr);
        ImGui::SetCurrentContext(imgui_context);
        ImGuiIO &io = ImGui::GetIO();
        PPR_TEST_ASSERT(io.Fonts->Fonts.Size == 2);

        // Atlas extent is read directly (the published-extent bookkeeping is
        // a different contract); the glyph metric is Inter's 'A' logical box
        // (the pinned stb loader stores X0/Y0/X1/Y1 normalized by the raster
        // density, imgui_draw.cpp:4846-4855) and ink is the count of
        // alpha-nonzero texels, the density proof independent of TexData
        // dimensions.
        const auto atlasExtent = [&]() {
            u8 *pixels = nullptr;
            int width = 0;
            int height = 0;
            int bpp = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);
            PPR_TEST_ASSERT(pixels != nullptr and width > 0 and height > 0);
            return int2{width, height};
        };
        const auto glyphExtentY = [&]() {
            ImFont *const body = io.Fonts->Fonts[0];
            ImFontBaked *const baked = body->GetFontBaked(body->LegacySize);
            PPR_TEST_ASSERT(baked != nullptr);
            ImFontGlyph *const glyph = baked->FindGlyph('A');
            PPR_TEST_ASSERT(glyph != nullptr);
            return glyph->Y1 - glyph->Y0;
        };

        // The live bake's density is the unit-level proof that the engine
        // knob reached the rasterizer: pinned GetFontBaked returns a bake
        // only when its RasterizerDensity matches the requested density
        // (imgui_draw.cpp:5440-5447), so a stale density-1 bake is excluded.
        const auto bakeDensity = [&]() {
            ImFont *const body = io.Fonts->Fonts[0];
            ImFontBaked *const baked = body->GetFontBaked(body->LegacySize);
            PPR_TEST_ASSERT(baked != nullptr);
            return baked->RasterizerDensity;
        };

        // Ink texels are IM_COL32(255,255,255,coverage) where glyph stamps
        // were converted from Alpha8 and zero elsewhere (imgui_draw.cpp:2511,
        // :2903-2912), so alpha nonzero is exactly "ink".
        const auto atlasInkPixelCount = [&]() {
            u8 *pixels = nullptr;
            int width = 0;
            int height = 0;
            int bpp = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);
            PPR_TEST_ASSERT(pixels != nullptr and width > 0 and height > 0 and bpp == 4);

            u64 ink = 0u;
            for (int index = 0; index < width * height; ++index) {
                ink += (pixels[index * 4 + 3] != 0u) ? 1u : 0u;
            }

            return ink;
        };
        // Completes the open frame; the atlas is unlocked only afterwards.
        DetachedUiDrawData frame_out{};

        // Baseline held by initialization itself (density 1.0, no frame yet).
        const int2 base_extent = atlasExtent();
        const float base_glyph_height = glyphExtentY();
        const float base_bake_density = bakeDensity();
        const u64 base_ink = atlasInkPixelCount();

        PPR_TEST_ASSERT(base_glyph_height > 0.0f);
        PPR_TEST_ASSERT(base_bake_density == 1.0f);
        PPR_TEST_ASSERT(base_ink > 0u);

        // 2x: the next update rebakes at density 2.0 before NewFrame.
        window.m_content_scale = float2{2.0f, 2.0f};
        window.m_framebuffer_size = int2{1600, 1200};
        const std::error_code update_2x = ui->update(frame, viewport);
        const std::error_code build_2x = ui->buildDetachedUiFrame(frame_out);
        const int2 doubled_extent = atlasExtent();
        const float doubled_glyph_height = glyphExtentY();
        const float doubled_bake_density = bakeDensity();
        const u64 doubled_ink = atlasInkPixelCount();

        PPR_TEST_ASSERT(not update_2x);
        PPR_TEST_ASSERT(not build_2x);
        // Extent checks are parameter-specific: the estimator sizes from the
        // previous build's stats and a fresh builder doubles the smaller axis
        // (512x256 -> 512x512 for this fixture), so they pin this trip, not a
        // general invariant (m2b-changes.md).
        PPR_TEST_ASSERT(doubled_extent.x >= base_extent.x and doubled_extent.y >= base_extent.y);
        PPR_TEST_ASSERT(doubled_extent.x * doubled_extent.y >= 2 * base_extent.x * base_extent.y);
        PPR_TEST_ASSERT(doubled_extent.x >= 2 * base_extent.x or doubled_extent.y >= 2 * base_extent.y);
        // The bake itself must carry exactly the clamped ratio: the engine
        // sets CurrentRasterizerDensity to clamp(max(1600/800, 1200/600)),
        // an exact 2.0f, and GetFontBaked only returns a bake whose
        // RasterizerDensity matches the requested density, so a stale or
        // unrebaked atlas fails here (imgui_draw.cpp:5440-5447).
        PPR_TEST_ASSERT(doubled_bake_density == 2.0f);
        // Logical-box contract: density sharpens the raster but must not move
        // the layout box (imgui_draw.cpp:4846-4855 divides the raster box by
        // rasterizer_density), so the 'A' box stays put within rounding.
        PPR_TEST_ASSERT(doubled_glyph_height > 0.9f * base_glyph_height);
        PPR_TEST_ASSERT(doubled_glyph_height < 1.1f * base_glyph_height);
        // Sharper raster, proven by ink area independent of TexData
        // dimensions: ~4x at 2x density (quadratic), bounded below 6x so a
        // doubly applied density (config x engine RasterizerDensity, ~16x)
        // still fails.
        PPR_TEST_ASSERT(static_cast<float>(doubled_ink) >= 1.5f * static_cast<float>(base_ink));
        PPR_TEST_ASSERT(static_cast<float>(doubled_ink) < 6.0f * static_cast<float>(base_ink));

        // Back to 1x: the glyph raster returns to the density-1 metrics.
        // Atlas dimensions are estimate-sized across trips and stay
        // unasserted here beyond validity.
        window.m_content_scale = float2{1.0f, 1.0f};
        window.m_framebuffer_size = int2{800, 600};
        const std::error_code update_1x = ui->update(frame, viewport);
        const std::error_code build_1x = ui->buildDetachedUiFrame(frame_out);
        const int2 restored_extent = atlasExtent();
        const float restored_glyph_height = glyphExtentY();
        const float restored_bake_density = bakeDensity();
        const u64 restored_ink = atlasInkPixelCount();

        PPR_TEST_ASSERT(not update_1x);
        PPR_TEST_ASSERT(not build_1x);
        PPR_TEST_ASSERT(restored_extent.x > 0 and restored_extent.y > 0);
        PPR_TEST_ASSERT(restored_bake_density == 1.0f);
        PPR_TEST_ASSERT(restored_glyph_height > 0.75f * base_glyph_height);
        PPR_TEST_ASSERT(restored_glyph_height < 1.25f * base_glyph_height);
        // The ink area comes back down to the density-1 baseline: the sharper
        // raster was a density effect, not a one-way content change.
        PPR_TEST_ASSERT(static_cast<float>(restored_ink) >= 0.75f * static_cast<float>(base_ink));
        PPR_TEST_ASSERT(static_cast<float>(restored_ink) <= 1.25f * static_cast<float>(base_ink));

        // Repeated 2x bake: same density and bytes bake identically.
        window.m_content_scale = float2{2.0f, 2.0f};
        window.m_framebuffer_size = int2{1600, 1200};
        const std::error_code update_2x_repeat = ui->update(frame, viewport);
        const std::error_code build_2x_repeat = ui->buildDetachedUiFrame(frame_out);
        const int2 repeated_extent = atlasExtent();
        const float repeated_glyph_height = glyphExtentY();
        const float repeated_bake_density = bakeDensity();
        const u64 repeated_ink = atlasInkPixelCount();

        PPR_TEST_ASSERT(not update_2x_repeat);
        PPR_TEST_ASSERT(not build_2x_repeat);
        PPR_TEST_ASSERT(repeated_bake_density == 2.0f);
        PPR_TEST_ASSERT(repeated_extent.x == doubled_extent.x and repeated_extent.y == doubled_extent.y);
        PPR_TEST_ASSERT(repeated_glyph_height == doubled_glyph_height);
        PPR_TEST_ASSERT(repeated_ink == doubled_ink);

        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // ------------------------------------------------------------------
    // monitor scale independent of framebuffer coordinates
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (content_scale_changes_physical_size_without_cumulative_geometry_scaling) {
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
            WindowModel{.m_window_size = int2{800, 600}}};
        window.m_framebuffer_size = int2{800, 600};
        WindowInputContext routed{safe_ptr<IInputService>{&inputs}};
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };
        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
        WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
        std::ignore = viewport.updateFromWindow();

        auto service = ui::createImGuiService();
        PPR_DEFER {
            if (service) {
                std::ignore = service->shutdown();
            }
        };
        const auto font_root = FontLoading::stagedFontRoot();
        const auto fonts = FontLoading::conceptSources(font_root);
        PPR_TEST_ASSERT(not service->initialize(routed, *rhi, *shader, fonts, 0));

        struct Sample {
            u64 m_publication{};
            float m_font_size{};
            float m_padding{};
            float m_density{};
            float m_ratio_x{};
            float m_ratio_y{};
            float m_window_border{};
            float m_child_border{};
            float m_popup_border{};
            float m_heading_size{};
            float m_emphasis_size{};
        };
        const TimeSpan frame{std::chrono::milliseconds{16}};
        const auto sample = [&](const float content, const int2 framebuffer) {
            window.m_content_scale = float2{content, content};
            window.m_framebuffer_size = framebuffer;
            PPR_TEST_ASSERT(not service->update(frame, viewport));
            ImGui::SetCurrentContext(static_cast<ImGuiContext *>(service->getContext()));
            ImGui::Begin("Scale sample");
            ImGui::TextUnformatted("Readable controls");

            ImFont *const heading = ui::theme::font(ui::theme::EFont::heading);
            ImGui::PushFont(heading, heading->LegacySize);
            const float heading_size = ImGui::GetFontSize();
            ImGui::TextUnformatted("HEADING");
            ImGui::PopFont();

            ImFont *const emphasis = ui::theme::font(ui::theme::EFont::emphasis);
            ImGui::PushFont(emphasis, emphasis->LegacySize);
            const float emphasis_size = ImGui::GetFontSize();
            ImGui::TextUnformatted("Emphasis");
            ImGui::PopFont();
            ImGui::End();

            const ImGuiIO &io = ImGui::GetIO();
            const ImFont *const font = io.Fonts->Fonts[0];
            Sample out{
                .m_font_size = ImGui::GetFontSize(),
                .m_padding = ImGui::GetStyle().FramePadding.x,
                .m_density = font->CurrentRasterizerDensity,
                .m_ratio_x = io.DisplayFramebufferScale.x,
                .m_ratio_y = io.DisplayFramebufferScale.y,
                .m_window_border = ImGui::GetStyle().WindowBorderSize,
                .m_child_border = ImGui::GetStyle().ChildBorderSize,
                .m_popup_border = ImGui::GetStyle().PopupBorderSize,
                .m_heading_size = heading_size,
                .m_emphasis_size = emphasis_size};
            DetachedUiDrawData detached{};
            PPR_TEST_ASSERT(not service->buildDetachedUiFrame(detached));
            out.m_publication = detached.m_font_version;
            return out;
        };

        const Sample base = sample(1.0f, int2{800, 600});
        PPR_TEST_ASSERT(base.m_window_border == 3.0f and base.m_popup_border == 3.0f and base.m_child_border == 2.0f);
        PPR_TEST_ASSERT(base.m_heading_size == 22.0f and base.m_emphasis_size == base.m_font_size);
        PPR_TEST_ASSERT(base.m_font_size > 0.0f and base.m_padding > 0.0f);
        for (const float content: {1.25f, 1.5f, 2.0f, 1.0f}) {
            const Sample current = sample(content, int2{800, 600});
            PPR_TEST_ASSERT(std::abs(current.m_font_size - base.m_font_size * content) < 0.1f);
            PPR_TEST_ASSERT(std::abs(current.m_padding - base.m_padding * content) < 0.1f);
            // Pinned ImGui rounds both PushFont and legacy preload to whole logical pixels.
            PPR_TEST_ASSERT(current.m_heading_size == std::round(base.m_heading_size * content));
            PPR_TEST_ASSERT(current.m_emphasis_size == current.m_font_size);
            PPR_TEST_ASSERT(current.m_window_border == std::trunc(3.0f * content));
            PPR_TEST_ASSERT(current.m_child_border == std::trunc(2.0f * content));
            PPR_TEST_ASSERT(current.m_popup_border == current.m_window_border);
            PPR_TEST_ASSERT(current.m_density == 1.0f);
            PPR_TEST_ASSERT(current.m_ratio_x == 1.0f and current.m_ratio_y == 1.0f);
        }

        const Sample two = sample(2.0f, int2{800, 600});
        const Sample steady = sample(2.0f, int2{800, 600});
        PPR_TEST_ASSERT(steady.m_publication == two.m_publication);
        PPR_TEST_ASSERT(steady.m_window_border == two.m_window_border and steady.m_child_border == two.m_child_border);
        PPR_TEST_ASSERT(steady.m_font_size == two.m_font_size and steady.m_padding == two.m_padding);

        const Sample mixed = sample(2.0f, int2{1600, 1200});
        PPR_TEST_ASSERT(std::abs(mixed.m_font_size * mixed.m_ratio_x - base.m_font_size * 2.0f) < 0.1f);
        PPR_TEST_ASSERT(mixed.m_padding == base.m_padding and mixed.m_density == 2.0f);

        const Sample high_ratio = sample(1.0f, int2{1600, 1200});
        PPR_TEST_ASSERT(std::abs(high_ratio.m_font_size * high_ratio.m_ratio_x - base.m_font_size) < 0.1f);
        PPR_TEST_ASSERT(high_ratio.m_padding == base.m_padding * 0.5f and high_ratio.m_density == 2.0f);

        const Sample asymmetric = sample(2.0f, int2{1600, 900});
        PPR_TEST_ASSERT(asymmetric.m_ratio_x == 2.0f and asymmetric.m_ratio_y == 1.5f);
        PPR_TEST_ASSERT(asymmetric.m_font_size == base.m_font_size and asymmetric.m_density == 2.0f);

        PPR_TEST_ASSERT(not service->shutdown());
        service.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // ------------------------------------------------------------------
    // icon vocabulary and merged icon bake
    // ------------------------------------------------------------------


    // The icon vocabulary is the single seam the merged atlas and every
    // surface label share: each label must start with a codepoint the merge
    // spans bake, carry readable words besides the icon, and keep the widget
    // ID it had before the icon — the "###" id part must hash identically to
    // the whole label under the pinned ImHashStr skip (imgui.cpp:2594-2601).
    PPR_UNIT_TEST (icon_vocabulary_labels_keep_ids_and_cover_merge_ranges) {
        struct DecodedScalar {
            char32_t m_codepoint{0u};
            std::size_t m_bytes{0u};
        };

        // Leading UTF-8 scalar decoded by its format byte, so a label whose
        // literal lost its multi-byte encoding fails loudly here.
        const auto decodeLeading = [](const std::string_view label) {
            PPR_TEST_ASSERT(not label.empty());
            const auto byte = [&](const std::size_t index) {
                PPR_TEST_ASSERT(index < label.size());
                return static_cast<unsigned>(static_cast<unsigned char>(label[index]));
            };

            const unsigned first = byte(0u);
            if ((first & 0xF8u) == 0xF0u) {
                return DecodedScalar{
                    static_cast<char32_t>(
                        ((first & 0x07u) << 18u) |
                        ((byte(1u) & 0x3Fu) << 12u) |
                        ((byte(2u) & 0x3Fu) << 6u) |
                        (byte(3u) & 0x3Fu)),
                    4u};
            }

            if ((first & 0xF0u) == 0xE0u) {
                return DecodedScalar{
                    static_cast<char32_t>(((first & 0x0Fu) << 12u) | ((byte(1u) & 0x3Fu) << 6u) | (byte(2u) & 0x3Fu)),
                    3u};
            }

            if ((first & 0xE0u) == 0xC0u) {
                return DecodedScalar{static_cast<char32_t>(((first & 0x1Fu) << 6u) | (byte(1u) & 0x3Fu)), 2u};
            }

            return DecodedScalar{static_cast<char32_t>(first), 1u};
        };

        const auto coveredByRanges = [](const char32_t codepoint) {
            return std::ranges::any_of(ui::icons::kMergeRanges, [codepoint](const UIFontGlyphRange &range) {
                return codepoint >= range.m_first and codepoint <= range.m_last;
            });
        };

        for (const std::string_view label: ui::icons::kLabels) {
            const DecodedScalar icon = decodeLeading(label);
            PPR_TEST_ASSERT(coveredByRanges(icon.m_codepoint));

            const auto separator = label.find("###");
            PPR_TEST_ASSERT(separator != std::string_view::npos);
            PPR_TEST_ASSERT(separator >= icon.m_bytes);

            // The words between the icon and the id carry the meaning: the
            // icon is never the sole carrier.
            const std::string_view words = label.substr(icon.m_bytes, separator - icon.m_bytes);
            PPR_TEST_ASSERT(words.find_first_not_of(" \t") != std::string_view::npos);

            const std::string_view id = label.substr(separator + 3u);
            PPR_TEST_ASSERT(not id.empty());
            // IDs stay plain ASCII even though the display label carries a
            // non-ASCII icon.
            PPR_TEST_ASSERT(
                std::ranges::none_of(id, [](const char c) { return static_cast<unsigned char>(c) >= 0x80u; }));

            // The pinned hash resets at "###" and skips the triple
            // (imgui.cpp:2594-2601), so the label hashes as its id alone and
            // widget IDs do not move when the icon lands.
            const std::string id_copy{id};
            PPR_TEST_ASSERT(ImHashStr(label.data()) == ImHashStr(id_copy.c_str()));
        }

        // The baked span union equals the codepoints the labels use: every
        // span is reachable from some label, so no range is dead weight.
        for (const UIFontGlyphRange &range: ui::icons::kMergeRanges) {
            PPR_TEST_ASSERT(range.m_first <= range.m_last);
            PPR_TEST_ASSERT(range.m_last <= 0xFFFFu);
            const bool used = std::ranges::any_of(ui::icons::kLabels, [&](const std::string_view label) {
                const char32_t icon = decodeLeading(label).m_codepoint;
                return icon >= range.m_first and icon <= range.m_last;
            });
            PPR_TEST_ASSERT(used);
        }

        // The icon-only help marker is the same baked codepoint as the Help
        // button label, minus its words, and is exactly one icon: it carries
        // no "###" id part, so it stays out of the label list above.
        const DecodedScalar marker = decodeLeading(ui::icons::kHelpMarkerGlyph);
        PPR_TEST_ASSERT(marker.m_codepoint == ui::icons::kHelp);
        PPR_TEST_ASSERT(marker.m_bytes == ui::icons::kHelpMarkerGlyph.size());
    };

    // Merge declarations the atlas could not honor are rejected before any
    // file is opened: a merge as the first source, a merge declaring no
    // spans, spans on a standalone face, an empty span, a span beyond the
    // 16-bit ImWchar codepoint space, and a span starting at codepoint 0
    // (the range list's terminator). Every rejected source sits on an absent
    // path with an otherwise valid size and byte count, so the declaration
    // gate — not the file probe — produces the error.
    PPR_UNIT_TEST (font_merge_declaration_rejections_precede_file_access) {
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

        // Unwind-safe teardown: on a failed PPR_TEST_ASSERT the input context
        // must be detached and the window handle released before
        // ~WindowInputContext and ~Window run, or their hard asserts abort
        // the process during unwinding and mask the failing predicate.
        // Registered before `routed.initialize` and before ui, so it is
        // destroyed after ~ImGuiService but before the context and window.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));

        auto ui = ui::createImGuiService();

        // Unwind-safe teardown: a failed PPR_TEST_ASSERT must surface as the
        // failing predicate, not as ~ImGuiService's live-context assert
        // terminating the process during unwinding (fail-fast).
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        const std::filesystem::path font_root = FontLoading::stagedFontRoot();
        const std::filesystem::path absent_path = font_root / "m3-absent-merge-source.otf";
        PPR_TEST_ASSERT(not std::filesystem::exists(absent_path));
        const auto rejected = [&](const std::span<const UIFontSource> sources) {
            const std::error_code error = ui->initialize(routed, *rhi, *shader, sources, 0);
            PPR_TEST_ASSERT(error == std::errc::invalid_argument);
            PPR_TEST_ASSERT(ui->getContext() == nullptr);
            PPR_TEST_ASSERT(ui->installedFontVersion() == 0u);
        };

        // A merge as the first source has no destination face.
        UIFontSource leading = FontLoading::awesomeSource(font_root);
        leading.m_path = absent_path;
        const std::span<const UIFontSource> leading_sources{&leading, 1u};
        rejected(leading_sources);

        // A merge with no declared spans contributes nothing.
        UIFontSource no_spans = FontLoading::awesomeSource(font_root);
        no_spans.m_path = absent_path;
        no_spans.m_merge_glyphs = {};
        const std::array<UIFontSource, 2u> no_spans_sources{FontLoading::interSource(font_root), no_spans};
        rejected(no_spans_sources);

        // A standalone face must not carry spans: they only mean something
        // to a merged source.
        UIFontSource base_with_spans = FontLoading::interSource(font_root);
        base_with_spans.m_path = absent_path;
        base_with_spans.m_merge_glyphs = ui::icons::kMergeRanges;
        const std::span<const UIFontSource> base_span_sources{&base_with_spans, 1u};
        rejected(base_span_sources);

        // An inverted span is empty in the ImWchar space.
        constexpr std::array<UIFontGlyphRange, 1u> inverted_range{
            UIFontGlyphRange{.m_first = 0xF021u, .m_last = 0xF020u}};
        UIFontSource inverted = FontLoading::awesomeSource(font_root);
        inverted.m_path = absent_path;
        inverted.m_merge_glyphs = inverted_range;
        const std::array<UIFontSource, 2u> inverted_sources{FontLoading::interSource(font_root), inverted};
        rejected(inverted_sources);

        // A span past 0xFFFF cannot exist in the 16-bit ImWchar atlas.
        constexpr std::array<UIFontGlyphRange, 1u> beyond_range{
            UIFontGlyphRange{.m_first = 0xFFFEu, .m_last = 0x10000u}};
        UIFontSource beyond = FontLoading::awesomeSource(font_root);
        beyond.m_path = absent_path;
        beyond.m_merge_glyphs = beyond_range;
        const std::array<UIFontSource, 2u> beyond_sources{FontLoading::interSource(font_root), beyond};
        rejected(beyond_sources);

        // Codepoint 0 terminates the range array in the pinned legacy preload
        // (imgui_draw.cpp:3556 iterates while ranges[0]); accepting a span
        // starting there would silently truncate the baked list from that
        // span onward.
        constexpr std::array<UIFontGlyphRange, 1u> zero_start_range{UIFontGlyphRange{.m_first = 0u, .m_last = 0x20u}};
        UIFontSource zero_start = FontLoading::awesomeSource(font_root);
        zero_start.m_path = absent_path;
        zero_start.m_merge_glyphs = zero_start_range;
        const std::array<UIFontSource, 2u> zero_start_sources{FontLoading::interSource(font_root), zero_start};
        rejected(zero_start_sources);

        PPR_TEST_ASSERT(not routed.m_context.hasInputListener(*ui->getInputListener()));
        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // The production composition [Inter, Awesome merge, Mono] bakes two
    // faces: the merge source adds no face of its own and attaches its
    // declared spans to the Inter source list, so every referenced icon
    // resolves with SourceIdx == 1 while the body face's own glyphs stay on
    // source 0. Whether the pinned preload baked the icons or
    // FindGlyphNoFallback loaded them lazily is deliberately not
    // distinguished: the atlas Locked guard is commented out in the pinned
    // build (imgui_draw.cpp:2750), so the paths are indistinguishable here
    // (m3-changes.md).
    PPR_UNIT_TEST (font_merge_bakes_icon_face_into_body_source) {
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

        // Unwind-safe teardown: on a failed PPR_TEST_ASSERT the input context
        // must be detached and the window handle released before
        // ~WindowInputContext and ~Window run, or their hard asserts abort
        // the process during unwinding and mask the failing predicate.
        // Registered before `routed.initialize` and before ui, so it is
        // destroyed after ~ImGuiService but before the context and window.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));

        auto ui = ui::createImGuiService();

        // Unwind-safe teardown: a failed PPR_TEST_ASSERT must surface as the
        // failing predicate, not as ~ImGuiService's live-context assert
        // terminating the process during unwinding (fail-fast).
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        const std::filesystem::path font_root = FontLoading::stagedFontRoot();
        const std::filesystem::path awesome_path = font_root / "Font Awesome 7 Free-Solid-900.otf";
        PPR_TEST_ASSERT(std::filesystem::exists(awesome_path));
        PPR_TEST_ASSERT(std::filesystem::file_size(awesome_path) == FontLoading::kAwesomeBytes);

        const auto sources = FontLoading::conceptSources(font_root);
        PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, sources, 0));
        PPR_TEST_ASSERT(ui->installedFontVersion() == 1u);

        auto *const imgui_context = static_cast<ImGuiContext *>(ui->getContext());
        PPR_TEST_ASSERT(imgui_context != nullptr);
        ImGui::SetCurrentContext(imgui_context);
        ImGuiIO &io = ImGui::GetIO();

        // The merge adds glyphs to the Pixelify body face, never a face of its own.
        PPR_TEST_ASSERT(io.Fonts->Fonts.Size == 4);
        ImFont *const body = ui::theme::font(ui::theme::EFont::body);
        PPR_TEST_ASSERT(body == io.Fonts->Fonts[0]);
        PPR_TEST_ASSERT(ui::theme::font(ui::theme::EFont::emphasis) == io.Fonts->Fonts[1]);
        PPR_TEST_ASSERT(ui::theme::font(ui::theme::EFont::heading) == io.Fonts->Fonts[2]);
        PPR_TEST_ASSERT(ui::theme::font(ui::theme::EFont::tiny) == io.Fonts->Fonts[3]);
        PPR_TEST_ASSERT(body->LegacySize == 16.0f and io.Fonts->Fonts[2]->LegacySize == 22.0f);
        PPR_TEST_ASSERT(io.Fonts->Fonts[3]->LegacySize == 12.0f);

        // The compact binding is a real second bake of the body file rather
        // than an empty slot: it resolves its own Latin glyphs at 12 px, and
        // carries no icon merge, so icon-bearing text must stay on the body
        // face.
        ImFontBaked *const tiny_baked = io.Fonts->Fonts[3]->GetFontBaked(io.Fonts->Fonts[3]->LegacySize);
        PPR_TEST_ASSERT(tiny_baked != nullptr);
        ImFontGlyph *const tiny_letter = tiny_baked->FindGlyphNoFallback(static_cast<ImWchar>('A'));
        PPR_TEST_ASSERT(tiny_letter != nullptr and tiny_letter->AdvanceX > 0.0f);
        PPR_TEST_ASSERT(tiny_baked->FindGlyphNoFallback(static_cast<ImWchar>(ui::icons::kHelp)) == nullptr);

        const auto bakedGlyph = [&](const char32_t codepoint) {
            ImFontBaked *const baked = body->GetFontBaked(body->LegacySize);
            PPR_TEST_ASSERT(baked != nullptr);
            return baked->FindGlyphNoFallback(static_cast<ImWchar>(codepoint));
        };

        // Each referenced icon resolves to a baked, advancing glyph whose
        // source is the merge face (source 1 of the Inter font), not the
        // text face.
        for (const UIFontGlyphRange &range: ui::icons::kMergeRanges) {
            for (char32_t codepoint = range.m_first; codepoint <= range.m_last; ++codepoint) {
                ImFontGlyph *const glyph = bakedGlyph(codepoint);
                PPR_TEST_ASSERT(glyph != nullptr);
                PPR_TEST_ASSERT(glyph->AdvanceX > 0.0f);
                PPR_TEST_ASSERT(glyph->SourceIdx == 1);
            }
        }

        // The text face is intact underneath the merge: a Latin letter still
        // resolves, from source 0.
        ImFontGlyph *const letter = bakedGlyph(static_cast<char32_t>('A'));
        PPR_TEST_ASSERT(letter != nullptr);
        PPR_TEST_ASSERT(letter->AdvanceX > 0.0f);
        PPR_TEST_ASSERT(letter->SourceIdx == 0);

        // The first native frame retires bake-growth predecessor textures before
        // measuring whether an enlarged symbol changes the frozen static atlas.
        io.DisplaySize = ImVec2{320.0f, 240.0f};
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::Render();

        unsigned char *before_pixels{};
        int before_width{}, before_height{};
        io.Fonts->GetTexDataAsRGBA32(&before_pixels, &before_width, &before_height);
        const Array<u8> original_pixels{
            before_pixels,
            before_pixels + static_cast<std::size_t>(before_width) * static_cast<std::size_t>(before_height) * 4u};
        const int texture_count = io.Fonts->TexList.Size;
        const auto *const atlas_texture = io.Fonts->TexData;

        ImGui::NewFrame();
        ImGui::Begin("Symbolic asset thumbnail");
        const ImVec4 clip{0.0f, 0.0f, 320.0f, 240.0f};
        ImGui::GetWindowDrawList()->AddText(
            body, body->LegacySize * 2.5f, ImVec2{30.0f, 30.0f}, 0xffffffffu, ui::icons::kCubesLabel.data(),
            ui::icons::kCubesLabel.data() + 3u, 0.0f, &clip);
        ImGui::End();
        ImGui::Render();

        unsigned char *after_pixels{};
        int after_width{}, after_height{};
        io.Fonts->GetTexDataAsRGBA32(&after_pixels, &after_width, &after_height);
        PPR_TEST_ASSERT(before_width == after_width and before_height == after_height);
        PPR_TEST_ASSERT(io.Fonts->TexList.Size == texture_count);
        PPR_TEST_ASSERT(io.Fonts->TexData == atlas_texture);
        PPR_TEST_ASSERT(ui->installedFontVersion() == 1u);
        PPR_TEST_ASSERT(std::equal(original_pixels.begin(), original_pixels.end(), after_pixels));

        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
        PPR_TEST_ASSERT(not routed.shutdown());
        std::ignore = window.release();
    };

    // ------------------------------------------------------------------
    // rasterizer comparison harness
    // ------------------------------------------------------------------

    // Objective input for the stb-vs-FreeType decision: bake the production
    // font list (Inter + merged Font Awesome + JetBrains Mono) at density 1
    // and 2 through the real engine path, then record the atlas bytes and
    // glyph metrics under <exe_dir>/m4-font-rasterizer/. The default build
    // records the stb_truetype baseline; the `msvc-dev-freetype` preset
    // (PPR_IMGUI_FREETYPE=ON) records the FreeType rows the two builds are
    // compared with. The harness never asserts that one loader "looks
    // better": it proves which loader actually linked in (runtime
    // FontLoaderName cross-checked against the compile definition), that a
    // repeated bake is byte-identical, and that FreeType loader flags
    // provably reach the rasterizer.
    namespace FontRasterizer {
        // FreeType loader flag values mirrored from imgui_freetype.h:31-40
        // (the module binding exports only imgui.h, so the enum is not
        // importable here). The flag-plumbing proof below fails if these
        // values ever drift from the pinned tree.
        inline constexpr unsigned kFreeTypeLightHinting = 1u << 3u;
        inline constexpr unsigned kFreeTypeForceAutoHint = 1u << 2u;

        enum class Variant : std::size_t {
            Default = 0u,
            LightHinting = 1u,
            ForceAutoHint = 2u,
        };
        inline constexpr std::size_t kVariantCount = 3u;

        struct VariantSpec final {
            std::string_view m_name{};
            unsigned m_flags{0u};
        };

        // Default hinting first: the pinned integration README's recommended
        // starting point, with the two documented alternatives to compare
        // against. Only the FreeType build runs the non-default rows.
        inline constexpr std::array<VariantSpec, kVariantCount> kVariants{
            VariantSpec{.m_name = "default", .m_flags = 0u},
            VariantSpec{.m_name = "light_hinting", .m_flags = kFreeTypeLightHinting},
            VariantSpec{.m_name = "force_auto_hint", .m_flags = kFreeTypeForceAutoHint},
        };

        // Atlas-wide raster statistics. `m_ink` counts alpha-nonzero texels
        // and `m_partial` counts anti-aliased edge texels (0 < a < 255), so
        // the pair separates "how much is drawn" from "how much is soft";
        // `m_histogram` bins the nonzero alpha values in 16 steps of 16.
        // `m_fnv1a` hashes the raw RGBA bytes: equal hashes mean
        // byte-identical bakes (the determinism proof), never equal quality.
        struct AtlasMetrics final {
            std::array<u64, 16u> m_histogram{};
            u64 m_texels{0u};
            u64 m_ink{0u};
            u64 m_partial{0u};
            u64 m_coverage{0u};
            u64 m_fnv1a{0u};
            int2 m_extent{};
        };

        [[nodiscard]] u64 fnv1aOf(const std::span<const u8> bytes) noexcept {
            u64 hash = 0xcbf29ce484222325ull;
            for (const u8 byte: bytes) {
                hash ^= byte;
                hash *= 0x100000001b3ull;
            }

            return hash;
        }

        // The pinned atlas stores glyph coverage in the alpha channel with
        // white RGB (imgui_draw.cpp:2511, :2903-2912), so alpha is the raster.
        [[nodiscard]] AtlasMetrics measureAtlas(const int width, const int height, const u8 *const pixels) noexcept {
            AtlasMetrics metrics{};
            metrics.m_extent = int2{width, height};
            metrics.m_texels = static_cast<u64>(width) * static_cast<u64>(height);

            for (int index = 0; index < width * height; ++index) {
                const u8 alpha = pixels[static_cast<std::size_t>(index) * 4u + 3u];
                metrics.m_coverage += alpha;
                if (alpha != 0u) {
                    ++metrics.m_ink;
                    metrics.m_histogram[static_cast<std::size_t>(alpha >> 4u)] += 1u;
                    if (alpha != 255u) {
                        ++metrics.m_partial;
                    }
                }
            }

            metrics.m_fnv1a = fnv1aOf(
                std::span{pixels, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u});
            return metrics;
        }

        // Per-glyph raster probe inside the glyph's packed atlas rectangle
        // (located through the glyph UVs, which point at the exact packed
        // rect). `m_max_h_step` is the largest horizontal alpha step in the
        // rectangle — a hinting proxy: vertical stem snapping produces binary
        // transitions while unhinted/lightly hinted rasters soften them.
        struct GlyphMetrics final {
            u64 m_ink{0u};
            u64 m_partial{0u};
            u64 m_coverage{0u};
            u64 m_max_h_step{0u};
            float m_extent_x{0.0f};
            float m_extent_y{0.0f};
            float m_advance_x{0.0f};
            int2 m_rect_min{};
            int2 m_rect_max{};
        };

        [[nodiscard]] GlyphMetrics measureGlyph(
            const int width, const int height, const u8 *const pixels, const ImFontGlyph &glyph) noexcept {
            const auto toPixel = [](const float uv, const int extent) noexcept {
                return std::clamp(static_cast<int>(uv * static_cast<float>(extent) + 0.5f), 0, extent);
            };
            const auto alphaAt = [&](const int x, const int y) noexcept {
                const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                          static_cast<std::size_t>(x);
                return pixels[index * 4u + 3u];
            };

            GlyphMetrics metrics{};
            metrics.m_extent_x = glyph.X1 - glyph.X0;
            metrics.m_extent_y = glyph.Y1 - glyph.Y0;
            metrics.m_advance_x = glyph.AdvanceX;
            metrics.m_rect_min = int2{toPixel(glyph.U0, width), toPixel(glyph.V0, height)};
            metrics.m_rect_max = int2{toPixel(glyph.U1, width), toPixel(glyph.V1, height)};

            for (int y = metrics.m_rect_min.y; y < metrics.m_rect_max.y; ++y) {
                for (int x = metrics.m_rect_min.x; x < metrics.m_rect_max.x; ++x) {
                    const u8 alpha = alphaAt(x, y);
                    metrics.m_coverage += alpha;
                    if (alpha != 0u) {
                        ++metrics.m_ink;
                        if (alpha != 255u) {
                            ++metrics.m_partial;
                        }
                    }

                    if (x + 1 < metrics.m_rect_max.x) {
                        const u8 next = alphaAt(x + 1, y);
                        const int difference = (next > alpha) ? next - alpha : alpha - next;
                        metrics.m_max_h_step = std::max(metrics.m_max_h_step, static_cast<u64>(difference));
                    }
                }
            }

            return metrics;
        }

        [[nodiscard]] const u8 *readAtlasPixels(ImFontAtlas &atlas, int &width, int &height) {
            u8 *pixels = nullptr;
            int bpp = 0;
            atlas.GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);
            PPR_TEST_ASSERT(pixels != nullptr and width > 0 and height > 0 and bpp == 4);

            return pixels;
        }

        [[nodiscard]] std::string hexOf(const u64 value) {
            constexpr std::string_view digits{"0123456789abcdef"};
            std::string text{"0x"};
            for (int shift = 60; shift >= 0; shift -= 4) {
                text += digits[static_cast<std::size_t>((value >> shift) & 0xfu)];
            }

            return text;
        }

        // Comma-joined CSV row assembly. Deliberately string-based
        // (std::to_string, never std::format): the MSVC-modules std::format
        // ICE precedent is recorded at Asset.Resilience.Tests.cpp:438-439.
        struct RowBuilder final {
            std::string m_row{};

            void cell(const std::string_view text) {
                if (not m_row.empty()) {
                    m_row += ',';
                }
                m_row += text;
            }

            void cell(const u64 value) {
                cell(std::to_string(value));
            }

            void cell(const int value) {
                cell(std::to_string(value));
            }

            void cell(const float value) {
                cell(std::to_string(value));
            }
        };

        // Fixed glyph sample: Latin coverage on both faces plus the two icon
        // codepoints the merged face contributes. Every sampled codepoint sits
        // inside a declared range (Latin within the default 0x20-0xFF face
        // range, icons within ui::icons::kMergeRanges), which the legacy
        // preload bakes with the atlas (imgui_draw.cpp:3541-3561), so reading
        // them can never mutate the atlas under measurement.
        inline constexpr std::array kBodySamples{
            char32_t{U'A'}, char32_t{U'g'}, char32_t{U'W'}, char32_t{U'@'}, char32_t{U'0'}, ui::icons::kClose,
            ui::icons::kReload};
        inline constexpr std::array kMonoSamples{char32_t{U'A'}, char32_t{U'0'}, char32_t{U'g'}};
        inline constexpr std::array<std::span<const char32_t>, 2u> kFontSamples{
            std::span{kBodySamples}, std::span{kMonoSamples}};

        // Artifact writer for one build: the metric CSVs, the run log, and
        // the atlas PNGs the two builds are compared with. The directory is
        // cleared on construction so a stale row from an earlier run can
        // never be mistaken for this one.
        struct Evidence final {
            std::filesystem::path m_dir{};
            std::string m_build{};
            std::string m_loader{};
            std::string m_loader_tag{};
            std::ofstream m_atlas_rows{};
            std::ofstream m_font_rows{};
            std::ofstream m_glyph_rows{};
            std::ofstream m_run_log{};

            explicit Evidence(const std::string_view build, const std::string_view loader) {
                m_build = build;
                m_loader = loader;
                m_loader_tag = loader.starts_with("FreeType") ? "freetype" : "stb";
                m_dir = hal::process::currentExecutablePath().parent_path() / "m4-font-rasterizer";

                std::error_code cleanup{};
                std::filesystem::remove_all(m_dir, cleanup);
                std::filesystem::create_directories(m_dir);
                PPR_TEST_ASSERT(std::filesystem::exists(m_dir));

                m_atlas_rows.open(m_dir / "atlas_metrics.csv", std::ios::trunc);
                m_font_rows.open(m_dir / "font_metrics.csv", std::ios::trunc);
                m_glyph_rows.open(m_dir / "glyph_metrics.csv", std::ios::trunc);
                m_run_log.open(m_dir / "run.txt", std::ios::trunc);
                PPR_TEST_ASSERT(
                    m_atlas_rows.is_open() and
                    m_font_rows.is_open() and
                    m_glyph_rows.is_open() and
                    m_run_log.is_open());

                m_atlas_rows
                    << "build,loader,variant,flags,phase,density,extent_x,extent_y,texels,ink,partial,coverage,"
                       "hist0,hist1,hist2,hist3,hist4,hist5,hist6,hist7,hist8,hist9,hist10,hist11,hist12,"
                       "hist13,hist14,hist15,fnv1a,png\n";
                m_font_rows << "build,loader,variant,phase,density,font_index,legacy_size,baked_size,baked_density,"
                               "glyphs_before,glyphs_after,metrics_total_surface\n";
                m_glyph_rows
                    << "build,loader,variant,phase,density,font_index,codepoint,source_index,extent_x,extent_y,"
                       "advance_x,ink,partial,coverage,max_h_step,rect_x0,rect_y0,rect_x1,rect_y1\n";

                note("build=" + m_build);
                note("loader=" + m_loader);
                note("build_flag_IMGUI_ENABLE_FREETYPE=" + std::to_string(PPR_M4_IMGUI_FREETYPE));
                note("artifacts=" + m_dir.string());
            }

            void note(const std::string &line) {
                std::println("[m4-rasterizer] {}", line);
                m_run_log << line << '\n';
            }

            void saveAtlasPng(const AtlasMetrics &metrics, const u8 *const pixels, const std::string &name) {
                const mango::image::Surface surface{
                    metrics.m_extent.x, metrics.m_extent.y,
                    mango::image::Format{32, mango::image::Format::UNORM, mango::image::Format::RGBA, 8, 8, 8, 8},
                    static_cast<std::size_t>(metrics.m_extent.x) * 4u, pixels};
                PPR_TEST_ASSERT(static_cast<bool>(surface.save((m_dir / name).string())));
            }

            /// Measures the live atlas and writes one atlas row. `phase` is
            /// "first" or "repeat"; the repeat row carries no PNG because its
            /// bytes must equal the first row's (asserted by the caller).
            [[nodiscard]] u64 atlasRow(
                ImFontAtlas &atlas, const VariantSpec &variant, const std::string_view phase, const float density) {
                int width = 0;
                int height = 0;
                const u8 *const pixels = readAtlasPixels(atlas, width, height);
                const AtlasMetrics metrics = measureAtlas(width, height, pixels);
                const int density_axis = static_cast<int>(density + 0.5f);

                std::string png_name{"-"};
                if (phase == "first") {
                    png_name = "atlas_" +
                               m_loader_tag +
                               "_" +
                               std::string{variant.m_name} +
                               "_d" +
                               std::to_string(density_axis) +
                               ".png";
                    saveAtlasPng(metrics, pixels, png_name);
                }

                RowBuilder row{};
                row.cell(m_build);
                row.cell(m_loader);
                row.cell(variant.m_name);
                row.cell(static_cast<u64>(variant.m_flags));
                row.cell(phase);
                row.cell(density_axis);
                row.cell(metrics.m_extent.x);
                row.cell(metrics.m_extent.y);
                row.cell(metrics.m_texels);
                row.cell(metrics.m_ink);
                row.cell(metrics.m_partial);
                row.cell(metrics.m_coverage);
                for (const u64 bin: metrics.m_histogram) {
                    row.cell(bin);
                }
                row.cell(hexOf(metrics.m_fnv1a));
                row.cell(png_name);
                m_atlas_rows << row.m_row << '\n';
                m_atlas_rows.flush();

                note(
                    "atlas variant=" +
                    std::string{variant.m_name} +
                    " flags=" +
                    std::to_string(variant.m_flags) +
                    " phase=" +
                    std::string{phase} +
                    " density=" +
                    std::to_string(density_axis) +
                    " extent=" +
                    std::to_string(metrics.m_extent.x) +
                    "x" +
                    std::to_string(metrics.m_extent.y) +
                    " texels=" +
                    std::to_string(metrics.m_texels) +
                    " ink=" +
                    std::to_string(metrics.m_ink) +
                    " partial=" +
                    std::to_string(metrics.m_partial) +
                    " coverage=" +
                    std::to_string(metrics.m_coverage) +
                    " fnv1a=" +
                    hexOf(metrics.m_fnv1a) +
                    " png=" +
                    png_name);
                return metrics.m_fnv1a;
            }

            /// Per-font rows (bake identity, glyph counts) plus per-glyph
            /// rows for the fixed sample. The pre/post glyph counts prove the
            /// sample did not mutate the measured atlas.
            void sampledRows(
                ImFontAtlas &atlas, const VariantSpec &variant, const std::string_view phase, const float density) {
                int width = 0;
                int height = 0;
                const u8 *const pixels = readAtlasPixels(atlas, width, height);
                const int density_axis = static_cast<int>(density + 0.5f);

                for (int font_index = 0; font_index < atlas.Fonts.Size; ++font_index) {
                    ImFont *const font = atlas.Fonts[font_index];
                    ImFontBaked *const baked = font->GetFontBaked(font->LegacySize);
                    PPR_TEST_ASSERT(baked != nullptr);
                    PPR_TEST_ASSERT(baked->RasterizerDensity == density);
                    const int glyphs_before = baked->Glyphs.Size;

                    for (const char32_t codepoint: kFontSamples[static_cast<std::size_t>(font_index)]) {
                        ImFontGlyph *const glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(codepoint));
                        PPR_TEST_ASSERT(glyph != nullptr);
                        const GlyphMetrics metrics = measureGlyph(width, height, pixels, *glyph);

                        RowBuilder row{};
                        row.cell(m_build);
                        row.cell(m_loader);
                        row.cell(variant.m_name);
                        row.cell(phase);
                        row.cell(density_axis);
                        row.cell(font_index);
                        row.cell(static_cast<u64>(codepoint));
                        row.cell(static_cast<u64>(glyph->SourceIdx));
                        row.cell(metrics.m_extent_x);
                        row.cell(metrics.m_extent_y);
                        row.cell(metrics.m_advance_x);
                        row.cell(metrics.m_ink);
                        row.cell(metrics.m_partial);
                        row.cell(metrics.m_coverage);
                        row.cell(metrics.m_max_h_step);
                        row.cell(metrics.m_rect_min.x);
                        row.cell(metrics.m_rect_min.y);
                        row.cell(metrics.m_rect_max.x);
                        row.cell(metrics.m_rect_max.y);
                        m_glyph_rows << row.m_row << '\n';

                        note(
                            "glyph variant=" +
                            std::string{variant.m_name} +
                            " phase=" +
                            std::string{phase} +
                            " density=" +
                            std::to_string(density_axis) +
                            " font=" +
                            std::to_string(font_index) +
                            " cp=0x" +
                            hexOf(static_cast<u64>(codepoint)).substr(2u) +
                            " ink=" +
                            std::to_string(metrics.m_ink) +
                            " max_h_step=" +
                            std::to_string(metrics.m_max_h_step));
                    }

                    const int glyphs_after = baked->Glyphs.Size;
                    PPR_TEST_ASSERT(glyphs_after == glyphs_before);
                    PPR_TEST_ASSERT(glyphs_before > 1);

                    RowBuilder row{};
                    row.cell(m_build);
                    row.cell(m_loader);
                    row.cell(variant.m_name);
                    row.cell(phase);
                    row.cell(density_axis);
                    row.cell(font_index);
                    row.cell(font->LegacySize);
                    row.cell(baked->Size);
                    row.cell(baked->RasterizerDensity);
                    row.cell(glyphs_before);
                    row.cell(glyphs_after);
                    row.cell(static_cast<u64>(baked->MetricsTotalSurface));
                    m_font_rows << row.m_row << '\n';
                    note(
                        "font variant=" +
                        std::string{variant.m_name} +
                        " phase=" +
                        std::string{phase} +
                        " density=" +
                        std::to_string(density_axis) +
                        " font=" +
                        std::to_string(font_index) +
                        " legacy_size=" +
                        std::to_string(font->LegacySize) +
                        " baked_size=" +
                        std::to_string(baked->Size) +
                        " glyphs=" +
                        std::to_string(baked->Glyphs.Size) +
                        " metrics_total_surface=" +
                        std::to_string(baked->MetricsTotalSurface));
                }

                m_font_rows.flush();
                m_glyph_rows.flush();
                m_run_log.flush();
            }
        };
    } // namespace FontRasterizer

    PPR_UNIT_TEST (font_rasterizer_comparison_evidence) {
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

        // Unwind-safe teardown: on a failed PPR_TEST_ASSERT the input context
        // must be detached and the window handle released before
        // ~WindowInputContext and ~Window run, or their hard asserts abort
        // the process during unwinding and mask the failing predicate.
        PPR_DEFER {
            std::ignore = routed.shutdown();
            if (window.m_handle != nullptr) {
                std::ignore = window.release();
            }
        };

        PPR_TEST_ASSERT(not routed.initialize(safe_ptr<Window>{&window}));
        WindowViewport viewport{safe_ptr<Window>{&window}, ViewportLayout{}};
        std::ignore = viewport.updateFromWindow();
        const TimeSpan frame{std::chrono::milliseconds{16}};

        auto ui = ui::createImGuiService();
        // Unwind-safe teardown: a failed PPR_TEST_ASSERT must surface as the
        // failing predicate, not as ~ImGuiService's live-context assert
        // terminating the process during unwinding (fail-fast).
        PPR_DEFER {
            if (ui) {
                std::ignore = ui->shutdown();
            }
        };

        const std::filesystem::path font_root = FontLoading::stagedFontRoot();
        const std::array sources{
            FontLoading::interSource(font_root), FontLoading::awesomeSource(font_root),
            FontLoading::monoSource(font_root)};
        PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, sources, 0));

        auto *const imgui_context = static_cast<ImGuiContext *>(ui->getContext());
        PPR_TEST_ASSERT(imgui_context != nullptr);
        ImGui::SetCurrentContext(imgui_context);
        ImGuiIO &io = ImGui::GetIO();
        ImFontAtlas &atlas = *io.Fonts;

        // The production composition: Inter + merged icons, then the mono
        // face. Two faces, so the rows below cover both a merged and a
        // standalone source.
        PPR_TEST_ASSERT(atlas.Fonts.Size == 2);

        // Loader identity: the runtime name the linked loader reports
        // (imgui_draw.cpp:4867 "stb_truetype", imgui_freetype.cpp:370
        // "FreeType (x.y.z; x.y.z)"), cross-checked against the compile
        // definition that travels to this TU through imgui.base PUBLIC. The
        // artifact identity is never inferred from the build flag alone.
        PPR_TEST_ASSERT(atlas.FontLoaderName != nullptr);
        const std::string loader_name{atlas.FontLoaderName};
#if PPR_M4_IMGUI_FREETYPE
        PPR_TEST_ASSERT(loader_name.starts_with("FreeType"));
#else
        PPR_TEST_ASSERT(loader_name == "stb_truetype");
#endif

        const bool free_type_loader = loader_name.starts_with("FreeType");
#if PPR_M4_IMGUI_FREETYPE
        constexpr std::string_view build_tag{"freetype"};
#else
        constexpr std::string_view build_tag{"default"};
#endif
        FontRasterizer::Evidence evidence{build_tag, loader_name};

        // Completes the open frame; the atlas is unlocked only afterwards.
        const auto bounce = [&](const int2 framebuffer) {
            window.m_content_scale = float2{
                static_cast<float>(framebuffer.x) / 800.0f, static_cast<float>(framebuffer.y) / 600.0f};
            window.m_framebuffer_size = framebuffer;
            PPR_TEST_ASSERT(not ui->update(frame, viewport));

            // Fixed content sample: the production labels plus a dense Latin
            // line. Identical for every variant, so a differing glyph set can
            // never masquerade as a rasterizer difference.
            ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
            ImGui::SetNextWindowSize(ImVec2{400.0f, 240.0f});
            PPR_TEST_ASSERT(ImGui::Begin("m4_raster_sample"));
            for (const std::string_view label: ui::icons::kLabels) {
                ImGui::TextUnformatted(label.data(), label.data() + label.size());
            }
            ImGui::TextUnformatted("Raster sample: AWgm@0123");
            ImGui::End();

            DetachedUiDrawData built{};
            PPR_TEST_ASSERT(not ui->buildDetachedUiFrame(built));
            PPR_TEST_ASSERT(built.m_has_content);
        };

        const std::size_t variant_count = free_type_loader ? FontRasterizer::kVariantCount : 1u;
        std::array<u64, FontRasterizer::kVariantCount> density_two_hashes{};
        std::array<u64, FontRasterizer::kVariantCount> density_one_hashes{};

        for (std::size_t variant_index = 0u; variant_index < variant_count; ++variant_index) {
            const FontRasterizer::VariantSpec &variant = FontRasterizer::kVariants[variant_index];

            // Flags are applied at the next source (re)initialization, which
            // the engine forces through the density rebake: rebuildFontAtlasForDensity_
            // runs ImFontAtlasBuildClear -> FontSrcDestroy/Init, and
            // ImGui_ImplFreeType_FontSrcInit reads atlas->FontLoaderFlags
            // (imgui_freetype.cpp:409). Setting flags without a rebake would
            // record the previous bake.
            atlas.FontLoaderFlags = variant.m_flags;

            // Loop invariant: every variant starts at density 1.0 (the init
            // bake, then the restore at the end of the previous iteration),
            // so the 1.0 -> 2.0 step below is a real rebake for every variant:
            // the engine rebuilds only when the computed density differs from
            // m_font_density (App.UI.ImGui.cpp:1159-1162), and a same-size
            // update would silently re-record the previous variant's atlas
            // under this variant's flags label.
            bounce(int2{1600, 1200});
            PPR_TEST_ASSERT(atlas.FontLoaderFlags == variant.m_flags);
            density_two_hashes[variant_index] = evidence.atlasRow(atlas, variant, "first", 2.0f);
            evidence.sampledRows(atlas, variant, "first", 2.0f);

            bounce(int2{800, 600});
            PPR_TEST_ASSERT(atlas.FontLoaderFlags == variant.m_flags);
            density_one_hashes[variant_index] = evidence.atlasRow(atlas, variant, "first", 1.0f);
            evidence.sampledRows(atlas, variant, "first", 1.0f);

            // Determinism: the same density and flags bake byte-identical
            // bytes (the repeat row carries no PNG).
            bounce(int2{1600, 1200});
            const u64 repeated = evidence.atlasRow(atlas, variant, "repeat", 2.0f);
            evidence.sampledRows(atlas, variant, "repeat", 2.0f);
            PPR_TEST_ASSERT(repeated == density_two_hashes[variant_index]);

            // Restore the loop invariant for the next variant.
            bounce(int2{800, 600});
        }

        if (free_type_loader) {
            // Flag-plumbing proof: identical inputs and identical content,
            // differing only in atlas->FontLoaderFlags, must not bake
            // identical bytes. Without this, a silently dropped flag would
            // record three "clean" variants and a misleading evidence table.
            const std::size_t default_index = std::to_underlying(FontRasterizer::Variant::Default);
            const std::size_t light_index = std::to_underlying(FontRasterizer::Variant::LightHinting);
            const std::size_t auto_index = std::to_underlying(FontRasterizer::Variant::ForceAutoHint);
            PPR_TEST_ASSERT(density_two_hashes[light_index] != density_two_hashes[default_index]);
            PPR_TEST_ASSERT(density_two_hashes[auto_index] != density_two_hashes[default_index]);
            PPR_TEST_ASSERT(density_one_hashes[light_index] != density_one_hashes[default_index]);
            PPR_TEST_ASSERT(density_one_hashes[auto_index] != density_one_hashes[default_index]);
        }

        PPR_TEST_ASSERT(not ui->shutdown());
        PPR_TEST_ASSERT(not ui->shutdown());
        ui.reset();
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
            detail::repeated_ui_init_shutdown_preserves_module_refs,
            detail::detached_colors_decode_only_for_srgb_targets_before_interpolation,
            detail::font_input_malformed_face_rejects_without_fallback,
            detail::font_input_size_limits_reject_before_file_access,
            detail::font_input_hash_mismatch_rejects_staged_file,
            detail::font_input_missing_file_propagates_open_error_and_tears_down,
            detail::font_input_declared_length_mismatch_rejects_both_directions,
            detail::font_density_rebake_grows_atlas_and_stays_stable,
            detail::content_scale_changes_physical_size_without_cumulative_geometry_scaling,
            detail::icon_vocabulary_labels_keep_ids_and_cover_merge_ranges,
            detail::font_merge_declaration_rejections_precede_file_access,
            detail::font_merge_bakes_icon_face_into_body_source,
            detail::font_rasterizer_comparison_evidence,
        });
    };

    const UnitTest &imgui_routingTests() noexcept {
        return imgui_routing;
    }
} // namespace pP::tests
