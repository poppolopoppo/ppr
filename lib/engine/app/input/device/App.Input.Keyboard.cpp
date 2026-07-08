module;
#include "pP/Macros.h"
module engine.app;

import :input.keyboard;

namespace pP {
    // ------------------------------------------------------------------
    // mouse state
    // ------------------------------------------------------------------

    void KeyboardState::setKeyPressed(const EKeyboardKey key) {
        m_keys.setPressed(key);
    }

    void KeyboardState::addCharacterInput(hal::native::char_t character) {
        PPR_VERIFY(m_characters_queued.push(character));
    }

    void KeyboardState::update(TimeSpan) {
        m_keys.update();

        m_characters = m_characters_queued;
        m_characters_queued.clear();
    }

    void KeyboardState::reset() {
        m_keys.reset();

        m_characters.clear();
        m_characters_queued.clear();
    }

    // ------------------------------------------------------------------
    // gamepad device
    // ------------------------------------------------------------------

    KeyboardDevice::KeyboardDevice() noexcept = default;

    KeyboardDevice::~KeyboardDevice() noexcept = default;

    void KeyboardDevice::supportedInputKeys(const Collector<InputKey> supports_key) const {
        InputKey::enumerateKeyboardKeys(supports_key);
    }

    void KeyboardDevice::postInputMessages(const TimeSpan dt, const Collector<InputMessage> post_event) {
        m_state.update(dt);

        m_state.m_keys.postInputMessages(m_device_id, dt, post_event);
    }

    void KeyboardDevice::resetInputState() noexcept {
        m_state.reset();
    }
}
