module;

module engine.app;

import :input.filtered_analog;
import engine.core;
import engine.math;
import std;

namespace pP {
    namespace {
        // Frame-partition-invariant blend factor for the first-order lag
        // dF/dt = lambda * (R - F): alpha = 1 - exp(-lambda * dt).
        // `sensitivity` is the convergence rate lambda in s^-1: larger values
        // track raw faster, zero freezes the filter, and huge values snap.
        // Closed form per step keeps equal wall time invariant to partitioning
        // for constant raw; time-varying raw converges as dt -> 0.
        [[nodiscard]] float filterBlend_(const float sensitivity, TimeSpan dt) noexcept {
            // Hitch guard: a stalled frame (>150ms) advances the filter as a
            // single 150ms step instead of snapping to raw.
            dt = std::min(dt, TimeSpan{std::chrono::milliseconds{150}});
            const double lambda = static_cast<double>(std::max(sensitivity, 0.0f));
            const double alpha = 1.0 - std::exp(-lambda * time::seconds(dt));
            return saturate(static_cast<float>(alpha));
        }
    }

    template<typename T>
    FilteredAnalog<T>::FilteredAnalog(T init, float sensitivity) noexcept
        : m_raw{init}, m_sensitivity{sensitivity} {
    }

    template<typename T>
    T FilteredAnalog<T>::filtered() const noexcept {
        return m_filtered.value_or(m_raw);
    }

    template<typename T>
    T FilteredAnalog<T>::delta() const noexcept { return m_delta; }

    template<typename T>
    T FilteredAnalog<T>::raw() const noexcept { return m_raw; }

    template<typename T>
    float FilteredAnalog<T>::sensitivity() const noexcept { return m_sensitivity; }

    template<typename T>
    void FilteredAnalog<T>::setSensitivity(float s) noexcept { m_sensitivity = s; }

    template<typename T>
    void FilteredAnalog<T>::add(const T &offset) noexcept {
        m_raw = m_raw + offset;
    }

    template<typename T>
    void FilteredAnalog<T>::addClamp(const T &offset, const T &vmin, const T &vmax) noexcept {
        m_raw = clamp(m_raw + offset, vmin, vmax);
    }

    template<typename T>
    void FilteredAnalog<T>::setRaw(const T &raw) noexcept {
        m_raw = raw;
    }

    template<typename T>
    void FilteredAnalog<T>::update(TimeSpan dt) noexcept {
        const float t = filterBlend_(m_sensitivity, dt);
        if (m_filtered.has_value()) {
            const T prev = *m_filtered;
            m_filtered = lerp(*m_filtered, m_raw, t);
            m_delta = *m_filtered - prev;
        } else {
            m_filtered = m_raw;
            m_delta = T{};
        }
    }

    template<typename T>
    void FilteredAnalog<T>::reset(T init) noexcept {
        m_raw = init;
        m_delta = T{};
        m_filtered.reset();
    }

    template<typename T>
    void FilteredAnalog<T>::clear() noexcept {
        reset(T{});
    }

    template<>
    void FilteredAnalog<Quaternion>::update(TimeSpan dt) noexcept {
        const float t = filterBlend_(m_sensitivity, dt);
        if (m_filtered.has_value()) {
            const Quaternion prev = *m_filtered;
            m_filtered = slerp(prev, m_raw, t);
            m_delta = conjugate(prev) * (*m_filtered);
        } else {
            m_filtered = m_raw;
            m_delta = Quaternion{0.0f, 0.0f, 0.0f, 1.0f};
        }
    }

    template<>
    void FilteredAnalog<Quaternion>::addClamp(const Quaternion &offset, const Quaternion &, const Quaternion &) noexcept {
        // Quaternion composition is multiplication, not addition, and component-wise clamp
        // does not preserve unit length — apply the offset as a rotation and re-normalize,
        // falling back to identity if the combined quaternion collapses to zero length.
        const Quaternion combined = m_raw * offset;
        m_raw = dot(combined, combined) > epsilon_v<float>
                    ? normalize(combined)
                    : Quaternion{0.0f, 0.0f, 0.0f, 1.0f};
    }

    template class FilteredAnalog<float>;
    template class FilteredAnalog<float2>;
    template class FilteredAnalog<float3>;
    template class FilteredAnalog<Quaternion>;
}
