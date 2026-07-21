#pragma once

// Single include point for Dear ImGui in the PPR engine.
// Sets up assertion/logging overrides before including <imgui.h>.
// Must NOT include any standard library headers (included in purview in .cppm).

#if not PPR_ENABLE_DEBUG
#   define IMGUI_DISABLE_DEBUG_TOOLS
#elif PPR_ENABLE_SANITIZER_ADDRESS
#   define IMGUI_DEBUG_PARANOID
#endif

#define ImDrawIdx unsigned short
#define ImTextureID pP::rhi::ComPtr<pP::rhi::ITextureView>

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

// Define additional constructors and implicit cast operators in imconfig.h to convert back and forth between your math types and ImVec2.
#define IM_VEC2_CLASS_EXTRA \
    inline constexpr ImVec2(const pP::float2& f) noexcept : x(f.x), y(f.y) {} \
    inline constexpr operator pP::float2 () const noexcept { return pP::float2(x, y); }

#define IM_VEC4_CLASS_EXTRA \
    inline constexpr ImVec4(const pP::float4& f) noexcept : x(f.x), y(f.y), z(f.z), w(f.w) {} \
    inline constexpr operator pP::float4 () const noexcept { return pP::float4(x, y, z, w); }

#include <imgui.h>
