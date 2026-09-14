module;

#include "pP/Macros.h"
#include "App.Platform.Glfw.include.hpp"

module engine.app;
import std;

import :input.key;
import :platform.glfw.input;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(GlfwInput, info, none);

    static void glfwJoystickCallback_(int jid, int event);

    static safe_ptr<GlfwInput> g_glfw_input{};

    std::error_code GlfwInput::initialize() {
        PPR_ASSERT(m_devices_by_id.empty() && "trying to initialize GlfwInput with dirty state");

        g_glfw_input.reset(this);
        bool success = false;
        PPR_DEFER {
            if (not success) [[unlikely]] {
                GlfwInput::shutdown();
            }
        };

        m_devices_by_id[m_keyboard.m_device_id] = safe_ptr{&m_keyboard};
        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_when_device_connected(m_keyboard));

        m_devices_by_id[m_mouse.m_device_id] = safe_ptr{&m_mouse};
        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_when_device_connected(m_mouse));

        ::glfwSetJoystickCallback(&glfwJoystickCallback_);

        success = true;
        return default_value_v;
    }

    std::error_code GlfwInput::shutdown() {
        PPR_DEFER {
            m_gamepads_ever_connected = false;
            m_timestamp = {};
            m_delta_time = {};

            m_keyboard.resetInputState();
            m_mouse.resetInputState();

            for (GamepadDevice &gamepad : m_gamepads) {
                gamepad.resetInputState();
            }

            m_devices_by_id.clear();
            m_all_contexts.clear();
            m_context_by_device.clear();
        };

        ::glfwSetJoystickCallback(nullptr);

        g_glfw_input.reset();

        for (const auto &[device_id, device] : m_devices_by_id) {
            PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_when_device_disconnected(*device));
        }

        return default_value_v;
    }

    // ------------------------------------------------------------------
    // input service devices
    // ------------------------------------------------------------------

    const KeyboardDevice &GlfwInput::getKeyboard() const noexcept {
        return m_keyboard;
    }

    const MouseDevice &GlfwInput::getMouse() const noexcept {
        return m_mouse;
    }

    const GamepadDevice &GlfwInput::getGamepad(const int controller_index) const noexcept {
        return m_gamepads[controller_index];
    }

    SharedInputDevice GlfwInput::getInputDeviceByID(const InputDeviceID &device_id) const noexcept {
        const auto it = m_devices_by_id.find(device_id);
        return it != m_devices_by_id.end() ? it->second : SharedInputDevice{};
    }

    std::error_code GlfwInput::enumerateInputDevices(const Collector<SharedInputDevice> each_device) const noexcept {
        return each_device.append(m_devices_by_id.values());
    }

    std::error_code GlfwInput::enumerateInputKeysSupported(const Collector<InputKey> supports_key) const {
        for (const SharedInputDevice &device: m_devices_by_id.values()) {
            PPR_RETURN_ERROR_ON_FAIL(GlfwInput, device->enumerateSupportedInputKeys(supports_key));
        }
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // input service contexts
    // ------------------------------------------------------------------

    InputContext &GlfwInput::getGlobalInputContext() noexcept {
        return m_global_context;
    }

    bool GlfwInput::hasInputContext(const InputContext &context) const noexcept {
        return &m_global_context == &context ||
               m_all_contexts.contains(&context);
    }

    void GlfwInput::addInputContext(SharedInputContext context) { // NOLINT(*-make-member-function-const, *-unnecessary-value-param)
        PPR_ASSERT(context.isValid());
        PPR_ASSERT(not hasInputContext(*context));

        m_all_contexts.emplace(std::move(context));
    }

    bool GlfwInput::removeInputContext(const InputContext &context) {
        return m_all_contexts.erase(&context) > 0u;
    }

    void GlfwInput::assignInputContextToDevice(const InputDeviceID device_id, SharedInputContext context) { // NOLINT(*-unnecessary-value-param)
        PPR_ASSERT(getInputDeviceByID(device_id).isValid());

        if (context.isValid()) {
            m_context_by_device[device_id] = std::move(context);
        } else {
            m_context_by_device.erase(device_id);
        }
    }

    void GlfwInput::clearInputContextDeviceAssignments() {
        m_context_by_device.clear();
    }

    std::error_code GlfwInput::enumerateInputContexts(Collector<InputContext> each_context) const {
        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, each_context(m_global_context));

        for (const SharedInputContext &context: m_all_contexts) {
            PPR_RETURN_ERROR_ON_FAIL(GlfwInput, each_context(*context));
        }
        for (const SharedInputContext &device_context: m_context_by_device.values()) {
            PPR_RETURN_ERROR_ON_FAIL(GlfwInput, each_context(*device_context));
        }
        return default_value_v;
    }

    std::error_code GlfwInput::enumerateInputContextDeviceAssignments(Collector<IInputDevice, InputContext> each_assignment) const {
        for (const auto &[device_id, context]: m_context_by_device) {
            const SharedInputDevice &input_device = m_devices_by_id.at(device_id);
            PPR_RETURN_ERROR_ON_FAIL(GlfwInput, each_assignment(*input_device, *context));
        }
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // input service events
    // ------------------------------------------------------------------

    std::error_code GlfwInput::pollInputDevices(const TimeSpan dt) {
        m_timestamp += dt;
        m_delta_time = dt;

        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_keyboard.pollInputMessages(m_delta_time));
        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, m_mouse.pollInputMessages(m_delta_time));

        for (GamepadDevice &gamepad: m_gamepads) {
            if (gamepad.isConnected()) {
                const auto it = m_context_by_device.find(gamepad.m_device_id);
                const InputContext &gamepad_context = m_context_by_device.end() == it ? m_global_context : *it->second;
                PPR_RETURN_ERROR_ON_FAIL(GlfwInput, pollInputGamepad_(gamepad_context, gamepad));
            }
        }

        return default_value_v;
    }

    void GlfwInput::resetInputDevices() noexcept {
        m_keyboard.resetInputState();
        m_mouse.resetInputState();

        for (GamepadDevice &gamepad: m_gamepads) {
            gamepad.resetInputState();
        }
    }

    void GlfwInput::postKeyboardCharacterInput(const InputContext &context, const hal::native::char_t codepoint) {
        m_keyboard.postKeyboardCharacterInput(context, codepoint);
    }

    void GlfwInput::postKeyboardKeyPressed(const InputContext &context, const EKeyboardKey key, const bool pressed) {
        m_keyboard.postKeyboardKeyPressed(m_delta_time, context, key, pressed);
    }

    void GlfwInput::postMouseButtonPressed(const InputContext &context, const EMouseButton button, const bool pressed) {
        m_mouse.postMouseButtonPressed(m_delta_time, context, button, pressed);
    }

    void GlfwInput::postMouseCursorPosition(const InputContext &context, const float2 &absolute_pos) {
        m_mouse.postMouseCursorPosition(m_delta_time, context, absolute_pos);
    }

    void GlfwInput::postMouseScrollWheel(const InputContext &context, const float2 &delta) {
        m_mouse.postMouseScrollWheel(m_delta_time, context, delta);
    }

    // ------------------------------------------------------------------
    // input service for gamepad handling
    // ------------------------------------------------------------------

    static void glfwJoystickCallback_(const int jid, const int event) {
        if (not ::glfwJoystickIsGamepad(jid)) {
            return;
        }

        GlfwInput &glfw_input = *g_glfw_input;
        GamepadDevice &gamepad = glfw_input.m_gamepads[jid];

        switch (event) {
            case GLFW_CONNECTED:
                gamepad.m_controller_id = GamepadControllerID(safe_narrowing(jid));
                gamepad.m_friendly_name = ::glfwGetGamepadName(jid);

                glfw_input.m_devices_by_id[gamepad.m_device_id].reset(&gamepad);

                PPR_LOG_WARNING_ON_FAIL(GlfwInput, glfw_input.m_when_device_connected(gamepad));
                break;

            case GLFW_DISCONNECTED:
                PPR_LOG_WARNING_ON_FAIL(GlfwInput, glfw_input.m_when_device_disconnected(gamepad));

                glfw_input.m_devices_by_id.erase(gamepad.m_device_id);

                gamepad.m_controller_id = none_v;
                gamepad.m_friendly_name.clear();
                break;

            default:
                PPR_ASSERT(false && "unhandled GLFW joystick callback event");
                break;
        }
    }

    std::error_code GlfwInput::pollInputGamepad_(const InputContext &context, GamepadDevice &gamepad) const {
        if (not gamepad.isConnected()) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        ::GLFWgamepadstate glfw_state{};
        if (not ::glfwGetGamepadState(gamepad.m_controller_id, &glfw_state)) {
            return make_error_code(std::errc::device_or_resource_busy);
        }

        // axes:
        gamepad.postGamepadAxis2DMoved(m_delta_time, context, EGamepadAxis::left_stick, float2(
            glfw_state.axes[GLFW_GAMEPAD_AXIS_LEFT_X],
            glfw_state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]
        ));

        gamepad.postGamepadAxis2DMoved(m_delta_time, context, EGamepadAxis::right_stick, float2(
            glfw_state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X],
            glfw_state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]
        ));

        gamepad.postGamepadAxis1DMoved(m_delta_time, context, EGamepadAxis::left_trigger,
            glfw_state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]);
        gamepad.postGamepadAxis1DMoved(m_delta_time, context, EGamepadAxis::right_trigger,
            glfw_state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]);

        // buttons:
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::start,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_START] == GLFW_PRESS);
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::back,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_BACK] == GLFW_PRESS);

        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::A,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS);
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::B,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS);
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::X,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_X] == GLFW_PRESS);
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::Y,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_Y] == GLFW_PRESS);

        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::dpad_down,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] == GLFW_PRESS);
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::dpad_left,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] == GLFW_PRESS);
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::dpad_right,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] == GLFW_PRESS);
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::dpad_up,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] == GLFW_PRESS);

        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::left_shoulder,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] == GLFW_PRESS);
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::right_shoulder,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] == GLFW_PRESS);

        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::left_thumb,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] == GLFW_PRESS);
        gamepad.postGamepadButtonPressed(m_delta_time, context, EGamepadButton::right_thumb,
            glfw_state.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB] == GLFW_PRESS);

        PPR_RETURN_ERROR_ON_FAIL(GlfwInput, gamepad.pollInputMessages(m_delta_time));

        // TODO: GLFW does not support rumble as of now (30/08/2026),
        // and will probably never since there an issue opened since 2013:
        // https://github.com/glfw/glfw/issues/57

        return default_value_v;
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
}
