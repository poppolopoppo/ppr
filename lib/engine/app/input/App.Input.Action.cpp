module;
#include "pP/Macros.h"
module engine.app;

import engine.core;
import :input.action;

namespace pP {
    InputAction::InputAction(
        const string_literal description,
        const EInputValueType value_type,
        const EInputActionFlags flags) noexcept
        : m_description{description}, m_value_type{value_type}, m_flags{flags} {
        PPR_ASSERT(not m_description.empty());
    }

    InputModifierEvent InputAction::modulate(const float value) noexcept {
        return [value](const TimeSpan dt, InputValue &output) noexcept {
            const auto modulator = static_cast<float>(static_cast<double>(value) *
                time::seconds(dt));
            output = output.modulate(modulator);
        };
    }

    InputModifierEvent InputAction::modulate(const float2 &value) noexcept {
        return [value](const TimeSpan dt, InputValue &output) noexcept {
            const float2 modulator = vector_cast<float>(vector_cast<double>(value) *
                time::seconds(dt));
            output = output.modulate(modulator);
        };
    }

    InputModifierEvent InputAction::modulate(const float3 &value) noexcept {
        return [value](const TimeSpan dt, InputValue &output) noexcept {
            const float3 modulator = vector_cast<float>(vector_cast<double>(value) *
                time::seconds(dt));
            output = output.modulate(modulator);
        };
    }

    InputModifierEvent InputAction::modulate(const std23::function_ref<float(TimeSpan dt) noexcept>  get_value) noexcept {
        return [get_value](const TimeSpan dt, InputValue &output) noexcept {
            const float modulator = get_value(dt);
            output = output.modulate(modulator);
        };
    }

    InputModifierEvent InputAction::modulate(const std23::function_ref<float2(TimeSpan dt) noexcept> get_value) noexcept {
        return [get_value](const TimeSpan dt, InputValue &output) noexcept {
            const float2 modulator = get_value(dt);
            output = output.modulate(modulator);
        };
    }

    InputModifierEvent InputAction::modulate(const std23::function_ref<float3(TimeSpan dt) noexcept> get_value) noexcept {
        return [get_value](const TimeSpan dt, InputValue &output) noexcept {
            const float3 modulator = get_value(dt);
            output = output.modulate(modulator);
        };
    }
}
