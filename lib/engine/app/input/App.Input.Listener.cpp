module;
#include "pP/Macros.h"
module engine.app;

import :input.listener;
import :input.action;
import :input.device;

import engine.core;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(Input, verbose, none)

    // ------------------------------------------------------------------
    // input listener
    // ------------------------------------------------------------------

    InputListener::InputListener() noexcept // NOLINT(*-pro-type-member-init)
        : m_listener_mode(EInputMessageResponse::consumed) {
    }

    bool InputListener::hasInputMapping(const InputMapping &mapping) const noexcept {
        // NOTE: heterogeneous lookup by mapped value; flat_set orders by
        // priority key only, so locate the entry with pointer identity.
        return std::ranges::any_of(m_mappings, [&](const auto &entry) noexcept {
            return entry.m_value.get() == &mapping;
        });
    }

    void InputListener::addInputMapping(SharedInputMapping mapping, const int priority) { // NOLINT(*-unnecessary-value-param)
        if (const bool inserted = m_mappings.emplace(priority, std::move(mapping)).second;
            inserted) [[likely]] {
            rebuildKeybindings_();
        }
    }

    bool InputListener::removeInputMapping(const InputMapping &mapping) {
        const auto it = std::ranges::find_if(m_mappings, [&](const auto &entry) noexcept {
            return entry.m_value.get() == &mapping;
        });
        if (it == m_mappings.end()) [[unlikely]] {
            return false;
        }

        m_mappings.erase(it);
        rebuildKeybindings_();
        return true;
    }

    void InputListener::clearInputMappings() {
        m_mappings.clear();
        m_action_events.clear();
    }

    bool InputListener::isKeyHandledByAction(const InputKey &key) const noexcept {
        return m_keybindings.contains(key);
    }

    std::optional<InputValue> InputListener::getActionValue(const InputAction &action) const noexcept {
        return m_action_events.at(&action).m_value;
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    static void invokeIFP_(const TimeSpan dt, const std::optional<InputModifierEvent> &modifier, InputActionEvent &event, const InputMessage &) noexcept {
        if (modifier.has_value()) {
            (*modifier)(dt, *event.m_value);
        }
    }

    static void invokeIFP_(const std::optional<InputTriggerEvent> &trigger, const InputActionEvent &event, const InputMessage &message) {
        if (trigger.has_value()) {
            (*trigger)(event, message.m_key);
        }
    }

    EInputMessageResponse InputListener::postCharacterInput(hal::native::char_t codepoint) const {
        if (m_listener_mode != EInputMessageResponse::unhandled and
            m_character_input_callback) {
            return m_character_input_callback(codepoint);
        }
        return EInputMessageResponse::unhandled;
    }

    EInputMessageResponse InputListener::postKeyEvent(const TimeSpan dt, const InputMessage &message) noexcept {
        using enum EInputMessageResponse;
        if (m_listener_mode == unhandled) [[unlikely]] {
            return unhandled;
        }

        if (m_raw_key_callback) {
            if (const EInputMessageResponse hook_response = m_raw_key_callback(dt, message);
                hook_response != unhandled) {
                return hook_response;
            }
        }

        if (message.m_key.isAny()) [[unlikely]] {
            return unhandled;
        }

        const auto [first, last] = m_keybindings.equal_range(message.m_key);
        if (first == last) [[likely]] {
            return unhandled;
        }

        bool has_handled_an_action = false;
        for (auto it = first; it != last; ++it) {
            const InputActionKeyMapping &key_mapping = getKeyMapping_(it->second);
            PPR_ASSERT(message.m_key == key_mapping.m_key || key_mapping.m_key.isAny());

            if (key_mapping.m_action->isDisabled()) {
                continue;
            }

            has_handled_an_action = true;

            InputActionEvent &event = m_action_events.at(key_mapping.m_action);
            event.m_value = message.m_value;

            invokeIFP_(dt, event.m_source->m_modifier, event, message);
            invokeIFP_(dt, key_mapping.m_modifier, event, message);

            switch (message.m_event) {
                case EInputMessageEvent::released:
                    event.m_trigger_state = EInputTriggerEvent::completed;
                    event.m_repeat_count = 0u;

                    invokeIFP_(key_mapping.m_when_completed, event, message);
                    invokeIFP_(event.m_source->m_when_completed, event, message);

                    if (m_action_callback) {
                        m_action_callback(event, message.m_key);
                    }

                    PPR_LOG(Input, verbose, "event completed", {
                        {"action", event.m_source->m_description.view()},
                        {"delta_time", event.m_elapsed_triggered_time},
                        {"input_key", message.m_key.m_name},
                        {"input_value", event.m_value},
                        {"consume", key_mapping.m_action->hasConsumeInput()},
                        });
                    break;

                case EInputMessageEvent::pressed:
                    event.m_trigger_state = EInputTriggerEvent::started;
                    event.m_elapsed_triggered_time = dt;
                    event.m_repeat_count = 0u;

                    invokeIFP_(key_mapping.m_when_started, event, message);
                    invokeIFP_(event.m_source->m_when_started, event, message);

                    if (m_action_callback) {
                        m_action_callback(event, message.m_key);
                    }

                    PPR_LOG(Input, verbose, "event started", {
                        {"action", event.m_source->m_description.view()},
                        {"delta_time", event.m_elapsed_triggered_time},
                        {"input_key", message.m_key.m_name},
                        {"input_value", event.m_value},
                        {"consume", key_mapping.m_action->hasConsumeInput()},
                        });

                    [[fallthrough]];
                case EInputMessageEvent::repeat:
                    [[fallthrough]];
                case EInputMessageEvent::double_click:
                    [[fallthrough]];
                case EInputMessageEvent::axis:
                    event.m_trigger_state = EInputTriggerEvent::triggered;

                    if (message.m_event == EInputMessageEvent::repeat) {
                        event.m_elapsed_triggered_time += dt;
                        ++event.m_repeat_count;
                    } else {
                        event.m_elapsed_triggered_time = dt;
                    }

                    invokeIFP_(key_mapping.m_when_triggered, event, message);
                    invokeIFP_(event.m_source->m_when_triggered, event, message);

                    if (m_action_callback) {
                        m_action_callback(event, message.m_key);
                    }

                    PPR_LOG(Input, debug, "event triggered", {
                        {"action", event.m_source->m_description.view()},
                        {"delta_time", event.m_elapsed_triggered_time},
                        {"input_key", message.m_key.m_name},
                        {"input_value", event.m_value},
                        {"repeat_count", event.m_repeat_count},
                        {"consume", key_mapping.m_action->hasConsumeInput()},
                        });
                    break;
            }

            // do not follow with other bound actions if this one consumed the input:
            if (key_mapping.m_action->hasConsumeInput()) {
                return m_listener_mode;
            }
        }

        return has_handled_an_action ? handled : unhandled;
    }

    const InputActionKeyMapping &InputListener::getKeyMapping_(const InputBinding &binding) const noexcept {
        // NOTE: flat_set iterators yield the pair itself; unwrap to the
        // mapped InputMapping before reaching its keymap.
        return (m_mappings.begin() + *binding.m_input_mapping)->m_value->m_keymap.at(*binding.m_key_mapping);
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
                m_action_events.emplace(action, std::move(it->second));

                old_action_events.erase(it);
            } else {
                m_action_events.emplace(action, InputActionEvent(action));
            }

            m_keybindings.emplace(key, binding);
            return true;
        };

        for (const auto &[input_mapping, mapped]: std::ranges::views::enumerate(m_mappings)) {
            for (const auto &[key_mapping, action_keymap]: std::ranges::views::enumerate(mapped.m_value->m_keymap)) {
                const InputBinding binding{
                    .m_input_mapping = InputMappingIndex(safe_narrowing(input_mapping)),
                    .m_key_mapping = KeyMappingIndex(safe_narrowing(key_mapping)),
                };

                if (not action_keymap.m_key.isAny()) [[likely]] {
                    map_key(action_keymap.m_key, action_keymap.m_action, binding);
                } else {
                    // expand EAnyKey to all possible keys for selected sources
                    InputKey::enumerateAny(get<EAnyKey>(action_keymap.m_key.m_code), [&](const InputKey &key) -> std::error_code {
                        if (key.m_value == action_keymap.m_key.m_value) {
                            map_key(key, action_keymap.m_action, binding);
                        }
                        return default_value_v;
                    });
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // input context
    // ------------------------------------------------------------------

    InputContext::InputContext(safe_ptr<InputContext> parent_context) noexcept
        : m_parent(std::move(parent_context)) {
    }

    InputContext::InputContext(InputContext &&other) noexcept = default;

    bool InputContext::hasInputListener(const InputListener &listener) const noexcept {
        // NOTE: see hasInputMapping — locate by pointer identity.
        return std::ranges::any_of(m_listeners, [&](const auto &entry) noexcept {
            return entry.m_value.get() == &listener;
        });
    }

    void InputContext::addInputListener(safe_ptr<InputListener> listener, int priority) { // NOLINT(*-unnecessary-value-param)
        m_listeners.emplace(priority, std::move(listener));
    }

    bool InputContext::removeInputListener(const InputListener &listener) {
        const auto it = std::ranges::find_if(m_listeners, [&](const auto &entry) noexcept {
            return entry.m_value.get() == &listener;
        });
        if (it == m_listeners.end()) [[unlikely]] {
            return false;
        }

        m_listeners.erase(it);
        return true;
    }

    void InputContext::clearInputListeners() {
        m_listeners.clear();
    }

    EInputMessageResponse InputContext::postCharacterInput(hal::native::char_t codepoint) const {
        using enum EInputMessageResponse;
        auto response{unhandled};

        for (const auto &[_, listener]: m_listeners) {
            switch (listener->postCharacterInput(codepoint)) {
                case consumed:
                    return consumed;
                case handled:
                    response = handled;
                case unhandled:
                    break;
            }
        }

        if (const InputContext *const p_parent = m_parent.get()) {
            switch (p_parent->postCharacterInput(codepoint)) {
                case consumed:
                    return consumed;
                case handled:
                    response = handled;
                case unhandled:
                    break;
            }
        }

        return response;
    }

    EInputMessageResponse InputContext::postKeyEvent(const TimeSpan dt, const InputMessage &message) const {
        using enum EInputMessageResponse;
        auto response{unhandled};

        for (const auto &[_, listener]: m_listeners) {
            switch (listener->postKeyEvent(dt, message)) {
                case consumed:
                    return consumed;
                case handled:
                    response = handled;
                case unhandled:
                    break;
            }
        }

        if (const InputContext *const p_parent = m_parent.get()) {
            switch (p_parent->postKeyEvent(dt, message)) {
                case consumed:
                    return consumed;
                case handled:
                    response = handled;
                case unhandled:
                    break;
            }
        }

        return response;
    }

    // ------------------------------------------------------------------
    // window input context - setup an input context for a specific window
    // ------------------------------------------------------------------

    WindowInputContext::WindowInputContext(safe_ptr<IInputService> inputs)
        : m_inputs(std::move(inputs)),
          m_context(safe_ptr(&m_inputs->getGlobalInputContext())) {
        PPR_ASSERT(m_inputs.isValid());

        m_inputs->addInputContext(SharedInputContext(&m_context));
    }

    WindowInputContext::~WindowInputContext() {
        PPR_ASSERT(not m_window.isValid());

        m_inputs->removeInputContext(m_context);
    }

    std::error_code WindowInputContext::initialize(safe_ptr<Window> window) {
        if (m_window.isValid()) [[unlikely]] {
            return make_error_code(std::errc::already_connected);
        }
        if (not window.isValid()) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        m_window = std::move(window);

        m_window->m_when_character_input.subscribe<&WindowInputContext::onWindowCharacterInput_>(this);
        m_window->m_when_keyboard_pressed.subscribe<&WindowInputContext::onKeyboardPressed_>(this);

        m_window->m_when_mouse_clicked.subscribe<&WindowInputContext::onMouseClicked_>(this);
        m_window->m_when_mouse_moved.subscribe<&WindowInputContext::onMouseMoved_>(this);
        m_window->m_when_mouse_scrolled.subscribe<&WindowInputContext::onMouseScrolled_>(this);

        return default_value_v;
    }

    std::error_code WindowInputContext::shutdown() {
        if (not m_window) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        m_window->m_when_mouse_clicked.reset();
        m_window->m_when_mouse_moved.reset();
        m_window->m_when_mouse_scrolled.reset();

        m_window->m_when_character_input.reset();
        m_window->m_when_keyboard_pressed.reset();

        m_window.reset();

        return default_value_v;
    }

    void WindowInputContext::onWindowCharacterInput_([[maybe_unused]] const Window &window, const hal::native::char_t codepoint) const {
        PPR_ASSERT(m_window.get() == &window);
        m_inputs->postKeyboardCharacterInput(m_context, codepoint);
    }

    void WindowInputContext::onKeyboardPressed_([[maybe_unused]] const Window &window, const EKeyboardKey key, const bool pressed) const {
        PPR_ASSERT(m_window.get() == &window);
        m_inputs->postKeyboardKeyPressed(m_context, key, pressed);
    }

    void WindowInputContext::onMouseClicked_([[maybe_unused]] const Window &window, const EMouseButton button, const bool clicked) const {
        PPR_ASSERT(m_window.get() == &window);
        m_inputs->postMouseButtonPressed(m_context, button, clicked);
    }

    void WindowInputContext::onMouseMoved_(const Window &window, const float2 &client_pos) const {
        PPR_ASSERT(m_window.get() == &window);
        // GLFW cursor positions are already client-area relative and downstream
        // consumers (e.g. ImGui mouse input) expect client space: forward as-is.
        m_inputs->postMouseCursorPosition(m_context, client_pos);
    }

    void WindowInputContext::onMouseScrolled_([[maybe_unused]] const Window &window, const float2 &delta) const {
        PPR_ASSERT(m_window.get() == &window);
        m_inputs->postMouseScrollWheel(m_context, delta);
    }
}
