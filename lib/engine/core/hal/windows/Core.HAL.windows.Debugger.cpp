module;

#include <cstdlib>

#include "Core.HAL.windows.include.hpp"

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
        ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

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

    // ------------------------------------------------------------------
    // thread names (visible to debuggers)
    // ------------------------------------------------------------------

    [[nodiscard]] ThreadId currentThreadId() noexcept {
        return ThreadId{::GetCurrentThreadId()};
    }

    void setThreadName(const std::string_view name) noexcept {
        using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
        static const auto set_thread_description = reinterpret_cast<SetThreadDescriptionFn>(
            ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));

        if (set_thread_description != nullptr) {
            const std::wstring wide_name = native::from(name);
            std::ignore = set_thread_description(::GetCurrentThread(), wide_name.c_str());
            return;
        }

        char legacy_buffer[128];
        if (::IsDebuggerPresent() && name.size() < std::size(legacy_buffer)) {
            std::memcpy(legacy_buffer, name.data(), name.size());
            legacy_buffer[name.size()] = '\0';

            struct ThreadNameInfo {
                DWORD m_type{0x1000};
                LPCSTR m_name;
                DWORD m_thread_id{DWORD(-1)};
                DWORD m_flags{0};
            };
            const ThreadNameInfo info{.m_name = legacy_buffer};
            ::RaiseException(0x406D1388, 0u, sizeof(info) / sizeof(ULONG_PTR), reinterpret_cast<const ULONG_PTR *>(&info));
        }
    }

    [[nodiscard]] std::size_t getThreadName(const ThreadId thread_id, char *out_buffer, const std::size_t capacity) noexcept {
        using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
        static const auto get_thread_description = reinterpret_cast<GetThreadDescriptionFn>(
            ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));

        if (get_thread_description == nullptr) {
            return 0u;
        }

        const HANDLE thread = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(thread_id.m_value));
        if (thread == nullptr) {
            return 0u;
        }
        PPR_DEFER { ::CloseHandle(thread); };

        PWSTR wide_name = nullptr;
        if (FAILED(get_thread_description(thread, &wide_name))) {
            return 0u;
        }
        PPR_DEFER { ::LocalFree(wide_name); };

        if (out_buffer == nullptr || capacity == 0u) {
            return transcode(std::wstring_view(wide_name), static_cast<char *>(nullptr), 0u);
        }
        return transcode(std::wstring_view(wide_name), out_buffer, capacity);
    }
}
