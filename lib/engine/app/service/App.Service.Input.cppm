module;

export module engine.app:service.input;

import engine.core;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // input service interface
    // ------------------------------------------------------------------

    struct InputActionEvent;
    struct InputKey;

    class IInputDevice;
    using InputDeviceID = Numeric<u32, IInputDevice>;
    using SharedInputDevice = safe_ptr<const IInputDevice>;

    class InputListener;
    using SharedInputListener = safe_ptr<InputListener>;

    class InputMapping;
    using SharedInputMapping = safe_ptr<const InputMapping>;

    class KeyboardState;
    class GamepadState;
    class MouseState;

    class IInputService : public virtual IService {
    public:
        // devices:
        [[nodiscard]] virtual const KeyboardState &
        getKeyboard() const noexcept = 0;

        [[nodiscard]] virtual const MouseState &
        getMouse() const noexcept = 0;

        [[nodiscard]] virtual const GamepadState &
        getGamepad(int controller_index) const noexcept = 0;

        [[nodiscard]] virtual SharedInputDevice
        getInputDevice(const InputDeviceID &device_id) const noexcept = 0;

        [[nodiscard]] virtual std::error_code enumerateInputDevices(Collector<SharedInputDevice> each_device) const noexcept = 0;

        [[nodiscard]] virtual std::error_code supportedInputKeys(Collector<InputKey> supports_key) const = 0;

        // input events:
        [[nodiscard]] virtual std::error_code postInputMessages(TimeSpan dt) = 0;

        virtual void resetInputState() noexcept = 0;

        // listeners:
        [[nodiscard]] virtual bool hasInputListener(const InputListener &listener) const noexcept = 0;

        virtual void pushInputListener(SharedInputListener listener) = 0;

        virtual bool popInputListener(const InputListener &listener) = 0;

        // mappings:
        [[nodiscard]] virtual bool hasGlobalInputMapping(const InputMapping &mapping) const noexcept = 0;

        virtual void addGlobalInputMapping(SharedInputMapping mapping, int priority = 0) = 0;

        virtual bool removeGlobalInputMapping(const InputMapping &mapping) = 0;

        // callbacks:
        using DeviceCallback = Callback<std::error_code (const IInputService &input, const IInputDevice &device)>;

        [[nodiscard]] virtual DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event on_connected) = 0;

        [[nodiscard]] virtual DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event on_disconnected) = 0;

        using TriggerCallback = Callback<std::error_code (const IInputService &input, const InputActionEvent &event, const InputKey &trigger)>;

        [[nodiscard]] virtual TriggerCallback::Handle whenActionStarted(TriggerCallback::Event on_started) = 0;

        [[nodiscard]] virtual TriggerCallback::Handle whenActionTriggered(TriggerCallback::Event on_triggered) = 0;

        [[nodiscard]] virtual TriggerCallback::Handle whenActionCompleted(TriggerCallback::Event on_completed) = 0;

        using UnhandledKeyCallback = Callback<std::error_code (const IInputService &input, const InputKey &input_key)>;

        [[nodiscard]] virtual UnhandledKeyCallback::Handle whenUnhandledKey(UnhandledKeyCallback::Event on_unhandled_key) = 0;

        using UpdateCallback = Callback<std::error_code (const IInputService &input, TimeSpan dt)>;

        [[nodiscard]] virtual UpdateCallback::Handle whenBeforeUpdated(UpdateCallback::Event on_update) = 0;

        [[nodiscard]] virtual UpdateCallback::Handle whenAfterUpdated(UpdateCallback::Event on_update) = 0;
    };

    extern template class Callback<std::error_code (const IInputService &, const IInputDevice &)>;
    extern template class Callback<std::error_code (const IInputService &, const InputActionEvent &, const InputKey &)>;
    extern template class Callback<std::error_code (const IInputService &, const InputKey &)>;
    extern template class Callback<std::error_code (const IInputService &, TimeSpan)>;
}
