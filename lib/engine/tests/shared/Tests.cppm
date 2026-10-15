module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

export module engine.tests;

import engine.core;
import std;

export namespace pP::tests {
    /// Test-runner invocation settings. Shuffle defaults to enabled with an
    /// automatically generated seed; explicit `--shuffle [seed]` / `--loop`
    /// forms and option-order precedence are preserved from the legacy
    /// parser. Help is represented explicitly (never overloaded onto
    /// loops == 0); `--loop 0` stays a successful no-op. File/environment
    /// sources are accepted when explicitly supplied but never auto-loaded:
    /// the shared entry passes CLI layers only, keeping runs hermetic.
    /// `--child-run` is CLI-only so a stray environment entry or settings
    /// file can never flip the forked-runner marker.
    struct TestSettings {
        std::string m_filter{};
        bool m_child_run{false};
        std::optional<u32> m_shuffle_seed{};
        bool m_no_shuffle{false};
        u32 m_loops{mem::is_asan_enabled_v ? 3u : 1u};
        u32 m_watchdog_timeout_s{30u};
        u32 m_watchdog_repeat_s{30u};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &TestSettings::m_filter,
                    Metadata{
                        "test.filter", "Run a specific test (or subtree) by path", "Selection", EMutability::startup,
                        ESource::all, false},
                    StringDomain{1024u, true}),
                field(
                    &TestSettings::m_child_run,
                    Metadata{
                        "test.child_run", "Mark this process as a child (forked) test runner", "Selection",
                        EMutability::startup, ESource::cli, false},
                    boolean_domain),
                field(
                    &TestSettings::m_shuffle_seed,
                    Metadata{
                        "test.shuffle_seed", "Explicit shuffle seed (absent means auto-seed)", "Execution",
                        EMutability::startup, ESource::all, false},
                    Range<u32>{0u, (std::numeric_limits<u32>::max)()}),
                field(
                    &TestSettings::m_no_shuffle,
                    Metadata{
                        "test.no_shuffle", "Disable test shuffling (declaration order)", "Execution",
                        EMutability::startup, ESource::all, false},
                    boolean_domain),
                field(
                    &TestSettings::m_loops,
                    Metadata{
                        "test.loops", "Run the full test suite N times (0 is a successful no-op)", "Execution",
                        EMutability::startup, ESource::all, false},
                    Range<u32>{0u, (std::numeric_limits<u32>::max)()}),
                field(
                    &TestSettings::m_watchdog_timeout_s,
                    Metadata{
                        "test.watchdog_timeout", "Report a test stalled longer than N seconds (0 disables)",
                        "Execution", EMutability::startup, ESource::all, false},
                    Range<u32>{0u, (std::numeric_limits<u32>::max)()}),
                field(
                    &TestSettings::m_watchdog_repeat_s,
                    Metadata{
                        "test.watchdog_repeat", "Repeat the stall report every N seconds (0 disables)", "Execution",
                        EMutability::startup, ESource::all, false},
                    Range<u32>{0u, (std::numeric_limits<u32>::max)()}),
            };
        }
    };

    /// Invocation CLI for one test executable. The program name labels the
    /// generated usage line; pass a string literal so the view stays alive.
    [[nodiscard]] config::CliSchema testCliSchema(std::string_view program);

    /// Parsed test invocation: resolved settings plus the raw CLI for
    /// order-sensitive decisions (shuffle/no-shuffle precedence).
    struct ParsedTests {
        config::Resolved<TestSettings> m_settings{};
        config::CliResult m_cli{};
        bool m_help_requested{false};
    };

    /// Pure parse + resolve seam (no randomness: an absent seed means
    /// auto-seed at the runner boundary). Lower layers are optional explicit
    /// file/environment content; the shared entry passes none.
    [[nodiscard]] Expected<ParsedTests> parseTestInvocation(
        std::span<const std::string_view> args, std::string_view program, config::Diagnostics &diagnostics);

    [[nodiscard]] Expected<ParsedTests> parseTestInvocation(
        std::span<const std::string_view> args, std::string_view program, std::span<const config::Layer> lowerLayers,
        config::Diagnostics &diagnostics);

    [[nodiscard]] Expected<ParsedTests> parseTestInvocation(
        int argc, char *const *argv, std::string_view program, config::Diagnostics &diagnostics);

    /// Runner-boundary shuffle decision: a --shuffle mention always wins by
    /// order; a winning --no-shuffle mention applies its resolved boolean
    /// (explicit --no-shuffle=false enables shuffling). Without any CLI
    /// mention, the resolved no-shuffle value applies (covers defaults and
    /// explicitly supplied file layers).
    [[nodiscard]] bool shuffleEnabled(const ParsedTests &parsed) noexcept;

    struct TestCli {
        UnitTest::Context m_context{};
        unsigned m_loops{mem::is_asan_enabled_v ? 3u : 1u};
        bool m_shuffle_enabled{true};
    };

    /// Shared tier entry: parse the invocation, print generated help (help
    /// suppresses execution deterministically), report invalid invocations
    /// with exit code 2, and otherwise run the suite. Test configuration is
    /// hermetic by default: only CLI layers resolve here, never user config
    /// files or the process environment. Child invocations inherit their
    /// resolved execution options through the forked-runner path, not
    /// through ambient configuration.
    [[nodiscard]] int runTests(int argc, char *argv[], const UnitTest &root, std::string_view program);

    [[nodiscard]] int runSuite(TestCli cli, const UnitTest &root);

}
