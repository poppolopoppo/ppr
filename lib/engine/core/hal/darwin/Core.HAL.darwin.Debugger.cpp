module;

#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include "pP/Macros.h"

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    void outputDebug(const char *ansi_msg) noexcept {
#if PPR_ENABLE_DEBUG
        ::write(STDERR_FILENO, ansi_msg, std::strlen(ansi_msg));
#endif
    }

    void outputDebug(const native::char_t *native_msg) noexcept {
#if PPR_ENABLE_DEBUG
        std::string converted = toString<char>(native::string_view(native_msg));
        outputDebug(converted.c_str());
#endif
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
#if PPR_ENABLE_DEBUG
        int mib[4];
        struct kinfo_proc info;
        size_t size = sizeof(info);
        info.kp_proc.p_flag = 0;
        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_PID;
        mib[3] = getpid();
        sysctl(mib, 4, &info, &size, nullptr, 0);
        return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
        return false;
#endif
    }

    void breakpoint() noexcept {
#if PPR_ENABLE_DEBUG
        __builtin_trap();
#endif
    }

    void breakpointIfDebugging() noexcept {
#if PPR_ENABLE_DEBUG
        if (isDebuggerPresent()) {
            breakpoint();
        }
#endif
    }

    void disableSystemErrorReporting() noexcept {
    }

    void installDebugAssertHooks() noexcept {
#if PPR_ENABLE_ASSERTIONS
        std::signal(SIGABRT, [](int) {
            ::write(STDERR_FILENO, "SIGABRT received\n", 17);
            ::_Exit(3);
        });
#endif
    }

    // ------------------------------------------------------------------
    // thread names (visible to debuggers)
    // ------------------------------------------------------------------

    [[nodiscard]] ThreadId currentThreadId() noexcept {
        u64 tid = 0u;
        std::ignore = ::pthread_threadid_np(nullptr, &tid);
        return ThreadId{tid};
    }

    void setThreadName(const std::string_view name) noexcept {
        char buffer[64]{}; // MAXTHREADNAMESIZE: 63 chars + NUL
        std::memcpy(buffer, name.data(), std::min(name.size(), sizeof(buffer) - 1u));
        std::ignore = ::pthread_setname_np(buffer);
    }

    [[nodiscard]] std::size_t getThreadName(const ThreadId thread_id, char *out_buffer, const std::size_t capacity) noexcept {
        const mach_port_t self = ::mach_task_self();
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t thread_count = 0u;

        PPR_PRAGMA_WARNING_PUSH()
        PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(-Wdeprecated-declarations) // task_threads: no public tid→pthread_t alternative
        const kern_return_t result = ::task_threads(self, &threads, &thread_count);
        PPR_PRAGMA_WARNING_POP()
        if (result != KERN_SUCCESS) {
            return 0u;
        }
        PPR_DEFER { ::vm_deallocate(self, reinterpret_cast<vm_address_t>(threads), thread_count * sizeof(mach_port_t)); };

        for (mach_msg_type_number_t i = 0u; i < thread_count; ++i) {
            pthread_t pthread = ::pthread_from_mach_thread_np(threads[i]);

            u64 native_tid = 0u;
            if (::pthread_threadid_np(pthread, &native_tid) != 0 || native_tid != thread_id.m_value) {
                continue;
            }

            char name_buffer[64]{};
            if (::pthread_getname_np(pthread, name_buffer, sizeof(name_buffer)) != 0) {
                return 0u;
            }

            const std::size_t length = std::strlen(name_buffer);
            if (out_buffer != nullptr && capacity > 0u) {
                std::memcpy(out_buffer, name_buffer, std::min(length, capacity));
            }
            return length;
        }

        return 0u;
    }
}
