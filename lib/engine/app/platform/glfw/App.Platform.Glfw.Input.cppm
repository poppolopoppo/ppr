module;

export module engine.app:platform.glfw.input;

import :service.input;

export namespace pP {
    // ------------------------------------------------------------------
    // GLFW input service
    // ------------------------------------------------------------------

    class GlfwInput : public IInputService {
        TriggerCallback m_when_action_started{};
        TriggerCallback m_when_action_triggered{};
        TriggerCallback m_when_action_completed{};

        DeviceCallback m_when_device_connected{};
        DeviceCallback m_when_device_disconnected{};

        UnhandledKeyCallback m_when_unhandled_key{};

        UpdateCallback m_when_updated{};

    public:
        void addInputMapping(SharedInputMapping mapping) override;

        void enumerateInputDevices(Collector<const IInputDevice &> each_device) const noexcept override;

        [[nodiscard]] std::optional<const IInputDevice &> findInputDevice(const InputDeviceID &device_id) const noexcept override;

        [[nodiscard]] const GamepadState &getGamepad() const noexcept override;

        [[nodiscard]] const KeyboardState &getKeyboard() const noexcept override;

        [[nodiscard]] const MouseState &getMouse() const noexcept override;

        [[nodiscard]] bool hasInputListener(const InputListener &listener) const noexcept override;

        [[nodiscard]] bool hasInputMapping(const InputMapping &mapping) const noexcept override;

        bool popInputListener(const InputListener &listener) override;

        void postInputMessages(TimeSpan dt) override;

        void pushInputListener(SharedInputListener listener) override;

        bool removeInputMapping(const InputMapping &mapping) override;

        void resetInputState() noexcept override;

        void supportedInputKeys(Collector<InputKey> supports_key) const override;

        [[nodiscard]] TriggerCallback::Handle whenActionStarted(TriggerCallback::Event on_started) override;

        [[nodiscard]] TriggerCallback::Handle whenActionTriggered(TriggerCallback::Event on_triggered) override;

        [[nodiscard]] TriggerCallback::Handle whenActionCompleted(TriggerCallback::Event on_completed) override;

        [[nodiscard]] DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event on_connected) override;

        [[nodiscard]] DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event on_disconnected) override;

        [[nodiscard]] UnhandledKeyCallback::Handle whenUnhandledKey(UnhandledKeyCallback::Event on_unhandled_key) override;

        [[nodiscard]] UpdateCallback::Handle whenUpdated(UpdateCallback::Event on_update) override;
    };
}
