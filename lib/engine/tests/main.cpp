import engine.core;
import engine.tests;

import std;

int main(const int argc, char *argv[]) {
    using namespace pP;

    static bool g_tests_failed = false;
    UnitTest::Context context{};
    context.m_fail_with = [](const UnitTest::IRun &run, const char *failure) {
        g_tests_failed = true;
        std::cerr << "unit-test <" << run.getTestId().path() << "> failed with: " << failure << std::endl;
    };

    unsigned loops = mem::is_asan_enabled_v ? 3u : 1u;
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
            loops = std::stoul(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::println(
                "Usage: EngineTests [--run-test <path>] [--child-run] [--shuffle [<seed>]] [--no-shuffle] [--loop <N>] [--help]\n"
                "\n"
                "  --run-test <path>   Run a specific test (or subtree) by path\n"
                "  --child-run         Mark this process as a child (forked) test runner\n"
                "  --shuffle [<seed>]  Randomize test execution order (default: on, auto-seed)\n"
                "  --no-shuffle        Disable test shuffling (declaration order)\n"
                "  --loop <N>          Run the full test suite N times\n"
                "  -h, --help          Show this help message\n"
                "\n"
                "Test paths use '/' separators, e.g. 'core/memory/asanPoisoning/poison_destroyed_then_read_triggers_asan'.\n");
            return 0;
        }
    }

    if (!shuffle_seen) {
        context.m_shuffle_seed = rng();
    }

    if (context.isChildRun()) {
        hal::disableSystemErrorReporting();
    }

    for (unsigned iter = 0u; iter < loops; ++iter) {
        if (iter > 0 && !explicit_seed && context.m_shuffle_seed.has_value()) {
            context.m_shuffle_seed = rng();
        }
        if (loops > 1 and context.m_shuffle_seed.has_value()) {
            std::cout << "[Running tests][Loop: " << iter << "/" << loops
                    << "][Shuffle: " << std::hex << std::uppercase
                    << std::setw(16) << std::setfill('0')
                    << context.m_shuffle_seed.value_or(0u) << std::dec << "]"
                    << std::endl;
        }
        UnitTest::run(context, tests::core);
    }
    return g_tests_failed ? -1 : 0;
}
