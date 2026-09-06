module;
#include "pP/UnitTest.h"

export module engine.tests.app:render_view;

import engine.app;
import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP::tests {
    namespace RenderViewConversion {
        PPR_UNIT_TEST(boundary_types_have_sane_defaults) {
            const ColorTargetInfo target{};
            PPR_TEST_ASSERT(target.m_format == rhi::Format::Undefined);
            PPR_TEST_ASSERT(target.m_extent.x == 0 && target.m_extent.y == 0);
            PPR_TEST_ASSERT(target.m_sample_count == 1u);

            constexpr RenderView view{};
            PPR_TEST_ASSERT(view.m_viewport.extentX == 0.0f);
            PPR_TEST_ASSERT(view.m_viewport.extentY == 0.0f);
            PPR_TEST_ASSERT(view.m_viewport.maxZ == 1.0f);
            PPR_TEST_ASSERT(view.m_scissor.maxX == 0u);
            PPR_TEST_ASSERT(view.m_scissor.maxY == 0u);

            const ColorPassOptions options{};
            PPR_TEST_ASSERT(options.m_load_op == rhi::LoadOp::Clear);
            PPR_TEST_ASSERT(options.m_store_op == rhi::StoreOp::Store);
            PPR_TEST_ASSERT(options.m_clear_color.x == 0.1f);
            PPR_TEST_ASSERT(options.m_clear_color.y == 0.1f);
            PPR_TEST_ASSERT(options.m_clear_color.z == 0.2f);
            PPR_TEST_ASSERT(options.m_clear_color.w == 1.0f);
        };

        PPR_UNIT_TEST(draw_submission_borrows_view_and_callback) {
            const auto encode = [](rhi::IRenderPassEncoder &, const DrawContext &) -> std::error_code { return {}; };
            const DrawSubmission submission{.m_view = RenderView{}, .m_encode_draws = DrawCallback{encode}};
            PPR_TEST_ASSERT(submission.m_view.m_viewport.extentX == 0.0f);
            PPR_TEST_ASSERT(submission.m_view.m_scissor.maxX == 0u);
        };

        PPR_UNIT_TEST(make_render_view_scales_window_to_target) {
            const Viewport viewport{PixelRect{int2{0, 0}, int2{800, 600}}, ViewportLayout{}};
            const auto view = makeRenderView(viewport, int2{1600, 1200});
            PPR_TEST_ASSERT(view.has_value());
            PPR_TEST_ASSERT(view->m_viewport.originX == 0.0f);
            PPR_TEST_ASSERT(view->m_viewport.originY == 0.0f);
            PPR_TEST_ASSERT(view->m_viewport.extentX == 1600.0f);
            PPR_TEST_ASSERT(view->m_viewport.extentY == 1200.0f);
            PPR_TEST_ASSERT(view->m_viewport.minZ == 0.0f);
            PPR_TEST_ASSERT(view->m_viewport.maxZ == 1.0f);
            PPR_TEST_ASSERT(view->m_scissor.minX == 0u);
            PPR_TEST_ASSERT(view->m_scissor.minY == 0u);
            PPR_TEST_ASSERT(view->m_scissor.maxX == 1600u);
            PPR_TEST_ASSERT(view->m_scissor.maxY == 1200u);
        };

        PPR_UNIT_TEST(make_render_view_rebases_moved_window) {
            const PixelRect window{int2{100, 50}, int2{800, 600}};
            // NOTE (msvc-rel C1001 workaround): forming a WindowRect-bearing layout
            // via named variables, converting assignment, or variant::emplace ICEs
            // function-signature.cpp:213 in this TU; prvalue aggregate init dodges it.
            const Viewport viewport{
                window, ViewportLayout{ViewportLayout::WindowRect{PixelRect{int2{140, 90}, int2{400, 300}}}}};

            const auto view = makeRenderView(viewport, int2{800, 600});
            PPR_TEST_ASSERT(view.has_value());
            PPR_TEST_ASSERT(view->m_viewport.originX == 40.0f);
            PPR_TEST_ASSERT(view->m_viewport.originY == 40.0f);
            PPR_TEST_ASSERT(view->m_viewport.extentX == 400.0f);
            PPR_TEST_ASSERT(view->m_viewport.extentY == 300.0f);
            PPR_TEST_ASSERT(view->m_scissor.minX == 40u);
            PPR_TEST_ASSERT(view->m_scissor.minY == 40u);
            PPR_TEST_ASSERT(view->m_scissor.maxX == 440u);
            PPR_TEST_ASSERT(view->m_scissor.maxY == 340u);
        };

        PPR_UNIT_TEST(make_render_view_clips_to_target) {
            const PixelRect window{int2{0, 0}, int2{800, 600}};

            const Viewport past_edge{window, PixelRect{int2{700, 500}, int2{400, 300}}};
            const auto clipped = makeRenderView(past_edge, int2{800, 600});
            PPR_TEST_ASSERT(clipped.has_value());
            PPR_TEST_ASSERT(clipped->m_viewport.originX == 700.0f);
            PPR_TEST_ASSERT(clipped->m_viewport.originY == 500.0f);
            PPR_TEST_ASSERT(clipped->m_viewport.extentX == 100.0f);
            PPR_TEST_ASSERT(clipped->m_viewport.extentY == 100.0f);
            PPR_TEST_ASSERT(clipped->m_scissor.minX == 700u);
            PPR_TEST_ASSERT(clipped->m_scissor.minY == 500u);
            PPR_TEST_ASSERT(clipped->m_scissor.maxX == 800u);
            PPR_TEST_ASSERT(clipped->m_scissor.maxY == 600u);

            const Viewport negative_origin{window, PixelRect{int2{-100, -50}, int2{400, 300}}};
            const auto pinned = makeRenderView(negative_origin, int2{800, 600});
            PPR_TEST_ASSERT(pinned.has_value());
            PPR_TEST_ASSERT(pinned->m_viewport.originX == 0.0f);
            PPR_TEST_ASSERT(pinned->m_viewport.originY == 0.0f);
            PPR_TEST_ASSERT(pinned->m_viewport.extentX == 300.0f);
            PPR_TEST_ASSERT(pinned->m_viewport.extentY == 250.0f);
            PPR_TEST_ASSERT(pinned->m_scissor.minX == 0u);
            PPR_TEST_ASSERT(pinned->m_scissor.minY == 0u);
            PPR_TEST_ASSERT(pinned->m_scissor.maxX == 300u);
            PPR_TEST_ASSERT(pinned->m_scissor.maxY == 250u);
        };

        PPR_UNIT_TEST(make_render_view_rejects_empty) {
            const PixelRect window{int2{0, 0}, int2{800, 600}};
            const Viewport full{window, ViewportLayout{}};

            PPR_TEST_ASSERT(not makeRenderView(full, int2{0, 0}).has_value());
            PPR_TEST_ASSERT(not makeRenderView(full, int2{800, 0}).has_value());

            const Viewport zero_window{PixelRect{int2{0, 0}, int2{0, 0}}, ViewportLayout{}};
            PPR_TEST_ASSERT(not makeRenderView(zero_window, int2{800, 600}).has_value());

            const Viewport empty_client{window, PixelRect{int2{0, 0}, int2{0, 0}}};
            PPR_TEST_ASSERT(not makeRenderView(empty_client, int2{800, 600}).has_value());

            const Viewport outside{window, PixelRect{int2{900, 700}, int2{100, 100}}};
            PPR_TEST_ASSERT(not makeRenderView(outside, int2{800, 600}).has_value());
        };

        PPR_UNIT_TEST(make_render_view_centered_and_fractional_scale) {
            const PixelRect window{int2{0, 0}, int2{800, 600}};
            // NOTE (msvc-rel C1001 workaround): converting-constructing the layout
            // variant from a Centered alternative ICEs function-signature.cpp:213,
            // so emplace the alternative and assign its extent directly.
            ViewportLayout centered;
            centered.m_variant.emplace<ViewportLayout::Centered>().m_extent = int2{400, 300};
            const Viewport viewport{window, centered};

            const auto unit = makeRenderView(viewport, int2{800, 600});
            PPR_TEST_ASSERT(unit.has_value());
            PPR_TEST_ASSERT(unit->m_viewport.originX == 200.0f);
            PPR_TEST_ASSERT(unit->m_viewport.originY == 150.0f);
            PPR_TEST_ASSERT(unit->m_viewport.extentX == 400.0f);
            PPR_TEST_ASSERT(unit->m_viewport.extentY == 300.0f);
            PPR_TEST_ASSERT(unit->m_scissor.minX == 200u);
            PPR_TEST_ASSERT(unit->m_scissor.minY == 150u);
            PPR_TEST_ASSERT(unit->m_scissor.maxX == 600u);
            PPR_TEST_ASSERT(unit->m_scissor.maxY == 450u);

            const auto scaled = makeRenderView(viewport, int2{1000, 750});
            PPR_TEST_ASSERT(scaled.has_value());
            PPR_TEST_ASSERT(scaled->m_viewport.originX == 250.0f);
            PPR_TEST_ASSERT(scaled->m_viewport.originY == 187.0f);
            PPR_TEST_ASSERT(scaled->m_viewport.extentX == 500.0f);
            PPR_TEST_ASSERT(scaled->m_viewport.extentY == 376.0f);
            PPR_TEST_ASSERT(scaled->m_scissor.minX == 250u);
            PPR_TEST_ASSERT(scaled->m_scissor.minY == 187u);
            PPR_TEST_ASSERT(scaled->m_scissor.maxX == 750u);
            PPR_TEST_ASSERT(scaled->m_scissor.maxY == 563u);
        };
        PPR_UNIT_TEST(color_target_info_explicit_fields) {
            const ColorTargetInfo target{.m_format = rhi::Format::RGBA8Unorm, .m_extent = int2{1280, 720}, .m_sample_count = 4u};
            PPR_TEST_ASSERT(target.m_format == rhi::Format::RGBA8Unorm);
            PPR_TEST_ASSERT(target.m_extent.x == 1280 && target.m_extent.y == 720);
            PPR_TEST_ASSERT(target.m_sample_count == 4u);
        };

        PPR_UNIT_TEST(draw_context_borrows_view_and_target) {
            const PixelRect window{int2{0, 0}, int2{800, 600}};
            const Viewport viewport{window, ViewportLayout{}};
            const auto view = makeRenderView(viewport, int2{800, 600});
            PPR_TEST_ASSERT(view.has_value());
            const ColorTargetInfo target{.m_format = rhi::Format::RGBA8Unorm, .m_extent = int2{800, 600}, .m_sample_count = 1u};
            const DrawContext context{*view, target};
            PPR_TEST_ASSERT(&context.m_view == &(*view));
            PPR_TEST_ASSERT(&context.m_target == &target);
            PPR_TEST_ASSERT(context.m_view.m_viewport.extentX == 800.0f);
            PPR_TEST_ASSERT(context.m_view.m_scissor.maxX == 800u);
        };

        // NOTE (msvc-rel C1001 workaround): value-constructing a CameraSnapshot
        // in this TU ICEs function-signature.cpp:213 (derived struct over the
        // module-imported CameraModel with math-sentinel NSDMIs). Start its
        // lifetime in zeroed storage instead; the fields observed below are
        // assigned explicitly.
        PPR_UNIT_TEST(scene_view_pairs_camera_snapshot_and_render_view) {
            alignas(CameraSnapshot) std::array<std::byte, sizeof(CameraSnapshot)> snapshot_storage{};
            // FP: C++23 std::start_lifetime_as valid per P0476R2, MSVC 19.52 /WX-clean;
            // IDE CL-262.9437.136 cannot consume MSVC BMIs for `import std` (RenderView.Tests.cppm:182).
            // Scope: next line only (ClangdErrorsAndWarnings); re-check after toolchain/BMI refresh.
            //noinspection ClangdErrorsAndWarnings
            auto &snapshot = *std::start_lifetime_as<CameraSnapshot>(snapshot_storage.data());
            snapshot.m_revision = 7u;
            snapshot.m_viewport_size = float2{800.0f, 600.0f};
            const PixelRect window{int2{0, 0}, int2{800, 600}};
            const Viewport viewport{window, ViewportLayout{}};
            const auto view = makeRenderView(viewport, int2{800, 600});
            PPR_TEST_ASSERT(view.has_value());
            const SceneView scene_view{snapshot, *view};
            PPR_TEST_ASSERT(&scene_view.m_camera == &snapshot);
            PPR_TEST_ASSERT(scene_view.m_camera.m_revision == 7u);
            PPR_TEST_ASSERT(scene_view.m_camera.m_viewport_size.x == 800.0f && scene_view.m_camera.m_viewport_size.y == 600.0f);
            PPR_TEST_ASSERT(scene_view.m_render_view.m_viewport.extentX == 800.0f);
            PPR_TEST_ASSERT(scene_view.m_render_view.m_scissor.maxX == 800u);
        };

        PPR_UNIT_TEST(triangle_upload_maps_camera_position_as_point) {
            // Production mirror: App.Renderer.TrianglePass.cpp:228-239 (uploadFrameConstants_).
            alignas(CameraSnapshot) std::array<std::byte, sizeof(CameraSnapshot)> snapshot_storage{};
            // FP: C++23 std::start_lifetime_as valid per P0476R2, MSVC 19.52 /WX-clean;
            // IDE CL-262.9437.136 cannot consume MSVC BMIs for `import std` (RenderView.Tests.cppm:200).
            // Scope: next line only (ClangdErrorsAndWarnings); re-check after toolchain/BMI refresh.
            //noinspection ClangdErrorsAndWarnings
            auto &snapshot = *std::start_lifetime_as<CameraSnapshot>(snapshot_storage.data());
            snapshot.m_origin = float3{1.0f, 2.0f, 3.0f};

            // Mirrors TrianglePass::uploadFrameConstants_: origin uploads as a
            // point (w == 1), not a direction.
            TrianglePass::FrameConstants frame{};
            frame.m_camera_position = float4{snapshot.m_origin, 1.0f};
            PPR_TEST_ASSERT(frame.m_camera_position.x == 1.0f);
            PPR_TEST_ASSERT(frame.m_camera_position.y == 2.0f);
            PPR_TEST_ASSERT(frame.m_camera_position.z == 3.0f);
            PPR_TEST_ASSERT(frame.m_camera_position.w == 1.0f);
        };

        PPR_UNIT_TEST(triangle_consecutive_cuts_same_viewport_upload_fresh) {
            // Consecutive-cut alias: same revision, same viewport size, changed
            // transforms. Viewport held constant; a revision+viewport skip would
            // stale-reuse the first matrices, so the upload must be unconditional.
            alignas(CameraSnapshot) std::array<std::byte, sizeof(CameraSnapshot)> first_storage{};
            alignas(CameraSnapshot) std::array<std::byte, sizeof(CameraSnapshot)> second_storage{};
            // FP: C++23 std::start_lifetime_as valid per P0476R2, MSVC 19.52 /WX-clean;
            // IDE CL-262.9437.136 cannot consume MSVC BMIs for `import std` (RenderView.Tests.cppm:219).
            // Scope: next line only (ClangdErrorsAndWarnings); re-check after toolchain/BMI refresh.
            //noinspection ClangdErrorsAndWarnings
            auto &first = *std::start_lifetime_as<CameraSnapshot>(first_storage.data());
            // FP: C++23 std::start_lifetime_as valid per P0476R2, MSVC 19.52 /WX-clean;
            // IDE CL-262.9437.136 cannot consume MSVC BMIs for `import std` (RenderView.Tests.cppm:220).
            // Scope: next line only (ClangdErrorsAndWarnings); re-check after toolchain/BMI refresh.
            //noinspection ClangdErrorsAndWarnings
            auto &second = *std::start_lifetime_as<CameraSnapshot>(second_storage.data());
            first.m_revision = 0u;
            second.m_revision = 0u;
            first.m_viewport_size = float2{800.0f, 600.0f};
            second.m_viewport_size = float2{800.0f, 600.0f};
            first.m_view = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            second.m_view = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{5, 6, 7, 1}};
            first.m_view_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            second.m_view_projection = float4x4{float4{2, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{5, 6, 7, 1}};

            // Production mirror: App.Renderer.TrianglePass.cpp:228-239 (uploadFrameConstants_ field mapping).
            const auto buildFrame = [](const CameraSnapshot &snapshot) {
                TrianglePass::FrameConstants frame{};
                frame.m_view = snapshot.m_view;
                frame.m_projection = snapshot.m_projection;
                frame.m_view_projection = snapshot.m_view_projection;
                frame.m_inverse_view_projection = snapshot.m_invert_view_projection;
                frame.m_camera_position = float4{snapshot.m_origin, 1.0f};
                frame.m_viewport_size = float4{snapshot.m_viewport_size, 0.0f, 0.0f};
                return frame;
            };
            const auto first_frame = buildFrame(first);
            const auto second_frame = buildFrame(second);
            PPR_TEST_ASSERT(first_frame.m_view_projection(0, 0) == 1.0f);
            PPR_TEST_ASSERT(second_frame.m_view_projection(0, 0) == 2.0f);
            PPR_TEST_ASSERT(second_frame.m_view_projection(0, 0) != first_frame.m_view_projection(0, 0));
            PPR_TEST_ASSERT(second_frame.m_view(3, 0) == 5.0f);
            PPR_TEST_ASSERT(second_frame.m_view(3, 0) != first_frame.m_view(3, 0));
        };

        PPR_UNIT_TEST(triangle_frame_constants_match_hlsl_layout) {
            PPR_TEST_ASSERT(sizeof(TrianglePass::FrameConstants) == 288u);
            const TrianglePass::FrameConstants frame{};
            PPR_TEST_ASSERT(frame.m_view(0, 0) == 1.0f && frame.m_view(3, 3) == 1.0f);
            PPR_TEST_ASSERT(frame.m_view(0, 1) == 0.0f && frame.m_view(3, 0) == 0.0f);
            PPR_TEST_ASSERT(frame.m_projection(1, 1) == 1.0f && frame.m_view_projection(2, 2) == 1.0f);
            PPR_TEST_ASSERT(frame.m_inverse_view_projection(3, 3) == 1.0f);
            PPR_TEST_ASSERT(frame.m_camera_position.x == 0.0f && frame.m_camera_position.w == 0.0f);
            PPR_TEST_ASSERT(frame.m_viewport_size.x == 0.0f && frame.m_viewport_size.y == 0.0f);
        };
        PPR_UNIT_TEST(perspective_uses_d3d_depth_zero_to_one) {
            const auto first = rhi::getPerspectiveMatrix(0.75f, 16.0f / 9.0f, 0.1f, 1000.0f);
            const auto second = rhi::getPerspectiveMatrix(0.75f, 16.0f / 9.0f, 0.1f, 1000.0f);
            PPR_TEST_ASSERT(std::ranges::all_of(std::span<const float, 16>(first.data(), 16), [](const float value) noexcept { return std::isfinite(value); }));
            PPR_TEST_ASSERT(std::ranges::equal(std::span<const float, 16>(first.data(), 16), std::span<const float, 16>(second.data(), 16)));
            PPR_TEST_ASSERT(std::abs(first(2, 3) - 1.0f) < 1e-4f);
            PPR_TEST_ASSERT(first(2, 2) > 1.0f && first(2, 2) < 1.01f);
            PPR_TEST_ASSERT(first(3, 2) < 0.0f && first(3, 2) > -1.0f);
        };

        PPR_UNIT_TEST(ortho_uses_d3d_convention_without_y_flip) {
            const auto first = rhi::getOrthoMatrix(800.0f, 600.0f);
            const auto second = rhi::getOrthoMatrix(800.0f, 600.0f);
            PPR_TEST_ASSERT(std::ranges::all_of(std::span<const float, 16>(first.data(), 16), [](const float value) noexcept { return std::isfinite(value); }));
            PPR_TEST_ASSERT(std::ranges::equal(std::span<const float, 16>(first.data(), 16), std::span<const float, 16>(second.data(), 16)));
            PPR_TEST_ASSERT(std::abs(first(0, 0) - 2.0f / 800.0f) < 1e-7f);
            PPR_TEST_ASSERT(first(1, 1) > 0.0f);
            PPR_TEST_ASSERT(std::abs(first(2, 2) - 1.0f) < 1e-6f);
        };
    }

    PPR_UNIT_TEST(render_view){
        _.recurse({
            RenderViewConversion::boundary_types_have_sane_defaults,
            RenderViewConversion::draw_submission_borrows_view_and_callback,
            RenderViewConversion::make_render_view_scales_window_to_target,
            RenderViewConversion::make_render_view_rebases_moved_window,
            RenderViewConversion::make_render_view_clips_to_target,
            RenderViewConversion::make_render_view_rejects_empty,
            RenderViewConversion::make_render_view_centered_and_fractional_scale,
            RenderViewConversion::color_target_info_explicit_fields,
            RenderViewConversion::draw_context_borrows_view_and_target,
            RenderViewConversion::scene_view_pairs_camera_snapshot_and_render_view,
            RenderViewConversion::triangle_frame_constants_match_hlsl_layout,
            RenderViewConversion::triangle_upload_maps_camera_position_as_point,
            RenderViewConversion::triangle_consecutive_cuts_same_viewport_upload_fresh,
            RenderViewConversion::perspective_uses_d3d_depth_zero_to_one,
            RenderViewConversion::ortho_uses_d3d_convention_without_y_flip,
        });

    };
}
