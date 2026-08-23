module;

export module engine.app:viewport.filtered_analog;

import std;
import engine.core;
import engine.math;

export namespace pP {
    // ------------------------------------------------------------------
    // FilteredAnalog — exponentially filtered analog input
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
