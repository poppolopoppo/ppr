module engine.tests.sim;

import engine.core;

namespace pP::tests {
    const UnitTest &chunkGridTests() noexcept;

    const UnitTest &tickTests() noexcept;

    const UnitTest &snapshotTests() noexcept;

    const UnitTest &ecsTests() noexcept;

    const UnitTest &physicsTests() noexcept;

    const UnitTest &worldGenTests() noexcept;

    // Defined here rather than as an inline constexpr umbrella in Sim.Tests.cppm:
    // same MSVC C1001 shape as engine.tests.core.
    extern const UnitTest sim = UnitTest::Named("sim") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            chunkGridTests(),
            tickTests(),
            snapshotTests(),
            ecsTests(),
            physicsTests(),
            worldGenTests(),
        });
    };
} // namespace pP::tests
