module;

#include "pP/Macros.h"

module engine.core;

import :hal;

import std;

namespace pP::hal {
    void outputDebug(const char *ansi_msg) noexcept {
        (void)ansi_msg;
    }

    void outputDebug(const native::char_t *native_msg) noexcept {
        (void)native_msg;
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
        return false;
    }

    void breakpoint() noexcept {
    }

    void breakpointIfDebugging() noexcept {
    }
}
