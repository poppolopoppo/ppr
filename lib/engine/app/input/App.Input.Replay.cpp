module;
#include "pP/Macros.h"

module engine.app;

import engine.core;
import engine.math;
import std;

import :input.replay;

namespace pP {
    InputReplay::InputReplay() noexcept = default;

    InputReplay::~InputReplay() noexcept {
        if (m_parent) {
            uninstallCaptureListener_();   // release safe_ptr to m_capture_listener from parent
        }
        detachParent();
    }

    void InputReplay::setParent(safe_ptr<IInputService> parent) noexcept {
        if (m_parent) {
            std::ignore = m_parent->popInputListener(m_capture_listener);
        }
        m_parent = parent;
        if (m_parent) {
            installCaptureListener_();
        }
    }

    void InputReplay::detachParent() noexcept {
        if (m_parent) {
            std::ignore = m_parent->popInputListener(m_capture_listener);
        }
        m_parent.reset();
    }

    void InputReplay::installCaptureListener_() noexcept {
        m_capture_listener.setInputListenerMode(EInputListenerResponse::unhandled); // never consume
        m_capture_listener.setRawKeyCallback([this](const InputMessage &msg) {
            if (m_mode == EInputReplayMode::record || m_mode == EInputReplayMode::both) {
                m_recorded.push_back(msg);   // (1) faithful record
            }
            routeToOwnListeners_(msg);       // (2) re-dispatch to own stack
        });
        m_parent->pushInputListener(safe_ptr<InputListener>{&m_capture_listener});
    }

    void InputReplay::uninstallCaptureListener_() noexcept {
        std::ignore = m_parent->popInputListener(m_capture_listener);
        m_capture_listener.setRawKeyCallback(nullptr);
    }

    void InputReplay::inject(const InputMessage &msg) {
        if (m_mode == EInputReplayMode::record || m_mode == EInputReplayMode::both) {
            m_recorded.push_back(msg);
        } else {
            m_injected.push_back(msg);
        }
    }

    void InputReplay::injectKey(const InputKey &key, const bool down) {
        const InputDeviceID device_id = key.isGamepad() ? m_gamepad.m_device_id
            : key.isMouse() ? m_mouse.m_device_id
            : m_keyboard.m_device_id;
        inject(InputMessage(
            key, InputValue{InputDigital{down}}, TimeSpan{zero_v}, device_id,
            down ? EInputMessageEvent::pressed : EInputMessageEvent::released));
    }

    void InputReplay::injectCursorDelta(const float2 delta) {
        inject(InputMessage(
            InputKey::mouse_2d, InputValue{InputAxis2D{float2{zero_v}, delta}},
            TimeSpan{zero_v}, m_mouse.m_device_id, EInputMessageEvent::axis));
    }

    void InputReplay::injectWheel(const float delta_y) {
        inject(InputMessage(
            InputKey::mouse_wheel_axis_y, InputValue{InputAxis1D{0.0f, delta_y}},
            TimeSpan{zero_v}, m_mouse.m_device_id, EInputMessageEvent::axis));
    }

    void InputReplay::injectGamepadStick(const int idx, const float2 value) {
        const InputKey key = idx == 0 ? InputKey::gamepad_left_2d : InputKey::gamepad_right_2d;
        inject(InputMessage(
            key, InputValue{InputAxis2D{float2{zero_v}, value}},
            TimeSpan{zero_v}, m_gamepad.m_device_id, EInputMessageEvent::axis));
    }

    void InputReplay::injectGamepadButton(const EGamepadButton button, const bool down) {
        if (const std::optional<InputKey> key = InputKey::from(button);
            key.has_value()) {
            inject(InputMessage(
                key.value(), InputValue{InputDigital{down}}, TimeSpan{zero_v},
                m_gamepad.m_device_id,
                down ? EInputMessageEvent::pressed : EInputMessageEvent::released));
        }
    }

    void InputReplay::injectGamepadTrigger(const int idx, const float value) {
        const InputKey key = idx == 0 ? InputKey::gamepad_left_trigger_axis : InputKey::gamepad_right_trigger_axis;
        inject(InputMessage(
            key, InputValue{InputAxis1D{0.0f, value}},
            TimeSpan{zero_v}, m_gamepad.m_device_id, EInputMessageEvent::axis));
    }

    void InputReplay::routeToOwnListeners_(const InputMessage &msg) {
        // Snapshot: dispatch may push/pop listeners, which would invalidate the live range.
        const Array<SharedInputListener> snapshot = m_listeners;
        for (const SharedInputListener &listener: snapshot) {
            if (listener->postKeyEvent(msg) == EInputListenerResponse::consumed) {
                return;
            }
        }
        std::ignore = m_global_listener.postKeyEvent(msg);
    }

    void InputReplay::setMode(const EInputReplayMode mode) noexcept {
        m_mode = mode;
    }

    void InputReplay::startRecording() noexcept {
        if (m_mode == EInputReplayMode::replay) {
            m_mode = EInputReplayMode::both;   // keep replaying while capturing
        }
    }

    void InputReplay::stopRecording() noexcept {
        m_mode = EInputReplayMode::replay;
    }

    const Array<InputMessage> &InputReplay::recording() const noexcept {
        return m_recorded;
    }

    void InputReplay::clearRecording() noexcept {
        m_recorded.clear();
    }

    void InputReplay::loadRecording(Array<InputMessage> frames) noexcept {
        // Recorded input is stored as a flat list with no frame association; it is
        // replayed in full on the next postInputMessages() call (burst-only, see there).
        m_injected = std::move(frames);
    }

    const KeyboardState &InputReplay::getKeyboard() const noexcept {
        return m_parent ? m_parent->getKeyboard() : m_keyboard.m_state;
    }

    const MouseState &InputReplay::getMouse() const noexcept {
        return m_parent ? m_parent->getMouse() : m_mouse.m_state;
    }

    const GamepadState &InputReplay::getGamepad(const int controller_index) const noexcept {
        return m_parent ? m_parent->getGamepad(controller_index) : m_gamepad.m_state;
    }

    SharedInputDevice InputReplay::getInputDevice(const InputDeviceID &device_id) const noexcept {
        if (m_parent) {
            return m_parent->getInputDevice(device_id);
        }
        if (m_keyboard.m_device_id == device_id) {
            return SharedInputDevice{&m_keyboard};   // borrows member state; must not outlive this InputReplay
        }
        if (m_mouse.m_device_id == device_id) {
            return SharedInputDevice{&m_mouse};      // borrows member state; must not outlive this InputReplay
        }
        if (m_gamepad.m_device_id == device_id) {
            return SharedInputDevice{&m_gamepad};    // borrows member state; must not outlive this InputReplay
        }
        return SharedInputDevice{};
    }

    std::error_code InputReplay::enumerateInputDevices(const Collector<SharedInputDevice> each_device) const noexcept {
        if (m_parent) {
            return m_parent->enumerateInputDevices(each_device);
        }
        // Handles borrow member state (m_keyboard/m_mouse/m_gamepad); they must not outlive this InputReplay.
        return each_device.append({
            SharedInputDevice{&m_keyboard},
            SharedInputDevice{&m_mouse},
            SharedInputDevice{&m_gamepad},
        });
    }

    std::error_code InputReplay::supportedInputKeys(const Collector<InputKey> supports_key) const {
        if (m_parent) {
            return m_parent->supportedInputKeys(supports_key);
        }
        if (const std::error_code err = m_keyboard.supportedInputKeys(supports_key)) [[unlikely]] {
            return err;
        }
        if (const std::error_code err = m_mouse.supportedInputKeys(supports_key)) [[unlikely]] {
            return err;
        }
        return m_gamepad.supportedInputKeys(supports_key);
    }

    std::error_code InputReplay::postInputMessages(const TimeSpan dt) {
        // Known limitation: replay is burst-only. The entire m_injected queue is drained
        // and cleared in a single call, so loadRecording() delivers all recorded messages
        // in one frame — there is no per-frame distribution of the captured timeline.
        if (m_mode == EInputReplayMode::replay || m_mode == EInputReplayMode::both) {
            for (const InputMessage &msg: m_injected) {
                routeToOwnListeners_(msg);   // replay drained
            }
            m_injected.clear();
        }

        if (m_parent) {
            // Intended: replayed input (above) and live input (below) are merged every frame —
            // during replay the capture listener forwards live user input to the same
            // own-listeners, so both sources drive the application.
            return m_parent->postInputMessages(dt);
        }
        return default_value_v;
    }

    void InputReplay::resetInputState() noexcept {
        if (m_parent) {
            m_parent->resetInputState();
            return;
        }
        m_keyboard.resetInputState();
        m_mouse.resetInputState();
        m_gamepad.resetInputState();
    }

    bool InputReplay::hasInputListener(const InputListener &listener) const noexcept {
        return std::ranges::contains(
            m_listeners, &listener,
            [](const SharedInputListener &shared_listener) noexcept -> const InputListener * {
                return shared_listener.get();
            });
    }

    void InputReplay::pushInputListener(SharedInputListener listener) {
        const int priority = listener->getPriority();
        const auto it = std::ranges::find_if(m_listeners,
            [priority](const SharedInputListener &existing) noexcept {
                return existing->getPriority() < priority;
            });
        m_listeners.insert(it, std::move(listener));
    }

    bool InputReplay::popInputListener(const InputListener &listener) {
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

    bool InputReplay::hasGlobalInputMapping(const InputMapping &mapping) const noexcept {
        if (m_parent) {
            return m_parent->hasGlobalInputMapping(mapping);
        }
        return m_global_listener.hasInputMapping(mapping);
    }

    void InputReplay::addGlobalInputMapping(SharedInputMapping mapping, const int priority) {
        if (m_parent) {
            m_parent->addGlobalInputMapping(std::move(mapping), priority);
            return;
        }
        m_global_listener.addMapping(std::move(mapping), priority);
    }

    bool InputReplay::removeGlobalInputMapping(const InputMapping &mapping) {
        if (m_parent) {
            return m_parent->removeGlobalInputMapping(mapping);
        }
        return m_global_listener.removeMapping(mapping);
    }

    auto InputReplay::whenDeviceConnected(DeviceCallback::Event on_connected) -> DeviceCallback::Handle {
        if (m_parent) {
            return m_parent->whenDeviceConnected(std::move(on_connected));
        }
        return DeviceCallback::Handle{};
    }

    auto InputReplay::whenDeviceDisconnected(DeviceCallback::Event on_disconnected) -> DeviceCallback::Handle {
        if (m_parent) {
            return m_parent->whenDeviceDisconnected(std::move(on_disconnected));
        }
        return DeviceCallback::Handle{};
    }

    auto InputReplay::whenActionStarted(TriggerCallback::Event on_started) -> TriggerCallback::Handle {
        if (m_parent) {
            return m_parent->whenActionStarted(std::move(on_started));
        }
        return TriggerCallback::Handle{};
    }

    auto InputReplay::whenActionTriggered(TriggerCallback::Event on_triggered) -> TriggerCallback::Handle {
        if (m_parent) {
            return m_parent->whenActionTriggered(std::move(on_triggered));
        }
        return TriggerCallback::Handle{};
    }

    auto InputReplay::whenActionCompleted(TriggerCallback::Event on_completed) -> TriggerCallback::Handle {
        if (m_parent) {
            return m_parent->whenActionCompleted(std::move(on_completed));
        }
        return TriggerCallback::Handle{};
    }

    auto InputReplay::whenUnhandledKey(UnhandledKeyCallback::Event on_unhandled_key) -> UnhandledKeyCallback::Handle {
        if (m_parent) {
            return m_parent->whenUnhandledKey(std::move(on_unhandled_key));
        }
        return UnhandledKeyCallback::Handle{};
    }

    auto InputReplay::whenBeforeUpdated(UpdateCallback::Event on_update) -> UpdateCallback::Handle {
        if (m_parent) {
            return m_parent->whenBeforeUpdated(std::move(on_update));
        }
        return UpdateCallback::Handle{};
    }

    auto InputReplay::whenAfterUpdated(UpdateCallback::Event on_update) -> UpdateCallback::Handle {
        if (m_parent) {
            return m_parent->whenAfterUpdated(std::move(on_update));
        }
        return UpdateCallback::Handle{};
    }
}
