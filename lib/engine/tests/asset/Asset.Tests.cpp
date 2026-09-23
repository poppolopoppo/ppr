module engine.tests.asset;

import engine.core;

namespace pP::tests {
    const UnitTest &imageTests() noexcept;

    const UnitTest &mipsChainTests() noexcept;

    const UnitTest &mipsCoverageTests() noexcept;

    const UnitTest &mipsBleedTests() noexcept;

    const UnitTest &mipsQualityTests() noexcept;

    const UnitTest &stagingTests() noexcept;

    const UnitTest &meshTests() noexcept;

    // UV transforms/sets + inverse-transpose normals (one focused group, own TU).
    const UnitTest &uvNormalTests() noexcept;

    const UnitTest &fuzzTests() noexcept;

    const UnitTest &stressTests() noexcept;

    const UnitTest &gpuTests() noexcept;

    const UnitTest &gateTests() noexcept;

    // CPU-staged indirect layouts (stride/buckets/clamp/empty).
    const UnitTest &indirectLayoutTests() noexcept;

    // Parity gate: direct-vs-indirect pixel-exact proof of drawIndirect
    // through the engine.rhi seam.
    const UnitTest &indirectParityTests() noexcept;

    // Compute-publish gate: GPU-written count, compute-path parity, count
    // guards, ring retirement (one focused test, own TU).
    const UnitTest &indirectComputeTests() noexcept;

    // Injection: upload/publish failure points leave no half-published entry;
    // the clamped over-count publish stays drawable and the empty CPU-staged
    // path draws clear-only (one focused test).
    const UnitTest &indirectInjectionTests() noexcept;

    // Device loss: mid-frame loss parks GPU calls with no_such_device at the
    // cache level, and shutdown + initialize restores the pre-loss readback
    // hash (one focused test; Vulkan CI + tier policy lives in the asset codemap).
    const UnitTest &indirectLossTests() noexcept;

    // Defined here rather than as an inline constexpr umbrella in Asset.Tests.cppm:
    // same MSVC C1001 shape as engine.tests.core. New groups append their
    // accessor + recurse entry here without touching the image group.
    extern const UnitTest asset = UnitTest::Named("asset") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            imageTests(),
            mipsChainTests(),
            mipsCoverageTests(),
            mipsBleedTests(),
            mipsQualityTests(),
            stagingTests(),
            meshTests(),
            uvNormalTests(),
            fuzzTests(),
            stressTests(),
            gpuTests(),
            gateTests(),
            indirectLayoutTests(),
            indirectParityTests(),
            indirectComputeTests(),
            indirectInjectionTests(),
            indirectLossTests(),
        });
    };
} // namespace pP::tests
