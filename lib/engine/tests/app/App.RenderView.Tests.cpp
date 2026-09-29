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
        PPR_UNIT_TEST(color_attachment_ops_have_sane_defaults) {
            const ColorAttachmentOps options{};
            PPR_TEST_ASSERT(options.m_load_op == rhi::LoadOp::Clear);
            PPR_TEST_ASSERT(options.m_store_op == rhi::StoreOp::Store);
            PPR_TEST_ASSERT(options.m_clear_color.x == 0.1f);
            PPR_TEST_ASSERT(options.m_clear_color.y == 0.1f);
            PPR_TEST_ASSERT(options.m_clear_color.z == 0.2f);
            PPR_TEST_ASSERT(options.m_clear_color.w == 1.0f);
        };

        PPR_UNIT_TEST(draw_submission_carries_camera_free_raster_state) {
            const auto encode = [](const DrawContext &) -> std::error_code { return {}; };
            DrawSubmission submission{"camera-free", DrawCallback{encode}};
            submission.m_viewport = rhi::Viewport{.originX = 4.0f, .originY = 8.0f, .extentX = 400.0f, .extentY = 300.0f};
            submission.m_scissor = rhi::ScissorRect{.minX = 4u, .minY = 8u, .maxX = 404u, .maxY = 308u};
            PPR_TEST_ASSERT(submission.m_viewport.has_value());
            PPR_TEST_ASSERT(submission.m_scissor.has_value());
            PPR_TEST_ASSERT(submission.m_viewport->extentX == 400.0f);
            PPR_TEST_ASSERT(submission.m_scissor->maxY == 308u);
        };

        PPR_UNIT_TEST(triangle_frame_constants_match_hlsl_layout) {
            PPR_TEST_ASSERT(sizeof(TrianglePass::FrameConstants) == 288u);
            const TrianglePass::FrameConstants frame{};
            PPR_TEST_ASSERT(frame.m_view(0, 0) == 1.0f && frame.m_view(3, 3) == 1.0f);
            PPR_TEST_ASSERT(frame.m_projection(1, 1) == 1.0f && frame.m_view_projection(2, 2) == 1.0f);
            PPR_TEST_ASSERT(frame.m_camera_position.w == 0.0f);
        };

        PPR_UNIT_TEST(perspective_uses_d3d_depth_zero_to_one) {
            const auto first = rhi::getPerspectiveMatrix(0.75f, 16.0f / 9.0f, 0.1f, 1000.0f);
            const auto second = rhi::getPerspectiveMatrix(0.75f, 16.0f / 9.0f, 0.1f, 1000.0f);
            PPR_TEST_ASSERT(std::ranges::all_of(std::span<const float, 16>(first.data(), 16), [](const float value) noexcept { return std::isfinite(value); }));
            PPR_TEST_ASSERT(std::ranges::equal(std::span<const float, 16>(first.data(), 16), std::span<const float, 16>(second.data(), 16)));
            PPR_TEST_ASSERT(std::abs(first(2, 3) - 1.0f) < 1e-4f);
        };

        PPR_UNIT_TEST(ortho_uses_d3d_convention_without_y_flip) {
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

        PPR_UNIT_TEST(handles_default_invalid) {
            PPR_TEST_ASSERT(not TextureHandle{}->isValid());
            PPR_TEST_ASSERT(not MaterialHandle{}->isValid());
            PPR_TEST_ASSERT(not TriangleBagHandle{}->isValid());
            PPR_TEST_ASSERT(none_v == 0xFFFFFFFFu);
            PPR_TEST_ASSERT(sizeof(GpuMaterial) == 80u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GpuMaterial, m_textures) == 48u);
            PPR_TEST_ASSERT(sizeof(GpuTextureRefs) == 16u);
            PPR_TEST_ASSERT(sizeof(GpuMaterialFlags) == 4u);
        };

        PPR_UNIT_TEST(build_material_maps_factors) {
            const Expected<GpuMaterial> gpu = buildGpuMaterial(
                mesh::MaterialAsset{}, {none_v, none_v, none_v, none_v});
            PPR_TEST_ASSERT(gpu.has_value());
            PPR_TEST_ASSERT(float4Equal_(gpu->m_base_color, float4{1.0f, 1.0f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(float4Equal_(gpu->m_emissive_metallic, float4{0.0f, 0.0f, 0.0f, 1.0f}));
            PPR_TEST_ASSERT(float4Equal_(gpu->m_rough_alpha_occl_nscale, float4{1.0f, 0.5f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(gpu->m_textures.m_albedo == none_v);
            PPR_TEST_ASSERT(gpu->m_textures.m_metallic_roughness == none_v);
            PPR_TEST_ASSERT(gpu->m_textures.m_normal == none_v);
            PPR_TEST_ASSERT(gpu->m_textures.m_emissive == none_v);
            PPR_TEST_ASSERT(gpu->m_flags.m_bits == 0u);
        };

        PPR_UNIT_TEST(build_material_texcoord_agreement) {
            mesh::MaterialAsset textured{};
            textured.m_base_color_map.m_image = mesh::ImageAssetId{0u};
            textured.m_base_color_map.m_texcoord = mesh::UvSetId{1u};
            textured.m_normal_map.m_image = mesh::ImageAssetId{2u};
            textured.m_normal_map.m_texcoord = mesh::UvSetId{1u};
            const TextureBindlessIndex albedo{7u};
            const TextureBindlessIndex normal{9u};
            const Expected<GpuMaterial> agreed = buildGpuMaterial(
                textured, {albedo, none_v, normal, none_v});
            PPR_TEST_ASSERT(agreed.has_value());
            PPR_TEST_ASSERT(agreed->m_textures.m_albedo == albedo);
            PPR_TEST_ASSERT(*agreed->m_texcoord == 1u);

            textured.m_normal_map.m_texcoord = mesh::UvSetId{0u};
            const Expected<GpuMaterial> split = buildGpuMaterial(
                textured, {albedo, none_v, normal, none_v});
            PPR_TEST_ASSERT(not split.has_value());
            PPR_TEST_ASSERT(split.error() == std::make_error_code(std::errc::invalid_argument));
        };

        PPR_UNIT_TEST(build_material_alpha_flags) {
            mesh::MaterialAsset mask{};
            mask.m_alpha_mode = mesh::EAlphaMode::mask;
            mask.m_alpha_cutoff = 0.25f;
            mask.m_is_two_sided = true;
            const Expected<GpuMaterial> gpu = buildGpuMaterial(
                mask, {none_v, none_v, none_v, none_v});
            PPR_TEST_ASSERT(gpu.has_value());
            PPR_TEST_ASSERT((gpu->m_flags.m_bits & kGpuMaterialAlphaModeMask) == enumOrd(mesh::EAlphaMode::mask));
            PPR_TEST_ASSERT((gpu->m_flags.m_bits & kGpuMaterialDoubleSidedBit) != 0u);
            PPR_TEST_ASSERT(gpu->m_rough_alpha_occl_nscale[1] == 0.25f);

            mesh::MaterialAsset blend{};
            blend.m_alpha_mode = mesh::EAlphaMode::blend;
            const Expected<GpuMaterial> rejected = buildGpuMaterial(
                blend, {none_v, none_v, none_v, none_v});
            PPR_TEST_ASSERT(not rejected.has_value());
            PPR_TEST_ASSERT(rejected.error() == std::make_error_code(std::errc::function_not_supported));
        };

        PPR_UNIT_TEST(pipeline_variant_key) {
            PPR_TEST_ASSERT(not checkPipelineVariant({.m_is_two_sided = false, .m_alpha = mesh::EAlphaMode::opaque}));
            PPR_TEST_ASSERT(not checkPipelineVariant({.m_is_two_sided = true, .m_alpha = mesh::EAlphaMode::mask}));
            PPR_TEST_ASSERT(checkPipelineVariant({.m_is_two_sided = false, .m_alpha = mesh::EAlphaMode::blend}) ==
                std::make_error_code(std::errc::function_not_supported));
            PPR_TEST_ASSERT(checkPipelineVariant({.m_is_two_sided = true, .m_alpha = mesh::EAlphaMode::blend}) ==
                std::make_error_code(std::errc::function_not_supported));
            const TrianglePipelineVariant opaque{};
            const TrianglePipelineVariant masked{.m_is_two_sided = false, .m_alpha = mesh::EAlphaMode::mask};
            const TrianglePipelineVariant sided{.m_is_two_sided = true, .m_alpha = mesh::EAlphaMode::opaque};
            PPR_TEST_ASSERT(opaque != masked);
            PPR_TEST_ASSERT(opaque != sided);
            PPR_TEST_ASSERT(masked != sided);
        };

        // Opaque stand-ins for the resolved vertex/index buffer pair. The
        // planners are pure CPU functions that only ever compare identity, so
        // distinct non-null sentinels are enough to prove grouping behaviour.
        rhi::IBuffer *const kBucketAVertices = reinterpret_cast<rhi::IBuffer *>(0x1000);
        rhi::IBuffer *const kBucketAIndices = reinterpret_cast<rhi::IBuffer *>(0x2000);
        rhi::IBuffer *const kBucketBVertices = reinterpret_cast<rhi::IBuffer *>(0x3000);
        rhi::IBuffer *const kBucketBIndices = reinterpret_cast<rhi::IBuffer *>(0x4000);

        // GPU payload layout, shared with mesh_bindless.slang: 96 B = 64 model
        // + 20 scalars + 12 pad, 16-aligned. A field-width or order change on
        // either side silently reinterprets every draw, so the contract is
        // pinned here rather than left to a draw that would look plausible.
        PPR_UNIT_TEST(instance_payload_layout_matches_the_shader) {
            PPR_TEST_ASSERT(sizeof(TrianglePass::InstancePayload) == 96u);
            PPR_TEST_ASSERT(alignof(TrianglePass::InstancePayload) == 16u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::InstancePayload, m_model) == 0u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::InstancePayload, m_vb_offset) == 64u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::InstancePayload, m_ib_start) == 68u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::InstancePayload, m_index_count) == 72u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::InstancePayload, m_material) == 76u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::InstancePayload, m_base_vertex) == 80u);
            PPR_TEST_ASSERT(std::is_trivially_copyable_v<TrianglePass::InstancePayload>);
            PPR_TEST_ASSERT(std::is_standard_layout_v<TrianglePass::InstancePayload>);

            // FrameConstants is uploaded as one 288-byte blob into g_frame and
            // is not layout-adaptive: 4 matrices + 2 float4.
            PPR_TEST_ASSERT(sizeof(TrianglePass::FrameConstants) == 288u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::FrameConstants, m_view_projection) == 128u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(TrianglePass::FrameConstants, m_camera_position) == 256u);
        };

        PPR_UNIT_TEST(draw_plan_contract) {
            constexpr TrianglePipelineVariant opaque{};
            constexpr TrianglePipelineVariant masked{.m_is_two_sided = false, .m_alpha = mesh::EAlphaMode::mask};

            const TrianglePass::ResolvedInstance instances[] = {
                {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 0u, .m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 0u, .m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 0u, .m_index_count = 12u}, .m_variant = opaque, .m_count = 12u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 0u, .m_index_count = 36u}, .m_variant = masked, .m_count = 36u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                {.m_variant = opaque, .m_count = 0u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 12u, .m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketBVertices, .m_index_buffer = kBucketBIndices},
            };
            const Expected<TrianglePass::DrawPlan> plan =
                TrianglePass::planDraws(instances);
            PPR_TEST_ASSERT(plan.has_value());
            PPR_TEST_ASSERT(plan->m_payload_count == 5u);
            PPR_TEST_ASSERT(plan->m_groups.size() == 4u);

            // Same variant, buffer pair, and exact resolved range batch together.
            PPR_TEST_ASSERT(plan->m_groups[0].m_first_payload == 0u);
            PPR_TEST_ASSERT(plan->m_groups[0].m_instance_count == 2u);
            PPR_TEST_ASSERT(plan->m_groups[0].m_count == 36u);
            PPR_TEST_ASSERT(plan->m_groups[0].m_source_indices.size() == 2u);
            // Vertex count, pipeline variant, and index start each split the group.
            PPR_TEST_ASSERT(plan->m_groups[1].m_first_payload == 2u);
            PPR_TEST_ASSERT(plan->m_groups[1].m_instance_count == 1u);
            PPR_TEST_ASSERT(plan->m_groups[1].m_count == 12u);
            PPR_TEST_ASSERT(plan->m_groups[2].m_first_payload == 3u);
            PPR_TEST_ASSERT(plan->m_groups[2].m_variant == masked);
            PPR_TEST_ASSERT(plan->m_groups[3].m_first_payload == 4u);
            PPR_TEST_ASSERT(plan->m_groups[3].m_vertex_buffer == kBucketBVertices);
            PPR_TEST_ASSERT(plan->m_groups[3].m_index_buffer == kBucketBIndices);
            PPR_TEST_ASSERT(plan->m_groups[3].m_ib_start == 12u);

            // Interleaved compatible inputs are compacted into a contiguous
            // payload interval while retaining their original source order.
            const TrianglePass::ResolvedInstance interleaved_instances[] = {
                {.m_payload = {.m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                {.m_payload = {.m_index_count = 36u}, .m_variant = masked, .m_count = 36u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                {.m_payload = {.m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
            };
            const Expected<TrianglePass::DrawPlan> interleaved = TrianglePass::planDraws(
                interleaved_instances);
            PPR_TEST_ASSERT(interleaved.has_value());
            PPR_TEST_ASSERT(interleaved->m_groups.size() == 2u);
            PPR_TEST_ASSERT(interleaved->m_groups[0].m_first_payload == 0u);
            PPR_TEST_ASSERT(interleaved->m_groups[0].m_instance_count == 2u);
            PPR_TEST_ASSERT(interleaved->m_groups[0].m_source_indices[0] == 0u);
            PPR_TEST_ASSERT(interleaved->m_groups[0].m_source_indices[1] == 2u);
            PPR_TEST_ASSERT(interleaved->m_groups[1].m_first_payload == 2u);

            // Regression (interleaved A/B/A/B/A): a group's payload base is a
            // PAYLOAD index, not a staged-draw index. Group 1 starts at payload
            // 3, so resolving its geometry by staged[3] would hand it A's
            // buffers (A occupies staged 0/2/4) while its payloads carry B's.
            // The group must carry B's buffers and B's source indices.
            {
                const TrianglePass::ResolvedInstance ab_instances[] = {
                    {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 0u, .m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                    {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 6u, .m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketBVertices, .m_index_buffer = kBucketBIndices},
                    {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 0u, .m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                    {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 6u, .m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketBVertices, .m_index_buffer = kBucketBIndices},
                    {.m_payload = {.m_vb_offset = 0u, .m_ib_start = 0u, .m_index_count = 36u}, .m_variant = opaque, .m_count = 36u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
                };
                const Expected<TrianglePass::DrawPlan> ab =
                    TrianglePass::planDraws(ab_instances);
                PPR_TEST_ASSERT(ab.has_value());
                PPR_TEST_ASSERT(ab->m_payload_count == 5u);
                PPR_TEST_ASSERT(ab->m_groups.size() == 2u);

                PPR_TEST_ASSERT(ab->m_groups[0].m_first_payload == 0u);
                PPR_TEST_ASSERT(ab->m_groups[0].m_instance_count == 3u);
                PPR_TEST_ASSERT(ab->m_groups[0].m_vertex_buffer == kBucketAVertices);
                PPR_TEST_ASSERT(ab->m_groups[0].m_index_buffer == kBucketAIndices);
                PPR_TEST_ASSERT(ab->m_groups[0].m_ib_start == 0u);
                PPR_TEST_ASSERT(ab->m_groups[0].m_source_indices.size() == 3u);
                PPR_TEST_ASSERT(ab->m_groups[0].m_source_indices[0] == 0u);
                PPR_TEST_ASSERT(ab->m_groups[0].m_source_indices[1] == 2u);
                PPR_TEST_ASSERT(ab->m_groups[0].m_source_indices[2] == 4u);

                // Non-zero payload base: the staged draw at that payload index
                // is an A draw, so this asserts the group did NOT read it.
                PPR_TEST_ASSERT(ab->m_groups[1].m_first_payload == 3u);
                PPR_TEST_ASSERT(ab->m_groups[1].m_instance_count == 2u);
                PPR_TEST_ASSERT(ab->m_groups[1].m_vertex_buffer == kBucketBVertices);
                PPR_TEST_ASSERT(ab->m_groups[1].m_index_buffer == kBucketBIndices);
                PPR_TEST_ASSERT(ab->m_groups[1].m_ib_start == 6u);
                PPR_TEST_ASSERT(ab->m_groups[1].m_source_indices.size() == 2u);
                PPR_TEST_ASSERT(ab->m_groups[1].m_source_indices[0] == 1u);
                PPR_TEST_ASSERT(ab->m_groups[1].m_source_indices[1] == 3u);
            }

            // Degenerate inputs stay valid: an empty span plans no groups, and a
            // single instance plans one group. The old four-span length
            // mismatch has no counterpart in the single-span signature.
            const TrianglePass::ResolvedInstance one[] = {
                {.m_payload = {.m_index_count = 3u}, .m_variant = opaque, .m_count = 3u, .m_vertex_buffer = kBucketAVertices, .m_index_buffer = kBucketAIndices},
            };
            const Expected<TrianglePass::DrawPlan> empty_plan =
                TrianglePass::planDraws(std::span<const TrianglePass::ResolvedInstance>{});
            PPR_TEST_ASSERT(empty_plan.has_value());
            PPR_TEST_ASSERT(empty_plan->m_groups.empty());
            PPR_TEST_ASSERT(empty_plan->m_payload_count == 0u);
            PPR_TEST_ASSERT(TrianglePass::planDraws(one).has_value());
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
            detail::RendererBoundary::instance_payload_layout_matches_the_shader,
            detail::RendererBoundary::draw_plan_contract,
            detail::RendererBoundary::perspective_uses_d3d_depth_zero_to_one,
            detail::RendererBoundary::ortho_uses_d3d_convention_without_y_flip,
        });
    };

    const UnitTest &render_viewTests() noexcept {
        return render_view;
    }
} // namespace pP::tests
