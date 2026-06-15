module;

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

    void breakpointIfDebugging() noexcept {
    }
}
