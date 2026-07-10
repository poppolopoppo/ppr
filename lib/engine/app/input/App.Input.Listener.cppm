module;
#include "pP/Macros.h"
export module engine.app:input.listener;

import engine.core;
import :input.mapping;

export namespace pP {
    // ------------------------------------------------------------------
    // input listener
    // ------------------------------------------------------------------

    enum class EInputListenerResponse : u8 {
        // listener do not trigger any mapping
        unhandled = 0,
        // listener trigger at least one mapping, but did not consume the input
        handled,
        // listener trigger at least one mapping, and input was consumed (message won't be handled by any other listener)
        consumed,
    };

    class InputListener : public safe_object {
        struct MappingAndPriority {
            SharedInputMapping m_mapping{};
            int m_priority{0};

            [[nodiscard]] bool operator==(const MappingAndPriority &other) const noexcept;

            [[nodiscard]] bool operator==(const InputMapping &mapping) const noexcept;

            [[nodiscard]] std::strong_ordering operator<=>(const MappingAndPriority &other) const noexcept;

            [[nodiscard]] std::strong_ordering operator<=>(const InputMapping &mapping) const noexcept;
        };

        using InputMappingIndex = Numeric<u32, MappingAndPriority>;
        using KeyMappingIndex = Numeric<u32, InputKey>;

        struct InputBinding {
            InputMappingIndex m_input_mapping{umax_v};
            KeyMappingIndex m_key_mapping{umax_v};
        };

        void rebuildKeybindings_();

        [[nodiscard]] const InputActionKeyMapping &getKeyMapping_(const InputBinding &binding) const noexcept;

        FlatSet<MappingAndPriority> m_mappings;
        FlatMap<SharedInputAction, InputActionEvent> m_action_events;
        FlatMultiMap<InputKey, InputBinding> m_keybindings;

        EInputListenerResponse m_listener_mode{EInputListenerResponse::consumed};

    public:
        constexpr InputListener() = default;

        [[nodiscard]] constexpr EInputListenerResponse getInputListenerMode() const noexcept {
            return m_listener_mode;
        }

        constexpr void setInputListenerMode(const EInputListenerResponse value) noexcept {
            m_listener_mode = value;
        }

        [[nodiscard]] bool hasInputMapping(const InputMapping &mapping) const noexcept;

        [[nodiscard]] bool isKeyHandledByAction(const InputKey &key) const noexcept;

        [[nodiscard]] std::optional<InputValue> getActionValue(const InputAction &action) const noexcept;

        void addMapping(SharedInputMapping mapping, int priority);

        bool removeMapping(const InputMapping &mapping);

        void clearAllMappings();

        [[nodiscard]] EInputListenerResponse postKeyEvent(const InputMessage &message) noexcept;
    };

    using SharedInputListener = safe_ptr<const InputListener>;
}
