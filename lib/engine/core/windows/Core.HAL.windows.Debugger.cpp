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

#if PPR_ENABLE_ASSERTIONS
    namespace {
        int __cdecl crtReportHook(int reportType, char *message, int *returnValue) {
            (void)reportType;
            if (returnValue) {
                *returnValue = 0;
            }

            if (message) {
                outputDebug(message);
            }

            throw std::logic_error(message ? message : "CRT assertion failure");
        }

        void __cdecl invalidParamHandler(
            const wchar_t *expr,
            const wchar_t *func,
            const wchar_t *file,
            const unsigned int line,
            uintptr_t
        ) {
            char buf[2048];
            const auto w2a = [](const wchar_t *ws) -> std::string {
                if (!ws) return std::string("(null)");
                return toString<char>(std::wstring_view(ws));
            };
            const auto [end, _] = std::format_to_n(buf, sizeof(buf) - 1,
                "Invalid parameter: {} in {} at {}:{}",
                w2a(expr), w2a(func), w2a(file), line);
            *end = '\0';

            outputDebug(buf);
            throw std::logic_error(buf);
        }

        void __cdecl purecallHandler() {
            constexpr const char *msg = "Pure virtual function call";
            outputDebug(msg);
            throw std::logic_error(msg);
        }

        [[noreturn]] void terminateHandler() noexcept {
            outputDebug("Terminate called, exiting\n");
            breakpointIfDebugging();
            std::_Exit(3);
        }
    }
#endif

    void installDebugAssertHooks() noexcept {
#if PPR_ENABLE_ASSERTIONS
        ::_CrtSetReportHook(&crtReportHook);
        ::_set_invalid_parameter_handler(&invalidParamHandler);
        ::_set_purecall_handler(&purecallHandler);
        std::set_terminate(&terminateHandler);
#endif
    }
}
