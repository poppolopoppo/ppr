module;
#include "pP/UnitTest.h"

#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32)
#include <cxxabi.h>
#endif

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Fatal {
        // ------------------------------------------------------------------
        // Fail-fast crash-fixture matrix (GLFW-free).
        //
        // Every fatal path must reach the fail-fast endpoint: Windows raises
        // a WER-captured fail-fast via hal::fatalExit (RaiseFailFastException),
        // while linux/darwin/generic reach it via std::abort (SIGABRT core
        // dump). The default assert policy is fatal via fatalExit("assert");
        // the setFailurePolicy override installed by Core.UnitTest.cpp RunImpl
        // is preserved untouched — the assert child below dies through a
        // noexcept boundary so the throwing test policy escalates to
        // terminate instead of being caught.
        //
        // Convention (mirrors App.RunGate.Tests.cpp B1' death tests): each
        // child runs ONLY when armed via PPR_CORE_FATAL_ARMED=<case> (the
        // parent arms it around spawnAndWait). Unarmed execution — including
        // full-suite runs — passes as a harmless no-op so the runner never
        // dies. Each child writes a unique per-case marker file
        // (ppr-core-fatal-<case>.txt, never the gate1 B1' marker file) and
        // arms exactly one death mechanism. Each parent asserts the expected
        // child death portably: fail-fast exits and abort codes differ per
        // platform, so parents assert abnormal death (exit_code != 0) plus
        // the armed marker, documented per case.
        // ------------------------------------------------------------------

        [[nodiscard]] bool isArmed(const char *wanted) noexcept {
#ifdef _WIN32
            char *stored = nullptr;
            const bool armed = ::_dupenv_s(&stored, nullptr, "PPR_CORE_FATAL_ARMED") == 0 and
                               stored != nullptr and
                               wanted != nullptr and
                               std::string_view(stored) == std::string_view(wanted);
            std::free(stored);
            return armed;
#else
            const char *stored = std::getenv("PPR_CORE_FATAL_ARMED");
            if (stored == nullptr or wanted == nullptr) {
                return false;
            }
            return std::string_view(stored) == std::string_view(wanted);
#endif
        }

        void setArmEnv(const char *value) {
#ifdef _WIN32
            const std::string armed = value != nullptr ? value : "0";
            ::_putenv_s("PPR_CORE_FATAL_ARMED", armed.c_str());
#else
            if (value != nullptr) {
                ::setenv("PPR_CORE_FATAL_ARMED", value, 1);
            } else {
                ::unsetenv("PPR_CORE_FATAL_ARMED");
            }
#endif
        }

        [[nodiscard]] std::filesystem::path markerPath(const char *fatal_case) {
            return std::filesystem::temp_directory_path() / ("ppr-core-fatal-" + std::string(fatal_case) + ".txt");
        }

        [[nodiscard]] FILE *openMarkerFile(const std::string &path, const char *mode) noexcept {
#ifdef _WIN32
            FILE *file = nullptr;
            return ::fopen_s(&file, path.c_str(), mode) == 0 ? file : nullptr;
#else
            return std::fopen(path.c_str(), mode);
#endif
        }

        void writeFatalMarker(const char *fatal_case, const char *line) noexcept {
            try {
                const std::string path = markerPath(fatal_case).string();
                FILE *file = openMarkerFile(path, "a");
                if (file != nullptr) {
                    std::fputs(line, file);
                    std::fputs("\n", file);
                    std::fclose(file);
                }
            } catch (...) {
            }
        }

        [[nodiscard]] std::string readFatalMarkers(const char *fatal_case) {
            std::string markers{};
            const std::string path = markerPath(fatal_case).string();
            FILE *file = openMarkerFile(path, "r");
            if (file != nullptr) {
                char line[256];
                while (std::fgets(line, sizeof(line), file) != nullptr) {
                    markers += line;
                }
                std::fclose(file);
            }
            return markers;
        }

        [[nodiscard]] int spawnFatalChild(const char *child_leaf, const char *fatal_case) {
            // Resolve the executable BEFORE arming: the path lookup may throw
            // while spawnAndWait runs, so the armed flag can never leak into a
            // full-suite run on a spawn failure. PPR_DEFER clears the flag even
            // when the spawn itself throws.
            const std::filesystem::path exe_path = hal::process::currentExecutablePath();
            setArmEnv(fatal_case);
            PPR_DEFER {
                setArmEnv(nullptr);
            };
            const std::vector<std::string> child_args{
                "--child-run", "--run-test", std::string{"core/fatal/"} + child_leaf};
            return hal::process::spawnAndWait(exe_path, child_args);
        }

        PPR_PRAGMA_WARNING_PUSH()
        PPR_PRAGMA_WARNING_DISABLE_MSVC(4297)
        PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(-Wexceptions)
        [[noreturn]] void throwInNoexcept_() noexcept {
            throw std::runtime_error{"ppr-core-fatal-terminate"};
        }
#if PPR_ENABLE_ASSERTIONS
        void assertInNoexcept_() noexcept {
            PPR_ASSERT(false);
        }
#endif
        PPR_PRAGMA_WARNING_POP()

        PPR_PRAGMA_WARNING_PUSH()
        PPR_PRAGMA_WARNING_DISABLE_MSVC(4717)
#ifdef __clang__
        // Unbounded recursion is the test mechanism. Spelled directly: the
        // hyphenated name cannot pass through
        // PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG (clang-format splits it and
        // stringizing mangles it).
        _Pragma("clang diagnostic ignored \"-Winfinite-recursion\"")
#endif
        PPR_NO_INLINE void overflowRecurse_(unsigned depth) {
            volatile unsigned char frame[2048]{};
            frame[0] = static_cast<unsigned char>(depth & 0xFFu);
            frame[sizeof(frame) - 1u] = static_cast<unsigned char>((depth + 1u) & 0xFFu);

            // The indirect call hides unbounded recursion; the later volatile read keeps this frame alive.
            void (*volatile recurse)(unsigned) = &overflowRecurse_;
            recurse(depth + 1u);

            (void) frame[0];
        }
        PPR_PRAGMA_WARNING_POP()

        struct TlsBomb {
            ~TlsBomb() noexcept(false) {
#ifdef _WIN32
                // The OS loader swallows exceptions escaping a worker
                // thread's TLS destructors (observed exit 0), so throw
                // would prove nothing here: terminate directly to prove
                // the TLS-dtor → terminate → fatalExit path instead.
                std::terminate();
#else
                throw std::runtime_error{"ppr-core-fatal-tls"};
#endif
            }
        };

        // Purecall path: invoke the currently installed CRT purecall
        // handler. A genuine pure-virtual dispatch through a base under
        // construction cannot link on MSVC (ctor calls devirtualize and
        // need the symbol defined, which would then resolve instead of
        // trapping), and __purecall is not directly linkable — but the
        // handler retrieved here IS the one installDebugAssertHooks
        // installed, so invoking it exercises the handler → fatalExit path
        // exactly (Windows → fatalExit("purecall"); Itanium ABI →
        // __cxa_pure_virtual → abort).

        // --- armed children: exactly one death mechanism each ----------------

        PPR_UNIT_TEST (uncaught_child) {
            if (not isArmed("uncaught")) {
                return;
            }
            writeFatalMarker("uncaught", "uncaught-armed");
            // Uncaught std::exception escaping on a worker thread: the runner
            // only catches exceptions on the test thread, so this reaches
            // std::terminate. Distinct from the noexcept-terminate case below
            // (thread-body escape vs test-thread noexcept boundary).
            std::thread worker([] { throw std::runtime_error{"ppr-core-fatal-uncaught"}; });
            worker.join();
        };

        PPR_UNIT_TEST (terminate_child) {
            if (not isArmed("terminate")) {
                return;
            }
            writeFatalMarker("terminate", "terminate-armed");
            // Throwing through a noexcept boundary on the test thread: the
            // RunImpl throwing assert policy is irrelevant here, terminate
            // runs and the platform handler fail-fasts (Windows) or aborts.
            throwInNoexcept_();
        };

        PPR_UNIT_TEST (tls_child) {
            if (not isArmed("tls")) {
                return;
            }
            writeFatalMarker("tls", "tls-armed");
            // Throwing thread_local destructor at thread exit: unwinding a
            // thread-exit destructor calls terminate. Distinct from both
            // thread-body escape and test-thread noexcept throw.
            std::thread worker([] {
                thread_local TlsBomb bomb{};
                (void) bomb;
            });
            worker.join();
        };

        PPR_UNIT_TEST (abort_child) {
            if (not isArmed("abort")) {
                return;
            }
            writeFatalMarker("abort", "abort-armed");
            std::abort();
        };

        PPR_UNIT_TEST (assert_child) {
            if (not isArmed("assert")) {
                return;
            }
#if PPR_ENABLE_ASSERTIONS
            writeFatalMarker("assert", "assert-armed");
            // Enabled PPR_ASSERT failure through noexcept: the RunImpl
            // throwing policy (setFailurePolicy override, preserved) throws,
            // and noexcept escalates it to terminate → fail-fast endpoint.
            assertInNoexcept_();
#else
            // Assertions compiled out (PPR_ASSERT is assume): no death to
            // prove, stay a harmless no-op so release suites stay green.
            return;
#endif
        };

        PPR_UNIT_TEST (invalid_param_child) {
            if (not isArmed("invalid_param")) {
                return;
            }
            writeFatalMarker("invalid_param", "invalid_param-armed");
#ifdef _WIN32
            // Real invalid-parameter dispatch: the CRT handler installed by
            // installDebugAssertHooks routes here, then fail-fasts with the
            // distinct "invalid-parameter" literal.
            ::_invalid_parameter(L"ppr-core-fatal", L"ppr-core-fatal", L"ppr-core-fatal", 0u, 0u);
#else
            // No CRT invalid-parameter hook off Windows: abort() is the
            // fail-fast equivalent and still dies abnormally (SIGABRT).
            std::abort();
#endif
        };

        PPR_UNIT_TEST (purecall_child) {
            if (not isArmed("purecall")) {
                return;
            }
            writeFatalMarker("purecall", "purecall-armed");
            // Real purecall entry: dispatches to the installed purecall
            // handler (Windows → fatalExit("purecall"); Itanium ABI →
            // __cxa_pure_virtual → abort). No recovery is possible past this
            // line.
#ifdef _WIN32
            const _purecall_handler installed = ::_get_purecall_handler();
            PPR_TEST_ASSERT(installed != nullptr);
            installed();
#else
            __cxxabiv1::__cxa_pure_virtual();
#endif
        };

        PPR_UNIT_TEST (stack_overflow_child) {
            if (not isArmed("stack_overflow")) {
                return;
            }
            writeFatalMarker("stack_overflow", "stack_overflow-armed");
            // Unbounded recursion retains a volatile frame across each indirect
            // call to prevent tail-call optimization and exhaust guard pages. Windows dies via
            // STATUS_STACK_OVERFLOW through WER; POSIX via SIGSEGV.
            overflowRecurse_(0u);
        };

        PPR_UNIT_TEST (fail_fast_child) {
            if (not isArmed("fail_fast")) {
                return;
            }
            writeFatalMarker("fail_fast", "fail_fast-armed");
            // The shared fail-fast endpoint itself, with a per-site distinct
            // literal: RaiseFailFastException on Windows, abort() elsewhere.
            hal::fatalExit("ppr-core-fatal-direct");
        };

        // --- parents: assert the expected child death per case ---------------

        PPR_UNIT_TEST (uncaught_fails_fast) {
            std::error_code ec{};
            std::filesystem::remove(markerPath("uncaught"), ec);
            const int exit_code = spawnFatalChild("uncaught_child", "uncaught");
            // Windows: terminate handler → RaiseFailFastException (0xC0000602
            // family); POSIX: SIGABRT/SIGSEGV signal death. Codes differ, so
            // assert abnormal death portably.
            PPR_TEST_ASSERT(exit_code != 0);
            PPR_TEST_ASSERT(readFatalMarkers("uncaught").find("uncaught-armed") != std::string::npos);
        };

        PPR_UNIT_TEST (terminate_fails_fast) {
            std::error_code ec{};
            std::filesystem::remove(markerPath("terminate"), ec);
            const int exit_code = spawnFatalChild("terminate_child", "terminate");
            // Windows: terminate handler → fail-fast; POSIX: terminate →
            // abort (SIGABRT). Assert abnormal death portably.
            PPR_TEST_ASSERT(exit_code != 0);
            PPR_TEST_ASSERT(readFatalMarkers("terminate").find("terminate-armed") != std::string::npos);
        };

        PPR_UNIT_TEST (tls_fails_fast) {
            std::error_code ec{};
            std::filesystem::remove(markerPath("tls"), ec);
            const int exit_code = spawnFatalChild("tls_child", "tls");
            // Throwing from a thread-exit destructor always terminates:
            // Windows fail-fast, POSIX SIGABRT. Assert abnormal death.
            PPR_TEST_ASSERT(exit_code != 0);
            PPR_TEST_ASSERT(readFatalMarkers("tls").find("tls-armed") != std::string::npos);
        };

        PPR_UNIT_TEST (abort_fails_fast) {
            std::error_code ec{};
            std::filesystem::remove(markerPath("abort"), ec);
            const int exit_code = spawnFatalChild("abort_child", "abort");
            // Windows: CRT abort (exit code 3, dialog suppressed by the report
            // hook); POSIX: SIGABRT. Assert abnormal death portably.
            PPR_TEST_ASSERT(exit_code != 0);
            PPR_TEST_ASSERT(readFatalMarkers("abort").find("abort-armed") != std::string::npos);
        };

        PPR_UNIT_TEST (assert_fails_fast) {
            std::error_code ec{};
            std::filesystem::remove(markerPath("assert"), ec);
            const int exit_code = spawnFatalChild("assert_child", "assert");
#if PPR_ENABLE_ASSERTIONS
            // Enabled PPR_ASSERT via noexcept → terminate → fail-fast
            // (Windows) or SIGABRT (POSIX). Assert abnormal death.
            PPR_TEST_ASSERT(exit_code != 0);
            PPR_TEST_ASSERT(readFatalMarkers("assert").find("assert-armed") != std::string::npos);
#else
            // Assertions compiled out: the child is a no-op and must exit
            // cleanly; proves the fixture is inert without assertions.
            PPR_TEST_ASSERT(exit_code == 0);
#endif
        };

        PPR_UNIT_TEST (invalid_param_fails_fast) {
            std::error_code ec{};
            std::filesystem::remove(markerPath("invalid_param"), ec);
            const int exit_code = spawnFatalChild("invalid_param_child", "invalid_param");
            // Windows: invalid-parameter handler → fail-fast; POSIX: abort()
            // fallback (SIGABRT). Assert abnormal death portably.
            PPR_TEST_ASSERT(exit_code != 0);
            PPR_TEST_ASSERT(readFatalMarkers("invalid_param").find("invalid_param-armed") != std::string::npos);
        };

        PPR_UNIT_TEST (purecall_fails_fast) {
            std::error_code ec{};
            std::filesystem::remove(markerPath("purecall"), ec);
            const int exit_code = spawnFatalChild("purecall_child", "purecall");
            // Windows: purecall handler → fail-fast; Itanium ABI:
            // __cxa_pure_virtual → abort. Assert abnormal death portably.
            PPR_TEST_ASSERT(exit_code != 0);
            PPR_TEST_ASSERT(readFatalMarkers("purecall").find("purecall-armed") != std::string::npos);
        };

        PPR_UNIT_TEST (stack_overflow_fails_fast) {
            std::error_code ec{};
            std::filesystem::remove(markerPath("stack_overflow"), ec);
            const int exit_code = spawnFatalChild("stack_overflow_child", "stack_overflow");
            // Windows: STATUS_STACK_OVERFLOW via WER; POSIX: SIGSEGV from
            // guard-page exhaustion. Assert abnormal death portably.
            PPR_TEST_ASSERT(exit_code != 0);
            PPR_TEST_ASSERT(readFatalMarkers("stack_overflow").find("stack_overflow-armed") != std::string::npos);
        };

        PPR_UNIT_TEST (fail_fast_fails_fast) {
            std::error_code ec{};
            std::filesystem::remove(markerPath("fail_fast"), ec);
            const int exit_code = spawnFatalChild("fail_fast_child", "fail_fast");
            // Direct hal::fatalExit: RaiseFailFastException on Windows
            // (plus TerminateProcess backstop), abort() elsewhere. Assert
            // abnormal death portably.
            PPR_TEST_ASSERT(exit_code != 0);
            PPR_TEST_ASSERT(readFatalMarkers("fail_fast").find("fail_fast-armed") != std::string::npos);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest fatal = UnitTest::Named("fatal") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Fatal::uncaught_child,
            detail::Fatal::terminate_child,
            detail::Fatal::tls_child,
            detail::Fatal::abort_child,
            detail::Fatal::assert_child,
            detail::Fatal::invalid_param_child,
            detail::Fatal::purecall_child,
            detail::Fatal::stack_overflow_child,
            detail::Fatal::fail_fast_child,
            detail::Fatal::uncaught_fails_fast,
            detail::Fatal::terminate_fails_fast,
            detail::Fatal::tls_fails_fast,
            detail::Fatal::abort_fails_fast,
            detail::Fatal::assert_fails_fast,
            detail::Fatal::invalid_param_fails_fast,
            detail::Fatal::purecall_fails_fast,
            detail::Fatal::stack_overflow_fails_fast,
            detail::Fatal::fail_fast_fails_fast,
        });
    };

    const UnitTest &fatalTests() noexcept {
        return fatal;
    }
} // namespace pP::tests
