module;
#include "pP/UnitTest.h"

export module engine.tests.app:viewport;

import engine.app;
import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP::tests {
    namespace ProjectionConv {
        PPR_UNIT_TEST (all_backends_use_one_projection) {
            const auto perspective = rhi::getPerspectiveMatrix(0.75f, 16.0f / 9.0f, 0.1f, 1000.0f);
            const auto ortho = rhi::getOrthoMatrix(800.0f, 600.0f);
            PPR_TEST_ASSERT(std::ranges::all_of(std::span<const float, 16>(perspective.data(), 16), [](const float value) noexcept { return std::isfinite(value); }));
            PPR_TEST_ASSERT(std::ranges::all_of(std::span<const float, 16>(ortho.data(), 16), [](const float value) noexcept { return std::isfinite(value); }));
            PPR_TEST_ASSERT(std::abs(perspective(2, 3) - 1.0f) < 1e-4f);
            PPR_TEST_ASSERT(std::abs(ortho(0, 0) - 2.0f / 800.0f) < 1e-7f);
            PPR_TEST_ASSERT(ortho(1, 1) > 0.0f);
            PPR_TEST_ASSERT(std::abs(ortho(2, 2) - 1.0f) < 1e-6f);
        };
    }

    namespace ViewportTypes {
        PPR_UNIT_TEST (window_viewport_layout_switch_bumps_revision) {
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}
            };
            WindowViewport viewport{SharedWindow{&window}, ViewportLayout{}};
            PPR_TEST_ASSERT(viewport.getViewport().getClientRect() == PixelRect{int2{10, 20}, int2{800, 600}});
            const auto base_revision = viewport.getViewportRevision();

            viewport.setLayout(ViewportLayout{ViewportLayout::Centered{int2{400, 300}}});
            PPR_TEST_ASSERT(viewport.getViewport().getClientRect() == PixelRect{int2{210, 170}, int2{400, 300}});
            PPR_TEST_ASSERT(viewport.getViewportRevision() != base_revision);

            const auto centered_revision = viewport.getViewportRevision();
            viewport.setLayout(ViewportLayout{ViewportLayout::Centered{int2{400, 300}}});
            PPR_TEST_ASSERT(viewport.getViewportRevision() == centered_revision);
            PPR_TEST_ASSERT(not viewport.updateFromWindow());
            std::ignore = window.release();
        };

        PPR_UNIT_TEST (camera_keeps_prior_state_on_zero_viewport) {
            const Viewport valid{PixelRect{int2{0, 0}, int2{800, 600}}, ViewportLayout{}};
            const Viewport zero{PixelRect{int2{0, 0}, int2{0, 0}}, ViewportLayout{}};
            Camera cam;
            cam.updateModel(std::chrono::milliseconds{16}, CameraModel{}, valid);
            cam.updateModel(std::chrono::milliseconds{16}, CameraModel{}, valid);
            PPR_TEST_ASSERT(cam.getSnapshot().m_viewport_size.x == 800.0f && cam.getSnapshot().m_viewport_size.y == 600.0f);
            const auto revision = cam.getRevision();

            cam.updateModel(std::chrono::milliseconds{16}, CameraModel{}, zero);
            PPR_TEST_ASSERT(cam.getSnapshot().m_viewport_size.x == 800.0f && cam.getSnapshot().m_viewport_size.y == 600.0f);
            PPR_TEST_ASSERT(cam.getRevision() == revision);
        };
    }

    namespace ServiceStores {
        struct MockSceneService : IService {
            int value{};
        };

        struct MockUiService : IService {
            int value{};
        };

        PPR_UNIT_TEST (child_store_shadows_parent) {
            MockSceneService scene;
            scene.value = 1;
            MockUiService ui;
            ui.value = 2;

            ServicesStore parent;
            PPR_TEST_ASSERT(parent.insert(safe_ptr<MockSceneService>(&scene)));

            ServicesStore ui_store{safe_ptr<ServicesStore>(&parent)};
            PPR_TEST_ASSERT(ui_store.insert(safe_ptr<MockUiService>(&ui)));

            const auto scene_svc = ui_store.tryGet<MockSceneService>();
            PPR_TEST_ASSERT(scene_svc.isValid());
            PPR_TEST_ASSERT(scene_svc->value == 1);

            const auto ui_svc = ui_store.tryGet<MockUiService>();
            PPR_TEST_ASSERT(ui_svc.isValid());
            PPR_TEST_ASSERT(ui_svc->value == 2);

            PPR_TEST_ASSERT(not parent.tryGet<MockUiService>().isValid());
        };

        PPR_UNIT_TEST (child_erase_keeps_parent_visible) {
            MockSceneService scene;
            scene.value = 7;
            MockUiService ui;

            ServicesStore parent;
            PPR_TEST_ASSERT(parent.insert(safe_ptr<MockSceneService>(&scene)));

            ServicesStore ui_store{safe_ptr<ServicesStore>(&parent)};
            PPR_TEST_ASSERT(ui_store.insert(safe_ptr<MockUiService>(&ui)));
            PPR_TEST_ASSERT(ui_store.erase(ui));

            const auto scene_svc = ui_store.tryGet<MockSceneService>();
            PPR_TEST_ASSERT(scene_svc.isValid());
            PPR_TEST_ASSERT(scene_svc->value == 7);
        };
    }

    namespace ViewportGeometry {
        PPR_UNIT_TEST (layout_variant_covers_all_alternatives) {
            const PixelRect window{int2{100, 50}, int2{800, 600}};

            const Viewport full{window, ViewportLayout{}};
            PPR_TEST_ASSERT(full.getClientRect() == window);

            ViewportLayout centered{ViewportLayout::Centered{int2{400, 300}}};
            PPR_TEST_ASSERT(Viewport{window, centered}.getClientRect() == PixelRect{int2{300, 200}, int2{400, 300}});

            ViewportLayout::WindowRect fixed;
            fixed.m_origin = int2{120, 70};
            fixed.m_extent = int2{200, 100};
            PPR_TEST_ASSERT(Viewport{window, ViewportLayout{fixed}}.getClientRect() == PixelRect{int2{120, 70}, int2{200, 100}});

            ViewportLayout::NormalizedWindowRect normalized;
            normalized.m_origin = float2{0.25f, 0.25f};
            normalized.m_extent = float2{0.5f, 0.5f};
            const PixelRect client = Viewport{window, ViewportLayout{normalized}}.getClientRect();
            PPR_TEST_ASSERT(client.getWidth() == 400);
            PPR_TEST_ASSERT(client.getHeight() == 300);
            PPR_TEST_ASSERT(std::abs(static_cast<float>(client.m_origin.x) - 300.5f) <= 1.0f);
            PPR_TEST_ASSERT(std::abs(static_cast<float>(client.m_origin.y) - 200.5f) <= 1.0f);
        };

        PPR_UNIT_TEST (screen_client_transforms_are_inverse) {
            const PixelRect window{int2{100, 50}, int2{800, 600}};
            const Viewport full{window, ViewportLayout{}};
            PPR_TEST_ASSERT(full.getClientRect() == window);
            const int2 origin_client = full.screenToClient(int2{100, 50});
            PPR_TEST_ASSERT(origin_client.x == 0 && origin_client.y == 0);
            const int2 origin_screen = full.clientToScreen(int2{0, 0});
            PPR_TEST_ASSERT(origin_screen.x == 100 && origin_screen.y == 50);
            const int2 corner_client = full.screenToClient(int2{900, 650});
            PPR_TEST_ASSERT(corner_client.x == 800 && corner_client.y == 600);

            ViewportLayout::WindowRect fixed;
            fixed.m_origin = int2{140, 90};
            fixed.m_extent = int2{400, 300};
            const Viewport viewport{window, ViewportLayout{fixed}};
            const int2 fixed_client = viewport.screenToClient(int2{140, 90});
            PPR_TEST_ASSERT(fixed_client.x == 0 && fixed_client.y == 0);
            const int2 fixed_screen = viewport.clientToScreen(int2{0, 0});
            PPR_TEST_ASSERT(fixed_screen.x == 140 && fixed_screen.y == 90);

            const int2 client{17, 23};
            const int2 round_client = viewport.screenToClient(viewport.clientToScreen(client));
            PPR_TEST_ASSERT(round_client.x == client.x && round_client.y == client.y);
            const int2 screen{200, 150};
            const int2 round_screen = viewport.clientToScreen(viewport.screenToClient(screen));
            PPR_TEST_ASSERT(round_screen.x == screen.x && round_screen.y == screen.y);
        };

        PPR_UNIT_TEST (normalized_client_rect_round_trips) {
            const PixelRect window{int2{16, 16}, int2{800, 800}};
            ViewportLayout::NormalizedWindowRect rect;
            rect.m_origin = float2{0.500625f, 0.500625f};
            rect.m_extent = float2{0.5f, 0.5f};
            const Viewport viewport{window, ViewportLayout{rect}};
            const NormalizedRect back = viewport.getNormalizedClientRect();
            PPR_TEST_ASSERT(std::abs(back.m_origin.x - rect.m_origin.x) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(back.m_origin.y - rect.m_origin.y) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(back.m_extent.x - rect.m_extent.x) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(back.m_extent.y - rect.m_extent.y) < 1e-5f);
        };

        PPR_UNIT_TEST (zero_window_propagates_empty_client) {
            const Viewport empty{PixelRect{int2{100, 50}, int2{0, 0}}, ViewportLayout{}};
            PPR_TEST_ASSERT(empty.getClientRect().m_extent.x == 0 && empty.getClientRect().m_extent.y == 0);

            ViewportLayout centered{ViewportLayout::Centered{int2{400, 300}}};
            const Viewport minimized{PixelRect{int2{0, 0}, int2{800, 0}}, centered};
            PPR_TEST_ASSERT(minimized.getClientRect().m_extent.x == 0 && minimized.getClientRect().m_extent.y == 0);

            const Viewport degenerate = minimized;
            PPR_TEST_ASSERT(degenerate.getNormalizedClientRect().m_extent.x == 0.0f
                            && degenerate.getNormalizedClientRect().m_extent.y == 0.0f);
        };

        PPR_UNIT_TEST (window_viewport_tracks_shared_window) {
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}
            };
            {
                WindowViewport viewport{SharedWindow{&window}, ViewportLayout{}};
                PPR_TEST_ASSERT(viewport.getViewport().getClientRect() == PixelRect{int2{10, 20}, int2{800, 600}});
                PPR_TEST_ASSERT(not viewport.updateFromWindow());

                window.m_window_size = int2{1024, 768};
                PPR_TEST_ASSERT(viewport.updateFromWindow());
                PPR_TEST_ASSERT(viewport.getViewport().getClientRect() == PixelRect{int2{10, 20}, int2{1024, 768}});

                window.m_window_size = int2{0, 0};
                PPR_TEST_ASSERT(viewport.updateFromWindow());
                PPR_TEST_ASSERT(viewport.getViewport().getClientRect().m_extent.x == 0
                                && viewport.getViewport().getClientRect().m_extent.y == 0);
            }
            std::ignore = window.release();
        };
    }

    namespace RectContains {
        PPR_UNIT_TEST (point_truth_table_int) {
            const PixelRect rect{int2{10, 20}, int2{80, 60}};
            PPR_TEST_ASSERT(rect.contains(int2{10, 20}));
            PPR_TEST_ASSERT(rect.contains(int2{90, 80}));
            PPR_TEST_ASSERT(rect.contains(int2{50, 50}));
            PPR_TEST_ASSERT(rect.contains(int2{90, 20}));
            PPR_TEST_ASSERT(rect.contains(int2{10, 80}));
            PPR_TEST_ASSERT(not rect.contains(int2{9, 50}));
            PPR_TEST_ASSERT(not rect.contains(int2{91, 50}));
            PPR_TEST_ASSERT(not rect.contains(int2{50, 19}));
            PPR_TEST_ASSERT(not rect.contains(int2{50, 81}));
        };

        PPR_UNIT_TEST (point_truth_table_float) {
            const NormalizedRect rect{float2{0.25f, 0.25f}, float2{0.5f, 0.5f}};
            PPR_TEST_ASSERT(rect.contains(float2{0.25f, 0.25f}));
            PPR_TEST_ASSERT(rect.contains(float2{0.75f, 0.75f}));
            PPR_TEST_ASSERT(rect.contains(float2{0.5f, 0.5f}));
            PPR_TEST_ASSERT(not rect.contains(float2{0.24f, 0.5f}));
            PPR_TEST_ASSERT(not rect.contains(float2{0.76f, 0.5f}));
            PPR_TEST_ASSERT(not rect.contains(float2{0.5f, 0.24f}));
            PPR_TEST_ASSERT(not rect.contains(float2{0.5f, 0.76f}));
        };

        PPR_UNIT_TEST (rect_truth_table) {
            const PixelRect outer{int2{0, 0}, int2{100, 100}};
            PPR_TEST_ASSERT(outer.contains(outer));
            const PixelRect inner{int2{10, 10}, int2{20, 20}};
            PPR_TEST_ASSERT(outer.contains(inner));
            const PixelRect flush{int2{80, 10}, int2{20, 20}};
            PPR_TEST_ASSERT(outer.contains(flush));
            const PixelRect overlap{int2{90, 90}, int2{20, 20}};
            PPR_TEST_ASSERT(not outer.contains(overlap));
            const PixelRect disjoint{int2{200, 200}, int2{10, 10}};
            PPR_TEST_ASSERT(not outer.contains(disjoint));
        };

        PPR_UNIT_TEST (empty_and_inverted_reject) {
            const PixelRect empty{int2{5, 5}, int2{0, 0}};
            PPR_TEST_ASSERT(empty.contains(int2{5, 5}));
            PPR_TEST_ASSERT(not empty.contains(int2{6, 5}));
            const PixelRect elsewhere{int2{50, 50}, int2{0, 0}};
            PPR_TEST_ASSERT(not PixelRect{int2{0, 0}, int2{10, 10}}.contains(elsewhere));
            const PixelRect inverted{int2{10, 10}, int2{-4, -4}};
            PPR_TEST_ASSERT(not inverted.contains(int2{8, 8}));
            PPR_TEST_ASSERT(not inverted.contains(PixelRect{int2{7, 7}, int2{1, 1}}));
        };
    }

    namespace RectNormalize {
        PPR_UNIT_TEST (normalize_maps_corners_and_center) {
            const PixelRect rect{int2{10, 20}, int2{80, 60}};
            const float2 origin_uv = rect.normalize(int2{10, 20});
            PPR_TEST_ASSERT(origin_uv.x == 0.0f && origin_uv.y == 0.0f);
            const float2 max_uv = rect.normalize(int2{90, 80});
            PPR_TEST_ASSERT(max_uv.x == 1.0f && max_uv.y == 1.0f);
            const float2 center_uv = rect.normalize(int2{50, 50});
            PPR_TEST_ASSERT(center_uv.x == 0.5f && center_uv.y == 0.5f);
        };

        PPR_UNIT_TEST (normalize_denormalize_round_trip) {
            const NormalizedRect rect{float2{0.25f, 0.25f}, float2{0.5f, 0.5f}};
            const float2 point{0.4f, 0.6f};
            const float2 back = rect.denormalize(rect.normalize(point));
            PPR_TEST_ASSERT(std::abs(back.x - point.x) < 1e-5f && std::abs(back.y - point.y) < 1e-5f);
            const float2 clamped = rect.denormalizeClamp(float2{2.0f, -1.0f});
            PPR_TEST_ASSERT(std::abs(clamped.x - 0.75f) < 1e-5f && std::abs(clamped.y - 0.25f) < 1e-5f);
        };

        PPR_UNIT_TEST(normalize_zero_extent_fails, UnitTest::expect_crash) {
            if constexpr (PPR_ENABLE_DEBUG) {
                const PixelRect empty{int2{5, 5}, int2{0, 0}};
                std::ignore = empty.normalize(int2{5, 5});
            }
        };
    }

    PPR_UNIT_TEST (viewport){
        _.recurse({
            ProjectionConv::all_backends_use_one_projection,
            ViewportTypes::window_viewport_layout_switch_bumps_revision,
            ViewportTypes::camera_keeps_prior_state_on_zero_viewport,
            ServiceStores::child_store_shadows_parent,
            ServiceStores::child_erase_keeps_parent_visible,
            ViewportGeometry::layout_variant_covers_all_alternatives,
            ViewportGeometry::screen_client_transforms_are_inverse,
            ViewportGeometry::normalized_client_rect_round_trips,
            ViewportGeometry::zero_window_propagates_empty_client,
            ViewportGeometry::window_viewport_tracks_shared_window,
            RectContains::point_truth_table_int,
            RectContains::point_truth_table_float,
            RectContains::rect_truth_table,
            RectContains::empty_and_inverted_reject,
            RectNormalize::normalize_maps_corners_and_center,
            RectNormalize::normalize_denormalize_round_trip,
        });

        if constexpr (PPR_ENABLE_ASSERTIONS) {
            _.recurse({
                RectNormalize::normalize_zero_extent_fails,
            });

        }
    };
}
