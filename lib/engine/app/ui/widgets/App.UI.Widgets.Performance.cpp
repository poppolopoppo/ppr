module;
#include "pP/Macros.h"

module engine.app;
import :ui.imgui;
import :ui.widgets.performance;
import engine.core;
import imgui;
import std;

namespace pP::ui {
    namespace {
        // ------------------------------------------------------------------
        // timing summaries and history plots
        // ------------------------------------------------------------------

        TimingStatistics summarize(const std::span<const float> samples) noexcept {
            std::array<float, PerformanceWidget::kWindow> scratch{};
            const auto p50 = percentile_nearest_rank(samples, scratch, 0.50f);
            const auto p95 = percentile_nearest_rank(samples, scratch, 0.95f);
            const auto p99 = percentile_nearest_rank(samples, scratch, 0.99f);
            PPR_ASSERT(p50.has_value() and p95.has_value() and p99.has_value());
            return {
                .m_average = std::accumulate(samples.begin(), samples.end(), 0.0f) / static_cast<float>(samples.size()),
                .m_p50 = *p50,
                .m_p95 = *p95,
                .m_p99 = *p99,
                .m_minimum = scratch.front(),
                .m_maximum = scratch[samples.size() - 1u],
                .m_over_budget = static_cast<u64>(std::ranges::count_if(
                    samples, [](const float value) { return value > PerformanceWidget::kBudgetMs; }))};
        }

        /// One labeled history plot, compact: the label carries the live
        /// average and p95 inline, the caption and the full percentile set live
        /// in a hover tooltip, and the short trace keeps its average and p95
        /// reference lines.
        void drawGraph(
            const char *label, const char *caption, float (*getter)(void *, int), void *data, const int count,
            const TimingStatistics &statistics, const float width) {
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::TextDisabled(
                "avg %.1f · p95 %.1f", static_cast<double>(statistics.m_average),
                static_cast<double>(statistics.m_p95));
            ImGui::SameLine();

            char caption_body[192];
            std::snprintf(
                caption_body, sizeof(caption_body), "%s\navg %.1f · p50 %.1f · p95 %.1f · p99 %.1f · max %.1f ms",
                caption, static_cast<double>(statistics.m_average), static_cast<double>(statistics.m_p50),
                static_cast<double>(statistics.m_p95), static_cast<double>(statistics.m_p99),
                static_cast<double>(statistics.m_maximum));
            ui::helpMarker(caption_body);

            const float ceiling = std::max(1.0f, statistics.m_maximum * 1.1f);
            ImGui::PushID(label);
            ImGui::PushStyleColor(ImGuiCol_PlotLines, theme::kTextSecondary.vec4());

            ImGui::PlotLines("##history", getter, data, count, 0, nullptr, 0.0f, ceiling, ImVec2{width, 44.0f});
            ImGui::PopStyleColor();

            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();

            const auto reference = [&](const float value, const ImVec4 color) {
                const float y = maximum.y - 3.0f - (value / ceiling) * (maximum.y - minimum.y - 6.0f);
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2{minimum.x + 3.0f, y}, ImVec2{maximum.x - 3.0f, y}, ImGui::GetColorU32(color));
            };

            reference(statistics.m_average, theme::kAccent.vec4(0.86f));
            reference(statistics.m_p95, theme::kWarning.vec4(0.86f));

            ImGui::PopID();
        }
    }

    // ------------------------------------------------------------------
    // timing history
    // ------------------------------------------------------------------

    void PerformanceWidget::observe(const float frame_ms, const float render_ms) noexcept {
        PPR_ASSERT(std::isfinite(frame_ms) and frame_ms >= 0.0f and std::isfinite(render_ms) and render_ms >= 0.0f);
        if (m_samples.isFull()) {
            std::ignore = m_samples.popFrontAssumeNotEmpty();
        }
        m_samples.pushBackAssumeNotFull(Sample{frame_ms, render_ms});

        if (m_until_refresh == 0u) {
            refresh_();
            m_until_refresh = 14u;
        } else {
            --m_until_refresh;
        }
    }

    void PerformanceWidget::observeGpu(const u64 submission, const float milliseconds) noexcept {
        PPR_ASSERT(std::isfinite(milliseconds) and milliseconds >= 0.0f);
        if (submission <= m_gpu_submission) {
            return;
        }
        m_gpu_submission = submission;
        if (m_gpu_samples.isFull()) {
            std::ignore = m_gpu_samples.popFrontAssumeNotEmpty();
        }
        m_gpu_samples.pushBackAssumeNotFull(milliseconds);

        std::array<float, kWindow> values{};
        for (std::size_t index = 0u; index < m_gpu_samples.size(); ++index) {
            values[index] = m_gpu_samples[index];
        }

        m_gpu = summarize(std::span{values}.first(m_gpu_samples.size()));
    }

    std::size_t PerformanceWidget::gpuSampleCount() const noexcept {
        return m_gpu_samples.size();
    }
    const TimingStatistics &PerformanceWidget::gpuStatistics() const noexcept {
        return m_gpu;
    }

    void PerformanceWidget::refresh_() noexcept {
        std::array<float, kWindow> frames{};
        std::array<float, kWindow> renders{};
        const std::size_t count = m_samples.size();
        PPR_ASSERT(count > 0u);
        for (std::size_t index = 0; index < count; ++index) {
            frames[index] = m_samples[index].m_frame;
            renders[index] = m_samples[index].m_render;
        }

        m_frame = summarize(std::span{frames}.first(count));
        m_render = summarize(std::span{renders}.first(count));
    }

    std::size_t PerformanceWidget::sampleCount() const noexcept {
        return m_samples.size();
    }
    const TimingStatistics &PerformanceWidget::frameStatistics() const noexcept {
        return m_frame;
    }
    const TimingStatistics &PerformanceWidget::renderStatistics() const noexcept {
        return m_render;
    }

    // ------------------------------------------------------------------
    // performance presentation
    // ------------------------------------------------------------------

    void PerformanceWidget::draw(const float bottom_inset) const {
        const ImGuiIO &io = ImGui::GetIO();
        if (io.DisplaySize.x <= 32.0f or io.DisplaySize.y <= bottom_inset + 32.0f) {
            return;
        }

        ImGui::SetNextWindowPos(
            ImVec2{8.0f, io.DisplaySize.y - bottom_inset - 8.0f}, ImGuiCond_Always, ImVec2{0.0f, 1.0f});
        const float width = std::min(440.0f, io.DisplaySize.x - 32.0f);
        ImGui::SetNextWindowSize(ImVec2{width + ImGui::GetStyle().WindowPadding.x * 2.0f, 0.0f}, ImGuiCond_Always);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                           ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_AlwaysAutoResize;
        const bool visible = ImGui::Begin("##performance_widget", nullptr, flags);
        struct WindowScope {
            ~WindowScope() noexcept {
                ImGui::End();
            }
        } window_scope;
        if (not visible) {
            return;
        }

        drawContents();
    }

    void PerformanceWidget::drawContents() const {
        // The trace owns the full content width instead of a fixed ceiling, so
        // widening the window spreads the same samples across a longer plot
        // rather than adding samples to it.
        const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const int count = checked_cast<int>(m_samples.size());
        // ImGui's getter API borrows mutable void*, but the callback only reads.
        auto *const samples = const_cast<RingBuffer<Sample, kWindow> *>(&m_samples);

        drawGraph(
            "Frame cadence", "Main-loop interval between frames, ms.",
            [](void *data, const int index) {
                return (*static_cast<const RingBuffer<Sample, kWindow> *>(data))[static_cast<std::size_t>(index)]
                    .m_frame;
            },
            samples, count, m_frame, width);

        drawGraph(
            "Render CPU", "Submit work per frame, last completed, ms.",
            [](void *data, const int index) {
                return (*static_cast<const RingBuffer<Sample, kWindow> *>(data))[static_cast<std::size_t>(index)]
                    .m_render;
            },
            samples, count, m_render, width);

        if (m_gpu_samples.isEmpty()) {
            ImGui::TextDisabled("GPU frame: waiting for timestamp samples / unavailable");
            ImGui::Dummy(ImVec2{width, 44.0f});
        } else {
            drawGraph(
                "GPU frame", "Completed command work, ms.",
                [](void *data, const int index) {
                    return (*static_cast<const RingBuffer<float, kWindow> *>(data))[static_cast<std::size_t>(index)];
                },
                const_cast<RingBuffer<float, kWindow> *>(&m_gpu_samples), checked_cast<int>(m_gpu_samples.size()),
                m_gpu, width);
        }
    }
}
