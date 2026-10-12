module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import std;

namespace pP::tests::detail::PerformanceWidgetTests {
    PPR_UNIT_TEST (empty_history_has_zero_statistics) {
        const ui::PerformanceWidget widget;
        PPR_TEST_ASSERT(widget.sampleCount() == 0u);
        for (const auto *statistics: {&widget.frameStatistics(), &widget.renderStatistics()}) {
            PPR_TEST_ASSERT(statistics->m_average == 0.0f);
            PPR_TEST_ASSERT(statistics->m_p50 == 0.0f);
            PPR_TEST_ASSERT(statistics->m_p95 == 0.0f);
            PPR_TEST_ASSERT(statistics->m_p99 == 0.0f);
            PPR_TEST_ASSERT(statistics->m_minimum == 0.0f);
            PPR_TEST_ASSERT(statistics->m_maximum == 0.0f);
            PPR_TEST_ASSERT(statistics->m_over_budget == 0u);
        }
    };

    PPR_UNIT_TEST (gpu_history_is_asynchronous_and_deduplicates_completed_submissions) {
        ui::PerformanceWidget widget;
        widget.observe(16.0f, 3.0f);
        PPR_TEST_ASSERT(widget.gpuSampleCount() == 0u);
        widget.observeGpu(10u, 2.0f);
        widget.observeGpu(10u, 99.0f);
        widget.observeGpu(9u, 99.0f);
        PPR_TEST_ASSERT(widget.gpuSampleCount() == 1u);
        PPR_TEST_ASSERT(widget.gpuStatistics().m_average == 2.0f);
        widget.observeGpu(12u, 4.0f);
        PPR_TEST_ASSERT(widget.gpuSampleCount() == 2u);
        PPR_TEST_ASSERT(widget.gpuStatistics().m_average == 3.0f);
        PPR_TEST_ASSERT(widget.sampleCount() == 1u);
        for (u64 id = 13u; id < 300u; ++id) {
            widget.observeGpu(id, 5.0f);
        }
        PPR_TEST_ASSERT(widget.gpuSampleCount() == ui::PerformanceWidget::kWindow);
        PPR_TEST_ASSERT(widget.gpuStatistics().m_minimum == 5.0f and widget.gpuStatistics().m_maximum == 5.0f);
    };

    PPR_UNIT_TEST (single_sample_and_budget_boundary) {
        ui::PerformanceWidget widget;
        widget.observe(ui::PerformanceWidget::kBudgetMs, 20.0f);
        PPR_TEST_ASSERT(widget.sampleCount() == 1u);
        const auto &frame = widget.frameStatistics();
        PPR_TEST_ASSERT(frame.m_average == ui::PerformanceWidget::kBudgetMs);
        PPR_TEST_ASSERT(
            frame.m_p50 == frame.m_average and frame.m_p95 == frame.m_average and frame.m_p99 == frame.m_average);
        PPR_TEST_ASSERT(frame.m_minimum == frame.m_average and frame.m_maximum == frame.m_average);
        PPR_TEST_ASSERT(frame.m_over_budget == 0u);
        const auto &render = widget.renderStatistics();
        PPR_TEST_ASSERT(render.m_average == 20.0f);
        PPR_TEST_ASSERT(render.m_p50 == 20.0f and render.m_p95 == 20.0f and render.m_p99 == 20.0f);
        PPR_TEST_ASSERT(render.m_minimum == 20.0f and render.m_maximum == 20.0f);
        PPR_TEST_ASSERT(render.m_over_budget == 1u);
    };

    PPR_UNIT_TEST (distinct_channels_report_nearest_rank_percentiles) {
        ui::PerformanceWidget widget;
        for (int index = 1; index <= 16; ++index) {
            widget.observe(static_cast<float>(index), static_cast<float>(101 - index));
        }
        PPR_TEST_ASSERT(widget.sampleCount() == 16u);
        const auto &frame = widget.frameStatistics();
        PPR_TEST_ASSERT(frame.m_average == 8.5f);
        PPR_TEST_ASSERT(frame.m_p50 == 8.0f and frame.m_p95 == 16.0f and frame.m_p99 == 16.0f);
        PPR_TEST_ASSERT(frame.m_minimum == 1.0f and frame.m_maximum == 16.0f);
        PPR_TEST_ASSERT(frame.m_over_budget == 0u);
        const auto &render = widget.renderStatistics();
        PPR_TEST_ASSERT(render.m_average == 92.5f);
        PPR_TEST_ASSERT(render.m_p50 == 92.0f and render.m_p95 == 100.0f and render.m_p99 == 100.0f);
        PPR_TEST_ASSERT(render.m_minimum == 85.0f and render.m_maximum == 100.0f);
        PPR_TEST_ASSERT(render.m_over_budget == 16u);
    };

    PPR_UNIT_TEST (rolling_history_discards_oldest_samples) {
        ui::PerformanceWidget widget;
        for (int index = 1; index <= 256; ++index) {
            widget.observe(static_cast<float>(index), static_cast<float>(index * 2));
        }
        PPR_TEST_ASSERT(widget.sampleCount() == ui::PerformanceWidget::kWindow);
        const auto &frame = widget.frameStatistics();
        PPR_TEST_ASSERT(frame.m_average == 136.5f);
        PPR_TEST_ASSERT(frame.m_p50 == 136.0f and frame.m_p95 == 244.0f and frame.m_p99 == 254.0f);
        PPR_TEST_ASSERT(frame.m_minimum == 17.0f and frame.m_maximum == 256.0f);
        PPR_TEST_ASSERT(frame.m_over_budget == 240u);
        const auto &render = widget.renderStatistics();
        PPR_TEST_ASSERT(render.m_average == 273.0f);
        PPR_TEST_ASSERT(render.m_p50 == 272.0f and render.m_p95 == 488.0f and render.m_p99 == 508.0f);
        PPR_TEST_ASSERT(render.m_minimum == 34.0f and render.m_maximum == 512.0f);
        PPR_TEST_ASSERT(render.m_over_budget == 240u);
    };

    PPR_UNIT_TEST (statistics_refresh_on_sample_cadence) {
        ui::PerformanceWidget widget;
        widget.observe(2.0f, 4.0f);
        for (int index = 0; index < 14; ++index) {
            widget.observe(20.0f, 40.0f);
            PPR_TEST_ASSERT(widget.frameStatistics().m_average == 2.0f);
            PPR_TEST_ASSERT(widget.renderStatistics().m_average == 4.0f);
        }
        PPR_TEST_ASSERT(widget.sampleCount() == 15u);
        widget.observe(30.0f, 60.0f);
        PPR_TEST_ASSERT(widget.sampleCount() == 16u);
        PPR_TEST_ASSERT(widget.frameStatistics().m_average == 19.5f);
        PPR_TEST_ASSERT(widget.renderStatistics().m_average == 39.0f);
    };
}

namespace pP::tests {
    const UnitTest performance_widget = UnitTest::Named("performance_widget") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::PerformanceWidgetTests::empty_history_has_zero_statistics,
            detail::PerformanceWidgetTests::gpu_history_is_asynchronous_and_deduplicates_completed_submissions,
            detail::PerformanceWidgetTests::single_sample_and_budget_boundary,
            detail::PerformanceWidgetTests::distinct_channels_report_nearest_rank_percentiles,
            detail::PerformanceWidgetTests::rolling_history_discards_oldest_samples,
            detail::PerformanceWidgetTests::statistics_refresh_on_sample_cadence,
        });
    };

    const UnitTest &performance_widgetTests() noexcept {
        return performance_widget;
    }
}
