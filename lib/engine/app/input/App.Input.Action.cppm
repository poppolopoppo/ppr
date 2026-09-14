module;
#include "pP/Macros.h"
export module engine.app:input.action;

import engine.core;
import :input.key;

export namespace pP {
    struct InputActionEvent;

    using InputModifierEvent = std::move_only_function<void(TimeSpan dt, InputValue &value) const noexcept>;

    using InputTriggerEvent = std::move_only_function<void(const InputActionEvent &event, const InputKey &trigger) const noexcept>;

    // ------------------------------------------------------------------
    // input action
    // ------------------------------------------------------------------

    enum class EInputActionFlags : u8 {
        none = 0b00u,

        consume_input = 0b01u,
        trigger_when_paused = 0b10u,

        all = consume_input | trigger_when_paused,
    };

    static_assert(details::TEnumFlags<EInputActionFlags>);

    enum class EInputTriggerEvent : u8 {
        inactive = 0,
        started,
        triggered,
        completed,
    };

    struct InputAction final : safe_object {
        string_literal m_description;
        EInputValueType m_value_type;
        EInputActionFlags m_flags{default_value_v};

        std::optional<InputModifierEvent> m_modifier;

        std::optional<InputTriggerEvent> m_when_started;
        std::optional<InputTriggerEvent> m_when_triggered;
        std::optional<InputTriggerEvent> m_when_completed;

        InputAction(
            string_literal description,
            EInputValueType value_type,
            EInputActionFlags flags = EInputActionFlags::consume_input) noexcept;

        void setStarted(InputTriggerEvent callback) noexcept { m_when_started = std::move(callback); }
        void setCompleted(InputTriggerEvent callback) noexcept { m_when_completed = std::move(callback); }
        void setTriggered(InputTriggerEvent callback) noexcept { m_when_triggered = std::move(callback); }

        [[nodiscard]] constexpr bool hasConsumeInput() const noexcept {
            return any(m_flags & EInputActionFlags::consume_input);
        }

        [[nodiscard]] constexpr bool hasTriggerWhenPaused() const noexcept {
            return any(m_flags & EInputActionFlags::trigger_when_paused);
        }

        [[nodiscard]] static InputModifierEvent modulate(float value) noexcept;

        [[nodiscard]] static InputModifierEvent modulate(const float2 &value) noexcept;

        [[nodiscard]] static InputModifierEvent modulate(const float3 &value) noexcept;

        template<class GetValueT>
            requires std::is_nothrow_invocable_r_v<float, const GetValueT &> or
                     std::is_nothrow_invocable_r_v<float2, const GetValueT &> or
                     std::is_nothrow_invocable_r_v<float3, const GetValueT &>
        [[nodiscard]] static InputModifierEvent modulate(GetValueT &&get_value) noexcept {
            return [get_value{std::forward<GetValueT>(get_value)}](const TimeSpan dt, InputValue &output) noexcept {
                output = output.modulate(dt, get_value());
            };
        }
    };

    using SharedInputAction = safe_ptr<const InputAction>;

    // ------------------------------------------------------------------
    // input action event
    // ------------------------------------------------------------------

    struct InputActionEvent final {
        SharedInputAction m_source{};
        std::optional<InputValue> m_value{};

        TimeSpan m_elapsed_triggered_time{zero_v};
        u32 m_repeat_count{0u};
        EInputTriggerEvent m_trigger_state{default_value_v};

        explicit InputActionEvent(SharedInputAction source) noexcept
            : m_source(std::move(source)) {
        }

        InputActionEvent(SharedInputAction source, InputValue value) noexcept
            : m_source{std::move(source)}, m_value{std::move(value)} {
        }

        [[nodiscard]] constexpr bool isTriggerInactive() const noexcept {
            return m_trigger_state == EInputTriggerEvent::inactive;
        }

        [[nodiscard]] constexpr bool isTriggerStarted() const noexcept {
            return m_trigger_state == EInputTriggerEvent::started;
        }

        [[nodiscard]] constexpr bool isTriggerActive() const noexcept {
            return m_trigger_state == EInputTriggerEvent::triggered;
        }

        [[nodiscard]] constexpr bool isTriggerCompleted() const noexcept {
            return m_trigger_state == EInputTriggerEvent::completed;
        }

        [[nodiscard]] std::optional<InputDigital> getIfDigitalValue() const noexcept;

        [[nodiscard]] std::optional<InputAxis1D> getIfAxis1DValue() const noexcept;

        [[nodiscard]] std::optional<InputAxis2D> getIfAxis2DValue() const noexcept;

        [[nodiscard]] std::optional<InputAxis3D> getIfAxis3DValue() const noexcept;

        [[nodiscard]] InputDigital getDigitalValue() const noexcept;

        [[nodiscard]] InputAxis1D getAxis1DValue() const noexcept;

        [[nodiscard]] const InputAxis2D &getAxis2DValue() const noexcept;

        [[nodiscard]] const InputAxis3D &getAxis3DValue() const noexcept;

        [[nodiscard]] friend bool operator==(const InputActionEvent &lhs, const InputAction &rhs) noexcept {
            return lhs.m_source.get() == &rhs;
        }

        [[nodiscard]] friend std::strong_ordering operator<=>(const InputActionEvent &lhs, const InputAction &rhs) noexcept {
            return lhs.m_source.get() <=> &rhs;
        }
    };

    // ------------------------------------------------------------------
    // input key to action mapping
    // ------------------------------------------------------------------

    struct InputActionKeyMapping final {
        SharedInputAction m_action{};
        InputKey m_key;

        std::optional<InputModifierEvent> m_modifier{};

        std::optional<InputTriggerEvent> m_when_started{};
        std::optional<InputTriggerEvent> m_when_triggered{};
        std::optional<InputTriggerEvent> m_when_completed{};

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

    class InputMapping final : public safe_object {
    public:
        string_literal m_description;
        StableVectorInplace<InputActionKeyMapping> m_keymap{};

        explicit InputMapping(string_literal description) noexcept;

        InputActionKeyMapping &mapInputKey(SharedInputAction action, InputKey key);

        InputActionKeyMapping &mapInputKey(SharedInputAction action, InputKey key, InputModifierEvent modifier);

        bool unmapInputKey(const InputAction &action, const InputKey &key);

        void unmapInputAction(const InputAction &action);

        void clearInputMappings();
    };
}
