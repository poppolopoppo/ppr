module;
#include "pP/Macros.h"

export module engine.app:input.replay;

import engine.core;
import engine.math;
import std;

import :service.input;
import :input.device;
import :input.key;
import :input.keyboard;
import :input.listener;
import :input.mapping;
import :input.mouse;
import :input.gamepad;

export namespace pP {
    enum class EInputReplayMode : u8 {
        replay = 0,
        record,
        both,
    };

    class InputReplay final : public IInputService {
    public:
        InputReplay() noexcept;
        ~InputReplay() noexcept;

        void setParent(safe_ptr<IInputService> parent) noexcept;   // pushes capture listener; records live input in record/both
        void detachParent() noexcept;

        void inject(const InputMessage &msg);
        void injectKey(const InputKey &key, bool down);
        void injectCursorDelta(float2 delta);
        void injectWheel(float delta_y);
        void injectGamepadStick(int idx, float2 value);
        void injectGamepadButton(EGamepadButton, bool down);
        void injectGamepadTrigger(int idx, float value);

        void setMode(EInputReplayMode mode) noexcept;
        void startRecording() noexcept;
        void stopRecording() noexcept;
        [[nodiscard]] const Array<InputMessage> &recording() const noexcept;
        void clearRecording() noexcept;
        void loadRecording(Array<InputMessage> frames) noexcept;   // for shipped replay

        [[nodiscard]] const KeyboardState &
        getKeyboard() const noexcept override;

        [[nodiscard]] const MouseState &
        getMouse() const noexcept override;

        [[nodiscard]] const GamepadState &
        getGamepad(int controller_index) const noexcept override;

        // Returns a device handle borrowing member state (m_keyboard/m_mouse/m_gamepad);
        // the returned SharedInputDevice must not outlive this InputReplay.
        [[nodiscard]] SharedInputDevice
        getInputDevice(const InputDeviceID &device_id) const noexcept override;

        // Yields device handles borrowing member state (m_keyboard/m_mouse/m_gamepad);
        // the returned SharedInputDevices must not outlive this InputReplay.
        [[nodiscard]] std::error_code enumerateInputDevices(Collector<SharedInputDevice> each_device) const noexcept override;

        [[nodiscard]] std::error_code supportedInputKeys(Collector<InputKey> supports_key) const override;

        [[nodiscard]] std::error_code postInputMessages(TimeSpan dt) override;

        void resetInputState() noexcept override;

        // listeners:
        [[nodiscard]] bool hasInputListener(const InputListener &listener) const noexcept override;

        void pushInputListener(SharedInputListener listener) override;

        bool popInputListener(const InputListener &listener) override;

        // mappings:
        [[nodiscard]] bool hasGlobalInputMapping(const InputMapping &mapping) const noexcept override;

        void addGlobalInputMapping(SharedInputMapping mapping, int priority) override;

        bool removeGlobalInputMapping(const InputMapping &mapping) override;

        // callbacks:
        [[nodiscard]] DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event on_connected) override;

        [[nodiscard]] DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event on_disconnected) override;

        [[nodiscard]] TriggerCallback::Handle whenActionStarted(TriggerCallback::Event on_started) override;

        [[nodiscard]] TriggerCallback::Handle whenActionTriggered(TriggerCallback::Event on_triggered) override;

        [[nodiscard]] TriggerCallback::Handle whenActionCompleted(TriggerCallback::Event on_completed) override;

        [[nodiscard]] UnhandledKeyCallback::Handle whenUnhandledKey(UnhandledKeyCallback::Event on_unhandled_key) override;

        [[nodiscard]] UpdateCallback::Handle whenBeforeUpdated(UpdateCallback::Event on_update) override;

        [[nodiscard]] UpdateCallback::Handle whenAfterUpdated(UpdateCallback::Event on_update) override;

    private:
        safe_ptr<IInputService> m_parent{};
        EInputReplayMode m_mode{EInputReplayMode::replay};

        InputListener m_global_listener{};          // consumers register global mappings here
        Array<SharedInputListener> m_listeners{};   // consumers push listeners here
        InputListener m_capture_listener{};         // pushed onto parent; captures live input in record/both

        Array<InputMessage> m_injected{};           // replay queue (not synchronized; single-threaded use expected)
                                                    // flat list, no frame association — drained in full on the first postInputMessages() (burst-only)
        Array<InputMessage> m_recorded{};           // recording buffer (not synchronized; single-threaded use expected)

        KeyboardDevice m_keyboard{InputDeviceID{0u}};
        MouseDevice    m_mouse{InputDeviceID{1u}};
        GamepadDevice  m_gamepad{InputDeviceID{2u}, 0u};

        void routeToOwnListeners_(const InputMessage &msg);   // pushed -> global -> unhandled
        void installCaptureListener_() noexcept;
        void uninstallCaptureListener_() noexcept;
    };
}
