module;
#include "pP/Macros.h"
export module engine.app:platform.glfw.input;

import :input.device;
import :input.gamepad;
import :input.keyboard;
import :input.listener;
import :input.mouse;
import :input.player;
import :service.player;

export namespace pP {
    class GlfwInput final : public IInputService, public IPlayerService {
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

        FlatMap<PlayerId, std::unique_ptr<Player>> m_players{};

        // Hot per-frame callbacks (placed near device data for cache locality)
        UpdateCallback m_when_before_updated{};
        UpdateCallback m_when_after_updated{};

        // input feeding (game thread, driven by GLFW callbacks):
        void onKey(int key, int scancode, int action, int mods) noexcept;
        void onChar(unsigned int codepoint) noexcept;
        void onMouseButton(int button, int action, int mods) noexcept;
        void onCursorPos(double x, double y) noexcept;
        void onScroll(double x_offset, double y_offset) noexcept;

    private:
        bool m_gamepads_ever_connected{};
        FlatSet<EKeyboardKey> m_held_keys{};
        FlatSet<EMouseButton> m_held_mouse_buttons{};

        FlatMap<InputDeviceID, PlayerId> m_device_to_player{};

        UnhandledKeyCallback m_when_unhandled_key{};

        void pollGamepads_() noexcept;
        void feedGamepad_(GamepadDevice &gamepad, int joystick_id) noexcept;
        void routeMessage_(const InputMessage &message) noexcept;
        [[nodiscard]] EInputListenerResponse dispatchToGlobalListeners_(const InputMessage &message) noexcept;

    public:
        // Callbacks (cold, set at initialization)
        TriggerCallback m_when_action_started{};
        TriggerCallback m_when_action_triggered{};
        TriggerCallback m_when_action_completed{};

        DeviceCallback m_when_device_connected{};
        DeviceCallback m_when_device_disconnected{};

        PlayerCallback m_when_player_added{};
        PlayerCallback m_when_player_removed{};

    public:
        GlfwInput() noexcept = default;

        [[nodiscard]] static GlfwInput &get() noexcept;

        void initialize();
        void shutdown();

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

        void enumerateInputDevices(Collector<SharedInputDevice> each_device) const noexcept override;

        void supportedInputKeys(Collector<InputKey> supports_key) const override;

        void postInputMessages(TimeSpan dt) override;

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

        // ------------------------------------------------------------------
        // IPlayerService overrides
        // ------------------------------------------------------------------

        [[nodiscard]] safe_ptr<Player> getPlayer(const PlayerId &id) const noexcept override;

        void enumeratePlayers(Collector<safe_ptr<Player>> each_player) const noexcept override;

        [[nodiscard]] safe_ptr<Player> getOrCreateKeyboardPlayer() override;

        [[nodiscard]] safe_ptr<Player> addGamepadPlayer(u32 user_id, u32 controller_index) override;

        [[nodiscard]] bool removePlayer(const PlayerId &id) override;

        [[nodiscard]] PlayerCallback::Handle whenPlayerAdded(PlayerCallback::Event on_added) override;

        [[nodiscard]] PlayerCallback::Handle whenPlayerRemoved(PlayerCallback::Event on_removed) override;

        // Test support: clear all player state (used by test suite isolation)
        void resetPlayers() noexcept;
    };
}
