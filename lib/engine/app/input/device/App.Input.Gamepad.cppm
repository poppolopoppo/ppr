module;
#include "pP/Macros.h"
export module engine.app:input.gamepad;

import engine.core;
import :input.key;
import :input.device;

export namespace pP {
    // ------------------------------------------------------------------
    // gamepad state
    // ------------------------------------------------------------------

    class GamepadState {
    public:
        InputDigitalState<EGamepadButton> m_buttons{};

        InputAxisState<float2> m_left_stick{};
        InputAxisState<float2> m_right_stick{};

        InputAxisState<float> m_left_trigger{};
        InputAxisState<float> m_right_trigger{};

        std::size_t m_controller_index{0u};

        float m_left_rumble{0.0f};
        float m_right_rumble{0.0f};

        bool m_enable_filtered_inputs: 1 {true};

        bool m_connected: 1 {false};
        bool m_was_connected: 1 {false};

        bool m_on_connected: 1 {false};
        bool m_on_disconnected: 1 {false};

        void setStatus(std::size_t controller_index, bool connected);

        void setButtonPressed(EGamepadButton button);

        void update(TimeSpan dt);

        void reset();
    };

    // ------------------------------------------------------------------
    // gamepad device
    // ------------------------------------------------------------------

    class GamepadDevice : public IInputDevice {
    public:
        GamepadState m_state{};
        InputDeviceID m_device_id;

        explicit GamepadDevice(InputDeviceID device_id, std::size_t controller_index) noexcept;

        ~GamepadDevice() noexcept override;

        [[nodiscard]] bool isConnected() const noexcept {
            return m_state.m_connected;
        }

        [[nodiscard]] size_t getControllerIndex() const noexcept {
            return m_state.m_controller_index;
        }

        [[nodiscard]] bool isFilteringInputs() const noexcept {
            return m_state.m_enable_filtered_inputs;
        }

        void setFilteringInputs(const bool enabled) noexcept {
            m_state.m_enable_filtered_inputs = enabled;
        }

        // IInputDevice interface:

        [[nodiscard]] const InputDeviceID &getInputDeviceID() const noexcept override {
            return m_device_id;
        }

        void supportedInputKeys(Collector<InputKey> supports_key) const override;

        void postInputMessages(TimeSpan dt, Collector<InputMessage> post_event) override;

        void resetInputState() noexcept override;
    };
}
