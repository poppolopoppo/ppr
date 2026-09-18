module;

export module engine.app:input.filtered_analog;

import engine.core;
import engine.math;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // FilteredAnalog — exponentially filtered analog input.
    //
    // Time contract: first-order lag dF/dt = lambda * (R - F) integrated as
    // alpha = 1 - exp(-lambda * dt), where `sensitivity` is the convergence
    // rate lambda in s^-1. Larger sensitivity tracks raw faster; zero freezes
    // the filter; very large values snap to raw. Equal wall time converges
    // equally regardless of frame partitioning (exact for constant raw).
    //
    // Compatibility: replaces the former alpha = pow(dt, 1/sensitivity)
    // contract, which tended to zero for sub-second dt at small sensitivity
    // (e.g. ~1e-12 at 16ms/0.15) and stalled post-priming motion in a
    // frame-rate-dependent way. Direction is preserved (larger = faster, huge
    // = instant), but absolute feel is retuned: previous small values such as
    // 0.15 now behave as a slow 0.15 Hz rate, so camera defaults moved to
    // faster rates (see camera controller).
    // ------------------------------------------------------------------

    template <typename T>
    class FilteredAnalog {
    public:
        using value_type = T;

        explicit FilteredAnalog(T init = T{}, float sensitivity = 2.0f) noexcept;

        [[nodiscard]] T filtered() const noexcept;

        [[nodiscard]] T delta() const noexcept;

        [[nodiscard]] T raw() const noexcept;

        [[nodiscard]] float sensitivity() const noexcept;

        void setSensitivity(float s) noexcept;

        void add(const T &offset) noexcept;

        void addClamp(const T &offset, const T &vmin, const T &vmax) noexcept;

        void setRaw(const T &raw) noexcept;

        void update(TimeSpan dt) noexcept;

        void reset(T init) noexcept;

        void clear() noexcept;

    private:
        T m_raw{};
        T m_delta{};
        std::optional<T> m_filtered{};
        float m_sensitivity{2.0f};
    };

    template<> void FilteredAnalog<Quaternion>::update(TimeSpan dt) noexcept;
    template<> void FilteredAnalog<Quaternion>::addClamp(const Quaternion &offset, const Quaternion &vmin, const Quaternion &vmax) noexcept;

    extern template class FilteredAnalog<float>;
    extern template class FilteredAnalog<float2>;
    extern template class FilteredAnalog<float3>;
    extern template class FilteredAnalog<Quaternion>;
}
