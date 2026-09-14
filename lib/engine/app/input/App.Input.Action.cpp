module;
#include "pP/Macros.h"
module engine.app;

import engine.core;
import :input.action;

namespace pP {
    // ------------------------------------------------------------------
    // input action
    // ------------------------------------------------------------------

    InputAction::InputAction(
        const string_literal description,
        const EInputValueType value_type,
        const EInputActionFlags flags) noexcept
        : m_description{description}, m_value_type{value_type}, m_flags{flags} {
        PPR_ASSERT(not m_description.empty());
    }

    InputModifierEvent InputAction::modulate(const float value) noexcept {
        return [value](const TimeSpan dt, InputValue &output) noexcept {
            output = output.modulate(dt, value);
        };
    }

    InputModifierEvent InputAction::modulate(const float2 &value) noexcept {
        return [value](const TimeSpan dt, InputValue &output) noexcept {
            output = output.modulate(dt, value);
        };
    }

    InputModifierEvent InputAction::modulate(const float3 &value) noexcept {
        return [value](const TimeSpan dt, InputValue &output) noexcept {
            output = output.modulate(dt, value);
        };
    }

    // ------------------------------------------------------------------
    // input action event
    // ------------------------------------------------------------------

    namespace {
        template<typename InputValueT>
            requires std::is_constructible_v<InputValue, InputValueT>
        [[nodiscard]] const InputValueT &getActionValue(const InputActionEvent &event) noexcept {
            return std::get<InputValueT>(*event.m_value);
        }

        template<typename InputValueT>
            requires std::is_constructible_v<InputValue, InputValueT>
        [[nodiscard]] std::optional<InputValueT> getIfActionValue(const InputActionEvent &event) noexcept {
            if (event.m_value.has_value()) {
                if (const InputValueT *input_value = std::get_if<InputValueT>(&event.m_value.value())) {
                    return *input_value;
                }
            }
            return std::nullopt;
        }
    }

    std::optional<InputDigital> InputActionEvent::getIfDigitalValue() const noexcept {
        return getIfActionValue<InputDigital>(*this);
    }

    std::optional<InputAxis1D> InputActionEvent::getIfAxis1DValue() const noexcept {
        return getIfActionValue<InputAxis1D>(*this);
    }

    std::optional<InputAxis2D> InputActionEvent::getIfAxis2DValue() const noexcept {
        return getIfActionValue<InputAxis2D>(*this);
    }

    std::optional<InputAxis3D> InputActionEvent::getIfAxis3DValue() const noexcept {
        return getIfActionValue<InputAxis3D>(*this);
    }

    InputDigital InputActionEvent::getDigitalValue() const noexcept {
        return getActionValue<InputDigital>(*this);
    }

    InputAxis1D InputActionEvent::getAxis1DValue() const noexcept {
        return getActionValue<InputAxis1D>(*this);
    }

    const InputAxis2D &InputActionEvent::getAxis2DValue() const noexcept {
        return getActionValue<InputAxis2D>(*this);
    }

    const InputAxis3D &InputActionEvent::getAxis3DValue() const noexcept {
        return getActionValue<InputAxis3D>(*this);
    }

    // ------------------------------------------------------------------
    // input mapping -> collection of input key mappings
    // ------------------------------------------------------------------

    InputMapping::InputMapping(const string_literal description) noexcept
        : m_description{description} {
        PPR_ASSERT(not m_description.empty());
    }

    InputActionKeyMapping &InputMapping::mapInputKey(SharedInputAction action, InputKey key) {
        PPR_ASSERT(action.isValid());

        const auto it = std::ranges::find_if(
            m_keymap,
            [&](const InputActionKeyMapping &key_mapping) noexcept -> bool {
                return key_mapping.m_action == action and key_mapping.m_key == key;
            });

        if (m_keymap.end() == it) {
            return m_keymap.emplaceBack(key, std::move(action));
        }
        return *it;
    }

    InputActionKeyMapping &InputMapping::mapInputKey(SharedInputAction action, const InputKey key, InputModifierEvent modifier) {
        InputActionKeyMapping &key_mapping = mapInputKey(std::move(action), key);
        key_mapping.m_modifier = std::move(modifier);
        return key_mapping;
    }

    bool InputMapping::unmapInputKey(const InputAction &action, const InputKey &key) {
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

    void InputMapping::unmapInputAction(const InputAction &action) {
        for (auto it = m_keymap.begin(); m_keymap.end() != it;) {
            if (it->m_action.get() == &action) {
                it = m_keymap.erase(it);
            } else {
                ++it;
            }
        }
    }

    void InputMapping::clearInputMappings() {
        m_keymap.clear();
    }
}
