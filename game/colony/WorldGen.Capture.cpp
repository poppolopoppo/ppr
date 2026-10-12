module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module game.colony.smoke;

import engine.app;
import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import engine.sim;
import game.colony.driver;
import game.colony.metric;
import game.colony.sim_packet;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(WorldCapture, info, none);
    namespace {
        constexpr u32 kCaptureEdge = 512u;
        struct CaptureView {
            std::string_view m_name;
            float m_x{}, m_y{}, m_width{};
        };

        [[nodiscard]] std::error_code writeReadback(
            rhi::IDevice &device, rhi::ITexture &target, const std::filesystem::path &path) {
            shader::ComPtr<ISlangBlob> blob{};
            rhi::SubresourceLayout layout{};
            PPR_RETURN_ERROR_ON_FAIL(
                WorldCapture, make_error_code(device.readTexture(&target, 0u, 0u, blob.writeRef(), &layout)));
            const std::size_t row_bytes = kCaptureEdge * 4u;
            if (not blob or
                layout.rowPitch < row_bytes or
                blob->getBufferSize() < (kCaptureEdge - 1u) * layout.rowPitch + row_bytes) {
                PPR_RETURN_ERROR_ON_FAIL(WorldCapture, std::errc::protocol_error);
            }
            std::ofstream file{path, std::ios::binary | std::ios::trunc};
            PPR_RETURN_ERROR_ON_FAIL(
                WorldCapture, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
            file << "P6\n" << kCaptureEdge << ' ' << kCaptureEdge << "\n255\n";
            const auto *const data = static_cast<const u8 *>(blob->getBufferPointer());
            std::array<char, kCaptureEdge * 3u> row{};
            for (u32 y = 0u; y < kCaptureEdge; ++y) {
                for (u32 x = 0u; x < kCaptureEdge; ++x) {
                    for (u32 c = 0u; c < 3u; ++c) {
                        row[x * 3u + c] = static_cast<char>(data[y * layout.rowPitch + x * 4u + c]);
                    }
                }
                file.write(row.data(), row.size());
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldCapture, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
            }
            file.close();
            PPR_RETURN_ERROR_ON_FAIL(
                WorldCapture, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
            return {};
        }

        class WorldCaptureApp final : public Application {
            std::string m_identity;
            std::filesystem::path m_directory;
            std::unique_ptr<ColonyDriver> m_driver;
            GridPass m_pass{};
            rhi::ComPtr<rhi::ITexture> m_target{};
            std::array<CaptureView, 4u> m_views{};
            std::error_code m_shutdown_error{};
            bool m_base_started{}, m_base_ready{}, m_pass_started{}, m_complete{};

        public:
            WorldCaptureApp(
                std::unique_ptr<ColonyDriver> driver, const std::array<CaptureView, 4u> views, std::string identity,
                std::filesystem::path directory)
                : Application(
                      ApplicationDomain{
                          .m_is_headless = true,
                          .m_is_interactive = false,
                          .m_needs_presence = false,
                          .m_needs_rendering = true,
                          .m_needs_user_interface = false},
                      "worldgen-capture", std::span<const char *const>{}),
                  m_identity(std::move(identity)), m_directory(std::move(directory)), m_driver(std::move(driver)),
                  m_views(views) {
            }

            [[nodiscard]] bool complete() const noexcept {
                return m_complete;
            }
            [[nodiscard]] std::error_code shutdownError() const noexcept {
                return m_shutdown_error;
            }

        protected:
            [[nodiscard]] std::error_code initialize() override {
                m_base_started = true;
                PPR_RETURN_ERROR_ON_FAIL(WorldCapture, Application::initialize());
                m_base_ready = true;
                const auto rhi_service = getServices().tryGet<IRhiService>();
                const auto shader_service = getServices().tryGet<IShaderService>();
                if (not rhi_service or not shader_service) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldCapture, std::errc::not_connected);
                }
                std::error_code directory_error{};
                std::filesystem::create_directories(m_directory, directory_error);
                PPR_RETURN_ERROR_ON_FAIL(WorldCapture, directory_error);

                m_pass_started = true;
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldCapture, m_pass.initialize(*rhi_service, *shader_service, getContentDir().path()));
                auto stage = captureWorldPresentation(*m_driver);
                PPR_RETURN_ERROR_ON_FAIL(WorldCapture, stage);
                PPR_RETURN_ERROR_ON_FAIL(WorldCapture, m_pass.applyStagedFrame(std::move(*stage)));
                return {};
            }

            [[nodiscard]] std::error_code update(const TimeSpan dt) override {
                // Frozen generation baseline: no simulation tick or held input enters a capture.
                return Application::update(dt);
            }

            [[nodiscard]] std::error_code render() override {
                PPR_RETURN_ERROR_ON_FAIL(WorldCapture, Application::render());
                const auto service = getServices().tryGet<IRhiService>();
                if (not service) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldCapture, std::errc::not_connected);
                }
                auto &device = service->getDevice();
                auto &renderer = getRenderer();
                // Explicit blocking diagnostic host, never used by interactive publication.
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldCapture,
                    m_pass.prepareCellUploads(device, [&renderer] { return renderer.waitOnHost(); }, true));
                rhi::TextureDesc desc{};
                desc.type = rhi::TextureType::Texture2D;
                desc.size = {kCaptureEdge, kCaptureEdge, 1u};
                desc.arrayLength = 1u;
                desc.mipCount = 1u;
                desc.format = rhi::Format::RGBA8Unorm;
                desc.memoryType = rhi::MemoryType::DeviceLocal;
                desc.usage = rhi::TextureUsage::RenderTarget;
                desc.defaultState = rhi::ResourceState::RenderTarget;
                desc.label = "worldgen closeup";
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldCapture, make_error_code(device.createTexture(desc, nullptr, m_target.writeRef())));

                std::ofstream manifest{m_directory / "gpu-cameras.txt", std::ios::trunc};
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldCapture, manifest ? std::error_code{} : std::make_error_code(std::errc::io_error));
                manifest << m_identity;
                const auto &capture_clock = ITimerClock::steady();
                for (const auto &view: m_views) {
                    const auto submission_begin = capture_clock.now();
                    CameraModel model{};
                    model.m_camera_mode = ECameraProjection::orthographic;
                    model.m_ortho_scale = view.m_width / kCaptureEdge;
                    model.m_origin = float3{view.m_x - view.m_width * 0.5f, view.m_y - view.m_width * 0.5f, -0.5f};
                    Camera camera{ECameraProjection::orthographic};
                    camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, kCaptureEdge, kCaptureEdge}});
                    PPR_RETURN_ERROR_ON_FAIL(WorldCapture, m_pass.update(TimeSpan{}, camera.getSnapshot()));
                    const auto snapshot = camera.getSnapshot();
                    const auto viewport = rhi::Viewport::fromSize(float(kCaptureEdge), float(kCaptureEdge));
                    const auto scissor = rhi::ScissorRect::fromSize(kCaptureEdge, kCaptureEdge);
                    const auto prepare = [&](const PreRenderContext &context) {
                        return m_pass.prepareBackground(context, snapshot, viewport, scissor);
                    };
                    const auto draw = [&](const DrawContext &context) -> std::error_code {
                        PPR_RETURN_ERROR_ON_FAIL(WorldCapture, m_pass.renderBackground(context));
                        PPR_RETURN_ERROR_ON_FAIL(WorldCapture, m_pass.render(context));
                        return m_pass.renderParticles(context);
                    };
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldCapture, renderer.renderToTexture(
                                          *m_target, {DrawSubmission{"worldgen.optical", draw}}, ColorAttachmentOps{},
                                          PreRenderCallback{prepare}));
                    const auto submission_end = capture_clock.now();
                    const auto submitted = renderer.lastSubmittedFenceValue();
                    m_pass.notifyFrameSubmitted(submitted);
                    PPR_RETURN_ERROR_ON_FAIL(WorldCapture, renderer.waitOnHost());
                    const auto completion_end = capture_clock.now();
                    PPR_RETURN_ERROR_ON_FAIL(WorldCapture, renderer.retireCompletedFrames());
                    const auto gpu_timing = renderer.lastGpuFrameTiming();
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldCapture,
                        writeReadback(device, *m_target, m_directory / (std::string{view.m_name} + ".ppm")));
                    manifest << std::format(
                        "{} center_m=({}, {}) width_m={} pixels={}\n", view.m_name, view.m_x, view.m_y, view.m_width,
                        kCaptureEdge);
                    manifest << std::format(
                        "cpu_submission_ms={:.6f} host_completion_wait_ms={:.6f}\n",
                        time::seconds(submission_end - submission_begin) * 1000.0,
                        time::seconds(completion_end - submission_end) * 1000.0);
                    if (gpu_timing and gpu_timing->m_submission == submitted) {
                        manifest << std::format(
                            "submission={} gpu_frame_ms={:.6f}\n", submitted, gpu_timing->m_milliseconds);
                    } else {
                        manifest << std::format("submission={} gpu_frame_ms=unavailable\n", submitted);
                    }
                }
                manifest << std::format(
                    "atlas_uploaded_bytes={} atlas_writes={} atlas_serializations={} atlas_deferred={} growth_waits={} resident_table_builds={} resident_table_uploads={} cached_tiles={}\n",
                    m_pass.atlasUploadedBytes(), m_pass.atlasWriteCount(), m_pass.atlasSerializationCount(),
                    m_pass.atlasDeferredCount(), m_pass.growthWaitCount(), m_pass.residentTableBuildCount(),
                    m_pass.residentTableUploadCount(), m_pass.cachedTileCount());
                const auto observations = m_pass.observations();
                manifest << std::format(
                    "texture_entries={} texture_slots={} texture_decoded_bytes={} particle_instances={} particle_buffer_bytes={} particle_uploads={} particle_uploaded_bytes={} background_targets={} background_target_bytes={} draw_commands={} dispatch_commands={} retained_atlases={}\n",
                    observations.m_texture_entries, observations.m_texture_slots, observations.m_texture_decoded_bytes,
                    observations.m_particle_instances, observations.m_particle_buffer_bytes,
                    observations.m_particle_uploads, observations.m_particle_uploaded_bytes,
                    observations.m_background_targets, observations.m_background_target_bytes,
                    observations.m_draw_commands, observations.m_dispatch_commands, observations.m_retained_atlases);
                manifest
                    << "Timing scope: blocking diagnostic host. CPU submission and host completion wait are separate from completed GPU frame time. GPU timestamps bracket the submitted background preparation, backdrop, terrain and particle commands; they do not isolate terrain cost. Resource counters describe retained pass payload, not total VRAM or driver allocation size.\n";
                manifest.close();
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldCapture, manifest ? std::error_code{} : std::make_error_code(std::errc::io_error));
                m_complete = true;
                requestExit();
                return {};
            }

            [[nodiscard]] std::error_code shutdown() override {
                std::error_code first{};
                // A failed wait does not authorize releasing resources still used by GPU work.
                // Keep this fail-stop boundary log-free even during exception unwinding.
                try {
                    first = prepareGpuTeardown_();
                    if (gpuTeardownDisposition() == GpuQuiescence::kUnknown) {
                        failStopOnUnknownGpuTeardown_(first);
                    }
                } catch (...) {
                    failStopOnUnknownGpuTeardown_(first);
                }
                if (m_pass_started and gpuTeardownDisposition() == GpuQuiescence::kRemovalConfirmed) {
                    PPR_RETAIN_ERROR_ON_FAIL(WorldCapture, first, m_pass.notifyDeviceLost());
                }
                m_target.setNull();
                if (m_pass_started) {
                    PPR_RETAIN_ERROR_ON_FAIL(WorldCapture, first, m_pass.shutdown());
                    m_pass_started = false;
                }
                if (m_driver) {
                    PPR_RETAIN_ERROR_ON_FAIL(WorldCapture, first, m_driver->shutdown());
                    m_driver.reset();
                }
                if (m_base_started) {
                    PPR_RETAIN_ERROR_ON_FAIL(WorldCapture, first, Application::shutdown());
                    m_base_started = false;
                    m_base_ready = false;
                }
                m_shutdown_error = first;
                return first;
            }
        };
    }

    std::error_code captureWorldGpuCloseups(
        std::unique_ptr<ColonyDriver> driver, const double spawn_x_m, const double floor_y_m, std::string identity,
        const std::filesystem::path &directory) {
        if (not driver or not driver->preparedWorld()) {
            PPR_RETURN_ERROR_ON_FAIL(WorldCapture, std::errc::invalid_argument);
        }
        const float x = static_cast<float>(spawn_x_m), y = static_cast<float>(floor_y_m);
        const float chunk_metres = static_cast<float>(sim::kChunkEdge * kFineCellMetres);
        const std::array<CaptureView, 4u> views{
            {{"start", x, y + 3.0f, 28.0f}, {"seam", std::floor(x / chunk_metres) * chunk_metres, y, 12.0f},
                {"surface", x, y, 4.0f}, {"texture", x, y - 1.0f, 2.0f}}};
        // Best-effort cleanup if Application construction throws before taking the owner.
        PPR_DEFER {
            if (driver) {
                PPR_LOG_WARNING_ON_FAIL(WorldCapture, driver->shutdown());
            }
        };
        WorldCaptureApp app{std::move(driver), views, identity, directory};
        PPR_RETURN_ERROR_ON_FAIL(WorldCapture, app.run());
        PPR_RETURN_ERROR_ON_FAIL(WorldCapture, app.shutdownError());
        PPR_RETURN_ERROR_ON_FAIL(
            WorldCapture, app.complete() ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
        return {};
    }
}
