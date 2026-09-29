module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.sim;

import std;

namespace pP::tests::detail {
    namespace TickSuite {
        using namespace pP::sim;

        constexpr TimeSpan kPeriod = std::chrono::milliseconds{10};

        PPR_UNIT_TEST (accumulator_consumes_whole_slices) {
            FixedTimestep timestep{kPeriod, 4u};

            PPR_TEST_ASSERT(timestep.period() == kPeriod);
            PPR_TEST_ASSERT(timestep.maxSlices() == 4u);
            PPR_TEST_ASSERT(timestep.overrunPolicy() == EOverrunPolicy::clamp);
            PPR_TEST_ASSERT(timestep.alpha() == 0.0);

            const Expected<TickReport> idle = timestep.advance(TimeSpan{});
            PPR_TEST_ASSERT(idle.has_value());
            PPR_TEST_ASSERT(idle->m_slices == 0u);
            PPR_TEST_ASSERT(not idle->m_overran);
            PPR_TEST_ASSERT(idle->m_backlog == TimeSpan{});
            PPR_TEST_ASSERT(timestep.alpha() == 0.0);

            const Expected<TickReport> partial = timestep.advance(std::chrono::milliseconds{5});
            PPR_TEST_ASSERT(partial.has_value());
            PPR_TEST_ASSERT(partial->m_slices == 0u);
            PPR_TEST_ASSERT(timestep.alpha() == 0.5);

            const Expected<TickReport> full = timestep.advance(std::chrono::milliseconds{5});
            PPR_TEST_ASSERT(full.has_value());
            PPR_TEST_ASSERT(full->m_slices == 1u);
            PPR_TEST_ASSERT(not full->m_overran);
            PPR_TEST_ASSERT(full->m_backlog == TimeSpan{});
            PPR_TEST_ASSERT(timestep.alpha() == 0.0);
        };

        PPR_UNIT_TEST (overrun_clamp_drops_excess) {
            FixedTimestep timestep{kPeriod, 4u, EOverrunPolicy::clamp};

            const Expected<TickReport> report = timestep.advance(std::chrono::milliseconds{55});
            PPR_TEST_ASSERT(report.has_value());
            PPR_TEST_ASSERT(report->m_overran);
            PPR_TEST_ASSERT(report->m_slices == 4u);
            PPR_TEST_ASSERT(report->m_backlog == std::chrono::milliseconds{5});
            PPR_TEST_ASSERT(timestep.alpha() == 0.5);
        };

        PPR_UNIT_TEST (overrun_defer_keeps_backlog) {
            FixedTimestep timestep{kPeriod, 4u, EOverrunPolicy::defer};

            const Expected<TickReport> report = timestep.advance(std::chrono::milliseconds{55});
            PPR_TEST_ASSERT(report.has_value());
            PPR_TEST_ASSERT(report->m_overran);
            PPR_TEST_ASSERT(report->m_slices == 4u);
            PPR_TEST_ASSERT(report->m_backlog == std::chrono::milliseconds{15});
            PPR_TEST_ASSERT(timestep.alpha() == 1.0);

            // The deferred slice is consumable by later advances.
            const Expected<TickReport> catch_up = timestep.advance(TimeSpan{});
            PPR_TEST_ASSERT(catch_up.has_value());
            PPR_TEST_ASSERT(catch_up->m_slices == 1u);
            PPR_TEST_ASSERT(not catch_up->m_overran);
        };

        PPR_UNIT_TEST (rejects_negative_elapsed) {
            FixedTimestep timestep{kPeriod, 4u};

            const Expected<TickReport> report = timestep.advance(TimeSpan{-1});
            PPR_TEST_ASSERT(not report.has_value());
            PPR_TEST_ASSERT(report.error() == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(timestep.alpha() == 0.0);
        };

        PPR_UNIT_TEST (overrun_hook_observes_reports) {
            FixedTimestep timestep{kPeriod, 2u};
            u32 invocations{};
            bool observed_overrun = false;

            timestep.setOverrunHook([&](const TickReport &report) {
                ++invocations;
                observed_overrun = report.m_overran;
            });

            PPR_TEST_ASSERT(timestep.advance(std::chrono::milliseconds{5}).has_value());
            PPR_TEST_ASSERT(invocations == 0u);

            PPR_TEST_ASSERT(timestep.advance(std::chrono::milliseconds{30}).has_value());
            PPR_TEST_ASSERT(invocations == 1u);
            PPR_TEST_ASSERT(observed_overrun);

            timestep.reset();
            PPR_TEST_ASSERT(timestep.alpha() == 0.0);
        };

        PPR_UNIT_TEST (declares_steps_in_order) {
            StepRegistry registry{};

            const Expected<u32> integrate = registry.declare("integrate");
            const Expected<u32> collide = registry.declare("collide");
            PPR_TEST_ASSERT(integrate.has_value());
            PPR_TEST_ASSERT(collide.has_value());
            PPR_TEST_ASSERT(*integrate == 0u);
            PPR_TEST_ASSERT(*collide == 1u);

            PPR_TEST_ASSERT(registry.size() == 2u);
            PPR_TEST_ASSERT(registry.order().size() == 2u);
            PPR_TEST_ASSERT(registry.order()[0] == 0u);
            PPR_TEST_ASSERT(registry.order()[1] == 1u);

            PPR_TEST_ASSERT(registry.find("collide") == 1u);
            PPR_TEST_ASSERT(registry.find("missing") == std::nullopt);
            PPR_TEST_ASSERT(registry.name(1u) == "collide");
        };

        PPR_UNIT_TEST (constraints_reorder_steps) {
            StepRegistry registry{};
            const std::array<std::string_view, 1u> after_emit{"integrate"};
            const std::array<std::string_view, 1u> after_integrate{"render"};

            // `emit` declares first but must run last: it waits on `integrate`,
            // which waits on `render` — a step that does not exist yet.
            PPR_TEST_ASSERT(registry.declare("emit", after_emit).has_value());
            PPR_TEST_ASSERT(registry.declare("render").has_value());
            PPR_TEST_ASSERT(registry.declare("integrate", after_integrate).has_value());

            PPR_TEST_ASSERT(registry.size() == 3u);
            PPR_TEST_ASSERT(registry.order().size() == 3u);
            PPR_TEST_ASSERT(registry.name(registry.order()[0]) == "render");
            PPR_TEST_ASSERT(registry.name(registry.order()[1]) == "integrate");
            PPR_TEST_ASSERT(registry.name(registry.order()[2]) == "emit");
        };

        PPR_UNIT_TEST (forward_references_resolve_before_dependencies) {
            StepRegistry registry{};
            const std::array<std::string_view, 1u> after_integrate{"integrate"};

            const Expected<u32> collide = registry.declare("collide", after_integrate);
            PPR_TEST_ASSERT(collide.has_value());
            PPR_TEST_ASSERT(*collide == 0u);
            PPR_TEST_ASSERT(registry.size() == 2u);
            PPR_TEST_ASSERT(registry.order().size() == 1u);
            PPR_TEST_ASSERT(registry.order()[0] == 0u);

            const Expected<u32> integrate = registry.declare("integrate");
            PPR_TEST_ASSERT(integrate.has_value());
            PPR_TEST_ASSERT(*integrate == 1u);

            PPR_TEST_ASSERT(registry.order().size() == 2u);
            PPR_TEST_ASSERT(registry.order()[0] == 1u);
            PPR_TEST_ASSERT(registry.order()[1] == 0u);
        };

        PPR_UNIT_TEST (systems_run_in_declared_dependency_order) {
            StepRegistry scheduler{};
            Registry entities{};
            std::vector<u32> ran{};
            Entity created{};
            const std::array<std::string_view, 1u> after_integrate{"integrate"};
            const std::array<std::string_view, 1u> after_prepare{"prepare"};

            const Expected<u32> emit = scheduler.declare("emit", after_integrate);
            const Expected<u32> integrate = scheduler.declare("integrate", after_prepare);
            const Expected<u32> prepare = scheduler.declare("prepare");
            PPR_TEST_ASSERT(emit.has_value());
            PPR_TEST_ASSERT(integrate.has_value());
            PPR_TEST_ASSERT(prepare.has_value());

            PPR_TEST_ASSERT(scheduler.addSystem(*emit, [&](Registry &registry) {
                PPR_TEST_ASSERT(&registry == &entities);
                ran.push_back(*emit);
            }).has_value());
            PPR_TEST_ASSERT(scheduler.addSystem(*integrate, [&](Registry &registry) {
                PPR_TEST_ASSERT(&registry == &entities);
                PPR_TEST_ASSERT(registry.isAlive(created));
                ran.push_back(*integrate);
            }).has_value());
            PPR_TEST_ASSERT(scheduler.addSystem(*prepare, [&](Registry &registry) {
                PPR_TEST_ASSERT(&registry == &entities);
                created = registry.create();
                ran.push_back(*prepare);
            }).has_value());

            PPR_TEST_ASSERT(not scheduler.runSystems(entities));
            PPR_TEST_ASSERT(ran.size() == scheduler.order().size());
            PPR_TEST_ASSERT(std::ranges::equal(ran, scheduler.order()));
            PPR_TEST_ASSERT(scheduler.name(ran[0]) == "prepare");
            PPR_TEST_ASSERT(scheduler.name(ran[1]) == "integrate");
            PPR_TEST_ASSERT(scheduler.name(ran[2]) == "emit");
        };

        PPR_UNIT_TEST (unbound_system_fails_before_any_system_runs) {
            StepRegistry scheduler{};
            Registry entities{};
            u32 invocations{};

            const Expected<u32> first = scheduler.declare("first");
            const Expected<u32> unbound = scheduler.declare("unbound");
            PPR_TEST_ASSERT(first.has_value());
            PPR_TEST_ASSERT(unbound.has_value());
            PPR_TEST_ASSERT(scheduler.addSystem(*first, [&](Registry &) { ++invocations; }).has_value());

            PPR_TEST_ASSERT(scheduler.runSystems(entities) == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(invocations == 0u);

            const Expected<void> duplicate = scheduler.addSystem(*first, [&](Registry &) { ++invocations; });
            const Expected<void> unknown = scheduler.addSystem(scheduler.size(), [&](Registry &) { ++invocations; });
            const Expected<void> empty = scheduler.addSystem(*unbound, SystemFn{});
            const std::array<std::string_view, 1u> after_pending{"pending"};
            PPR_TEST_ASSERT(scheduler.declare("later", after_pending).has_value());
            const std::optional<u32> pending = scheduler.find("pending");
            PPR_TEST_ASSERT(pending.has_value());
            const Expected<void> placeholder = scheduler.addSystem(*pending, [&](Registry &) { ++invocations; });
            PPR_TEST_ASSERT(not duplicate.has_value());
            PPR_TEST_ASSERT(not unknown.has_value());
            PPR_TEST_ASSERT(not empty.has_value());
            PPR_TEST_ASSERT(not placeholder.has_value());
            PPR_TEST_ASSERT(duplicate.error() == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(unknown.error() == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(empty.error() == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(placeholder.error() == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(invocations == 0u);
        };

        PPR_UNIT_TEST (rejects_empty_and_duplicate_names) {
            StepRegistry registry{};
            PPR_TEST_ASSERT(registry.declare("step").has_value());

            const Expected<u32> empty = registry.declare("");
            PPR_TEST_ASSERT(not empty.has_value());
            PPR_TEST_ASSERT(empty.error() == std::make_error_code(std::errc::invalid_argument));

            const Expected<u32> duplicate = registry.declare("step");
            PPR_TEST_ASSERT(not duplicate.has_value());
            PPR_TEST_ASSERT(duplicate.error() == std::make_error_code(std::errc::invalid_argument));

            PPR_TEST_ASSERT(registry.size() == 1u);
            PPR_TEST_ASSERT(registry.order().size() == 1u);
        };

        PPR_UNIT_TEST (rejects_self_reference_and_cycles) {
            StepRegistry registry{};

            const std::array<std::string_view, 1u> after_self{"self"};
            const Expected<u32> self = registry.declare("self", after_self);
            PPR_TEST_ASSERT(not self.has_value());
            PPR_TEST_ASSERT(self.error() == std::make_error_code(std::errc::resource_deadlock_would_occur));
            PPR_TEST_ASSERT(registry.size() == 0u);
            PPR_TEST_ASSERT(registry.order().empty());

            // Back-edge through a forward reference: x waits on y, y waits on x.
            const std::array<std::string_view, 1u> after_y{"y"};
            const std::array<std::string_view, 1u> after_x{"x"};
            PPR_TEST_ASSERT(registry.declare("x", after_y).has_value());

            const Expected<u32> cycle = registry.declare("y", after_x);
            PPR_TEST_ASSERT(not cycle.has_value());
            PPR_TEST_ASSERT(cycle.error() == std::make_error_code(std::errc::resource_deadlock_would_occur));

            // Fail-closed: x stays declared, y stays an unscheduled placeholder.
            PPR_TEST_ASSERT(registry.size() == 2u);
            PPR_TEST_ASSERT(registry.order().size() == 1u);
            PPR_TEST_ASSERT(registry.order()[0] == 0u);
            PPR_TEST_ASSERT(registry.name(0u) == "x");
        };
    } // namespace TickSuite
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest tick = UnitTest::Named("tick") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::TickSuite::accumulator_consumes_whole_slices,
            detail::TickSuite::overrun_clamp_drops_excess,
            detail::TickSuite::overrun_defer_keeps_backlog,
            detail::TickSuite::rejects_negative_elapsed,
            detail::TickSuite::overrun_hook_observes_reports,
            detail::TickSuite::declares_steps_in_order,
            detail::TickSuite::constraints_reorder_steps,
            detail::TickSuite::forward_references_resolve_before_dependencies,
            detail::TickSuite::systems_run_in_declared_dependency_order,
            detail::TickSuite::unbound_system_fails_before_any_system_runs,
            detail::TickSuite::rejects_empty_and_duplicate_names,
            detail::TickSuite::rejects_self_reference_and_cycles,
        });
    };

    const UnitTest &tickTests() noexcept {
        return tick;
    }
} // namespace pP::tests
