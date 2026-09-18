module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Context {
        namespace Background {
            PPR_UNIT_TEST (done_is_never_event) {
                const SharedContext ctx = context::background();
                PPR_TEST_ASSERT(ctx->pollEvent() == false);
            };

            PPR_UNIT_TEST (error_is_none) {
                const SharedContext ctx = context::background();
                PPR_TEST_ASSERT(not ctx->error());
            };

            PPR_UNIT_TEST (value_is_none) {
                const SharedContext ctx = context::background();
                PPR_TEST_ASSERT(not ctx->value("key").has_value());
            };
        }

        PPR_UNIT_TEST (background){
            _.recurse({
                Background::done_is_never_event,
                Background::error_is_none,
                Background::value_is_none,
            });


        };

        namespace Cancel {
            PPR_UNIT_TEST (done_initially_empty) {
                const auto [ctx, cancel] = context::withCancel(context::background());
                PPR_TEST_ASSERT(ctx->pollEvent() == false);
            };

            PPR_UNIT_TEST (manual_cancel_fires_done) {
                const auto [ctx, cancel] = context::withCancel(context::background());
                cancel();
                PPR_TEST_ASSERT(ctx->pollEvent());
                PPR_TEST_ASSERT(!!ctx->error());
            };

            PPR_UNIT_TEST (cancel_is_idempotent) {
                const auto [ctx, cancel] = context::withCancel(context::background());
                cancel();
                cancel();
                cancel();
                PPR_TEST_ASSERT(!!ctx->error());
                PPR_TEST_ASSERT(ctx->pollEvent());
            };

            PPR_UNIT_TEST (parent_cancel_propagates) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                const auto [child, cancel_child] = context::withCancel(parent);
                cancel_parent();
                PPR_TEST_ASSERT(child->pollEvent());
                PPR_TEST_ASSERT(!!child->error());
            };

            PPR_UNIT_TEST (grandparent_cancel_propagates) {
                const auto [gp, cancel_gp] = context::withCancel(context::background());
                const auto [parent, cancel_parent] = context::withCancel(gp);
                const auto [child, cancel_child] = context::withCancel(parent);
                cancel_gp();
                PPR_TEST_ASSERT(child->pollEvent());
            };

            PPR_UNIT_TEST (child_cancel_does_not_affect_parent) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                const auto [child, cancel_child] = context::withCancel(parent);
                cancel_child();
                PPR_TEST_ASSERT(!parent->pollEvent());
                PPR_TEST_ASSERT(!parent->error());
            };
        }

        PPR_UNIT_TEST (cancel){
            _.recurse({
                Cancel::done_initially_empty,
                Cancel::manual_cancel_fires_done,
                Cancel::cancel_is_idempotent,
                Cancel::parent_cancel_propagates,
                Cancel::grandparent_cancel_propagates,
                Cancel::child_cancel_does_not_affect_parent,
            });


        };

        namespace CancelClause {
            PPR_UNIT_TEST (clause_fires_done_with_error) {
                const auto [ctx, cancel_] = context::withCancelClause(context::background());
                const std::error_code err{42, std::generic_category()};
                cancel_(err);
                PPR_TEST_ASSERT(ctx->pollEvent());
                PPR_TEST_ASSERT(!!ctx->error());
            };

            PPR_UNIT_TEST (clause_error_matches) {
                const auto [ctx, cancel_] = context::withCancelClause(context::background());
                const std::error_code err{42, std::generic_category()};
                cancel_(err);
                PPR_TEST_ASSERT(!!ctx->error());
                PPR_TEST_ASSERT(ctx->error().value() == 42);
            };
        }


        PPR_UNIT_TEST (cancel_clause){
            _.recurse({
                CancelClause::clause_fires_done_with_error,
                CancelClause::clause_error_matches,
            });


        };

        namespace WithoutCancel {
            PPR_UNIT_TEST (done_never_fires) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                const SharedContext child = context::withoutCancel(parent);
                cancel_parent();
                PPR_TEST_ASSERT(not child->pollEvent());
            };

            PPR_UNIT_TEST (error_always_none) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                const SharedContext child = context::withoutCancel(parent);
                cancel_parent();
                PPR_TEST_ASSERT(not child->error());
            };
        }

        PPR_UNIT_TEST (without_cancel){
            _.recurse({
                WithoutCancel::done_never_fires,
                WithoutCancel::error_always_none,
            });


        };

        namespace AfterFunc {
            PPR_UNIT_TEST (callback_called_on_destruction) {
                bool called = false;
                {
                    const SharedContext parent = context::background();
                    const SharedContext ctx = context::withAfterFunc(
                        parent, [&called](const IContext &) noexcept { called = true; });
                }
                PPR_TEST_ASSERT(called);
            };

            PPR_UNIT_TEST (callback_receives_context) {
                bool received_correct_context = false;
                const SharedContext parent = context::background();
                {
                    const SharedContext ctx = context::withAfterFunc(
                        parent, [&received_correct_context](const IContext &c) noexcept {
                            received_correct_context = (&c != nullptr);
                        });
                }
                PPR_TEST_ASSERT(received_correct_context);
            };
        }

        PPR_UNIT_TEST (after_func){
            _.recurse({
                AfterFunc::callback_called_on_destruction,
                AfterFunc::callback_receives_context,
            });


        };

        namespace Value {
            PPR_UNIT_TEST (value_is_retrievable) {
                const SharedContext parent = context::background();
                const SharedContext ctx = context::withValue(parent, "test_key", opaque::Value{42});
                const auto val = ctx->value("test_key");
                PPR_TEST_ASSERT(val.has_value());
            };

            PPR_UNIT_TEST (missing_key_returns_none) {
                const SharedContext parent = context::background();
                const SharedContext ctx = context::withValue(parent, "test_key", opaque::Value{42});
                const auto val = ctx->value("other_key");
                PPR_TEST_ASSERT(!val.has_value());
            };

            PPR_UNIT_TEST (values_from_parent_fallback) {
                const SharedContext parent = context::withValue(
                    context::background(), "parent_key", 99);
                const SharedContext child = context::withValue(
                    parent, "child_key", 42);
                const auto parent_val = child->value("parent_key");
                PPR_TEST_ASSERT(parent_val.has_value());
            };
        }

        PPR_UNIT_TEST (value){
            _.recurse({
                Value::value_is_retrievable,
                Value::missing_key_returns_none,
                Value::values_from_parent_fallback,
            });


        };

        namespace Deadline {

            PPR_UNIT_TEST (deadline_is_set) {
                TimerManager timer{ITimerClock::steady()};
                const TimePoint dl = timer.now() + std::chrono::seconds(60);
                const auto [parent, cancel_] = context::withCancel(context::background());
                const SharedContext ctx = context::withDeadline(parent, dl, timer);
                PPR_TEST_ASSERT(ctx->pollEvent() == false);
                std::ignore = cancel_;
            };

            PPR_UNIT_TEST (parent_cancel_overrides_deadline) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                TimerManager timer{ITimerClock::steady()};
                const TimePoint dl = timer.now() + std::chrono::seconds(60);
                const SharedContext child = context::withDeadline(parent, dl, timer);
                cancel_parent();
                PPR_TEST_ASSERT(child->pollEvent());
            };

            PPR_UNIT_TEST (timeout_sets_deadline) {
                TimerManager timer{ITimerClock::steady()};

                const SharedContext ctx = context::withTimeout(
                    context::background(), std::chrono::milliseconds(150), timer);
                PPR_TEST_ASSERT(ctx->pollEvent() == false);

                while (not ctx->pollEvent()) {
                    std::this_thread::yield();
                    TimeSpan dt{};
                    std::ignore = timer.tick(&dt);
                }
            };
        }

        PPR_UNIT_TEST (deadline){
            _.recurse({
                Deadline::deadline_is_set,
                Deadline::parent_cancel_overrides_deadline,
                Deadline::timeout_sets_deadline,
            });


        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest background = UnitTest::Named("background") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Context::Background::done_is_never_event,
            detail::Context::Background::error_is_none,
            detail::Context::Background::value_is_none,
        });
    };
    extern const UnitTest cancel = UnitTest::Named("cancel") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Context::Cancel::done_initially_empty,
            detail::Context::Cancel::manual_cancel_fires_done,
            detail::Context::Cancel::cancel_is_idempotent,
            detail::Context::Cancel::parent_cancel_propagates,
            detail::Context::Cancel::grandparent_cancel_propagates,
            detail::Context::Cancel::child_cancel_does_not_affect_parent,
        });
    };
    extern const UnitTest cancel_clause = UnitTest::Named("cancel_clause") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Context::CancelClause::clause_fires_done_with_error,
            detail::Context::CancelClause::clause_error_matches,
        });
    };
    extern const UnitTest without_cancel = UnitTest::Named("without_cancel") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Context::WithoutCancel::done_never_fires,
            detail::Context::WithoutCancel::error_always_none,
        });
    };
    extern const UnitTest after_func = UnitTest::Named("after_func") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Context::AfterFunc::callback_called_on_destruction,
            detail::Context::AfterFunc::callback_receives_context,
        });
    };
    // NOTE: identifier is `context_value` (not `value`) because the opaque
    // group in Core.Opaque.Tests.cpp owns `pP::tests::value`; the test NAME
    // stays "value" so the path core/context/value is unchanged.
    extern const UnitTest context_value = UnitTest::Named("value") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Context::Value::value_is_retrievable,
            detail::Context::Value::missing_key_returns_none,
            detail::Context::Value::values_from_parent_fallback,
        });
    };
    extern const UnitTest deadline = UnitTest::Named("deadline") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Context::Deadline::deadline_is_set,
            detail::Context::Deadline::parent_cancel_overrides_deadline,
            detail::Context::Deadline::timeout_sets_deadline,
        });
    };
    extern const UnitTest context = UnitTest::Named("context") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            background,
            cancel,
            cancel_clause,
            without_cancel,
            after_func,
            context_value,
            deadline,
        });
    };
} // namespace pP::tests
