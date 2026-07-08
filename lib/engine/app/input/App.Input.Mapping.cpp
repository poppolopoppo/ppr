module;
#include "pP/Macros.h"
module engine.app;

import engine.core;
import :input.mapping;

namespace pP {
    InputMapping::InputMapping(const string_literal description) noexcept
        : m_description{description} {
        PPR_ASSERT(not m_description.empty());
    }

    InputActionKeyMapping &InputMapping::mapKey(SharedInputAction action, InputKey key) {
        PPR_ASSERT(action.isValid());

        const auto it = std::ranges::find_if(
            m_keymap,
            [&](const InputActionKeyMapping &key_mapping) noexcept -> bool {
                return key_mapping.m_action == action and key_mapping.m_key ==  key;
            });

        if (m_keymap.end() == it) {
            return m_keymap.emplaceBack(key, std::move(action));
        }
        return *it;
    }

    bool InputMapping::unmapKey(const InputAction &action, const InputKey &key) {
        const auto it = std::ranges::find_if(
            m_keymap,
            [&](const InputActionKeyMapping &key_mapping) noexcept -> bool {
               return key_mapping.m_action == &action and key_mapping.m_key == key;
            });

        if (m_keymap.end() != it) [[likely]] {
            m_keymap.erase(it);
            return true;
        }
        return false;
    }

    void InputMapping::unmapAction(const InputAction &action) {
        for (auto it = m_keymap.begin(); m_keymap.end() != it; ) {
            if (it->m_action.get() == &action) {
                it = m_keymap.erase(it);
            } else {
                ++it;
            }
        }
    }

    void InputMapping::clearAllMappings() {
        m_keymap.clear();
    }
}
