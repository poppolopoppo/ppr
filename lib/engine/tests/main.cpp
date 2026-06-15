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

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};

        if (arg == "--run-test" && i + 1 < argc) {
            context.setFilter(argv[++i]);
        } else if (arg == "--child-run") {
            context.markAsChildRun();
        } else if (arg == "--shuffle") {
            unsigned seed = static_cast<unsigned>(std::random_device{}());
            if (i + 1 < argc) {
                const std::string_view next{argv[i + 1]};
                auto [ptr, ec] = std::from_chars(next.data(), next.data() + next.size(), seed);
                if (ec == std::errc{}) {
                    ++i;
                }
            }
            context.m_shuffle_seed = seed;
        } else if (arg == "--help" || arg == "-h") {
            std::println(
                "Usage: EngineTests [--run-test <path>] [--child-run] [--shuffle [<seed>]] [--help]\n"
                "\n"
                "  --run-test <path>   Run a specific test (or subtree) by path\n"
                "  --child-run         Mark this process as a child (forked) test runner\n"
                "  --shuffle [<seed>]  Randomize test execution order (optional seed for reproducibility)\n"
                "  -h, --help          Show this help message\n"
                "\n"
                "Test paths use '/' separators, e.g. 'core/memory/asanPoisoning/poison_destroyed_then_read_triggers_asan'.\n");
            return 0;
        }
    }

    if (context.isChildRun()) {
        hal::disableSystemErrorReporting();
    }

    UnitTest::run(context, tests::core);
    return g_tests_failed ? -1 : 0;
}
