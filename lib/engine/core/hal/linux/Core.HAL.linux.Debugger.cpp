module;

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <csignal>

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

    void outputDebug(const native::char_t *wide_msg) noexcept {
#if PPR_ENABLE_DEBUG
        std::string converted = toString<char>(native::string_view(wide_msg));
        outputDebug(converted.c_str());
#endif
    }

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

    [[nodiscard]] std::size_t getThreadName(const ThreadId thread_id, char *out_buffer, const std::size_t capacity) noexcept {
        const std::string comm_path = "/proc/" + std::to_string(thread_id.m_value) + "/comm";

        const int fd = ::open(comm_path.c_str(), O_RDONLY);
        if (fd < 0) {
            return 0u;
        }
        PPR_DEFER { ::close(fd); };

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
