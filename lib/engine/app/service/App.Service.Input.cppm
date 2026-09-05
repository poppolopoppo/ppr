module;

export module engine.app:service.input;

import engine.core;
import engine.math;
import std;

export namespace pP {
    enum class EGamepadButton : u8;
    enum class EMouseButton : u8;
    enum class EKeyboardKey : u8;
    // ------------------------------------------------------------------
    // input service interface
    // ------------------------------------------------------------------

    struct InputAction;
    struct InputActionEvent;
    struct InputActionKeyMapping;
    struct InputKey;

    class IInputDevice;
    using InputDeviceID = Numeric<u64, IInputDevice>;
    using SharedInputDevice = safe_ptr<const IInputDevice>;

    class InputContext;
    using SharedInputContext = safe_ptr<const InputContext>;

    class InputListener;
    using SharedInputListener = safe_ptr<const InputListener>;

    class InputMapping;
    using SharedInputMapping = safe_ptr<const InputMapping>;

    class KeyboardDevice;
    class GamepadDevice;
    class MouseDevice;

    using GamepadControllerID = Numeric<int, IInputDevice>;

    class IInputService : public virtual IService {
    public:
        // devices:
        [[nodiscard]] virtual const KeyboardDevice &
        getKeyboard() const noexcept = 0;

        [[nodiscard]] virtual const MouseDevice &
        getMouse() const noexcept = 0;

        [[nodiscard]] virtual const GamepadDevice &
        getGamepad(int controller_index) const noexcept = 0;

        [[nodiscard]] virtual SharedInputDevice
        getInputDeviceByID(const InputDeviceID &device_id) const noexcept = 0;

        [[nodiscard]] virtual std::error_code enumerateInputDevices(Collector<SharedInputDevice> each_device) const noexcept = 0;

        [[nodiscard]] virtual std::error_code enumerateInputKeysSupported(Collector<InputKey> supports_key) const = 0;

        // contexts:
        [[nodiscard]] virtual InputContext &getGlobalInputContext() noexcept = 0;

        [[nodiscard]] virtual bool hasInputContext(const InputContext &context) const noexcept = 0;

        virtual void addInputContext(SharedInputContext context) = 0;

        virtual bool removeInputContext(const InputContext &context) = 0;

        virtual void assignInputContextToDevice(InputDeviceID device_id, SharedInputContext context) = 0;

        virtual void clearInputContextDeviceAssignments() = 0;

        [[nodiscard]] virtual std::error_code enumerateInputContexts(Collector<InputContext> each_context) const = 0;

        [[nodiscard]] virtual std::error_code enumerateInputContextDeviceAssignments(Collector<IInputDevice, InputContext> each_assignment) const = 0;

        // input events:
        [[nodiscard]] virtual std::error_code pollInputDevices(TimeSpan dt) = 0;

        virtual void resetInputDevices() noexcept = 0;

        virtual void postKeyboardCharacterInput(const InputContext &context, hal::native::char_t codepoint) = 0;

        virtual void postKeyboardKeyPressed(const InputContext &context, EKeyboardKey key, bool pressed) = 0;

        virtual void postMouseButtonPressed(const InputContext &context, EMouseButton button, bool pressed) = 0;

        virtual void postMouseCursorPosition(const InputContext &context, const float2 &absolute_pos) = 0;

        virtual void postMouseScrollWheel(const InputContext &context, const float2 &delta) = 0;

        // callbacks:
        using DeviceCallback = BroadcastCallback<std::error_code (const IInputDevice &device)>;

        [[nodiscard]] virtual DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event on_connected) = 0;

        [[nodiscard]] virtual DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event on_disconnected) = 0;
    };

    extern template class BroadcastCallback<std::error_code (const IInputDevice &)>;
    extern template class BroadcastCallback<std::error_code (const InputActionEvent &, const InputKey &)>;
    extern template class BroadcastCallback<std::error_code (const InputKey &)>;
}
