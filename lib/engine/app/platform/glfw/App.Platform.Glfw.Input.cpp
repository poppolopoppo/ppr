module;

#include "pP/Macros.h"
#include "App.Platform.Glfw.include.hpp"

module engine.app;

import std;
import :platform.glfw.input;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(GlfwInput, info, none);

    /*static*/
    GlfwInput &GlfwInput::get() noexcept {
        static GlfwInput g_instance{};
        return g_instance;
    }

    std::error_code GlfwInput::initialize() {
        m_devices.emplace(InputDeviceID{0u}, safe_ptr<const IInputDevice>{&m_keyboard});
        m_devices.emplace(InputDeviceID{1u}, safe_ptr<const IInputDevice>{&m_mouse});
        for (GamepadDevice &gamepad: m_gamepads) {
            m_devices.emplace(gamepad.getInputDeviceID(), safe_ptr<const IInputDevice>{&gamepad});
        }

        return default_value_v;
    }

    std::error_code GlfwInput::shutdown() {
        m_player_service.reset();
        m_graph.clear();
        m_devices.clear();
        m_held_keys.clear();
        m_held_mouse_buttons.clear();

        return default_value_v;
    }

    // ------------------------------------------------------------------
    // input service devices
    // ------------------------------------------------------------------

    const KeyboardState &GlfwInput::getKeyboard() const noexcept {
        return m_keyboard.m_state;
    }

    const MouseState &GlfwInput::getMouse() const noexcept {
        return m_mouse.m_state;
    }

    const GamepadState &GlfwInput::getGamepad(const int controller_index) const noexcept {
        return m_gamepads[controller_index].m_state;
    }

    SharedInputDevice GlfwInput::getInputDevice(const InputDeviceID &device_id) const noexcept {
        const auto it = m_devices.find(device_id);
        return it != m_devices.end() ? it->second : SharedInputDevice{};
    }

    std::error_code GlfwInput::enumerateInputDevices(const Collector<SharedInputDevice> each_device) const noexcept {
        return each_device.append(m_devices.values());
    }

    std::error_code GlfwInput::supportedInputKeys(const Collector<InputKey> supports_key) const {
        for (const SharedInputDevice &device: m_devices.values()) {
            if (const auto err = device->supportedInputKeys(supports_key)) [[unlikely]] {
                return err;
            }
        }
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // input service events
    // ------------------------------------------------------------------

    [[nodiscard]] static std::optional<EKeyboardKey> glfwToKeyboardKey_(const int key) noexcept {
        if (key >= GLFW_KEY_SPACE && key <= 126) {
            if (key == GLFW_KEY_SPACE) {
                return EKeyboardKey::space;
            }
            if (key >= 'A' && key <= 'Z') {
                return static_cast<EKeyboardKey>(key + 32);
            }
            return static_cast<EKeyboardKey>(key);
        }

        switch (key) {
            case GLFW_KEY_ESCAPE: return EKeyboardKey::escape;
            case GLFW_KEY_ENTER: return EKeyboardKey::enter;
            case GLFW_KEY_TAB: return EKeyboardKey::tab;
            case GLFW_KEY_BACKSPACE: return EKeyboardKey::backspace;
            case GLFW_KEY_INSERT: return EKeyboardKey::insert;
            case GLFW_KEY_DELETE: return EKeyboardKey::delete_;
            case GLFW_KEY_RIGHT: return EKeyboardKey::right_arrow;
            case GLFW_KEY_LEFT: return EKeyboardKey::left_arrow;
            case GLFW_KEY_DOWN: return EKeyboardKey::down_arrow;
            case GLFW_KEY_UP: return EKeyboardKey::up_arrow;
            case GLFW_KEY_PAGE_UP: return EKeyboardKey::page_up;
            case GLFW_KEY_PAGE_DOWN: return EKeyboardKey::page_down;
            case GLFW_KEY_HOME: return EKeyboardKey::home;
            case GLFW_KEY_END: return EKeyboardKey::end;
            case GLFW_KEY_CAPS_LOCK: return EKeyboardKey::caps_lock;
            case GLFW_KEY_NUM_LOCK: return EKeyboardKey::num_lock;
            case GLFW_KEY_SCROLL_LOCK: return EKeyboardKey::scroll_lock;
            case GLFW_KEY_PAUSE: return EKeyboardKey::pause;
            case GLFW_KEY_PRINT_SCREEN: return EKeyboardKey::print_screen;
            case GLFW_KEY_LEFT_SHIFT: return EKeyboardKey::left_shift;
            case GLFW_KEY_RIGHT_SHIFT: return EKeyboardKey::right_shift;
            case GLFW_KEY_LEFT_CONTROL: return EKeyboardKey::left_control;
            case GLFW_KEY_RIGHT_CONTROL: return EKeyboardKey::right_control;
            case GLFW_KEY_LEFT_ALT: return EKeyboardKey::left_alt;
            case GLFW_KEY_RIGHT_ALT: return EKeyboardKey::right_alt;
            case GLFW_KEY_LEFT_SUPER: return EKeyboardKey::left_super;
            case GLFW_KEY_RIGHT_SUPER: return EKeyboardKey::right_super;
            case GLFW_KEY_F1: return EKeyboardKey::f1;
            case GLFW_KEY_F2: return EKeyboardKey::f2;
            case GLFW_KEY_F3: return EKeyboardKey::f3;
            case GLFW_KEY_F4: return EKeyboardKey::f4;
            case GLFW_KEY_F5: return EKeyboardKey::f5;
            case GLFW_KEY_F6: return EKeyboardKey::f6;
            case GLFW_KEY_F7: return EKeyboardKey::f7;
            case GLFW_KEY_F8: return EKeyboardKey::f8;
            case GLFW_KEY_F9: return EKeyboardKey::f9;
            case GLFW_KEY_F10: return EKeyboardKey::f10;
            case GLFW_KEY_F11: return EKeyboardKey::f11;
            case GLFW_KEY_F12: return EKeyboardKey::f12;
            default: return std::nullopt;
        }
    }

    void GlfwInput::onKey(const int key, const int, const int action, const int) noexcept {
        const std::optional<EKeyboardKey> mapped = glfwToKeyboardKey_(key);
        if (not mapped.has_value()) {
            return;
        }

        if (action == GLFW_RELEASE) {
            m_held_keys.erase(*mapped);
        } else {
            m_held_keys.insert(*mapped);
        }
    }

    void GlfwInput::onChar(const unsigned int codepoint) noexcept {
        if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
            return;
        }
        m_keyboard.m_state.addCharacterInput(static_cast<hal::native::char_t>(codepoint));
    }

    void GlfwInput::onMouseButton(const int button, const int action, const int) noexcept {
        if (button < 0 || button > 4) {
            return;
        }

        const auto mapped = static_cast<EMouseButton>(button);
        if (action == GLFW_RELEASE) {
            m_held_mouse_buttons.erase(mapped);
        } else {
            m_held_mouse_buttons.insert(mapped);
        }
    }

    void GlfwInput::onCursorPos(const double x, const double y) noexcept {
        m_mouse.m_state.setCursorPos(int2{static_cast<int>(x), static_cast<int>(y)});
    }

    void GlfwInput::onScroll(const double x_offset, const double y_offset) noexcept {
        m_mouse.m_state.addWheelDeltaX(static_cast<int>(x_offset));
        m_mouse.m_state.addWheelDeltaY(static_cast<int>(y_offset));
    }

    std::error_code GlfwInput::pollGamepads_() noexcept {
        if (!m_gamepads_ever_connected) [[likely]] {
            return default_value_v;
        }

        for (std::size_t i = 0; i < m_gamepads.size(); ++i) {
            GamepadDevice &gamepad = m_gamepads[i];
            const int joystick_id = static_cast<int>(i);
            const bool present = ::glfwJoystickPresent(joystick_id) == GLFW_TRUE;
            const bool was_connected = gamepad.isConnected();

            if (present && !was_connected) {
                m_gamepads_ever_connected = true;
                PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_player_service->addGamepadPlayer(static_cast<u32>(i)));

                gamepad.m_state.setStatus(i, true);
                feedGamepad_(gamepad, joystick_id);

                PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_when_device_connected(*this, gamepad));
            } else if (!present && was_connected) {
                gamepad.m_state.setStatus(i, false);
                PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_when_device_disconnected(*this, gamepad));

                if (const auto player_id = m_graph.findPlayerForDevice(gamepad.getInputDeviceID())) {
                    PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_player_service->removePlayer(*player_id));
                }
            } else if (present) {
                feedGamepad_(gamepad, joystick_id);
            }
        }

        return default_value_v;
    }

    void GlfwInput::feedGamepad_(GamepadDevice &gamepad, const int joystick_id) noexcept {
        int axes_count = 0;
        const float *const axes = ::glfwGetJoystickAxes(joystick_id, &axes_count);
        if (axes != nullptr) {
            if (axes_count > 0) {
                gamepad.m_state.m_left_stick.set(float2{axes[0], axes[1]});
            }
            if (axes_count > 2) {
                gamepad.m_state.m_right_stick.set(float2{axes[2], axes[3]});
            }
            if (axes_count > 4) {
                gamepad.m_state.m_left_trigger.set(axes[4]);
            }
            if (axes_count > 5) {
                gamepad.m_state.m_right_trigger.set(axes[5]);
            }
        }

        int buttons_count = 0;
        if (const unsigned char *const buttons = ::glfwGetJoystickButtons(joystick_id, &buttons_count)) {
            for (int b = 0; b < buttons_count && b <= 13; ++b) {
                if (buttons[b] == GLFW_PRESS) {
                    gamepad.m_state.m_buttons.setPressed(static_cast<EGamepadButton>(b));
                }
            }
        }
    }

    EInputListenerResponse GlfwInput::dispatchToGlobalListeners_(const InputMessage &message) noexcept {
        return m_global_listener.postKeyEvent(message);
    }

    std::error_code GlfwInput::routeMessage_(const InputMessage &message) noexcept {
        SharedPlayer player;
        if (const auto player_id = m_graph.findPlayerForDevice(message.m_device_id)) {
            player = m_graph.getPlayer(*player_id);
        }

        if (player) {
            if (const EInputListenerResponse response = player->getListener().postKeyEvent(message);
                response == EInputListenerResponse::unhandled) {
                if (dispatchToGlobalListeners_(message) == EInputListenerResponse::unhandled) {
                    PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_when_unhandled_key(*this, message.m_key));
                }
            }
            return default_value_v;
        }

        if (dispatchToGlobalListeners_(message) == EInputListenerResponse::unhandled) {
            PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_when_unhandled_key(*this, message.m_key));
        }

        return default_value_v;
    }

    std::error_code GlfwInput::postInputMessages(const TimeSpan dt) {
        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_when_before_updated(*this, dt));

        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, pollGamepads_());

        for (const EKeyboardKey key: m_held_keys) {
            m_keyboard.m_state.m_keys.setPressed(key);
        }
        for (const EMouseButton button: m_held_mouse_buttons) {
            m_mouse.m_state.m_buttons.setPressed(button);
        }

        const auto route = [&](const InputMessage &message) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(GlfwInput, routeMessage_(message));
            return default_value_v;
        };

        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_keyboard.postInputMessages(dt, route));

        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_mouse.postInputMessages(dt, route));

        for (GamepadDevice &gamepad: m_gamepads) {
            PPR_RETURN_ERROR_ON_FAIL(GlfwInput, gamepad.postInputMessages(dt, route));
        }

        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_when_after_updated(*this, dt));

        return default_value_v;
    }

    void GlfwInput::resetInputState() noexcept {
        m_keyboard.resetInputState();
        m_mouse.resetInputState();

        for (GamepadDevice &gamepad: m_gamepads) {
            gamepad.resetInputState();
        }
    }

    // ------------------------------------------------------------------
    // input service listeners
    // ------------------------------------------------------------------

    bool GlfwInput::hasInputListener(const InputListener &listener) const noexcept {
        return std::ranges::contains(
            m_listeners, &listener,
            [](const SharedInputListener &shared_listener) noexcept -> const InputListener * {
                return shared_listener.get();
            });
    }


    void GlfwInput::pushInputListener(SharedInputListener listener) {
        m_listeners.push_back(std::move(listener));
    }

    bool GlfwInput::popInputListener(const InputListener &listener) {
        const auto it = std::ranges::find(
            m_listeners, &listener,
            [](const SharedInputListener &shared_listener) noexcept -> const InputListener * {
                return shared_listener.get();
            });

        if (m_listeners.end() != it) [[likely]] {
            m_listeners.erase(it);
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    // input service mappings
    // ------------------------------------------------------------------

    bool GlfwInput::hasGlobalInputMapping(const InputMapping &mapping) const noexcept {
        return m_global_listener.hasInputMapping(mapping);
    }

    void GlfwInput::addGlobalInputMapping(SharedInputMapping mapping, const int priority) {
        m_global_listener.addMapping(std::move(mapping), priority);
    }

    bool GlfwInput::removeGlobalInputMapping(const InputMapping &mapping) {
        return m_global_listener.removeMapping(mapping);
    }

    // ------------------------------------------------------------------
    // input service callbacks
    // ------------------------------------------------------------------

    auto GlfwInput::whenDeviceConnected(DeviceCallback::Event on_connected) -> DeviceCallback::Handle {
        return m_when_device_connected.add(std::move(on_connected));
    }

    auto GlfwInput::whenDeviceDisconnected(DeviceCallback::Event on_disconnected) -> DeviceCallback::Handle {
        return m_when_device_disconnected.add(std::move(on_disconnected));
    }

    auto GlfwInput::whenActionStarted(TriggerCallback::Event on_started) -> TriggerCallback::Handle {
        return m_when_action_started.add(std::move(on_started));
    }

    auto GlfwInput::whenActionTriggered(TriggerCallback::Event on_started) -> TriggerCallback::Handle {
        return m_when_action_triggered.add(std::move(on_started));
    }

    auto GlfwInput::whenActionCompleted(TriggerCallback::Event on_completed) -> TriggerCallback::Handle {
        return m_when_action_completed.add(std::move(on_completed));
    }

    auto GlfwInput::whenUnhandledKey(UnhandledKeyCallback::Event on_unhandled_key) -> UnhandledKeyCallback::Handle {
        return m_when_unhandled_key.add(std::move(on_unhandled_key));
    }

    auto GlfwInput::whenBeforeUpdated(UpdateCallback::Event on_update) -> UpdateCallback::Handle {
        return m_when_before_updated.add(std::move(on_update));
    }

    auto GlfwInput::whenAfterUpdated(UpdateCallback::Event on_update) -> UpdateCallback::Handle {
        return m_when_after_updated.add(std::move(on_update));
    }
}
