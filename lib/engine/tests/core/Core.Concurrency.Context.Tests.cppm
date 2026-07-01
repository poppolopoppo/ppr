module;
#include "pP/Macros.h"
export module engine.tests:core_context;
import engine.core;
import std;

export namespace pP::tests {
    namespace Context {
        namespace Background {
            PPR_UNIT_TEST(done_is_never_event) {
                const SharedContext ctx = context::background();
                PPR_ASSERT(ctx->pollEvent() == false);
            };

            PPR_UNIT_TEST(error_is_none) {
                const SharedContext ctx = context::background();
                PPR_ASSERT(not ctx->error().has_value());
            };

            PPR_UNIT_TEST(value_is_none) {
                const SharedContext ctx = context::background();
                PPR_ASSERT(not ctx->value("key").has_value());
            };
        }

        PPR_UNIT_TEST(background) {
            _.recurse({
                Background::done_is_never_event,
                Background::error_is_none,
                Background::value_is_none,
            });
        };

        namespace Cancel {
            PPR_UNIT_TEST(done_initially_empty) {
                const auto [ctx, cancel] = context::withCancel(context::background());
                PPR_ASSERT(ctx->pollEvent() == false);
            };

            PPR_UNIT_TEST(manual_cancel_fires_done) {
                const auto [ctx, cancel] = context::withCancel(context::background());
                cancel();
                PPR_ASSERT(ctx->pollEvent());
                PPR_ASSERT(ctx->error().has_value());
            };

            PPR_UNIT_TEST(cancel_is_idempotent) {
                const auto [ctx, cancel] = context::withCancel(context::background());
                cancel();
                cancel();
                cancel();
                PPR_ASSERT(ctx->error().has_value());
                PPR_ASSERT(ctx->pollEvent());
            };

            PPR_UNIT_TEST(parent_cancel_propagates) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                const auto [child, cancel_child] = context::withCancel(parent);
                cancel_parent();
                PPR_ASSERT(child->pollEvent());
                PPR_ASSERT(child->error().has_value());
            };

            PPR_UNIT_TEST(grandparent_cancel_propagates) {
                const auto [gp, cancel_gp] = context::withCancel(context::background());
                const auto [parent, cancel_parent] = context::withCancel(gp);
                const auto [child, cancel_child] = context::withCancel(parent);
                cancel_gp();
                PPR_ASSERT(child->pollEvent());
            };

            PPR_UNIT_TEST(child_cancel_does_not_affect_parent) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                const auto [child, cancel_child] = context::withCancel(parent);
                cancel_child();
                PPR_ASSERT(!parent->pollEvent());
                PPR_ASSERT(!parent->error().has_value());
            };
        }

        PPR_UNIT_TEST(cancel) {
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
            PPR_UNIT_TEST(clause_fires_done_with_error) {
                const auto [ctx, cancel_] = context::withCancelClause(context::background());
                const std::error_code err{42, std::generic_category()};
                cancel_(err);
                PPR_ASSERT(ctx->pollEvent());
                PPR_ASSERT(ctx->error().has_value());
            };

            PPR_UNIT_TEST(clause_error_matches) {
                const auto [ctx, cancel_] = context::withCancelClause(context::background());
                const std::error_code err{42, std::generic_category()};
                cancel_(err);
                PPR_ASSERT(ctx->error().has_value());
                PPR_ASSERT(ctx->error()->value() == 42);
            };
        }


        PPR_UNIT_TEST(cancel_clause) {
            _.recurse({
                CancelClause::clause_fires_done_with_error,
                CancelClause::clause_error_matches,
            });
        };

        namespace WithoutCancel {
            PPR_UNIT_TEST(done_never_fires) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                const SharedContext child = context::withoutCancel(parent);
                cancel_parent();
                PPR_ASSERT(not child->pollEvent());
            };

            PPR_UNIT_TEST(error_always_none) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                const SharedContext child = context::withoutCancel(parent);
                cancel_parent();
                PPR_ASSERT(not child->error().has_value());
            };
        }

        PPR_UNIT_TEST(without_cancel) {
            _.recurse({
                WithoutCancel::done_never_fires,
                WithoutCancel::error_always_none,
            });
        };

        namespace AfterFunc {
            PPR_UNIT_TEST(callback_called_on_destruction) {
                bool called = false;
                {
                    const SharedContext parent = context::background();
                    const SharedContext ctx = context::withAfterFunc(
                        parent, [&called](const IContext &) noexcept { called = true; });
                }
                PPR_ASSERT(called);
            };

            PPR_UNIT_TEST(callback_receives_context) {
                bool received_correct_context = false;
                const SharedContext parent = context::background();
                {
                    const SharedContext ctx = context::withAfterFunc(
                        parent, [&received_correct_context](const IContext &c) noexcept {
                            received_correct_context = true;
                            static_cast<void>(c);
                        });
                }
                PPR_ASSERT(received_correct_context);
            };
        }

        PPR_UNIT_TEST(after_func) {
            _.recurse({
                AfterFunc::callback_called_on_destruction,
                AfterFunc::callback_receives_context,
            });
        };

        namespace Value {
            PPR_UNIT_TEST(value_is_retrievable) {
                const SharedContext parent = context::background();
                const SharedContext ctx = context::withValue(parent, "test_key", opaque::Value{42});
                const auto val = ctx->value("test_key");
                PPR_ASSERT(val.has_value());
            };

            PPR_UNIT_TEST(missing_key_returns_none) {
                const SharedContext parent = context::background();
                const SharedContext ctx = context::withValue(parent, "test_key", opaque::Value{42});
                const auto val = ctx->value("other_key");
                PPR_ASSERT(!val.has_value());
            };

            PPR_UNIT_TEST(values_from_parent_fallback) {
                const SharedContext parent = context::withValue(
                    context::background(), "parent_key", 99);
                const SharedContext child = context::withValue(
                    parent, "child_key", 42);
                const auto parent_val = child->value("parent_key");
                PPR_ASSERT(parent_val.has_value());
            };
        }

        PPR_UNIT_TEST(value) {
            _.recurse({
                Value::value_is_retrievable,
                Value::missing_key_returns_none,
                Value::values_from_parent_fallback,
            });
        };

        namespace Deadline {
            PPR_UNIT_TEST(deadline_is_set) {
                const TimePoint dl = TimerManager::mainTimer().now() + std::chrono::seconds(60);
                const auto [parent, cancel_] = context::withCancel(context::background());
                const SharedContext ctx = context::withDeadline(parent, dl);
                PPR_ASSERT(ctx->pollEvent() == false);
                std::ignore = cancel_;
            };

            PPR_UNIT_TEST(parent_cancel_overrides_deadline) {
                const auto [parent, cancel_parent] = context::withCancel(context::background());
                const TimePoint dl = TimerManager::mainTimer().now() + std::chrono::seconds(60);
                const SharedContext child = context::withDeadline(parent, dl);
                cancel_parent();
                PPR_ASSERT(child->pollEvent());
            };

            PPR_UNIT_TEST(timeout_sets_deadline) {
                const SharedContext ctx = context::withTimeout(
                    context::background(), std::chrono::milliseconds(150));
                PPR_ASSERT(ctx->pollEvent() == false);

                TimerManager &timer = TimerManager::mainTimer();

                while (not ctx->pollEvent()) {
                    std::this_thread::yield();
                    timer.tick();
                }
            };
        }

        PPR_UNIT_TEST(deadline) {
            _.recurse({
                Deadline::deadline_is_set,
                Deadline::parent_cancel_overrides_deadline,
                Deadline::timeout_sets_deadline,
            });
        };
    }

    PPR_UNIT_TEST(context) {
        _.recurse({
            Context::background,
            Context::cancel,
            Context::cancel_clause,
            Context::without_cancel,
            Context::after_func,
            Context::value,
            Context::deadline,
        });
    };
}
