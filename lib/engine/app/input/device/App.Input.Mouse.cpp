module;
#include "pP/Macros.h"
module engine.app;

import :input.mouse;

namespace pP {
    // ------------------------------------------------------------------
    // mouse state
    // ------------------------------------------------------------------

    void MouseState::setButtonPressed(const EMouseButton button) {
        m_buttons.setPressed(button);
    }

    void MouseState::setCursorPos(const int2 &position) noexcept {
        // snap on pixel center
        m_cursor_pos.set(vector_cast<float>(position) + 0.5f);
    }

    void MouseState::addWheelDeltaX(const int delta) noexcept {
        m_wheel_x.add(static_cast<float>(delta));
    }

    void MouseState::addWheelDeltaY(const int delta) noexcept {
        m_wheel_y.add(static_cast<float>(delta));
    }

    void MouseState::setWheelX(const int delta) noexcept {
        m_wheel_x.set(static_cast<float>(delta));
    }

    void MouseState::setWheelY(const int delta) noexcept {
        m_wheel_y.set(static_cast<float>(delta));
    }

    void MouseState::update(const TimeSpan dt) {
        const double elapsed_seconds = time::seconds(dt);

        m_cursor_pos.update(elapsed_seconds);
        m_wheel_x.update(elapsed_seconds);
        m_wheel_y.update(elapsed_seconds);

        m_buttons.update();
    }

    void MouseState::reset() {
        m_cursor_pos.reset();
        m_wheel_x.reset();
        m_wheel_y.reset();

        m_buttons.reset();
    }

    // ------------------------------------------------------------------
    // gamepad device
    // ------------------------------------------------------------------

    MouseDevice::MouseDevice() noexcept = default;

    MouseDevice::~MouseDevice() noexcept = default;

    void MouseDevice::supportedInputKeys(const Collector<InputKey> supports_key) const {
        InputKey::enumerateMouseAxes(supports_key);
        InputKey::enumerateMouseButtons(supports_key);
    }

    void MouseDevice::postInputMessages(const TimeSpan dt, const Collector<InputMessage> post_event) {
        m_state.update(dt);

        const bool enable_filtered_inputs = m_state.m_enable_filtered_inputs;

        m_state.m_cursor_pos.postInputMessages(
            m_device_id,
            InputKey::mouse_2d,
            enable_filtered_inputs,
            dt, post_event);

        m_state.m_wheel_x.postInputMessages(
            m_device_id,
            InputKey::mouse_wheel_axis_x,
            enable_filtered_inputs,
            dt, post_event);

        m_state.m_wheel_y.postInputMessages(
            m_device_id,
            InputKey::mouse_wheel_axis_y,
            enable_filtered_inputs,
            dt, post_event);

        m_state.m_buttons.postInputMessages(m_device_id, dt, post_event);
    }

    void MouseDevice::resetInputState() noexcept {
        m_state.reset();
    }
}
