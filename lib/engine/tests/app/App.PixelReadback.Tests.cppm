module;

#include "pP/UnitTest.h"

export module engine.tests.app:pixel_readback;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import engine.app;
import imgui_internal;
import std;

// The `imgui` module re-exports ImGui names unqualified; bring them into scope here so the
// qualified `ImGui::` calls (SetCurrentContext/ShowDemoWindow) and `ImGuiContext` resolve.
using namespace ImGui;

export namespace pP::tests {
    namespace detail {
        constexpr u32 kTargetSize = 256;
        // ImGui's demo window spawns at (650, 20) with a 550x680 size (SetNextWindowPos
        // ImGuiCond_FirstUseEver in imgui_demo.cpp), so the UI readback target must be
        // large enough to contain it.
        constexpr u32 kUiWidth = 1280;
        constexpr u32 kUiHeight = 720;

        std::filesystem::path findAssetsDir() {
            auto dir = std::filesystem::current_path();
            for (int i = 0; i < 8; ++i) {
                if (std::filesystem::exists(dir / "assets" / "shaders" / "triangle.slang")) {
                    return dir / "assets";
                }
                if (not dir.has_parent_path()) {
                    break;
                }
                dir = dir.parent_path();
            }
            return {};
        }

        struct TestApp : Application {
            using Application::Application;
            [[nodiscard]] std::error_code boot() { return Application::initialize(); }
            [[nodiscard]] std::error_code teardown() { return Application::shutdown(); }
        };

        // Maps a pixel's raw bytes to a logical channel (0=R,1=G,2=B) honoring the
        // texture format's in-memory byte order (RGBA vs BGRA).
        auto channelReader(rhi::Format format) {
            const bool is_bgra = (format == rhi::Format::BGRA8Unorm) || (format == rhi::Format::BGRA8UnormSrgb);
            return [is_bgra](const std::uint8_t *px, int channel) -> float {
                int idx = channel;
                if (is_bgra) {
                    idx = (channel == 0) ? 2 : (channel == 1) ? 1 : 0;
                }
                return static_cast<float>(px[idx]) / 255.0f;
            };
        }
    }

    PPR_UNIT_TEST(app_pixel_readback) {
        using namespace detail;

        TestApp app{"PixelReadback", std::span<const char * const>{}};

        PPR_TEST_ASSERT(app.boot().value() == 0);

        PPR_DEFER {
            (void) app.teardown();
        };

        const auto rhi_service = app.getServices().get<IRhiService>();
        PPR_TEST_ASSERT(!!rhi_service);

        const auto window_service = app.getServices().get<IWindowService>();
        PPR_TEST_ASSERT(!!window_service);

        const auto input_service = app.getServices().get<IInputService>();
        PPR_TEST_ASSERT(!!input_service);

        const auto assets = findAssetsDir();
        PPR_TEST_ASSERT(not assets.empty());

        auto offscreen_window = window_service->createWindow(WindowModel{
            .m_window_size = int2{static_cast<int>(kTargetSize), static_cast<int>(kTargetSize)},
            .m_visible = false,
        });

        PPR_TEST_ASSERT(!!offscreen_window);

        PPR_DEFER {
            if (offscreen_window) {
                (void) window_service->destroyWindow(std::move(*offscreen_window));
            }
        };

        Renderer renderer;
        PPR_TEST_ASSERT(renderer.initialize(*rhi_service, *window_service, **offscreen_window, assets).value() == 0);

        const rhi::Format format = renderer.getSurfaceFormat();
        rhi::IDevice &device = rhi_service->getDevice();

        const auto makeTarget = [&](u32 w, u32 h) -> rhi::ComPtr<rhi::ITexture> {
            rhi::ComPtr<rhi::ITexture> tex;
            rhi::TextureDesc tex_desc{};
            tex_desc.type = rhi::TextureType::Texture2D;
            tex_desc.format = format;
            tex_desc.size = {w, h, 1};
            tex_desc.mipCount = 1;
            tex_desc.sampleCount = 1;
            tex_desc.usage = static_cast<rhi::TextureUsage>(
                static_cast<std::uint32_t>(rhi::TextureUsage::RenderTarget) |
                static_cast<std::uint32_t>(rhi::TextureUsage::CopySource));
            tex_desc.defaultState = rhi::ResourceState::RenderTarget;
            tex_desc.label = "pixel readback target";
            if (const auto rc = pP::rhi::result(device.createTexture(tex_desc, nullptr, tex.writeRef())); rc) {
                _.logFmt("skipping app_pixel_readback: offscreen texture creation failed");
                return nullptr;
            }
            return tex;
        };

        const auto readback = [&](rhi::ITexture *tex, u32 w, u32 h) -> std::vector<std::uint8_t> {
            rhi::SubresourceLayout layout{};
            layout.size = rhi::Extent3D{w, h, 1u};
            layout.colPitch = 4;
            layout.rowPitch = static_cast<size_t>(w) * 4u;
            layout.slicePitch = layout.rowPitch * h;
            layout.sizeInBytes = layout.slicePitch;
            layout.blockWidth = 1;
            layout.blockHeight = 1;
            layout.rowCount = h;

            std::vector<std::uint8_t> buf(layout.sizeInBytes);
            if (const auto rc = pP::rhi::result(device.readTexture(tex, 0, 0, layout, buf.data())); rc) {
                _.logFmt("skipping app_pixel_readback: texture readback failed");
                return {};
            }
            return buf;
        };

        const auto full_viewport = rhi::Viewport{
            0.0f, 0.0f,
            static_cast<float>(kTargetSize), static_cast<float>(kTargetSize),
            0.0f, 1.0f
        };
        const auto full_scissor = rhi::ScissorRect{
            0, 0,
            kTargetSize, kTargetSize
        };

        // ---- Triangle pass ----
        auto target = makeTarget(kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(!!target);

        // Use a real perspective camera (the 4-arg path VideoGameApp actually uses)
        // so this test faithfully reproduces the production transform instead of the
        // identity-camera 3-arg overload, which never exercises Camera::viewProjection().
        Camera scene_camera;
        {
            const float4x4 view = lookAt(float3{0.0f, 0.0f, 5.0f}, float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 1.0f, 0.0f});
            const float4x4 proj = rhi::getPerspectiveMatrix(rhi::DeviceType::D3D12, 60.0f * std::numbers::pi_v<float> / 180.0f, 1.0f, 0.1f, 1000.0f);
            scene_camera.setView(view);
            scene_camera.setProjection(proj);
        }

        const auto scene_draw = [&renderer, &scene_camera](rhi::IRenderPassEncoder &pass, const rhi::Viewport &viewport, const rhi::ScissorRect &scissor) -> std::error_code {
            return renderer.drawTriangle(pass, scene_camera, viewport, scissor);
        };
        const ViewportEntry scene_entry{
            .viewport = full_viewport,
            .scissor = full_scissor,
            .draw = scene_draw,
        };

        PPR_TEST_ASSERT(renderer.renderInto(*target, std::span{&scene_entry, 1}).value() == 0);

        auto data = readback(target.get(), kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(not data.empty());
        const u32 row_pitch = kTargetSize * 4u;

        const auto read_channel = channelReader(format);
        const auto pixel = [&](u32 x, u32 y) -> const std::uint8_t * {
            return data.data() + static_cast<size_t>(y) * row_pitch + static_cast<size_t>(x) * 4u;
        };

        const auto is_dominant = [&](u32 x, u32 y, int channel) -> bool {
            return read_channel(pixel(x, y), channel) > 0.5f;
        };
        const auto is_recessive = [&](u32 x, u32 y, int channel) -> bool {
            return read_channel(pixel(x, y), channel) < 0.4f;
        };

        if (not is_dominant(128u, 106u, 0) || not is_recessive(128u, 106u, 1) || not is_recessive(128u, 106u, 2)) {
            _.log(("app_pixel_readback: expected RED at (128,106), got r=" + std::to_string(read_channel(pixel(128u, 106u), 0))
                   + " g=" + std::to_string(read_channel(pixel(128u, 106u), 1))
                   + " b=" + std::to_string(read_channel(pixel(128u, 106u), 2)))
                .c_str());
            PPR_TEST_ASSERT(false);
        }
        if (not is_dominant(106u, 150u, 1) || not is_recessive(106u, 150u, 0) || not is_recessive(106u, 150u, 2)) {
            _.log(("app_pixel_readback: expected GREEN at (106,150), got r=" + std::to_string(read_channel(pixel(106u, 150u), 0))
                   + " g=" + std::to_string(read_channel(pixel(106u, 150u), 1))
                   + " b=" + std::to_string(read_channel(pixel(106u, 150u), 2)))
                .c_str());
            PPR_TEST_ASSERT(false);
        }
        if (not is_dominant(150u, 150u, 2) || not is_recessive(150u, 150u, 0) || not is_recessive(150u, 150u, 1)) {
            _.log(("app_pixel_readback: expected BLUE at (150,150), got r=" + std::to_string(read_channel(pixel(150u, 150u), 0))
                   + " g=" + std::to_string(read_channel(pixel(150u, 150u), 1))
                   + " b=" + std::to_string(read_channel(pixel(150u, 150u), 2)))
                .c_str());
            PPR_TEST_ASSERT(false);
        }
        if (is_dominant(250u, 128u, 0) || is_dominant(250u, 128u, 1) || is_dominant(250u, 128u, 2)) {
            _.log(("app_pixel_readback: expected background (no primary) at far-right, got r=" + std::to_string(read_channel(pixel(250u, 128u), 0))
                   + " g=" + std::to_string(read_channel(pixel(250u, 128u), 1))
                   + " b=" + std::to_string(read_channel(pixel(250u, 128u), 2)))
                .c_str());
            PPR_TEST_ASSERT(false);
        }

        // ---- UI overlay pass (demo window) ----
        auto ui_svc = ui::createImGuiService();
        if (ui_svc) {
            if (auto ec = ui_svc->initialize(*rhi_service, *window_service, *input_service, **offscreen_window, format); not ec) {
                // The demo window spawns at (650, 20) — outside any 256x256 viewport — so
                // the UI framebuffer must be sized to contain it. Run two full frames
                // (newFrame + renderOverlay each, since ImGui requires Render() every
                // frame, and DeltaTime must be positive after the first frame); the demo
                // window's first-use layout settles by the second frame, which is the one
                // read back below.
                (void) ui_svc->onResize(int2{static_cast<int>(kUiWidth), static_cast<int>(kUiHeight)});
                const pP::TimeSpan kFrame = std::chrono::milliseconds{16};
                const float2 ui_size{static_cast<float>(kUiWidth), static_cast<float>(kUiHeight)};
                const auto spawn_demo_window = [&ui_svc]() {
                    ImGui::SetCurrentContext(static_cast<ImGuiContext *>(ui_svc->getContext()));
                    static bool g_show_demo_window{true};
                    ImGui::ShowDemoWindow(&g_show_demo_window);
                };
                const auto ui_draw = [&ui_svc, ui_size](rhi::IRenderPassEncoder &pass, const rhi::Viewport &, const rhi::ScissorRect &) -> std::error_code {
                    return ui_svc->renderOverlay(pass, ui_size);
                };
                const ViewportEntry ui_entry{
                    .viewport = rhi::Viewport{
                        0.0f, 0.0f,
                        static_cast<float>(kUiWidth), static_cast<float>(kUiHeight),
                        0.0f, 1.0f
                    },
                    .scissor = rhi::ScissorRect{0, 0, kUiWidth, kUiHeight},
                    .draw = ui_draw,
                };

                auto scratch = makeTarget(kUiWidth, kUiHeight);
                auto ui_target = makeTarget(kUiWidth, kUiHeight);
                if (scratch && ui_target) {
                    PPR_TEST_ASSERT(not hasFailed(ui_svc->newFrame(kFrame)));
                    spawn_demo_window();

                    if (auto rc = renderer.renderInto(*scratch, std::span{&ui_entry, 1}); not rc) {
                        PPR_TEST_ASSERT(not hasFailed(ui_svc->newFrame(kFrame)));
                        spawn_demo_window();

                        if (auto rc2 = renderer.renderInto(*ui_target, std::span{&ui_entry, 1}); not rc2) {
                            auto ui_data = readback(ui_target.get(), kUiWidth, kUiHeight);
                            PPR_TEST_ASSERT(not ui_data.empty());

                            if (not ui_data.empty()) {
                                constexpr u32 ui_row = kUiWidth * 4u;
                                const auto read_ui = channelReader(format);

                                bool found_non_background = false;
                                for (u32 y = 0; y < kUiHeight && not found_non_background; y += 4u) {
                                    for (u32 x = 0; x < kUiWidth; x += 4u) {
                                        const auto *px = ui_data.data() + static_cast<size_t>(y) * ui_row + static_cast<size_t>(x) * 4u;
                                        const float r = read_ui(px, 0);
                                        const float g = read_ui(px, 1);
                                        const float b = read_ui(px, 2);
                                        if (r > 0.5f || g > 0.5f || b > 0.5f) {
                                            found_non_background = true;
                                            break;
                                        }
                                    }
                                }

                                if (not found_non_background) {
                                        _.logFmt("app_pixel_readback: UI overlay rendered nothing (expected demo window)");
                                        PPR_TEST_ASSERT(false);
                                    }
                            }
                        }
                    }
                }
            }
        }
    };
}
