module;
#include "pP/Macros.h"
export module engine.app:platform.glfw.input;

import :input.device;
import :input.gamepad;
import :input.keyboard;
import :input.listener;
import :input.mouse;
import :input.player;
import :player.graph;
import :service.player;

export namespace pP {
    class GlfwInput final : public IInputService {
    public:
        // Hot data (game thread)
        KeyboardDevice m_keyboard{InputDeviceID{0u}};
        MouseDevice m_mouse{InputDeviceID{1u}};
        std::array<GamepadDevice, 4u> m_gamepads{
            GamepadDevice{InputDeviceID{2u}, 0u},
            GamepadDevice{InputDeviceID{3u}, 1u},
            GamepadDevice{InputDeviceID{4u}, 2u},
            GamepadDevice{InputDeviceID{5u}, 3u},
        };

        FlatMap<InputDeviceID, SharedInputDevice> m_devices{};

        InputListener m_global_listener{};
        Array<SharedInputListener> m_listeners{};

        PlayerGraph m_graph{};

        // Hot per-frame callbacks (placed near device data for cache locality)
        UpdateCallback m_when_before_updated{};
        UpdateCallback m_when_after_updated{};

        // Held key/mouse button state (hot, accessed per-frame)
        FlatSet<EKeyboardKey> m_held_keys{};
        FlatSet<EMouseButton> m_held_mouse_buttons{};

        // input feeding (game thread, driven by GLFW callbacks):
        void onKey(int key, int scancode, int action, int mods) noexcept;
        void onChar(unsigned int codepoint) noexcept;
        void onMouseButton(int button, int action, int mods) noexcept;
        void onCursorPos(double x, double y) noexcept;
        void onScroll(double x_offset, double y_offset) noexcept;

    private:
        bool m_gamepads_ever_connected{};

        UnhandledKeyCallback m_when_unhandled_key{};

        std::error_code pollGamepads_() noexcept;
        void feedGamepad_(GamepadDevice &gamepad, int joystick_id) noexcept;
        std::error_code routeMessage_(const InputMessage &message) noexcept;
        [[nodiscard]] EInputListenerResponse dispatchToGlobalListeners_(const InputMessage &message) noexcept;
        [[nodiscard]] EInputListenerResponse dispatchToPushedListeners_(const InputMessage &message) noexcept;

    public:
        // Callbacks (cold, set at initialization)
        TriggerCallback m_when_action_started{};
        TriggerCallback m_when_action_triggered{};
        TriggerCallback m_when_action_completed{};

        DeviceCallback m_when_device_connected{};
        DeviceCallback m_when_device_disconnected{};

        safe_ptr<IPlayerService> m_player_service{};

    public:
        GlfwInput() noexcept = default;

        [[nodiscard]] static GlfwInput &get() noexcept;

        std::error_code initialize();
        std::error_code shutdown();

        // ------------------------------------------------------------------
        // IInputService overrides
        // ------------------------------------------------------------------

        [[nodiscard]] const KeyboardState &
        getKeyboard() const noexcept override;

        [[nodiscard]] const MouseState &
        getMouse() const noexcept override;

        [[nodiscard]] const GamepadState &
        getGamepad(int controller_index) const noexcept override;

        [[nodiscard]] SharedInputDevice
        getInputDevice(const InputDeviceID &device_id) const noexcept override;

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
    };
}
