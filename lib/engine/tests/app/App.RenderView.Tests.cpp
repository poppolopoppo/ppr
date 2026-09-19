module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
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
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest render_view = UnitTest::Named("render_view") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::RendererBoundary::color_attachment_ops_have_sane_defaults,
            detail::RendererBoundary::draw_submission_carries_camera_free_raster_state,
            detail::RendererBoundary::triangle_frame_constants_match_hlsl_layout,
            detail::RendererBoundary::perspective_uses_d3d_depth_zero_to_one,
            detail::RendererBoundary::ortho_uses_d3d_convention_without_y_flip,
        });
    };

    const UnitTest &render_viewTests() noexcept {
        return render_view;
    }
} // namespace pP::tests
