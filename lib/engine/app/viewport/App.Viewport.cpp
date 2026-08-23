module;
module engine.app;
import :viewport;
import engine.core;
import engine.math;
import engine.rhi;
import std;

namespace pP {
    Viewport::Viewport(const int2 &clientRect, EViewportFlags flags) noexcept
        : m_clientRect(clientRect), m_flags(flags) {}

    void Viewport::setClientRect(const int2 &value) noexcept {
        m_clientRect = value;
    }

    int2 Viewport::screenToClient(const int2 &screenPos) const noexcept {
        return screenPos - m_clientRect;
    }

    int2 Viewport::clientToScreen(const int2 &clientPos) const noexcept {
        return clientPos + m_clientRect;
    }

    float2 Viewport::clientToTexCoord(const int2 &clientPos) const noexcept {
        const float2 size = float2{static_cast<float>(m_clientRect.x), static_cast<float>(m_clientRect.y)};
        return float2{
            size.x > 0.0f ? static_cast<float>(clientPos.x) / size.x : 0.0f,
            size.y > 0.0f ? static_cast<float>(clientPos.y) / size.y : 0.0f
        };
    }

    int2 Viewport::texCoordToClient(const float2 &texCoord) const noexcept {
        return int2{
            static_cast<i32>(texCoord.x * static_cast<float>(m_clientRect.x)),
            static_cast<i32>(texCoord.y * static_cast<float>(m_clientRect.y))
        };
    }
}
