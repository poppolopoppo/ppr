module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import std;

namespace pP::tests::detail {
    namespace DeviceHarness {
        struct Capture {
            Array<InputMessage> messages{};
            InputListener listener{};
            InputContext context{};

            Capture() noexcept {
                listener.setRawKeyCallback([this](const TimeSpan, const InputMessage &message) -> EInputMessageResponse {
                    messages.push_back(message);
                    return EInputMessageResponse::handled;
                });
                context.addInputListener(safe_ptr{&listener}, 0);
            }
        };
    }

    namespace Keyboard {
        PPR_UNIT_TEST (construct_with_device_id) {
            const KeyboardDevice device{InputDeviceID{42u}};
            PPR_TEST_ASSERT(device.getInputDeviceID() == InputDeviceID{42u});
            PPR_TEST_ASSERT(device.m_keys.m_pressed.empty());
        };

        PPR_UNIT_TEST (supported_keys_non_empty) {
            const KeyboardDevice device{InputDeviceID{0u}};
            u32 count = 0u;
            PPR_TEST_ASSERT(not device.enumerateSupportedInputKeys([&](const InputKey &) noexcept -> std::error_code {
                ++count;
                return default_value_v;
            }));
            PPR_TEST_ASSERT(count > 0u);
        };

        PPR_UNIT_TEST (post_key_event) {
            KeyboardDevice device{InputDeviceID{0u}};
            DeviceHarness::Capture capture{};

            PPR_TEST_ASSERT(device.m_keys.postInputMessages(
                TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::space, true));
            PPR_TEST_ASSERT(capture.messages.size() == 1u);
            PPR_TEST_ASSERT(capture.messages[0u].m_device_id == InputDeviceID{0u});
            PPR_TEST_ASSERT(capture.messages[0u].isPressed());

            // Pressing again while held posts a repeat.
            PPR_TEST_ASSERT(device.m_keys.postInputMessages(
                TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::space, true));
            PPR_TEST_ASSERT(capture.messages.size() == 2u);
            PPR_TEST_ASSERT(capture.messages[1u].isRepeat());

            // Releasing posts released and frees the key.
            PPR_TEST_ASSERT(device.m_keys.postInputMessages(
                TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::space, false));
            PPR_TEST_ASSERT(capture.messages.size() == 3u);
            PPR_TEST_ASSERT(capture.messages[2u].isReleased());
            PPR_TEST_ASSERT(device.m_keys.m_pressed.empty());
        };

        PPR_UNIT_TEST (reset_clears_state) {
            KeyboardDevice device{InputDeviceID{0u}};
            DeviceHarness::Capture capture{};

            PPR_TEST_ASSERT(device.m_keys.postInputMessages(
                TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::enter, true));
            PPR_TEST_ASSERT(capture.messages.size() == 1u);

            device.resetInputState();

            // Releasing after reset posts nothing: the key is no longer held.
            PPR_TEST_ASSERT(not device.m_keys.postInputMessages(
                TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::enter, false));
            PPR_TEST_ASSERT(capture.messages.size() == 1u);
        };
    }

    namespace Mouse {
        PPR_UNIT_TEST (construct_with_device_id) {
            const MouseDevice device{InputDeviceID{7u}};
            PPR_TEST_ASSERT(device.getInputDeviceID() == InputDeviceID{7u});
            PPR_TEST_ASSERT(not device.m_has_axis_filtering);
        };

        PPR_UNIT_TEST (toggle_filtering_inputs) {
            MouseDevice device{InputDeviceID{0u}};
            PPR_TEST_ASSERT(not device.m_has_axis_filtering);
            device.m_has_axis_filtering = true;
            PPR_TEST_ASSERT(device.m_has_axis_filtering);
            device.m_has_axis_filtering = false;
            PPR_TEST_ASSERT(not device.m_has_axis_filtering);
        };

        PPR_UNIT_TEST (supported_keys_include_mouse) {
            const MouseDevice device{InputDeviceID{0u}};
            u32 count = 0u;
            PPR_TEST_ASSERT(not device.enumerateSupportedInputKeys([&](const InputKey &) noexcept -> std::error_code {
                ++count;
                return default_value_v;
            }));
            PPR_TEST_ASSERT(count > 0u);
        };
    }

    namespace Gamepad {
        PPR_UNIT_TEST (construct_with_id_and_index) {
            GamepadDevice device{InputDeviceID{2u}};
            device.m_controller_id = GamepadControllerID{3};
            PPR_TEST_ASSERT(device.getInputDeviceID() == InputDeviceID{2u});
            PPR_TEST_ASSERT(*device.m_controller_id == 3);
        };

        PPR_UNIT_TEST (is_disconnected_by_default) {
            const GamepadDevice device{InputDeviceID{0u}};
            PPR_TEST_ASSERT(!device.isConnected());
        };

        PPR_UNIT_TEST (set_status_connected) {
            GamepadDevice device{InputDeviceID{0u}};
            device.m_controller_id = GamepadControllerID{0};
            PPR_TEST_ASSERT(device.isConnected());
        };

        PPR_UNIT_TEST (axis_posts_terminal_zero_once) {
            GamepadDevice device{InputDeviceID{0u}};
            device.m_has_axis_filtering = false;
            DeviceHarness::Capture capture{};
            const TimeSpan dt{std::chrono::milliseconds{16}};

            device.postGamepadAxis2DMoved(dt, capture.context, EGamepadAxis::left_stick, float2{0.0f, 1.0f});
            PPR_TEST_ASSERT(capture.messages.size() == 1u);
            PPR_TEST_ASSERT(capture.messages[0u].m_key == InputKey::gamepad_left_2d);
            PPR_TEST_ASSERT(capture.messages[0u].getAxis2DValue().m_absolute.y > 0.0f);

            const float resting_value = device.m_left_stick.m_dead_zone / 2.0f;
            device.postGamepadAxis2DMoved(dt, capture.context, EGamepadAxis::left_stick, float2{0.0f, resting_value});
            PPR_TEST_ASSERT(capture.messages.size() == 2u);
            PPR_TEST_ASSERT(distance(capture.messages[1u].getAxis2DValue().m_absolute, float2{zero_v}) < epsilon_v<float>);
            PPR_TEST_ASSERT(distance(capture.messages[1u].getAxis2DValue().m_relative, float2{zero_v}) < epsilon_v<float>);

            device.postGamepadAxis2DMoved(dt, capture.context, EGamepadAxis::left_stick, float2{0.0f, resting_value});
            PPR_TEST_ASSERT(capture.messages.size() == 2u);
        };
    }

    namespace DigitalState {
        PPR_UNIT_TEST (pressed_down_up_transitions) {
            InputDigitalState<EKeyboardKey> state{};
            DeviceHarness::Capture capture{};
            PPR_TEST_ASSERT(state.m_pressed.empty());

            state.postInputMessages(TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::space, true);
            PPR_TEST_ASSERT(not state.m_pressed.empty());
            PPR_TEST_ASSERT(capture.messages.size() == 1u);
            PPR_TEST_ASSERT(capture.messages[0u].isPressed());

            state.postInputMessages(TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::space, true);
            PPR_TEST_ASSERT(capture.messages.size() == 2u);
            PPR_TEST_ASSERT(capture.messages[1u].isRepeat());

            state.postInputMessages(TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::space, false);
            PPR_TEST_ASSERT(state.m_pressed.empty());
            PPR_TEST_ASSERT(capture.messages.size() == 3u);
            PPR_TEST_ASSERT(capture.messages[2u].isReleased());
        };

        PPR_UNIT_TEST (reset_clears_held) {
            InputDigitalState<EKeyboardKey> state{};
            DeviceHarness::Capture capture{};

            state.postInputMessages(TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::enter, true);
            PPR_TEST_ASSERT(not state.m_pressed.empty());

            state.resetInputState();
            PPR_TEST_ASSERT(state.m_pressed.empty());
            PPR_TEST_ASSERT(not state.postInputMessages(
                TimeSpan{}, capture.context, InputDeviceID{0u}, EKeyboardKey::enter, false));
            PPR_TEST_ASSERT(capture.messages.size() == 1u);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest devices = UnitTest::Named("devices") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Keyboard::construct_with_device_id,
            detail::Keyboard::supported_keys_non_empty,
            detail::Keyboard::post_key_event,
            detail::Keyboard::reset_clears_state,
            detail::Mouse::construct_with_device_id,
            detail::Mouse::toggle_filtering_inputs,
            detail::Mouse::supported_keys_include_mouse,
            detail::Gamepad::construct_with_id_and_index,
            detail::Gamepad::is_disconnected_by_default,
            detail::Gamepad::set_status_connected,
            detail::Gamepad::axis_posts_terminal_zero_once,
            detail::DigitalState::pressed_down_up_transitions,
            detail::DigitalState::reset_clears_held,
        });
    };
} // namespace pP::tests
