module;
#include "pP/UnitTest.h"

export module engine.tests;

import engine.core;
import std;

export namespace pP::tests {

    struct TestCli {
        UnitTest::Context m_context{};
        unsigned m_loops{mem::is_asan_enabled_v ? 3u : 1u};
    };

    [[nodiscard]] TestCli parseCli(const int argc, char *argv[]);

    [[nodiscard]] int runSuite(TestCli cli, const UnitTest &root);

}
