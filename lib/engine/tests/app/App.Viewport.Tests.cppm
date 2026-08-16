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
        PPR_UNIT_TEST(device_type_mapping) {
            PPR_TEST_ASSERT(rhi::projectionConventionFromDeviceType(rhi::DeviceType::D3D11) == rhi::EProjectionConvention::D3D);
            PPR_TEST_ASSERT(rhi::projectionConventionFromDeviceType(rhi::DeviceType::D3D12) == rhi::EProjectionConvention::D3D);
            PPR_TEST_ASSERT(rhi::projectionConventionFromDeviceType(rhi::DeviceType::Default) == rhi::EProjectionConvention::D3D);
            PPR_TEST_ASSERT(rhi::projectionConventionFromDeviceType(rhi::DeviceType::Vulkan) == rhi::EProjectionConvention::VK);
            PPR_TEST_ASSERT(rhi::projectionConventionFromDeviceType(rhi::DeviceType::Metal) == rhi::EProjectionConvention::VK);
            PPR_TEST_ASSERT(rhi::projectionConventionFromDeviceType(rhi::DeviceType::WGPU) == rhi::EProjectionConvention::VK);
        };

        PPR_UNIT_TEST(unknown_device_type_falls_back_to_d3d) {
            const auto unknown = static_cast<rhi::DeviceType>(0x7F);
            PPR_TEST_ASSERT(rhi::projectionConventionFromDeviceType(unknown) == rhi::EProjectionConvention::D3D);
        };

        PPR_UNIT_TEST(constexpr_evaluable) {
            static_assert(rhi::projectionConventionFromDeviceType(rhi::DeviceType::Vulkan) == rhi::EProjectionConvention::VK);
        };
    }

    namespace OrthoMatrix {
        constexpr float kEps = 1e-4f;

        PPR_UNIT_TEST(d3d_y_down_depth_0_1) {
            const auto m = rhi::getOrthoMatrix(rhi::DeviceType::D3D12, 800.0f, 600.0f);

            PPR_TEST_ASSERT(std::abs(m(0, 0) - 2.0f / 800.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(m(1, 1) - (-2.0f / 600.0f)) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 0) + 1.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 1) - 1.0f) < kEps);

            PPR_TEST_ASSERT(std::abs(m(2, 2) - 0.5f) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 2) - 0.5f) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 3) - 1.0f) < kEps);
        };

        PPR_UNIT_TEST(vk_y_down_depth_minus1_1) {
            const auto m = rhi::getOrthoMatrix(rhi::DeviceType::Vulkan, 800.0f, 600.0f);

            PPR_TEST_ASSERT(std::abs(m(0, 0) - 2.0f / 800.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(m(1, 1) - (-2.0f / 600.0f)) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 0) + 1.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 1) - 1.0f) < kEps);

            PPR_TEST_ASSERT(std::abs(m(2, 2) + 0.5f) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 2)) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 3) - 1.0f) < kEps);
        };

        PPR_UNIT_TEST(conventions_share_xy_mapping) {
            const auto d3d = rhi::getOrthoMatrix(rhi::DeviceType::D3D12, 800.0f, 600.0f);
            const auto vk = rhi::getOrthoMatrix(rhi::DeviceType::Vulkan, 800.0f, 600.0f);

            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 4; ++col) {
                    const bool depth_mapping = (row == 2 && col >= 2) || (row == 3 && col == 2);
                    if (depth_mapping) {
                        continue;
                    }
                    PPR_TEST_ASSERT(std::abs(d3d(row, col) - vk(row, col)) < kEps);
                }
            }
        };
    }

    namespace PerspectiveMatrix {
        constexpr float kEps = 1e-4f;
        constexpr float kPi = 3.14159265358979323846f;

        PPR_UNIT_TEST(d3d_depth_range) {
            constexpr float fov = kPi / 4.0f;
            const auto m = rhi::getPerspectiveMatrix(rhi::DeviceType::D3D12, fov, 16.0f / 9.0f, 0.1f, 1000.0f);

            const float y_scale = 1.0f / std::tan(fov * 0.5f);
            PPR_TEST_ASSERT(std::abs(m(1, 1) - y_scale) < kEps);
            PPR_TEST_ASSERT(m(0, 0) < m(1, 1));

            const float a = 1000.0f / (1000.0f - 0.1f);
            PPR_TEST_ASSERT(std::abs(m(2, 2) - a) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 2) + a * 0.1f) < kEps);
            PPR_TEST_ASSERT(std::abs(m(2, 3) - 1.0f) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 3)) < kEps);
        };

        PPR_UNIT_TEST(vk_depth_range_y_flip) {
            constexpr float fov = kPi / 4.0f;
            const auto m = rhi::getPerspectiveMatrix(rhi::DeviceType::Vulkan, fov, 16.0f / 9.0f, 0.1f, 1000.0f);

            const float y_scale = 1.0f / std::tan(fov * 0.5f);
            PPR_TEST_ASSERT(std::abs(m(1, 1) + y_scale) < kEps);

            const float c = (1000.0f + 0.1f) / (0.1f - 1000.0f) * 0.5f;
            const float d = 2.0f * 0.1f * 1000.0f / (0.1f - 1000.0f) * 0.5f;
            PPR_TEST_ASSERT(std::abs(m(2, 2) - c) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 2) - d) < kEps);
            PPR_TEST_ASSERT(std::abs(m(3, 3) - d) < kEps);
        };

        PPR_UNIT_TEST(conventions_share_x_scale) {
            constexpr float fov = kPi / 4.0f;
            const auto d3d = rhi::getPerspectiveMatrix(rhi::DeviceType::D3D12, fov, 16.0f / 9.0f, 0.1f, 1000.0f);
            const auto vk = rhi::getPerspectiveMatrix(rhi::DeviceType::Vulkan, fov, 16.0f / 9.0f, 0.1f, 1000.0f);

            PPR_TEST_ASSERT(std::abs(d3d(0, 0) - vk(0, 0)) < kEps);
        };
    }

    namespace ViewportTypes {
        static_assert(std::is_copy_constructible_v<ViewportConfig>);
        static_assert(sizeof(ViewportConfig) == sizeof(int2));

        PPR_UNIT_TEST(config_plain_data) {
            ViewportConfig config;
            PPR_TEST_ASSERT(config.framebuffer_size.x == 0);
            PPR_TEST_ASSERT(config.framebuffer_size.y == 0);

            config.framebuffer_size = int2(1920, 1080);
            const ViewportConfig copy = config;
            PPR_TEST_ASSERT(copy.framebuffer_size == config.framebuffer_size);
        };

        PPR_UNIT_TEST(entry_aggregate_init_and_draw) {
            int calls = 0;
            const auto draw = [&calls](rhi::IRenderPassEncoder &, const rhi::Viewport &, const rhi::ScissorRect &) -> std::error_code {
                ++calls;
                return default_value_v;
            };
            const ViewportEntry entry{
                .viewport = rhi::Viewport{0.0f, 0.0f, 800.0f, 600.0f, 0.0f, 1.0f},
                .scissor = rhi::ScissorRect{0, 0, 800, 600},
                .draw = draw,
            };

            PPR_TEST_ASSERT(entry.pipeline.get() == nullptr);
            PPR_TEST_ASSERT(entry.viewport.extentX == 800.0f);
            PPR_TEST_ASSERT(entry.scissor.maxX == 800);

            auto *const dummy_pass = static_cast<rhi::IRenderPassEncoder *>(nullptr);
            const rhi::Viewport vp = entry.viewport;
            const rhi::ScissorRect sc = entry.scissor;
            const auto ec = entry.draw(*dummy_pass, vp, sc);
            PPR_TEST_ASSERT(!ec);
            PPR_TEST_ASSERT(calls == 1);
        };
    }

    namespace ViewportServices {
        struct MockSceneService : IService {
            int value{};
        };

        struct MockUiService : IService {
            int value{};
        };

        PPR_UNIT_TEST(child_store_shadows_parent) {
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

        PPR_UNIT_TEST(child_erase_keeps_parent_visible) {
            MockSceneService scene;
            scene.value = 7;
            MockUiService ui;

            ServicesStore parent;
            PPR_TEST_ASSERT(parent.insert(safe_ptr<MockSceneService>(&scene)));

            ServicesStore ui_store{safe_ptr<ServicesStore>(&parent)};
            PPR_TEST_ASSERT(ui_store.insert(safe_ptr<MockUiService>(&ui)));
            PPR_TEST_ASSERT(ui_store.erase<MockUiService>());

            const auto scene_svc = ui_store.tryGet<MockSceneService>();
            PPR_TEST_ASSERT(scene_svc.isValid());
            PPR_TEST_ASSERT(scene_svc->value == 7);
        };
    }

    PPR_UNIT_TEST(app_viewport) {
        _.recurse({
            ProjectionConv::device_type_mapping,
            ProjectionConv::unknown_device_type_falls_back_to_d3d,
            ProjectionConv::constexpr_evaluable,
            OrthoMatrix::d3d_y_down_depth_0_1,
            OrthoMatrix::vk_y_down_depth_minus1_1,
            OrthoMatrix::conventions_share_xy_mapping,
            PerspectiveMatrix::d3d_depth_range,
            PerspectiveMatrix::vk_depth_range_y_flip,
            PerspectiveMatrix::conventions_share_x_scale,
            ViewportTypes::config_plain_data,
            ViewportTypes::entry_aggregate_init_and_draw,
            ViewportServices::child_store_shadows_parent,
            ViewportServices::child_erase_keeps_parent_visible,
        });
    };
}
