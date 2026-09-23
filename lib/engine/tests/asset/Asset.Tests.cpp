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

    // Phase 7 L1: CPU-staged indirect layouts (stride/buckets/clamp/empty).
    const UnitTest &indirectLayoutTests() noexcept;

    // Phase 7 L1 parity gate (adopted L0 proof): direct-vs-indirect
    // pixel-exact. The L0 hello-indirect + counter spikes are deleted — the
    // parity gate subsumes the drawIndirect proof; the UAV-counter, Append(),
    // and count-buffer shapes belong to L2's compute kernel (carries live in
    // TrianglePass::ensureIndirectScratch_).
    const UnitTest &indirectParityTests() noexcept;

    // Phase 7 L2b compute-publish gate: GPU-written count, compute-path
    // parity, count guards, ring retirement (one focused test, own TU).
    const UnitTest &indirectComputeTests() noexcept;

    // Phase 7 L3 injection: upload/publish failure points leave no
    // half-published entry; the clamped over-count publish stays drawable
    // and the empty L1 staged path draws clear-only (one focused test).
    const UnitTest &indirectInjectionTests() noexcept;

    // Phase 7 L3 device loss: mid-frame loss parks GPU calls with
    // no_such_device at the cache level, and shutdown + initialize restores
    // the pre-loss readback hash (one focused test; carries the Vulkan CI
    // note as a file comment).
    const UnitTest &indirectLossTests() noexcept;

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
            indirectLayoutTests(),
            indirectParityTests(),
            indirectComputeTests(),
            indirectInjectionTests(),
            indirectLossTests(),
        });
    };
} // namespace pP::tests
