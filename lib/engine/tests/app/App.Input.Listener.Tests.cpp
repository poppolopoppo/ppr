module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import std;

namespace pP::tests::detail {
    // Minimal InputListener wired to a single action/key mapping; posts synthetic
    // InputMessages and records which trigger handlers fired.
    struct ListenerHarness {
        InputAction action;
        InputMapping mapping;
        InputListener listener;
        InputKey m_key;

        explicit ListenerHarness(
            const InputKey key = InputKey::w,
            const EInputValueType value_type = EInputValueType::digital)
            : action{"TestAction", value_type, EInputActionFlags::none},
              mapping{"TestMapping"},
              listener{},
              m_key{key} {
            mapping.mapInputKey(SharedInputAction{&action}, m_key);
            listener.addInputMapping(SharedInputMapping{&mapping}, 0);
        }

        EInputMessageResponse post(
            const EInputMessageEvent event,
            InputValue value = InputValue{InputDigital{true}}) {
            const InputMessage msg{
                m_key,
                std::move(value),
                InputDeviceID{0u},
                event
            };
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
        h.action.setTriggered([&triggered_count](const InputActionEvent &, const InputKey &) noexcept {
            ++triggered_count;
        });
        h.post(EInputMessageEvent::pressed);
        PPR_TEST_ASSERT(triggered_count == 1);
    };

    PPR_UNIT_TEST (repeat_fires_triggered_handler) {
        ListenerHarness h;
        int triggered_count = 0;
        h.action.setTriggered([&triggered_count](const InputActionEvent &, const InputKey &) noexcept {
            ++triggered_count;
        });
        h.post(EInputMessageEvent::repeat);
        PPR_TEST_ASSERT(triggered_count == 1);
    };

    PPR_UNIT_TEST (axis_fires_triggered_handler) {
        ListenerHarness h{InputKey::mouse_2d, EInputValueType::axis_2d};
        int triggered_count = 0;
        h.action.setTriggered([&triggered_count](const InputActionEvent &, const InputKey &) noexcept {
            ++triggered_count;
        });
        h.post(EInputMessageEvent::axis, InputValue{InputAxis2D{float2{1.0f, 0.0f}, float2{1.0f, 0.0f}}});
        PPR_TEST_ASSERT(triggered_count == 1);
    };

    PPR_UNIT_TEST (released_fires_completed_handler) {
        ListenerHarness h;
        int completed_count = 0;
        h.action.setCompleted([&completed_count](const InputActionEvent &, const InputKey &) noexcept {
            ++completed_count;
        });
        h.post(EInputMessageEvent::released);
        PPR_TEST_ASSERT(completed_count == 1);
    };

    PPR_UNIT_TEST (pressed_does_not_fire_completed_handler) {
        ListenerHarness h;
        int completed_count = 0;
        h.action.setCompleted([&completed_count](const InputActionEvent &, const InputKey &) noexcept {
            ++completed_count;
        });
        h.post(EInputMessageEvent::pressed);
        PPR_TEST_ASSERT(completed_count == 0);
    };

    PPR_UNIT_TEST (modulate_fires_once_per_event_not_per_trigger) {
        ListenerHarness h;
        int modulate_count = 0;
        h.action.m_modifier = [&modulate_count](TimeSpan, InputValue &) noexcept {
            ++modulate_count;
        };
        h.action.setStarted([](const InputActionEvent &, const InputKey &) noexcept {
        });
        h.action.setTriggered([](const InputActionEvent &, const InputKey &) noexcept {
        });
        h.post(EInputMessageEvent::pressed);
        PPR_TEST_ASSERT(modulate_count == 1);
    };
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest input_listener = UnitTest::Named("input_listener") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::pressed_fires_started_handler,
            detail::pressed_fires_triggered_handler,
            detail::repeat_fires_triggered_handler,
            detail::axis_fires_triggered_handler,
            detail::released_fires_completed_handler,
            detail::pressed_does_not_fire_completed_handler,
            detail::modulate_fires_once_per_event_not_per_trigger,
        });
    };
} // namespace pP::tests
