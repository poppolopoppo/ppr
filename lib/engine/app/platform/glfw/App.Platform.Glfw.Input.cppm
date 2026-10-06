module;
#include "pP/Macros.h"
export module engine.app:platform.glfw.input;

import :input.batch;
import :input.device;
import :input.listener;

import engine.core;
import std;

export namespace pP {
    class GlfwInput final : public IInputService {
        std::error_code pollInputGamepad_(const InputContext &context, GamepadDevice &gamepad) const;

    public:
        // Hot data (game thread)
        KeyboardDevice m_keyboard{InputDeviceID{"Keyboard"}};
        MouseDevice m_mouse{InputDeviceID{"MouseJoy"}};
        std::array<GamepadDevice, 4u> m_gamepads{
            GamepadDevice{InputDeviceID{"GamePad0"}},
            GamepadDevice{InputDeviceID{"GamePad1"}},
            GamepadDevice{InputDeviceID{"GamePad2"}},
            GamepadDevice{InputDeviceID{"GamePad3"}},
        };

        InputContext m_global_context{};

        FlatSet<SharedInputContext> m_all_contexts{};
        FlatMap<InputDeviceID, SharedInputDevice> m_devices_by_id{};
        FlatMap<InputDeviceID, SharedInputContext> m_context_by_device{};

        TimePoint m_timestamp{};
        TimeSpan m_delta_time{};

        // Owned completed-batch boundary (P0): native pumps collect here
        // without client dispatch; update() dispatches exactly once after
        // pumping. Off by default, so direct posts keep immediate behavior.
        PendingInputBatch m_native_batch{};
        bool m_collecting_native_batch{false};

        bool m_gamepads_ever_connected: 1 {false};

        // Callbacks (cold, set at initialization)
        DeviceCallback m_when_device_connected{};
        DeviceCallback m_when_device_disconnected{};

        GlfwInput() noexcept = default;

        std::error_code initialize();

        std::error_code shutdown();

        // ------------------------------------------------------------------
        // IInputService overrides
        // ------------------------------------------------------------------

        [[nodiscard]] const KeyboardDevice &getKeyboard() const noexcept override;

        [[nodiscard]] const MouseDevice &getMouse() const noexcept override;

        [[nodiscard]] const GamepadDevice &getGamepad(int controller_index) const noexcept override;

        [[nodiscard]] SharedInputDevice getInputDeviceByID(const InputDeviceID &device_id) const noexcept override;

        [[nodiscard]] std::error_code enumerateInputDevices(
            Collector<SharedInputDevice> each_device) const noexcept override;

        [[nodiscard]] std::error_code enumerateInputKeysSupported(Collector<InputKey> supports_key) const override;

        // contexts:
        [[nodiscard]] InputContext &getGlobalInputContext() noexcept override;

        [[nodiscard]] bool hasInputContext(const InputContext &context) const noexcept override;

        void addInputContext(SharedInputContext context) override;

        bool removeInputContext(const InputContext &context) override;

        void assignInputContextToDevice(InputDeviceID device_id, SharedInputContext context) override;

        void clearInputContextDeviceAssignments() override;

        [[nodiscard]] std::error_code enumerateInputContexts(Collector<InputContext> each_context) const override;

        [[nodiscard]] std::error_code enumerateInputContextDeviceAssignments(
            Collector<IInputDevice, InputContext> each_assignment) const override;

        // input events:
        [[nodiscard]] std::error_code pollInputDevices(TimeSpan dt) override;

        void resetInputDevices() noexcept override;

        void postKeyboardCharacterInput(const InputContext &context, hal::native::char_t codepoint) override;

        void postKeyboardKeyPressed(const InputContext &context, EKeyboardKey key, bool pressed) override;

        void postMouseButtonPressed(const InputContext &context, EMouseButton button, bool pressed) override;

        void postMouseCursorPosition(const InputContext &context, const float2 &absolute_pos) override;

        void postMouseScrollWheel(const InputContext &context, const float2 &delta) override;

        // callbacks:
        [[nodiscard]] DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event on_connected) override;

        [[nodiscard]] DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event on_disconnected) override;

        // P0 completed batches: redirect pump-time posts into the owned
        // batch (no client dispatch) and replay them exactly once at the
        // defined advancement point. Concrete-type API (not IInputService):
        // the platform holds the concrete GlfwInput, so no interface change.
        // The appliers run the deferred client halves of focus + hotplug
        // control records, in collection order with the input replay.
        [[nodiscard]] std::error_code dispatchCollectedBatches(
            PendingFocusApplier &&focus_applier = {}, PendingDeviceApplier &&device_applier = {});

        void setNativeBatchCollection(bool collecting) noexcept;

        // Collection routing for window-callback control records (focus):
        // the live input service while a native collection is open, else
        // null — callers fire immediately on the null path.
        [[nodiscard]] static safe_ptr<GlfwInput> collectingNativeBatchTarget() noexcept;

        // Deferred-half entry points for hotplug control records: fire the
        // connected/disconnected broadcast for a registry change the
        // platform half already applied at collection time.
        [[nodiscard]] std::error_code notifyDeviceConnected(const IInputDevice &device);

        [[nodiscard]] std::error_code notifyDeviceDisconnected(const IInputDevice &device);
    };
}
