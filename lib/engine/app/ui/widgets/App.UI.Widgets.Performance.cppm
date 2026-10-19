module;

export module engine.app:ui.widgets.performance;

import engine.core;
import std;

export namespace pP::ui {
    // ------------------------------------------------------------------
    // timing statistics
    // ------------------------------------------------------------------

    struct TimingStatistics {
        float m_average{};
        float m_p50{};
        float m_p95{};
        float m_p99{};
        float m_minimum{};
        float m_maximum{};
        u64 m_over_budget{};
    };

    // ------------------------------------------------------------------
    // performance widget
    // ------------------------------------------------------------------

    /// Client-owned CPU and asynchronously completed GPU timing telemetry. Sampling never drives frame admission.
    class PerformanceWidget {
    public:
        static constexpr std::size_t kWindow = 240u;
        static constexpr float kBudgetMs = 1000.0f / 60.0f;

        void observe(float frame_ms, float render_ms) noexcept;
        void observeGpu(u64 submission, float milliseconds) noexcept;
        void draw(float bottom_inset) const;

        /// Draw the existing histories in the caller's current window; no window ownership or positioning.
        void drawContents() const;

        [[nodiscard]] std::size_t gpuSampleCount() const noexcept;
        [[nodiscard]] const TimingStatistics &gpuStatistics() const noexcept;
        [[nodiscard]] std::size_t sampleCount() const noexcept;
        [[nodiscard]] const TimingStatistics &frameStatistics() const noexcept;
        [[nodiscard]] const TimingStatistics &renderStatistics() const noexcept;

    private:
        struct Sample {
            float m_frame;
            float m_render;
        };
        RingBuffer<Sample, kWindow> m_samples{};
        RingBuffer<float, kWindow> m_gpu_samples{};
        TimingStatistics m_gpu{};
        u64 m_gpu_submission{};
        TimingStatistics m_frame{};
        TimingStatistics m_render{};
        u32 m_until_refresh{};
        void refresh_() noexcept;
    };
}
