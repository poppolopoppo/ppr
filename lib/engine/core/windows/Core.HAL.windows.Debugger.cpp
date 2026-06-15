module;

#include <cstdlib>

#include "Core.HAL.windows.include.h"

#include <crtdbg.h>
#include <werapi.h>

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    void outputDebug(const char *ansi_msg) noexcept {
#if PPR_ENABLE_DEBUG
        ::OutputDebugStringA(ansi_msg);
#else
        (void) ansi_msg;
#endif
    }

    void outputDebug(const native::char_t *wide_msg) noexcept {
#if PPR_ENABLE_DEBUG
        ::OutputDebugStringW(wide_msg);
#else
        (void) wide_msg;
#endif
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
#if PPR_ENABLE_DEBUG
        return ::IsDebuggerPresent();
#else
        return false;
#endif
    }

    void breakpoint() noexcept {
#if PPR_ENABLE_DEBUG
        __debugbreak();
#endif
    }

    void breakpointIfDebugging() noexcept {
#if PPR_ENABLE_DEBUG
        if (::IsDebuggerPresent())
            __debugbreak();
#endif
    }

    void disableSystemErrorReporting() noexcept {
        ::_set_error_mode(_OUT_TO_STDERR);

        for (const int channel: {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            ::_CrtSetReportMode(channel, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            ::_CrtSetReportFile(channel, _CRTDBG_FILE_STDERR);
        }

        ::SetErrorMode(SEM_NOGPFAULTERRORBOX |
                       SEM_FAILCRITICALERRORS |
                       SEM_NOOPENFILEERRORBOX);

        ::WerSetFlags(WER_FAULT_REPORTING_FLAG_QUEUE);
    }
}
