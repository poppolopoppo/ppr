#pragma once

#include "pP/Macros.h"

// Single include point for Dear ImGui in the PPR engine.
// Sets up assertion/logging overrides before including <imgui.h>.
// Must NOT include any standard library headers (included in purview in .cppm).

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

#include <imgui.h>
