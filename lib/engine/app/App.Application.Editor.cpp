module;

#include "pP/Macros.h"

module engine.app;

import :application_editor;
import :display;
import :display.settings;
import :frame.packet;
import :frame.render_lane;
import :input.action;
import :input.device;
import :input.key;
import :input.listener;
import :input.routing;
import :platform.glfw.input;
import :renderer;
import :renderer.grid_pass;
import :renderer.post_process_pass;
import :renderer.presentation;
import :renderer.frame_fence;
import :renderer.triangle_pass;
import :renderer.types;
import :scene.camera.controller;
import :service.input;
import :service.ui;
import :service.window;
import :ui.imgui;
import :window.viewport;
import :window.monitor;

import engine.core;
import engine.image;
import engine.math;
import engine.mesh;
import engine.rhi;
import std;

namespace pP {
    namespace {
        class CpuObservationScope {
        public:
            CpuObservationScope(const ITimerClock *const clock, TimeSpan &duration) noexcept
                : m_clock(clock), m_duration(duration), m_start(clock ? clock->now() : TimePoint{}) {
            }
            ~CpuObservationScope() noexcept {
                if (m_clock) {
                    m_duration += m_clock->now() - m_start;
                }
            }

        private:
            const ITimerClock *m_clock;
            TimeSpan &m_duration;
            TimePoint m_start;
        };

        // Every settlement path — applied, minimized, obsolete, cancelled, drop
        // in transition, or worker error — must carry the same grid-payload
        // manifest the client staged, so the derived host never has to guess
        // which slot a global flag went with. GPU retirement stays a distinct
        // milestone (m_submit_value via the fence); this is the CPU-consumption
        // sideband. Allocating (the ack arrays copy) and therefore NOT
        // noexcept: allocation failure throws bad_alloc to the existing
        // executor/teardown boundaries, which treat allocation failure as fatal.
        void stampGridEcho(const GridPass::GridStagedFrame &stage, RenderFrameResult &result) {
            result.m_grid_reset_caches = stage.m_reset_caches;
            result.m_grid_full_refresh = stage.m_full_refresh;
            result.m_grid_ack_seed = stage.m_ack_seed;
            result.m_grid_ack_chunk_ids = stage.m_ack_chunk_ids;
            result.m_grid_ack_revisions = stage.m_ack_revisions;
        }

        // Stage B (resolve/post) renders into the post pass's own R-sized
        // RGBA16F output; the Renderer derives that one-color, no-depth,
        // single-sample signature from the attachment it is handed, and stage C
        // samples the same display-linear image.

    } // namespace

    // UI frontend/GPU split handoffs (defined in App.UI.ImGui.cpp with
    // external linkage): the Editor runs Detached + Context halves on main
    // and the GPU half on the owner lane. Forward-declared here so this
    // implementation unit stays within its allowed scope (no service-header
    // change); the concrete type never leaks across the boundary.
    [[nodiscard]] std::error_code shutdownImGuiFrontendDetachedOnMain(class IUIService &ui);
    [[nodiscard]] std::error_code releaseImGuiGpuResourcesOnOwnerLane(class IUIService &ui);
    [[nodiscard]] std::error_code shutdownImGuiFrontendContextOnMain(class IUIService &ui);

    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(Editor, debug, none)

    // ------------------------------------------------------------------
    // application editor
    // ------------------------------------------------------------------

    ApplicationEditor::ApplicationEditor(const std::string_view name, const std::span<const char *const> argv)
        : ApplicationEditor(name, argv, nullptr) {
    }

    ApplicationEditor::ApplicationEditor(
        const std::string_view name, const std::span<const char *const> argv,
        const GpuTeardownFailureHandler on_gpu_teardown_failure)
        : Application(
              ApplicationDomain{
                  .m_is_headless = false,
                  .m_is_interactive = true,
                  .m_needs_presence = false,
                  .m_needs_rendering = true,
                  .m_needs_user_interface = true,
              },
              name, argv, on_gpu_teardown_failure) {
    }

    ApplicationEditor::ApplicationEditor(
        const std::string_view name, const std::span<const char *const> argv, std::unique_ptr<IPlatform> platform,
        const ITimerClock &clock, const GpuTeardownFailureHandler on_gpu_teardown_failure)
        : Application(
              ApplicationDomain{
                  .m_is_headless = false,
                  .m_is_interactive = true,
                  .m_needs_presence = false,
                  .m_needs_rendering = true,
                  .m_needs_user_interface = true,
              },
              name, argv, std::move(platform), clock, on_gpu_teardown_failure) {
    }

    ApplicationEditor::~ApplicationEditor() noexcept = default;

    // ------------------------------------------------------------------
    // client service
    // ------------------------------------------------------------------

    safe_ptr<const Camera> ApplicationEditor::getMainCamera() const noexcept {
        return m_camera.get();
    }

    safe_ptr<const Player> ApplicationEditor::getMainPlayer() const noexcept {
        return m_player.get();
    }

    safe_ptr<const WindowViewport> ApplicationEditor::getMainViewport() const noexcept {
        return m_main_viewport.get();
    }

    safe_ptr<Camera> ApplicationEditor::getMainCamera() noexcept {
        return m_camera.get();
    }

    safe_ptr<Player> ApplicationEditor::getMainPlayer() noexcept {
        return m_player.get();
    }

    safe_ptr<WindowViewport> ApplicationEditor::getMainViewport() noexcept {
        return m_main_viewport.get();
    }

    safe_ptr<const InputContext> ApplicationEditor::getMainInputContext() const noexcept {
        return safe_ptr(&m_main_input_context->m_context);
    }

    safe_ptr<InputContext> ApplicationEditor::getMainInputContext() noexcept {
        return safe_ptr(&m_main_input_context->m_context);
    }

    // ------------------------------------------------------------------
    // lifecycle
    // ------------------------------------------------------------------

    std::error_code ApplicationEditor::initialize() {
        PPR_RETURN_ERROR_ON_FAIL(Editor, Application::initialize());

        m_player = std::make_unique<Player>(PlayerIdentity{});
        m_camera = std::make_unique<Camera>(ECameraProjection::perspective);

        // Camera controller is a pure motion integrator; the
        // background-drag latch lives in Routing (m_bg_state).
        m_camera_input_mapping = std::make_unique<InputMapping>("camera_input_mapping");
        auto free_camera = std::make_unique<FreeCameraController>();
        free_camera->lookAt(float3{0.0f, 0.0f, -2.0f}, float3{0.0f, 0.0f, 0.0f}, math::axis_y);
        free_camera->provideInputActionKeyMappings(*m_camera_input_mapping);
        m_camera_controller = std::move(free_camera);

        m_player->getListener().addInputMapping(
            m_camera_input_mapping, static_cast<int>(EInputMappingPriority::camera));

        const safe_ptr<IInputService> input_service = getPlatform().getInputService();

        m_main_input_context = std::make_unique<WindowInputContext>(input_service);

        m_input_background_latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui), safe_ptr{&m_player->getListener()},
            static_cast<int>(EInputListenerPriority::player), static_cast<int>(EInputListenerPriority::detector));

        // Lifetime rule: the broadcast stores a non-owning function_ref
        // view, so the member below owns the callable; it is assigned
        // before subscribing a view of it, and the handle is reset in
        // shutdown() before member destruction.
        m_on_device_disconnected = [this](const IInputDevice &) noexcept {
            // Session reset (Routing latch) is separate from motion reset (controller).
            if (m_input_background_latch) {
                m_input_background_latch->resetInputState();
            }
            if (m_camera_controller) {
                m_camera_controller->resetInputState();
            }
            return default_value_v;
        };
        m_device_disconnected_handle = input_service->whenDeviceDisconnected(m_on_device_disconnected);

        ServicesStore &app_services = getServices();
        const safe_ptr<IRhiService> rhi_service{app_services.inject()};
        const safe_ptr<IShaderService> shader_service{app_services.inject()};

        // Quiescent-startup handoff (explicit): pass + UI GPU objects
        // initialize here on main while the render lane owns no work yet —
        // no frame is queued, no sweep runs, and the executor below is
        // installed only afterwards. This quiescent start is the handoff
        // permission for main-side creation; it does NOT mean all later
        // lifecycle runs on one lane. Steady-state pass mutations run on
        // the owner lane via staged payloads (executeRenderWork_), UI GPU
        // work runs on the encode lane via detached data, and teardown
        // releases split frontend (main) from GPU (owner lane) below.
        m_triangle_pass = std::make_unique<TrianglePass>();
        PPR_RETURN_ERROR_ON_FAIL(Editor, m_triangle_pass->initialize(*rhi_service, *shader_service, getContentDir()));

        m_grid_pass = std::make_unique<GridPass>();
        const std::error_code grid_err = m_grid_pass->initialize(*rhi_service, *shader_service, getContentDir());
        if (grid_err) [[unlikely]] {
            // run() also calls shutdown on failed initialization. Clean the
            // partially initialized pass now, retaining the original error.
            std::error_code cleanup_err{};
            PPR_RETAIN_ERROR_ON_FAIL(Editor, cleanup_err, m_grid_pass->shutdown());
            m_grid_pass.reset();
            return grid_err;
        }

        m_post_process = std::make_unique<PostProcessPass>();
        const std::error_code post_err = m_post_process->initialize(*shader_service, getContentDir());
        if (post_err) [[unlikely]] {
            // Mirror the grid failure path: release whatever the partial
            // initialization created and retain the original error.
            std::error_code cleanup_err{};
            PPR_RETAIN_ERROR_ON_FAIL(Editor, cleanup_err, m_post_process->shutdown());
            m_post_process.reset();
            PPR_RETURN_ERROR_ON_FAIL(Editor, post_err);
        }

        // Always-on temporal reconstruction owns the padded 4x scene target
        // and the resolved history pair. Stage A draws into its scene texture,
        // stage B reconstructs into its next history image.
        m_reconstruction = std::make_unique<ReconstructionPass>();
        const std::error_code reconstruction_err = m_reconstruction->initialize(*shader_service, getContentDir());
        if (reconstruction_err) [[unlikely]] {
            std::error_code cleanup_err{};
            PPR_RETAIN_ERROR_ON_FAIL(Editor, cleanup_err, m_reconstruction->shutdown());
            m_reconstruction.reset();
            PPR_RETAIN_ERROR_ON_FAIL(Editor, cleanup_err, m_post_process->shutdown());
            m_post_process.reset();
            PPR_RETURN_ERROR_ON_FAIL(Editor, reconstruction_err);
        }

        m_ui_service = ui::createImGuiService();

        // UI face slots: Sour Gummy carries body, emphasis and the smaller
        // read-out the Inspector binds (ui::theme::EFont::tiny); Jersey 25
        // carries headings. Font Awesome merges into the body, so it does not
        // consume a separate slot. Keep ui::theme::font's contract in sync.
        // Static pinned artifacts are staged beside the executable. The
        // digests pin the exact bytes: a missing or altered file fails UI
        // initialization instead of silently substituting a default font.
        const std::filesystem::path font_root = getContentDir().path() / "fonts";
        const std::array font_sources{
            UIFontSource{
                .m_path = font_root / "SourGummy-Medium.ttf",
                .m_expected_bytes = 71728u,
                .m_size_pixels = 15.0f,
                .m_sha256 = sha256DigestFromHex("4fc77ca2182c2b0f21724f87d7b4343d56c009f13430f174f2820296444bd075"),
            },
            UIFontSource{
                .m_path = font_root / "Font Awesome 7 Free-Solid-900.otf",
                .m_merge_glyphs = ui::icons::kMergeRanges,
                .m_expected_bytes = 414704u,
                // Merged icons bake slightly below the body size: Font Awesome
                // solid glyphs fill their em box, so at 1:1 they read taller
                // than the text cap height beside them.
                .m_size_pixels = 13.0f,
                .m_sha256 = sha256DigestFromHex("daa92774861a14e9ccbccc5e73a5fba514271ba4d1bd7e5adbf952182bc00835"),
                .m_merge_into_previous = true,
            },
            UIFontSource{
                .m_path = font_root / "SourGummy-Bold.ttf",
                .m_expected_bytes = 72180u,
                .m_size_pixels = 15.0f,
                .m_sha256 = sha256DigestFromHex("62c9d98e95e1bb971e8d7a2d3a6d197575156d5ec69d2498ae8f2a7c925411b1"),
            },
            UIFontSource{
                .m_path = font_root / "Jersey25-Regular.ttf",
                .m_expected_bytes = 142372u,
                .m_size_pixels = 21.0f,
                .m_sha256 = sha256DigestFromHex("4d4acdd89a748e9548041a3046d95536c500e1a7be9197c6aeef7d980f79d370"),
            },
            // The compact read-out binding: the same pinned body file at a
            // smaller size, so no new artifact or licence enters the set.
            UIFontSource{
                .m_path = font_root / "SourGummy-Medium.ttf",
                .m_expected_bytes = 71728u,
                .m_size_pixels = 13.0f,
                .m_sha256 = sha256DigestFromHex("4fc77ca2182c2b0f21724f87d7b4343d56c009f13430f174f2820296444bd075"),
            },
        };

        PPR_RETURN_ERROR_ON_FAIL(
            Editor, m_ui_service->initialize(
                        *m_main_input_context, *rhi_service, *shader_service, font_sources,
                        m_input_background_latch->m_foreground_priority));

        PPR_RETURN_ERROR_ON_FAIL(
            Editor,
            m_input_background_latch->initialize(m_main_input_context->m_context, m_ui_service->getInputListener()));
        m_input_latch_initialized = true;

        IWindowService &window_service = *getPlatform().getWindowService();

        safe_ptr<Window> main_window{};
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, window_service.createWindow(
                        WindowModel{
                            .m_title = getName(),
                            .m_window_size = int2{1280, 720},
                        },
                        &main_window));

        // There is no surface or admitted frame yet. Apply validated startup
        // window intent on the native thread; the first surface is created on
        // its owner lane from this resulting framebuffer geometry. Runtime
        // changes instead use Application's suspended request/ack transaction.
        DisplaySettings display_settings = displayController().committed();
        const auto capabilities = displayCapabilities();
        if (not capabilities) {
            PPR_RETURN_ERROR_ON_FAIL(Editor, capabilities.error());
        }
        config::Diagnostics display_diagnostics{};
        if (const std::error_code error = validateDisplaySettings(
                display_settings, *capabilities, display_diagnostics)) {
            PPR_RETURN_ERROR_ON_FAIL(Editor, error);
        }
        const auto selected_monitor = resolveDisplayMonitor(
            display_settings.m_monitor, *capabilities, display_diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(Editor, selected_monitor);
        const DisplayMonitorCapability *const selected_capability = *selected_monitor;
        if (display_settings.m_monitor != "auto" and
            selected_capability != nullptr and
            not selected_capability->m_identity.empty()) {
            display_settings.m_monitor = selected_capability->m_identity;
            setDisplaySettings(display_settings);
        }

        SharedMonitor monitor{};
        PPR_RETURN_ERROR_ON_FAIL(Editor, window_service.enumerateMonitors([&](const SharedMonitor &entry) {
            if (selected_capability != nullptr and
                (not selected_capability->m_identity.empty() ? entry->m_identity == selected_capability->m_identity
                                                             : entry->m_name == selected_capability->m_name)) {
                if (monitor) {
                    PPR_RETURN_ERROR_ON_FAIL(Editor, std::make_error_code(std::errc::invalid_argument));
                }

                monitor = entry;
            }

            return std::error_code{};
        }));

        if (not monitor) {
            PPR_RETURN_ERROR_ON_FAIL(Editor, std::make_error_code(std::errc::no_such_device));
        }

        if (displayWindowChange(display_settings, DisplaySettings{})) {
            int2 size = main_window->m_window_size;
            if (display_settings.m_width != 0u) {
                size = int2{static_cast<int>(display_settings.m_width), static_cast<int>(display_settings.m_height)};
            } else if (display_settings.m_window_mode == EDisplayWindowMode::fullscreen) {
                size = monitor->m_video_mode.m_resolution;
            }

            const auto geometry = window_service.configureDisplayWindow(
                *main_window, display_settings.m_window_mode == EDisplayWindowMode::fullscreen ? &*monitor : nullptr,
                display_settings.m_window_mode == EDisplayWindowMode::fullscreen ? monitor->m_virtual_position
                                                                                 : main_window->m_window_position,
                size,
                display_settings.m_refresh_hz != 0u ? std::optional<u32>{display_settings.m_refresh_hz} : std::nullopt);
            PPR_RETURN_ERROR_ON_FAIL(Editor, geometry);
        }

        if (display_settings.m_window_mode == EDisplayWindowMode::windowed) {
            const auto centered = window_service.centerDisplayWindow(*main_window, *monitor);
            PPR_RETURN_ERROR_ON_FAIL(Editor, centered);
        }

        // Phase 3 approved default (follow-vsync): no monitor-derived CPU
        // cap. The foreground target comes from the committed
        // DisplaySettings (Application::setDisplaySettings); monitor refresh
        // stays informational (a panel "use current refresh as cap" action
        // is Phase 5, never an implicit cap).
        m_main_viewport = std::make_unique<WindowViewport>(main_window, ViewportLayout{});
        bindDisplaySurface_(main_window->m_handle);

        PPR_RETURN_ERROR_ON_FAIL(Editor, m_main_input_context->initialize(main_window));

        std::ignore = window_service.setMainWindow(main_window);

        // Window-blur drag clear: focus loss releases background hold.
        main_window->m_when_focused.subscribe<&ApplicationEditor::onMainWindowFocused_>(this);

        getServices().insert_or_assign(safe_ptr(m_ui_service));

        // P2 lane executor install (startup boundary): the worker consumes
        // owned packets + detached UI through this hook. Services resolve
        // once here, never mid-frame on the worker. Passes stay
        // editor-owned; the lane is only their execution affinity.
        // Captured at the service boundary for executeRenderWork_: on the
        // worker this is the only legal source (never read the store from
        // the lane); on the inline path it is the same main-owned service,
        // so the inline branch decodes it identically.
        m_lane_rhi_service = getServices().tryGet<IRhiService>();
        m_lane_shader_service = getServices().tryGet<IShaderService>();
        if (renderLaneOwnsRenderer_()) {
            renderLane()->setWorldExecutor([this](const GridWorldCommand &command, GridWorldResult &result) {
                result.m_error = executeGridWorld_(command, result);
            });
            renderLane()->setExecutor(
                [this](const RenderFrameRequest &request, RenderFrameResult &result) -> std::error_code {
                    if (not request.m_packet) [[unlikely]] {
                        result.m_dropped = true;
                        return std::make_error_code(std::errc::invalid_argument);
                    }
                    stampGridEcho(request.m_grid_stage, result);
                    return executeRenderWork_(request, result);
                });
        }
        return default_value_v;
    }

    void ApplicationEditor::setPostProcessSettings(
        const PostProcessSettings &settings, const u32 pixel_scale) noexcept {
        // CPU-only policy set: no GPU work and no frame drain. Clamping once
        // here keeps every owned frame request value-valid; the next submit
        // copies the stored values onto both request paths.
        m_post_settings = settings;
        m_pixel_scale = std::clamp(pixel_scale, 1u, 8u);
    }

    void ApplicationEditor::setLightingInspectionSettings(
        const LightingInspectionSettings &settings, const i32 sky_hour) noexcept {
        // CPU-only policy set: no GPU work and no frame drain. The frame
        // request revalidates both values before any backend work.
        m_inspection = settings;
        m_sky_hour = sky_hour;
    }

    std::error_code ApplicationEditor::ensureSceneDepth_(rhi::IDevice &device, const int2 raster_extent) {
        // One completion-gated depth target per ring slot. Stage A writes it
        // and stage B never reads it, but the slot a later frame rotates onto
        // must not be rewritten before its writer's fence completes.
        const u32 index = m_scene_depth_selected;
        SceneDepthSlot &slot = m_scene_depths[index];
        if (slot.m_depth and slot.m_extent.x == raster_extent.x and slot.m_extent.y == raster_extent.y) {
            return {};
        }

        rhi::TextureDesc depth{};
        depth.type = rhi::TextureType::Texture2DMS;
        depth.size = {.width = safe_narrowing(raster_extent.x), .height = safe_narrowing(raster_extent.y), .depth = 1u};
        depth.arrayLength = 1u;
        depth.mipCount = 1u;
        depth.format = rhi::Format::D32Float;
        depth.sampleCount = ReconstructionPass::kSampleCount;
        depth.memoryType = rhi::MemoryType::DeviceLocal;
        depth.usage = rhi::TextureUsage::DepthStencil;
        depth.defaultState = rhi::ResourceState::DepthWrite;
        depth.label = "scene stage depth";

        rhi::ComPtr<rhi::ITexture> depth_texture;
        rhi::ComPtr<rhi::ITextureView> depth_view;
        PPR_RETURN_ERROR_ON_FAIL(Editor, device.createTexture(depth, nullptr, depth_texture.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(Editor, depth_texture->getDefaultView(depth_view.writeRef()));

        slot.m_depth = std::move(depth_texture);
        slot.m_view = std::move(depth_view);
        slot.m_extent = raster_extent;
        return {};
    }

    std::error_code ApplicationEditor::resolveStageAttachments_(rhi::IDevice &device, const int2 raster_extent) {
        rhi::ITexture &scene_color = m_reconstruction->sceneTexture();
        if (m_scene_color_texture != &scene_color) {
            PPR_RETURN_ERROR_ON_FAIL(Editor, scene_color.getDefaultView(m_scene_color_view.writeRef()));
            m_scene_color_texture = &scene_color;
        }

        rhi::ITexture &post_output = m_post_process->outputTexture();
        if (m_post_output_texture != &post_output) {
            PPR_RETURN_ERROR_ON_FAIL(Editor, post_output.getDefaultView(m_post_output_view.writeRef()));
            m_post_output_texture = &post_output;
        }

        return ensureSceneDepth_(device, raster_extent);
    }

    void ApplicationEditor::releaseStageTargets_() noexcept {
        m_scene_depths = {};
        m_scene_depth_cursor = 0u;
        m_scene_depth_selected = 0u;
        m_scene_color_view.setNull();
        m_scene_color_texture = nullptr;
        m_post_output_view.setNull();
        m_post_output_texture = nullptr;
    }

    // ------------------------------------------------------------------
    // scene flow
    // ------------------------------------------------------------------

    std::error_code ApplicationEditor::replaceMainCameraController(
        std::unique_ptr<ICameraController> controller, const ECameraProjection projection) {
        if (not controller) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        if (not m_camera or
            not m_camera_controller or
            not m_camera_input_mapping or
            not m_player or
            not m_main_input_context or
            not m_main_viewport) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        // Prepare before detaching so allocation failures leave the current
        // controller and its mapping intact.
        auto next_mapping = std::make_unique<InputMapping>("camera_input_mapping");
        controller->resetInputState();
        controller->provideInputActionKeyMappings(*next_mapping);

        InputListener &listener = m_player->getListener();
        if (not listener.removeInputMapping(*m_camera_input_mapping)) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        m_camera_controller->resetInputState();
        m_player->clearFrameMessages();
        if (m_input_background_latch) {
            m_input_background_latch->resetInputState();
        }

        // Destroy the old mapping (including its callback captures) before
        // the old controller at scope exit.
        auto previous_controller = std::move(m_camera_controller);
        auto previous_mapping = std::move(m_camera_input_mapping);
        m_camera_controller = std::move(controller);
        m_camera_input_mapping = std::move(next_mapping);
        listener.addInputMapping(m_camera_input_mapping, static_cast<int>(EInputMappingPriority::camera));
        m_camera->setCameraMode(projection);
        m_camera->signalCameraCutNextFrame();
        return default_value_v;
    }

    std::error_code ApplicationEditor::loadScene(const std::filesystem::path &dir, const std::string_view file) {
        PPR_RETURN_ERROR_ON_FAIL(Editor, unloadScene());
        if (not m_triangle_pass) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        Expected<mesh::SceneAsset> scene = mesh::importAndConvert(dir, file);
        if (not scene.has_value()) [[unlikely]] {
            return scene.error();
        }

        return setLoadedScene(dir, std::move(*scene));
    }

    std::error_code ApplicationEditor::setLoadedScene(
        const std::filesystem::path &dir, mesh::SceneAsset &&loaded_scene) {
        const hal::ProfileScope profile_scope{"ApplicationEditor::setLoadedScene", "Scene"};
        Array<image::ImageAsset> images{};
        for (const mesh::ImageRef &ref: loaded_scene.m_images) {
            mem::SharedBuffer bytes{};
            if (ref.m_is_file) {
                Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(dir / ref.m_rel_path);
                if (not mapped.has_value()) [[unlikely]] {
                    return mapped.error();
                }

                bytes = *mapped;
            } else {
                bytes = ref.m_bytes;
            }

            if (not bytes.isValid()) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }

            Expected<image::ImageAsset> decoded = image::decodeToRgba8(
                bytes.getBufferData(), ref.m_ext, image::ImageDecodeDesc{}, image::EImageUsage::color);
            if (not decoded.has_value()) [[unlikely]] {
                return decoded.error();
            }

            images.push_back(*decoded);
        }

        Expected<TrianglePass::UploadedScene> uploaded = m_triangle_pass->uploadScene(loaded_scene, images);
        if (not uploaded.has_value()) [[unlikely]] {
            return uploaded.error();
        }

        m_scene = std::move(loaded_scene);
        m_images = std::move(images);
        m_uploaded_scene = std::move(*uploaded);

        PPR_LOG(
            Editor, info, "scene loaded",
            {
                {"meshes", m_scene->m_meshes.size()},
                {"images", m_images.size()},
                {"prims", m_uploaded_scene.m_primitives.size()},
            });
        return default_value_v;
    }

    std::error_code ApplicationEditor::unloadScene() {
        std::error_code first_err{};
        // Release on main: the GpuCaches that own these scene handles have
        // main-thread affinity (pass caches are main-initialized). The
        // per-frame instance list is a lane-side replay now: dropping the
        // client stage below empties it next execute. Explicit
        // affinity-and-quiescence contract: startup and quiesced/shutdown
        // only — mid-run use rides the staged payload on the owner lane.
        if (m_scene.has_value() and m_triangle_pass) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_triangle_pass->releaseScene(m_uploaded_scene));
        }

        m_uploaded_scene = TrianglePass::UploadedScene{};
        m_images.clear();
        m_scene.reset();
        m_triangle_stage.m_instances.clear();
        return first_err;
    }

    std::error_code ApplicationEditor::submitSceneInstances_() {
        const hal::ProfileScope profile_scope{"App.Editor.Update.SubmitInstances", "Update"};
        // Stage owned instances instead of mutating the pass: the lane
        // replays this payload onto the pass inside executeRenderWork_.
        m_triangle_stage.m_instances.clear();

        const mesh::SceneAsset &scene = *m_scene;

        for (const mesh::SceneInstance &instance: scene.m_instances) {
            if (instance.m_mesh >= scene.m_meshes.size() or instance.m_node >= scene.m_nodes.size()) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }

            const mesh::StaticMeshAsset &mesh_asset = scene.m_meshes[instance.m_mesh];
            const float4x4 world = scene.m_nodes[instance.m_node].m_world.toMatrix();
            for (std::size_t prim_slot = 0u; prim_slot < mesh_asset.m_primitives.size(); ++prim_slot) {
                Expected<TrianglePass::JoinedInstancePrim> joined = TrianglePass::joinInstancePrim(
                    scene, m_uploaded_scene, instance, prim_slot);
                if (not joined.has_value()) [[unlikely]] {
                    return joined.error();
                }

                m_triangle_stage.m_instances.push_back(
                    TrianglePass::SubmittedInstance{
                        .m_bag = joined->m_bag, .m_material = joined->m_material, .m_model = world});
            }
        }
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // teardown
    // ------------------------------------------------------------------

    std::error_code ApplicationEditor::shutdown() {
        std::error_code first_err{};

        // Teardown is the inverse of setup. B1' fail-stop first: admission
        // is already stopped and GPU quiescence established (or re-reported)
        // idempotently here — no new frames enter shutdown on this lane.
        // UNKNOWN completion fail-stops BEFORE anything a pending frame may
        // still read is released; the OS reclaims retained objects. Only
        // proven idle or positively-confirmed removal takes the release path
        // below (removal = no_such_device_or_address or a latched-terminal
        // fence). abandon()/notifyDeviceLost() prove nothing by themselves
        // and are never used as proof on the UNKNOWN path. The boundary is
        // log-free (see Application::shutdown): the quiescence error is
        // retained silently so a throw during message formatting cannot
        // escape before the fail-stop below. The catch-all hardens the
        // UNKNOWN boundary itself: no exception between classification and
        // the fail-stop may skip _Exit.
        try {
            if (const std::error_code teardown_err = prepareGpuTeardown_()) {
                if (not first_err) {
                    first_err = teardown_err;
                }
            }
            if (gpuTeardownDisposition() == GpuQuiescence::kUnknown) {
                failStopOnUnknownGpuTeardown_(first_err);
            }
        } catch (...) {
            failStopOnUnknownGpuTeardown_(first_err);
        }
        const bool device_loss_confirmed = gpuTeardownDisposition() == GpuQuiescence::kRemovalConfirmed;
        // P2 teardown order: settle lane packets first (no new lane frame
        // can start afterwards), then release renderer/lane-owned GPU state
        // ON the lane while main-owned triangle GPU state releases on main
        // below (its GpuCaches captured the initializing main thread and
        // reject other threads). The UI service shuts down on main below
        // (ImGui context is client-owned); windows die on main after the
        // base shutdown joins the lane.
        bool lane_releases_done = false;
        if (renderLaneOwnsRenderer_()) {
            // Gated settle: the drain's handoff error is retained and an
            // unacknowledged drain escalates (no release behind outstanding
            // handoff), mirroring the acknowledged-handoff rule below.
            bool drain_acknowledged = false;
            const std::error_code drain_err = renderLane()->drainQueuedFrames(&drain_acknowledged);
            if (drain_err and not first_err) {
                first_err = drain_err;
            }
            if (not drain_acknowledged) {
                failStopOnUnknownGpuTeardown_(
                    drain_err ? drain_err : std::make_error_code(std::errc::operation_canceled));
            }

            // These main-owned packets were never handed to the lane. Release
            // their GPU leases only below positive quiescence and drain proof,
            // while their owning passes and RHI service still exist.
            m_grid_stage = GridPass::GridStagedFrame{};
            m_triangle_stage = TrianglePass::TriangleStagedFrame{};

            std::error_code lane_sync_err{};
            // Acknowledged completion (the lane ran the release) is the
            // only permission to treat its result as an operation error;
            // a handoff failure means the releases may still be pending.
            bool lane_acknowledged = false;
            try {
                auto operation_owner = std::make_unique<RenderLaneOperation>(
                    [this, device_loss_confirmed]() -> std::error_code {
                        std::error_code lane_err{};
                        if (device_loss_confirmed) {
                            PPR_LOG(Editor, warning, "shutdown idle wait failed, confirming device loss", {});
                            getRenderer().notifyDeviceLost();
                            if (m_grid_pass) {
                                PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_grid_pass->notifyDeviceLost());
                            }
                            if (m_post_process) {
                                PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_post_process->notifyDeviceLost());
                            }
                            if (m_reconstruction) {
                                PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_reconstruction->notifyDeviceLost());
                            }
                            // Triangle device-loss notification runs on main
                            // below: its GpuCaches captured the initializing
                            // main thread and reject other threads.
                        }
                        // Triangle scene release runs on main below (scene
                        // handles before triangle caches, while they are
                        // still alive).
                        if (m_grid_pass) {
                            PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_grid_pass->shutdown());
                        }
                        if (m_post_process) {
                            PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_post_process->shutdown());
                        }
                        if (m_reconstruction) {
                            PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_reconstruction->shutdown());
                        }
                        // Triangle shutdown runs on main below, before the
                        // resets and the base shutdown's renderer/RHI release.
                        return lane_err;
                    });

                auto operation_ticket = renderLane()->tryBeginOperation(operation_owner);

                const auto operation_completion = operation_ticket ? renderLane()->waitOperation(*operation_ticket)
                                                                   : Expected<RenderLaneOperationCompletion>{
                                                                         std::unexpected{operation_ticket.error()}};

                lane_acknowledged = operation_completion and operation_completion->m_ran;
                lane_sync_err = operation_completion ? (operation_completion->m_operation_error
                                                               ? operation_completion->m_operation_error
                                                               : operation_completion->m_transport_error)
                                                     : operation_completion.error();
            } catch (...) {
                lane_sync_err = std::make_error_code(std::errc::state_not_recoverable);
            }
            // UNKNOWN retain-and-fail-stop, log-free and BEFORE any
            // PPR_RETAIN_ERROR_ON_FAIL(message()-allocating) diagnostic and
            // BEFORE dependent destruction: an incomplete lane handoff (or
            // a lane whose terminal error latched without a confirmed
            // removal behind it) means the lane may still hold borrowed
            // captures — never reset passes/scene/services behind live work.
            // Process-level fail-stop, as in Application::shutdown's UNKNOWN
            // route. Owners are retained (no reset runs) for the OS.
            if (not lane_acknowledged) {
                failStopOnUnknownGpuTeardown_(lane_sync_err);
            }
            // Completed handoff: the release operation's error follows
            // normal propagation (retained silently — a throwing
            // message() must never escape shutdown).
            if (lane_sync_err and not first_err) {
                first_err = lane_sync_err;
            }
            // A sticky lane terminal is missing proof ONLY when no
            // acknowledged removal stands behind it. After a completed
            // RemovalConfirmed handoff the same latch is the proof itself —
            // preserve the confirmed removal, never reinterpret it as UNKNOWN.
            if (const std::error_code terminal = renderLane()->terminalError()) {
                if (not device_loss_confirmed) {
                    failStopOnUnknownGpuTeardown_(terminal);
                }
            }
            // Main-side triangle teardown (main-owned GpuCaches affinity:
            // the pass caches captured the initializing main thread at
            // Editor.cpp:221-222 and reject other threads, so quiescence
            // above permits the release here — it never migrates ownership
            // to the lane). Release scene handles before triangle caches;
            // finish before the base shutdown releases renderer/RHI below.
            // No further triangle work is admitted after. Each stage retains
            // the first error and continues independent cleanup; no
            // exception escapes past the resets and the base shutdown's
            // stop/join. UNKNOWN/unacknowledged fail-stop above is preserved
            // (failStopOnUnknownGpuTeardown_ never returns); ordinary
            // operation errors propagate via retain only.
            try {
                if (device_loss_confirmed and m_triangle_pass) {
                    PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_triangle_pass->notifyDeviceLost());
                }
            } catch (const std::bad_alloc &) {
                hal::fatalExit("allocation-failure");
            } catch (...) {
                if (not first_err) {
                    first_err = std::make_error_code(std::errc::state_not_recoverable);
                }
            }
            try {
                if (m_scene.has_value() and m_triangle_pass) {
                    PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_triangle_pass->releaseScene(m_uploaded_scene));
                }
            } catch (const std::bad_alloc &) {
                hal::fatalExit("allocation-failure");
            } catch (...) {
                if (not first_err) {
                    first_err = std::make_error_code(std::errc::state_not_recoverable);
                }
            }
            try {
                if (m_triangle_pass) {
                    PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_triangle_pass->shutdown());
                }
            } catch (const std::bad_alloc &) {
                hal::fatalExit("allocation-failure");
            } catch (...) {
                if (not first_err) {
                    first_err = std::make_error_code(std::errc::state_not_recoverable);
                }
            }
            // CPU-side scene clear on main; the guarded blocks below observe
            // the resets and skip (no double release).
            m_uploaded_scene = TrianglePass::UploadedScene{};
            m_images.clear();
            m_scene.reset();
            if (m_triangle_pass) {
                m_triangle_pass->clearInstances();
            }
            m_grid_pass.reset();
            m_post_process.reset();
            m_reconstruction.reset();
            releaseStageTargets_();
            m_triangle_pass.reset();
            lane_releases_done = true;
        }

        if (not lane_releases_done) {
            // Inline has no queued lane packets. The positive quiescence guard
            // above likewise precedes release of unconsumed main-side stages.
            m_grid_stage = GridPass::GridStagedFrame{};
            m_triangle_stage = TrianglePass::TriangleStagedFrame{};
        }

        if (device_loss_confirmed and not lane_releases_done and not renderLaneOwnsRenderer_()) {
            PPR_LOG(Editor, warning, "shutdown idle wait failed, confirming device loss", {});
            getRenderer().notifyDeviceLost();
            if (m_grid_pass) {
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_grid_pass->notifyDeviceLost());
            }
            if (m_post_process) {
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_post_process->notifyDeviceLost());
            }
            if (m_reconstruction) {
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_reconstruction->notifyDeviceLost());
            }
            if (m_triangle_pass) {
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_triangle_pass->notifyDeviceLost());
            }
        }

        // Release scene GPU handles while the pass caches are still alive,
        // before the §2.4 pass shutdown below. Skipped when the lane already
        // released them above (resets make every guard below a no-op).
        if (m_scene.has_value() and not lane_releases_done) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, unloadScene());
        }
        noteTeardownStage_("scene");

        // Detach detector first so no callback fires during teardown.
        m_device_disconnected_handle.reset();

        if (m_camera_controller) {
            m_camera_controller->resetInputState();
        }

        if (m_input_background_latch) {
            if (m_input_latch_initialized) {
                PPR_RETAIN_ERROR_ON_FAIL(
                    Editor, first_err, m_input_background_latch->shutdown(m_main_input_context->m_context));
                m_input_latch_initialized = false;
            }
            m_input_background_latch.reset();
        }

        if (m_ui_service) {
            ServicesStore &app_services = getServices();
            if (not app_services.erase(*m_ui_service)) {
                PPR_LOG(Editor, warning, "ui service was not registered");
            }

            // Frontend/GPU split (client-owned frontend stays on main):
            // detach the input listener + consume gating on main first, so
            // no client callback fires while the worker releases GPU state;
            // release GPU objects on the owner lane after quiescence (the
            // acknowledged handoff is the only permission to treat the
            // result as an operation error); destroy the live ImGui context
            // on main last. Quiescence permits the GPU release — it never
            // migrates frontend destruction to the worker.
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, shutdownImGuiFrontendDetachedOnMain(*m_ui_service));
            if (renderLaneOwnsRenderer_()) {
                std::error_code ui_err{};
                bool ui_acknowledged = false;
                try {
                    auto operation_owner = std::make_unique<RenderLaneOperation>(
                        [this]() -> std::error_code { return releaseImGuiGpuResourcesOnOwnerLane(*m_ui_service); });

                    auto operation_ticket = renderLane()->tryBeginOperation(operation_owner);

                    const auto operation_completion = operation_ticket ? renderLane()->waitOperation(*operation_ticket)
                                                                       : Expected<RenderLaneOperationCompletion>{
                                                                             std::unexpected{operation_ticket.error()}};

                    ui_acknowledged = operation_completion and operation_completion->m_ran;
                    ui_err = operation_completion
                                 ? (operation_completion->m_operation_error ? operation_completion->m_operation_error
                                                                            : operation_completion->m_transport_error)
                                 : operation_completion.error();
                } catch (...) {
                    ui_err = std::make_error_code(std::errc::state_not_recoverable);
                }
                // Incomplete handoff → retain the owner (no reset below) and
                // fail-stop; a completed release's error propagates.
                // Dependent destruction (context destroy + reset below)
                // runs only after this acknowledgement.
                if (not ui_acknowledged) {
                    failStopOnUnknownGpuTeardown_(ui_err);
                }
                if (ui_err and not first_err) {
                    first_err = ui_err;
                }
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, shutdownImGuiFrontendContextOnMain(*m_ui_service));
            } else {
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, releaseImGuiGpuResourcesOnOwnerLane(*m_ui_service));
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, shutdownImGuiFrontendContextOnMain(*m_ui_service));
            }
            m_ui_service.reset();
        }
        noteTeardownStage_("ui");

        if (m_grid_pass and not lane_releases_done) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_grid_pass->shutdown());
            m_grid_pass.reset();
        }

        if (m_post_process and not lane_releases_done) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_post_process->shutdown());
            m_post_process.reset();
        }

        if (m_reconstruction and not lane_releases_done) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_reconstruction->shutdown());
            m_reconstruction.reset();
        }

        releaseStageTargets_();

        if (m_triangle_pass and not lane_releases_done) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_triangle_pass->shutdown());
            m_triangle_pass.reset();
        }

        // The captured lane/inline RHI service is detached on every path:
        // the engine tears the service down below and the safe_ptr must not
        // outlive it on the inline route either.
        m_lane_rhi_service.reset();
        m_lane_shader_service.reset();

        safe_ptr<Window> main_window{};
        if (m_main_input_context) {
            // Discard pump-collected input records BEFORE destroying the input
            // context they observe. A throwing dispatch already releases its
            // own records via the batch guard, but never-dispatched records
            // would otherwise strand safe_ptr observers across this
            // context's destruction (GlfwInput::shutdown clears the batch
            // only later, via Application::shutdown below).
            if (const safe_ptr<IInputService> input_service = getPlatform().getInputService()) {
                if (GlfwInput *const glfw_inputs = dynamic_cast<GlfwInput *>(input_service.get())) {
                    glfw_inputs->m_native_batch.clear();
                }
            }

            main_window = m_main_input_context->m_window;

            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_main_input_context->shutdown());

            m_main_input_context->m_context.clearInputListeners();
            m_main_input_context.reset();
        }

        if (m_main_viewport) {
            PPR_ASSERT(main_window == &m_main_viewport->getWindow());
            m_main_viewport.reset();
        }

        if (main_window) {
            // Drop blur subscription alongside detector detach.
            main_window->m_when_focused.reset();

            // Release the surface while the renderer still owns the queue;
            // the window handle dies in destroyWindow below. Skipped after
            // confirmed loss: notifyDeviceLost above already released every
            // surface without touching the device. On the lane the release
            // runs there (by handle — the live Window is never touched off
            // main); inline it runs here directly.
            if (not device_loss_confirmed) {
                if (renderLaneOwnsRenderer_()) {
                    const WindowHandle doomed_handle = main_window->m_handle;
                    std::error_code surface_err{};
                    bool surface_acknowledged = false;
                    try {
                        auto operation_owner = std::make_unique<RenderLaneOperation>(
                            [this, doomed_handle]() -> std::error_code {
                                // A live window need not have presented yet;
                                // the Renderer creates its surface lazily.
                                if (getRenderer().surfaceGenerationFor(doomed_handle) == kSurfaceGenerationNone) {
                                    return {};
                                }

                                return getRenderer().destroyWindowSurface(doomed_handle);
                            });

                        auto operation_ticket = renderLane()->tryBeginOperation(operation_owner);

                        const auto operation_completion = operation_ticket
                                                              ? renderLane()->waitOperation(*operation_ticket)
                                                              : Expected<RenderLaneOperationCompletion>{
                                                                    std::unexpected{operation_ticket.error()}};

                        surface_acknowledged = operation_completion and operation_completion->m_ran;
                        surface_err = operation_completion ? (operation_completion->m_operation_error
                                                                     ? operation_completion->m_operation_error
                                                                     : operation_completion->m_transport_error)
                                                           : operation_completion.error();
                    } catch (...) {
                        surface_err = std::make_error_code(std::errc::state_not_recoverable);
                    }
                    // Incomplete handoff → retain the window/renderer (no
                    // destroyWindow/reset below) and fail-stop BEFORE the
                    // window destruction; a completed release's error
                    // propagates normally.
                    if (not surface_acknowledged) {
                        failStopOnUnknownGpuTeardown_(surface_err);
                    }
                    if (surface_err and not first_err) {
                        first_err = surface_err;
                    }
                } else if (getRenderer().surfaceGenerationFor(main_window->m_handle) != kSurfaceGenerationNone) {
                    PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, getRenderer().destroyWindowSurface(*main_window));
                }
            }

            IWindowService &window_service = *getPlatform().getWindowService();
            std::ignore = window_service.setMainWindow(nullptr);
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, window_service.destroyWindow(std::move(main_window)));
        }

        if (m_player) {
            m_player->getListener().clearInputMappings();
            m_player->clearFrameMessages();
        }

        m_camera_input_mapping.reset();
        m_camera_controller.reset();
        m_camera.reset();

        PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, Application::shutdown());
        return first_err;
    }

    // ------------------------------------------------------------------
    // frame update and settlement
    // ------------------------------------------------------------------

    void ApplicationEditor::onMainWindowFocused_([[maybe_unused]] const Window &window, const bool focused) {
        setBackgroundPriority(backgroundThrottleWanted_(focused));

        if (not focused) {
            // Session reset (Routing latch) is separate from motion reset (controller).
            if (m_input_background_latch) {
                m_input_background_latch->resetInputState();
            }

            if (m_camera_controller) {
                m_camera_controller->resetInputState();
            }
        }
    }

    bool ApplicationEditor::backgroundThrottleWanted_(const bool focused) const noexcept {
        // Raw focus loss only: the legacy opt-out and
        // display.throttle_unfocused apply centrally in
        // Application::getTargetFrameTime/displayView, so enabling
        // throttling while already blurred takes effect without another
        // focus event.
        return not focused;
    }

    std::error_code ApplicationEditor::update(TimeSpan dt) {
        const hal::ProfileScope scope{"App.Editor.Update"};
        if (not m_main_viewport) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        PPR_RETURN_ERROR_ON_FAIL(Editor, Application::update(dt));

        m_main_viewport->updateFromWindow();

        m_camera->updateModel(dt, *m_camera_controller, m_main_viewport->getViewport());

        // B2' single admission gate: run() already probed BEFORE this whole
        // virtual stage (busy frames never reach update), so the packet is
        // always built here. The Renderer re-checks admission at submit time
        // as the defensive enforcement backstop (quarantine/terminal
        // included); capacity-busy there retires as a dropped frame in the
        // execute step, never an app failure.

        // P0 serial-explicit build-packet stage (same thread): copy the
        // camera snapshot, viewport (+ revision), and window metadata into
        // owned packet storage BEFORE pass staging. Owner-lane pass updates
        // consume the packet-owned snapshot — never the live camera — so a
        // post-capture producer mutation cannot alter render preparation.
        {
            const hal::ProfileScope scope_frame_packet{"PPR.Frame.BuildPacket"};
            m_frame_packet = makeFramePacket(
                getFrameIndex(), dt, getTimerManager().getTotalElapsed(), m_camera->getSnapshot(),
                m_main_viewport->getViewport(), m_main_viewport->getViewportRevision(), m_main_viewport->getWindow());
            // Generational surface claim at capture: the consume path rejects
            // a stale packet before acquisition. First frame observes
            // kSurfaceGenerationNone (no record yet) and always accepts. On the
            // lane the mirror (fed by lane results) is the hint; the lane
            // re-verifies every claim against the live record at submit — main
            // never touches Renderer state while the lane owns it.
            if (renderLaneOwnsRenderer_()) {
                m_frame_packet->m_surface_generation = laneSurfaceGeneration(m_main_viewport->getWindow().m_handle);
            } else {
                m_frame_packet->m_surface_generation = getRenderer().surfaceGenerationFor(
                    m_main_viewport->getWindow().m_handle);
            }

            // One active-presentation clock at the application boundary. A
            // minimized window has no render target, so its interval never
            // accumulates; rejected attempts keep the value and simply do not
            // commit a pass baseline. No pass reads a clock.
            //
            // Caveat (recorded, not changed): this excludes minimized
            // suspension only. An OS sleep/suspend can still deliver one large
            // dt to the first admitted frame afterwards; suppressing that needs
            // an explicit suspend-aware clock decision at the application
            // boundary, which is outside this task.
            if (hasRenderTarget(*m_frame_packet)) {
                m_active_elapsed += dt;
            }
            m_frame_packet->m_active_elapsed = m_active_elapsed;

            // Screen-history reset identity: only a frame whose capture observed
            // a camera cut advances it, so a rejected cut frame leaves the next
            // frame on the same reset generation.
            if (m_frame_packet->m_camera.m_has_camera_cut) {
                ++m_camera_cut_generation;
            }
            m_frame_packet->m_render_camera_cut = m_camera_cut_generation;
        }

        // P2 pass-affinity: pass mutations run ON the owner lane (worker
        // or inline) inside executeRenderWork_ via the owned staged
        // payloads — the client only builds the owned payload here and
        // never touches pass state (m_camera_view / m_submitted_tiles /
        // m_submitted_instances / tile caches).
        if (m_scene.has_value()) {
            PPR_RETURN_ERROR_ON_FAIL(Editor, submitSceneInstances_());
        } else {
            // No scene: the staging payload is empty so the lane replay
            // clears the per-frame instance list, mirroring the old
            // clearInstances-on-unload edge.
            m_triangle_stage.m_instances.clear();
        }

        if (m_ui_service) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(Editor, m_ui_service->update(dt, *m_main_viewport));
        }

        PPR_ASSERT(m_frame_packet.has_value());
        return default_value_v;
    }

    std::error_code ApplicationEditor::render() {
        const hal::ProfileScope scope{"App.Editor.Render"};
        PPR_RETURN_ERROR_ON_FAIL(Editor, Application::render());

        // P0 serial-explicit consume: single-use packet owned by this frame.
        // A missing packet means update() never staged one (fail closed).
        if (not m_frame_packet) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }
        const FramePacket packet = *m_frame_packet;
        m_frame_packet.reset();
        return submitRenderWork_(packet);
    }

    std::error_code ApplicationEditor::requestFrameCapture() {
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, surfaceReadbackEnabled() and not m_frame_capture_armed
                        ? std::error_code{}
                        : std::make_error_code(std::errc::operation_not_permitted));
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, m_frame_capture_sequence != std::numeric_limits<u64>::max()
                        ? std::error_code{}
                        : std::make_error_code(std::errc::value_too_large));

        ++m_frame_capture_sequence;
        m_frame_capture_armed = true;
        m_frame_capture_requested = true;
        return {};
    }

    std::error_code ApplicationEditor::rearmFrameCapture() {
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, m_frame_capture_armed and
                            not m_frame_capture_requested and
                            m_frame_capture_sequence != 0u and
                            m_frame_capture_success_sequence == m_frame_capture_sequence
                        ? std::error_code{}
                        : std::make_error_code(std::errc::operation_not_permitted));

        m_frame_capture_armed = false;
        return {};
    }

    void ApplicationEditor::onLaneFrameConsumed_(const RenderFrameResult &result) {
        // Chain the base settlement contract first: reconciles the
        // construction-reservation credit against the lane's settlement.
        // A stale result (foreign epoch) never reaches diagnostics or edit
        // acks: the consumption slot that fell out of order was for ANOTHER
        // run, so validating it against the current packet would cross-wire.
        // GPU retirement stays the fence/ledger's milestone, never this hook:
        // the consumption ack says the WORLD EDIT applied; it does NOT say
        // the GPU finished, nor release a GPU-pending construction credit.
        // A stale result (foreign epoch) neither releases a credit in the
        // base hook nor acks world edits/mirrors here: the settlement
        // belongs to another run, and nothing it posted is applies here.
        if (result.m_reservation_epoch != 0u and result.m_reservation_epoch != reservationEpoch_()) {
            return;
        }
        Application::onLaneFrameConsumed_(result);
        if (result.m_surface_capture_attempted and result.m_surface_capture_sequence == m_frame_capture_sequence) {
            m_frame_capture_requested = false;

            if (result.m_presented and
                not result.m_dropped and
                not result.m_error and
                not result.m_sweep_error and
                not result.m_surface_capture_error and
                not result.m_submit_possibly_executed and
                result.m_submit_value != kFenceValueNone and
                result.m_surface_pixels and
                result.m_surface_pixels->m_submission == result.m_submit_value) {
                m_frame_capture_success_sequence = result.m_surface_capture_sequence;
            }
        }

        m_diagnostics.m_grid_observations = result.m_grid_observations;
        m_diagnostics.m_grid_staged_tiles = result.m_grid_staged_tiles;
        m_diagnostics.m_grid_cached_tiles = result.m_grid_cached_tiles;
        m_diagnostics.m_last_sweep_error = result.m_sweep_error;
        m_diagnostics.m_ui_font_version_applied = result.m_ui_font_version_applied;
        m_diagnostics.m_last_submit_value = result.m_submit_value;
    }

    // ------------------------------------------------------------------
    // stage settlement
    // ------------------------------------------------------------------
    void ApplicationEditor::discardUnsubmittedStages_() noexcept {
        if (m_grid_pass) {
            m_grid_pass->discardWorldLightingUnsubmitted();
            m_grid_pass->discardUnsubmittedParticles();
        }
        if (m_reconstruction) {
            m_reconstruction->discardUnsubmitted();
        }
        if (m_post_process) {
            m_post_process->discardUnsubmitted();
        }
    }

    void ApplicationEditor::settleSceneStage_(
        RenderFrameResult &out, const FenceCompletionValue before, const std::error_code error) noexcept {
        RenderStageResult &stage = out.m_stages[kRenderStageScene];
        stage.m_error = error;
        stage.m_settlement = FrameSettlement::kRejected;

        // Off-thread (affinity) rejection is invocation-local: the Renderer
        // leaves owner-confined settlement untouched, so classify from the
        // returned error and never consult the member fate.
        if (error == std::make_error_code(std::errc::operation_not_permitted)) {
            discardUnsubmittedStages_();
            return;
        }

        Renderer &renderer = getRenderer();
        const FenceCompletionValue after = renderer.lastSubmittedFenceValue();
        const FrameSettlement fate = renderer.lastSubmitSettlement();
        if (fate == FrameSettlement::kSubmitted and after != kFenceValueNone and after > before) {
            // Confirmed acceptance: commit A's state exactly once. Publication
            // consumers stamp the same value; the reconstruction pins the
            // scene writer even if B is rejected.
            stage.m_submission = after;
            stage.m_settlement = FrameSettlement::kSubmitted;
            m_grid_pass->notifyFrameSubmitted(after);
            m_triangle_pass->notifyFrameSubmitted(after);
            m_grid_pass->notifyWorldLightingSubmitted(after);
            m_reconstruction->notifySceneSubmitted(after);
            m_scene_depths[m_scene_depth_selected].m_last_use = std::max(
                m_scene_depths[m_scene_depth_selected].m_last_use, after);
            return;
        }

        if (fate == FrameSettlement::kSubmitFailed) {
            // Possibly executed: admission stops and dependent GPU storage is
            // quarantined. No retry, no in-place rollback, no discard.
            stage.m_settlement = FrameSettlement::kSubmitFailed;
            out.m_submit_possibly_executed = true;
            return;
        }

        // Definite pre-submit rejection: nothing for this stage is committed.
        discardUnsubmittedStages_();
    }

    void ApplicationEditor::settleResolveStage_(
        RenderFrameResult &out, const FenceCompletionValue before, const std::error_code error) noexcept {
        RenderStageResult &stage = out.m_stages[kRenderStageResolve];
        stage.m_error = error;
        stage.m_settlement = FrameSettlement::kRejected;
        if (m_reconstruction) {
            // A rejected/not-attempted B commits nothing: report the accepted
            // history role and explicitly clear the frame-local reset fact.
            out.m_reconstruction = m_reconstruction->historyStamp();
            out.m_reconstruction->m_frame = out.m_frame_id;
            out.m_reconstruction->m_reset = false;
        }

        if (error == std::make_error_code(std::errc::operation_not_permitted)) {
            m_reconstruction->discardUnsubmitted();
            m_post_process->discardUnsubmitted();
            return;
        }

        Renderer &renderer = getRenderer();
        const FenceCompletionValue after = renderer.lastSubmittedFenceValue();
        const FrameSettlement fate = renderer.lastSubmitSettlement();
        if (fate == FrameSettlement::kSubmitted and after != kFenceValueNone and after > before) {
            // Confirmed acceptance: the written history role/camera/phase and
            // the exposure role/active-time baseline commit together exactly
            // once. A's commits above remain untouched.
            stage.m_submission = after;
            stage.m_settlement = FrameSettlement::kSubmitted;
            m_reconstruction->notifySubmitted(after);
            m_post_process->notifySubmitted(after);
            if (m_reconstruction) {
                out.m_reconstruction = m_reconstruction->historyStamp();
                out.m_reconstruction->m_frame = out.m_frame_id;
            }
            return;
        }

        if (fate == FrameSettlement::kSubmitFailed) {
            stage.m_settlement = FrameSettlement::kSubmitFailed;
            out.m_submit_possibly_executed = true;
            return;
        }

        // Rejected B keeps A's commits and the previous screen-history role,
        // camera, phase and exposure baseline. Only the pending resolution is
        // discarded (a tentative history pair retires after A's scene readers).
        m_reconstruction->discardUnsubmitted();
        m_post_process->discardUnsubmitted();
    }

    void ApplicationEditor::settlePresentStage_(
        RenderFrameResult &out, const FenceCompletionValue before, const std::error_code error) noexcept {
        RenderStageResult &stage = out.m_stages[kRenderStagePresent];
        stage.m_error = error;
        stage.m_settlement = FrameSettlement::kRejected;

        if (error == std::make_error_code(std::errc::operation_not_permitted)) {
            return;
        }

        Renderer &renderer = getRenderer();
        const FenceCompletionValue after = renderer.lastSubmittedFenceValue();
        const FrameSettlement fate = renderer.lastSubmitSettlement();
        if (fate == FrameSettlement::kSubmitted and after != kFenceValueNone and after > before) {
            // Presentation evidence only: the picture's reader fence extends
            // through this submission, and no lighting/history/exposure state
            // is committed here.
            stage.m_submission = after;
            stage.m_settlement = FrameSettlement::kSubmitted;
            m_post_process->notifyPresented(after);
            if (auto *fence_aware = dynamic_cast<IFenceAwareUiService *>(m_ui_service.get())) {
                fence_aware->notifyFrameSubmitted(after);
            }
            return;
        }

        if (fate == FrameSettlement::kSubmitFailed) {
            stage.m_settlement = FrameSettlement::kSubmitFailed;
            out.m_submit_possibly_executed = true;
        }
    }

    // ------------------------------------------------------------------
    // lane submission and execution
    // ------------------------------------------------------------------

    std::error_code ApplicationEditor::submitRenderWork_(const FramePacket &packet) {
        // CLIENT lane (main): detach the UI frame, then hand the owned
        // packet to the worker — or run it inline when no lane owns the
        // Renderer. Either way the same execute step runs exactly once.
        if (not m_ui_service) [[unlikely]] {
            PPR_RETURN_ERROR_ON_FAIL(Editor, make_error_code(std::errc::not_connected));
        }

        // Complete the frontend frame even when minimization or a display
        // transition leaves no render target. The next update starts a new
        // ImGui frame regardless of whether this packet can reach the GPU.
        DetachedUiDrawData detached{};
        detached.m_frame_id = packet.m_frame_id;
        PPR_RETURN_ERROR_ON_FAIL(Editor, m_ui_service->buildDetachedUiFrame(detached));

        if (not hasRenderTarget(packet)) {
            // Durable slot outcome: no render target ⇒ dropped at admission,
            // and the staged payload never applied. The consumption hook
            // still fires so armed flags survive and no driver ack rides
            // a non-applied slot.
            RenderFrameResult transient{};
            transient.m_frame_id = packet.m_frame_id;
            transient.m_dropped = true;
            transient.m_reservation_epoch = reservationEpoch_();
            transient.m_reservation_frame_id = packet.m_frame_id;
            try {
                stampGridEcho(m_grid_stage, transient);
            } catch (const std::bad_alloc &) {
                throw; // The run boundary drains accepted work before fatal propagation.
            } catch (...) {
                transient.m_error = std::make_error_code(std::errc::state_not_recoverable);
                onLaneFrameConsumed_(transient);
                return transient.m_error;
            }
            onLaneFrameConsumed_(transient);
            return {};
        }
        if (renderLaneOwnsRenderer_()) {
            RenderFrameRequest request{};
            request.m_observe_grid = m_observe_grid;
            request.m_post = m_post_settings;
            request.m_pixel_scale = m_pixel_scale;
            request.m_inspection = m_inspection;
            request.m_sky_hour = m_sky_hour;
            request.m_capture_final_frame = m_frame_capture_requested and m_frame_capture_eligible;
            request.m_surface_capture_sequence = m_frame_capture_sequence;
            try {
                request.m_packet = std::make_shared<const FramePacket>(packet);
                request.m_ui = std::move(detached);
                request.m_grid_stage = std::move(m_grid_stage);
                request.m_triangle_stage = std::move(m_triangle_stage);
            } catch (const std::bad_alloc &) {
                throw; // The run boundary drains accepted work before fatal propagation.
            } catch (...) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
            // Tag the frame with its construction epoch + id so the lane
            // result posts back the same identity: the owner matches it
            // against the main-side reservation record and never releases
            // another run's credit on a floating/stale settlement.
            request.m_reservation_epoch = reservationEpoch_();
            request.m_reservation_frame_id = packet.m_frame_id;
            // Capture the per-slot settlement manifest BEFORE the request
            // (and its owned ack arrays) move across the transport or drop:
            // every never-applied outcome for this slot must round-trip the
            // same metadata so the owner re-arms armed flags and keeps the
            // dirty set pending.
            RenderFrameResult dropped_or_failed{};
            dropped_or_failed.m_frame_id = packet.m_frame_id;
            dropped_or_failed.m_reservation_epoch = reservationEpoch_();
            dropped_or_failed.m_reservation_frame_id = packet.m_frame_id;
            try {
                stampGridEcho(request.m_grid_stage, dropped_or_failed);
            } catch (const std::bad_alloc &) {
                throw; // The run boundary drains accepted work before fatal propagation.
            } catch (...) {
                dropped_or_failed.m_dropped = true;
                dropped_or_failed.m_error = std::make_error_code(std::errc::state_not_recoverable);
                onLaneFrameConsumed_(dropped_or_failed);
                return dropped_or_failed.m_error;
            }
            if (const std::error_code submit_err = renderLane()->submitFrame(std::move(request))) {
                dropped_or_failed.m_dropped = true;
                if (submit_err == std::make_error_code(std::errc::device_or_resource_busy)) {
                    if ((++m_lane_dropped_frames % 60u) == 1u) {
                        PPR_LOG(
                            Editor, info, "dropping frame on full render transport",
                            {
                                {"frame", packet.m_frame_id},
                                {"dropped", m_lane_dropped_frames},
                            });
                    }
                    onLaneFrameConsumed_(dropped_or_failed);
                    waitForAdmissionHint_();
                    return default_value_v;
                }
                // Non-busy transport failure (lane closed / cancelled): the
                // slot never settles through the lane, so fire the same
                // never-applied settlement here — no silent loss of the
                // armed flags or the ack manifest — then surface the error.
                dropped_or_failed.m_error = submit_err;
                onLaneFrameConsumed_(dropped_or_failed);
                return submit_err;
            }
            // Successful transport handoff: the record's queued half owns
            // the frame until its GPU submission settles. Definitely
            // unsubmitted transport outcomes never park here.
            noteReservationQueued_(packet.m_frame_id);
            return default_value_v;
        }

        // Inline reference: build the same request, then apply on main.
        RenderFrameRequest inline_request{};
        inline_request.m_observe_grid = m_observe_grid;
        inline_request.m_post = m_post_settings;
        inline_request.m_pixel_scale = m_pixel_scale;
        inline_request.m_inspection = m_inspection;
        inline_request.m_sky_hour = m_sky_hour;
        inline_request.m_capture_final_frame = m_frame_capture_requested and m_frame_capture_eligible;
        inline_request.m_surface_capture_sequence = m_frame_capture_sequence;
        try {
            inline_request.m_packet = std::make_shared<const FramePacket>(packet);
            inline_request.m_ui = std::move(detached);
            inline_request.m_grid_stage = std::move(m_grid_stage);
            inline_request.m_triangle_stage = std::move(m_triangle_stage);
        } catch (const std::bad_alloc &) {
            throw; // The run boundary drains accepted work before fatal propagation.
        } catch (...) {
            return std::make_error_code(std::errc::state_not_recoverable);
        }
        // Same reservation-identity tag as the lane request: the inline
        // frame settles through the same hook and must match its record.
        inline_request.m_reservation_epoch = reservationEpoch_();
        inline_request.m_reservation_frame_id = packet.m_frame_id;

        RenderFrameResult inline_result{};
        inline_result.m_frame_id = packet.m_frame_id;
        inline_result.m_reservation_epoch = reservationEpoch_();
        inline_result.m_reservation_frame_id = packet.m_frame_id;
        try {
            stampGridEcho(inline_request.m_grid_stage, inline_result);
        } catch (const std::bad_alloc &) {
            throw; // The run boundary drains accepted work before fatal propagation.
        } catch (...) {
            inline_result.m_dropped = true;
            inline_result.m_error = std::make_error_code(std::errc::state_not_recoverable);
            onLaneFrameConsumed_(inline_result);
            return inline_result.m_error;
        }
        std::error_code inline_err{};
        try {
            inline_err = executeRenderWork_(inline_request, inline_result);
        } catch (const std::bad_alloc &) {
            throw; // The run boundary drains accepted work before fatal propagation.
        } catch (...) {
            inline_err = std::make_error_code(std::errc::state_not_recoverable);
        }
        inline_result.m_error = inline_err;
        // The inline route settles the same owned result the lane would post:
        // its aggregate value and per-stage outcomes are already final in
        // `m_stages`. Any future inline settlement must consume the stage
        // record, not reintroduce a single-submission assumption here.
        m_diagnostics.m_grid_staged_tiles = inline_result.m_grid_staged_tiles;
        m_diagnostics.m_grid_cached_tiles = inline_result.m_grid_cached_tiles;
        m_diagnostics.m_last_sweep_error = inline_result.m_sweep_error;
        m_diagnostics.m_ui_font_version_applied = inline_result.m_ui_font_version_applied;
        m_diagnostics.m_last_submit_value = inline_result.m_submit_value;
        onLaneFrameConsumed_(inline_result);
        return inline_err;
    }

    std::error_code ApplicationEditor::submitGridWorld(std::shared_ptr<const GridWorldCommand> command) {
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, command ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        if (renderLaneOwnsRenderer_()) {
            return renderLane()->requestGridWorld(std::move(command));
        }
        GridWorldResult result{};
        result.m_kind = command->m_kind;
        result.m_sequence = command->m_sequence;
        result.m_job_id = command->m_job_id;
        result.m_world_generation = command->m_world_generation;
        result.m_content_fingerprint = command->m_content_fingerprint;
        result.m_error = executeGridWorld_(*command, result);
        onGridWorldCompleted_(result);
        return {};
    }

    std::error_code ApplicationEditor::executeGridWorld_(const GridWorldCommand &command, GridWorldResult &result) {
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, m_lane_rhi_service.isValid() ? std::error_code{} : std::make_error_code(std::errc::not_connected));

        const auto completed = getRenderer().completedFenceValue();
        if (command.m_kind != EGridWorldCommand::begin and command.m_kind != EGridWorldCommand::cancel) {
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, m_grid_pass->worldCandidateMatches(
                            command.m_job_id, command.m_world_generation, command.m_content_fingerprint)
                            ? std::error_code{}
                            : std::make_error_code(std::errc::operation_not_permitted));
        }
        switch (command.m_kind) {
            case EGridWorldCommand::begin:
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, command.m_stage.m_uploads.empty() and
                                    command.m_stage.m_tiles.size() <= GridPass::kGridTileCacheCapacity and
                                    command.m_content_fingerprint == command.m_stage.m_content_fingerprint and
                                    (not command.m_final_batch or command.m_stage.m_tiles.empty())
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));
                if (not m_grid_pass->worldCandidateAdmissionReady(completed)) {
                    // The previous world is still retiring; retain the command and retry.
                    return std::make_error_code(std::errc::device_or_resource_busy);
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, m_grid_pass->stageWorldCandidate(
                                command.m_job_id, command.m_world_generation, command.m_stage, command.m_final_batch));

                result.m_applied = true;
                return {};
            case EGridWorldCommand::append:
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, m_grid_pass->appendWorldCandidate(
                                command.m_job_id, command.m_world_generation, command.m_stage.m_uploads,
                                command.m_final_batch));
                result.m_applied = true;
                break;
            case EGridWorldCommand::advance: break;
            case EGridWorldCommand::activate:
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, m_grid_pass->activateWorldCandidate(
                                command.m_job_id, command.m_world_generation, command.m_content_fingerprint));
                result.m_applied = true;
                return {};
            case EGridWorldCommand::cancel: {
                if (not m_grid_pass->worldCandidateMatches(
                        command.m_job_id, command.m_world_generation, command.m_content_fingerprint)) {
                    result.m_applied = false;
                    return {};
                }

                m_grid_pass->cancelWorldCandidate(command.m_job_id);
                result.m_applied = true;
                return {};
            }
            default: PPR_RETURN_ERROR_ON_FAIL(Editor, std::errc::invalid_argument);
        }

        const auto ready = m_grid_pass->advanceWorldCandidate(m_lane_rhi_service->getDevice(), completed);
        PPR_RETURN_ERROR_ON_FAIL(Editor, ready);

        result.m_applied = true;
        result.m_ready = *ready;
        return {};
    }

    std::error_code ApplicationEditor::executeRenderWork_(
        const RenderFrameRequest &request, RenderFrameResult &out_result) {
        out_result.m_presented_grid = {};
        out_result.m_presented_particles.reset();
        out_result.m_particle_census.reset();
        out_result.m_surface_capture_sequence = 0u;
        out_result.m_stages = {};

        const ITimerClock *const clock = request.m_observe_grid ? &observationClock() : nullptr;
        RenderCpuObservations cpu{};
        if (clock) {
            cpu.m_frame_id = request.m_packet->m_frame_id;
            cpu.m_start = clock->now();
        }
        PPR_DEFER {
            if (clock) {
                cpu.m_end = clock->now();
                out_result.m_cpu_observations = cpu;
            }
        };
        Renderer &renderer = getRenderer();
        GridWorldIdentity active_identity{};

        PPR_DEFER {
            out_result.m_gpu_timing = renderer.lastGpuFrameTiming();
            if (request.m_observe_grid and m_grid_pass) {
                out_result.m_grid_observations = m_grid_pass->observations();
            }
        };
        // Tests-only per-stage submit-failure seam: consumed by exactly this
        // executed frame. If the frame never reaches the armed stage (A was
        // rejected, ambiguity stopped admission, the packet dropped), the
        // injection is discarded with the frame and cannot leak into a later
        // one. Production never arms it.
        const std::optional<u32> injected_stage = std::exchange(m_test_stage_failure, std::nullopt);

        const FramePacket &packet = *request.m_packet;
        const DetachedUiDrawData &ui = request.m_ui;
        const GridPass::GridStagedFrame &grid_stage = request.m_grid_stage;
        const TrianglePass::TriangleStagedFrame &triangle_stage = request.m_triangle_stage;
        out_result.m_frame_id = packet.m_frame_id;
        out_result.m_reservation_epoch = request.m_reservation_epoch;
        out_result.m_reservation_frame_id = request.m_reservation_frame_id;
        out_result.m_active_elapsed = packet.m_active_elapsed;
        stampGridEcho(grid_stage, out_result);

        // The independent resolve and post owners are lane-owned: a request
        // without a live pass (initialization failed or teardown raced) fails
        // closed.
        if (not m_post_process or not m_reconstruction) [[unlikely]] {
            PPR_RETURN_ERROR_ON_FAIL(Editor, make_error_code(std::errc::not_connected));
        }

        // The lane-confined rhi service captured at startup is the only legal
        // source on this (possibly worker) thread: never read the service store
        // here — that hidden-singleton lookup would race the main thread's
        // teardown erase. Fail closed when it is invalid.
        const safe_ptr<IRhiService> rhi_service = m_lane_rhi_service;
        if (not rhi_service.isValid()) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        // Non-rendering outcome for zero-size/minimized surfaces: skip uploads
        // and submission, report success. This is also the empty-packet
        // fail-closed: a packet with no render target performs zero backend
        // work. No stage is attempted, so no consumption ack: the payload is
        // re-staged next buildable frame.
        if (not hasRenderTarget(packet)) {
            return default_value_v;
        }

        // Generational rejection before ANY backend work (uploads fence the
        // queue, so they must not run for a stale packet): a packet built for
        // an older surface shape retires here as an explicit non-rendering
        // outcome.
        const SurfaceIdentity packet_surface{
            .m_handle = packet.m_surface_identity,
            .m_generation = packet.m_surface_generation,
        };
        if (renderer.isSurfaceWorkObsolete(packet_surface)) {
            PPR_LOG(
                Editor, info, "dropping packet for obsolete surface generation",
                {
                    {"frame", packet.m_frame_id},
                    {"packet_generation", packet.m_surface_generation},
                });
            return default_value_v;
        }

        // Post settings are a pass precondition; the presentation grid applies
        // the retained P1..8 resolution control to the real framebuffer extent.
        PPR_RETURN_ERROR_ON_FAIL(Editor, validatePostProcessSettings(request.m_post));
        PPR_RETURN_ERROR_ON_FAIL(Editor, validateLightingInspectionSettings(request.m_inspection));
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, request.m_sky_hour >= -1 and request.m_sky_hour <= 23
                        ? std::error_code{}
                        : std::make_error_code(std::errc::invalid_argument));
        const auto presentation = makePresentationGrid(packet.m_framebuffer_size, request.m_pixel_scale);
        PPR_RETURN_ERROR_ON_FAIL(Editor, presentation ? std::error_code{} : presentation.error());
        const PresentationGrid grid = *presentation;

        // A numerical buffer can become the solver's next candidate after
        // publication; stale draws are rejected below.
        if (not m_grid_pass->gpuParticleFrameCurrent(grid_stage.m_gpu_particles)) {
            out_result.m_dropped = true;
            return {};
        }

        // Completed numeric-status inspection and bounded retirements ride the
        // ordinary frame boundary. A completed numeric failure stops further
        // admission with its category; it is never reported as success.
        PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->collectWorldLightingCompleted(renderer.completedFenceValue()));
        PPR_RETURN_ERROR_ON_FAIL(Editor, m_reconstruction->collectCompleted(renderer.completedFenceValue()));
        std::ignore = m_post_process->collectCompleted(renderer.completedFenceValue());

        // ------------------------------------------------------------------
        // frame preparation (before stage A arms its owners)
        // ------------------------------------------------------------------
        rhi::IDevice &device = rhi_service->getDevice();
        {
            const hal::ProfileScope scope_frame_prepare{"PPR.Frame.PrepareRender"};
            const CpuObservationScope cpu_staging{clock, cpu.m_staging};
            const u64 active_generation = m_grid_pass->worldGeneration();
            if (not grid_stage.m_retain_content and
                (active_generation == 0u or grid_stage.m_ack_seed == active_generation)) {
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->applyStagedFrame(grid_stage));
                out_result.m_grid_staging_applied = true;
            }
            if (not grid_stage.m_retain_content or grid_stage.m_update_actors_on_retained_terrain) {
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_triangle_pass->applyStagedFrame(triangle_stage));
            }

            // Publish this request's preflighted lease before admitting its
            // content. An empty replacement supersedes an older nonempty view
            // without requiring an intervening frame.
            if (grid_stage.m_gpu_particles) {
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->applyGpuParticleView(*grid_stage.m_gpu_particles));
            }

            {
                const hal::ProfileScope scope_uploads{"App.Editor.PrepareUploads"};
                const CpuObservationScope cpu_uploads{clock, cpu.m_uploads};
                const auto published = m_grid_pass->tryPrepareCellUploads(device, renderer.completedFenceValue());
                PPR_RETURN_ERROR_ON_FAIL(Editor, published);
                out_result.m_grid_stage_applied = out_result.m_grid_staging_applied and *published;

                // Resident particles and their clipping terrain form one
                // cohort. A partial terrain upload cannot draw the new
                // numerical view.
                if (grid_stage.m_gpu_particles and not *published) {
                    return {};
                }
            }

            // Stage A/B inputs are selected here, before any attachment is
            // evaluated: reconstruction proposes the padded/jittered scene
            // camera and the next screen-history write; post selects its own
            // resolved output cohort (so outputTexture() is valid below).
            active_identity = m_grid_pass->activeWorldIdentity();
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, m_reconstruction->beginFrame(
                            device, packet.m_camera, grid, active_identity, packet.m_render_camera_cut,
                            renderer.completedFenceValue()));

            const std::error_code post_begin = m_post_process->beginFrame(
                device, grid.m_resolved, active_identity, renderer.completedFenceValue());
            if (post_begin) {
                m_reconstruction->discardUnsubmitted();
                if (post_begin == std::make_error_code(std::errc::device_or_resource_busy)) {
                    out_result.m_dropped = true;
                    return {};
                }
                return post_begin;
            }

            // Depth targets are per ring slot: the slot this frame rotates onto
            // must not be rewritten before its previous writer completed.
            std::array<FenceCompletionValue, kFrameRingSlots> depth_values{};
            for (u32 index = 0u; index < kFrameRingSlots; ++index) {
                depth_values[index] = m_scene_depths[index].m_last_use;
            }

            const u32 depth_slot = selectRingSlot(renderer.completedFenceValue(), depth_values, m_scene_depth_cursor);
            if (not slotFenceComplete(renderer.completedFenceValue(), depth_values[depth_slot])) {
                discardUnsubmittedStages_();
                out_result.m_dropped = true;
                return {};
            }

            m_scene_depth_cursor = depth_slot;
            m_scene_depth_selected = depth_slot;

            // The reconstruction owner supplies the padded/jittered camera to
            // the scene passes; post-processing consumes only the resolved
            // content rectangle.
            const CameraSnapshot &scene_camera = m_reconstruction->sceneCamera();
            const std::error_code triangle_update = m_triangle_pass->update(packet.m_delta_time, scene_camera);
            if (triangle_update) {
                discardUnsubmittedStages_();
                return triangle_update;
            }

            // Sample-rate categorical shading on the real 4x MSAA target. No
            // alternate AA path exists; an unsupported device fails here.
            const std::error_code grid_update = m_grid_pass->update(packet.m_delta_time, scene_camera, true);
            if (grid_update) {
                discardUnsubmittedStages_();
                return grid_update;
            }

            const std::error_code attachments = resolveStageAttachments_(device, grid.m_raster);
            if (attachments) {
                discardUnsubmittedStages_();
                return attachments;
            }
        }
        const CameraSnapshot scene_camera = m_reconstruction->sceneCamera();
        out_result.m_grid_staged_tiles = m_grid_pass->stagedTileCount();
        out_result.m_grid_cached_tiles = m_grid_pass->cachedTileCount();

        // ------------------------------------------------------------------
        // stage A: publication, lighting, scene and gas
        // ------------------------------------------------------------------
        const rhi::Viewport scene_viewport = rhi::Viewport::fromSize(
            static_cast<float>(grid.m_raster.x), static_cast<float>(grid.m_raster.y));
        const rhi::ScissorRect scene_scissor = rhi::ScissorRect::fromSize(
            safe_narrowing(grid.m_raster.x), safe_narrowing(grid.m_raster.y));

        // Named local closures: DrawSubmission stores a function_ref to them,
        // so a temporary closure would dangle the moment the full-expression
        // ends (ASan stack-use-after-scope at encode time).
        const auto scene_content_draw = [&](const DrawContext &context) -> std::error_code {
            {
                const CpuObservationScope cpu_prepare{clock, cpu.m_triangle_draw};
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, m_triangle_pass->prepare(context.m_device, context.m_render_pipeline_key));
            }

            {
                const CpuObservationScope cpu_draw{clock, cpu.m_grid_draw};
                // Plan §3 draw order: background, particles (opaque then
                // optical), opaque terrain, other opaque work, gas, translucent
                // terrain. Particles live on z = 0 with the terrain, so they are
                // drawn first and terrain's LessEqual test wins every depth tie;
                // the optical particle class writes no depth, the opaque one
                // does. The gas lane keeps its existing depth-disabled slot and
                // blend. Do not rearrange this for profiling.
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderBackground(context));
                {
                    const hal::ProfileScope scope{"Scene.Particles"};
                    context.m_pass.pushDebugGroup("Scene.Particles", {});
                    PPR_DEFER {
                        context.m_pass.popDebugGroup();
                    };
                    PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderSolidLiquidParticles(context));
                }
                {
                    const hal::ProfileScope scope{"Scene.Materials"};
                    context.m_pass.pushDebugGroup("Scene.Materials", {});
                    PPR_DEFER {
                        context.m_pass.popDebugGroup();
                    };
                    PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderGpuIndirect(context));
                }
                {
                    const hal::ProfileScope scope{"Scene.Gas"};
                    context.m_pass.pushDebugGroup("Scene.Gas", {});
                    PPR_DEFER {
                        context.m_pass.popDebugGroup();
                    };
                    PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderGasParticles(context));
                }
                {
                    const hal::ProfileScope scope{"Scene.Materials"};
                    context.m_pass.pushDebugGroup("Scene.Materials", {});
                    PPR_DEFER {
                        context.m_pass.popDebugGroup();
                    };
                    PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderTranslucent(context));
                }
            }

            const CpuObservationScope cpu_draw{clock, cpu.m_triangle_draw};
            return m_triangle_pass->render(context);
        };
        DrawSubmission scene_draws{string_literal{std::in_place, "scene.content"}, scene_content_draw};
        scene_draws.m_viewport = scene_viewport;
        scene_draws.m_scissor = scene_scissor;

        bool lighting_armed{};
        const auto scene_prepare = [&](const PreRenderContext &context) -> std::error_code {
            const CpuObservationScope cpu_prepare{clock, cpu.m_grid_prepare};
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, m_grid_pass->prepareBackgroundInputs(context, scene_camera, scene_viewport, scene_scissor));

            // World lighting is armed only when the admitted world supplies its
            // publication owners. A world with no published catalog (the
            // loading presentation before activation, or an unsupported world)
            // keeps the documented `m_has_resolved_cells == 0` flat fallback
            // and commits no lighting phase; the first frame whose world
            // publishes its catalog prepares and commits A's lighting state.
            //
            // The cached backdrop follows this prepare in the same submission
            // so it samples this attempt's fine CH write.
            if (m_grid_pass->lightingInputsAvailable()) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, m_grid_pass->prepareWorldLighting(
                                context, WorldLightingFrame{
                                             .m_active_elapsed = packet.m_active_elapsed,
                                             .m_sky_hour = request.m_sky_hour,
                                         }));
                lighting_armed = true;
            }

            PPR_RETURN_ERROR_ON_FAIL(
                Editor, m_grid_pass->prepareBackground(context, scene_camera, scene_viewport, scene_scissor));
            PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->prepareGpuVisibility(context));
            PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->prepareGpuParticles(context));
            return {};
        };

        // Submission-local numeric status copies observe this attempt's draws;
        // the tail runs after the render pass ends and before submit.
        const auto scene_finalize = [&](const PreRenderContext &context) -> std::error_code {
            return lighting_armed ? m_grid_pass->finalizeWorldLighting(context) : std::error_code{};
        };

        const std::array<rhi::RenderPassColorAttachment, 1> scene_colors{
            rhi::RenderPassColorAttachment{.view = m_scene_color_view, .clearValue = {0.1f, 0.1f, 0.2f, 1.0f}}};
        rhi::RenderPassDepthStencilAttachment scene_depth{};
        scene_depth.view = m_scene_depths[m_scene_depth_selected].m_view;
        scene_depth.depthClearValue = 1.0f;
        rhi::RenderPassDesc scene_pass_desc{};
        scene_pass_desc.colorAttachments = scene_colors.data();
        scene_pass_desc.colorAttachmentCount = 1u;
        scene_pass_desc.depthStencilAttachment = &scene_depth;

        std::error_code frame_error{};
        {
            const hal::ProfileScope scope_frame_execute{"PPR.Frame.Execute"};
            const CpuObservationScope cpu_surface{clock, cpu.m_surface};
            if (injected_stage == kRenderStageScene) {
                renderer.setInjectedSubmitFailure(true);
            }
            const FenceCompletionValue before = renderer.lastSubmittedFenceValue();
            try {
                frame_error = renderer.render(
                    string_literal{std::in_place, "scene.stage"}, scene_pass_desc, {scene_draws},
                    PreRenderCallback{scene_prepare}, PostRenderCallback{scene_finalize});
            } catch (const std::bad_alloc &) {
                throw; // The run boundary drains accepted work before fatal propagation.
            } catch (...) {
                frame_error = std::make_error_code(std::errc::state_not_recoverable);
            }
            settleSceneStage_(out_result, before, frame_error);
        }

        // ------------------------------------------------------------------
        // stage B: Hable resolve and independent post-processing
        // ------------------------------------------------------------------
        if (out_result.m_stages[kRenderStageScene].m_settlement == FrameSettlement::kSubmitted) {
            const auto resolve_prepare = [&](const PreRenderContext &context) -> std::error_code {
                // The reconstruction prepare selects and encodes the next
                // history image; only then is its resolved view evaluated for
                // the post pass in the same submission.
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_reconstruction->prepare(context));
                return m_post_process->prepare(
                    context, m_reconstruction->resolved(), request.m_post, packet.m_active_elapsed);
            };
            const auto resolve_finalize = [&](const PreRenderContext &context) -> std::error_code {
                std::error_code first{};
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first, m_reconstruction->finalize(context));
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first, m_post_process->finalize(context));
                return first;
            };
            const auto resolved_draw = [&](const DrawContext &context) -> std::error_code {
                return m_post_process->render(context);
            };
            DrawSubmission resolve_draws{string_literal{std::in_place, "Post.AgX"}, resolved_draw};

            rhi::ITexture &output_texture = m_post_process->outputTexture();
            const CpuObservationScope cpu_surface{clock, cpu.m_surface};
            if (injected_stage == kRenderStageResolve) {
                renderer.setInjectedSubmitFailure(true);
            }
            const FenceCompletionValue before = renderer.lastSubmittedFenceValue();
            try {
                frame_error = renderer.renderToTexture(
                    output_texture, {resolve_draws},
                    ColorAttachmentOps{.m_clear_color = float4{0.0f, 0.0f, 0.0f, 1.0f}},
                    PreRenderCallback{resolve_prepare}, PostRenderCallback{resolve_finalize});
            } catch (const std::bad_alloc &) {
                throw; // The run boundary drains accepted work before fatal propagation.
            } catch (...) {
                frame_error = std::make_error_code(std::errc::state_not_recoverable);
            }
            settleResolveStage_(out_result, before, frame_error);
        }

        // ------------------------------------------------------------------
        // stage C: fixed-P presentation and native UI
        // ------------------------------------------------------------------
        // Presentation of a new picture requires an accepted stage B. When B
        // was rejected, a retained picture (the last accepted output for this
        // world and extent) may still be presented: that extends its reader
        // fence and commits no lighting/history/exposure state.
        //
        // A possibly-executed submit stops admission: C is never attempted
        // there. Submitting after an ambiguous A/B would be refused anyway, and
        // selecting a retained picture first would mutate the post pass's
        // selection outside any submission. The retained-picture selection is
        // therefore made only when C will actually be submitted.
        bool present_picture = not out_result.m_submit_possibly_executed and
                               out_result.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kSubmitted;
        const bool fresh_picture = present_picture;
        if (not present_picture and
            not out_result.m_submit_possibly_executed and
            m_post_process->retainedPictureAvailable(grid.m_resolved, active_identity)) {
            m_post_process->selectRetainedPicture(grid.m_resolved);
            present_picture = true;
        }

        if (present_picture) {
            const auto present_draw = [&](const DrawContext &context) -> std::error_code {
                return m_post_process->present(context, grid);
            };
            const auto ui_encode = [&](const DrawContext &draw_context) -> std::error_code {
                const CpuObservationScope cpu_ui{clock, cpu.m_ui_draw};
                PPR_ASSERT(m_ui_service != nullptr);
                return m_ui_service->renderDetachedUiFrame(ui, draw_context);
            };
            DrawSubmission present_submission{string_literal{std::in_place, "Present.Scene"}, present_draw};
            DrawSubmission ui_draws{string_literal{std::in_place, "Present.UI"}, ui_encode};

            // The retained inspection overlay is read-only C-stage work drawn
            // from the same submitted world/camera as a fresh picture. A
            // retained picture carries no matching current world state, so it
            // is presented without an overlay instead of fabricating one. The
            // current frame must also have armed lighting: a stale published
            // attempt from a publication-failure window is not this frame's
            // submitted state.
            const bool inspection_active = fresh_picture and
                                           lighting_armed and
                                           request.m_inspection.m_diagnostic != ELightingDiagnostic::off and
                                           m_grid_pass->inspectionAvailable();
            const auto inspect_draw = [&](const DrawContext &context) -> std::error_code {
                return m_grid_pass->renderWorldLightingInspection(context, packet.m_camera, request.m_inspection);
            };
            DrawSubmission inspection_submission{string_literal{std::in_place, "lighting.inspect"}, inspect_draw};

            // Named initializer-list backing arrays: SurfaceRenderPass borrows
            // the draw list, so an inline `{submission}` temporary inside an
            // assigned aggregate would dangle after its statement.
            const std::initializer_list<const DrawSubmission> present_draws{present_submission};
            const std::initializer_list<const DrawSubmission> inspection_draws{inspection_submission};
            const std::initializer_list<const DrawSubmission> ui_draw_list{ui_draws};

            SurfaceRenderPass present_pass{};
            // Presentation samples the resolved output as a fullscreen blit,
            // so the surface pass owns no depth attachment.
            present_pass.m_depth_policy = ESurfaceDepthPolicy::none;
            present_pass.m_draws = present_draws;

            SurfaceRenderPass inspection_pass{};
            inspection_pass.m_surface_color = ColorAttachmentOps{.m_load_op = rhi::LoadOp::Load};
            // Read-only diagnostic colors over the presented picture; no depth
            // and no blend.
            inspection_pass.m_depth_policy = ESurfaceDepthPolicy::none;
            inspection_pass.m_draws = inspection_draws;

            SurfaceRenderPass ui_pass{};
            ui_pass.m_surface_color = ColorAttachmentOps{.m_load_op = rhi::LoadOp::Load};
            // Detached UI draws, constructed synchronously on this lane
            // (inline or worker): the encode reads ONLY the detached data,
            // never the live ImGui frontend.
            ui_pass.m_draws = ui_draw_list;

            std::array<SurfaceRenderPass, 3u> surface_passes{present_pass, inspection_pass, ui_pass};
            u32 pass_count = 2u;
            if (inspection_active) {
                ++pass_count;
            } else {
                surface_passes[1u] = ui_pass;
            }
            const SurfaceWindowSnapshot window_snapshot{
                .m_handle = packet.m_surface_identity,
                .m_native = packet.m_surface_native,
                .m_display = packet.m_surface_display,
                .m_framebuffer_size = packet.m_framebuffer_size,
                .m_title = packet.m_window_title,
            };

            // One-shot capture admission: a duplicate packet queued behind the
            // first attempt carries the same sequence and never attempts twice.
            const bool capture = request.m_capture_final_frame and
                                 request.m_surface_capture_sequence > m_frame_capture_attempted_sequence;
            bool capture_image_acquired{};
            const u64 previous_capture_sequence = m_frame_capture_attempted_sequence;
            if (capture) {
                m_frame_capture_attempted_sequence = request.m_surface_capture_sequence;
                out_result.m_surface_capture_attempted = true;
                out_result.m_surface_capture_sequence = request.m_surface_capture_sequence;
            }

            if (injected_stage == kRenderStagePresent) {
                renderer.setInjectedSubmitFailure(true);
            }
            const FenceCompletionValue before = renderer.lastSubmittedFenceValue();
            {
                const CpuObservationScope cpu_surface{clock, cpu.m_surface};
                try {
                    frame_error = renderer.renderAndPresent(
                        window_snapshot, {surface_passes.data(), pass_count}, packet_surface, &out_result.m_presented,
                        capture ? &out_result.m_surface_pixels : nullptr, capture ? &capture_image_acquired : nullptr);
                } catch (const std::bad_alloc &) {
                    throw; // The run boundary drains accepted work before fatal propagation.
                } catch (...) {
                    frame_error = std::make_error_code(std::errc::state_not_recoverable);
                }
            }
            settlePresentStage_(out_result, before, frame_error);

            // Accepted inspection work extends the lighting owner's reader
            // fence: the attempt's status/jump/mask slot must stay valid until
            // this C submission completed. A rejected C proves nothing and
            // extends nothing.
            const RenderStageResult &present_stage = out_result.m_stages[kRenderStagePresent];
            if (inspection_active and present_stage.m_settlement == FrameSettlement::kSubmitted) {
                m_grid_pass->notifyWorldLightingReadSubmitted(present_stage.m_submission);
                out_result.m_inspection = LightingInspectionStamp{
                    .m_settings = request.m_inspection,
                    .m_world = active_identity,
                    .m_camera_revision = packet.m_camera.m_revision,
                    .m_submission = present_stage.m_submission,
                };
            }

            if (capture) {
                // A capture stays armed across known pre-acquire backpressure
                // only when no stage of this attempt was accepted: a rejected
                // settlement with no fence advance and no acquisition. It
                // never retries an accepted/unknown submit.
                const bool no_accepted_stage = out_result.m_stages[kRenderStageScene].m_submission ==
                                                   kFenceValueNone and
                                               out_result.m_stages[kRenderStageResolve].m_submission ==
                                                   kFenceValueNone and
                                               out_result.m_stages[kRenderStagePresent].m_submission == kFenceValueNone;
                const bool pre_acquire_transient = not capture_image_acquired and
                                                   not out_result.m_presented and
                                                   not out_result.m_surface_pixels and
                                                   no_accepted_stage and
                                                   not out_result.m_submit_possibly_executed and
                                                   (not frame_error or
                                                       frame_error == std::errc::device_or_resource_busy or
                                                       frame_error == std::errc::resource_unavailable_try_again);
                if (pre_acquire_transient) {
                    m_frame_capture_attempted_sequence = previous_capture_sequence;
                    out_result.m_surface_capture_attempted = false;
                    out_result.m_surface_capture_sequence = 0u;
                } else {
                    out_result.m_surface_capture_error = frame_error;
                    if (not frame_error and (not out_result.m_presented or not out_result.m_surface_pixels)) {
                        out_result.m_surface_capture_error = std::make_error_code(std::errc::io_error);
                    }
                }
            }
        }

        // ------------------------------------------------------------------
        // result aggregation
        // ------------------------------------------------------------------
        // The aggregate accepted value is the latest accepted stage, retained
        // across a later rejection. A never-attempted or rejected frame keeps
        // kFenceValueNone.
        out_result.m_submit_value = aggregateAcceptedSubmission(out_result.m_stages);
        out_result.m_surface_handle = packet.m_surface_identity;
        out_result.m_surface_generation = renderer.surfaceGenerationFor(packet.m_surface_identity);
        if (m_ui_service) {
            out_result.m_ui_font_version_applied = m_ui_service->installedFontVersion();
        }

        // The first failing stage/error remains explicit. Ordinary capacity
        // backpressure is a dropped presentation, never an app failure, and a
        // replaced stage is never retried or committed in place.
        std::error_code stage_failure{};
        for (const RenderStageResult &stage: out_result.m_stages) {
            if (stage.m_error and not stage_failure) {
                stage_failure = stage.m_error;
            }
        }
        if (stage_failure == std::make_error_code(std::errc::resource_unavailable_try_again) or
            stage_failure == std::make_error_code(std::errc::device_or_resource_busy)) {
            // A later stage was definitely rejected by ordinary backpressure:
            // earlier accepted stages stand and the frame is not presented.
            // This is a dropped submission stage, distinct from a frame that
            // never had a render target or an obsolete surface claim.
            [[maybe_unused]] u32 dropped_stage = kRenderStageCount;
            for (u32 stage = 0u; stage < kRenderStageCount; ++stage) {
                if (out_result.m_stages[stage].m_error == stage_failure) {
                    dropped_stage = stage;
                    break;
                }
            }
            PPR_LOG(
                Editor, info, "dropped frame at stage rejection",
                {
                    {"frame", packet.m_frame_id},
                    {"stage", dropped_stage},
                    {"accepted", out_result.m_submit_value},
                    {"error", stage_failure.message()},
                });
            return default_value_v;
        }
        PPR_RETURN_ERROR_ON_FAIL(Editor, stage_failure);

        if (out_result.m_presented) {
            out_result.m_presented_grid = m_grid_pass->activeWorldIdentity();
            out_result.m_presented_particles = m_grid_pass->activeParticlePresentation();
        }

        return default_value_v;
    }
}
