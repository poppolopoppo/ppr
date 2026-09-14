module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import std;

namespace pP::tests::detail {
    namespace ZeroVProbe {
        PPR_UNIT_TEST (brace_splat_int2) {
            const int2 v{zero_v};
            PPR_TEST_ASSERT(v.x == 0 && v.y == 0);
        };

        PPR_UNIT_TEST (brace_splat_float2) {
            const float2 v{zero_v};
            PPR_TEST_ASSERT(v.x == 0.0f && v.y == 0.0f);
        };

        PPR_UNIT_TEST (zero_extent_rect_preserves_origin) {
            const PixelRect r{int2{3, 4}, int2{zero_v}};
            PPR_TEST_ASSERT(r.m_origin.x == 3 && r.m_origin.y == 4);
            PPR_TEST_ASSERT(r.m_extent.x == 0 && r.m_extent.y == 0);
            const NormalizedRect f{float2{1.0f, 2.0f}, float2{zero_v}};
            PPR_TEST_ASSERT(f.m_origin.x == 1.0f && f.m_origin.y == 2.0f);
            PPR_TEST_ASSERT(f.m_extent.x == 0.0f && f.m_extent.y == 0.0f);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest zerov_probe = UnitTest::Named("zerov_probe") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::ZeroVProbe::brace_splat_int2,
            detail::ZeroVProbe::brace_splat_float2,
            detail::ZeroVProbe::zero_extent_rect_preserves_origin,
        });
    };
} // namespace pP::tests
