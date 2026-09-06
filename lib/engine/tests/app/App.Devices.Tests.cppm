module;
#include "pP/UnitTest.h"

export module engine.tests.app:devices;

import engine.core;
import engine.app;
import std;

export namespace pP::tests {
    namespace DeviceHarness {
        struct Capture {
            Array<InputMessage> messages{};
            InputListener listener{};
            InputContext context{};

            Capture() noexcept {
                listener.setRawKeyCallback([this](const TimeSpan, const InputMessage &message) {
                    messages.push_back(message);
                });
                context.addInputListener(safe_ptr<InputListener>{&listener}, 0);
            }
        };
    }

    namespace Keyboard {
        PPR_UNIT_TEST(construct_with_device_id) {
            const KeyboardDevice device{InputDeviceID{42u}};
            PPR_TEST_ASSERT(device.getInputDeviceID() == InputDeviceID{42u});
            PPR_TEST_ASSERT(device.m_keys.m_pressed.empty());
        };

        PPR_UNIT_TEST(supported_keys_non_empty) {
            const KeyboardDevice device{InputDeviceID{0u}};
            u32 count = 0u;
            PPR_TEST_ASSERT(not device.enumerateSupportedInputKeys([&](const InputKey &) noexcept -> std::error_code {
                ++count;
                return default_value_v;
                }));
            PPR_TEST_ASSERT(count > 0u);
        };

        PPR_UNIT_TEST(post_key_event) {
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

        PPR_UNIT_TEST(reset_clears_state) {
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
        PPR_UNIT_TEST(construct_with_device_id) {
            const MouseDevice device{InputDeviceID{7u}};
            PPR_TEST_ASSERT(device.getInputDeviceID() == InputDeviceID{7u});
            PPR_TEST_ASSERT(not device.m_has_axis_filtering);
        };

        PPR_UNIT_TEST(toggle_filtering_inputs) {
            MouseDevice device{InputDeviceID{0u}};
            PPR_TEST_ASSERT(not device.m_has_axis_filtering);
            device.m_has_axis_filtering = true;
            PPR_TEST_ASSERT(device.m_has_axis_filtering);
            device.m_has_axis_filtering = false;
            PPR_TEST_ASSERT(not device.m_has_axis_filtering);
        };

        PPR_UNIT_TEST(supported_keys_include_mouse) {
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
        PPR_UNIT_TEST(construct_with_id_and_index) {
            GamepadDevice device{InputDeviceID{2u}};
            device.m_controller_id = GamepadControllerID{3};
            PPR_TEST_ASSERT(device.getInputDeviceID() == InputDeviceID{2u});
            PPR_TEST_ASSERT(*device.m_controller_id == 3);
        };

        PPR_UNIT_TEST(is_disconnected_by_default) {
            const GamepadDevice device{InputDeviceID{0u}};
            PPR_TEST_ASSERT(!device.isConnected());
        };

        PPR_UNIT_TEST(set_status_connected) {
            GamepadDevice device{InputDeviceID{0u}};
            device.m_controller_id = GamepadControllerID{0};
            PPR_TEST_ASSERT(device.isConnected());
        };
    }

    namespace DigitalState {
        PPR_UNIT_TEST(pressed_down_up_transitions) {
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

        PPR_UNIT_TEST(reset_clears_held) {
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

    PPR_UNIT_TEST(devices) {
        _.recurse({
            Keyboard::construct_with_device_id,
            Keyboard::supported_keys_non_empty,
            Keyboard::post_key_event,
            Keyboard::reset_clears_state,
            Mouse::construct_with_device_id,
            Mouse::toggle_filtering_inputs,
            Mouse::supported_keys_include_mouse,
            Gamepad::construct_with_id_and_index,
            Gamepad::is_disconnected_by_default,
            Gamepad::set_status_connected,
            DigitalState::pressed_down_up_transitions,
            DigitalState::reset_clears_held,
        });
    };
}
