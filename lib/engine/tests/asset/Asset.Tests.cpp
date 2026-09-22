module engine.tests.asset;

import engine.core;

namespace pP::tests {
    const UnitTest &imageTests() noexcept;

    const UnitTest &stagingTests() noexcept;

    const UnitTest &meshTests() noexcept;

    const UnitTest &fuzzTests() noexcept;

    const UnitTest &stressTests() noexcept;

    const UnitTest &gpuTests() noexcept;

    const UnitTest &gateTests() noexcept;

    // Defined here rather than as an inline constexpr umbrella in Asset.Tests.cppm:
    // same MSVC C1001 shape as engine.tests.core. Lane B appends the mesh
    // accessor + recurse entry here without touching the image group.
    extern const UnitTest asset = UnitTest::Named("asset") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            imageTests(),
            stagingTests(),
            meshTests(),
            fuzzTests(),
            stressTests(),
            gpuTests(),
            gateTests(),
        });
    };
} // namespace pP::tests
