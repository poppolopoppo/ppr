module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.math;
import engine.app;
import engine.rhi;
import engine.mesh;
import std;

// Indirect layouts (CPU-only): InstancePayload stride/pad and the
// planIndirectDraws contract (per-(prim,instance) records, u32 count header,
// maxCount clamp, count-0 skip, ≤4 variant buckets). One focused test per TU.
namespace pP::tests::detail {
    namespace IndirectLayout {
        PPR_UNIT_TEST (indirect_payload_and_plan_contract) {
            // Stride: 96 B = 64 model + 20 scalars + 12 pad, 16-aligned.
            PPR_TEST_ASSERT(sizeof(TrianglePass::InstancePayload) == 96u);
            PPR_TEST_ASSERT(alignof(TrianglePass::InstancePayload) == 16u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::InstancePayload, m_model) == 0u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::InstancePayload, m_vb_offset) == 64u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::InstancePayload, m_material) == 76u);
            PPR_TEST_ASSERT(std::is_trivially_copyable_v<TrianglePass::InstancePayload>);
            PPR_TEST_ASSERT(std::is_standard_layout_v<TrianglePass::InstancePayload>);
            // Seam records: 16 B D3D12 ExecuteIndirect draw records.
            PPR_TEST_ASSERT(sizeof(rhi::IndirectDrawArguments) == 16u);
            PPR_TEST_ASSERT(sizeof(rhi::IndirectDrawIndexedArguments) == 20u);
            PPR_TEST_ASSERT(sizeof(rhi::IndirectDispatchArguments) == 12u);

            const TrianglePipelineVariant opaque{};
            const TrianglePipelineVariant masked{.m_twosided = false, .m_alpha = mesh::AlphaMode::mask};
            const TrianglePipelineVariant sided{.m_twosided = true, .m_alpha = mesh::AlphaMode::opaque};

            // Empty plan draws nothing (count-0 skips draw at the plan level).
            {
                const Expected<TrianglePass::IndirectPlan> plan = TrianglePass::planIndirectDraws(
                    std::span<const TrianglePipelineVariant>{},
                    std::span<const BagBucketId>{},
                    std::span<const u32>{},
                    16u);
                PPR_TEST_ASSERT(plan.has_value());
                PPR_TEST_ASSERT(plan->m_total_count == 0u);
                PPR_TEST_ASSERT(plan->m_args.empty());
                PPR_TEST_ASSERT(plan->m_buckets.empty());
            }

            // Buckets: mixed variants group into one contiguous range each;
            // count-0 prims emit no record; payload indices ride
            // startInstanceLocation in emission order.
            {
                const TrianglePipelineVariant variants[] = {opaque, masked, opaque, sided, masked};
                const BagBucketId bags[] = {
                    BagBucketId{0u}, BagBucketId{0u}, BagBucketId{0u}, BagBucketId{0u}, BagBucketId{0u}
                };
                const u32 counts[] = {36u, 0u, 12u, 24u, 6u};
                const Expected<TrianglePass::IndirectPlan> plan = TrianglePass::planIndirectDraws(
                    variants, bags, counts, 16u);
                PPR_TEST_ASSERT(plan.has_value());
                PPR_TEST_ASSERT(plan->m_total_count == 4u);
                PPR_TEST_ASSERT(plan->m_args.size() == 4u);
                PPR_TEST_ASSERT(plan->m_buckets.size() == 3u);
                // Opaque bucket: records 0-1 (36 + 12 verts), masked skipped
                // the count-0 prim and holds record 3, sided holds record 2.
                PPR_TEST_ASSERT(plan->m_args[0].vertexCountPerInstance == 36u);
                PPR_TEST_ASSERT(plan->m_args[0].instanceCount == 1u);
                PPR_TEST_ASSERT(plan->m_args[0].startInstanceLocation == 0u);
                PPR_TEST_ASSERT(plan->m_args[1].vertexCountPerInstance == 12u);
                PPR_TEST_ASSERT(plan->m_args[1].startInstanceLocation == 1u);
                PPR_TEST_ASSERT(plan->m_args[2].vertexCountPerInstance == 24u);
                PPR_TEST_ASSERT(plan->m_args[3].vertexCountPerInstance == 6u);
                PPR_TEST_ASSERT(plan->m_buckets[0].m_arg_count == 2u);
                PPR_TEST_ASSERT(plan->m_buckets[0].m_first_arg == 0u);
            }

            // Interleaved bag buckets group into contiguous slices: args
            // reorder by (variant, bucket) while startInstanceLocation keeps
            // the payload index, so no bucket's drawIndirect range leaks a
            // foreign record.
            {
                const TrianglePipelineVariant variants[] = {opaque, opaque, opaque};
                const BagBucketId bags[] = {BagBucketId{0u}, BagBucketId{1u}, BagBucketId{0u}};
                const u32 counts[] = {3u, 4u, 5u};
                const Expected<TrianglePass::IndirectPlan> plan = TrianglePass::planIndirectDraws(
                    variants, bags, counts, 16u);
                PPR_TEST_ASSERT(plan.has_value());
                PPR_TEST_ASSERT(plan->m_total_count == 3u);
                PPR_TEST_ASSERT(plan->m_buckets.size() == 2u);
                PPR_TEST_ASSERT(plan->m_buckets[0].m_first_arg == 0u);
                PPR_TEST_ASSERT(plan->m_buckets[0].m_arg_count == 2u);
                PPR_TEST_ASSERT(plan->m_buckets[1].m_first_arg == 2u);
                PPR_TEST_ASSERT(plan->m_buckets[1].m_arg_count == 1u);
                PPR_TEST_ASSERT(plan->m_args[0].vertexCountPerInstance == 3u);
                PPR_TEST_ASSERT(plan->m_args[0].startInstanceLocation == 0u);
                PPR_TEST_ASSERT(plan->m_args[1].vertexCountPerInstance == 5u);
                PPR_TEST_ASSERT(plan->m_args[1].startInstanceLocation == 2u);
                PPR_TEST_ASSERT(plan->m_args[2].vertexCountPerInstance == 4u);
                PPR_TEST_ASSERT(plan->m_args[2].startInstanceLocation == 1u);
            }

            // Clamp: maxCount clamps to the payload capacity (first-N wins).
            {
                const TrianglePipelineVariant variants[] = {opaque, opaque, opaque, opaque, opaque};
                const BagBucketId bags[] = {
                    BagBucketId{0u}, BagBucketId{0u}, BagBucketId{0u}, BagBucketId{0u}, BagBucketId{0u}
                };
                const u32 counts[] = {3u, 3u, 3u, 3u, 3u};
                const Expected<TrianglePass::IndirectPlan> plan = TrianglePass::planIndirectDraws(
                    variants, bags, counts, 2u);
                PPR_TEST_ASSERT(plan.has_value());
                PPR_TEST_ASSERT(plan->m_total_count == 2u);
                PPR_TEST_ASSERT(plan->m_args.size() == 2u);
                PPR_TEST_ASSERT(plan->m_buckets.size() == 1u);
                PPR_TEST_ASSERT(plan->m_buckets[0].m_arg_count == 2u);
            }

            // Fifth distinct variant fails closed (only opaque/mask × cull
            // exist — a fifth key is a caller bug, never a silent bucket).
            {
                const TrianglePipelineVariant variants[] = {
                    opaque,
                    masked,
                    sided,
                    TrianglePipelineVariant{.m_twosided = true, .m_alpha = mesh::AlphaMode::mask},
                    TrianglePipelineVariant{.m_twosided = false, .m_alpha = mesh::AlphaMode::blend},
                };
                const BagBucketId bags[] = {
                    BagBucketId{0u}, BagBucketId{0u}, BagBucketId{0u}, BagBucketId{0u}, BagBucketId{0u}
                };
                const u32 counts[] = {3u, 3u, 3u, 3u, 3u};
                const Expected<TrianglePass::IndirectPlan> plan = TrianglePass::planIndirectDraws(
                    variants, bags, counts, 16u);
                PPR_TEST_ASSERT(not plan.has_value());
                PPR_TEST_ASSERT(plan.error() == std::make_error_code(std::errc::invalid_argument));
            }

            // Ragged spans fail closed.
            {
                const TrianglePipelineVariant variants[] = {opaque};
                const BagBucketId bags[] = {BagBucketId{0u}};
                const u32 counts[] = {3u, 3u};
                PPR_TEST_ASSERT(not TrianglePass::planIndirectDraws(variants, bags, counts, 16u).has_value());
            }
        };
    } // namespace IndirectLayout
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest indirect_layout = UnitTest::Named("indirect_layout") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::IndirectLayout::indirect_payload_and_plan_contract,
        });
    };

    const UnitTest &indirectLayoutTests() noexcept {
        return indirect_layout;
    }
} // namespace pP::tests
