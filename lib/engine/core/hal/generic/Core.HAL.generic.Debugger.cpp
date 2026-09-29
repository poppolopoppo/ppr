module;

#include "pP/Macros.h"

module engine.core;

import :hal;

import std;

namespace pP::hal {
    void outputDebug(const char *ansi_msg) noexcept {
        (void) ansi_msg;
    }

    void outputDebug(const native::char_t *native_msg) noexcept {
        (void) native_msg;
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
        return false;
    }

    void breakpoint() noexcept {
    }

    void breakpointIfDebugging() noexcept {
    }

    void disableSystemErrorReporting() noexcept {
    }

    void installDebugAssertHooks() noexcept {
    }

    // ------------------------------------------------------------------
    // profiling markers (PIX on Windows, no-op elsewhere)
    // ------------------------------------------------------------------

    void profileBegin(const char *name) noexcept {
        (void) name;
    }

    void profileEnd() noexcept {
    }

    ProfileScope::ProfileScope(const char *name) noexcept {
        profileBegin(name);
    }

    ProfileScope::~ProfileScope() noexcept {
        profileEnd();
    }

    // ------------------------------------------------------------------
    // thread names (visible to debuggers)
    // ------------------------------------------------------------------

    [[nodiscard]] ThreadId currentThreadId() noexcept {
        return ThreadId{0u};
    }

    void setThreadName(const std::string_view name) noexcept {
        (void) name;
    }

    [[nodiscard]] std::size_t getThreadName(const ThreadId thread_id, char *out_buffer, const std::size_t capacity) noexcept {
        (void) thread_id;
        (void) out_buffer;
        (void) capacity;
        return 0u;
    }
}
