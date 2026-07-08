module;
#include "pP/Macros.h"
export module engine.app:input.keyboard;

import engine.core;
import :input.key;
import :input.device;

export namespace pP {
    // ------------------------------------------------------------------
    // keyboard state
    // ------------------------------------------------------------------

    class KeyboardState {
        Stack<wchar_t, 8u> m_characters_queued;
    public:
        InputDigitalState<EKeyboardKey> m_keys{};

        Stack<wchar_t, 8u> m_characters;

        void setKeyPressed(EKeyboardKey button);

        void addCharacterInput(hal::native::char_t character);

        void update(TimeSpan dt);

        void reset();
    };

    // ------------------------------------------------------------------
    // gamepad device
    // ------------------------------------------------------------------

    class KeyboardDevice : public IInputDevice {
    public:
        KeyboardState m_state{};
        const InputDeviceID m_device_id;

        KeyboardDevice() noexcept;

        ~KeyboardDevice() noexcept override;

        // IInputDevice interface:

        [[nodiscard]] const InputDeviceID &getInputDeviceID() const noexcept override {
            return m_device_id;
        }

        void supportedInputKeys(Collector<InputKey> supports_key) const override;

        void postInputMessages(TimeSpan dt, Collector<InputMessage> post_event) override;

        void resetInputState() noexcept override;
    };
}
