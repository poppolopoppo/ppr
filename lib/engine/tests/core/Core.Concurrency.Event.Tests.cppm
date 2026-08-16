module;
#include "pP/UnitTest.h"
export module engine.tests.core:event;
import engine.core;
import std;

export namespace pP::tests {

    namespace Events {
        class DummySignal final : public ISignal {
        public:
            std::size_t m_expected_tag{0u};

            explicit DummySignal(const std::size_t expected_tag) noexcept
                : m_expected_tag{expected_tag} {
            }

            void notify([[maybe_unused]] const std::size_t event_tag) noexcept override {
                PPR_TEST_ASSERT(event_tag == m_expected_tag);
            }

            void wait() noexcept override {

            }
        };

        namespace Never {
            PPR_UNIT_TEST(poll_returns_false) {
                NeverEvent event;
                PPR_TEST_ASSERT(!event.pollEvent());
            };

            PPR_UNIT_TEST(subscribe_returns_default) {
                NeverEvent event;
                const auto restore = event.subscribeEvent(TagPtr<ISignal>{nullptr, 0u});
                PPR_TEST_ASSERT(restore.isNull());
            };

            PPR_UNIT_TEST(unsubscribe_noop) {
                NeverEvent event;
                event.unsubscribeEvent(TagPtr<ISignal>{nullptr, 0u}, default_value_v);
            };

            PPR_UNIT_TEST(reset_noop) {
                NeverEvent event;
                event.resetEvent();
                PPR_TEST_ASSERT(!event.pollEvent());
            };
        }

        namespace Pulse {
            PPR_UNIT_TEST(poll_initially_false) {
                PulseEvent event;
                PPR_TEST_ASSERT(!event.pollEvent());
            };

            PPR_UNIT_TEST(emit_sets_flag) {
                PulseEvent event;
                event.emitEvent();
                PPR_TEST_ASSERT(event.pollEvent());
            };

            PPR_UNIT_TEST(reset_clears_flag) {
                PulseEvent event;
                event.emitEvent();
                PPR_TEST_ASSERT(event.pollEvent());
                event.resetEvent();
                PPR_TEST_ASSERT(!event.pollEvent());
            };

            PPR_UNIT_TEST(emit_twice_stays_set) {
                PulseEvent event;
                event.emitEvent();
                event.emitEvent();
                PPR_TEST_ASSERT(event.pollEvent());
            };

            PPR_UNIT_TEST(reset_then_emit_sets_again) {
                PulseEvent event;
                event.emitEvent();
                event.resetEvent();
                event.emitEvent();
                PPR_TEST_ASSERT(event.pollEvent());
            };

            PPR_UNIT_TEST(subscribe_returns_previous) {
                PulseEvent event;
                DummySignal dummy{5u};
                auto prev = event.subscribeEvent(TagPtr<ISignal>{&dummy, dummy.m_expected_tag});
                PPR_TEST_ASSERT(prev.isNull());
                PPR_TEST_ASSERT(prev.getTag() == 0u);
                prev = event.subscribeEvent(prev);
                PPR_TEST_ASSERT(prev.getData() == &dummy);
                PPR_TEST_ASSERT(prev.getTag() == dummy.m_expected_tag);
            };

            PPR_UNIT_TEST(unsubscribe_restores_previous) {
                PulseEvent event;
                const TagPtr<ISignal> dummy{nullptr, 5u};
                const auto prev = event.subscribeEvent(dummy);
                event.unsubscribeEvent(dummy, prev);
                PPR_TEST_ASSERT(!event.pollEvent());
            };
        }

        namespace Broadcast {
            PPR_UNIT_TEST(poll_initially_false) {
                BroadcastEvent event;
                PPR_TEST_ASSERT(!event.pollEvent());
            };

            PPR_UNIT_TEST(emit_sets_flag) {
                BroadcastEvent event;
                event.emitEvent();
                PPR_TEST_ASSERT(event.pollEvent());
            };

            PPR_UNIT_TEST(reset_clears_flag) {
                BroadcastEvent event;
                event.emitEvent();
                event.resetEvent();
                PPR_TEST_ASSERT(!event.pollEvent());
            };

            PPR_UNIT_TEST(subscribe_returns_default) {
                BroadcastEvent event;
                DummySignal dummy{5u};
                auto prev = event.subscribeEvent(TagPtr<ISignal>{&dummy, dummy.m_expected_tag});
                PPR_TEST_ASSERT(prev.isNull());
                PPR_TEST_ASSERT(prev.getTag() == 0u);
                prev = event.subscribeEvent(prev);
                PPR_TEST_ASSERT(prev.getData() == nullptr);
            };

            PPR_UNIT_TEST(unsubscribe_removes_subscriber) {
                BroadcastEvent event;
                const TagPtr<ISignal> dummy{nullptr, 5u};
                const auto prev = event.subscribeEvent(dummy);
                event.unsubscribeEvent(dummy, prev);
                PPR_TEST_ASSERT(!event.pollEvent());
            };
        }

        namespace SignalSingle {
            PPR_UNIT_TEST(poll_empty_returns_nullopt) {
                PulseEvent event;
                auto signal = select(event);
                const auto result = signal.poll();
                PPR_TEST_ASSERT(!result.has_value());
            };

            PPR_UNIT_TEST(poll_after_emit_returns_event) {
                PulseEvent event;
                auto signal = select(event);
                event.emitEvent();
                const auto result = signal.poll();
                PPR_TEST_ASSERT(result.has_value());
                PPR_TEST_ASSERT(result.value() == std::addressof(event));
            };

            PPR_UNIT_TEST(reset_clears_pending) {
                PulseEvent event;
                auto signal = select(event);
                event.emitEvent();
                const auto result = signal.poll();
                PPR_TEST_ASSERT(result.has_value());
                signal.reset();
                const auto result2 = signal.poll();
                PPR_TEST_ASSERT(!result2.has_value());
            };

            PPR_UNIT_TEST(iterator_sentinel) {
                PulseEvent event;
                auto signal = select(event);
                event.emitEvent();
                const auto it = signal.begin();
                const auto end = signal.end();
                PPR_TEST_ASSERT(it != end);
            };
        }

        namespace SignalMulti {
            PPR_UNIT_TEST(poll_two_events_emits_first) {
                PulseEvent a;
                PulseEvent b;
                auto signal = select(a, b);
                a.emitEvent();
                const auto result = signal.poll();
                PPR_TEST_ASSERT(result.has_value());
                PPR_TEST_ASSERT(result->index() == 0u);
            };

            PPR_UNIT_TEST(poll_two_events_emits_second) {
                PulseEvent a;
                PulseEvent b;
                auto signal = select(a, b);
                b.emitEvent();
                const auto result = signal.poll();
                PPR_TEST_ASSERT(result.has_value());
                PPR_TEST_ASSERT(result->index() == 1u);
            };

            PPR_UNIT_TEST(reset_clears_specific_event) {
                PulseEvent a;
                PulseEvent b;
                auto signal = select(a, b);
                a.emitEvent();
                const auto result = signal.poll();
                PPR_TEST_ASSERT(result.has_value());
                signal.reset(*result);
                const auto result2 = signal.poll();
                PPR_TEST_ASSERT(!result2.has_value());
            };

            PPR_UNIT_TEST(both_events_set) {
                PulseEvent a;
                PulseEvent b;
                auto signal = select(a, b);
                a.emitEvent();
                b.emitEvent();
                const auto r1 = signal.poll();
                PPR_TEST_ASSERT(r1.has_value());
                const auto r2 = signal.poll();
                PPR_TEST_ASSERT(r2.has_value());
            };
        }

        PPR_UNIT_TEST(never_event) {
            _.recurse({
                Never::poll_returns_false,
                Never::subscribe_returns_default,
                Never::unsubscribe_noop,
                Never::reset_noop,
            });
        };

        PPR_UNIT_TEST(pulse_event) {
            _.recurse({
                Pulse::poll_initially_false,
                Pulse::emit_sets_flag,
                Pulse::reset_clears_flag,
                Pulse::emit_twice_stays_set,
                Pulse::reset_then_emit_sets_again,
                Pulse::subscribe_returns_previous,
                Pulse::unsubscribe_restores_previous,
            });
        };

        PPR_UNIT_TEST(broadcast_event) {
            _.recurse({
                Broadcast::poll_initially_false,
                Broadcast::emit_sets_flag,
                Broadcast::reset_clears_flag,
                Broadcast::subscribe_returns_default,
                Broadcast::unsubscribe_removes_subscriber,
            });
        };

        PPR_UNIT_TEST(signal_single) {
            _.recurse({
                SignalSingle::poll_empty_returns_nullopt,
                SignalSingle::poll_after_emit_returns_event,
                SignalSingle::reset_clears_pending,
                SignalSingle::iterator_sentinel,
            });
        };

        PPR_UNIT_TEST(signal_multi) {
            _.recurse({
                SignalMulti::poll_two_events_emits_first,
                SignalMulti::poll_two_events_emits_second,
                SignalMulti::reset_clears_specific_event,
                SignalMulti::both_events_set,
            });
        };
    }

    PPR_UNIT_TEST(event) {
        _.recurse({
            Events::never_event,
            Events::pulse_event,
            Events::broadcast_event,
            Events::signal_single,
            Events::signal_multi,
        });
    };
}
