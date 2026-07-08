module;

export module engine.app:service.input;

import engine.core;
import :input.device;
import :input.listener;
import :input.mapping;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // input service interface
    // ------------------------------------------------------------------

    class KeyboardState;
    class GamepadState;
    class MouseState;

    class IInputService : public IService {
    public:
        [[nodiscard]] virtual const KeyboardState &
        getKeyboard() const noexcept = 0;

        [[nodiscard]] virtual const GamepadState &
        getGamepad() const noexcept = 0;

        [[nodiscard]] virtual const MouseState &
        getMouse() const noexcept = 0;

        [[nodiscard]] virtual std::optional<const IInputDevice &>
        findInputDevice(const InputDeviceID &device_id) const noexcept = 0;

        virtual void enumerateInputDevices(Collector<const IInputDevice &> each_device) const noexcept = 0;

        virtual void supportedInputKeys(Collector<InputKey> supports_key) const = 0;

        virtual void postInputMessages(TimeSpan dt) = 0;

        virtual void resetInputState() noexcept = 0;

        // listeners:
        [[nodiscard]] virtual bool hasInputListener(const InputListener &listener) const noexcept = 0;

        virtual void pushInputListener(SharedInputListener listener) = 0;

        virtual bool popInputListener(const InputListener &listener) = 0;

        // mappings:
        [[nodiscard]] virtual bool hasInputMapping(const InputMapping &mapping) const noexcept = 0;

        virtual void addInputMapping(SharedInputMapping mapping) = 0;

        virtual bool removeInputMapping(const InputMapping &mapping) = 0;

        // callbacks:
        using DeviceCallback = Callback<void(const IInputService &input, const IInputDevice &device)>;

        [[nodiscard]] virtual DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event on_connected) = 0;

        [[nodiscard]] virtual DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event on_disconnected) = 0;

        using UnhandledKeyCallback = Callback<void(const IInputService &input, const InputKey &input_key)>;

        [[nodiscard]] virtual UnhandledKeyCallback::Handle whenUnhandledKey(UnhandledKeyCallback::Event on_unhandled_key) = 0;

        using UpdateCallback = Callback<void(const IInputService &input, TimeSpan dt)>;

        [[nodiscard]] virtual UpdateCallback::Handle whenUpdated(UpdateCallback::Event on_update) = 0;
    };
}
