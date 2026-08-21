module;
#include "pP/Macros.h"

module engine.tests;

import std;

namespace pP::tests {
    static std::atomic<bool> g_tests_failed{false};

    TestCli parseCli(const int argc, char *argv[]) {
        UnitTest::Context context{};
        context.m_fail_with = [](const UnitTest::IRun &run, const char *failure) {
            g_tests_failed = true;
            std::cerr << "unit-test <" << run.getTestId().path() << "> failed with: " << failure << std::endl;
        };

        TestCli cli{};
        bool shuffle_seen = false;
        bool explicit_seed = false;
        std::random_device rng{};

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{argv[i]};

            if (arg == "--run-test" && i + 1 < argc) {
                context.setFilter(argv[++i]);
            } else if (arg == "--child-run") {
                context.markAsChildRun();
            } else if (arg == "--shuffle") {
                shuffle_seen = true;
                unsigned seed = rng();
                if (i + 1 < argc) {
                    const std::string_view next{argv[i + 1]};
                    if (auto [ptr, ec] = std::from_chars(next.data(), next.data() + next.size(), seed);
                        ec == std::errc{}) {
                        ++i;
                        explicit_seed = true;
                    }
                }
                context.m_shuffle_seed = seed;
            } else if (arg == "--no-shuffle") {
                shuffle_seen = true;
                context.m_shuffle_seed = std::nullopt;
            } else if (arg == "--loop" && i + 1 < argc) {
                cli.m_loops = std::stoul(argv[++i]);
            } else if (arg == "--help" || arg == "-h") {
                std::println(
                    "Usage: <test> [--run-test <path>] [--child-run] [--shuffle [<seed>]] [--no-shuffle] [--loop <N>] [--help]\n"
                    "\n"
                    "  --run-test <path>   Run a specific test (or subtree) by path\n"
                    "                  If the filter matches no tests, the exit code will be non-zero\n"
                    "  --child-run         Mark this process as a child (forked) test runner\n"
                    "  --shuffle [<seed>]  Randomize test execution order (default: on, auto-seed)\n"
                    "  --no-shuffle        Disable test shuffling (declaration order)\n"
                    "  --loop <N>          Run the full test suite N times\n"
                    "  -h, --help          Show this help message\n");
                cli.m_loops = 0;
            }
        }

        if (!shuffle_seen) {
            context.m_shuffle_seed = rng();
        }

        cli.m_context = std::move(context);
        return cli;
    }

    int runSuite(TestCli cli, const UnitTest &root) {
        if (cli.m_loops == 0) {
            return 0;
        }

        hal::disableSystemErrorReporting();
        hal::installDebugAssertHooks();

        if (cli.m_context.hasFilter()) {
            std::cerr << "[Filter: " << cli.m_context.m_filter_path << "]" << std::endl;
        }

        for (unsigned iter = 0u; iter < cli.m_loops; ++iter) {
            if (iter > 0 && cli.m_context.m_shuffle_seed.has_value()) {
                std::random_device rng{};
                cli.m_context.m_shuffle_seed = rng();
            }
            if (cli.m_loops > 1 && cli.m_context.m_shuffle_seed.has_value()) {
                std::cout << "[Running tests][Loop: " << iter << "/" << cli.m_loops
                            << "][Shuffle: " << std::hex << std::uppercase
                            << std::setw(16) << std::setfill('0')
                            << cli.m_context.m_shuffle_seed.value_or(0u) << std::dec << "]"
                            << std::endl;
            }
            UnitTest::run(cli.m_context, root);
        }

        if (cli.m_context.hasFilter() && cli.m_context.numExecuted() == 0u) {
            std::cerr << "error: filter '" << cli.m_context.m_filter_path << "' matched no tests" << std::endl;
            g_tests_failed = true;
        }

        return g_tests_failed ? -1 : 0;
    }
}
