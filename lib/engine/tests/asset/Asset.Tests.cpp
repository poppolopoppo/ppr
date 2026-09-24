module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import std;

namespace pP::tests::detail::SharedGpu {
    [[nodiscard]] std::error_code acquire();

    [[nodiscard]] std::error_code release();
} // namespace pP::tests::detail::SharedGpu

namespace pP::tests {
    const UnitTest &imageTests() noexcept;

    const UnitTest &meshTests() noexcept;

    const UnitTest &renderCachesTests() noexcept;

    const UnitTest &renderGatesTests() noexcept;

    const UnitTest &renderIndirectTests() noexcept;

    const UnitTest &renderQuarantineTests() noexcept;

    const UnitTest &resilienceTests() noexcept;

    const UnitTest &observeTests() noexcept;

    // Shared render scope: the first acquire boots the headless kSharedDomain
    // app once per loop (Asset.GpuFixture.cpp); subgroups hold no refs of
    // their own. Inverse teardown: per-leaf TrianglePass::shutdown, then this
    // release (app teardown + reset), then the quarantine below — which boots
    // only private apps. Lazy by construction: this body only runs when the
    // --run-test filter matches asset/render, so CPU-only filtered runs
    // (decode/convert/resilience/observe) never boot a device here.
    const UnitTest render = UnitTest::Named("render") / [](UnitTest::IRun &_) -> void {
        // Phase 2: shared scope over caches + gates + indirect.
        PPR_TEST_ASSERT(not detail::SharedGpu::acquire());
        _.recurse({
            renderCachesTests(),
            renderGatesTests(),
            renderIndirectTests(),
        });
        PPR_TEST_ASSERT(not detail::SharedGpu::release());
        // Phase 3: quarantine (private app/device only, shared session gone).
        _.recurse({
            renderQuarantineTests(),
        });
    };

    // Defined here rather than as an inline constexpr umbrella in Asset.Tests.cppm:
    // same MSVC C1001 shape as engine.tests.core.
    extern const UnitTest asset = UnitTest::Named("asset") / [](UnitTest::IRun &_) -> void {
        // Phase 1: CPU-only tops hold no SharedGpu refs and never acquire.
        _.recurse({
            imageTests(),
            meshTests(),
            resilienceTests(),
            observeTests(),
        });
        // Phase 2+3: shared-GPU scope, then quarantine (both in render above).
        _.recurse({
            render,
        });
    };
} // namespace pP::tests
