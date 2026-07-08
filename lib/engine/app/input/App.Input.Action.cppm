module;
#include "pP/Macros.h"
export module engine.app:input.action;

import engine.core;
import :input.device;
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

    struct InputAction : safe_object {
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
            EInputActionFlags flags) noexcept;

        [[nodiscard]] constexpr bool hasConsumeInput() const noexcept {
            return any(m_flags & EInputActionFlags::consume_input);
        }

        [[nodiscard]] constexpr bool hasTriggerWhenPaused() const noexcept {
            return any(m_flags & EInputActionFlags::trigger_when_paused);
        }

        [[nodiscard]] static InputModifierEvent modulate(float value) noexcept;

        [[nodiscard]] static InputModifierEvent modulate(const float2 &value) noexcept;

        [[nodiscard]] static InputModifierEvent modulate(const float3 &value) noexcept;

        [[nodiscard]] static InputModifierEvent modulate(std23::function_ref<float(TimeSpan dt) noexcept> get_value) noexcept;

        [[nodiscard]] static InputModifierEvent modulate(std23::function_ref<float2(TimeSpan dt) noexcept> get_value) noexcept;

        [[nodiscard]] static InputModifierEvent modulate(std23::function_ref<float3(TimeSpan dt) noexcept> get_value) noexcept;
    };

    using SharedInputAction = safe_ptr<const InputAction>;

    // ------------------------------------------------------------------
    // input action event
    // ------------------------------------------------------------------

    struct InputActionEvent {
        SharedInputAction m_source;
        std::optional<InputValue> m_value{};

        TimeSpan m_elapsed_triggered_time{zero_v};
        u32 m_repeat_count{0u};
        EInputTriggerEvent m_trigger_state{default_value_v};

        explicit constexpr InputActionEvent(SharedInputAction source) noexcept
            : m_source{std::move(source)} {
        }

        constexpr InputActionEvent(SharedInputAction source, InputValue value) noexcept
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

        [[nodiscard]] std::optional<InputDigital> getDigitalValue() const noexcept;

        [[nodiscard]] std::optional<InputAxis1D> getAxis1DValue() const noexcept;

        [[nodiscard]] std::optional<InputAxis2D> getAxis2DValue() const noexcept;

        [[nodiscard]] std::optional<InputAxis3D> getAxis3DValue() const noexcept;

        [[nodiscard]] friend bool operator==(const InputActionEvent &lhs, const InputAction &rhs) noexcept {
            return lhs.m_source.get() == &rhs;
        }

        [[nodiscard]] friend std::strong_ordering operator<=>(const InputActionEvent &lhs, const InputAction &rhs) noexcept {
            return lhs.m_source.get() <=> &rhs;
        }
    };
}
