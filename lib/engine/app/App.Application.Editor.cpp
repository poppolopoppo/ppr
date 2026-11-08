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
import :renderer.retro_pass;
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
import engine.mesh;
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

    } // namespace

    // ------------------------------------------------------------------
    // editor-owned world light field (field mode)
    // ------------------------------------------------------------------

    struct ApplicationEditor::FieldLightingState {
        lighting::WorldLightSourceGpu m_source{};
        lighting::WorldLightField m_field{};
        lighting::WorldLightPlan m_plan{};
        u32 m_budget{};
        u32 m_accumulation_samples{8u};
        float m_history_retention{0.9f};
        bool m_initialized{};
        bool m_frame_logged{};
    };

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

        m_retro_pass = std::make_unique<RetroPass>();
        const std::error_code retro_err = m_retro_pass->initialize(*rhi_service, *shader_service, getContentDir());
        if (retro_err) [[unlikely]] {
            // Mirror the grid failure path: release whatever the partial
            // initialization created and retain the original error.
            std::error_code cleanup_err{};
            PPR_RETAIN_ERROR_ON_FAIL(Editor, cleanup_err, m_retro_pass->shutdown());
            m_retro_pass.reset();
            PPR_RETURN_ERROR_ON_FAIL(Editor, retro_err);
        }

        m_ui_service = ui::createImGuiService();

        // Default Site Office face slots: Pixelify body/emphasis, then Big
        // Shoulders headings, then the body family again at the smaller size
        // the Inspector read-out binds (ui::theme::EFont::tiny). Font Awesome
        // merges into the body, so it does not consume a separate slot. Keep
        // ui::theme::font's contract in sync.
        // Static pinned artifacts are staged beside the executable. The
        // digests pin the exact bytes: a missing or altered file fails UI
        // initialization instead of silently substituting a default font.
        const std::filesystem::path font_root = getContentDir().path() / "fonts";
        const std::array font_sources{
            UIFontSource{
                .m_path = font_root / "PixelifySans-Regular.ttf",
                .m_expected_bytes = 93780u,
                .m_size_pixels = 16.0f,
                .m_sha256 = sha256DigestFromHex("5436d485d1713afa376e1744911cbbcc0849eae1eb55ec9bc34c0753993ffef9"),
            },
            UIFontSource{
                .m_path = font_root / "Font Awesome 7 Free-Solid-900.otf",
                .m_merge_glyphs = ui::icons::kMergeRanges,
                .m_expected_bytes = 414704u,
                .m_size_pixels = 16.0f,
                .m_sha256 = sha256DigestFromHex("daa92774861a14e9ccbccc5e73a5fba514271ba4d1bd7e5adbf952182bc00835"),
                .m_merge_into_previous = true,
            },
            UIFontSource{
                .m_path = font_root / "PixelifySans-SemiBold.ttf",
                .m_expected_bytes = 97492u,
                .m_size_pixels = 16.0f,
                .m_sha256 = sha256DigestFromHex("a4b54982991cd47450df317a451a7f066bf6b91aa033e105643aba2cf7bd35c3"),
            },
            UIFontSource{
                .m_path = font_root / "BigShouldersDisplay-Bold.ttf",
                .m_expected_bytes = 122896u,
                .m_size_pixels = 22.0f,
                .m_sha256 = sha256DigestFromHex("cbfe03f89e2bb3aba487644cdaf9abc9b3eb59946efaf894dda5cd05e1690c83"),
            },
            // The compact read-out binding: the same pinned body file at a
            // smaller size, so no new artifact or licence enters the set.
            UIFontSource{
                .m_path = font_root / "PixelifySans-Regular.ttf",
                .m_expected_bytes = 93780u,
                .m_size_pixels = 12.0f,
                .m_sha256 = sha256DigestFromHex("5436d485d1713afa376e1744911cbbcc0849eae1eb55ec9bc34c0753993ffef9"),
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

    void ApplicationEditor::setRetroSettings(const RetroSettings &settings) noexcept {
        // CPU-only policy set: no GPU work and no frame drain. Clamping once
        // here keeps every owned frame request value-valid; the next submit
        // copies the stored settings onto both request paths.
        m_retro_settings = settings;
        m_retro_settings.m_pixel_scale = RetroPass::clampPixelScale(settings.m_pixel_scale);
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
                            if (m_retro_pass) {
                                PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_retro_pass->notifyDeviceLost());
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
                        if (m_retro_pass) {
                            PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_retro_pass->shutdown());
                        }
                        if (m_field_lighting and m_field_lighting->m_initialized) {
                            PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_field_lighting->m_field.shutdown());
                            PPR_RETAIN_ERROR_ON_FAIL(Editor, lane_err, m_field_lighting->m_source.shutdown());
                            m_field_lighting.reset();
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
            m_retro_pass.reset();
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
            if (m_retro_pass) {
                PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_retro_pass->notifyDeviceLost());
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

        if (m_retro_pass and not lane_releases_done) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_retro_pass->shutdown());
            m_retro_pass.reset();
        }

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
        m_diagnostics.m_lighting_pending = result.m_lighting_pending;
        m_diagnostics.m_lighting_update_age = result.m_lighting_update_age;
        m_diagnostics.m_lighting_error = result.m_lighting_error;
        if (result.m_presented and result.m_lighting_camera) {
            m_diagnostics.m_lighting_camera = result.m_lighting_camera;
            m_diagnostics.m_lighting_extent = result.m_lighting_extent.value_or(int2{zero_v});
            m_diagnostics.m_lighting_target_extent = result.m_lighting_target_extent.value_or(int2{zero_v});
            m_diagnostics.m_lighting_world = result.m_lighting_world;
            m_diagnostics.m_lighting_revision = result.m_lighting_revision;
        }
    }

    void ApplicationEditor::stampSubmittedPasses_(const FenceCompletionValue previously_submitted) noexcept {
        // Submission acceptance is independent of present: the Renderer
        // commits the SUBMIT's value before presenting, so an accepted
        // submit advances lastSubmitted even when the present below it
        // fails. Stamp ALL participants off the advancement — never off
        // the present verdict — so ring-slot bookkeeping agrees with the
        // fence even on present failure. A rejected or quarantined frame
        // advances nothing and stamps nothing.
        Renderer &renderer = getRenderer();
        const FenceCompletionValue accepted = renderer.lastSubmittedFenceValue();
        if (accepted == previously_submitted or accepted == kFenceValueNone) {
            return;
        }
        if (m_scene_passes_encoded) {
            m_grid_pass->notifyFrameSubmitted(accepted);
            m_triangle_pass->notifyFrameSubmitted(accepted);
        }
        if (m_retro_pass) {
            m_retro_pass->notifyFrameSubmitted(accepted);
        }
        if (auto *fence_aware = dynamic_cast<IFenceAwareUiService *>(m_ui_service.get())) {
            fence_aware->notifyFrameSubmitted(accepted);
        }
    }

    // ------------------------------------------------------------------
    // lane submission and execution
    // ------------------------------------------------------------------

    Expected<bool> ApplicationEditor::admitPictureFrame(
        const RetroSettings &settings, const int2 target_extent, const std::optional<int2> retained_extent) {
        if (not settings.m_spatial_enabled and not settings.m_temporal_2x_enabled) {
            return false;
        }

        const bool odd_target = target_extent.x > 0 and
                                target_extent.y > 0 and
                                (target_extent.x % 2 != 0 or target_extent.y % 2 != 0);
        if (odd_target and retained_extent) {
            // Validate the real existing image's reconstruction policy, not
            // a fabricated even replacement for the new target. No fresh
            // reconstruction is admitted by this result.
            const int2 raster{retained_extent->x / 2, retained_extent->y / 2};
            const auto error = RetroPass::validateFrameSettings(settings, raster, *retained_extent);
            if (error) {
                PPR_LOG(
                    Editor, error, "Spatial picture admission rejected",
                    {{"origin", "retained"}, {"raster_width", raster.x}, {"raster_height", raster.y},
                        {"output_width", retained_extent->x}, {"output_height", retained_extent->y},
                        {"requested_width", target_extent.x}, {"requested_height", target_extent.y},
                        {"spatial", settings.m_spatial_enabled}, {"legacy", settings.m_enabled},
                        {"temporal", settings.m_temporal_enabled}, {"hdr", settings.m_hdr_enabled},
                        {"exposure_ev", settings.m_exposure_ev}});
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, error);
            return true;
        }

        const int2 raster{target_extent.x / 2, target_extent.y / 2};
        const auto error = RetroPass::validateFrameSettings(settings, raster, target_extent);
        if (error) {
            PPR_LOG(
                Editor, error, "Spatial picture admission rejected",
                {{"origin", "fresh"}, {"raster_width", raster.x}, {"raster_height", raster.y},
                    {"output_width", target_extent.x}, {"output_height", target_extent.y},
                    {"requested_width", target_extent.x}, {"requested_height", target_extent.y},
                    {"spatial", settings.m_spatial_enabled}, {"legacy", settings.m_enabled},
                    {"temporal", settings.m_temporal_enabled}, {"hdr", settings.m_hdr_enabled},
                    {"exposure_ev", settings.m_exposure_ev}});
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, error);
        return false;
    }

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

        const auto picture_admission = admitPictureFrame(
            m_retro_settings, packet.m_framebuffer_size,
            m_grid_stage.m_retain_content and
                    m_grid_stage.m_update_actors_on_retained_terrain and
                    m_diagnostics.m_lighting_camera
                ? std::optional<int2>{m_diagnostics.m_lighting_extent}
                : std::nullopt);
        const std::error_code spatial_policy_error = picture_admission ? std::error_code{} : picture_admission.error();
        if (spatial_policy_error) {
            // This is the CLIENT boundary. Do not query mutable window state
            // from the worker-side admission helper or add it to frame payloads.
            const auto &window = m_main_viewport->getWindow();
            PPR_LOG(
                Editor, error, "Client spatial admission window snapshot",
                {{"frame", packet.m_frame_id}, {"packet_width", packet.m_framebuffer_size.x},
                    {"packet_height", packet.m_framebuffer_size.y},
                    {"window_framebuffer_width", window.m_framebuffer_size.x},
                    {"window_framebuffer_height", window.m_framebuffer_size.y},
                    {"window_client_width", window.m_window_size.x}, {"window_client_height", window.m_window_size.y}});
        }

        if (not hasRenderTarget(packet) or spatial_policy_error) {
            // Durable slot outcome: no render target ⇒ dropped at admission,
            // and the staged payload never applied. The consumption hook
            // still fires so armed flags survive and no driver ack rides
            // a non-applied slot.
            RenderFrameResult transient{};
            transient.m_frame_id = packet.m_frame_id;
            transient.m_dropped = true;
            transient.m_error = spatial_policy_error;
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
            PPR_RETURN_ERROR_ON_FAIL(Editor, spatial_policy_error);
            return {};
        }
        if (renderLaneOwnsRenderer_()) {
            RenderFrameRequest request{};
            request.m_observe_grid = m_observe_grid;
            request.m_retro = m_retro_settings;
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
        inline_request.m_retro = m_retro_settings;
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

                if (command.m_field) {
                    // Field mode owns no receiver windows, matter identity,
                    // bounce or discovery: the begin command only initializes
                    // the dense source and the cascade field, then the ordinary
                    // per-frame dirty stage drives the update.
                    if (not m_field_lighting) {
                        m_field_lighting = std::make_unique<FieldLightingState>();
                    }
                    auto &field = *m_field_lighting;
                    if (not field.m_initialized) {
                        const auto source_error = field.m_source.initialize(
                            *m_lane_rhi_service, *m_lane_shader_service, command.m_field_plan,
                            command.m_field_materials, getContentDir().path());
                        if (source_error) {
                            m_grid_pass->cancelWorldCandidate(command.m_job_id);
                            PPR_RETURN_ERROR_ON_FAIL(Editor, source_error);
                        }

                        const auto field_error = field.m_field.initialize(
                            *m_lane_rhi_service, *m_lane_shader_service, command.m_field_plan, getContentDir().path());
                        if (field_error) {
                            // Partial initialization: release the source that
                            // already initialized so a retry or teardown never
                            // leaks its GPU resources.
                            std::error_code cleanup{};
                            PPR_RETAIN_ERROR_ON_FAIL(Editor, cleanup, field.m_source.shutdown());
                            m_grid_pass->cancelWorldCandidate(command.m_job_id);
                            PPR_RETURN_ERROR_ON_FAIL(Editor, field_error);
                        }

                        field.m_plan = command.m_field_plan;
                        field.m_budget = command.m_field_budget;
                        field.m_accumulation_samples = command.m_field_accumulation_samples;
                        field.m_history_retention = command.m_field_history_retention;
                        field.m_initialized = true;
                    }
                }

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
        out_result.m_temporal_2x_submission.reset();
        out_result.m_particle_census.reset();
        out_result.m_lighting_revision = 0u;
        out_result.m_surface_capture_sequence = 0u;
        m_scene_passes_encoded = false;

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

        PPR_DEFER {
            out_result.m_gpu_timing = renderer.lastGpuFrameTiming();
            if (request.m_observe_grid and m_grid_pass) {
                out_result.m_grid_observations = m_grid_pass->observations();
            }
        };
        const FramePacket &packet = *request.m_packet;
        const DetachedUiDrawData &ui = request.m_ui;
        const GridPass::GridStagedFrame &grid_stage = request.m_grid_stage;
        const TrianglePass::TriangleStagedFrame &triangle_stage = request.m_triangle_stage;
        const bool native_samples = request.m_retro.m_spatial_enabled or request.m_retro.m_temporal_2x_enabled;

        const bool offscreen_requested = request.m_retro.m_enabled or
                                         request.m_retro.m_palette_enabled or
                                         request.m_retro.m_hdr_enabled or
                                         request.m_retro.m_temporal_enabled or
                                         native_samples;
        out_result.m_frame_id = packet.m_frame_id;
        out_result.m_reservation_epoch = request.m_reservation_epoch;
        out_result.m_reservation_frame_id = request.m_reservation_frame_id;
        stampGridEcho(grid_stage, out_result);

        // Retro is a lane-owned pass: a policy-enabled request without a live
        // pass (initialization failed or teardown raced) fails closed instead
        // of silently falling back to the native path.
        if (offscreen_requested and not m_retro_pass) [[unlikely]] {
            PPR_RETURN_ERROR_ON_FAIL(Editor, make_error_code(std::errc::not_connected));
        }

        // Cell uploads write a mapped atlas shared with earlier GPU frames.
        // Fence and publish pending visible cells before opening the
        // surface render pass. On the worker this whole function runs on
        // the fixed lane (atlas preparation is render-owned); inline it
        // runs on main. Either way no live Window/camera/input/service or
        // live ImGui state is touched — only the owned packet, the
        // detached UI data, and lane-confined GPU state.
        // The lane-confined rhi service captured at startup is the only
        // legal source on this (possibly worker) thread: never read the
        // service store here — that hidden-singleton lookup would race the
        // main thread's teardown erase. Fail closed when it is invalid.
        const safe_ptr<IRhiService> rhi_service = m_lane_rhi_service;
        if (not rhi_service.isValid()) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        if (native_samples) {
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, rhi_service->getDevice().hasFeature("standard-sample-locations-4") and
                                rhi_service->getDevice().hasFeature("sample-rate-shading")
                            ? std::error_code{}
                            : std::make_error_code(std::errc::not_supported));
        }

        // Non-rendering outcome for zero-size/minimized surfaces:
        // skip uploads and present, report success. This is also the
        // empty-packet fail-closed: a packet with no render target performs
        // zero backend work. No stage application (no render happened), so
        // no consumption ack: re-staged next buildable frame.
        if (not hasRenderTarget(packet)) {
            return default_value_v;
        }

        // Generational rejection before ANY backend work (uploads fence the
        // queue, so they must not run for a stale packet): a packet built
        // for an older surface shape retires here as an explicit
        // non-rendering outcome. Presentation-only state is dropped; the
        // world edits inside the staged payload stay in the driver's
        // dirty set (no consumption ack rides a dropped frame), so the
        // next buildable frame re-stages them.
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

        // A numerical buffer can become the solver's next candidate after
        // publication; stale draws are rejected below.
        const bool particles_current = m_grid_pass->gpuParticleFrameCurrent(grid_stage.m_gpu_particles);

        const int2 retro_extent = native_samples
                                      ? int2{packet.m_framebuffer_size.x / 2, packet.m_framebuffer_size.y / 2}
                                  : request.m_retro.m_enabled
                                      ? RetroPass::sceneExtent(packet.m_framebuffer_size, request.m_retro.m_pixel_scale)
                                      : packet.m_framebuffer_size;
        CameraSnapshot raster_camera = packet.m_camera;
        CameraSnapshot scene_camera = packet.m_camera;
        RetroLightingPublication frame_lighting_publication{};

        if (not particles_current) {
            out_result.m_dropped = true;
            return {};
        }

        // P2 pass-affinity: ALL pass CPU-state mutations run here, on the
        // pass's owner lane (worker or main-inline), applied from the
        // owned payload captured in update(). The camera snapshot +
        // delta time come from the packet (immutable), the tile list and
        // upload cells ride the per-frame staged payload built by the
        // client — nothing live is read by either.
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
            // content. An empty replacement must supersede an older nonempty
            // view without requiring an intervening frame.
            if (grid_stage.m_gpu_particles) {
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->applyGpuParticleView(*grid_stage.m_gpu_particles));
            }

            {
                const hal::ProfileScope scope_uploads{"App.Editor.PrepareUploads"};
                const CpuObservationScope cpu_uploads{clock, cpu.m_uploads};
                const auto published = m_grid_pass->tryPrepareCellUploads(
                    rhi_service->getDevice(), renderer.completedFenceValue());
                PPR_RETURN_ERROR_ON_FAIL(Editor, published);
                out_result.m_grid_stage_applied = out_result.m_grid_staging_applied and *published;

                // Resident particles and their clipping terrain form one cohort.
                // A partial terrain upload cannot draw the new numerical view.
                if (grid_stage.m_gpu_particles and not *published) {
                    return {};
                }
            }

            if (m_retro_pass) {
                if (native_samples) {
                    PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->validateSpatialContent());
                }

                const auto identity = m_grid_pass->activeWorldIdentity();
                const auto raster = m_retro_pass->beginFrame(
                    request.m_retro, scene_camera, retro_extent,
                    native_samples ? packet.m_framebuffer_size : retro_extent, identity.m_world_generation,
                    identity.m_job_id, identity.m_content_fingerprint, TimeSpan{}, packet.m_delta_time, 0u,
                    frame_lighting_publication);
                PPR_RETURN_ERROR_ON_FAIL(Editor, raster);

                raster_camera = *raster;
            }

            PPR_RETURN_ERROR_ON_FAIL(Editor, m_triangle_pass->update(packet.m_delta_time, raster_camera));
            PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->update(packet.m_delta_time, raster_camera, native_samples));
        }
        const bool offscreen = request.m_retro.m_enabled or
                               request.m_retro.m_palette_enabled or
                               request.m_retro.m_hdr_enabled or
                               native_samples or
                               (m_retro_pass and m_retro_pass->temporalActive());
        out_result.m_grid_staged_tiles = m_grid_pass->stagedTileCount();
        out_result.m_grid_cached_tiles = m_grid_pass->cachedTileCount();

        // Detached UI submission, bound synchronously on this lane: the
        // callback captures the detached data by view for the duration of
        // the encode below (never stored, never transported further).
        const string_literal ui_description{std::in_place, "ui.detached"};
        // Named local closure: DrawSubmission stores a function_ref to it, so
        // a temporary closure would dangle the moment this full-expression
        // ends (ASan stack-use-after-scope at encode time).
        const auto ui_encode = [&](const DrawContext &draw_context) -> std::error_code {
            const CpuObservationScope cpu_ui{clock, cpu.m_ui_draw};
            PPR_ASSERT(m_ui_service != nullptr);
            return m_ui_service->renderDetachedUiFrame(ui, draw_context);
        };
        const DrawSubmission ui_draws{
            ui_description,
            ui_encode,
        };

        const auto grid_viewport = rhi::Viewport::fromSize(
            static_cast<float>(packet.m_framebuffer_size.x), static_cast<float>(packet.m_framebuffer_size.y));
        const auto grid_scissor = rhi::ScissorRect::fromSize(
            safe_narrowing(packet.m_framebuffer_size.x), safe_narrowing(packet.m_framebuffer_size.y));
        const auto retro_viewport = rhi::Viewport::fromSize(
            static_cast<float>(retro_extent.x), static_cast<float>(retro_extent.y));
        const auto retro_scissor = rhi::ScissorRect::fromSize(
            safe_narrowing(retro_extent.x), safe_narrowing(retro_extent.y));

        // Scene contents shared by both presentation policies: the retro
        // scene draw forwards here through RetroPass::prepareScene; the
        // native policy encodes it directly. The per-part CPU observation
        // scopes stay attached to the work they time in either mode.
        const auto draw_scene_contents = [&](const DrawContext &context) -> std::error_code {
            // The draw context supplies the actual native or offscreen scene
            // signature. Prepare admitted variants before acknowledging fresh
            // scene work, even when this frame submits no triangle instances.
            {
                const CpuObservationScope cpu_prepare{clock, cpu.m_triangle_draw};
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, m_triangle_pass->prepare(context.m_device, context.m_render_pipeline_key));
            }

            m_scene_passes_encoded = true;
            {
                const CpuObservationScope cpu_draw{clock, cpu.m_grid_draw};
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderBackground(context));
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderGpuIndirect(context));
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderParticles(context));
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderTranslucent(context));
            }

            const CpuObservationScope cpu_draw{clock, cpu.m_triangle_draw};
            return m_triangle_pass->render(context);
        };

        // CPU facts for capture and temporal receipts follow the actual scene draw.
        // A policy request or prepared target is not proof of its encoding.
        std::optional<RenderPipelineSignature> captured_scene_signature{};
        int2 captured_scene_extent{};

        const auto retro_scene_draw = [&](const DrawContext &context) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(Editor, draw_scene_contents(context));

            if (request.m_retro.m_temporal_2x_enabled) {
                captured_scene_signature = static_cast<const RenderPipelineSignature &>(context.m_render_pipeline_key);
                captured_scene_extent = context.m_target_extent;
            }

            return {};
        };

        const auto scene_draw = [&](const DrawContext &context) -> std::error_code {
            if (offscreen) {
                return m_retro_pass->render(context);
            }
            return draw_scene_contents(context);
        };

        const bool capture = request.m_capture_final_frame and
                             request.m_surface_capture_sequence > m_frame_capture_attempted_sequence;

        const auto prepare_background = [&](const PreRenderContext &context, const CameraSnapshot &camera,
                                            const rhi::Viewport viewport,
                                            const rhi::ScissorRect scissor) -> std::error_code {
            return m_grid_pass->prepareBackground(context, camera, viewport, scissor);
        };

        const auto scene_prepare = [&](const PreRenderContext &context) -> std::error_code {
            // Field mode: apply the committed dirty stage, encode the dense
            // source upload and the cascade field update, then rebind the fresh
            // (ping-ponged) field view before the scene samples it. No receiver
            // windows, matter identity, bounce, discovery or deadline planning.
            if (m_field_lighting and m_field_lighting->m_initialized) {
                auto &field = *m_field_lighting;
                const i32 sky_hour = grid_stage.m_temporal_sky_hour.value_or(-1);

                // The source revision is authoritative: it adopts the candidate
                // only when the update actually changes something, so a clean
                // frame (or an empty particle batch) leaves it unchanged.
                auto storage = lighting::buildWorldLightSourceUpdate(
                    grid_stage, field.m_plan.m_width, field.m_source.revision() + 1u, sky_hour);
                PPR_RETURN_ERROR_ON_FAIL(Editor, field.m_source.applyUpdate(storage.m_update));
                PPR_RETURN_ERROR_ON_FAIL(Editor, field.m_source.encode(context.m_encoder));
                // The injected frame delta drives the retention EMA; use the real
                // packet delta (falling back to the 60 Hz nominal on a zero delta).
                const float frame_delta = std::chrono::duration<float>(packet.m_delta_time).count();
                const float field_dt = frame_delta > 0.0f ? frame_delta : 1.0f / 60.0f;
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, field.m_field.update(
                                context.m_encoder, field.m_source, field_dt,
                                field.m_budget != 0u ? field.m_budget
                                                     : static_cast<u32>(field.m_plan.m_levels[0].m_sample_count),
                                field.m_accumulation_samples, field.m_history_retention));
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->setWorldLightField(field.m_field.view()));

                out_result.m_lighting_source_revision = field.m_source.revision();
                out_result.m_lighting_field_cycle = field.m_field.cycle();

                // Once per run, on the first frame where a source revision
                // actually landed (revision > 0): prove a dirty-chunk update was
                // applied and encoded, not just an empty first field frame. The
                // bounded run test greps for this. Not per-frame.
                if (not field.m_frame_logged and out_result.m_lighting_source_revision > 0u) {
                    PPR_LOG(
                        Editor, info, "world light field frame",
                        {{"revision", out_result.m_lighting_source_revision},
                            {"cycle", out_result.m_lighting_field_cycle.value_or(0u)}, {"frame", packet.m_frame_id}});
                    field.m_frame_logged = true;
                }
            }

            if (offscreen) {
                {
                    const CpuObservationScope cpu_prepare{clock, cpu.m_grid_prepare};
                    PPR_RETURN_ERROR_ON_FAIL(
                        Editor, prepare_background(
                                    context, request.m_retro.m_temporal_2x_enabled ? raster_camera : scene_camera,
                                    retro_viewport, retro_scissor));
                    PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->prepareGpuVisibility(context));

                    PPR_RETURN_ERROR_ON_FAIL(
                        Editor,
                        m_grid_pass->prepareGpuParticles(
                            context,
                            capture and m_measure_world_radiance ? grid_stage.m_capture_particle_census_edge : 0u));
                }

                return m_retro_pass->prepareScene(
                    context,
                    m_retro_pass->temporal2xActive()
                        ? RetroPass::temporal2xScenePipelineSignature()
                        : RetroPass::scenePipelineSignature(
                              m_retro_pass->temporalActive() or m_retro_pass->spatialActive()),
                    retro_extent, retro_scene_draw);
            }
            const CpuObservationScope cpu_prepare{clock, cpu.m_grid_prepare};
            PPR_RETURN_ERROR_ON_FAIL(Editor, prepare_background(context, scene_camera, grid_viewport, grid_scissor));
            PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->prepareGpuVisibility(context));

            return m_grid_pass->prepareGpuParticles(
                context, capture and m_measure_world_radiance ? grid_stage.m_capture_particle_census_edge : 0u);
        };

        DrawSubmission scene_submission{"scene", scene_draw};

        const std::initializer_list<SurfaceRenderPass> surface_passes{
            {
                .m_additional_colors = {},
                // Retro scene work renders into the pass's scene target and
                // composites as a fullscreen blit, so the surface pass owns
                // no depth attachment in that mode.
                .m_depth_policy = offscreen ? ESurfaceDepthPolicy::none : ESurfaceDepthPolicy::renderer_owned,
                .m_draws = {scene_submission},
                .m_prepare = PreRenderCallback{scene_prepare},
            },
            {
                .m_surface_color =
                    ColorAttachmentOps{
                        .m_load_op = rhi::LoadOp::Load,
                    },
                .m_additional_colors = {},
                // Detached UI draws, constructed synchronously on this lane
                // (inline or worker): the encode reads ONLY the detached
                // data, never the live ImGui frontend.
                .m_draws = {ui_draws},
            },
        };

        // Submission acceptance is independent of present: capture the
        // fence frontier first so every participant is stamped off an
        // accepted SUBMIT even when the present below it fails (G1-5).
        // An obsolete packet (resized since capture without a same-frame
        // reconfigure) or a capacity-busy ledger retires here as an
        // explicit non-rendering outcome — dropped, never fatal: the next
        // frame recaptures a fresh packet against the current surface.
        // Quarantine/terminal failures keep their distinct codes and
        // propagate to teardown/recovery — never suppressed as dropped
        // frames.
        //
        // The window travels as a snapshot (owned copies only): the lane
        // never touches the live Window. The title borrows packet storage,
        // which outlives the call through the lane request.
        const SurfaceWindowSnapshot window_snapshot{
            .m_handle = packet.m_surface_identity,
            .m_native = packet.m_surface_native,
            .m_display = packet.m_surface_display,
            .m_framebuffer_size = packet.m_framebuffer_size,
            .m_title = packet.m_window_title,
        };
        const FenceCompletionValue previously_submitted = renderer.lastSubmittedFenceValue();
        std::error_code frame_err{};
        // Exception-safe submission-fate boundary (allocation-free, noexcept):
        // an accepted SUBMIT followed by a throwing present diagnostic must
        // still stamp participants + classify the fate exactly once. The
        // guard captures this frame's before value and finalizes on every
        // exit (normal + unwind) against the after value. The normal path
        // invokes this same body before capture proof, never a second stamp.
        struct SubmitFateFinalizer {
            ApplicationEditor *m_self;
            Renderer *m_renderer;
            RenderFrameResult *m_out;
            FenceCompletionValue m_before;
            const std::error_code *m_frame_err;
            const std::optional<RenderPipelineSignature> *m_scene_signature;
            const int2 *m_scene_extent;
            const int2 *m_output_extent;
            const RetroLightingPublication *m_lighting_publication;
            bool m_temporal_2x;
            bool m_done{};

            void finalize() noexcept {
                if (m_done) {
                    return;
                }

                m_done = true;
                m_self->stampSubmittedPasses_(m_before);
                // Off-thread (affinity) rejection is invocation-local:
                // the Renderer leaves owner-confined settlement untouched
                // on that path, so classify from the returned error and
                // never consult the member (which still holds the
                // previous owner-thread fate). Every other path classifies
                // THIS invocation only (F1 stale fate): an accepted SUBMIT
                // must advance the fence strictly past this frame's before
                // value. A rejection reports definitely-unsubmitted with
                // this frame's identity — never the previous frame's value
                // — and only a submit failure quarantines (possibly
                // executed, no value).
                if (m_frame_err != nullptr and
                    *m_frame_err == std::make_error_code(std::errc::operation_not_permitted)) {
                    m_out->m_submit_value = kFenceValueNone;
                    m_out->m_submit_possibly_executed = false;
                    return;
                }
                const FenceCompletionValue after = m_renderer->lastSubmittedFenceValue();
                const FrameSettlement fate = m_renderer->lastSubmitSettlement();
                if (fate == FrameSettlement::kSubmitted and after != kFenceValueNone and after > m_before) {
                    m_out->m_submit_value = after;
                    m_out->m_submit_possibly_executed = false;

                    // Stamping above latches Retro's actual accepted encoding.
                    // Historical receipts from rejected draws cannot match this
                    // fresh successful scene and submission fence.
                    if (m_temporal_2x and
                        m_self->m_scene_passes_encoded and
                        m_out->m_presented and
                        m_frame_err != nullptr and
                        not *m_frame_err and
                        *m_scene_signature and
                        **m_scene_signature == RetroPass::temporal2xScenePipelineSignature()) {
                        const auto observed = m_self->m_retro_pass->lastTemporal2xSubmission();
                        if (observed and
                            observed->m_submission == after and
                            observed->m_raster_extent.x == m_scene_extent->x and
                            observed->m_raster_extent.y == m_scene_extent->y and
                            observed->m_output_extent.x == m_output_extent->x and
                            observed->m_output_extent.y == m_output_extent->y and
                            observed->m_lighting_publication == *m_lighting_publication) {
                            m_out->m_temporal_2x_submission = *observed;
                        }
                    }
                } else if (fate == FrameSettlement::kSubmitFailed) {
                    m_out->m_submit_value = kFenceValueNone;
                    m_out->m_submit_possibly_executed = true;
                } else {
                    m_out->m_submit_value = kFenceValueNone;
                    m_out->m_submit_possibly_executed = false;
                }
            }

            ~SubmitFateFinalizer() noexcept {
                if (not m_done) {
                    finalize();
                }
            }
        };
        SubmitFateFinalizer submit_fate_guard{
            this, &renderer, &out_result, previously_submitted, &frame_err, &captured_scene_signature,
            &captured_scene_extent, &packet.m_framebuffer_size, &frame_lighting_publication,
            request.m_retro.m_temporal_2x_enabled};

        bool capture_image_acquired{};
        const u64 previous_capture_sequence = m_frame_capture_attempted_sequence;
        if (capture) {
            m_frame_capture_attempted_sequence = request.m_surface_capture_sequence;
            out_result.m_surface_capture_attempted = true;
            out_result.m_surface_capture_sequence = request.m_surface_capture_sequence;
        }

        {
            const hal::ProfileScope scope_frame_execute{"PPR.Frame.Execute"};
            const CpuObservationScope cpu_surface{clock, cpu.m_surface};
            frame_err = renderer.renderAndPresent(
                window_snapshot, {surface_passes.begin(), surface_passes.size()}, packet_surface,
                &out_result.m_presented, capture ? &out_result.m_surface_pixels : nullptr,
                capture ? &capture_image_acquired : nullptr);
        }

        // The same exactly-once fate body now stamps the accepted slot before
        // its capture-only readback is mapped. Early exits still use the guard.
        submit_fate_guard.finalize();

        if (capture) {
            const bool pre_acquire_transient = not capture_image_acquired and
                                               not out_result.m_presented and
                                               not out_result.m_surface_pixels and
                                               renderer.lastSubmitSettlement() == FrameSettlement::kRejected and
                                               renderer.lastSubmittedFenceValue() == previously_submitted and
                                               (not frame_err or
                                                   frame_err == std::errc::device_or_resource_busy or
                                                   frame_err == std::errc::resource_unavailable_try_again);
            if (pre_acquire_transient) {
                // The Renderer explicitly proves no acquisition/copy and no
                // accepted/unknown submit. Keep the same one-shot request armed.
                m_frame_capture_attempted_sequence = previous_capture_sequence;
                out_result.m_surface_capture_attempted = false;
                out_result.m_surface_capture_sequence = 0u;
            } else {
                out_result.m_surface_capture_error = frame_err;
                if (not frame_err and (not out_result.m_presented or not out_result.m_surface_pixels)) {
                    out_result.m_surface_capture_error = std::make_error_code(std::errc::io_error);
                }
            }
        }

        // Surface lane-observable outcome: the accepted SUBMIT value
        // independent of present, and the possibly-executed discriminator
        // for ambiguous throws (kSubmitFailed) were finalized by the guard
        // above on all paths (normal + unwind). The value on the result is
        // the same value the retirement frontier will later cover — the
        // main-side settlement pairs them by equality, never by presence.
        // Report lane-observable state back to main (surface generation
        // mirror + font ack); the completion pump refreshes them.
        out_result.m_surface_handle = packet.m_surface_identity;
        out_result.m_surface_generation = renderer.surfaceGenerationFor(packet.m_surface_identity);
        if (m_ui_service) {
            out_result.m_ui_font_version_applied = m_ui_service->installedFontVersion();
        }
        if (frame_err == std::make_error_code(std::errc::resource_unavailable_try_again) or
            frame_err == std::make_error_code(std::errc::device_or_resource_busy)) {
            PPR_LOG(
                Editor, info, "dropping non-rendering frame",
                {
                    {"frame", packet.m_frame_id},
                    {"error", frame_err.message()},
                });
            return default_value_v;
        }
        PPR_RETURN_ERROR_ON_FAIL(Editor, frame_err);

        if (out_result.m_presented) {
            out_result.m_presented_grid = m_grid_pass->activeWorldIdentity();
            out_result.m_presented_particles = m_grid_pass->activeParticlePresentation();
        }

        return default_value_v;
    }
}
