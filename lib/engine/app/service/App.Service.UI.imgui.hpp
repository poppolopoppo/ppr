#pragma once

#include <mango/math/math.hpp>

// Single include point for Dear ImGui in the PPR engine.
// Sets up assertion/logging overrides before including <imgui.h>.
// Included in the global module fragment — standard library headers are
// acceptable here (no C5244 warning, no conflict with import std;).

#if not PPR_ENABLE_DEBUG
#   define IMGUI_DISABLE_DEBUG_TOOLS
#elif PPR_ENABLE_SANITIZER_ADDRESS
#   define IMGUI_DEBUG_PARANOID
#endif

namespace pP::ui {
    struct TextureId {
        void *m_rhi_texture_view{nullptr};

        constexpr TextureId() noexcept = default;

        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr TextureId(void *rhi_texture_view) noexcept
            : m_rhi_texture_view(rhi_texture_view) {
        }

        constexpr bool operator ==(const TextureId &) const noexcept = default;
    };
}

#define ImDrawIdx unsigned short
#define ImTextureID pP::ui::TextureId

// Wrap ImGui assertions inside our assert backend
#if PPR_ENABLE_ASSERTIONS
#include <source_location>

namespace pP::ui {
    void imGuiAssertFailure(
        const char *message,
        const std::source_location &location = std::source_location::current() );
}

#define IM_ASSERT(_EXPR)  ((void)(                          \
    (!!(_EXPR)) ||                                          \
    (pP::ui::imGuiAssertFailure(PPR_STRINGIZE(_EXPR)),  0)  \
    ))
#else
#define IM_ASSERT(_EXPR)                                    \
    ((void)0)
#endif

// Wrap ImGui logging inside our logging backend
#if PPR_ENABLE_LOGGING
namespace pP::ui {
    // the only call site is `IMGUI_DEBUG_PRINTF("%s", buffer);`
    void imGuiDebugPrintf(const char *format, const char *buffer);
}

#define IMGUI_DEBUG_PRINTF(_FMT, ...)                       \
    pP::ui::imGuiDebugPrintf(_FMT, __VA_ARGS__)

#else
#define IMGUI_DEBUG_PRINTF(_FMT, ...)                       \
    ((void)0)
#endif

// Define additional constructors and implicit cast operators in imconfig.h to convert back and forth between your math types and ImVec2/4.
// Uses mango types directly — pP::float2/float4 are aliases for these.
#define IM_VEC2_CLASS_EXTRA \
    inline constexpr ImVec2(const mango::math::float32x2& f) noexcept : x(f.x), y(f.y) {} \
    inline constexpr operator mango::math::float32x2 () const noexcept { return mango::math::float32x2(x, y); }

#define IM_VEC4_CLASS_EXTRA \
    inline constexpr ImVec4(const mango::math::float32x4& f) noexcept : x(f.x), y(f.y), z(f.z), w(f.w) {} \
    inline constexpr operator mango::math::float32x4 () const noexcept { return mango::math::float32x4(x, y, z, w); }

#include <imgui.h>
