module;
#include "pP/Macros.h"
module engine.app;

import :input.device;
import :input.listener;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(InputDevice, info, none);

    // ------------------------------------------------------------------
    // analog axis input state
    // ------------------------------------------------------------------

    template<typename T>
    void InputAxisState<T>::update(const double elapsed_seconds) noexcept {
        m_raw.m_relative = m_next_raw_absolute - m_raw.m_absolute;
        if (dot2(m_raw.m_relative) > dot2(m_dead_zone)) {
            m_raw.m_absolute = m_next_raw_absolute;
        } else {
            m_raw.m_relative = value_type{zero_v};
        }

        const float blend_rate = saturate(static_cast<float>(
            std::pow(elapsed_seconds,
                1.0 / std::max<float>(m_sensitivity, epsilon_v<float>))));

        const value_type next_filtered = lerp(
            m_filtered.m_absolute,
            m_raw.m_absolute,
            blend_rate);

        m_filtered.m_relative = next_filtered - m_filtered.m_absolute;
        m_filtered.m_absolute = next_filtered;
    }

    template<typename T>
    bool InputAxisState<T>::postInputMessages(
        const TimeSpan dt,
        const InputContext &context,
        const InputDeviceID device_id,
        const InputKey input_key,
        const bool enable_filtered_inputs) const noexcept {
        if (const auto &analog_value = get(enable_filtered_inputs);
            dot2(analog_value.m_relative) > m_dead_zone) {
            InputValue input_value(analog_value);
            PPR_ASSERT(input_value.getType() == input_key.m_value);

            std::ignore = context.postKeyEvent(dt, InputMessage{
                input_key, std::move(input_value),
                device_id, EInputMessageEvent::axis
            });
            return true;
        }
        return false;
    }

    template<typename T>
    void InputAxisState<T>::reset() noexcept {
        reset(zero_v);
    }

    template<typename T>
    void InputAxisState<T>::reset(const_reference init) noexcept {
        m_raw.m_absolute = init;
        m_raw.m_relative = value_type{zero_v};
        m_filtered = m_raw;
        m_next_raw_absolute = m_raw.m_absolute;
    }

    template struct InputAxisState<float2>;
    template struct InputAxisState<float>;

    // ------------------------------------------------------------------
    // digital input state
    // ------------------------------------------------------------------

    template<typename ButtonT>
        requires std::is_enum_v<ButtonT> || std::is_integral_v<ButtonT>
    bool InputDigitalState<ButtonT>::postInputMessages(const TimeSpan dt, const InputContext &context, const InputDeviceID device_id, ButtonT button,
                                                       const bool pressed) {
        const std::optional input_key = InputKey::from(button);
        if (not input_key.has_value()) {
            return false;
        }

        if (pressed) {
            const auto [it, inserted] = m_pressed.insert(button);

            std::ignore = context.postKeyEvent(dt, InputMessage{
                input_key.value(), InputDigital(true),
                device_id, inserted ? EInputMessageEvent::pressed : EInputMessageEvent::repeat
            });
            return true;
        }

        if (m_pressed.erase(button) > 0u) {
            std::ignore = context.postKeyEvent(dt, InputMessage{
                input_key.value(), InputDigital(true),
                device_id, EInputMessageEvent::released
            });
            return true;
        }

        return false;
    }

    template class InputDigitalState<EKeyboardKey>;
    template class InputDigitalState<EGamepadButton>;
    template class InputDigitalState<EMouseButton>;

    // ------------------------------------------------------------------
    // keyboard device
    // ------------------------------------------------------------------

    KeyboardDevice::KeyboardDevice(const InputDeviceID device_id) noexcept
        : m_device_id{device_id} {
    }

    void KeyboardDevice::postKeyboardCharacterInput(const InputContext &context, const hal::native::char_t codepoint) {
        m_character_inputs.push_back(codepoint);
        std::ignore = context.postCharacterInput(codepoint);
    }

    void KeyboardDevice::postKeyboardKeyPressed(const TimeSpan dt, const InputContext &context, const EKeyboardKey key, const bool pressed) {
        if (not m_keys.postInputMessages(dt, context, m_device_id, key, pressed)) [[unlikely]] {
            PPR_LOG(InputDevice, verbose, "unbound keyboard key received", {
                {"device_id", m_device_id},
                {"keyboard_key", enumOrd(key)},
                });
        }
    }

    std::error_code KeyboardDevice::enumerateSupportedInputKeys(const Collector<InputKey> supports_key) const {
        return InputKey::enumerateKeyboardKeys(supports_key);
    }

    std::error_code KeyboardDevice::pollInputMessages(const TimeSpan) {
        // reset character inputs every frame: MUST HAPPEN **BEFORE** POLLING WINDOW EVENTS
        m_character_inputs.clear();
        return default_value_v;
    }

    void KeyboardDevice::resetInputState() noexcept {
        m_keys.resetInputState();
        m_character_inputs.clear();
    }

    // ------------------------------------------------------------------
    // mouse device
    // ------------------------------------------------------------------

    MouseDevice::MouseDevice(const InputDeviceID device_id) noexcept
        : m_device_id{device_id} {
    }

    std::error_code MouseDevice::enumerateSupportedInputKeys(const Collector<InputKey> supports_key) const {
        if (const std::error_code err = InputKey::enumerateMouseAxes(supports_key)) [[unlikely]] {
            return err;
        }
        return InputKey::enumerateMouseButtons(supports_key);
    }

    void MouseDevice::postMouseButtonPressed(TimeSpan dt, const InputContext &context, EMouseButton button, bool pressed) {
        if (not m_buttons.postInputMessages(dt, context, m_device_id, button, pressed)) [[unlikely]] {
            PPR_LOG(InputDevice, verbose, "unbound mouse button received", {
                {"device_id", m_device_id},
                {"mouse_button", enumOrd(button)},
                });
        }
    }

    void MouseDevice::postMouseCursorPosition(const TimeSpan dt, const InputContext &context, const float2 &absolute_pos) {
        m_cursor_pos.set(absolute_pos);
        m_cursor_pos.update(time::seconds(dt));
        std::ignore = m_cursor_pos.postInputMessages(dt, context, m_device_id, InputKey::mouse_2d, m_has_axis_filtering);
    }

    void MouseDevice::postMouseScrollWheel(const TimeSpan dt, const InputContext &context, const float2 &delta) {
        const double elapsed_seconds = time::seconds(dt);

        m_wheel_x.add(delta.x);
        m_wheel_x.update(elapsed_seconds);
        std::ignore = m_wheel_x.postInputMessages(dt, context, m_device_id, InputKey::mouse_wheel_axis_y, m_has_axis_filtering);

        m_wheel_y.add(delta.y);
        m_wheel_y.update(elapsed_seconds);
        std::ignore = m_wheel_y.postInputMessages(dt, context, m_device_id, InputKey::mouse_wheel_axis_y, m_has_axis_filtering);
    }

    std::error_code MouseDevice::pollInputMessages(const TimeSpan dt) {
        std::ignore = dt;
        return default_value_v;
    }

    void MouseDevice::resetInputState() noexcept {
        m_cursor_pos.reset();
        m_wheel_x.reset();
        m_wheel_y.reset();
    }

    // ------------------------------------------------------------------
    // gamepad device
    // ------------------------------------------------------------------

    GamepadDevice::GamepadDevice(const InputDeviceID device_id) noexcept
        : m_device_id(device_id) {
    }

    void GamepadDevice::postGamepadAxis1DMoved(const TimeSpan dt, const InputContext &context, const EGamepadAxis axis, const float value) {
        const double elapsed_seconds = time::seconds(dt);

        switch (axis) {
            case EGamepadAxis::left_trigger:
                m_left_trigger.set(value);
                m_left_trigger.update(elapsed_seconds);
                std::ignore = m_left_trigger.postInputMessages(dt, context, m_device_id, InputKey::gamepad_left_trigger_axis, m_has_trigger_filtering);
                break;

            case EGamepadAxis::right_trigger:
                m_right_trigger.set(value);
                m_right_trigger.update(elapsed_seconds);
                std::ignore = m_right_trigger.postInputMessages(dt, context, m_device_id, InputKey::gamepad_right_trigger_axis, m_has_trigger_filtering);
                break;

            default:
                break;
        }
    }

    void GamepadDevice::postGamepadAxis2DMoved(const TimeSpan dt, const InputContext &context, const EGamepadAxis axis, const float2 &value) {
        const double elapsed_seconds = time::seconds(dt);

        switch (axis) {
            case EGamepadAxis::left_stick:
                m_left_stick.set(value);
                m_left_stick.update(elapsed_seconds);
                std::ignore = m_left_stick.postInputMessages(dt, context, m_device_id, InputKey::gamepad_left_2d, m_has_axis_filtering);
                break;

            case EGamepadAxis::right_stick:
                m_right_stick.set(value);
                m_right_stick.update(elapsed_seconds);
                std::ignore = m_right_trigger.postInputMessages(dt, context, m_device_id, InputKey::gamepad_right_2d, m_has_axis_filtering);
                break;

            default:
                break;
        }
    }

    void GamepadDevice::postGamepadButtonPressed(TimeSpan dt, const InputContext &context, EGamepadButton button, bool pressed) {
        if (not m_buttons.postInputMessages(dt, context, m_device_id, button, pressed)) [[unlikely]] {
            PPR_LOG(InputDevice, verbose, "unbound gamepad button received", {
                {"device_id", m_device_id},
                {"gamepad_button", enumOrd(button)},
                });
        }
    }

    std::error_code GamepadDevice::enumerateSupportedInputKeys(const Collector<InputKey> supports_key) const {
        if (const std::error_code err = InputKey::enumerateGamepadAxes(supports_key)) [[unlikely]] {
            return err;
        }
        return InputKey::enumerateGamepadButtons(supports_key);
    }

    std::error_code GamepadDevice::pollInputMessages(const TimeSpan dt) {
        const double elapsed_seconds = time::seconds(dt);

        m_left_rumble.update(elapsed_seconds);
        m_right_rumble.update(elapsed_seconds);

        return default_value_v;
    }

    void GamepadDevice::resetInputState() noexcept {
        m_buttons.resetInputState();

        m_left_stick.reset();
        m_right_stick.reset();

        m_left_trigger.reset();
        m_right_trigger.reset();

        m_left_rumble.reset();
        m_right_rumble.reset();
    }
}
