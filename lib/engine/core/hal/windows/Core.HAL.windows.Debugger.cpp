module;

#include <cstdlib>

#include "Core.HAL.windows.include.hpp"

#include <pix3.h>
#include <crtdbg.h>
#include <errhandlingapi.h>
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

#if PPR_ENABLE_DEBUG
        for (const int channel: {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            ::_CrtSetReportMode(channel, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            ::_CrtSetReportFile(channel, _CRTDBG_FILE_STDERR);
        }
#endif

        // NOTE: SEM_NOGPFAULTERRORBOX is deliberately absent from the mask: it
        // suppresses WER invocation entirely, which would block LocalDumps
        // capture. UI suppression comes from WER_FAULT_REPORTING_NO_UI below.
        ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

        ::WerSetFlags(WER_FAULT_REPORTING_NO_UI);
    }

    [[noreturn]] void fatalExit(const char *reason) noexcept {
        // `reason` is a distinct static literal per call site; it is unused at
        // runtime by design — the distinct return address of each caller
        // (terminate vs invalid-parameter vs purecall vs assert vs
        // unexpected-exception) is what distinguishes the dumps. No logging, no
        // allocation, no locks: this runs inside noexcept terminate and under
        // broken CRT invariants. RaiseFailFastException bypasses every handler
        // and raises a WER-captured fail-fast without unwinding.
        (void) reason;
        ::RaiseFailFastException(nullptr, nullptr, 0);
        // Unreachable: RaiseFailFastException never returns. TerminateProcess
        // is a lock-free backstop that preserves the [[noreturn]] contract.
        ::TerminateProcess(::GetCurrentProcess(), 3);
        std::unreachable();
    }

    namespace {
        // CRT report hook: stderr routing plus dialog suppression. Never shows
        // a message box: it claims the report so no Abort/Retry/Ignore dialog
        // appears, then returns control to the caller whose fatal policy
        // (fail-fast above) produces the WER dump.
        int __cdecl crtReportHook([[maybe_unused]] int reportType, char *message, int *returnValue) {
            if (returnValue) {
                *returnValue = 0; // suppress the default CRT debug-break / dialog
            }

            if (message) {
                outputDebug(message);
            }

            // Break into the debugger if one is attached. If no debugger is
            // attached this is a no-op and the process continues to the caller's
            // fatal path. No exception is thrown so the failure cannot be
            // silently swallowed by caller exception handling.
            breakpointIfDebugging();

            return TRUE; // we have handled the report: no "Abort/Retry/Ignore" box
        }

        void __cdecl invalidParamHandler(
            const wchar_t *expr, const wchar_t *func, const wchar_t *file, const unsigned int line,
            [[maybe_unused]] uintptr_t reserved) {
            // Bounded, allocation-free debug record: fixed buffer with
            // truncated transcoding, no std::string. Informational only —
            // fatalExit takes no pointers into this buffer.
            char reason[256];
            std::size_t length = 0u;
            const auto append_wide = [&](const wchar_t *wide) noexcept {
                if (wide != nullptr and length < sizeof(reason) - 1u) {
                    // transcode reports the full required size when the tail
                    // does not fit (nothing is written then): clamp so length
                    // never overshoots the buffer.
                    const std::size_t rest = sizeof(reason) - 1u - length;
                    const std::size_t written = transcode(std::wstring_view(wide), reason + length, rest);
                    length += written < rest ? written : rest;
                }
            };
            const auto append_ansi = [&](const char *ansi) noexcept {
                if (ansi != nullptr and length < sizeof(reason) - 1u) {
                    const std::size_t rest = sizeof(reason) - 1u - length;
                    const std::size_t count = std::strlen(ansi) < rest ? std::strlen(ansi) : rest;
                    std::memcpy(reason + length, ansi, count);
                    length += count;
                }
            };
            append_ansi("Invalid parameter: ");
            append_wide(expr);
            append_ansi(" in ");
            append_wide(func);
            append_ansi(" at ");
            append_wide(file);
            append_ansi(":");
            {
                char digits[12];
                std::size_t digit_count = 0u;
                unsigned int rest = line;
                do {
                    digits[digit_count++] = static_cast<char>('0' + rest % 10u);
                    rest /= 10u;
                } while (rest > 0u and digit_count < sizeof(digits));
                while (digit_count > 0u and length < sizeof(reason) - 1u) {
                    reason[length++] = digits[--digit_count];
                }
            }
            reason[length] = '\0';

            outputDebug(reason);
            breakpointIfDebugging();

            fatalExit("invalid-parameter");
        }

        void __cdecl purecallHandler() {
            constexpr const char *msg = "Pure virtual function call";

            outputDebug(msg);
            breakpointIfDebugging();

            fatalExit("purecall");
        }

        [[noreturn]] void terminateHandler() noexcept {
            // No outputDebug here: OutputDebugString synchronizes on a system
            // mutex and can block, which is unsafe inside noexcept terminate
            // under broken invariants. The fail-fast below needs no logging —
            // the dump's call stack identifies this path.
            breakpointIfDebugging();

            fatalExit("terminate");
        }
    }

    void installDebugAssertHooks() noexcept {
        // Installed in ALL builds, not only PPR_ENABLE_ASSERTIONS: Release
        // still needs dump-producing fatal paths for terminate,
        // invalid-parameter, and purecall (PPR_ASSERT itself compiles out in
        // Release, but these CRT/terminate paths stay live). The CRT report
        // hook is ungated with them: it is a harmless no-op where debug CRT
        // reporting is compiled out, and keeps stderr routing plus dialog
        // suppression wherever reports can fire. No path shows a message box.
        _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &crtReportHook);
        ::_set_invalid_parameter_handler(&invalidParamHandler);
        ::_set_purecall_handler(&purecallHandler);
        std::set_terminate(&terminateHandler);
    }

    namespace {
        constexpr bool profileStringsEqual_(const char *lhs, const char *rhs) noexcept {
            while (*lhs != '\0' and *rhs != '\0') {
                if (*lhs != *rhs) {
                    return false;
                }
                ++lhs;
                ++rhs;
            }
            return *lhs == *rhs;
        }

        // Reserved-red predicate: only the exact case-sensitive Blocking
        // category takes the red override; every near-miss still hashes.
        constexpr bool profileIsBlocking_(const char *category) noexcept {
            return category != nullptr and profileStringsEqual_(category, profile_blocking_category);
        }

        struct ProfileRgb_ {
            UINT8 m_red;
            UINT8 m_green;
            UINT8 m_blue;
        };

        // Integer HSV-to-RGB with S = 0.65, V = 0.90 over hue 60..300:
        // chroma = 149, match lift = 80 (in 0..255). Pure arithmetic.
        constexpr ProfileRgb_ profileRgbForHue_(const u32 hue) noexcept {
            const u32 sector = hue / 60u;
            const u32 frac = hue % 60u;
            const u32 rise = (149u * frac + 30u) / 60u;
            const u32 fall = (149u * (60u - frac) + 30u) / 60u;

            u32 red = 0u;
            u32 green = 0u;
            u32 blue = 0u;
            switch (sector) {
                case 1u:
                    red = fall;
                    green = 149u;
                    break;
                case 2u:
                    green = 149u;
                    blue = rise;
                    break;
                case 3u:
                    green = fall;
                    blue = 149u;
                    break;
                case 4u:
                    red = rise;
                    blue = 149u;
                    break;
                default:
                    red = 149u;
                    blue = fall;
                    break;
            }
            return ProfileRgb_{
                static_cast<UINT8>(red + 80u), static_cast<UINT8>(green + 80u), static_cast<UINT8>(blue + 80u)};
        }

        // Hue 300 is magenta, never Blocking red (reserved-red exclusion).
        static_assert(profileRgbForHue_(300u).m_red == 229u, "hue 300 must render magenta, not red");
        static_assert(profileRgbForHue_(300u).m_green == 80u, "hue 300 must render magenta, not red");
        static_assert(profileRgbForHue_(300u).m_blue == 229u, "hue 300 must render magenta, not red");
        static_assert(profileIsBlocking_(profile_blocking_category), "exact Blocking category must take red");
        static_assert(not profileIsBlocking_(nullptr), "uncategorized scopes must hash, never take red");
        static_assert(not profileIsBlocking_("blocking"), "Blocking match is case-sensitive");
        static_assert(not profileIsBlocking_("App"), "ordinary segments must hash, never take red");

        // Deterministic segment color: FNV-1a (32-bit, over unsigned bytes)
        // of the explicit category, or of the name segment up to the first
        // '.' when uncategorized. 241 is prime, so the spread multiplier
        // permutes residues without collapsing them; 91 keeps the six live
        // segments (App/Sim/Physics/Shader/RHI/Colony) at least 24 degrees
        // apart. Allocation-free, no caches.
        constexpr u32 kHueSpread_ = 91u;

        constexpr UINT profileColorFor_(const char *name, const char *category) noexcept {
            if (profileIsBlocking_(category)) {
                return PIX_COLOR(255u, 64u, 64u);
            }

            const char *text = category != nullptr ? category : name;
            std::size_t length = 0u;
            while (text[length] != '\0' and (category != nullptr or text[length] != '.')) {
                ++length;
            }

            u32 hash = 2166136261u;
            for (std::size_t i = 0u; i < length; ++i) {
                hash ^= static_cast<u32>(static_cast<unsigned char>(text[i]));
                hash *= 16777619u;
            }

            // Reduce before spreading: the full hash times the spread would
            // overflow u32; residues stay below 240 * 91.
            const u32 hue = 60u + ((hash % 241u) * kHueSpread_) % 241u;
            const ProfileRgb_ rgb = profileRgbForHue_(hue);
            return PIX_COLOR(rgb.m_red, rgb.m_green, rgb.m_blue);
        }
    }

    // ------------------------------------------------------------------
    // profiling markers (PIX on Windows, no-op elsewhere)
    // ------------------------------------------------------------------

    void profileBegin(const char *name, const char *category) noexcept {
        PPR_ASSERT(name != nullptr);
        PPR_ASSERT(name[0] != '\0');
        if (category != nullptr) {
            PPR_ASSERT(category[0] != '\0');
        }
        ::PIXBeginEvent(profileColorFor_(name, category), name);
    }

    void profileEnd() noexcept {
        ::PIXEndEvent();
    }

    ProfileScope::ProfileScope(const char *name, const char *category) noexcept {
        profileBegin(name, category);
    }

    ProfileScope::~ProfileScope() noexcept {
        profileEnd();
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
            ::RaiseException(
                0x406D1388, 0u, sizeof(info) / sizeof(ULONG_PTR), reinterpret_cast<const ULONG_PTR *>(&info));
        }
    }

    void setThreadPriority(EPriority priority) noexcept {
        int level = THREAD_PRIORITY_NORMAL;
        switch (priority) {
            case EPriority::idle: level = THREAD_PRIORITY_IDLE; break;
            case EPriority::low: level = THREAD_PRIORITY_BELOW_NORMAL; break;
            case EPriority::normal: level = THREAD_PRIORITY_NORMAL; break;
            case EPriority::high: level = THREAD_PRIORITY_ABOVE_NORMAL; break;
            case EPriority::realtime: level = THREAD_PRIORITY_TIME_CRITICAL; break;
        }
        std::ignore = ::SetThreadPriority(::GetCurrentThread(), level);
    }

    [[nodiscard]] std::size_t getThreadName(
        const ThreadId thread_id, char *out_buffer, const std::size_t capacity) noexcept {
        using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
        static const auto get_thread_description = reinterpret_cast<GetThreadDescriptionFn>(
            ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));

        if (get_thread_description == nullptr) {
            return 0u;
        }

        const HANDLE thread = ::OpenThread(
            THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(thread_id.m_value));
        if (thread == nullptr) {
            return 0u;
        }
        PPR_DEFER {
            ::CloseHandle(thread);
        };

        PWSTR wide_name = nullptr;
        if (FAILED(get_thread_description(thread, &wide_name))) {
            return 0u;
        }
        PPR_DEFER {
            ::LocalFree(wide_name);
        };

        if (out_buffer == nullptr || capacity == 0u) {
            return transcode(std::wstring_view(wide_name), static_cast<char *>(nullptr), 0u);
        }
        return transcode(std::wstring_view(wide_name), out_buffer, capacity);
    }
}
