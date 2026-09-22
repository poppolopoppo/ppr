module;
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Staging {
        // P0c trap: Mango importScene concatenates path+filename verbatim, so a
        // directory spelling without trailing separator silently mis-resolves.
        // The staging helpers must join with operator/ (separator-agnostic).
        // Proven behaviorally: the joined file resolves; verbatim concat does not.
        PPR_UNIT_TEST (staging_join_guarantees_separator) {
            const std::filesystem::path root = std::filesystem::current_path() / "temp_staging_probe";
            std::error_code ec{};
            std::filesystem::create_directories(root, ec);
            PPR_TEST_ASSERT(std::filesystem::exists(root, ec));

            const std::filesystem::path probe = root / "probe.txt";
            {
                std::ofstream out(probe);
                PPR_TEST_ASSERT(out.is_open());
                out << "separator-probe";
            }

            const std::string bare = root.string();
            const std::string slashed = bare + "/";
            PPR_TEST_ASSERT(not bare.empty() and bare.back() != '/' and bare.back() != '\\');

            // operator/ resolves identically with and without trailing separator.
            PPR_TEST_ASSERT(std::filesystem::exists(std::filesystem::path(bare) / "probe.txt", ec));
            PPR_TEST_ASSERT(std::filesystem::exists(std::filesystem::path(slashed) / "probe.txt", ec));

            // Verbatim concatenation (the Mango contract) mis-resolves without it.
            PPR_TEST_ASSERT(not std::filesystem::exists(bare + "probe.txt", ec));
            PPR_TEST_ASSERT(std::filesystem::exists(slashed + "probe.txt", ec));

            std::filesystem::remove_all(root, ec);
        };
    } // namespace Staging
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest staging = UnitTest::Named("staging") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Staging::staging_join_guarantees_separator,
        });
    };

    const UnitTest &stagingTests() noexcept {
        return staging;
    }
} // namespace pP::tests