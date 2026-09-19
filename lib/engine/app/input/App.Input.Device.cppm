module;
#include "pP/Macros.h"
export module engine.app:input.device;

import engine.core;
import engine.math;
import std;
import :service.input;
import :input.key;

export namespace pP {
    // ------------------------------------------------------------------
    // input messages
    // ------------------------------------------------------------------

    enum class EInputMessageEvent : u8 {
        pressed = 0,
        released,
        repeat,
        double_click,
        axis,
    };

    enum class EInputMessageResponse : u8 {
        // listener do not trigger any mapping
        unhandled = 0,
        // listener trigger at least one mapping, but did not consume the input
        handled,
        // listener trigger at least one mapping, and input was consumed (message won't be handled by any other listener)
        consumed,
    };

    struct InputMessage {
        InputKey m_key;
        InputValue m_value{};

        InputDeviceID m_device_id{};
        EInputMessageEvent m_event{};

        constexpr InputMessage(
            InputKey key,
            InputValue value,
            const InputDeviceID device_id,
            const EInputMessageEvent event) noexcept
            : m_key{std::move(key)}, m_value{std::move(value)},
              m_device_id{device_id}, m_event{event} {
        }

        constexpr ~InputMessage() noexcept = default;

        [[nodiscard]] constexpr bool isPressed() const noexcept {
            return m_event == EInputMessageEvent::pressed;
        }

        [[nodiscard]] constexpr bool isReleased() const noexcept {
            return m_event == EInputMessageEvent::released;
        }

        [[nodiscard]] constexpr bool isRepeat() const noexcept {
            return m_event == EInputMessageEvent::repeat;
        }

        [[nodiscard]] constexpr bool isDoubleClick() const noexcept {
            return m_event == EInputMessageEvent::double_click;
        }

        [[nodiscard]] constexpr bool isAxis() const noexcept {
            return m_event == EInputMessageEvent::axis;
        }

        [[nodiscard]] constexpr InputDigital getDigitalValue() const noexcept {
            return std::get<InputDigital>(m_value);
        }

        [[nodiscard]] constexpr InputAxis1D getAxis1DValue() const noexcept {
            return std::get<InputAxis1D>(m_value);
        }

        [[nodiscard]] constexpr const InputAxis2D &getAxis2DValue() const noexcept {
            return std::get<InputAxis2D>(m_value);
        }

        [[nodiscard]] constexpr const InputAxis3D &getAxis3DValue() const noexcept {
            return std::get<InputAxis3D>(m_value);
        }
    };

    template<>
    struct details::relocatable<InputMessage> : std::true_type {
    };

    // ------------------------------------------------------------------
    // abstract input device
    // ------------------------------------------------------------------

    class IInputDevice : public safe_object {
    public:
        // ReSharper disable once CppHidingFunction
        virtual ~IInputDevice() noexcept = default;

        [[nodiscard]] virtual const InputDeviceID &getInputDeviceID() const noexcept = 0;

        [[nodiscard]] virtual std::error_code enumerateSupportedInputKeys(Collector<InputKey> supports_key) const = 0;

        [[nodiscard]] virtual std::error_code pollInputMessages(TimeSpan dt) = 0;

        virtual void resetInputState() noexcept = 0;
    };

    using SharedInputDevice = safe_ptr<const IInputDevice>;

    // ------------------------------------------------------------------
    // analog axis input state
    // ------------------------------------------------------------------

    template<typename T>
    struct InputAxisState {
        using value_type = T;
        using const_reference = param_lvref_t<const T>;
        details::input_value<value_type> m_raw{};
        details::input_value<value_type> m_filtered{};

        value_type m_next_raw_absolute{zero_v};
        bool m_has_active_output{false};

        float m_dead_zone{epsilon_v<float>};
        float m_sensitivity{2.0f};

        [[nodiscard]] constexpr const details::input_value<value_type> &
        get(const bool use_filtered) const noexcept {
            return use_filtered ? m_filtered : m_raw;
        }

        void add(const_reference delta) noexcept {
            m_next_raw_absolute += delta;
        }

        void addClamp(const_reference offset, const_reference vmin, const_reference vmax) noexcept {
            m_next_raw_absolute = clamp(m_next_raw_absolute + offset, vmin, vmax);
        }

        void set(const_reference absolute) noexcept {
            m_next_raw_absolute = absolute;
        }

        void update(double elapsed_seconds) noexcept;

        bool postInputMessages(
            TimeSpan dt,
            const InputContext &context,
            InputDeviceID device_id,
            InputKey input_key,
            bool enable_filtered_inputs,
            bool emit_terminal_inactive = false) noexcept;

        void reset() noexcept;

        void reset(const_reference init) noexcept;
    };

    extern template struct InputAxisState<float2>;
    extern template struct InputAxisState<float>;

    // ------------------------------------------------------------------
    // digital input state
    // ------------------------------------------------------------------

    template<typename ButtonT>
        requires std::is_enum_v<ButtonT>
    or std::is_integral_v<ButtonT>

    class InputDigitalState {
    public:
        using set_type = FlatSet<ButtonT>;

        set_type m_pressed{};

        InputDigitalState() noexcept = default;

        bool postInputMessages(
            TimeSpan dt,
            const InputContext &context,
            InputDeviceID device_id,
            ButtonT button, bool pressed);

        void resetInputState() noexcept {
            m_pressed.clear();
        }
    };

    extern template class InputDigitalState<EKeyboardKey>;
    extern template class InputDigitalState<EGamepadButton>;
    extern template class InputDigitalState<EMouseButton>;

    // ------------------------------------------------------------------
    // keyboard device
    // ------------------------------------------------------------------

    class KeyboardDevice final : public IInputDevice {
    public:
        InputDeviceID m_device_id;

        InputDigitalState<EKeyboardKey> m_keys{};
        Array<hal::native::char_t> m_character_inputs{};

        explicit KeyboardDevice(InputDeviceID device_id) noexcept;

        void postKeyboardCharacterInput(const InputContext &context, hal::native::char_t codepoint);

        void postKeyboardKeyPressed(TimeSpan dt, const InputContext &context, EKeyboardKey key, bool pressed);

        // IInputDevice interface:

        [[nodiscard]] const InputDeviceID &getInputDeviceID() const noexcept override {
            return m_device_id;
        }

        [[nodiscard]] std::error_code enumerateSupportedInputKeys(Collector<InputKey> supports_key) const override;

        [[nodiscard]] std::error_code pollInputMessages(TimeSpan dt) override;

        void resetInputState() noexcept override;
    };

    // ------------------------------------------------------------------
    // mouse device
    // ------------------------------------------------------------------

    class MouseDevice : public IInputDevice {
    public:
        const InputDeviceID m_device_id;

        InputDigitalState<EMouseButton> m_buttons;

        InputAxisState<float2> m_cursor_pos;

        InputAxisState<float> m_wheel_x;
        InputAxisState<float> m_wheel_y;

        bool m_has_axis_filtering: 1 {false};

        explicit MouseDevice(InputDeviceID device_id) noexcept;

        void postMouseButtonPressed(TimeSpan dt, const InputContext &context, EMouseButton button, bool pressed);

        void postMouseCursorPosition(TimeSpan dt, const InputContext &context, const float2 &absolute_pos);

        void postMouseScrollWheel(TimeSpan dt, const InputContext &context, const float2 &delta);

        // IInputDevice interface:

        [[nodiscard]] const InputDeviceID &getInputDeviceID() const noexcept override {
            return m_device_id;
        }

        [[nodiscard]] std::error_code enumerateSupportedInputKeys(Collector<InputKey> supports_key) const override;

        [[nodiscard]] std::error_code pollInputMessages(TimeSpan dt) override;

        void resetInputState() noexcept override;
    };

    // ------------------------------------------------------------------
    // gamepad device
    // ------------------------------------------------------------------

    class GamepadDevice : public IInputDevice {
    public:
        const InputDeviceID m_device_id;
        GamepadControllerID m_controller_id{none_v};

        std::string m_friendly_name{};

        InputDigitalState<EGamepadButton> m_buttons;

        InputAxisState<float2> m_left_stick;
        InputAxisState<float2> m_right_stick;

        InputAxisState<float> m_left_trigger;
        InputAxisState<float> m_right_trigger;

        InputAxisState<float> m_left_rumble;
        InputAxisState<float> m_right_rumble;

        bool m_has_axis_filtering: 1 {true};
        bool m_has_trigger_filtering: 1 {true};
        bool m_has_rumble_filtering: 1 {true};

        explicit GamepadDevice(InputDeviceID device_id) noexcept;

        [[nodiscard]] bool isConnected() const noexcept {
            return m_controller_id != none_v;
        }

        void postGamepadAxis1DMoved(TimeSpan dt, const InputContext &context, EGamepadAxis axis, float value);

        void postGamepadAxis2DMoved(TimeSpan dt, const InputContext &context, EGamepadAxis axis, const float2 &value);

        void postGamepadButtonPressed(TimeSpan dt, const InputContext &context, EGamepadButton button, bool pressed);

        // IInputDevice interface:

        [[nodiscard]] const InputDeviceID &getInputDeviceID() const noexcept override {
            return m_device_id;
        }

        [[nodiscard]] std::error_code enumerateSupportedInputKeys(Collector<InputKey> supports_key) const override;

        [[nodiscard]] std::error_code pollInputMessages(TimeSpan dt) override;

        void resetInputState() noexcept override;
    };
}
