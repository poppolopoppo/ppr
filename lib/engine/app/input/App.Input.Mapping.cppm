module;
#include "pP/Macros.h"
export module engine.app:input.mapping;

import engine.core;
import :input.action;
import :input.key;

export namespace pP {
    // ------------------------------------------------------------------
    // input key to action mapping
    // ------------------------------------------------------------------

    struct InputActionKeyMapping {
        SharedInputAction m_action;
        InputKey m_key;

        std::optional<InputModifierEvent> m_modifier;

        std::optional<InputTriggerEvent> m_when_started;
        std::optional<InputTriggerEvent> m_when_triggered;
        std::optional<InputTriggerEvent> m_when_completed;

        constexpr InputActionKeyMapping(InputKey key, SharedInputAction action) noexcept
            : m_action{std::move(action)}, m_key{std::move(key)} {
        }

        constexpr InputActionKeyMapping(
            InputKey key, SharedInputAction action,
            InputModifierEvent modifier) noexcept
            : m_action{std::move(action)}, m_key{std::move(key)},
              m_modifier{std::move(modifier)} {
        }

        constexpr InputActionKeyMapping(
            InputKey key, SharedInputAction action,
            InputTriggerEvent when_triggered) noexcept
            : m_action{std::move(action)}, m_key{std::move(key)},
              m_when_triggered{std::move(when_triggered)} {
        }

        constexpr InputActionKeyMapping(
            InputKey key, SharedInputAction action,
            InputModifierEvent modifier,
            InputTriggerEvent when_triggered) noexcept
            : m_action{std::move(action)}, m_key{std::move(key)},
              m_modifier{std::move(modifier)},
              m_when_triggered{std::move(when_triggered)} {
        }

        [[nodiscard]] friend constexpr bool operator==(const InputActionKeyMapping &lhs, const InputActionKeyMapping &rhs) noexcept {
            return lhs.m_key == rhs.m_key and lhs.m_action == rhs.m_action;
        }
    };

    // ------------------------------------------------------------------
    // input mapping -> collection of input key mappings
    // ------------------------------------------------------------------

    using InputMappingProvider = std23::function_ref<void(Collector<InputActionKeyMapping>)>;

    class InputMapping : public safe_object {
    public:
        string_literal m_description;
        StableVectorInplace<InputActionKeyMapping> m_keymap{};

        explicit InputMapping(string_literal description) noexcept;

        InputActionKeyMapping &mapKey(SharedInputAction action, InputKey key);

        bool unmapKey(const InputAction &action, const InputKey &key);

        void unmapAction(const InputAction &action);

        void clearAllMappings();
    };

    using SharedInputMapping = safe_ptr<const InputMapping>;
}
