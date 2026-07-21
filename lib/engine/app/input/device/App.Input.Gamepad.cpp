module;
#include "pP/Macros.h"
module engine.app;

import :input.gamepad;

namespace pP {
    // ------------------------------------------------------------------
    // gamepad state
    // ------------------------------------------------------------------

    void GamepadState::setStatus(const std::size_t controller_index, const bool connected) {
        m_controller_index = controller_index;
        m_connected = connected;
    }

    void GamepadState::setButtonPressed(const EGamepadButton button) {
        PPR_ASSERT(m_connected);
        m_buttons.setPressed(button);
    }

    void GamepadState::update(const TimeSpan dt) {
        m_on_connected = not m_was_connected and m_connected;
        m_on_disconnected = m_was_connected and not m_connected;
        m_was_connected = m_connected;

        const double elapsed_seconds = time::seconds(dt);

        m_left_stick.update(elapsed_seconds);
        m_right_stick.update(elapsed_seconds);

        m_left_trigger.update(elapsed_seconds);
        m_right_trigger.update(elapsed_seconds);

        m_buttons.update();
    }

    void GamepadState::reset() {
        m_connected = false;
        m_was_connected = false;
        m_on_connected = false;
        m_on_disconnected = false;

        m_left_stick.reset();
        m_right_stick.reset();

        m_left_trigger.reset();
        m_right_trigger.reset();

        m_buttons.reset();
    }

    // ------------------------------------------------------------------
    // gamepad device
    // ------------------------------------------------------------------

    GamepadDevice::GamepadDevice(const InputDeviceID device_id, const std::size_t controller_index) noexcept
        : m_device_id{device_id} {
        m_state.m_controller_index = controller_index;
    }

    GamepadDevice::~GamepadDevice() noexcept = default;

    std::error_code GamepadDevice::supportedInputKeys(const Collector<InputKey> supports_key) const {
        if (const std::error_code err = InputKey::enumerateGamepadAxes(supports_key)) [[unlikely]] {
            return err;
        }
        return InputKey::enumerateGamepadButtons(supports_key);
    }

    std::error_code GamepadDevice::postInputMessages(const TimeSpan dt, const Collector<InputMessage> post_event) {
        m_state.update(dt);

        if (not m_state.m_connected) [[likely]] {
            return default_value_v;
        }

        const bool enable_filtered_inputs = m_state.m_enable_filtered_inputs;

        m_state.m_left_stick.postInputMessages(
            m_device_id,
            InputKey::gamepad_left_2d,
            enable_filtered_inputs,
            dt, post_event);

        m_state.m_right_stick.postInputMessages(
            m_device_id,
            InputKey::gamepad_right_2d,
            enable_filtered_inputs,
            dt, post_event);

        m_state.m_left_trigger.postInputMessages(
            m_device_id,
            InputKey::gamepad_left_trigger_axis,
            enable_filtered_inputs,
            dt, post_event);

        m_state.m_right_trigger.postInputMessages(
            m_device_id,
            InputKey::gamepad_right_trigger_axis,
            enable_filtered_inputs,
            dt, post_event);

        return m_state.m_buttons.postInputMessages(m_device_id, dt, post_event);
    }

    void GamepadDevice::resetInputState() noexcept {
        m_state.reset();
    }
}
