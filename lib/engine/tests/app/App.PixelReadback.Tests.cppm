module;

#include "pP/UnitTest.h"

export module engine.tests.app:pixel_readback;

import engine.core;
import engine.math;
import engine.rhi;
import engine.app;
import std;

export namespace pP::tests {
    namespace detail {
        constexpr u32 kTargetSize = 256;

        std::filesystem::path findAssetsDir() {
            auto dir = std::filesystem::current_path();
            for (int i = 0; i < 8; ++i) {
                if (std::filesystem::exists(dir / "assets" / "shaders" / "triangle.slang")) {
                    return dir / "assets";
                }
                if (not
                    dir.has_parent_path())
                {
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

        CameraSnapshot makeSnapshot(const float3 &eye, const float3 &target, const float aspect) {
            CameraSnapshot snapshot{};
            snapshot.m_view = float4x4::lookat(target, eye, float3{0.0f, 1.0f, 0.0f});
            snapshot.m_projection = rhi::getPerspectiveMatrix(
                60.0f * std::numbers::pi_v<float> / 180.0f, aspect, 0.1f, 1000.0f);
            snapshot.m_view_projection = snapshot.m_view * snapshot.m_projection;
            snapshot.m_invert_view_projection = inverse(snapshot.m_view_projection);
            snapshot.m_origin = eye;
            snapshot.m_viewport_size = float2{static_cast<float>(kTargetSize), static_cast<float>(kTargetSize)};
            snapshot.m_revision = 1;
            return snapshot;
        }

        RenderView fullView(const u32 left, const u32 top, const u32 right, const u32 bottom) {
            RenderView view{};
            view.m_viewport = rhi::Viewport{
                static_cast<float>(left), static_cast<float>(top),
                static_cast<float>(right - left), static_cast<float>(bottom - top), 0.0f, 1.0f
            };
            view.m_scissor = rhi::ScissorRect{left, top, right, bottom};
            return view;
        }
    }

    PPR_UNIT_TEST(pixel_readback) {
        using namespace detail;

        TestApp app{"PixelReadback", std::span<const char * const>{}};

        PPR_TEST_ASSERT(app.boot().value() == 0);

        PPR_DEFER{
            PPR_TEST_ASSERT(app.teardown().value() == 0);


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

        PPR_DEFER{
            if (offscreen_window) {
                (void) window_service->destroyWindow(std::move(*offscreen_window));


            }
        };

        Renderer renderer;
        PPR_TEST_ASSERT(renderer.initialize(*rhi_service).value() == 0);

        PPR_DEFER{
            PPR_TEST_ASSERT(renderer.shutdown().value() == 0);

        };

        TrianglePass triangle;
        PPR_TEST_ASSERT(triangle.initialize(*rhi_service, assets).value() == 0);

        PPR_DEFER{
            PPR_TEST_ASSERT(triangle.shutdown().value() == 0);

        };

        // Hidden-window surface lifecycle: create + resolve the format used
        // for offscreen targets below.
        const WindowHandle surface_handle = (**offscreen_window).m_handle;
        PPR_TEST_ASSERT(renderer.createWindowSurface(*window_service, **offscreen_window).value() == 0);
        rhi::Format format = renderer.getWindowSurfaceFormat(surface_handle);
        if (format == rhi::Format::Undefined) {
            format = rhi::Format::RGBA8Unorm;
        }

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
                _.logFmt("skipping pixel_readback: offscreen texture creation failed");
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
                _.logFmt("skipping pixel_readback: texture readback failed");
                return {};
            }
            return buf;
        };

        const auto has_primary_color = [&](const std::vector<std::uint8_t> &pixels,
                                           const u32 left, const u32 top, const u32 right, const u32 bottom) {
            const auto read = channelReader(format);
            return std::ranges::any_of(std::views::iota(top, bottom), [&](const u32 y) {
                return std::ranges::any_of(std::views::iota(left, right), [&](const u32 x) {
                    const auto *const px = pixels.data() + static_cast<size_t>(y) * kTargetSize * 4u
                                           + static_cast<size_t>(x) * 4u;
                    return read(px, 0) > 0.5f;
                });
            });
        };

        const ColorPassOptions options{};

        // ---- Clear-only submission (empty span) ----
        auto clear_target = makeTarget(kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(!!clear_target);
        PPR_TEST_ASSERT(renderer.submitToTexture(*clear_target, std::span<const DrawSubmission>{}, options).value() == 0);
        PPR_TEST_ASSERT(renderer.waitForIdle().value() == 0);
        const auto clear_data = readback(clear_target.get(), kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(not clear_data.empty());
        PPR_TEST_ASSERT(not has_primary_color(clear_data, 0, 0, kTargetSize, kTargetSize));

        // ---- Single triangle submission via TrianglePass ----
        const CameraSnapshot snapshot = makeSnapshot(float3{0.0f, 0.0f, 5.0f}, float3{0.0f, 0.0f, 0.0f}, 1.0f);
        const SceneView scene_view{snapshot, fullView(32, 16, 160, 144)};
        const auto encode_triangle = [&](rhi::IRenderPassEncoder &pass, const DrawContext &ctx) -> std::error_code {
            return triangle.draw(pass, scene_view, ctx.m_target);
        };
        const DrawSubmission triangle_submission{
            .m_view = scene_view.m_render_view,
            .m_encode_draws = DrawCallback{encode_triangle}
        };
        const std::array<DrawSubmission, 1> single{triangle_submission};
        auto client_target = makeTarget(kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(!!client_target);
        PPR_TEST_ASSERT(renderer.submitToTexture(*client_target, single, options).value() == 0);
        PPR_TEST_ASSERT(renderer.waitForIdle().value() == 0);
        const auto client_data = readback(client_target.get(), kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(not client_data.empty());
        PPR_TEST_ASSERT(has_primary_color(client_data, 32, 16, 160, 144));

        // ---- Two submissions with disjoint scissors ----
        const CameraSnapshot second_snapshot = makeSnapshot(float3{0.5f, 0.0f, 5.0f}, float3{0.5f, 0.0f, 0.0f}, 0.5f);
        const SceneView second_scene{second_snapshot, fullView(128, 0, 256, 256)};
        const auto encode_second = [&](rhi::IRenderPassEncoder &pass, const DrawContext &ctx) -> std::error_code {
            return triangle.draw(pass, second_scene, ctx.m_target);
        };
        const std::array<DrawSubmission, 2> two_submissions{
            triangle_submission,
            DrawSubmission{.m_view = second_scene.m_render_view, .m_encode_draws = DrawCallback{encode_second}},
        };
        auto two_client_target = makeTarget(kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(!!two_client_target);
        PPR_TEST_ASSERT(renderer.submitToTexture(*two_client_target, two_submissions, options).value() == 0);
        PPR_TEST_ASSERT(renderer.waitForIdle().value() == 0);
        const auto two_client_data = readback(two_client_target.get(), kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(not two_client_data.empty());
        PPR_TEST_ASSERT(has_primary_color(two_client_data, 32, 16, 160, 144));
        PPR_TEST_ASSERT(has_primary_color(two_client_data, 128, 0, 256, 256));

        // ---- Callback-error recovery: failing submit, then clean submit ----
        u32 failing_calls = 0;
        const auto encode_failing = [&](rhi::IRenderPassEncoder &, const DrawContext &) -> std::error_code {
            ++failing_calls;
            return std::make_error_code(std::errc::invalid_argument);
        };
        const std::array<DrawSubmission, 1> failing{
            DrawSubmission{.m_view = fullView(0, 0, kTargetSize, kTargetSize), .m_encode_draws = DrawCallback{encode_failing}},
        };
        auto error_target = makeTarget(kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(!!error_target);
        PPR_TEST_ASSERT(renderer.submitToTexture(*error_target, failing, options).value() != 0);
        PPR_TEST_ASSERT(failing_calls == 1);
        PPR_TEST_ASSERT(renderer.submitToTexture(*error_target, std::span<const DrawSubmission>{}, options).value() == 0);
        PPR_TEST_ASSERT(renderer.waitForIdle().value() == 0);
        const auto error_data = readback(error_target.get(), kTargetSize, kTargetSize);
        PPR_TEST_ASSERT(not error_data.empty());
        PPR_TEST_ASSERT(not has_primary_color(error_data, 0, 0, kTargetSize, kTargetSize));

        PPR_TEST_ASSERT(renderer.destroyWindowSurface(surface_handle).value() == 0);
        PPR_TEST_ASSERT(renderer.getWindowSurfaceFormat(surface_handle) == rhi::Format::Undefined);
    };
}
