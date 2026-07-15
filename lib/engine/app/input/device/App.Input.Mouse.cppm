module;
#include "pP/Macros.h"
export module engine.app:input.mouse;

import engine.core;
import :input.key;
import :input.device;

export namespace pP {
    // ------------------------------------------------------------------
    // mouse state
    // ------------------------------------------------------------------

    class MouseState {
    public:
        InputDigitalState<EMouseButton> m_buttons{};

        InputAxisState<float2> m_cursor_pos{};
        InputAxisState<float> m_wheel_x{};
        InputAxisState<float> m_wheel_y{};

        bool m_enable_filtered_inputs: 1 {true};

        void setButtonPressed(EMouseButton button);

        void setCursorPos(const int2 &position) noexcept;

        void addWheelDeltaX(int delta) noexcept;
        void addWheelDeltaY(int delta) noexcept;

        void setWheelX(int delta) noexcept;
        void setWheelY(int delta) noexcept;

        void update(TimeSpan dt);

        void reset();
    };

    // ------------------------------------------------------------------
    // gamepad device
    // ------------------------------------------------------------------

    class MouseDevice : public IInputDevice {
    public:
        MouseState m_state{};
        InputDeviceID m_device_id;

        explicit MouseDevice(InputDeviceID device_id) noexcept;

        ~MouseDevice() noexcept override;

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
