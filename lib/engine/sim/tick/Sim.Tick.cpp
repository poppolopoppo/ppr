module;
#include "pP/Macros.h"

module engine.sim;

import :tick;

import engine.core;

import std;

namespace pP::sim {
    FixedTimestep::FixedTimestep(const TimeSpan period, const u32 maxSlices, const EOverrunPolicy policy)
        : m_period(period), m_max_slices(maxSlices), m_policy(policy) {
        PPR_ASSERT(period > TimeSpan{});
        PPR_ASSERT(maxSlices > 0u);
    }

    double FixedTimestep::alpha() const noexcept {
        if (m_backlog <= TimeSpan{}) {
            return 0.0;
        }

        const double fraction = static_cast<double>(m_backlog.count()) / static_cast<double>(m_period.count());
        return std::clamp(fraction, 0.0, 1.0);
    }

    void FixedTimestep::reset() noexcept {
        m_backlog = TimeSpan{};
    }

    void FixedTimestep::setOverrunHook(OverrunHook hook) {
        m_overrun_hook = std::move(hook);
    }

    Expected<TickReport> FixedTimestep::advance(const TimeSpan elapsed) {
        if (elapsed < TimeSpan{}) [[unlikely]] {
            return std::unexpected{make_error_code(std::errc::invalid_argument)};
        }

        // Whole slices come out of the backlog plus this advance; the remainder is
        // the sub-period fraction that carries into the next advance.
        const TimeSpan total = m_backlog + elapsed;
        const auto whole = total / m_period;
        const auto budget = static_cast<decltype(whole)>(m_max_slices);

        TickReport report{};
        report.m_overran = whole > budget;
        report.m_slices = static_cast<u32>(std::min(whole, budget));

        // Clamp discards everything past the budget; defer keeps it as backlog.
        const bool keep_overrun = report.m_overran and m_policy == EOverrunPolicy::defer;
        m_backlog = keep_overrun
                        ? total - m_period * static_cast<decltype(whole)>(report.m_slices)
                        : total % m_period;
        report.m_backlog = m_backlog;

        if (report.m_overran and m_overrun_hook) {
            m_overrun_hook(report);
        }

        return report;
    }

    Expected<u32> StepRegistry::declare(const std::string_view name) {
        return declare(name, std::span<const std::string_view>{});
    }

    Expected<u32> StepRegistry::declare(const std::string_view name, const std::span<const std::string_view> after) {
        if (name.empty()) [[unlikely]] {
            return std::unexpected{make_error_code(std::errc::invalid_argument)};
        }

        // Assemble and validate the candidate registry first: any rejection below
        // returns before m_steps and m_order are touched.
        Array<Step> steps = m_steps;
        const std::optional<u32> existing = indexOf(steps, name);

        u32 id{};
        if (existing.has_value()) {
            if (steps[*existing].m_declared) [[unlikely]] {
                return std::unexpected{make_error_code(std::errc::invalid_argument)};
            }

            // Completing a placeholder left by an earlier forward reference.
            id = *existing;
            steps[id].m_declared = true;
        } else {
            Step step{};
            step.m_name = std::string{name};
            step.m_declared = true;

            id = safe_narrowing(steps.size());
            steps.push_back(std::move(step));
        }

        for (const std::string_view dependency: after) {
            if (dependency.empty()) [[unlikely]] {
                return std::unexpected{make_error_code(std::errc::invalid_argument)};
            }

            if (dependency == name) [[unlikely]] {
                return std::unexpected{make_error_code(std::errc::resource_deadlock_would_occur)};
            }

            std::optional<u32> dependency_id = indexOf(steps, dependency);
            if (not dependency_id.has_value()) {
                // Forward reference: an undeclared step occupies its id until declared.
                Step pending{};
                pending.m_name = std::string{dependency};

                dependency_id = safe_narrowing(steps.size());
                steps.push_back(std::move(pending));
            }

            steps[id].m_after.push_back(*dependency_id);
        }

        for (Step &step: steps) {
            step.m_before.clear();
        }

        for (u32 step_id = 0u; step_id < steps.size(); ++step_id) {
            for (const u32 dependency: steps[step_id].m_after) {
                steps[dependency].m_before.push_back(step_id);
            }
        }

        Array<u32> order{};
        if (not resolve(steps, order)) [[unlikely]] {
            return std::unexpected{make_error_code(std::errc::resource_deadlock_would_occur)};
        }

        m_systems.resize(steps.size());
        m_steps = std::move(steps);
        m_order = std::move(order);

        return id;
    }

    Expected<void> StepRegistry::addSystem(const u32 step, SystemFn fn) {
        if (step >= m_steps.size() or not m_steps[step].m_declared or not fn or m_systems[step])
        [[unlikely]] {
            return std::unexpected{make_error_code(std::errc::invalid_argument)};
        }

        m_systems[step] = std::move(fn);
        return {};
    }

    std::error_code StepRegistry::runSystems(Registry &registry) {
        for (const u32 step: m_order) {
            if (not m_systems[step]) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }
        }

        for (const u32 step: m_order) {
            m_systems[step](registry);
        }

        return {};
    }

    u32 StepRegistry::size() const noexcept {
        return safe_narrowing(m_steps.size());
    }

    std::optional<u32> StepRegistry::find(const std::string_view name) const noexcept {
        return indexOf(m_steps, name);
    }

    std::string_view StepRegistry::name(const u32 step) const noexcept {
        PPR_ASSERT(step < m_steps.size());

        return m_steps[step].m_name;
    }

    std::span<const u32> StepRegistry::order() const noexcept {
        return m_order;
    }

    std::optional<u32> StepRegistry::indexOf(const Array<Step> &steps, const std::string_view name) noexcept {
        for (u32 id = 0u; id < steps.size(); ++id) {
            if (steps[id].m_name == name) {
                return id;
            }
        }

        return std::nullopt;
    }

    bool StepRegistry::resolve(const Array<Step> &steps, Array<u32> &out) {
        const u32 count = safe_narrowing(steps.size());

        Array<u32> indegree{};
        indegree.reserve(steps.size());
        for (const Step &step: steps) {
            indegree.push_back(safe_narrowing(step.m_after.size()));
        }

        Array<u8> settled(steps.size(), u8{});

        out.clear();
        out.reserve(steps.size());

        u32 settled_count = 0u;
        while (settled_count < count) {
            // Stable Kahn: always take the lowest-id ready step so ties break by
            // declaration order.
            u32 ready = count;
            for (u32 id = 0u; id < count; ++id) {
                if (settled[id] == 0u and indegree[id] == 0u) {
                    ready = id;
                    break;
                }
            }

            if (ready == count) {
                // Everyone left still waits on an unsatisfied dependency: a cycle.
                return false;
            }

            settled[ready] = 1u;
            ++settled_count;

            if (steps[ready].m_declared) {
                out.push_back(ready);
            }

            for (const u32 successor: steps[ready].m_before) {
                if (indegree[successor] > 0u) {
                    --indegree[successor];
                }
            }
        }

        return true;
    }
}
