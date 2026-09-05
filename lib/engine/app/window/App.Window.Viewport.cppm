module;
#include "pP/Macros.h"
export module engine.app:window.viewport;

import :window.handle;

import engine.core;
import engine.math;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // simple rectangle struct to hold both integral and floating point data
    // ------------------------------------------------------------------

    template<math::details::TArithmetic T>
    struct BasicRect {
        using vector_type = math::Vector<T, 2>;

        vector_type m_origin{zero_v};
        vector_type m_extent{zero_v};

        constexpr BasicRect() = default;

        static constexpr BasicRect fromAabb(const vector_type &min_pos, const vector_type &max_pos) noexcept {
            return {min_pos, max_pos - min_pos};
        }

        static constexpr BasicRect fromSize(const vector_type &extent) noexcept {
            return {vector_type(zero_v), extent};
        }

        constexpr BasicRect(const T left, const T top, const T width, const T height) noexcept
            : m_origin(left, top), m_extent(width, height) {
        }

        constexpr BasicRect(const vector_type &origin, const vector_type &extent) noexcept
            : m_origin(origin), m_extent(extent) {
        }

        [[nodiscard]] constexpr T getLeft() const noexcept { return m_origin.x; }
        [[nodiscard]] constexpr T getRight() const noexcept { return m_origin.x + m_extent.x; }
        [[nodiscard]] constexpr T getTop() const noexcept { return m_origin.y; }
        [[nodiscard]] constexpr T getBottom() const noexcept { return m_origin.y + m_extent.y; }
        [[nodiscard]] constexpr T getWidth() const noexcept { return m_extent.x; }
        [[nodiscard]] constexpr T getHeight() const noexcept { return m_extent.y; }

        [[nodiscard]] float getAspectRatio() const noexcept;

        [[nodiscard]] constexpr const vector_type &min() const noexcept { return m_origin; }
        [[nodiscard]] constexpr vector_type max() const noexcept { return m_origin + m_extent; }

        [[nodiscard]] constexpr vector_type getCenter() const noexcept {
            // NOTE: component-wise (see operator==).
            return vector_type{m_origin.x + m_extent.x / 2, m_origin.y + m_extent.y / 2};
        }

        constexpr void setLeft(const T value) noexcept {
            m_origin.x = value;
        }

        constexpr void setRight(const T value) noexcept {
            m_extent.x = value - m_origin.x;
        }

        constexpr void setTop(const T value) noexcept {
            m_origin.y = value;
        }

        constexpr void setBottom(const T value) noexcept {
            m_extent.y = value - m_origin.y;
        }

        constexpr void setWidth(const T value) noexcept {
            m_extent.x = value;
        }

        constexpr void setHeight(const T value) noexcept {
            m_extent.y = value;
        }

        [[nodiscard]] constexpr bool contains(const math::Vector<T, 2> &point) const noexcept {
            return point.x >= m_origin.x && point.y >= m_origin.y && point.x <= m_origin.x + m_extent.x &&
                   point.y <= m_origin.y + m_extent.y;
        }

        [[nodiscard]] constexpr bool contains(const BasicRect &other) const noexcept {
            return other.m_origin.x >= m_origin.x && other.m_origin.y >= m_origin.y &&
                   other.m_origin.x + other.m_extent.x <= m_origin.x + m_extent.x &&
                   other.m_origin.y + other.m_extent.y <= m_origin.y + m_extent.y;
        }

        [[nodiscard]] constexpr float2 normalize(const vector_type &pos) const noexcept {
            const float2 delta = vector_cast<float>(pos - m_origin);
            const float2 extent = vector_cast<float>(m_extent);
#if PPR_ENABLE_ASSERTIONS
            PPR_ASSERT(dot2(extent) > epsilon_v<float>);
#endif
            return delta / extent;
        }

        [[nodiscard]] constexpr float2 normalizeClamp(const vector_type &pos) const noexcept {
            return saturate(normalize(pos));
        }

        [[nodiscard]] constexpr vector_type denormalize(const float2 &uv) const noexcept {
            return vector_cast<T>(uv * vector_cast<float>(m_extent)) + m_origin;
        }

        [[nodiscard]] constexpr vector_type denormalizeClamp(const float2 &uv) const noexcept {
            return denormalize(saturate(uv));
        }

        [[nodiscard]] constexpr bool operator ==(const BasicRect &other) const noexcept {
            return m_origin.x == other.m_origin.x && m_origin.y == other.m_origin.y &&
                   m_extent.x == other.m_extent.x && m_extent.y == other.m_extent.y;
        }
    };

    extern template struct BasicRect<int>;
    extern template struct BasicRect<float>;

    using PixelRect = BasicRect<int>; // screen pixels
    using NormalizedRect = BasicRect<float>; // normalized in window space

    // ------------------------------------------------------------------
    // Viewport layout -> policy when parent window is resized
    // ------------------------------------------------------------------

    struct ViewportLayout {
        struct FullWindow final {
        };

        struct Centered final {
            int2 m_extent{};
        };

        struct WindowRect final : PixelRect {
        };

        struct NormalizedWindowRect final : NormalizedRect {
        };

        std::variant<FullWindow, Centered, WindowRect, NormalizedWindowRect> m_variant{FullWindow{}};

        constexpr ViewportLayout() noexcept = default;

        explicit constexpr ViewportLayout(decltype(m_variant) variant) noexcept
            : m_variant(std::move(variant)) {
        }

        ViewportLayout &operator =(decltype(m_variant) variant) noexcept {
            m_variant = std::move(variant);
            return *this;
        }

        [[nodiscard]] PixelRect clientRect(const PixelRect &window_rect) const noexcept;
    };

    // ------------------------------------------------------------------
    // Viewport -> immutable client & window coordinate transforms
    // ------------------------------------------------------------------

    struct Viewport final {
        PixelRect m_window_rect{};
        PixelRect m_client_rect{};

        Viewport() noexcept = default;

        Viewport(const PixelRect &window_rect, const PixelRect &client_rect) noexcept
            : m_window_rect(window_rect), m_client_rect(client_rect) {
        }

        explicit Viewport(const Window &window, const ViewportLayout &layout = {}) noexcept;

        explicit Viewport(const PixelRect &window_rect, const ViewportLayout &layout = {}) noexcept;

        [[nodiscard]] const PixelRect &getWindowRect() const noexcept { return m_window_rect; }
        [[nodiscard]] const PixelRect &getClientRect() const noexcept { return m_client_rect; }

        [[nodiscard]] NormalizedRect getNormalizedClientRect() const noexcept;

        [[nodiscard]] int2 screenToWindow(const int2 &screen_pos) const noexcept;

        [[nodiscard]] int2 windowToClient(const int2 &window_pos) const noexcept;

        [[nodiscard]] int2 clientToWindow(const int2 &client_pos) const noexcept;

        [[nodiscard]] int2 windowToScreen(const int2 &window_pos) const noexcept;

        [[nodiscard]] int2 screenToClient(const int2 &screen_pos) const noexcept {
            return windowToClient(screenToWindow(screen_pos));
        }

        [[nodiscard]] int2 clientToScreen(const int2 &client_pos) const noexcept {
            return windowToScreen(clientToWindow(client_pos));
        }

        [[nodiscard]] float2 normalizeClient(const int2 &client_pos) const noexcept;

        [[nodiscard]] int2 denormalizeClient(const float2 &client_uv) const noexcept;

        [[nodiscard]] float2 normalizeWindow(const int2 &window_pos) const noexcept;

        [[nodiscard]] int2 denormalizeWindow(const float2 &window_uv) const noexcept;

        [[nodiscard]] float2 screenToClientNormalized(const int2 &screen_pos) const noexcept {
            return normalizeClient(screenToClient(screen_pos));
        }

        [[nodiscard]] int2 clientNormalizedToScreen(const float2 &client_uv) const noexcept {
            return clientToScreen(denormalizeClient(client_uv));
        }

        [[nodiscard]] bool operator==(const Viewport &other) const noexcept {
            return m_window_rect == other.m_window_rect and m_client_rect == other.m_client_rect;
        }
    };

    // ------------------------------------------------------------------
    // WindowViewport -> viewport client associated to a window, handles updates
    // ------------------------------------------------------------------

    class WindowViewport : public safe_object {
    public:
        using ViewportRevision = Numeric<u32, WindowViewport>;

        WindowViewport(SharedWindow window, ViewportLayout layout) noexcept;

        ~WindowViewport() noexcept;

        [[nodiscard]] const Window &getWindow() const noexcept { return *m_window; }
        [[nodiscard]] const ViewportLayout &getLayout() const noexcept { return m_layout; }
        [[nodiscard]] const Viewport &getViewport() const noexcept { return m_viewport; }
        [[nodiscard]] ViewportRevision getViewportRevision() const noexcept { return m_viewport_revision; }

        void setLayout(ViewportLayout layout) noexcept;

        // Owner-driven refresh (post-poll): no window subscription is held, so
        // owners call this after polling window events.
        bool updateFromWindow() noexcept;

    private:
        // Window binding is fixed for life; move (never reassign) the viewport.
        const SharedWindow m_window{};

        Viewport m_viewport{};
        ViewportLayout m_layout{};
        ViewportRevision m_viewport_revision{};
    };
}
