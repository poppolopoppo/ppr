module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // input listener
    // ------------------------------------------------------------------

    // Minimal InputListener wired to a single action/key mapping; posts synthetic
    // InputMessages and records which trigger handlers fired.
    struct ListenerHarness {
        InputAction action;
        InputMapping mapping;
        InputListener listener;
        InputKey m_key;

        explicit ListenerHarness(
            const InputKey key = InputKey::w, const EInputValueType value_type = EInputValueType::digital)
            : action{"TestAction", value_type, EInputActionFlags::none}, mapping{"TestMapping"}, listener{},
              m_key{key} {
            mapping.mapInputKey(SharedInputAction{&action}, m_key);
            listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        }

        EInputMessageResponse post(const EInputMessageEvent event, InputValue value = InputValue{InputDigital{true}}) {
            const InputMessage msg{m_key, std::move(value), InputDeviceID{0u}, event};
            return listener.postKeyEvent(TimeSpan{}, msg);
        }
    };

    PPR_UNIT_TEST (pressed_fires_started_handler) {
        ListenerHarness h;
        int started_count = 0;
        bool saw_started_state = false;
        h.action.setStarted([&](const InputActionEvent &event, const InputKey &) noexcept {
            ++started_count;
            saw_started_state = event.isTriggerStarted();
        });
        h.post(EInputMessageEvent::pressed);
        PPR_TEST_ASSERT(started_count == 1);
        PPR_TEST_ASSERT(saw_started_state);
    };

    PPR_UNIT_TEST (pressed_fires_triggered_handler) {
        ListenerHarness h;
        int triggered_count = 0;
        h.action.setTriggered(
            [&triggered_count](const InputActionEvent &, const InputKey &) noexcept { ++triggered_count; });
        h.post(EInputMessageEvent::pressed);
        PPR_TEST_ASSERT(triggered_count == 1);
    };

    PPR_UNIT_TEST (repeat_fires_triggered_handler) {
        ListenerHarness h;
        int triggered_count = 0;
        h.action.setTriggered(
            [&triggered_count](const InputActionEvent &, const InputKey &) noexcept { ++triggered_count; });
        h.post(EInputMessageEvent::repeat);
        PPR_TEST_ASSERT(triggered_count == 1);
    };

    PPR_UNIT_TEST (axis_fires_triggered_handler) {
        ListenerHarness h{InputKey::mouse_2d, EInputValueType::axis_2d};
        int triggered_count = 0;
        h.action.setTriggered(
            [&triggered_count](const InputActionEvent &, const InputKey &) noexcept { ++triggered_count; });
        h.post(EInputMessageEvent::axis, InputValue{InputAxis2D{float2{1.0f, 0.0f}, float2{1.0f, 0.0f}}});
        PPR_TEST_ASSERT(triggered_count == 1);
    };

    PPR_UNIT_TEST (released_fires_completed_handler) {
        ListenerHarness h;
        int completed_count = 0;
        h.action.setCompleted(
            [&completed_count](const InputActionEvent &, const InputKey &) noexcept { ++completed_count; });
        h.post(EInputMessageEvent::released);
        PPR_TEST_ASSERT(completed_count == 1);
    };

    PPR_UNIT_TEST (pressed_does_not_fire_completed_handler) {
        ListenerHarness h;
        int completed_count = 0;
        h.action.setCompleted(
            [&completed_count](const InputActionEvent &, const InputKey &) noexcept { ++completed_count; });
        h.post(EInputMessageEvent::pressed);
        PPR_TEST_ASSERT(completed_count == 0);
    };

    PPR_UNIT_TEST (modulate_fires_once_per_event_not_per_trigger) {
        ListenerHarness h;
        int modulate_count = 0;
        h.action.m_modifier = [&modulate_count](TimeSpan, InputValue &) noexcept { ++modulate_count; };
        h.action.setStarted([](const InputActionEvent &, const InputKey &) noexcept {});
        h.action.setTriggered([](const InputActionEvent &, const InputKey &) noexcept {});
        h.post(EInputMessageEvent::pressed);
        PPR_TEST_ASSERT(modulate_count == 1);
    };

    // Two mappings bound to the same key at different priorities; observes
    // per-action trigger counts, dispatch order, and live event state.
    struct ConsumeHarness {
        InputAction upper;
        InputAction lower;
        InputMapping upper_mapping;
        InputMapping lower_mapping;
        InputListener listener;
        InputKey m_key;

        int upper_count = 0;
        int lower_count = 0;
        Array<int> fire_order{};
        int lower_repeat_count = -1;

        explicit ConsumeHarness(
            const EInputActionFlags upper_flags = EInputActionFlags::consume_input,
            const EInputActionFlags lower_flags = EInputActionFlags::none, const int upper_priority = 0,
            const int lower_priority = 1, const bool lower_first = false)
            : upper{"Upper", EInputValueType::digital, upper_flags},
              lower{"Lower", EInputValueType::digital, lower_flags}, upper_mapping{"UpperMapping"},
              lower_mapping{"LowerMapping"}, listener{}, m_key{InputKey::w} {
            upper_mapping.mapInputKey(SharedInputAction{&upper}, m_key);
            lower_mapping.mapInputKey(SharedInputAction{&lower}, m_key);
            if (lower_first) {
                listener.addInputMapping(SharedInputMapping{&lower_mapping}, lower_priority);
                listener.addInputMapping(SharedInputMapping{&upper_mapping}, upper_priority);
            } else {
                listener.addInputMapping(SharedInputMapping{&upper_mapping}, upper_priority);
                listener.addInputMapping(SharedInputMapping{&lower_mapping}, lower_priority);
            }

            upper.setTriggered([this](const InputActionEvent &, const InputKey &) {
                ++upper_count;
                fire_order.push_back(0);
            });
            lower.setTriggered([this](const InputActionEvent &, const InputKey &) {
                ++lower_count;
                fire_order.push_back(1);
            });
            listener.setActionCallback([this](const InputActionEvent &event, const InputKey &) {
                if (event.m_source.get() == &lower) {
                    lower_repeat_count = static_cast<int>(event.m_repeat_count);
                }
            });
        }

        EInputMessageResponse post(const EInputMessageEvent event) {
            const InputMessage msg{m_key, InputValue{InputDigital{true}}, InputDeviceID{0u}, event};
            return listener.postKeyEvent(TimeSpan{}, msg);
        }
    };

    PPR_UNIT_TEST (consume_flag_off_without_rebuild_runs_shadowed_action) {
        ConsumeHarness h;
        PPR_TEST_ASSERT(h.post(EInputMessageEvent::pressed) == EInputMessageResponse::consumed);
        PPR_TEST_ASSERT(h.upper_count == 1);
        PPR_TEST_ASSERT(h.lower_count == 0);

        h.upper.setConsumeInput(false);

        PPR_TEST_ASSERT(h.post(EInputMessageEvent::pressed) == EInputMessageResponse::handled);
        PPR_TEST_ASSERT(h.upper_count == 2);
        PPR_TEST_ASSERT(h.lower_count == 1);
    };

    PPR_UNIT_TEST (consume_flag_on_without_rebuild_suppresses_lower_priority) {
        ConsumeHarness h{EInputActionFlags::none, EInputActionFlags::none};
        PPR_TEST_ASSERT(h.post(EInputMessageEvent::pressed) == EInputMessageResponse::handled);
        PPR_TEST_ASSERT(h.upper_count == 1);
        PPR_TEST_ASSERT(h.lower_count == 1);

        h.upper.setConsumeInput(true);

        PPR_TEST_ASSERT(h.post(EInputMessageEvent::pressed) == EInputMessageResponse::consumed);
        PPR_TEST_ASSERT(h.upper_count == 2);
        PPR_TEST_ASSERT(h.lower_count == 1);
    };

    PPR_UNIT_TEST (same_key_dispatch_follows_mapping_priority) {
        ConsumeHarness h{EInputActionFlags::none, EInputActionFlags::consume_input, 0, 1, true};
        const auto response = h.post(EInputMessageEvent::pressed);
        PPR_TEST_ASSERT(h.upper_count == 1);
        PPR_TEST_ASSERT(h.lower_count == 1);
        PPR_TEST_ASSERT(h.fire_order.size() == 2u);
        PPR_TEST_ASSERT(h.fire_order[0] == 0);
        PPR_TEST_ASSERT(h.fire_order[1] == 1);
        PPR_TEST_ASSERT(response == EInputMessageResponse::consumed);
    };

    PPR_UNIT_TEST (remove_and_readd_consuming_mapping_preserves_live_state) {
        ConsumeHarness h;
        PPR_TEST_ASSERT(h.post(EInputMessageEvent::pressed) == EInputMessageResponse::consumed);
        PPR_TEST_ASSERT(h.lower_count == 0);

        PPR_TEST_ASSERT(h.listener.removeInputMapping(h.upper_mapping));
        PPR_TEST_ASSERT(h.post(EInputMessageEvent::pressed) == EInputMessageResponse::handled);
        PPR_TEST_ASSERT(h.lower_count == 1);
        h.post(EInputMessageEvent::repeat);
        h.post(EInputMessageEvent::repeat);
        PPR_TEST_ASSERT(h.lower_repeat_count == 2);

        h.listener.addInputMapping(SharedInputMapping{&h.upper_mapping}, 0);

        // live state survives the rebuild even though the consuming mapping
        // shadows the key again: the value entry is still present ...
        const auto retained = h.listener.getActionValue(h.lower);
        PPR_TEST_ASSERT(retained.has_value());

        // ... and the shadowed action is suppressed again:
        h.post(EInputMessageEvent::pressed);
        PPR_TEST_ASSERT(h.lower_count == 3);

        // removing the consumer resumes from preserved repeat state, not fresh:
        PPR_TEST_ASSERT(h.listener.removeInputMapping(h.upper_mapping));
        h.post(EInputMessageEvent::repeat);
        PPR_TEST_ASSERT(h.lower_repeat_count == 3);
    };
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest input_listener = UnitTest::Named("input_listener") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::pressed_fires_started_handler,
            detail::pressed_fires_triggered_handler,
            detail::repeat_fires_triggered_handler,
            detail::axis_fires_triggered_handler,
            detail::released_fires_completed_handler,
            detail::pressed_does_not_fire_completed_handler,
            detail::modulate_fires_once_per_event_not_per_trigger,
            detail::consume_flag_off_without_rebuild_runs_shadowed_action,
            detail::consume_flag_on_without_rebuild_suppresses_lower_priority,
            detail::same_key_dispatch_follows_mapping_priority,
            detail::remove_and_readd_consuming_mapping_preserves_live_state,
        });
    };

    const UnitTest &input_listenerTests() noexcept {
        return input_listener;
    }
} // namespace pP::tests
