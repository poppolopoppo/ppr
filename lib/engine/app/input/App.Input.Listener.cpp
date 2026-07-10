module;
#include "pP/Macros.h"
module engine.app;

import engine.core;
import :input.listener;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(Input, info, none)

    // ------------------------------------------------------------------
    // input action event
    // ------------------------------------------------------------------

    template<typename InputValueT>
        requires std::is_constructible_v<InputValue, InputValueT>
    [[nodiscard]] std::optional<InputValueT> getActionValue(const InputActionEvent &event) noexcept {
        if (not event.m_value.has_value()) [[unlikely]] return std::nullopt;
        return std::visit(
            overloaded(
                [](const InputValueT &value) noexcept -> std::optional<InputValueT> {
                    return value;
                },
                [](const auto &) noexcept -> std::optional<InputValueT> {
                    return std::nullopt;
                }),
            *event.m_value);
    }

    std::optional<InputDigital> InputActionEvent::getDigitalValue() const noexcept {
        return getActionValue<InputDigital>(*this);
    }

    std::optional<InputAxis1D> InputActionEvent::getAxis1DValue() const noexcept {
        return getActionValue<InputAxis1D>(*this);
    }

    std::optional<InputAxis2D> InputActionEvent::getAxis2DValue() const noexcept {
        return getActionValue<InputAxis2D>(*this);
    }

    std::optional<InputAxis3D> InputActionEvent::getAxis3DValue() const noexcept {
        return getActionValue<InputAxis3D>(*this);
    }

    // ------------------------------------------------------------------
    // input mapping with priority
    // ------------------------------------------------------------------

    bool InputListener::MappingAndPriority::operator==(const MappingAndPriority &other) const noexcept {
        return m_mapping == other.m_mapping;
    }

    bool InputListener::MappingAndPriority::operator==(const InputMapping &mapping) const noexcept {
        return m_mapping.get() == &mapping;
    }

    std::strong_ordering InputListener::MappingAndPriority::operator<=>(const MappingAndPriority &other) const noexcept {
        if (m_priority == other.m_priority) {
            return m_mapping.get() <=> other.m_mapping.get();
        }
        return m_priority <=> other.m_priority;
    }

    std::strong_ordering InputListener::MappingAndPriority::operator<=>(const InputMapping &mapping) const noexcept {
        return m_mapping.get() <=> &mapping;
    }

    // ------------------------------------------------------------------
    // input listener
    // ------------------------------------------------------------------

    bool InputListener::hasInputMapping(const InputMapping &mapping) const noexcept {
        return m_mappings.contains(mapping);
    }

    void InputListener::addMapping(SharedInputMapping mapping, const int priority) {
        if (const bool inserted = m_mappings.emplace(std::move(mapping), priority).second;
            inserted) [[likely]] {
            rebuildKeybindings_();
        }
    }

    bool InputListener::removeMapping(const InputMapping &mapping) {
        const auto it = m_mappings.find(mapping);
        if (it == m_mappings.end()) [[unlikely]] {
            return false;
        }

        m_mappings.erase(it);
        rebuildKeybindings_();
        return true;
    }

    void InputListener::clearAllMappings() {
        m_mappings.clear();
        m_action_events.clear();
    }

    bool InputListener::isKeyHandledByAction(const InputKey &key) const noexcept {
        return m_keybindings.contains(key);
    }

    std::optional<InputValue> InputListener::getActionValue(const InputAction &action) const noexcept {
        return m_action_events.at(&action).m_value;
    }

    static void invokeIFP_(const std::optional<InputModifierEvent> &modifier, InputActionEvent &event, const InputMessage &message) noexcept {
        if (modifier.has_value()) {
            (*modifier)(message.m_delta_time, *event.m_value);
        }
    }

    static void invokeIFP_(const std::optional<InputTriggerEvent> &trigger, const InputActionEvent &event, const InputMessage &message) {
        if (trigger.has_value()) {
            (*trigger)(event, message.m_key);
        }
    }

    EInputListenerResponse InputListener::postKeyEvent(const InputMessage &message) noexcept {
        if (PPR_ENSURE(message.m_key.isAny() && "any keys or axes are not supported as input")) [[unlikely]] {
            return EInputListenerResponse::unhandled;
        }

        const auto [first, last] = m_keybindings.equal_range(message.m_key);
        if (first == last) [[likely]] {
            return EInputListenerResponse::unhandled;
        }

        for (auto it = first; it != last; ++it) {
            const InputActionKeyMapping &key_mapping = getKeyMapping_(it->second);
            PPR_ASSERT(message.m_key == key_mapping.m_key || key_mapping.m_key.isAny());

            InputActionEvent &event = m_action_events.at(key_mapping.m_action);
            event.m_value = message.m_value;

            invokeIFP_(event.m_source->m_modifier, event, message);
            invokeIFP_(key_mapping.m_modifier, event, message);

            switch (message.m_event) {
                case EInputMessageEvent::pressed:
                    event.m_trigger_state = EInputTriggerEvent::started;
                    event.m_elapsed_triggered_time = message.m_delta_time;
                    event.m_repeat_count = 0u;

                    invokeIFP_(key_mapping.m_when_started, event, message);
                    invokeIFP_(event.m_source->m_when_started, event, message);

                    PPR_LOG(Input, verbose, "event started", {
                            {"action", event.m_source->m_description.view()},
                            {"delta_time", event.m_elapsed_triggered_time},
                            {"input_value", opaqueValue(event.m_value)},
                            });
                    break;

                case EInputMessageEvent::released:
                    event.m_trigger_state = EInputTriggerEvent::completed;
                    event.m_repeat_count = 0u;

                    invokeIFP_(key_mapping.m_when_completed, event, message);
                    invokeIFP_(event.m_source->m_when_completed, event, message);

                    PPR_LOG(Input, verbose, "event completed", {
                            {"action", event.m_source->m_description.view()},
                            {"delta_time", event.m_elapsed_triggered_time},
                            {"input_value", event.m_value},
                            });
                    break;

                case EInputMessageEvent::repeat:
                    [[fallthrough]];
                case EInputMessageEvent::double_click:
                    [[fallthrough]];
                case EInputMessageEvent::axis:
                    event.m_trigger_state = EInputTriggerEvent::triggered;
                    if (message.m_event == EInputMessageEvent::repeat) {
                        event.m_elapsed_triggered_time += message.m_delta_time;
                        ++event.m_repeat_count;
                    } else {
                        event.m_elapsed_triggered_time = message.m_delta_time;
                    }

                    invokeIFP_(key_mapping.m_when_triggered, event, message);
                    invokeIFP_(event.m_source->m_when_triggered, event, message);

                    PPR_LOG(Input, verbose, "event triggered", {
                            {"action", event.m_source->m_description.view()},
                            {"delta_time", event.m_elapsed_triggered_time},
                            {"input_value", event.m_value},
                            {"repeat_count", event.m_repeat_count},
                            });
                    break;
            }
        }

        return m_listener_mode;
    }

    const InputActionKeyMapping &InputListener::getKeyMapping_(const InputBinding &binding) const noexcept {
        return (m_mappings.begin() + *binding.m_input_mapping)->
                m_mapping->m_keymap.at(*binding.m_key_mapping);
    }

    void InputListener::rebuildKeybindings_() {
        m_keybindings.clear(); // rebuilding keys and action events

        auto old_action_events = std::move(m_action_events);
        {
            // all that fuzz just to be able to reserve the size of the flat_map :'(
            Array<SharedInputAction> action_events_keys;
            action_events_keys.reserve(old_action_events.size());

            Array<InputActionEvent> action_events_values;
            action_events_values.reserve(old_action_events.size());

            m_action_events.replace(std::move(action_events_keys), std::move(action_events_values));
        }

        const auto map_key = [&](const InputKey &key, const SharedInputAction &action, const InputBinding &binding) -> bool {
            auto [first, last] = m_keybindings.equal_range(key);
            if (first != last) {
                last = std::prev(last);
            }

            if (m_keybindings.end() != last and last->first == key and
                getKeyMapping_(last->second).m_action->hasConsumeInput()) {
                return false;
            }

            if (const auto it = old_action_events.find(action); old_action_events.end() != it) {
                m_action_events.emplace(action, InputActionEvent(action));
            } else {
                m_action_events.emplace(action, std::move(it->second));

                old_action_events.erase(it);
            }

            m_keybindings.emplace(key, binding);
            return true;
        };

        for (const auto &[input_mapping, mapped]: std::ranges::views::enumerate(m_mappings)) {
            for (const auto &[key_mapping, action_keymap]: std::ranges::views::enumerate(mapped.m_mapping->m_keymap)) {
                const InputBinding binding{
                    .m_input_mapping = InputMappingIndex(safe_narrowing(input_mapping)),
                    .m_key_mapping = KeyMappingIndex(safe_narrowing(key_mapping)),
                };

                if (not action_keymap.m_key.isAny()) [[likely]] {
                    map_key(action_keymap.m_key, action_keymap.m_action, binding);
                } else {
                    // expand AnyKey to all possible keys
                    InputKey::enumerateAll([&](const InputKey &key) {
                        if (key.m_value == action_keymap.m_key.m_value) {
                            map_key(key, action_keymap.m_action, binding);
                        }
                    });
                }
            }
        }
    }
}
