module;
#include "pP/Macros.h"

export module engine.tests.app:devices;

import engine.core;
import engine.app;
import std;

export namespace pP::tests {
    namespace Keyboard {
        PPR_UNIT_TEST(construct_with_device_id) {
            const KeyboardDevice device{InputDeviceID{42u}};
            PPR_ASSERT(device.getInputDeviceID() == InputDeviceID{42u});
            PPR_ASSERT(device.m_state.m_keys.getDown().empty());
        };

        PPR_UNIT_TEST(supported_keys_non_empty) {
            const KeyboardDevice device{InputDeviceID{0u}};
            u32 count = 0u;
            device.supportedInputKeys([&](const InputKey &) noexcept { ++count; });
            PPR_ASSERT(count > 0u);
        };

        PPR_UNIT_TEST(post_key_event) {
            KeyboardDevice device{InputDeviceID{0u}};
            device.m_state.setKeyPressed(EKeyboardKey::space);

            Array<InputMessage> messages;
            device.postInputMessages(zero_v, [&](const InputMessage &msg) {
                messages.push_back(msg);
            });

            PPR_ASSERT(messages.size() == 2u);
            PPR_ASSERT(messages[0u].m_device_id == InputDeviceID{0u});
            PPR_ASSERT(messages[0u].isPressed());
            PPR_ASSERT(messages[1u].m_device_id == InputDeviceID{0u});
            PPR_ASSERT(messages[1u].isRepeat());
        };

        PPR_UNIT_TEST(reset_clears_state) {
            KeyboardDevice device{InputDeviceID{0u}};
            device.m_state.setKeyPressed(EKeyboardKey::enter);
            device.resetInputState();

            Array<InputMessage> messages;
            device.postInputMessages(zero_v, [&](const InputMessage &msg) {
                messages.push_back(msg);
            });

            PPR_ASSERT(messages.empty());
        };
    }

    namespace Mouse {
        PPR_UNIT_TEST(construct_with_device_id) {
            const MouseDevice device{InputDeviceID{7u}};
            PPR_ASSERT(device.getInputDeviceID() == InputDeviceID{7u});
            PPR_ASSERT(device.isFilteringInputs());
        };

        PPR_UNIT_TEST(toggle_filtering_inputs) {
            MouseDevice device{InputDeviceID{0u}};
            PPR_ASSERT(device.isFilteringInputs());
            device.setFilteringInputs(false);
            PPR_ASSERT(!device.isFilteringInputs());
            device.setFilteringInputs(true);
            PPR_ASSERT(device.isFilteringInputs());
        };

        PPR_UNIT_TEST(supported_keys_include_mouse) {
            const MouseDevice device{InputDeviceID{0u}};
            u32 count = 0u;
            device.supportedInputKeys([&](const InputKey &) noexcept { ++count; });
            PPR_ASSERT(count > 0u);
        };
    }

    namespace Gamepad {
        PPR_UNIT_TEST(construct_with_id_and_index) {
            const GamepadDevice device{InputDeviceID{2u}, 3u};
            PPR_ASSERT(device.getInputDeviceID() == InputDeviceID{2u});
            PPR_ASSERT(device.getControllerIndex() == 3u);
        };

        PPR_UNIT_TEST(is_disconnected_by_default) {
            const GamepadDevice device{InputDeviceID{0u}, 0u};
            PPR_ASSERT(!device.isConnected());
        };

        PPR_UNIT_TEST(set_status_connected) {
            GamepadDevice device{InputDeviceID{0u}, 0u};
            device.m_state.setStatus(0u, true);
            PPR_ASSERT(device.isConnected());
        };
    }

    namespace DigitalState {
        PPR_UNIT_TEST(pressed_down_up_transitions) {
            InputDigitalState<EKeyboardKey> state{};
            PPR_ASSERT(state.getDown().empty());
            PPR_ASSERT(state.getPressed().empty());
            PPR_ASSERT(state.getUp().empty());

            state.setPressed(EKeyboardKey::space);
            state.update();

            PPR_ASSERT(state.isDown(EKeyboardKey::space));
            PPR_ASSERT(state.isPressed(EKeyboardKey::space));
            PPR_ASSERT(!state.isUp(EKeyboardKey::space));

            state.update();

            PPR_ASSERT(!state.isDown(EKeyboardKey::space));
            PPR_ASSERT(!state.isPressed(EKeyboardKey::space));
            PPR_ASSERT(state.isUp(EKeyboardKey::space));
        };

        PPR_UNIT_TEST(any_down_and_any_pressed) {
            InputDigitalState<EKeyboardKey> state{};
            PPR_ASSERT(!state.anyDown());
            PPR_ASSERT(!state.anyPressed());

            state.setPressed(EKeyboardKey::enter);
            state.update();

            PPR_ASSERT(state.anyDown());
            PPR_ASSERT(state.anyPressed());

            state.update();

            PPR_ASSERT(!state.anyDown());
            PPR_ASSERT(!state.anyPressed());
            PPR_ASSERT(state.anyUp());
        };
    }

    PPR_UNIT_TEST(app_devices) {
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
            DigitalState::any_down_and_any_pressed,
        });
    };
}
