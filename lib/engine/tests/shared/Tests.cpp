module;
#include "pP/Macros.h"
#include <cstdio>

module engine.tests;

import engine.core;

import std;

namespace pP::tests {
    static std::atomic<bool> g_tests_failed{false};

    // ------------------------------------------------------------------
    // cli schema
    // ------------------------------------------------------------------

    [[nodiscard]] config::CliSchema testCliSchema(const std::string_view program) {
        config::CliSchema schema{};
        schema.m_program = program;
        schema.m_description = "Test runner invocation.";

        config::OptionDecl filter{};
        filter.m_canonical = "test.filter";
        filter.m_description = "Run a specific test (or subtree) by path. "
                               "If the filter matches no tests, the exit code will be non-zero.";
        filter.m_aliases.push_back("run-test");
        schema.m_options.push_back(std::move(filter));

        config::OptionDecl child{};
        child.m_canonical = "test.child_run";
        child.m_description = "Mark this process as a child (forked) test runner.";
        child.m_aliases.push_back("child-run");
        child.m_is_switch = true;
        schema.m_options.push_back(std::move(child));

        config::OptionDecl shuffle{};
        shuffle.m_canonical = "test.shuffle_seed";
        shuffle.m_description = "Randomize test execution order (default: on, auto-seed).";
        shuffle.m_aliases.push_back("shuffle");
        shuffle.m_value_optional = true;
        shuffle.m_absent_value = "";
        schema.m_options.push_back(std::move(shuffle));

        config::OptionDecl no_shuffle{};
        no_shuffle.m_canonical = "test.no_shuffle";
        no_shuffle.m_description = "Disable test shuffling (declaration order).";
        no_shuffle.m_aliases.push_back("no-shuffle");
        no_shuffle.m_is_switch = true;
        schema.m_options.push_back(std::move(no_shuffle));

        config::OptionDecl loops{};
        loops.m_canonical = "test.loops";
        loops.m_description = "Run the full test suite N times.";
        loops.m_aliases.push_back("loop");
        schema.m_options.push_back(std::move(loops));

        config::OptionDecl watchdog_timeout{};
        watchdog_timeout.m_canonical = "test.watchdog_timeout";
        watchdog_timeout.m_description = "Report a test stalled longer than N seconds (0 disables).";
        watchdog_timeout.m_aliases.push_back("watchdog-timeout");
        schema.m_options.push_back(std::move(watchdog_timeout));

        config::OptionDecl watchdog_repeat{};
        watchdog_repeat.m_canonical = "test.watchdog_repeat";
        watchdog_repeat.m_description = "Repeat the stall report every N seconds (0 disables).";
        watchdog_repeat.m_aliases.push_back("watchdog-repeat");
        schema.m_options.push_back(std::move(watchdog_repeat));

        return schema;
    }

    // ------------------------------------------------------------------
    // invocation parsing
    // ------------------------------------------------------------------
    [[nodiscard]] Expected<ParsedTests> parseTestInvocation(
        const std::span<const std::string_view> args, const std::string_view program,
        const std::span<const config::Layer> lowerLayers, config::Diagnostics &diagnostics) {
        const config::CliSchema schema = testCliSchema(program);
        auto parsed = config::parseCliArgs(args, schema, diagnostics);
        if (not parsed) {
            return std::unexpected{parsed.error()};
        }
        if (auto valid = config::validateSchema<TestSettings>(diagnostics); not valid) {
            return std::unexpected{valid.error()};
        }

        if (parsed->m_help_requested) {
            ParsedTests help{};
            help.m_cli = std::move(*parsed);
            help.m_help_requested = true;
            return help;
        }

        config::CliResult cli_copy{};
        cli_copy.m_assignments = parsed->m_assignments;
        cli_copy.m_command = parsed->m_command;
        cli_copy.m_help_requested = parsed->m_help_requested;

        Array<config::Layer> layers{};
        for (const auto &layer: lowerLayers) {
            layers.push_back(layer);
        }
        layers.push_back(config::toLayer(std::move(*parsed), "cli"));

        auto resolved = config::resolve<TestSettings>(
            std::span<const config::Layer>{layers.data(), layers.size()}, diagnostics);
        if (not resolved) {
            return std::unexpected{resolved.error()};
        }

        ParsedTests invocation{};
        invocation.m_settings = std::move(*resolved);
        invocation.m_cli = std::move(cli_copy);

        return invocation;
    }

    [[nodiscard]] Expected<ParsedTests> parseTestInvocation(
        const std::span<const std::string_view> args, const std::string_view program,
        config::Diagnostics &diagnostics) {
        return parseTestInvocation(args, program, std::span<const config::Layer>{}, diagnostics);
    }

    [[nodiscard]] Expected<ParsedTests> parseTestInvocation(
        const int argc, char *const *argv, const std::string_view program, config::Diagnostics &diagnostics) {
        Array<std::string_view> args{};
        for (int index = 1; index < argc; ++index) {
            args.emplace_back(argv[index] != nullptr ? argv[index] : "");
        }

        return parseTestInvocation(std::span<const std::string_view>{args.data(), args.size()}, program, diagnostics);
    }

    // ------------------------------------------------------------------
    // shuffle
    // ------------------------------------------------------------------
    [[nodiscard]] bool shuffleEnabled(const ParsedTests &parsed) noexcept {
        const config::Assignment *last = nullptr;
        for (const auto &assignment: parsed.m_cli.m_assignments) {
            if (assignment.m_name == "test.shuffle_seed" or assignment.m_name == "test.no_shuffle") {
                last = std::addressof(assignment);
            }
        }

        // Order-sensitive precedence: an explicit --shuffle mention always
        // enables shuffling, while the winning --no-shuffle mention applies
        // its resolved boolean (so --no-shuffle=false enables shuffling).
        // With no CLI mention the resolved value covers explicitly supplied
        // file layers and defaults.
        if (last != nullptr and last->m_name == "test.shuffle_seed") {
            return true;
        }
        return not parsed.m_settings.m_value.m_no_shuffle;
    }

    // ------------------------------------------------------------------
    // runner
    // ------------------------------------------------------------------
    namespace {
        /// Runner-boundary construction from a parsed invocation. Seed
        /// generation stays here (and in runSuite for auto-seed); parsing
        /// itself never touches random_device. Shuffle/no-shuffle conflicts
        /// resolve by CLI order: the last mention wins, matching the legacy
        /// option-order precedence. Without any CLI mention, an explicitly
        /// supplied no-shuffle value (file layer) still disables shuffling.
        [[nodiscard]] TestCli makeTestCli(const ParsedTests &parsed) {
            TestCli cli{};

            const TestSettings &settings = parsed.m_settings.m_value;
            cli.m_loops = settings.m_loops;
            cli.m_context.m_fail_with = [](const UnitTest::IRun &run, const char *failure,
                                            const std::source_location &site) {
                g_tests_failed = true;
                std::cerr << site.file_name() << "(" << site.line() << "): "
                          << "unit-test <" << run.getTestId().path() << "> failed with: " << failure << std::endl;
            };
            if (not settings.m_filter.empty()) {
                cli.m_context.setFilter(settings.m_filter);
            }
            if (settings.m_child_run) {
                cli.m_context.markAsChildRun();
            }

            cli.m_context.m_shuffle_seed = settings.m_shuffle_seed;
            cli.m_shuffle_enabled = shuffleEnabled(parsed);
            if (not cli.m_shuffle_enabled) {
                cli.m_context.m_shuffle_seed = std::nullopt;
            }

            cli.m_context.m_watchdog_timeout = std::chrono::seconds(settings.m_watchdog_timeout_s);
            cli.m_context.m_watchdog_repeat = std::chrono::seconds(settings.m_watchdog_repeat_s);

            return cli;
        }
    } // namespace

    int runSuite(TestCli cli, const UnitTest &root) {
        if (cli.m_loops == 0) {
            return 0;
        }

        if (cli.m_shuffle_enabled and not cli.m_context.m_shuffle_seed.has_value()) {
            std::random_device rng{};
            cli.m_context.m_shuffle_seed = rng();
        }

        if (not cli.m_shuffle_enabled) {
            cli.m_context.m_shuffle_seed = std::nullopt;
        }

        if (cli.m_context.hasFilter()) {
            std::cerr << "[Filter: " << cli.m_context.m_filter_path << "]" << std::endl;
        }

        for (unsigned iter = 0u; iter < cli.m_loops; ++iter) {
            if (iter > 0 and cli.m_context.m_shuffle_seed.has_value()) {
                std::random_device rng{};
                cli.m_context.m_shuffle_seed = rng();
            }
            if (cli.m_loops > 1 and cli.m_context.m_shuffle_seed.has_value()) {
                std::cout << "[Running tests][Loop: " << iter << "/" << cli.m_loops << "][Shuffle: " << std::hex
                          << std::uppercase << std::setw(16) << std::setfill('0')
                          << cli.m_context.m_shuffle_seed.value_or(0u) << std::dec << "]" << std::endl;
            }
            UnitTest::run(cli.m_context, root);
        }

        if (cli.m_context.hasFilter() and cli.m_context.numExecuted() == 0u) {
            std::cerr << "error: filter '" << cli.m_context.m_filter_path << "' matched no tests" << std::endl;
            g_tests_failed = true;
        }

        return g_tests_failed ? -1 : 0;
    }

    [[nodiscard]] int runTests(const int argc, char *argv[], const UnitTest &root, const std::string_view program) {
        [[maybe_unused]] const hal::process::Context process_context{};

        config::Diagnostics diagnostics{};
        auto parsed = parseTestInvocation(argc, argv, program, diagnostics);
        if (not parsed) {
            std::println(stderr, "{}", diagnostics.format());
            return config::kExitInvalidUsage;
        }

        if (parsed->m_help_requested) {
            std::println("{}", config::generateHelp(testCliSchema(program), parsed->m_settings.m_value));
            return 0;
        }

        return runSuite(makeTestCli(*parsed), root);
    }
}
