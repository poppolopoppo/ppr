module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.mesh;
import engine.app;
import engine.math;
import engine.rhi;
import std;

namespace pP::tests::detail {
    namespace RendererBoundary {
        PPR_UNIT_TEST (color_attachment_ops_have_sane_defaults) {
            const ColorAttachmentOps options{};
            PPR_TEST_ASSERT(options.m_load_op == rhi::LoadOp::Clear);
            PPR_TEST_ASSERT(options.m_store_op == rhi::StoreOp::Store);
            PPR_TEST_ASSERT(options.m_clear_color.x == 0.1f);
            PPR_TEST_ASSERT(options.m_clear_color.y == 0.1f);
            PPR_TEST_ASSERT(options.m_clear_color.z == 0.2f);
            PPR_TEST_ASSERT(options.m_clear_color.w == 1.0f);
        };

        PPR_UNIT_TEST (draw_submission_carries_camera_free_raster_state) {
            const auto encode = [](const DrawContext &) -> std::error_code { return {}; };
            DrawSubmission submission{"camera-free", DrawCallback{encode}};
            submission.m_viewport = rhi::Viewport{.originX = 4.0f, .originY = 8.0f, .extentX = 400.0f, .extentY = 300.0f};
            submission.m_scissor = rhi::ScissorRect{.minX = 4u, .minY = 8u, .maxX = 404u, .maxY = 308u};
            PPR_TEST_ASSERT(submission.m_viewport.has_value());
            PPR_TEST_ASSERT(submission.m_scissor.has_value());
            PPR_TEST_ASSERT(submission.m_viewport->extentX == 400.0f);
            PPR_TEST_ASSERT(submission.m_scissor->maxY == 308u);
        };

        PPR_UNIT_TEST (triangle_frame_constants_match_hlsl_layout) {
            PPR_TEST_ASSERT(sizeof(TrianglePass::FrameConstants) == 288u);
            const TrianglePass::FrameConstants frame{};
            PPR_TEST_ASSERT(frame.m_view(0, 0) == 1.0f && frame.m_view(3, 3) == 1.0f);
            PPR_TEST_ASSERT(frame.m_projection(1, 1) == 1.0f && frame.m_view_projection(2, 2) == 1.0f);
            PPR_TEST_ASSERT(frame.m_camera_position.w == 0.0f);
        };

        PPR_UNIT_TEST (perspective_uses_d3d_depth_zero_to_one) {
            const auto first = rhi::getPerspectiveMatrix(0.75f, 16.0f / 9.0f, 0.1f, 1000.0f);
            const auto second = rhi::getPerspectiveMatrix(0.75f, 16.0f / 9.0f, 0.1f, 1000.0f);
            PPR_TEST_ASSERT(std::ranges::all_of(std::span<const float, 16>(first.data(), 16), [](const float value) noexcept { return std::isfinite(value); }));
            PPR_TEST_ASSERT(std::ranges::equal(std::span<const float, 16>(first.data(), 16), std::span<const float, 16>(second.data(), 16)));
            PPR_TEST_ASSERT(std::abs(first(2, 3) - 1.0f) < 1e-4f);
        };

        PPR_UNIT_TEST (ortho_uses_d3d_convention_without_y_flip) {
            const auto matrix = rhi::getOrthoMatrix(800.0f, 600.0f);
            PPR_TEST_ASSERT(std::abs(matrix(0, 0) - 2.0f / 800.0f) < 1e-7f);
            PPR_TEST_ASSERT(matrix(1, 1) > 0.0f);
            PPR_TEST_ASSERT(std::abs(matrix(2, 2) - 1.0f) < 1e-6f);
        };

        // GPU rows are plain float arrays; mango float4 is the
        // expected side only (float4 == float4 yields a simd mask, not bool).
        [[nodiscard]] bool float4Equal_(const float (&lhs)[4], const float4 &rhs) noexcept {
            return lhs[0] == rhs.x and lhs[1] == rhs.y and lhs[2] == rhs.z and lhs[3] == rhs.w;
        }

        PPR_UNIT_TEST (handles_default_invalid) {
            PPR_TEST_ASSERT(not pP::isValid(TextureHandle{}));
            PPR_TEST_ASSERT(not pP::isValid(MaterialHandle{}));
            PPR_TEST_ASSERT(not pP::isValid(TriangleBagHandle{}));
            PPR_TEST_ASSERT(*kNoTexture == 0xFFFFFFFFu);
            PPR_TEST_ASSERT(sizeof(GpuMaterial) == 80u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GpuMaterial, m_textures) == 48u);
            PPR_TEST_ASSERT(sizeof(GpuTextureRefs) == 16u);
            PPR_TEST_ASSERT(sizeof(GpuMaterialFlags) == 4u);
        };

        PPR_UNIT_TEST (build_material_maps_factors) {
            const Expected<GpuMaterial> gpu = buildGpuMaterial(
                mesh::MaterialAsset{}, {kNoTexture, kNoTexture, kNoTexture, kNoTexture});
            PPR_TEST_ASSERT(gpu.has_value());
            PPR_TEST_ASSERT(float4Equal_(gpu->m_base_color, float4{1.0f, 1.0f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(float4Equal_(gpu->m_emissive_metallic, float4{0.0f, 0.0f, 0.0f, 1.0f}));
            PPR_TEST_ASSERT(float4Equal_(gpu->m_rough_alpha_occl_nscale, float4{1.0f, 0.5f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(gpu->m_textures.m_albedo == kNoTexture);
            PPR_TEST_ASSERT(gpu->m_textures.m_metallic_roughness == kNoTexture);
            PPR_TEST_ASSERT(gpu->m_textures.m_normal == kNoTexture);
            PPR_TEST_ASSERT(gpu->m_textures.m_emissive == kNoTexture);
            PPR_TEST_ASSERT(gpu->m_flags.m_bits == 0u);
        };

        PPR_UNIT_TEST (build_material_texcoord_agreement) {
            mesh::MaterialAsset textured{};
            textured.m_base_color_map.m_image = mesh::ImageAssetId{0u};
            textured.m_base_color_map.m_texcoord = mesh::UvSetId{1u};
            textured.m_normal_map.m_image = mesh::ImageAssetId{2u};
            textured.m_normal_map.m_texcoord = mesh::UvSetId{1u};
            const TextureBindlessIndex albedo{7u};
            const TextureBindlessIndex normal{9u};
            const Expected<GpuMaterial> agreed = buildGpuMaterial(
                textured, {albedo, kNoTexture, normal, kNoTexture});
            PPR_TEST_ASSERT(agreed.has_value());
            PPR_TEST_ASSERT(agreed->m_textures.m_albedo == albedo);
            PPR_TEST_ASSERT(*agreed->m_texcoord == 1u);

            textured.m_normal_map.m_texcoord = mesh::UvSetId{0u};
            const Expected<GpuMaterial> split = buildGpuMaterial(
                textured, {albedo, kNoTexture, normal, kNoTexture});
            PPR_TEST_ASSERT(not split.has_value());
            PPR_TEST_ASSERT(split.error() == std::make_error_code(std::errc::invalid_argument));
        };

        PPR_UNIT_TEST (build_material_alpha_flags) {
            mesh::MaterialAsset mask{};
            mask.m_alpha_mode = mesh::AlphaMode::mask;
            mask.m_alpha_cutoff = 0.25f;
            mask.m_twosided = true;
            const Expected<GpuMaterial> gpu = buildGpuMaterial(
                mask, {kNoTexture, kNoTexture, kNoTexture, kNoTexture});
            PPR_TEST_ASSERT(gpu.has_value());
            PPR_TEST_ASSERT((gpu->m_flags.m_bits & kGpuMaterialAlphaModeMask) == enumOrd(mesh::AlphaMode::mask));
            PPR_TEST_ASSERT((gpu->m_flags.m_bits & kGpuMaterialDoubleSidedBit) != 0u);
            PPR_TEST_ASSERT(gpu->m_rough_alpha_occl_nscale[1] == 0.25f);

            mesh::MaterialAsset blend{};
            blend.m_alpha_mode = mesh::AlphaMode::blend;
            const Expected<GpuMaterial> rejected = buildGpuMaterial(
                blend, {kNoTexture, kNoTexture, kNoTexture, kNoTexture});
            PPR_TEST_ASSERT(not rejected.has_value());
            PPR_TEST_ASSERT(rejected.error() == std::make_error_code(std::errc::function_not_supported));
        };

        PPR_UNIT_TEST (pipeline_variant_key) {
            PPR_TEST_ASSERT(not checkPipelineVariant({.m_twosided = false, .m_alpha = mesh::AlphaMode::opaque}));
            PPR_TEST_ASSERT(not checkPipelineVariant({.m_twosided = true, .m_alpha = mesh::AlphaMode::mask}));
            PPR_TEST_ASSERT(checkPipelineVariant({.m_twosided = false, .m_alpha = mesh::AlphaMode::blend}) ==
                            std::make_error_code(std::errc::function_not_supported));
            PPR_TEST_ASSERT(checkPipelineVariant({.m_twosided = true, .m_alpha = mesh::AlphaMode::blend}) ==
                            std::make_error_code(std::errc::function_not_supported));
            const TrianglePipelineVariant opaque{};
            const TrianglePipelineVariant masked{.m_twosided = false, .m_alpha = mesh::AlphaMode::mask};
            const TrianglePipelineVariant sided{.m_twosided = true, .m_alpha = mesh::AlphaMode::opaque};
            PPR_TEST_ASSERT(opaque != masked);
            PPR_TEST_ASSERT(opaque != sided);
            PPR_TEST_ASSERT(masked != sided);
        };

        PPR_UNIT_TEST (indirect_plan_contract) {
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
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest render_view = UnitTest::Named("render_view") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::RendererBoundary::color_attachment_ops_have_sane_defaults,
            detail::RendererBoundary::draw_submission_carries_camera_free_raster_state,
            detail::RendererBoundary::triangle_frame_constants_match_hlsl_layout,
            detail::RendererBoundary::handles_default_invalid,
            detail::RendererBoundary::build_material_maps_factors,
            detail::RendererBoundary::build_material_texcoord_agreement,
            detail::RendererBoundary::build_material_alpha_flags,
            detail::RendererBoundary::pipeline_variant_key,
            detail::RendererBoundary::indirect_plan_contract,
            detail::RendererBoundary::perspective_uses_d3d_depth_zero_to_one,
            detail::RendererBoundary::ortho_uses_d3d_convention_without_y_flip,
        });
    };

    const UnitTest &render_viewTests() noexcept {
        return render_view;
    }
} // namespace pP::tests
