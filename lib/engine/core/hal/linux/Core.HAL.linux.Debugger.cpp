module;

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>

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

    // NOTE: no outputDebug(native::char_t) definition here — on POSIX
    // native::char_t is char, so it would redefine the overload above;
    // native strings route to it directly.

    [[nodiscard]] bool isDebuggerPresent() noexcept {
        return false;
    }

    void breakpoint() noexcept {
#if PPR_ENABLE_DEBUG
        raise(SIGTRAP);
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
    // profiling markers (PIX on Windows, no-op elsewhere)
    // ------------------------------------------------------------------

    void profileBegin(const char *name, const char *category) noexcept {
        (void) name;
        (void) category;
    }

    void profileEnd() noexcept {
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
        return ThreadId{static_cast<u64>(::syscall(SYS_gettid))};
    }

    void setThreadName(const std::string_view name) noexcept {
        char buffer[16]{}; // comm limit: 15 chars + NUL
        std::memcpy(buffer, name.data(), std::min(name.size(), sizeof(buffer) - 1u));
        std::ignore = ::prctl(PR_SET_NAME, buffer, 0u, 0u, 0u);
    }

    void setThreadPriority(EPriority priority) noexcept {
        if (priority == EPriority::realtime) {
            sched_param param{};
            param.sched_priority = ::sched_get_priority_max(SCHED_FIFO);
            if (::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param) == 0) {
                return;
            }
            std::ignore = ::setpriority(PRIO_PROCESS, 0, -20);
            return;
        }

        int nice_value = 0;
        switch (priority) {
            case EPriority::idle: nice_value = 19; break;
            case EPriority::low: nice_value = 10; break;
            case EPriority::normal: nice_value = 0; break;
            case EPriority::high: nice_value = -10; break;
            case EPriority::realtime: nice_value = -20; break;
        }
        std::ignore = ::setpriority(PRIO_PROCESS, 0, nice_value);
    }

    [[nodiscard]] std::size_t getThreadName(
        const ThreadId thread_id, char *out_buffer, const std::size_t capacity) noexcept {
        const std::string comm_path = "/proc/" + std::to_string(thread_id.m_value) + "/comm";

        const int fd = ::open(comm_path.c_str(), O_RDONLY);
        if (fd < 0) {
            return 0u;
        }
        PPR_DEFER {
            ::close(fd);
        };

        char raw[16]{};
        const auto result = ::read(fd, raw, sizeof(raw) - 1u);
        if (result <= 0) {
            return 0u;
        }

        std::size_t length = static_cast<std::size_t>(result);
        if (raw[length - 1u] == '\n') {
            --length;
        }

        if (out_buffer != nullptr && capacity > 0u) {
            std::memcpy(out_buffer, raw, std::min(length, capacity));
        }
        return length;
    }
}
