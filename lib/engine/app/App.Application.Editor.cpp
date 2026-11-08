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
import :renderer.lighting_pass;
import :renderer.world_radiance;
import :renderer.optical_geometry;
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
        constexpr u32 kScreenTimingSlots = 4u;
        constexpr u32 kScreenTimingStride = 13u;
        constexpr u32 kNoMaterialBand = GridPass::kMaterialTimingBands;

        // Sixteen stripes cover the current raster. Two observations of a
        // stripe are needed before its lower measured cost contributes credit.
        struct TerrainMaterialTiming {
            CameraModel m_camera{};
            lighting::TerrainReceiverCohort m_cohort{};
            u64 m_epoch{}, m_last_frame{};
            int2 m_extent{zero_v};
            std::array<float, GridPass::kMaterialTimingBands> m_recent{}, m_cost{};
            std::array<float, 8u> m_overhead{};
            u32 m_samples{}, m_next_band{}, m_overhead_next{};
            bool m_enabled{};

            void updateView(
                const lighting::TerrainReceiverCohort cohort, const CameraSnapshot &camera, const int2 extent,
                const bool temporal_2x) noexcept {
                const bool enabled = temporal_2x and
                                     extent.x > 0 and
                                     extent.y > 0 and
                                     GridPass::supportsMaterialTiming(
                                         static_cast<u32>(extent.x / 2), static_cast<u32>(extent.y / 2));
                const float motion_limit = camera.m_ortho_scale * float(std::min(extent.x, extent.y)) / 32.0f;
                if (cohort == m_cohort and
                    extent.x == m_extent.x and
                    extent.y == m_extent.y and
                    camera.m_camera_mode == m_camera.m_camera_mode and
                    camera.m_ortho_scale == m_camera.m_ortho_scale and
                    std::memcmp(&camera.m_basis, &m_camera.m_basis, sizeof(camera.m_basis)) == 0 and
                    std::abs(camera.m_origin.x - m_camera.m_origin.x) <= motion_limit and
                    std::abs(camera.m_origin.y - m_camera.m_origin.y) <= motion_limit and
                    std::abs(camera.m_origin.z - m_camera.m_origin.z) <= motion_limit and
                    enabled == m_enabled)
                    return;

                ++m_epoch;
                m_enabled = enabled;
                m_cohort = cohort;
                m_camera = camera;
                m_extent = extent;
                m_cost = {};
                m_recent = {};
                m_samples = m_next_band = 0u;
            }

            [[nodiscard]] bool due(const u64 frame) const noexcept {
                return m_enabled and (m_samples < 2u * kNoMaterialBand or frame - m_last_frame >= 8u);
            }

            void accept(const u32 band, const u64 frame) noexcept {
                // Each band earns credit only after two observations. Pair the
                // initial draws so measured bands can fund pending edit work.
                const bool first_of_pair = m_samples < 2u * kNoMaterialBand and m_samples % 2u == 0u;
                m_next_band = first_of_pair ? band : (band + 7u) % kNoMaterialBand;
                m_last_frame = frame;
                m_samples = std::min(m_samples + 1u, 2u * kNoMaterialBand);
            }

            void observe(const u32 band, const float sample) noexcept {
                m_cost[band] = std::min(m_recent[band], sample);
                m_recent[band] = sample;
            }

            [[nodiscard]] float credit(const float terrain_floor) const noexcept {
                float sum{};
                for (const float cost: m_cost)
                    sum += cost;

                return 0.8f * std::min(sum, terrain_floor);
            }

            void observeOverhead(const float milliseconds) noexcept {
                m_overhead[m_overhead_next] = milliseconds;
                m_overhead_next = (m_overhead_next + 1u) % m_overhead.size();
            }
        };

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

        constexpr u32 kTemporalRefreshEdge = 32u;

        // Tile selection bounds refresh work; the optical source still spans the world.
        lighting::WorldRadianceReceiverWindow temporalRefreshWindow_(
            const lighting::WorldRadianceSource &source, const u32 cursor) noexcept {
            constexpr u32 edge = kTemporalRefreshEdge;
            const u32 columns = (source.width() + edge - 1u) / edge;
            const u32 rows = (source.height() + edge - 1u) / edge;
            const u32 tile = cursor % (columns * rows);
            const u32 x = (tile % columns) * edge;
            const u32 y = (tile / columns) * edge;
            return {
                {static_cast<i32>(x), static_cast<i32>(y)},
                {std::min(edge, source.width() - x), std::min(edge, source.height() - y)}};
        }

        bool retainableTerrainViewError_(const std::error_code error) noexcept {
            // Only the lighting contract's own domain/admission failures may
            // retain a picture. RHI, UI, submission and terminal errors do not.
            return error == std::errc::argument_out_of_domain or
                   error == std::errc::result_out_of_range or
                   error == std::errc::no_buffer_space or
                   error == std::errc::value_too_large or
                   error == std::errc::not_supported or
                   error == std::errc::function_not_supported or
                   error == std::errc::invalid_argument or
                   error == std::errc::device_or_resource_busy or
                   error == std::errc::timed_out;
        }


    } // namespace

    // ------------------------------------------------------------------
    // editor-owned small-scene world reference field
    // ------------------------------------------------------------------

    struct ApplicationEditor::TerrainLightingState {
        CameraSnapshot m_view_camera{}, m_candidate_camera{}, m_discovery_camera{};
        lighting::WorldRadiancePass m_pass{};
        rhi::ComPtr<rhi::ITexture> m_submission_target{};
        rhi::ComPtr<rhi::IQueryPool> m_screen_queries{};
        std::shared_ptr<const lighting::WorldRadianceGeneration> m_source{}, m_candidate_source{}, m_combined_source{};
        // Stamp only the normally acquired pass donor. Clear after accepted
        // prepare; this weak identity never keeps canceled GPU storage alive.
        std::weak_ptr<const lighting::WorldRadianceGeneration> m_discovery_donor{};
        std::shared_ptr<const lighting::WorldRadianceFaceDiscovery> m_discovery_result{};

        // Accepted optical epoch and the oldest subsequently observed request.
        // These small values do not retain a newer mutable solver publication.
        std::shared_ptr<const lighting::WorldRadianceSource> m_terrain_source{}, m_candidate_terrain_source{},
            m_bound_terrain_source{}, m_projected_matter_source{};
        std::optional<lighting::WorldRadianceMatterIdentity> m_matter_latest{}, m_matter_frozen{};
        std::optional<lighting::WorldRadianceGameplayDemand> m_gameplay_demand{};
        u64 m_delivered_query{};
        std::optional<WorldMatterLightingReceipt> m_matter_receipt{};
        std::optional<TimeSpan> m_matter_pending_elapsed{}, m_matter_after_frozen_elapsed{};
        TimeSpan m_matter_frozen_elapsed{}, m_matter_projected_elapsed{};

        lighting::TerrainReceiverCohort m_candidate_cohort{}, m_rejected_cohort{};
        int2 m_view_extent{zero_v}, m_candidate_extent{zero_v};
        TimeSpan m_view_request_elapsed{}, m_candidate_request_elapsed{};
        std::optional<TimeSpan> m_window_pending_elapsed{};

        // Source publication is independent of the per-camera view request.
        TimeSpan m_source_request_elapsed{}, m_publication_elapsed{}, m_last_age_report_elapsed{};
        TimeSpan m_temporal_refresh_started{}, m_temporal_refresh_reported{}, m_temporal_refresh_finished{};
        TimeSpan m_matter_admitted_elapsed{};
        std::array<TimeSpan, 8u> m_matter_service_times{}, m_matter_refresh_times{};
        TimeSpan m_discovery_request_elapsed{}, m_discovery_prepare_cpu{};

        std::optional<u64> m_last_field_frame{};
        FenceCompletionValue m_submission{kFenceValueNone}, m_bounce_submission{kFenceValueNone};
        FenceCompletionValue m_discovery_submission{kFenceValueNone}, m_matter_submission{kFenceValueNone};
        FenceCompletionValue m_discovery_first_submission{kFenceValueNone};
        FenceCompletionValue m_surface_cost_submission{kFenceValueNone};
        std::array<FenceCompletionValue, kScreenTimingSlots> m_screen_query_fences{};
        std::array<u64, kScreenTimingSlots> m_screen_query_frames{}, m_material_query_epochs{};
        std::array<u32, kScreenTimingSlots> m_material_query_bands{};
        std::array<float, kScreenTimingSlots> m_screen_query_material_credit{};
        TerrainMaterialTiming m_material_timing{};
        std::array<float, 8u> m_screen_terrain_ms{};

        u64 m_discovery_epoch{}, m_matter_epoch{}, m_bound_matter_terrain_revision{};
        std::error_code m_error{}, m_discovery_error{};
        std::optional<lighting::WorldRadianceReceiverWindow> m_attempted_window{}, m_rejected_window{};
        lighting::WorldRadianceReceiverWindow m_discovery_window{};
        enum class Stage : u8 {
            idle,
            field,
            matter,
            submission_unknown
        };
        lighting::WorldRadianceTemporalSettings m_temporal_settings{};
        std::array<float, 8u> m_surface_gpu_milliseconds{}, m_screen_gpu_milliseconds{};
        u32 m_surface_cost_next{}, m_temporal_refresh_cursor{}, m_screen_cost_next{}, m_screen_cost_samples{};
        u32 m_matter_service_next{};
        float m_temporal_budget_milliseconds{};
        Stage m_stage{Stage::idle};
        bool m_initialized{}, m_loading{}, m_submission_pending{}, m_retainable_failure{};
        bool m_matter_prepared{}, m_matter_presented_once{}, m_matter_includes_refresh{};
        bool m_source_first_presented{}, m_temporal_budget_exhausted{};
        std::array<bool, kScreenTimingSlots> m_screen_query_complete{};
        bool m_temporal_refresh_next{true}, m_temporal_refresh_pending{}, m_screen_timing_unknown{};
        bool m_direct_window{}, m_window_update{}, m_temporal_cascades{}, m_temporal_skip_table{};
        bool m_discovery_enabled{}, m_discovery_prepared{}, m_bounce_enabled{}, m_bounce_prepared{};
        bool m_grid_upload_begin_observed{}, m_grid_upload_end_observed{};
        std::optional<lighting::WorldRadianceTimingStatus> m_reported_timing_status{};
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

    namespace {
        void observeTerrainPublicationPhase_(
            const bool enabled, const char *const phase, const lighting::TerrainReceiverCohort cohort,
            const u64 sequence = 0u, const FenceCompletionValue submission = kFenceValueNone) {
            if (not enabled) {
                return;
            }

            PPR_LOG(
                Editor, info, "World radiance publication phase",
                {{"phase", phase}, {"job", cohort.m_job_id}, {"world", cohort.m_world_generation},
                    {"content", cohort.m_content_fingerprint}, {"revision", cohort.m_geometry_revision},
                    {"sequence", sequence}, {"submission", submission}});
        }
    } // namespace

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

    void ApplicationEditor::setWorldRadianceReceiverDemands(
        const std::span<const lighting::WorldRadianceReceiverWindow> demands) {
        m_lighting_receiver_demands.assign(demands.begin(), demands.end());
    }

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
                        PPR_RETAIN_ERROR_ON_FAIL(
                            Editor, lane_err,
                            releaseTerrainLighting_(
                                device_loss_confirmed ? GpuQuiescence::kRemovalConfirmed : GpuQuiescence::kIdleProven));
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
            PPR_RETAIN_ERROR_ON_FAIL(
                Editor, first_err,
                releaseTerrainLighting_(
                    device_loss_confirmed ? GpuQuiescence::kRemovalConfirmed : GpuQuiescence::kIdleProven));
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

        if (not lane_releases_done) {
            PPR_RETAIN_ERROR_ON_FAIL(
                Editor, first_err,
                releaseTerrainLighting_(
                    device_loss_confirmed ? GpuQuiescence::kRemovalConfirmed : GpuQuiescence::kIdleProven));
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
            request.m_expected_lighting_revision = m_expected_lighting_revision;
            request.m_lighting_receiver_demands = m_lighting_receiver_demands;
            request.m_gameplay_query = m_gameplay_query and m_gameplay_query->m_field_world
                                           ? m_gameplay_query
                                           : std::exchange(m_gameplay_query, std::nullopt);
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
        inline_request.m_expected_lighting_revision = m_expected_lighting_revision;
        inline_request.m_lighting_receiver_demands = m_lighting_receiver_demands;
        inline_request.m_gameplay_query = m_gameplay_query and m_gameplay_query->m_field_world
                                              ? m_gameplay_query
                                              : std::exchange(m_gameplay_query, std::nullopt);
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
        result.m_lighting_revision = command->m_lighting_revision;
        result.m_error = executeGridWorld_(*command, result);
        onGridWorldCompleted_(result);
        return {};
    }

    std::error_code ApplicationEditor::submitTerrainLighting_(
        const bool discovery, const u64 actual_frame, const bool bounce, const bool matter,
        const std::optional<TimeSpan> elapsed) {
        auto &state = *m_terrain_lighting;
        auto &renderer = getRenderer();
        const auto before = renderer.lastSubmittedFenceValue();
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, state.m_stage != TerrainLightingState::Stage::submission_unknown
                        ? std::error_code{}
                        : std::make_error_code(std::errc::state_not_recoverable));

        PPR_RETURN_ERROR_ON_FAIL(
            Editor, u32(discovery) + u32(bounce) + u32(matter) <= 1u
                        ? std::error_code{}
                        : std::make_error_code(std::errc::invalid_argument));
        float matter_allowance_ms{};
        if (state.m_temporal_cascades and not discovery and not bounce) {
            const auto surface = renderer.lastSurfaceGpuFrameTiming();
            if (surface and
                surface->m_submission > state.m_surface_cost_submission and
                std::isfinite(surface->m_milliseconds) and
                surface->m_milliseconds >= 0.0f) {
                state.m_surface_cost_submission = surface->m_submission;
                state.m_surface_gpu_milliseconds[state.m_surface_cost_next] = surface->m_milliseconds;
                state.m_surface_cost_next = (state.m_surface_cost_next + 1u) % state.m_surface_gpu_milliseconds.size();
            }

            PPR_RETURN_ERROR_ON_FAIL(Editor, observeScreenLighting_());
            const float surface_milliseconds = *std::ranges::max_element(state.m_surface_gpu_milliseconds);
            const float terrain_floor = *std::ranges::min_element(state.m_screen_terrain_ms);
            const float material_credit = state.m_material_timing.credit(terrain_floor);
            const float calibration = state.m_material_timing.due(actual_frame)
                                          ? *std::ranges::max_element(state.m_material_timing.m_overhead)
                                          : 0.0f;
            // During the first Matter projection GPU clocks can change within a few
            // presents. Keep the worst of a full query rotation for that transition.
            // Material credit and the whole-surface frame guard keep their full histories.
            const auto &screen_costs = state.m_screen_gpu_milliseconds;
            const auto count = state.m_matter_frozen and not state.m_matter_presented_once ? kScreenTimingSlots
                                                                                           : screen_costs.size();
            float screen_peak{};
            for (std::size_t age = 0; age < count; ++age) {
                const auto index = (state.m_screen_cost_next + screen_costs.size() - 1u - age) % screen_costs.size();
                screen_peak = std::max(screen_peak, screen_costs[index]);
            }

            const float reserve = std::max(0.0f, screen_peak - material_credit) + calibration;
            // The accepted temporal path targets60Hz. Reserve whole-surface cost
            // against that frame interval, and screen lighting against its own6/10ms budget.
            // Matter and field still share the actual-frame work token.
            constexpr float frame_milliseconds = 1000.0f / 60.0f;
            const float available = std::max(
                0.0f,
                std::min(state.m_temporal_budget_milliseconds - reserve, frame_milliseconds - surface_milliseconds));
            matter_allowance_ms = available;
            bool allow_refresh = true;
            if (elapsed and state.m_temporal_refresh_pending and state.m_matter_frozen) {
                const auto service = *std::ranges::max_element(state.m_matter_service_times);
                const auto successor = state.m_matter_after_frozen_elapsed.value_or(state.m_matter_admitted_elapsed);
                // Leave time to certify, present and observe the last submitted
                // page. A coalesced successor also needs its full measured service.
                constexpr auto cutoff = std::chrono::milliseconds{900};
                allow_refresh = *elapsed - state.m_matter_frozen_elapsed < cutoff and
                                *elapsed - successor + service < cutoff;
            }
            const auto budgeted = matter ? Expected<bool>{available > 0.0f}
                                         : state.m_pass.budgetTemporalFrame(available, allow_refresh);
            PPR_RETURN_ERROR_ON_FAIL(Editor, budgeted);
            if (m_measure_world_radiance) {
                PPR_LOG(
                    Editor, info, "WORLD_TEMPORAL_FRAME_ALLOWANCE",
                    {{"frame", actual_frame}, {"total_ms", state.m_temporal_budget_milliseconds},
                        {"surface_ms", surface_milliseconds}, {"screen_lighting_estimate_ms", reserve},
                        {"calibration_reserve_ms", calibration}, {"material_credit_ms", material_credit},
                        {"frame_budget_ms", frame_milliseconds}, {"producer_ms", available},
                        {"screen_measured", state.m_screen_cost_samples > 0u},
                        {"surface_measured", state.m_surface_cost_submission != kFenceValueNone}});
            }
            if (not *budgeted) {
                if (not state.m_temporal_budget_exhausted) {
                    PPR_LOG(
                        Editor, warning, "WORLD_TEMPORAL_FRAME_BUDGET_EXHAUSTED",
                        {{"frame", actual_frame}, {"surface_ms", surface_milliseconds},
                            {"screen_lighting_estimate_ms", reserve}, {"calibration_reserve_ms", calibration},
                            {"total_ms", state.m_temporal_budget_milliseconds}});
                }
                state.m_temporal_budget_exhausted = true;
                state.m_submission_pending = true;
                return {};
            }
            state.m_temporal_budget_exhausted = false;
        }

        auto &submission = matter      ? state.m_matter_submission
                           : bounce    ? state.m_bounce_submission
                           : discovery ? state.m_discovery_submission
                                       : state.m_submission;

        bool encode_started{}, settled{};
        state.m_submission_pending = false;
        state.m_retainable_failure = false;

        const auto settle = [&]() -> std::error_code {
            settled = true;
            const auto submitted = renderer.lastSubmittedFenceValue();
            if (submitted > before) {
                submission = submitted;
                if (discovery and state.m_discovery_first_submission == kFenceValueNone)
                    state.m_discovery_first_submission = submitted;
                const auto stamped = state.m_pass.notifySubmitted(submitted);
                if (stamped) {
                    // The queue fence exists, but failed ownership bookkeeping
                    // cannot become an idle/cancel-ready claim.
                    state.m_stage = TerrainLightingState::Stage::submission_unknown;
                }

                PPR_RETURN_ERROR_ON_FAIL(Editor, stamped);

                observeTerrainPublicationPhase_(
                    m_measure_world_radiance,
                    matter      ? "matter.submit.accepted"
                    : bounce    ? "bounce.submit.accepted"
                    : discovery ? "discovery.submit.accepted"
                                : "field.submit.accepted",
                    state.m_candidate_cohort, discovery ? state.m_discovery_epoch : 0u, submitted);
                return {};
            }

            if (state.m_submission_pending) {
                // No encoder touched this prepared batch. Keep the exact
                // admitted source/camera/resources for the next credit probe.
                return {};
            }

            if (renderer.lastSubmitSettlement() == FrameSettlement::kRejected) {
                if (encode_started) {
                    // encode marks its batch before fallible pass creation. An
                    // entered callback needs abandonment even if encode failed.
                    PPR_RETURN_ERROR_ON_FAIL(Editor, state.m_pass.discardUnsubmitted());
                }

                state.m_stage = TerrainLightingState::Stage::idle;
                return {};
            }

            // No accepted fence and no definite rejection is unknown fate.
            // Persist this before propagation: neither cancel nor prepare may
            // release/reuse the batch without terminal positive quiescence.
            state.m_stage = TerrainLightingState::Stage::submission_unknown;
            if (encode_started) {
                PPR_RETURN_ERROR_ON_FAIL(Editor, state.m_pass.notifySubmitFailed());
            }
            return {};
        };

        PPR_DEFER {
            if (not settled) {
                // Exception after accepted submission still stamps its fence.
                // Failure to settle would leave completion unknown: retain/fail-stop.
                if (settle()) {
                    hal::fatalExit("lighting-submission-settlement-unknown");
                }
            }
        };

        const auto encode = [&](const PreRenderContext &context) {
            encode_started = true;
            const auto result = matter      ? state.m_pass.encodeMatter(context, actual_frame, matter_allowance_ms)
                                : bounce    ? state.m_pass.encodeBounce(context, actual_frame)
                                : discovery ? state.m_pass.encodeDiscovery(context, actual_frame)
                                            : state.m_pass.encode(context, actual_frame);
            if (matter and result == std::errc::resource_unavailable_try_again)
                encode_started = false; // prepareMatter's documented pre-encode epoch check touched no batch.

            return result;
        };

        const auto error = renderer.renderToTexture(*state.m_submission_target, {}, {}, PreRenderCallback{encode});

        // Renderer stamps capacity rejection before invoking preparation. Never
        // classify an entered encoder, accepted fence or unknown submit as retry.
        state.m_submission_pending = error == std::errc::device_or_resource_busy and
                                     not encode_started and
                                     renderer.lastSubmittedFenceValue() == before and
                                     renderer.lastSubmitSettlement() == FrameSettlement::kRejected;
        const auto settled_error = settle();
        PPR_RETURN_ERROR_ON_FAIL(Editor, settled_error);
        if (state.m_submission_pending) {
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(Editor, error);
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, submission > before ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
        return {};
    }

    std::error_code ApplicationEditor::beginTerrainLighting_(const GridWorldCommand &command) {
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, m_lane_shader_service.isValid() and
                            m_retro_pass and
                            command.m_stage.m_world_radiance_source and
                            not command.m_stage.m_lighting_receivers and
                            command.m_lighting_camera and
                            command.m_lighting_extent.x > 0 and
                            command.m_lighting_extent.y > 0
                        ? std::error_code{}
                        : std::make_error_code(std::errc::invalid_argument));

        if (not m_terrain_lighting)
            m_terrain_lighting = std::make_unique<TerrainLightingState>();

        auto &state = *m_terrain_lighting;
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, state.m_stage != TerrainLightingState::Stage::submission_unknown
                        ? std::error_code{}
                        : std::make_error_code(std::errc::state_not_recoverable));

        const auto discovery_retired = cancelTerrainDiscovery_();
        PPR_RETURN_ERROR_ON_FAIL(Editor, discovery_retired);
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, *discovery_retired ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));

        PPR_RETURN_ERROR_ON_FAIL(
            Editor, state.m_stage == TerrainLightingState::Stage::idle
                        ? std::error_code{}
                        : std::make_error_code(std::errc::device_or_resource_busy));

        if (not state.m_initialized) {
            const auto error = state.m_pass.initialize(
                m_lane_rhi_service->getDevice(), *m_lane_shader_service, getContentDir().path());
            if (error) {
                PPR_LOG_WARNING_ON_FAIL(Editor, state.m_pass.shutdown({GpuQuiescence::kIdleProven, {}}));
                PPR_RETURN_ERROR_ON_FAIL(Editor, error);
            }

            state.m_initialized = true;
        }

        if (not state.m_submission_target) {
            rhi::TextureDesc description{};
            description.size = {1u, 1u, 1u};
            description.format = rhi::Format::RGBA16Float;
            description.usage = rhi::TextureUsage::RenderTarget;
            description.defaultState = rhi::ResourceState::RenderTarget;
            description.label = "Editor small-scene world radiance submission";
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, m_lane_rhi_service->getDevice().createTexture(
                            description, nullptr, state.m_submission_target.writeRef()));
        }

        PPR_RETURN_ERROR_ON_FAIL(Editor, m_retro_pass->enableCompletedPictureRetention());

        lighting::WorldRadianceSettings settings{
            4ull * 1024u * 1024u * 1024u, 1024ull * 1024u * 1024u, 512ull * 1024u * 1024u, 0u, 0.5f, false, false,
            lighting::WorldRadianceRefinedSpacing::native_cell_centres,
            lighting::WorldRadianceTransport::straight256_reference, 65504.0f};
        settings.m_measure_gpu = m_measure_world_radiance;
        settings.m_diagnostic_coefficients = command.m_direct_window and
                                             surfaceReadbackEnabled() and
                                             m_measure_world_radiance and
                                             command.m_lighting_receiver_demands.size() == 3u;
        if (command.m_direct_window and not command.m_temporal_cascades) {
            const auto window = lighting::planWorldRadianceReceiverWindow(
                *command.m_stage.m_world_radiance_source, *command.m_lighting_camera,
                command.m_lighting_receiver_demands);
            state.m_retainable_failure = not window and retainableTerrainViewError_(window.error());
            PPR_RETURN_ERROR_ON_FAIL(Editor, window);

            const bool discovery_measurement = surfaceReadbackEnabled() and m_measure_world_radiance and m_observe_grid;
            if (discovery_measurement and (window->m_counts[0] == 0u or window->m_counts[1] == 0u)) {
                PPR_LOG(
                    Editor, error,
                    "Discovery measurement requires a nonempty receiver window; exterior without demands is unsupported");
                PPR_RETURN_ERROR_ON_FAIL(Editor, std::errc::not_supported);
            }

            settings.m_transport = lighting::WorldRadianceTransport::direct_window;
            settings.m_receiver_window = *window;
            settings.m_receiver_rows_per_quantum = command.m_single_bounce ? 256u : 512u;
        }

        if (command.m_temporal_cascades) {
            if (not state.m_screen_queries) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, m_lane_rhi_service->getDevice().getInfo().timestampFrequency > 0u
                                ? std::error_code{}
                                : std::make_error_code(std::errc::not_supported));
                rhi::QueryPoolDesc queries{};
                queries.type = rhi::QueryType::Timestamp;
                queries.count = kScreenTimingSlots * kScreenTimingStride;
                queries.label = "World radiance screen bounds";
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor,
                    m_lane_rhi_service->getDevice().createQueryPool(queries, state.m_screen_queries.writeRef()));
            }

            const auto desired = lighting::planWorldRadianceReceiverWindow(
                *command.m_stage.m_world_radiance_source, *command.m_lighting_camera,
                command.m_lighting_receiver_demands);
            state.m_retainable_failure = not desired and retainableTerrainViewError_(desired.error());
            PPR_RETURN_ERROR_ON_FAIL(Editor, desired);
            settings.m_receiver_window = desired->m_counts[0] > 0u and desired->m_counts[1] > 0u
                                             ? *desired
                                             : temporalRefreshWindow_(*command.m_stage.m_world_radiance_source, 0u);
            settings.m_temporal_coverage_window = settings.m_receiver_window;
            settings.m_transport = lighting::WorldRadianceTransport::temporal_cascades;
            settings.m_reserve_bytes += GridPass::kMaterialTimingReserve;
            settings.m_temporal = command.m_temporal_settings;
            settings.m_measure_gpu = true;
            settings.m_temporal_frame_allowance_ms = std::max(
                                                         command.m_stage.m_world_radiance_source->width(),
                                                         command.m_stage.m_world_radiance_source->height()) > 2048u
                                                         ? 10.0f
                                                         : 6.0f;
            settings.m_temporal_skip_table = command.m_temporal_skip_table;
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, command.m_lighting_request_elapsed >= TimeSpan{}
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));
            settings.m_temporal_sample_time_ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(command.m_lighting_request_elapsed).count());
            settings.m_diagnostic_coefficients = false;
        }

        // Game's existing worldgen-report observation switch is part of this
        // explicit route; ordinary direct captures keep their earlier policy.
        const bool discovery_enabled =
            not command.m_temporal_cascades and
            (command.m_single_bounce or
                (command.m_direct_window and surfaceReadbackEnabled() and m_measure_world_radiance and m_observe_grid));

        const lighting::TerrainReceiverCohort cohort{
            command.m_job_id, command.m_world_generation, command.m_content_fingerprint,
            command.m_stage.m_world_radiance_revision};
        if (command.m_direct_window and
            not command.m_temporal_cascades and
            command.m_stage.m_world_radiance_source->width() == 4096u and
            command.m_stage.m_world_radiance_source->height() == 4096u) {
            // Explicit complete-set admission stress, separately from the
            // playable request. Its112-byte rows exceed this unchanged512MiB
            // single-resource ceiling, so prepare must reject before allocation.
            auto stress = settings;
            stress.m_receiver_window = {{0, 0}, {4096u, 4096u}};
            const auto complete = state.m_pass.prepare(
                command.m_stage.m_world_radiance_source, cohort, stress, getRenderer().completedFenceValue());
            PPR_RETURN_ERROR_ON_FAIL(Editor, complete);
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, complete->m_admission_error == std::errc::no_buffer_space
                            ? std::error_code{}
                            : std::make_error_code(std::errc::state_not_recoverable));

            if (m_measure_world_radiance) {
                PPR_LOG(
                    Editor, info, "Direct-window ALL-PAGES admission stress rejected",
                    {{"receivers", complete->m_expected_rows}, {"queries_bound", complete->m_query_upper_bound},
                        {"field_bytes", complete->m_refined_bytes}, {"peak_bytes", complete->m_peak_bytes},
                        {"resource_ceiling", stress.m_max_resource_bytes}, {"full_world_budget_qualified", false},
                        {"GPU_stress", "not executed; complete set failed admission"}});
            }
        }

        observeTerrainPublicationPhase_(
            m_measure_world_radiance, "field.cpu-prepare.begin", cohort, command.m_sequence);

        const auto demand = state.m_pass.prepare(
            command.m_stage.m_world_radiance_source, cohort, settings, getRenderer().completedFenceValue());
        PPR_RETURN_ERROR_ON_FAIL(Editor, demand ? std::error_code{} : demand.error());
        PPR_RETURN_ERROR_ON_FAIL(Editor, demand->m_admission_error);

        // A playable temporal world always consumes committed Matter. Compile its
        // shared projection programs during world setup, before the first request.
        if (command.m_temporal_cascades)
            PPR_RETURN_ERROR_ON_FAIL(Editor, state.m_pass.prepareMatterPipeline());

        if (discovery_enabled) {
            PPR_RETURN_ERROR_ON_FAIL(Editor, state.m_pass.prepareDiscoveryPipeline());
        }

        state.m_discovery_donor.reset();

        observeTerrainPublicationPhase_(m_measure_world_radiance, "field.cpu-prepare.end", cohort, command.m_sequence);

        if (m_measure_world_radiance) {
            PPR_LOG(
                Editor, info, "World radiance field admitted",
                {{"queries", demand->m_query_upper_bound}, {"rows", demand->m_expected_rows},
                    {"peak_bytes", demand->m_peak_bytes}, {"field_bytes", demand->m_refined_bytes},
                    {"transport", static_cast<u32>(settings.m_transport)}, {"direct_window", command.m_direct_window},
                    {"temporal_levels", command.m_temporal_cascades ? settings.m_temporal.m_cascade_count : 0u},
                    {"temporal_spacing", command.m_temporal_cascades ? settings.m_temporal.m_refined_spacing : 0u},
                    {"r8_skip_table", settings.m_temporal_skip_table},
                    {"receiver_min_x", settings.m_receiver_window.m_minimum[0]},
                    {"receiver_min_y", settings.m_receiver_window.m_minimum[1]},
                    {"receiver_count_x", settings.m_receiver_window.m_counts[0]},
                    {"receiver_count_y", settings.m_receiver_window.m_counts[1]},
                    {"pages_pending", state.m_pass.pendingBatches()}});
        }

        if (not state.m_source or
            state.m_source->cohort().m_world_generation != cohort.m_world_generation or
            state.m_source->cohort().m_content_fingerprint != cohort.m_content_fingerprint) {
            state.m_temporal_refresh_finished = command.m_lighting_request_elapsed;
            state.m_temporal_refresh_cursor = 0u;
            state.m_temporal_refresh_started = command.m_lighting_request_elapsed;
        }
        state.m_temporal_refresh_pending = false;
        state.m_temporal_refresh_next = true;

        state.m_candidate_terrain_source = command.m_stage.m_world_radiance_source;
        state.m_candidate_cohort = cohort;
        state.m_candidate_camera = *command.m_lighting_camera;
        state.m_candidate_request_elapsed = command.m_lighting_request_elapsed;
        state.m_candidate_extent = command.m_lighting_extent;
        state.m_candidate_source.reset();
        state.m_reported_timing_status.reset();
        state.m_error.clear();
        state.m_discovery_error.clear();
        state.m_loading = true;
        state.m_grid_upload_begin_observed = false;
        state.m_grid_upload_end_observed = false;
        state.m_stage = TerrainLightingState::Stage::field;
        state.m_direct_window = command.m_direct_window;
        state.m_temporal_cascades = command.m_temporal_cascades;
        state.m_temporal_settings = command.m_temporal_settings;
        state.m_temporal_budget_milliseconds = settings.m_temporal_frame_allowance_ms;
        state.m_temporal_skip_table = command.m_temporal_skip_table;
        state.m_bounce_enabled = command.m_single_bounce;
        state.m_discovery_enabled = discovery_enabled;
        state.m_window_pending_elapsed = state.m_direct_window
                                             ? std::optional<TimeSpan>{command.m_lighting_request_elapsed}
                                             : std::nullopt;
        state.m_window_update = false;
        state.m_attempted_window =
            state.m_direct_window ? std::optional<lighting::WorldRadianceReceiverWindow>{settings.m_receiver_window}
                                  : std::nullopt;
        state.m_rejected_window.reset();
        state.m_rejected_cohort = {};
        if (not state.m_direct_window) {
            PPR_RETURN_ERROR_ON_FAIL(Editor, submitTerrainLighting_());
        }
        return {};
    }

    Expected<bool> ApplicationEditor::advanceTerrainLighting_() {
        auto &state = *m_terrain_lighting;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Editor, state.m_stage != TerrainLightingState::Stage::submission_unknown
                        ? std::error_code{}
                        : std::make_error_code(std::errc::state_not_recoverable));

        if (state.m_stage == TerrainLightingState::Stage::idle)
            return bool(state.m_candidate_source);

        if (state.m_direct_window) {
            if (state.m_pass.canEncodeNextBatch()) {
                if (not state.m_loading) {
                    // No page is submitted. The exact preceding accepted page
                    // (if any) already completed before allowing this state.
                    state.m_submission_pending = false;
                    state.m_stage = TerrainLightingState::Stage::idle;
                }
                return false;
            }

            const auto complete = state.m_pass.advance(getRenderer().completedFenceValue());
            if (not complete and
                complete.error() == std::errc::result_out_of_range and
                state.m_submission != kFenceValueNone and
                slotFenceComplete(getRenderer().completedFenceValue(), state.m_submission)) {
                // Numerical rejection after the actual page fence is proven.
                // No later page is submitted; cancellation can retire this
                // failed candidate while keeping the previous completed world.
                state.m_stage = TerrainLightingState::Stage::idle;
                state.m_retainable_failure = true;
                state.m_error = complete.error();
                if (state.m_window_update) {
                    state.m_rejected_window = state.m_attempted_window;
                    state.m_rejected_cohort = state.m_candidate_cohort;
                    state.m_loading = state.m_window_update = false;
                    state.m_candidate_source.reset();
                }
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, complete);
            if (not *complete) {
                if (not state.m_loading and state.m_pass.canEncodeNextBatch())
                    state.m_stage = TerrainLightingState::Stage::idle;
                return false;
            }
        }

        if (state.m_submission_pending and not state.m_direct_window) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, submitTerrainLighting_());
            if (state.m_submission_pending)
                return false;
        }

        const auto completed = getRenderer().completedFenceValue();
        if (not slotFenceComplete(completed, state.m_submission))
            return false;

        observeTerrainPublicationPhase_(
            m_measure_world_radiance, "field.fence-observed.acquire.begin", state.m_candidate_cohort, 0u,
            state.m_submission);

        state.m_stage = TerrainLightingState::Stage::idle;
        if (state.m_direct_window and not state.m_loading)
            return false; // Retire canceled input without publishing it as a donor.

        const auto generation = state.m_pass.acquire(completed);
        state.m_retainable_failure = not generation and retainableTerrainViewError_(generation.error());
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, generation);

        observeTerrainPublicationPhase_(
            m_measure_world_radiance, "field.fence-observed.acquire.end", state.m_candidate_cohort, 0u,
            state.m_submission);

        observeTerrainLighting_();

        if (not state.m_loading)
            return false; // Accepted canceled work retires but cannot publish into an unrelated world.

        state.m_candidate_source = *generation;
        state.m_discovery_donor = *generation;
        return true;
    }

    std::error_code ApplicationEditor::observeScreenLighting_() {
        if (not m_terrain_lighting or not m_terrain_lighting->m_screen_queries)
            return {};

        auto &state = *m_terrain_lighting;
        PPR_RETURN_ERROR_ON_FAIL(
            Editor,
            state.m_screen_timing_unknown ? std::make_error_code(std::errc::state_not_recoverable) : std::error_code{});
        const auto completed = getRenderer().completedFenceValue();
        for (u32 slot = 0u; slot < state.m_screen_query_fences.size(); ++slot) {
            const auto fence = state.m_screen_query_fences[slot];
            if (fence == kFenceValueNone or not slotFenceComplete(completed, fence))
                continue;

            if (not state.m_screen_query_complete[slot]) {
                state.m_screen_query_fences[slot] = kFenceValueNone;
                continue;
            }

            const u32 band = state.m_material_query_bands[slot];
            const u32 query_count = band < kNoMaterialBand ? kScreenTimingStride : 8u;
            rhi::QueryResultState readiness{};
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, state.m_screen_queries->getResultState(kScreenTimingStride * slot, query_count, &readiness));
            if (readiness == rhi::QueryResultState::Pending)
                continue;
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, readiness == rhi::QueryResultState::Resolved
                            ? std::error_code{}
                            : std::make_error_code(std::errc::state_not_recoverable));

            std::array<u64, kScreenTimingStride> ticks{};
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, state.m_screen_queries->getResult(kScreenTimingStride * slot, query_count, ticks.data()));
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, ticks[1] >= ticks[0] and ticks[3] >= ticks[2] and ticks[5] >= ticks[4] and ticks[7] >= ticks[6]
                            ? std::error_code{}
                            : std::make_error_code(std::errc::result_out_of_range));
            const double scale = 1000.0 / double(m_lane_rhi_service->getDevice().getInfo().timestampFrequency);
            const double prepare_ms = double(ticks[1] - ticks[0]) * scale;
            const double background_ms = double(ticks[3] - ticks[2]) * scale;
            const double terrain_ms = double(ticks[5] - ticks[4]) * scale;
            const double particles_ms = double(ticks[7] - ticks[6]) * scale;
            double calibration_ms{}, sample_ms{};
            if (band < kNoMaterialBand) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, std::is_sorted(ticks.begin() + 8u, ticks.end())
                                ? std::error_code{}
                                : std::make_error_code(std::errc::result_out_of_range));
                calibration_ms = double(ticks[12] - ticks[8]) * scale;
                state.m_material_timing.observeOverhead(static_cast<float>(calibration_ms));
                if (state.m_material_query_epochs[slot] == state.m_material_timing.m_epoch) {
                    const double fixed = double(ticks[10] - ticks[9]) * scale;
                    const double stripe = double(ticks[11] - ticks[10]) * scale;
                    sample_ms = std::max(0.0, stripe - fixed);
                    state.m_material_timing.observe(band, static_cast<float>(sample_ms));
                }
            }

            const float total = static_cast<float>(
                prepare_ms + background_ms + terrain_ms + particles_ms + calibration_ms);
            PPR_RETURN_ERROR_ON_FAIL(
                Editor,
                std::isfinite(total) ? std::error_code{} : std::make_error_code(std::errc::result_out_of_range));

            state.m_screen_terrain_ms[state.m_screen_cost_next] = static_cast<float>(terrain_ms);
            state.m_screen_gpu_milliseconds[state.m_screen_cost_next] = static_cast<float>(
                prepare_ms + background_ms + terrain_ms + particles_ms);
            state.m_screen_cost_next = (state.m_screen_cost_next + 1u) % state.m_screen_gpu_milliseconds.size();
            state.m_screen_cost_samples = std::min(state.m_screen_cost_samples + 1u, 8u);
            state.m_screen_query_fences[slot] = kFenceValueNone;
            if (m_measure_world_radiance) {
                PPR_LOG(
                    Editor, info, "WORLD_SCREEN_LIGHTING_BOUND",
                    {{"frame", state.m_screen_query_frames[slot]}, {"fence", fence}, {"prepare_ms", prepare_ms},
                        {"background_ms", background_ms}, {"terrain_ms", terrain_ms}, {"particles_ms", particles_ms},
                        {"calibration_ms", calibration_ms},
                        {"material_credit_ms", state.m_screen_query_material_credit[slot]},
                        {"material_sample_ms", sample_ms}, {"material_band", band}, {"total_ms", total}});
            }
        }
        return {};
    }

    void ApplicationEditor::observeTerrainLighting_() {
        if (not m_measure_world_radiance or not m_terrain_lighting) {
            return;
        }

        auto &state = *m_terrain_lighting;
        const auto timing = state.m_pass.pollTiming(state.m_candidate_cohort, getRenderer().completedFenceValue());
        if (state.m_reported_timing_status == timing.m_status) {
            return;
        }

        state.m_reported_timing_status = timing.m_status;

        const auto status_name = [](const lighting::WorldRadianceTimingStatus status) {
            switch (status) {
                case lighting::WorldRadianceTimingStatus::disabled: return "disabled";
                case lighting::WorldRadianceTimingStatus::pending: return "pending";
                case lighting::WorldRadianceTimingStatus::ready: return "ready";
                case lighting::WorldRadianceTimingStatus::unsupported: return "unsupported";
                case lighting::WorldRadianceTimingStatus::failed: return "failed";
                case lighting::WorldRadianceTimingStatus::retired_unresolved: return "retired-unresolved";
                case lighting::WorldRadianceTimingStatus::not_found: return "not-found";
            }

            return "invalid-status";
        };

        PPR_LOG(
            Editor, info, "World radiance producer GPU observation",
            {{"job", timing.m_cohort.m_job_id}, {"world", timing.m_cohort.m_world_generation},
                {"content", timing.m_cohort.m_content_fingerprint}, {"revision", timing.m_cohort.m_geometry_revision},
                {"submission", timing.m_submission}, {"status", status_name(timing.m_status)},
                {"error", timing.m_error.message()}, {"has_gpu_milliseconds", timing.m_milliseconds.has_value()},
                {"scope",
                    "producer encode including mandatory statistics copy; excludes CPU prepare and source uploads"},
                {"query_allocation", "opaque native overhead; reserve-covered, not measured VRAM"}});

        if (timing.m_milliseconds) {
            PPR_LOG(
                Editor, info, "World radiance producer GPU elapsed",
                {{"submission", timing.m_submission}, {"gpu_milliseconds", *timing.m_milliseconds},
                    {"qualified_product_budget", false}});
        }
    }

    // ------------------------------------------------------------------
    // explicit discovery measurement on the actual rendered-frame schedule
    // ------------------------------------------------------------------

    Expected<bool> ApplicationEditor::cancelTerrainDiscovery_() {
        auto &state = *m_terrain_lighting;
        if (state.m_projected_matter_source) {
            state.m_projected_matter_source.reset();
            state.m_matter_frozen.reset();
            state.m_matter_after_frozen_elapsed.reset();
        }

        if (state.m_matter_prepared) {
            const auto retired = state.m_pass.cancelMatter(getRenderer().completedFenceValue());
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
            if (not *retired)
                return false;

            state.m_matter_prepared = false;
            state.m_matter_frozen.reset();
            state.m_matter_after_frozen_elapsed.reset();
            state.m_submission_pending = false;
            state.m_stage = TerrainLightingState::Stage::idle;
        }

        if (state.m_bounce_prepared) {
            const auto retired = state.m_pass.cancelBounce(getRenderer().completedFenceValue());
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
            if (not *retired)
                return false;

            state.m_bounce_prepared = false;
            state.m_submission_pending = false;
        }

        if (not state.m_discovery_prepared)
            return true;

        const auto retired = state.m_pass.cancelDiscovery(getRenderer().completedFenceValue());
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
        if (not *retired)
            return false;

        state.m_discovery_prepared = false;
        state.m_submission_pending = false;
        // Keep the immutable completed result until its replacement completes.
        // Its independent buffers/overlap remain charged by the producer ledger.
        return true;
    }

    Expected<bool> ApplicationEditor::updateTerrainDiscovery_(
        const CameraSnapshot &camera, const TimeSpan elapsed, const lighting::WorldRadianceReceiverWindow window,
        const u64 actual_frame) {
        auto &state = *m_terrain_lighting;
        if (not state.m_discovery_enabled)
            return true;

        state.m_retainable_failure = false;
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, state.m_discovery_error);

        const auto source = state.m_source->source();
        const auto cohort = state.m_source->cohort();
        const auto matches = [&] {
            return state.m_discovery_result and
                   state.m_discovery_result->source().get() == source.get() and
                   state.m_discovery_result->cohort() == cohort and
                   state.m_discovery_result->window().m_minimum == window.m_minimum and
                   state.m_discovery_result->window().m_counts == window.m_counts;
        };

        if (not matches() and not state.m_window_pending_elapsed)
            state.m_window_pending_elapsed = elapsed;

        // A completed bounce still owns its pass workspace after discovery
        // scratch has retired. A changed covered subwindow must retire it too.
        if ((state.m_bounce_prepared and not matches()) or
            (state.m_discovery_prepared and (state.m_discovery_window.m_minimum != window.m_minimum or
                                                state.m_discovery_window.m_counts != window.m_counts))) {
            const auto retired = cancelTerrainDiscovery_();
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
            if (not *retired)
                return false;
        }

        if (matches()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Editor, state.m_discovery_result->maximumGpuMilliseconds() and
                                (state.m_bounce_enabled or *state.m_discovery_result->maximumGpuMilliseconds() <= 6.0)
                            ? std::error_code{}
                            : std::make_error_code(std::errc::result_out_of_range));
            return true; // Camera/extent changes with identical query geometry reuse this exact result.
        }

        if (not state.m_discovery_prepared) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Editor, state.m_discovery_epoch != std::numeric_limits<u64>::max()
                            ? std::error_code{}
                            : std::make_error_code(std::errc::value_too_large));

            const lighting::WorldRadianceDiscoverySettings settings{
                4ull * 1024u * 1024u * 1024u, 1024ull * 1024u * 1024u, 512ull * 1024u * 1024u,
                ++state.m_discovery_epoch, window, 0u, true, false};
            const auto begin = observationClock().now();
            const auto demand = state.m_pass.prepareDiscovery(
                source, cohort, settings, getRenderer().completedFenceValue());
            state.m_discovery_prepare_cpu = observationClock().now() - begin;
            const auto error = demand ? demand->m_admission_error : demand.error();
            if (error) {
                state.m_discovery_error = error;
                const auto message = std::format(
                    "World discovery NO_GO epoch={} stage=prepare error={} cold={} CPU_setup_ms={} source=({},{}) world={} revision={}",
                    state.m_discovery_epoch, error.message(), state.m_discovery_epoch == 1u,
                    time::seconds(state.m_discovery_prepare_cpu) * 1000.0, source->width(), source->height(),
                    cohort.m_world_generation, cohort.m_geometry_revision);
                PPR_LOG_RAW(Editor, error, message);
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, error);

            state.m_discovery_camera = camera;
            state.m_discovery_window = window;
            state.m_discovery_request_elapsed = *state.m_window_pending_elapsed;
            state.m_discovery_submission = kFenceValueNone;
            state.m_discovery_first_submission = kFenceValueNone;
            state.m_discovery_prepared = true;

            const auto message = std::format(
                "World discovery admitted epoch={} frame={} camera={} minimum=({},{}) counts=({},{}) "
                "trace_calls={} records={} quanta={} capacity={} bitset_bytes={} key_bytes={} OWNED_bytes={} peak_model_bytes={} "
                "external_reserve_ESTIMATE={} cold_setup={} CPU_setup_ms={} source=({},{}) source_instance={} job={} world={} revision={} content={}",
                state.m_discovery_epoch, actual_frame, camera.m_revision, window.m_minimum[0], window.m_minimum[1],
                window.m_counts[0], window.m_counts[1], demand->m_trace_upper_bound, demand->m_record_count,
                demand->m_batches, demand->m_key_capacity, demand->m_bitset_bytes, demand->m_key_bytes,
                demand->m_owned_bytes, demand->m_peak_bytes, settings.m_reserve_bytes, state.m_discovery_epoch == 1u,
                time::seconds(state.m_discovery_prepare_cpu) * 1000.0, source->width(), source->height(),
                reinterpret_cast<std::uintptr_t>(source.get()), cohort.m_job_id, cohort.m_world_generation,
                cohort.m_geometry_revision, cohort.m_content_fingerprint);
            PPR_LOG_RAW(Editor, info, message);
        }

        const auto completed = getRenderer().completedFenceValue();
        const auto ready = state.m_pass.advanceDiscovery(completed);
        if (not ready) {
            state.m_discovery_error = ready.error();
            const auto message = std::format(
                "World discovery NO_GO epoch={} stage=completion error={}", state.m_discovery_epoch,
                ready.error().message());
            PPR_LOG_RAW(Editor, error, message);
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, ready);

        // Retire the completed quantum before admitting its successor on this
        // render request. The shared frame stamp still limits all phases to one.
        if (state.m_pass.canEncodeDiscovery()) {
            if (state.m_last_field_frame != actual_frame) {
                state.m_last_field_frame = actual_frame;
                const auto error = submitTerrainLighting_(true, actual_frame);
                if (error)
                    state.m_discovery_error = error;

                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, error);
            }
            return false;
        }

        if (not *ready)
            return false;

        const auto result = state.m_pass.acquireDiscovery(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, result);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Editor, (*result)->source().get() == source.get() and
                            (*result)->cohort() == cohort and
                            (*result)->requestEpoch() == state.m_discovery_epoch and
                            (*result)->window().m_minimum == window.m_minimum and
                            (*result)->window().m_counts == window.m_counts and
                            (*result)->completionFence() == state.m_discovery_submission and
                            slotFenceComplete(completed, (*result)->completionFence()) and
                            (*result)->maximumGpuMilliseconds()
                        ? std::error_code{}
                        : std::make_error_code(std::errc::state_not_recoverable));

        state.m_discovery_result = *result;
        const auto &statistics = (*result)->statistics();
        const double age_ms = time::seconds(elapsed - state.m_discovery_request_elapsed) * 1000.0;
        const double maximum_gpu_ms = *(*result)->maximumGpuMilliseconds();
        const bool gpu_budget = maximum_gpu_ms <= 6.0;
        if (not gpu_budget and not state.m_bounce_enabled)
            state.m_discovery_error = std::make_error_code(std::errc::result_out_of_range);

        const auto message = std::format(
            "World discovery completed epoch={} frame={} request_camera={} completed_camera={} request_fence={} completion_fence={} "
            "minimum=({},{}) counts=({},{}) traces={} unique_faces={} hit_occurrences={} conservative_offscreen_distinct={} "
            "segments={} skipped={} maximum_quantum_GPU_ms={} request_to_matching_discovery_readiness_ms={} cold_setup={} CPU_setup_ms={} "
            "normal_GPU_6ms={} warm_age_1s={} budget_verdict={} source_instance={} job={} world={} revision={} content={} moments_qualified=false full_live_VRAM_UNMEASURED=true",
            state.m_discovery_epoch, actual_frame, state.m_discovery_camera.m_revision, camera.m_revision,
            u64{state.m_discovery_first_submission}, u64{(*result)->completionFence()}, window.m_minimum[0],
            window.m_minimum[1], window.m_counts[0], window.m_counts[1], statistics.m_queries_completed,
            (*result)->faceCount(), (*result)->faceHitOccurrences(), (*result)->conservativeOffscreenFaceCount(),
            statistics.m_segments, statistics.m_skipped_blocks, maximum_gpu_ms, age_ms, state.m_discovery_epoch == 1u,
            time::seconds(state.m_discovery_prepare_cpu) * 1000.0, gpu_budget,
            state.m_discovery_epoch == 1u ? "COLD_REPORT_ONLY"
            : age_ms <= 1000.0            ? "PASS"
                                          : "NO_GO",
            gpu_budget and (state.m_discovery_epoch == 1u or age_ms <= 1000.0) ? "WITHIN_MEASURED_COMPONENT_GATES"
                                                                               : "NO_GO",
            reinterpret_cast<std::uintptr_t>(source.get()), cohort.m_job_id, cohort.m_world_generation,
            cohort.m_geometry_revision, cohort.m_content_fingerprint);
        PPR_LOG_RAW(Editor, info, message);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, state.m_discovery_error);
        return true;
    }

    Expected<bool> ApplicationEditor::updateTerrainBounce_(
        const TimeSpan elapsed, const lighting::WorldRadianceReceiverWindow window, const u64 actual_frame) {
        auto &state = *m_terrain_lighting;
        if (not state.m_bounce_enabled)
            return true;

        const auto matches = [&](const auto &generation) {
            return generation and
                   generation->source().get() == state.m_source->source().get() and
                   generation->cohort() == state.m_source->cohort() and
                   generation->probeMinimum() == window.m_minimum and
                   generation->probeCounts() == window.m_counts and
                   state.m_discovery_result and
                   generation->bounceEpoch() == state.m_discovery_result->requestEpoch();
        };
        if (matches(state.m_combined_source))
            return true;

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Editor, state.m_window_pending_elapsed and state.m_discovery_result
                        ? std::error_code{}
                        : std::make_error_code(std::errc::state_not_recoverable));

        if (not state.m_bounce_prepared) {
            // Discovery scratch can retire; the immutable keys remain owned by
            // the result and then the bounce batch. No device wait or new queue.
            const auto retired = cancelTerrainDiscovery_();
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
            if (not *retired)
                return false;

            const lighting::WorldRadianceBounceSettings settings{
                4ull * 1024u * 1024u * 1024u, 1024ull * 1024u * 1024u, 512ull * 1024u * 1024u,
                state.m_discovery_result->requestEpoch(), true};
            const auto begin = observationClock().now();
            const auto admitted = state.m_pass.prepareBounce(
                state.m_discovery_result, settings, getRenderer().completedFenceValue(), &observationClock());
            const auto cpu_prepare = observationClock().now() - begin;
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, admitted);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, admitted->m_admission_error);

            state.m_bounce_prepared = true;
            state.m_bounce_submission = kFenceValueNone;
            const auto message = std::format(
                "WORLD_BOUNCE_ADMISSION epoch={} frame={} faces={} lookup_capacity={} moment_bytes={} lookup_bytes={} "
                "parity_bytes={} indirect_bytes={} combined_bytes={} trace_upper={} records={} quanta={} "
                "OWNED_new_bytes={} peak_model_bytes={} external_reserve_ESTIMATE={} CPU_prepare_ms={} "
                "source_instance={} job={} world={} content={} revision={}",
                settings.m_request_epoch, actual_frame, admitted->m_faces, admitted->m_lookup_capacity,
                admitted->m_moment_bytes, admitted->m_lookup_bytes, admitted->m_parity_bytes,
                admitted->m_indirect_bytes, admitted->m_combined_bytes, admitted->m_trace_upper_bound,
                admitted->m_record_count, admitted->m_batches, admitted->m_owned_bytes, admitted->m_peak_bytes,
                settings.m_reserve_bytes, time::seconds(cpu_prepare) * 1000.0,
                reinterpret_cast<std::uintptr_t>(state.m_source->source().get()), state.m_source->cohort().m_job_id,
                state.m_source->cohort().m_world_generation, state.m_source->cohort().m_content_fingerprint,
                state.m_source->cohort().m_geometry_revision);
            PPR_LOG_RAW(Editor, info, message);
        }

        if (state.m_pass.canEncodeBounce()) {
            if (state.m_last_field_frame != actual_frame) {
                state.m_last_field_frame = actual_frame;
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, submitTerrainLighting_(false, actual_frame, true));
            }
            return false;
        }

        const auto completed = getRenderer().completedFenceValue();
        const auto ready = state.m_pass.advanceBounce(completed);
        if (not ready)
            state.m_discovery_error = ready.error();
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, ready);
        if (not *ready)
            return false;

        const auto combined = state.m_pass.acquireBounce(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, combined);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Editor, matches(*combined) and
                            (*combined)->completionFence() == state.m_bounce_submission and
                            slotFenceComplete(completed, (*combined)->completionFence()) and
                            (*combined)->maximumGpuMilliseconds()
                        ? std::error_code{}
                        : std::make_error_code(std::errc::state_not_recoverable));

        const double age_ms = time::seconds(elapsed - *state.m_window_pending_elapsed) * 1000.0;
        const double maximum_gpu_ms = *(*combined)->maximumGpuMilliseconds();
        const bool gpu_budget = maximum_gpu_ms <= 6.0;
        const auto message = std::format(
            "WORLD_BOUNCE_COMPLETE epoch={} frame={} fence={} window_min=({},{}) window_count=({},{}) "
            "rays={} rows={} maximum_quantum_GPU_ms={} request_to_combined_ready_ms={} cold={} "
            "component_GPU_6ms={} warm_age_1s={} full_live_VRAM_UNMEASURED=true "
            "source_instance={} job={} world={} content={} revision={}",
            (*combined)->bounceEpoch(), actual_frame, u64{(*combined)->completionFence()}, window.m_minimum[0],
            window.m_minimum[1], window.m_counts[0], window.m_counts[1], (*combined)->statistics().m_queries_completed,
            (*combined)->statistics().m_rows_written, maximum_gpu_ms, age_ms, state.m_discovery_epoch == 1u,
            gpu_budget ? "WITHIN_COMPONENT" : "NO_GO",
            state.m_discovery_epoch == 1u ? "COLD_REPORT_ONLY"
            : age_ms <= 1000.0            ? "PASS"
                                          : "NO_GO",
            reinterpret_cast<std::uintptr_t>((*combined)->source().get()), (*combined)->cohort().m_job_id,
            (*combined)->cohort().m_world_generation, (*combined)->cohort().m_content_fingerprint,
            (*combined)->cohort().m_geometry_revision);
        PPR_LOG_RAW(Editor, info, message);

        // Timing NO_GO is evidence, not a correctness error for this explicit
        // measurement. Keep the receipt and finish its finite output/capture.
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, state.m_discovery_error);

        if (const auto identity = (*combined)->source()->matterIdentity()) {
            const auto statistics = (*combined)->source()->matterStatistics();
            const bool new_epoch = state.m_matter_frozen == identity;
            const bool same_epoch = state.m_matter_receipt and state.m_matter_receipt->m_identity == *identity;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Editor, statistics and (new_epoch or same_epoch)
                            ? std::error_code{}
                            : std::make_error_code(std::errc::state_not_recoverable));
            const auto requested = new_epoch ? state.m_matter_frozen_elapsed
                                             : state.m_matter_receipt->m_request_elapsed;
            const auto projected = new_epoch ? state.m_matter_projected_elapsed
                                             : state.m_matter_receipt->m_projection_ready_elapsed;

            const lighting::WorldRadianceEmissionAggregate aggregate{
                statistics->m_overlay_cells, statistics->m_emission_maximum, statistics->m_emission_bound};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Editor,
                lighting::admitWorldRadianceExposure(aggregate, std::exp2(double(m_retro_settings.m_exposure_ev))));

            state.m_combined_source = *combined;
            state.m_matter_receipt = WorldMatterLightingReceipt{
                *identity, (*combined)->source()->matterComparedIdentity(), *statistics,
                (*combined)->source()->matterCompletionFence(), (*combined)->completionFence(), requested, projected,
                elapsed};
        } else {
            state.m_combined_source = *combined;
        }

        state.m_publication_elapsed = elapsed;
        state.m_source_first_presented = false;
        return true;
    }

    // ------------------------------------------------------------------
    // immutable optical Matter epochs on the existing actual-frame work token
    // ------------------------------------------------------------------

    Expected<bool> ApplicationEditor::updateTerrainMatter_(
        const std::optional<GridParticleGpuView> &view, const CameraSnapshot &camera, const int2 extent,
        const TimeSpan elapsed, const std::span<const lighting::WorldRadianceReceiverWindow> demands,
        const u64 actual_frame) {
        auto &state = *m_terrain_lighting;
        if (not state.m_bounce_enabled and not state.m_temporal_cascades)
            return true;

        if (state.m_temporal_cascades and
            m_measure_world_radiance and
            elapsed - state.m_temporal_refresh_reported >= std::chrono::seconds{1}) {
            state.m_temporal_refresh_reported = elapsed;
            PPR_LOG(
                Editor, info, "WORLD_TEMPORAL_REFRESH_DEBT",
                {{"frame", actual_frame}, {"next_tile", state.m_temporal_refresh_cursor},
                    {"round_age_ms", time::seconds(elapsed - state.m_temporal_refresh_started) * 1000.0},
                    {"matter_pending", state.m_matter_pending_elapsed.has_value()}});
        }

        if (state.m_loading and not state.m_window_update)
            return false; // World bootstrap/replacement must first activate its provisional terrain.

        const bool current = view and
                             view->m_metadata and
                             view->current() and
                             state.m_terrain_source and
                             state.m_source and
                             view->m_world == GridWorldIdentity{
                                                  state.m_source->cohort().m_job_id,
                                                  state.m_source->cohort().m_world_generation,
                                                  state.m_source->cohort().m_content_fingerprint};
        if (not current and not state.m_matter_prepared) {
            // A revoked/missing live frame cannot admit a new snapshot or draw.
            // It still allows already accepted field work to settle independently.
            return bool(state.m_matter_frozen) or
                   state.m_window_update or
                   state.m_stage == TerrainLightingState::Stage::field or
                   state.m_discovery_prepared or
                   state.m_bounce_prepared;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Editor, current or state.m_matter_frozen ? std::error_code{}
                                                     : std::make_error_code(std::errc::state_not_recoverable));
        const lighting::WorldRadianceMatterIdentity requested =
            current
                ? lighting::
                      WorldRadianceMatterIdentity{{view->m_world, view->m_publication, view->m_terrain_revision, view->m_joint_tick}, view->m_device_epoch}
                : *state.m_matter_frozen;
        if (current) {
            if (not state.m_matter_latest or *state.m_matter_latest != requested) {
                state.m_matter_latest = requested;
                if (not state.m_matter_pending_elapsed)
                    state.m_matter_pending_elapsed = elapsed;

                if (state.m_matter_frozen and
                    *state.m_matter_frozen != requested and
                    not state.m_matter_after_frozen_elapsed) {
                    state.m_matter_after_frozen_elapsed = elapsed;
                    const auto message = std::format(
                        "WORLD_MATTER_COALESCED frame={} frozen_pub={} frozen_joint={} latest_pub={} latest_joint={} "
                        "oldest_pending_elapsed_ms={} first_after_frozen_elapsed_ms={} clock=explicit_frame_request_elapsed",
                        actual_frame, state.m_matter_frozen->m_particles.m_publication,
                        state.m_matter_frozen->m_particles.m_joint_tick, requested.m_particles.m_publication,
                        requested.m_particles.m_joint_tick, time::seconds(*state.m_matter_pending_elapsed) * 1000.0,
                        time::seconds(elapsed) * 1000.0);
                    PPR_LOG_RAW(Editor, info, message);
                }
            }

            // The selected canonical geometry is bound only once to the real Matter
            // terrain revision. A later revision needs a new staged canonical base.
            if (not state.m_bound_terrain_source) {
                const auto bound = lighting::WorldRadianceSource::bindMatterIdentity(
                    state.m_terrain_source, view->m_world, view->m_terrain_revision);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, bound);
                state.m_bound_terrain_source = *bound;
                state.m_bound_matter_terrain_revision = view->m_terrain_revision;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Editor, state.m_bound_matter_terrain_revision == view->m_terrain_revision
                            ? std::error_code{}
                            : std::make_error_code(std::errc::operation_not_permitted));
        }

        if (state.m_matter_frozen and not state.m_matter_prepared and not state.m_projected_matter_source)
            return true; // Finish this accepted field and its first actual present before supersession.

        const auto incorporated = state.m_source->source()->matterIdentity();
        if (not state.m_matter_prepared and
            not state.m_projected_matter_source and
            incorporated and
            *incorporated == requested)
            return true;

        if (not state.m_matter_prepared and not state.m_projected_matter_source) {
            const bool discovery_pending = state.m_discovery_prepared and
                                           (not state.m_discovery_result or
                                               state.m_discovery_result->requestEpoch() != state.m_discovery_epoch);
            const bool bounce_pending = state.m_bounce_prepared and
                                        (not state.m_combined_source or
                                            state.m_combined_source->bounceEpoch() != state.m_discovery_epoch or
                                            state.m_combined_source->source().get() != state.m_source->source().get());
            if (state.m_window_update or
                state.m_stage == TerrainLightingState::Stage::field or
                discovery_pending or
                bounce_pending or
                (incorporated and not state.m_source_first_presented))
                return true; // Finish accepted query geometry; queued source changes do not restart it.

            const auto retired = cancelTerrainDiscovery_();
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
            if (not *retired)
                return false;

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Editor, state.m_matter_epoch < std::numeric_limits<u64>::max()
                            ? std::error_code{}
                            : std::make_error_code(std::errc::value_too_large));
            const lighting::WorldRadianceMatterSettings settings{
                4ull * 1024u * 1024u * 1024u,
                1024ull * 1024u * 1024u + (state.m_temporal_cascades ? GridPass::kMaterialTimingReserve : 0u),
                512ull * 1024u * 1024u, ++state.m_matter_epoch, true};
            const auto begin = observationClock().now();
            const auto admitted = state.m_pass.prepareMatter(
                state.m_bound_terrain_source, *view, settings, getRenderer().completedFenceValue());
            const auto cpu_prepare = observationClock().now() - begin;
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, admitted);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, admitted->m_admission_error);

            state.m_matter_prepared = true;
            state.m_matter_frozen = requested;
            state.m_matter_frozen_elapsed = *state.m_matter_pending_elapsed;
            state.m_matter_admitted_elapsed = elapsed;
            state.m_matter_after_frozen_elapsed.reset();
            state.m_matter_submission = kFenceValueNone;
            state.m_stage = TerrainLightingState::Stage::matter;
            const auto message = std::format(
                "WORLD_MATTER_ADMISSION frame={} epoch={} pub={} terrain={} joint={} device={} rows={} quanta={} "
                "overlay_bytes={} lookup_bytes={} sort_bytes={} scan_bytes={} occupancy_bytes={} palette_bytes={} "
                "input_lease_bytes={} OWNED_bytes={} peak_model_bytes={} external_reserve_ESTIMATE={} CPU_prepare_ms={} "
                "oldest_request_elapsed_ms={} full_live_VRAM_UNMEASURED=true",
                actual_frame, settings.m_request_epoch, requested.m_particles.m_publication,
                requested.m_particles.m_terrain_revision, requested.m_particles.m_joint_tick, requested.m_device_epoch,
                admitted->m_input_rows, admitted->m_batches, admitted->m_overlay_bytes, admitted->m_lookup_bytes,
                admitted->m_sort_bytes, admitted->m_scan_bytes, admitted->m_occupancy_bytes, admitted->m_palette_bytes,
                admitted->m_input_lease_bytes, admitted->m_owned_bytes, admitted->m_peak_bytes,
                settings.m_reserve_bytes, time::seconds(cpu_prepare) * 1000.0,
                time::seconds(state.m_matter_frozen_elapsed) * 1000.0);
            PPR_LOG_RAW(Editor, info, message);
        }

        const auto completed = getRenderer().completedFenceValue();
        if (state.m_matter_prepared and not state.m_pass.canEncodeMatter()) {
            const auto ready = state.m_pass.advanceMatter(completed);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, ready);
            if (not *ready and not state.m_pass.canEncodeMatter())
                return false;

            // Retiring an intermediate fence can make the next quantum encodable
            // in this frame. The submission below still consumes the shared frame token.
        }

        if (state.m_matter_prepared and state.m_pass.canEncodeMatter()) {
            if (state.m_last_field_frame != actual_frame) {
                state.m_last_field_frame = actual_frame;
                const auto submitted = submitTerrainLighting_(false, actual_frame, false, true);
                if (submitted == std::errc::resource_unavailable_try_again) {
                    // Proven pre-encode revocation: no snapshot was accepted.
                    // Retry the latest view, preserving the oldest request age.
                    const auto retired = state.m_pass.cancelMatter(getRenderer().completedFenceValue());
                    PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        Editor, *retired ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
                    state.m_matter_prepared = false;
                    state.m_matter_frozen.reset();
                    state.m_matter_after_frozen_elapsed.reset();
                    state.m_stage = TerrainLightingState::Stage::idle;
                    return false;
                }

                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, submitted);
            }
            return false;
        }

        if (state.m_matter_prepared) {
            const auto projected = state.m_pass.acquireMatter(completed);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, projected);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Editor, (*projected)->matterIdentity() == state.m_matter_frozen and
                                (*projected)->matchesTerrainSource(*state.m_terrain_source) and
                                (*projected)->matterCompletionFence() == state.m_matter_submission
                            ? std::error_code{}
                            : std::make_error_code(std::errc::state_not_recoverable));
            const auto retired = state.m_pass.cancelMatter(completed);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Editor, *retired ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            state.m_projected_matter_source = *projected;
            state.m_matter_prepared = false;
            state.m_stage = TerrainLightingState::Stage::idle;
            state.m_matter_projected_elapsed = elapsed;
        }

        // A completed immutable projection survives a recoverable field
        // admission failure. A later admissible window retries this source,
        // rather than losing the frozen epoch or resnapshotting newer Hot data.
        const auto projected = state.m_projected_matter_source;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Editor, projected ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
        auto desired = lighting::planWorldRadianceReceiverWindow(*projected, camera, demands);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, desired);
        if (state.m_temporal_cascades and state.m_gameplay_demand and desired->m_counts[0] == 0u)
            *desired = state.m_gameplay_demand->m_window;

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Editor, desired->m_counts[0] != 0u and desired->m_counts[1] != 0u
                        ? std::error_code{}
                        : std::make_error_code(std::errc::not_supported));
        lighting::WorldRadianceSettings settings{
            4ull * 1024u * 1024u * 1024u, 1024ull * 1024u * 1024u, 512ull * 1024u * 1024u, 0u, .5f, false,
            m_measure_world_radiance, lighting::WorldRadianceRefinedSpacing::native_cell_centres,
            lighting::WorldRadianceTransport::direct_window, 65504.0f, *desired};
        settings.m_diagnostic_coefficients = surfaceReadbackEnabled() and
                                             m_measure_world_radiance and
                                             demands.size() == 3u;
        if (state.m_temporal_cascades) {
            settings.m_transport = lighting::WorldRadianceTransport::temporal_cascades;
            settings.m_reserve_bytes += GridPass::kMaterialTimingReserve;
            settings.m_temporal = state.m_temporal_settings;
            settings.m_measure_gpu = true;
            settings.m_temporal_frame_allowance_ms = state.m_temporal_budget_milliseconds;
            settings.m_temporal_skip_table = state.m_temporal_skip_table;
            settings.m_temporal_coverage_window = *desired;
            settings.m_gameplay_demand = state.m_gameplay_demand;

            // Share setup, source projection and overlapping cascade parents.
            // Leave room for the mandatory epoch before admitting one offscreen tile.
            const auto service = *std::ranges::max_element(state.m_matter_service_times);
            const auto sampled = std::ranges::count_if(
                state.m_matter_service_times, [](const TimeSpan duration) { return duration > TimeSpan{}; });
            const auto with_refresh = std::max<TimeSpan>(
                service + std::chrono::milliseconds{150}, *std::ranges::max_element(state.m_matter_refresh_times));
            // A newer request can arrive immediately after freezing this one.
            // Its wait includes this whole epoch plus its own mandatory service.
            const auto queued = state.m_matter_admitted_elapsed - state.m_matter_frozen_elapsed;
            if (state.m_source->skyHour() == state.m_temporal_settings.m_sky_hour and
                not state.m_gameplay_demand and
                sampled >= 4 and
                elapsed - state.m_temporal_refresh_finished >= std::chrono::seconds{1} and
                std::max(queued, service) + with_refresh + std::chrono::milliseconds{34} < std::chrono::seconds{1}) {
                settings.m_temporal_refresh_window = temporalRefreshWindow_(
                    *projected, state.m_temporal_refresh_cursor);
            }

            settings.m_prioritize_convergence = state.m_source->skyHour() == state.m_temporal_settings.m_sky_hour and
                                                not settings.m_temporal_refresh_window and
                                                projected->matchesProjectedOptics(*state.m_source->source()) and
                                                state.m_source->currentReceiverWindow().contains(*desired);

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Editor, elapsed >= TimeSpan{} ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            settings.m_temporal_sample_time_ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
            settings.m_diagnostic_coefficients = false;
        }
        const auto admitted = state.m_pass.prepare(projected, state.m_source->cohort(), settings, completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, admitted);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, admitted->m_admission_error);

        state.m_matter_includes_refresh = settings.m_temporal_refresh_window.has_value();
        state.m_temporal_refresh_pending = state.m_matter_includes_refresh;
        state.m_projected_matter_source.reset();
        state.m_discovery_donor.reset();
        state.m_candidate_cohort = state.m_source->cohort();
        state.m_attempted_window = *desired;
        state.m_candidate_camera = camera;
        state.m_candidate_extent = extent;
        state.m_candidate_request_elapsed = state.m_matter_frozen_elapsed;
        state.m_window_pending_elapsed = state.m_matter_frozen_elapsed;
        state.m_candidate_source.reset();
        state.m_error.clear();
        state.m_discovery_error.clear();
        state.m_loading = state.m_window_update = true;
        state.m_stage = TerrainLightingState::Stage::field;
        return true;
    }

    Expected<bool> ApplicationEditor::updateTerrainLightingView_(
        const CameraSnapshot &camera, const int2 extent, const TimeSpan elapsed,
        const std::span<const lighting::WorldRadianceReceiverWindow> demands, const u64 actual_frame) {
        auto &state = *m_terrain_lighting;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Editor, state.m_stage != TerrainLightingState::Stage::submission_unknown
                        ? std::error_code{}
                        : std::make_error_code(std::errc::state_not_recoverable));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Editor, state.m_source and extent.x > 0 and extent.y > 0
                        ? std::error_code{}
                        : std::make_error_code(std::errc::invalid_argument));

        if (state.m_temporal_cascades) {
            if (state.m_loading and not state.m_window_update)
                return false;

            if (state.m_window_update) {
                const auto ready = advanceTerrainLighting_();
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, ready);
                if (*ready) {
                    state.m_source = std::move(state.m_candidate_source);
                    if (state.m_source->supplementalRefreshYielded()) {
                        state.m_temporal_refresh_pending = false;
                        if (m_measure_world_radiance) {
                            PPR_LOG(
                                Editor, info, "WORLD_TEMPORAL_REFRESH_YIELDED",
                                {{"frame", actual_frame}, {"tile", state.m_temporal_refresh_cursor}});
                        }
                    }
                    if (state.m_temporal_refresh_pending) {
                        state.m_temporal_refresh_finished = elapsed;
                        if (m_measure_world_radiance) {
                            PPR_LOG(
                                Editor, info, "WORLD_TEMPORAL_REFRESH_COMPLETED",
                                {{"frame", actual_frame}, {"tile", state.m_temporal_refresh_cursor},
                                    {"same_matter_batch", state.m_matter_frozen.has_value()}});
                        }

                        const auto &source = *state.m_source->source();
                        const u32 tiles = ((source.width() + kTemporalRefreshEdge - 1u) / kTemporalRefreshEdge) *
                                          ((source.height() + kTemporalRefreshEdge - 1u) / kTemporalRefreshEdge);
                        state.m_temporal_refresh_cursor = (state.m_temporal_refresh_cursor + 1u) % tiles;
                        if (state.m_temporal_refresh_cursor == 0u)
                            state.m_temporal_refresh_started = elapsed;
                    }
                    state.m_temporal_refresh_next = not state.m_temporal_refresh_pending;
                    state.m_temporal_refresh_pending = false;
                    state.m_loading = state.m_window_update = false;
                    state.m_source_request_elapsed = state.m_candidate_request_elapsed;
                    state.m_publication_elapsed = elapsed;
                    state.m_source_first_presented = false;
                    if (const auto identity = state.m_source->source()->matterIdentity()) {
                        const auto statistics = state.m_source->source()->matterStatistics();
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            Editor,
                            statistics ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
                        const bool new_epoch = state.m_matter_frozen == identity;
                        const auto requested = new_epoch                ? state.m_matter_frozen_elapsed
                                               : state.m_matter_receipt ? state.m_matter_receipt->m_request_elapsed
                                                                        : elapsed;
                        const auto projected = new_epoch ? state.m_matter_projected_elapsed
                                               : state.m_matter_receipt
                                                   ? state.m_matter_receipt->m_projection_ready_elapsed
                                                   : elapsed;
                        const lighting::WorldRadianceEmissionAggregate aggregate{
                            statistics->m_overlay_cells, statistics->m_emission_maximum, statistics->m_emission_bound};
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            Editor, lighting::admitWorldRadianceExposure(
                                        aggregate, std::exp2(double(m_retro_settings.m_exposure_ev))));
                        state.m_matter_receipt = WorldMatterLightingReceipt{
                            *identity, state.m_source->source()->matterComparedIdentity(), *statistics,
                            state.m_source->source()->matterCompletionFence(), state.m_source->completionFence(),
                            requested, projected, elapsed};
                    }
                    state.m_window_pending_elapsed.reset();
                } else if (state.m_pass.canEncodeNextBatch() and state.m_last_field_frame != actual_frame) {
                    state.m_last_field_frame = actual_frame;
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        Editor, submitTerrainLighting_(false, actual_frame, false, false, elapsed));
                }
            }

            auto desired = lighting::planWorldRadianceReceiverWindow(*state.m_source->source(), camera, demands);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, desired);
            if (state.m_gameplay_demand and desired->m_counts[0] == 0u)
                *desired = state.m_gameplay_demand->m_window;

            const bool outside_window = not state.m_source->currentReceiverWindow().contains(*desired);
            const bool sky_changed = state.m_source->skyHour() != state.m_temporal_settings.m_sky_hour;
            const bool coverage_missing = outside_window or sky_changed;

            // A frozen Matter epoch can present its certified spatial window
            // while a newer sky request is pending. Give its queued successor
            // the next admission so one field incorporates both input changes.
            // A moved camera still needs spatial coverage before presentation.
            const bool combine_sky_with_matter = sky_changed and
                                                 not outside_window and
                                                 state.m_matter_frozen and
                                                 state.m_matter_after_frozen_elapsed;
            if (not state.m_window_update and
                not combine_sky_with_matter and
                (state.m_source_first_presented or coverage_missing) and
                not state.m_matter_prepared) {
                lighting::WorldRadianceSettings settings{
                    4ull * 1024u * 1024u * 1024u, 1024ull * 1024u * 1024u, 512ull * 1024u * 1024u, 0u, 0.5f, false,
                    m_measure_world_radiance, lighting::WorldRadianceRefinedSpacing::native_cell_centres,
                    lighting::WorldRadianceTransport::temporal_cascades, 65504.0f};
                const bool empty_demand = desired->m_counts[0] == 0u or desired->m_counts[1] == 0u;
                const bool refresh = empty_demand or (not state.m_gameplay_demand and
                                                         not coverage_missing and
                                                         state.m_temporal_refresh_next);
                settings.m_receiver_window = refresh ? temporalRefreshWindow_(
                                                           *state.m_source->source(), state.m_temporal_refresh_cursor)
                                                     : *desired;
                settings.m_temporal_coverage_window = empty_demand ? settings.m_receiver_window : *desired;
                settings.m_gameplay_demand = state.m_gameplay_demand;
                settings.m_prioritize_convergence = not refresh and not coverage_missing;
                settings.m_reserve_bytes += GridPass::kMaterialTimingReserve;
                settings.m_temporal = state.m_temporal_settings;
                settings.m_measure_gpu = true;
                settings.m_temporal_frame_allowance_ms = state.m_temporal_budget_milliseconds;
                settings.m_temporal_skip_table = state.m_temporal_skip_table;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Editor,
                    elapsed >= TimeSpan{} ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
                settings.m_temporal_sample_time_ns = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
                const auto admitted = state.m_pass.prepare(
                    state.m_source->source(), state.m_source->cohort(), settings, getRenderer().completedFenceValue());
                if (not admitted and admitted.error() == std::errc::device_or_resource_busy) {
                    // Presentation leases still own all three slots. Keep drawing
                    // the completed field until ordinary fence retirement frees one.
                } else {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, admitted);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, admitted->m_admission_error);
                    state.m_temporal_refresh_pending = refresh;
                    if (m_measure_world_radiance) {
                        PPR_LOG(
                            Editor, info, "WORLD_TEMPORAL_REGION_ADMITTED",
                            {{"frame", actual_frame}, {"refresh", refresh},
                                {"refresh_tile", state.m_temporal_refresh_cursor},
                                {"refresh_round_age_ms",
                                    time::seconds(elapsed - state.m_temporal_refresh_started) * 1000.0},
                                {"min_x", settings.m_receiver_window.m_minimum[0]},
                                {"min_y", settings.m_receiver_window.m_minimum[1]},
                                {"count_x", settings.m_receiver_window.m_counts[0]},
                                {"count_y", settings.m_receiver_window.m_counts[1]}});
                    }
                    state.m_candidate_cohort = state.m_source->cohort();
                    state.m_candidate_camera = camera;
                    state.m_candidate_extent = extent;
                    state.m_candidate_request_elapsed = state.m_source_first_presented ? elapsed
                                                                                       : state.m_source_request_elapsed;
                    state.m_candidate_source.reset();
                    state.m_loading = state.m_window_update = true;
                    state.m_stage = TerrainLightingState::Stage::field;
                }
            }

            state.m_view_camera = camera;
            state.m_view_extent = extent;
            state.m_view_request_elapsed = elapsed;
            state.m_retainable_failure = false;
            return not state.m_matter_frozen or state.m_source->source()->matterIdentity() == state.m_matter_frozen;
        }

        if (state.m_direct_window) {
            if (state.m_loading and not state.m_window_update)
                return false; // Only the new world candidate may use the pass while it loads.

            if (not state.m_loading and
                not state.m_window_update and
                state.m_stage != TerrainLightingState::Stage::idle) {
                const auto retired = advanceTerrainLighting_();
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
                if (state.m_stage != TerrainLightingState::Stage::idle)
                    return false;
            }

            if (state.m_window_update) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, state.m_error);
                const auto ready = advanceTerrainLighting_();
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, ready);
                if (not *ready) {
                    // Completion may have made the next strip ready after the
                    // frame's early submission check. Spend only unused credit.
                    if (state.m_pass.canEncodeNextBatch() and state.m_last_field_frame != actual_frame) {
                        state.m_last_field_frame = actual_frame;
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            Editor, submitTerrainLighting_(false, actual_frame, false, false, elapsed));
                    }
                    return false;
                }

                state.m_source = std::move(state.m_candidate_source);
                state.m_loading = state.m_window_update = false;
                state.m_source_request_elapsed = state.m_candidate_request_elapsed;
                state.m_publication_elapsed = elapsed;
                state.m_source_first_presented = false;
            }

            const auto source = state.m_source->source();
            const auto desired = lighting::planWorldRadianceReceiverWindow(*source, camera, demands);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, desired);
            if (state.m_discovery_enabled and (desired->m_counts[0] == 0u or desired->m_counts[1] == 0u)) {
                state.m_retainable_failure = false;
                PPR_LOG(
                    Editor, error,
                    "Discovery measurement requires a nonempty receiver window; exterior without demands is unsupported");
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, std::errc::not_supported);
            }

            const auto minimum = state.m_source->probeMinimum();
            const auto counts = state.m_source->probeCounts();
            bool covered = true;
            if (desired->m_counts[0] != 0u) {
                for (u32 axis = 0u; axis < 2u; ++axis) {
                    covered = covered and
                              desired->m_minimum[axis] >= minimum[axis] and
                              u64{static_cast<u32>(desired->m_minimum[axis])} + desired->m_counts[axis] <=
                                  u64{static_cast<u32>(minimum[axis])} + counts[axis];
                }
            }

            // The direct generation fixes the single-bounce query geometry at
            // field admission. Camera requests coalesce while ALL of those
            // admitted edges finish; no newer view cancels discovery or bounce.
            const lighting::WorldRadianceReceiverWindow admitted_window{minimum, counts};
            const auto query_window = state.m_bounce_enabled ? admitted_window : *desired;

            // A finished field need not have been presented to admit an
            // uncovered coalesced camera window. Its receipt stays attached to
            // the completed combined field until the replacement completes.
            const auto donor = state.m_discovery_donor.lock();
            const bool matching_result = state.m_discovery_result and
                                         state.m_discovery_result->source().get() == source.get() and
                                         state.m_discovery_result->cohort() == state.m_source->cohort() and
                                         state.m_discovery_result->window().m_minimum == query_window.m_minimum and
                                         state.m_discovery_result->window().m_counts == query_window.m_counts;
            const bool restore_donor = state.m_discovery_enabled and
                                       not matching_result and
                                       (not donor or
                                           donor->source().get() != source.get() or
                                           donor->cohort() != state.m_source->cohort());

            // Canceled replacement donors still restore through ordinary direct
            // publication before any old-source discovery is admitted.
            if (state.m_bounce_enabled and not restore_donor) {
                const auto discovered = updateTerrainDiscovery_(
                    state.m_candidate_camera, elapsed, query_window, actual_frame);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, discovered);
                if (not *discovered)
                    return false;

                const auto bounced = updateTerrainBounce_(elapsed, query_window, actual_frame);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, bounced);
                if (not *bounced)
                    return false;
            }

            if (not covered or restore_donor) {
                if (not state.m_window_pending_elapsed)
                    state.m_window_pending_elapsed = elapsed;

                const auto retired = cancelTerrainDiscovery_();
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, retired);
                if (not *retired)
                    return false;

                const auto cohort = state.m_source->cohort();
                const bool rejected = state.m_rejected_window and
                                      state.m_rejected_window->m_minimum == desired->m_minimum and
                                      state.m_rejected_window->m_counts == desired->m_counts and
                                      state.m_rejected_cohort.m_job_id == cohort.m_job_id and
                                      state.m_rejected_cohort.m_world_generation == cohort.m_world_generation and
                                      state.m_rejected_cohort.m_content_fingerprint == cohort.m_content_fingerprint and
                                      state.m_rejected_cohort.m_geometry_revision == cohort.m_geometry_revision;
                if (rejected) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, state.m_error);
                }

                // A safe rejection belongs only to its exact demand/cohort.
                // A distinct request can replace the proven-completed batch;
                // unknown submission was rejected before reaching this branch.
                if (state.m_rejected_window)
                    state.m_window_pending_elapsed = elapsed;

                state.m_error.clear();
                state.m_rejected_window.reset();

                // Preserve the actual oldest unsatisfied receiver request across
                // admission failures and repeated polling at the same camera.
                if (not state.m_window_pending_elapsed)
                    state.m_window_pending_elapsed = elapsed;

                lighting::WorldRadianceSettings settings{
                    4ull * 1024u * 1024u * 1024u, 1024ull * 1024u * 1024u, 512ull * 1024u * 1024u, 0u, .5f, false,
                    m_measure_world_radiance, lighting::WorldRadianceRefinedSpacing::native_cell_centres,
                    lighting::WorldRadianceTransport::direct_window, 65504.0f, *desired};
                settings.m_receiver_rows_per_quantum = state.m_bounce_enabled ? 256u : 512u;
                settings.m_diagnostic_coefficients = surfaceReadbackEnabled() and
                                                     m_measure_world_radiance and
                                                     demands.size() == 3u;
                const auto admitted = state.m_pass.prepare(
                    source, state.m_source->cohort(), settings, getRenderer().completedFenceValue());
                const auto error = admitted ? admitted->m_admission_error : admitted.error();
                state.m_retainable_failure = retainableTerrainViewError_(error);
                if (error and state.m_retainable_failure) {
                    state.m_error = error;
                    state.m_rejected_window = *desired;
                    state.m_rejected_cohort = cohort;
                }

                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, error);

                state.m_discovery_donor.reset();

                state.m_candidate_cohort = cohort;
                state.m_attempted_window = *desired;
                state.m_candidate_camera = camera;
                state.m_candidate_extent = extent;
                state.m_candidate_request_elapsed = elapsed;
                state.m_candidate_source.reset();
                state.m_loading = state.m_window_update = true;
                state.m_submission_pending = false;
                state.m_stage = TerrainLightingState::Stage::field;
                if (m_measure_world_radiance) {
                    PPR_LOG(
                        Editor, info, "Direct-window receiver demand admitted",
                        {{"frame_schedule", "one page per actual render request"}, {"minimum_x", desired->m_minimum[0]},
                            {"minimum_y", desired->m_minimum[1]}, {"count_x", desired->m_counts[0]},
                            {"count_y", desired->m_counts[1]}, {"pages", state.m_pass.pendingBatches()},
                            {"peak_bytes", admitted->m_peak_bytes}, {"queries_bound", admitted->m_query_upper_bound},
                            {"restoring_retained_source_donor", restore_donor}});
                }
                return false;
            }

            if (not state.m_bounce_enabled) {
                const auto discovered = updateTerrainDiscovery_(camera, elapsed, query_window, actual_frame);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Editor, discovered);
                if (not *discovered)
                    return false;
            }

            state.m_window_pending_elapsed.reset();
            state.m_rejected_window.reset();
            state.m_error.clear();
        }

        // The completed world field is camera-independent. Pan/zoom only replace
        // immutable draw constants; no exact-view query, allocation or submission.
        state.m_view_camera = camera;
        state.m_view_extent = extent;
        state.m_view_request_elapsed = elapsed;
        state.m_retainable_failure = false;
        return true;
    }

    std::error_code ApplicationEditor::releaseTerrainLighting_(const GpuQuiescence quiescence) {
        if (not m_terrain_lighting) {
            return {};
        }
        std::error_code error{};
        if (quiescence == GpuQuiescence::kIdleProven)
            PPR_RETAIN_ERROR_ON_FAIL(Editor, error, observeScreenLighting_());

        const auto release = m_terrain_lighting->m_pass.shutdown({quiescence, {}});
        PPR_RETAIN_ERROR_ON_FAIL(Editor, error, release);
        if (not release)
            m_terrain_lighting.reset();

        return error;
    }

    std::error_code ApplicationEditor::executeGridWorld_(const GridWorldCommand &command, GridWorldResult &result) {
        PPR_RETURN_ERROR_ON_FAIL(
            Editor, not command.m_single_bounce or (command.m_kind == EGridWorldCommand::begin and
                                                       command.m_direct_window and
                                                       command.m_stage.m_world_radiance_source)
                        ? std::error_code{}
                        : std::make_error_code(std::errc::invalid_argument));

        PPR_RETURN_ERROR_ON_FAIL(
            Editor, m_lane_rhi_service.isValid() ? std::error_code{} : std::make_error_code(std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            Editor,
            not m_terrain_lighting or m_terrain_lighting->m_stage != TerrainLightingState::Stage::submission_unknown
                ? std::error_code{}
                : std::make_error_code(std::errc::state_not_recoverable));

        const auto completed = getRenderer().completedFenceValue();
        if (command.m_lighting_revision != 0u) {
            const bool begins = command.m_kind == EGridWorldCommand::begin;
            const bool matches =
                begins
                    ? command.m_stage.m_world_radiance_source and
                          command.m_stage.m_world_radiance_revision == command.m_lighting_revision
                    : m_terrain_lighting and
                          (m_terrain_lighting->m_loading or command.m_kind == EGridWorldCommand::cancel) and
                          m_terrain_lighting->m_candidate_cohort.m_geometry_revision == command.m_lighting_revision and
                          m_terrain_lighting->m_candidate_cohort.m_job_id == command.m_job_id and
                          m_terrain_lighting->m_candidate_cohort.m_world_generation == command.m_world_generation and
                          m_terrain_lighting->m_candidate_cohort.m_content_fingerprint == command.m_content_fingerprint;
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, matches ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));
        }

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
                    Editor, (command.m_stage.m_uploads.empty() or (command.m_stage.m_world_radiance_source and
                                                                      command.m_final_batch and
                                                                      command.m_stage.m_full_refresh and
                                                                      command.m_stage.m_reset_caches)) and
                                    command.m_stage.m_tiles.size() <= GridPass::kGridTileCacheCapacity and
                                    command.m_content_fingerprint == command.m_stage.m_content_fingerprint and
                                    (not command.m_final_batch or
                                        command.m_stage.m_tiles.empty() or
                                        command.m_stage.m_world_radiance_source)
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));
                if (not m_grid_pass->worldCandidateAdmissionReady(completed)) {
                    // The previous world is still retiring; retain the command and retry.
                    return std::make_error_code(std::errc::device_or_resource_busy);
                }

                if (m_terrain_lighting) {
                    const auto retired = cancelTerrainDiscovery_();
                    PPR_RETURN_ERROR_ON_FAIL(Editor, retired);
                    PPR_RETURN_ERROR_ON_FAIL(
                        Editor,
                        *retired ? std::error_code{} : std::make_error_code(std::errc::device_or_resource_busy));
                }

                if (m_terrain_lighting and m_terrain_lighting->m_stage != TerrainLightingState::Stage::idle) {
                    auto &state = *m_terrain_lighting;
                    const bool supersedes_window = command.m_lighting_revision != 0u and
                                                   state.m_temporal_cascades and
                                                   state.m_window_update and
                                                   state.m_stage == TerrainLightingState::Stage::field and
                                                   state.m_candidate_cohort.m_job_id == command.m_job_id and
                                                   state.m_candidate_cohort.m_world_generation ==
                                                       command.m_world_generation and
                                                   state.m_candidate_cohort.m_content_fingerprint ==
                                                       command.m_content_fingerprint and
                                                   state.m_candidate_cohort.m_geometry_revision <
                                                       command.m_lighting_revision;
                    if (supersedes_window) {
                        // Stop at the accepted page fence. prepare() replaces the unfinished
                        // batch; the displayed generation and its presentation leases survive.
                        if (not slotFenceComplete(completed, state.m_submission))
                            return std::make_error_code(std::errc::device_or_resource_busy);

                        // Use the normal retirement path to collect the accepted page's timing.
                        state.m_loading = false;
                        const auto retired = advanceTerrainLighting_();
                        state.m_loading = state.m_stage != TerrainLightingState::Stage::idle;
                        PPR_RETURN_ERROR_ON_FAIL(Editor, retired);
                        if (state.m_loading)
                            return std::make_error_code(std::errc::device_or_resource_busy);

                        state.m_window_update = false;
                        state.m_temporal_refresh_pending = false;
                        state.m_submission_pending = false;
                        state.m_matter_frozen.reset();
                        state.m_matter_after_frozen_elapsed.reset();
                    } else {
                        const auto ready = advanceTerrainLighting_();
                        PPR_RETURN_ERROR_ON_FAIL(Editor, ready);
                        if (state.m_stage != TerrainLightingState::Stage::idle)
                            return std::make_error_code(std::errc::device_or_resource_busy);
                    }
                }

                observeTerrainPublicationPhase_(
                    m_measure_world_radiance and bool(command.m_stage.m_world_radiance_source),
                    "grid-candidate.stage.begin",
                    {command.m_job_id, command.m_world_generation, command.m_content_fingerprint,
                        command.m_stage.m_world_radiance_revision},
                    command.m_sequence);

                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, m_grid_pass->stageWorldCandidate(
                                command.m_job_id, command.m_world_generation, command.m_stage, command.m_final_batch));

                observeTerrainPublicationPhase_(
                    m_measure_world_radiance and bool(command.m_stage.m_world_radiance_source),
                    "grid-candidate.stage.end",
                    {command.m_job_id, command.m_world_generation, command.m_content_fingerprint,
                        command.m_stage.m_world_radiance_revision},
                    command.m_sequence);

                if (command.m_stage.m_world_radiance_source) {
                    if (command.m_lighting_revision != 0u) {
                        // Staging has effects even if subsequent field prepare
                        // fails. Preserve its outer identity for fenced cancel.
                        if (not m_terrain_lighting) {
                            m_terrain_lighting = std::make_unique<TerrainLightingState>();
                        }

                        m_terrain_lighting->m_candidate_cohort = {
                            command.m_job_id, command.m_world_generation, command.m_content_fingerprint,
                            command.m_lighting_revision};
                        result.m_applied = true;
                    }

                    const auto error = beginTerrainLighting_(command);
                    if (error) {
                        m_grid_pass->cancelWorldCandidate(command.m_job_id);
                        if (m_terrain_lighting) {
                            m_terrain_lighting->m_loading = false;
                        }
                        PPR_RETURN_ERROR_ON_FAIL(Editor, error);
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
                if (m_terrain_lighting and m_terrain_lighting->m_loading) {
                    const auto &state = *m_terrain_lighting;
                    PPR_RETURN_ERROR_ON_FAIL(
                        Editor, state.m_candidate_source and
                                        state.m_candidate_cohort.m_job_id == command.m_job_id and
                                        state.m_candidate_cohort.m_world_generation == command.m_world_generation and
                                        state.m_candidate_cohort.m_content_fingerprint == command.m_content_fingerprint
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::operation_not_permitted));
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, m_grid_pass->activateWorldCandidate(
                                command.m_job_id, command.m_world_generation, command.m_content_fingerprint));
                if (m_terrain_lighting and m_terrain_lighting->m_loading) {
                    auto &state = *m_terrain_lighting;
                    state.m_source = std::move(state.m_candidate_source);
                    state.m_terrain_source = std::move(state.m_candidate_terrain_source);
                    state.m_bound_terrain_source.reset();
                    state.m_projected_matter_source.reset();
                    state.m_bound_matter_terrain_revision = 0u;
                    state.m_matter_latest.reset();
                    state.m_matter_frozen.reset();
                    state.m_matter_receipt.reset();
                    state.m_matter_pending_elapsed.reset();
                    state.m_matter_after_frozen_elapsed.reset();
                    state.m_matter_presented_once = false;
                    state.m_matter_service_times = {};
                    state.m_matter_refresh_times = {};
                    state.m_matter_service_next = 0u;
                    state.m_view_camera = state.m_candidate_camera;
                    state.m_view_extent = state.m_candidate_extent;
                    state.m_view_request_elapsed = state.m_candidate_request_elapsed;
                    state.m_source_request_elapsed = state.m_candidate_request_elapsed;
                    state.m_publication_elapsed = getTimerManager().getTotalElapsed();
                    state.m_last_age_report_elapsed = state.m_publication_elapsed;
                    state.m_source_first_presented = false;
                    state.m_loading = false;

                    if (m_measure_world_radiance) {
                        PPR_LOG(
                            Editor, info, "World radiance source activated",
                            {{"world", state.m_source->cohort().m_world_generation},
                                {"revision", state.m_source->cohort().m_geometry_revision},
                                {"field_begin_to_publication_ms",
                                    time::seconds(state.m_publication_elapsed - state.m_source_request_elapsed) *
                                        1000.0},
                                {"clock", "sampled injected Application elapsed"},
                                {"original_user_edit_latency", "unmeasured"}});
                    }
                }

                result.m_applied = true;
                return {};
            case EGridWorldCommand::cancel: {
                if (command.m_lighting_revision != 0u) {
                    // The outer binding remains alive until every operation of
                    // this canceled attempt is settled, not merely deselected.
                    // Repeated polls use the existing control slot/result flag.
                    if (m_grid_pass->worldCandidateMatches(
                            command.m_job_id, command.m_world_generation, command.m_content_fingerprint)) {
                        m_grid_pass->cancelWorldCandidate(command.m_job_id);
                    }

                    auto &state = *m_terrain_lighting;
                    state.m_loading = false;
                    if (state.m_submission_pending) {
                        // The existing pre-encode rejection proof means no GPU
                        // work owns this prepared retry.
                        state.m_submission_pending = false;
                        state.m_stage = TerrainLightingState::Stage::idle;
                    }

                    if (state.m_stage != TerrainLightingState::Stage::idle) {
                        const auto retired = advanceTerrainLighting_();
                        PPR_RETURN_ERROR_ON_FAIL(Editor, retired);
                    }

                    const bool idle = state.m_stage == TerrainLightingState::Stage::idle;
                    if (idle) {
                        state.m_candidate_source.reset();
                        state.m_error.clear();
                        state.m_retainable_failure = false;
                    }

                    result.m_applied = true;
                    result.m_ready = idle and
                                     m_grid_pass->worldCandidateAdmissionReady(getRenderer().completedFenceValue());
                    return {};
                }

                if (not m_grid_pass->worldCandidateMatches(
                        command.m_job_id, command.m_world_generation, command.m_content_fingerprint)) {
                    result.m_applied = false;
                    return {};
                }

                m_grid_pass->cancelWorldCandidate(command.m_job_id);
                if (m_terrain_lighting and
                    m_terrain_lighting->m_loading and
                    m_terrain_lighting->m_candidate_cohort.m_job_id == command.m_job_id and
                    m_terrain_lighting->m_candidate_cohort.m_world_generation == command.m_world_generation and
                    m_terrain_lighting->m_candidate_cohort.m_content_fingerprint == command.m_content_fingerprint) {
                    auto &state = *m_terrain_lighting;
                    const bool candidate_work = state.m_stage == TerrainLightingState::Stage::field;
                    state.m_loading = false;
                    if (candidate_work) {
                        state.m_error.clear();
                        state.m_retainable_failure = false;
                    }

                    if (candidate_work and state.m_submission_pending) {
                        // Prepared only: Renderer rejected before encode, so no
                        // GPU completion is needed to cancel this retry.
                        state.m_submission_pending = false;
                        state.m_stage = TerrainLightingState::Stage::idle;
                    }

                    if (state.m_stage == TerrainLightingState::Stage::idle) {
                        state.m_candidate_source.reset();
                    }
                }

                result.m_applied = true;
                return {};
            }
            default: PPR_RETURN_ERROR_ON_FAIL(Editor, std::errc::invalid_argument);
        }

        if (m_measure_world_radiance and
            m_terrain_lighting and
            m_terrain_lighting->m_loading and
            not m_terrain_lighting->m_grid_upload_begin_observed) {
            observeTerrainPublicationPhase_(
                true, "grid-candidate.uploads.begin", m_terrain_lighting->m_candidate_cohort, command.m_sequence);
            m_terrain_lighting->m_grid_upload_begin_observed = true;
        }

        const auto ready = m_grid_pass->advanceWorldCandidate(m_lane_rhi_service->getDevice(), completed);
        PPR_RETURN_ERROR_ON_FAIL(Editor, ready);

        if (*ready and
            m_measure_world_radiance and
            m_terrain_lighting and
            m_terrain_lighting->m_loading and
            not m_terrain_lighting->m_grid_upload_end_observed) {
            observeTerrainPublicationPhase_(
                true, "grid-candidate.uploads.end", m_terrain_lighting->m_candidate_cohort, command.m_sequence);
            m_terrain_lighting->m_grid_upload_end_observed = true;
        }

        result.m_applied = true;
        result.m_ready = *ready;
        if (result.m_ready and m_terrain_lighting and m_terrain_lighting->m_loading) {
            const auto field_ready = advanceTerrainLighting_();
            PPR_RETURN_ERROR_ON_FAIL(Editor, field_ready);
            result.m_ready = *field_ready and bool(m_terrain_lighting->m_candidate_source);
        }
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
        observeTerrainLighting_();
        PPR_RETURN_ERROR_ON_FAIL(Editor, observeScreenLighting_());

        PPR_DEFER {
            out_result.m_gpu_timing = renderer.lastGpuFrameTiming();
            if (request.m_observe_grid and m_grid_pass) {
                out_result.m_grid_observations = m_grid_pass->observations();
            }
        };
        const FramePacket &packet = *request.m_packet;
        bool encoded_screen_work{};
        PPR_DEFER {
            // Producer and surface submissions share a frame ID. A retained or
            // skipped scene can advance lighting without encoding Grid work.
            if (m_measure_world_radiance and
                not encoded_screen_work and
                m_terrain_lighting and
                m_terrain_lighting->m_temporal_cascades and
                m_terrain_lighting->m_last_field_frame == packet.m_frame_id) {
                PPR_LOG(Editor, info, "WORLD_TEMPORAL_NO_SCREEN_WORK", {{"frame", packet.m_frame_id}});
            }
        };

        const DetachedUiDrawData &ui = request.m_ui;
        const GridPass::GridStagedFrame &grid_stage = request.m_grid_stage;
        const TrianglePass::TriangleStagedFrame &triangle_stage = request.m_triangle_stage;
        const bool native_samples = request.m_retro.m_spatial_enabled or request.m_retro.m_temporal_2x_enabled;
        if (m_terrain_lighting and m_terrain_lighting->m_temporal_cascades and m_terrain_lighting->m_source) {
            auto &state = *m_terrain_lighting;
            state.m_material_timing.updateView(
                state.m_source->cohort(), packet.m_camera, packet.m_framebuffer_size,
                request.m_retro.m_temporal_2x_enabled);
        }

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
        // publication. Stale draws are rejected below; independent accepted
        // lighting phases may still settle before that rejection.
        const bool particles_current = m_grid_pass->gpuParticleFrameCurrent(grid_stage.m_gpu_particles) and
                                       (not m_terrain_lighting or
                                           not m_terrain_lighting->m_bounce_enabled or
                                           grid_stage.m_gpu_particles.has_value());

        if (m_terrain_lighting and m_terrain_lighting->m_direct_window) {
            auto &state = *m_terrain_lighting;
            if (state.m_stage == TerrainLightingState::Stage::field and
                state.m_loading and
                state.m_pass.canEncodeNextBatch() and
                state.m_last_field_frame != packet.m_frame_id) {
                state.m_last_field_frame = packet.m_frame_id;
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, submitTerrainLighting_(false, packet.m_frame_id, false, false, packet.m_total_elapsed));
            }
        }

        const int2 retro_extent = native_samples
                                      ? int2{packet.m_framebuffer_size.x / 2, packet.m_framebuffer_size.y / 2}
                                  : request.m_retro.m_enabled
                                      ? RetroPass::sceneExtent(packet.m_framebuffer_size, request.m_retro.m_pixel_scale)
                                      : packet.m_framebuffer_size;
        CameraSnapshot raster_camera = packet.m_camera;
        CameraSnapshot scene_camera = packet.m_camera;
        const lighting::WorldRadianceGeneration *selected_lighting{};
        const lighting::WorldRadianceGeneration *bound_lighting{};
        RetroLightingPublication frame_lighting_publication{};
        bool retain_picture{}, completed_fallback{};
        std::optional<RetroCompletedPicture> retained_picture{};
        const auto world_identity = m_grid_pass->activeWorldIdentity();
        const bool lighting = m_terrain_lighting and
                              m_terrain_lighting->m_source and
                              m_terrain_lighting->m_source->cohort().m_job_id == world_identity.m_job_id and
                              m_terrain_lighting->m_source->cohort().m_world_generation ==
                                  world_identity.m_world_generation and
                              m_terrain_lighting->m_source->cohort().m_content_fingerprint ==
                                  world_identity.m_content_fingerprint;
        // The published temporal field owns its viewport correction during
        // activation as well as after the game attaches its first Matter view.
        const bool screen_contact = lighting and
                                    (m_terrain_lighting->m_temporal_cascades or grid_stage.m_screen_contact_pilot);
        const bool odd_spatial_target = native_samples and
                                        (packet.m_framebuffer_size.x % 2 != 0 or packet.m_framebuffer_size.y % 2 != 0);
        if (odd_spatial_target and
            lighting and
            grid_stage.m_retain_content and
            grid_stage.m_update_actors_on_retained_terrain) {
            retained_picture = m_retro_pass->restoreCompletedPicture(renderer.completedFenceValue());
        }

        const auto picture_admission = admitPictureFrame(
            request.m_retro, packet.m_framebuffer_size,
            retained_picture ? std::optional<int2>{retained_picture->m_output_extent} : std::nullopt);
        PPR_RETURN_ERROR_ON_FAIL(Editor, picture_admission);
        const bool retained_only = *picture_admission;

        if (not lighting and request.m_gameplay_query and request.m_gameplay_query->m_field_world) {
            WorldRadianceGameplayQueryReceipt reply{};
            reply.m_sequence = request.m_gameplay_query->m_sequence;
            reply.m_error = std::make_error_code(std::errc::operation_canceled);
            out_result.m_gameplay_query = std::move(reply);
        }
        if (lighting) {
            auto &state = *m_terrain_lighting;
            if (state.m_temporal_cascades and grid_stage.m_temporal_sky_hour)
                state.m_temporal_settings.m_sky_hour = *grid_stage.m_temporal_sky_hour;

            state.m_gameplay_demand.reset();
            if (request.m_gameplay_query and
                request.m_gameplay_query->m_field_world and
                (request.m_gameplay_query->m_sequence == 0u or
                    request.m_gameplay_query->m_sequence != state.m_delivered_query)) {
                const auto &query = *request.m_gameplay_query;
                const auto &cohort = state.m_source->cohort();
                WorldRadianceGameplayQueryReceipt reply{};
                reply.m_sequence = query.m_sequence;
                reply.m_world = cohort.m_world_generation;
                reply.m_revision = cohort.m_geometry_revision;
                if (not state.m_temporal_cascades or query.m_sequence == 0u) {
                    reply.m_error = std::make_error_code(std::errc::invalid_argument);
                } else if (*query.m_field_world != world_identity or
                           query.m_world != reply.m_world or
                           query.m_revision != reply.m_revision) {
                    reply.m_error = std::make_error_code(std::errc::operation_canceled);
                } else {
                    const auto window = lighting::WorldRadianceGameplayQuery::windowForWorldPoint(
                        *state.m_source->source(), query.m_center_m, query.m_edge);
                    if (window) {
                        state.m_gameplay_demand = lighting::WorldRadianceGameplayDemand{
                            query.m_sequence, {window->m_minimum, window->m_counts}};
                    } else {
                        reply.m_error = window.error();
                    }
                }

                if (reply.m_error) {
                    out_result.m_gameplay_query = std::move(reply);
                    state.m_delivered_query = query.m_sequence;
                }
            }

            const bool committed_revision = request.m_expected_lighting_revision == 0u or
                                            request.m_expected_lighting_revision ==
                                                state.m_source->cohort().m_geometry_revision;
            Expected<bool> ready{false};
            // Activation reaches the render lane before the driver commits its
            // matching Matter view. Keep the old packet out of the new source.
            if (committed_revision) {
                ready = updateTerrainMatter_(
                    grid_stage.m_gpu_particles, packet.m_camera, retro_extent, packet.m_total_elapsed,
                    request.m_lighting_receiver_demands, packet.m_frame_id);
            }

            if (ready and *ready and not retained_only) {
                ready = updateTerrainLightingView_(
                    packet.m_camera, retro_extent, packet.m_total_elapsed, request.m_lighting_receiver_demands,
                    packet.m_frame_id);
            } else if (ready and retained_only) {
                ready = false;
            }
            if (not ready)
                state.m_retainable_failure = retainableTerrainViewError_(ready.error());
            if (not ready) {
                // Numeric/admission failure belongs to this lighting request.
                // Submission, terminal-device and unrelated pass errors propagate.
                PPR_RETURN_ERROR_ON_FAIL(Editor, state.m_retainable_failure ? std::error_code{} : ready.error());
                state.m_error = ready.error();
                out_result.m_lighting_error = ready.error();
            }

            if (ready and
                not *ready and
                not retained_only and
                committed_revision and
                state.m_temporal_cascades and
                state.m_source and
                slotFenceComplete(renderer.completedFenceValue(), state.m_source->completionFence())) {
                // Full-world support remains usable during a queued source epoch;
                // this draw does not acknowledge that epoch as fresh.
                completed_fallback = true;
            }

            if (ready and
                not *ready and
                not retained_only and
                committed_revision and
                (not state.m_loading or state.m_window_update) and
                state.m_bounce_enabled and
                state.m_combined_source and
                state.m_matter_receipt and
                state.m_terrain_source and
                state.m_combined_source->completionFence() == state.m_matter_receipt->m_field_completion and
                slotFenceComplete(renderer.completedFenceValue(), state.m_combined_source->completionFence()) and
                state.m_combined_source->source()->matterIdentity() ==
                    std::optional{state.m_matter_receipt->m_identity} and
                state.m_combined_source->source()->matchesTerrainSource(*state.m_terrain_source) and
                state.m_combined_source->cohort() == state.m_source->cohort()) {
                // Pending work owns separate pass resources. The old immutable
                // combined field and its receipt can still produce a fresh
                // frame, but only with independently proven current coverage.
                const auto &complete = *state.m_combined_source;
                const auto desired = lighting::planWorldRadianceReceiverWindow(
                    *complete.source(), packet.m_camera, request.m_lighting_receiver_demands);
                PPR_RETURN_ERROR_ON_FAIL(Editor, desired);

                completed_fallback = true;
                for (u32 axis = 0u; axis < 2u; ++axis) {
                    completed_fallback = completed_fallback and
                                         desired->m_minimum[axis] >= complete.probeMinimum()[axis] and
                                         u64{static_cast<u32>(desired->m_minimum[axis])} + desired->m_counts[axis] <=
                                             u64{static_cast<u32>(complete.probeMinimum()[axis])} +
                                                 complete.probeCounts()[axis];
                }
            }

            retain_picture = retained_only or
                             ((not ready or not *ready) and not completed_fallback) or
                             not committed_revision;
            bool latest_match{};
            std::optional<TimeSpan> displayed_request{};
            if (retain_picture) {
                if (not retained_picture) {
                    retained_picture = m_retro_pass->restoreCompletedPicture(renderer.completedFenceValue());
                }
                if (retained_picture) {
                    const auto &picture = *retained_picture;
                    // A retained picture has no receiver-coverage stamp. Even
                    // matching camera/cohort metadata cannot prove a newly
                    // requested offscreen page was part of that picture.
                    latest_match = not state.m_direct_window and
                                   picture.m_world_identity ==
                                       std::array<u64, 3>{
                                           world_identity.m_world_generation, world_identity.m_job_id,
                                           world_identity.m_content_fingerprint} and
                                   (request.m_expected_lighting_revision == 0u or
                                       picture.m_lighting_revision == request.m_expected_lighting_revision) and
                                   picture.m_output_extent.x == packet.m_framebuffer_size.x and
                                   picture.m_output_extent.y == packet.m_framebuffer_size.y and
                                   std::memcmp(
                                       &picture.m_camera.m_invert_view_projection,
                                       &packet.m_camera.m_invert_view_projection,
                                       sizeof(packet.m_camera.m_invert_view_projection)) == 0;
                    displayed_request = picture.m_request_elapsed;
                }
            } else {
                scene_camera = completed_fallback ? packet.m_camera : state.m_view_camera;
                const auto &selected = state.m_bounce_enabled ? state.m_combined_source : state.m_source;
                selected_lighting = selected.get(); // State owns this readiness candidate, not a binding proof.
                latest_match = not completed_fallback and
                               (not state.m_temporal_cascades or
                                   (selected and selected->skyHour() == state.m_temporal_settings.m_sky_hour));
                displayed_request = state.m_view_request_elapsed;
            }

            // A coherent intermediate picture is usable without satisfying the
            // latest request. Direct windows age from the first outstanding
            // coverage request, not from the old picture's matching camera.
            if (not latest_match and state.m_direct_window) {
                if (not state.m_window_pending_elapsed)
                    state.m_window_pending_elapsed = packet.m_total_elapsed;

                displayed_request = state.m_window_pending_elapsed;
                if (state.m_matter_pending_elapsed and *state.m_matter_pending_elapsed < *displayed_request)
                    displayed_request = state.m_matter_pending_elapsed;
            }

            out_result.m_lighting_provisional = state.m_bounce_enabled;
            out_result.m_matter_queued = state.m_matter_pending_elapsed.has_value();
            if (state.m_matter_pending_elapsed and packet.m_total_elapsed >= *state.m_matter_pending_elapsed)
                out_result.m_matter_pending_age = packet.m_total_elapsed - *state.m_matter_pending_elapsed;

            out_result.m_lighting_pending = not latest_match;
            out_result.m_lighting_update_age = not latest_match and
                                                       displayed_request and
                                                       packet.m_total_elapsed >= *displayed_request
                                                   ? packet.m_total_elapsed - *displayed_request
                                                   : TimeSpan{};
        }


        if (not particles_current) {
            out_result.m_dropped = true;
            return {}; // No stale Grid/actor/census draw; lighting fence progress above remains valid.
        }

        // P2 pass-affinity: ALL pass CPU-state mutations run here, on the
        // pass's owner lane (worker or main-inline), applied from the
        // owned payload captured in update(). The camera snapshot +
        // delta time come from the packet (immutable), the tile list and
        // upload cells ride the per-frame staged payload built by the
        // client — nothing live is read by either.
        if (not retain_picture) {
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
            // legacy view without requiring an intervening legacy frame.
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

            if (selected_lighting) {
                const auto &state = *m_terrain_lighting;
                const auto &selected = state.m_bounce_enabled ? state.m_combined_source : state.m_source;
                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, selected.get() == selected_lighting
                                ? std::error_code{}
                                : std::make_error_code(std::errc::state_not_recoverable));

                // Staging and cache publication can clear or change the resident
                // mapping. Validate and bind only after those operations finish.
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->setWorldRadianceLighting(selected));
                bound_lighting = selected.get(); // Grid/state retain this exact immutable binding.
            }

            if (m_retro_pass) {
                if (native_samples) {
                    PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->validateSpatialContent());
                }

                const auto identity = m_grid_pass->activeWorldIdentity();
                auto &publication = frame_lighting_publication;
                if (bound_lighting) {
                    const auto &cohort = bound_lighting->cohort();
                    PPR_RETURN_ERROR_ON_FAIL(
                        Editor, cohort.m_world_generation == identity.m_world_generation and
                                        cohort.m_job_id == identity.m_job_id and
                                        cohort.m_content_fingerprint == identity.m_content_fingerprint and
                                        (request.m_expected_lighting_revision == 0u or
                                            cohort.m_geometry_revision == request.m_expected_lighting_revision)
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::invalid_argument));

                    publication.m_completion = bound_lighting->completionFence();
                    publication.m_bounce_epoch = bound_lighting->bounceEpoch();
                    publication.m_probe_minimum = bound_lighting->probeMinimum();
                    publication.m_probe_counts = bound_lighting->probeCounts();
                }

                const auto raster = m_retro_pass->beginFrame(
                    request.m_retro, scene_camera, retro_extent,
                    native_samples ? packet.m_framebuffer_size : retro_extent, identity.m_world_generation,
                    identity.m_job_id, identity.m_content_fingerprint,
                    lighting ? m_terrain_lighting->m_view_request_elapsed : TimeSpan{},
                    bound_lighting ? bound_lighting->cohort().m_geometry_revision : 0u, publication);
                PPR_RETURN_ERROR_ON_FAIL(Editor, raster);

                raster_camera = *raster;
            }

            auto background = GridPass::BackgroundResponse::unlit;
            if (lighting) {
                background = GridPass::BackgroundResponse::lit_backwall;
                if (screen_contact) {
                    background = m_terrain_lighting->m_temporal_cascades
                                     ? GridPass::BackgroundResponse::screen_contact
                                     : GridPass::BackgroundResponse::screen_contact_pilot;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(Editor, m_triangle_pass->update(packet.m_delta_time, raster_camera));
            PPR_RETURN_ERROR_ON_FAIL(
                Editor, m_grid_pass->update(
                            packet.m_delta_time, raster_camera, native_samples, GridPass::ReceiverContinuation::nearest,
                            lighting ? GridPass::ReceiverResponse::mapped_diffuse : GridPass::ReceiverResponse::flat,
                            background));
        }
        const bool offscreen = request.m_retro.m_enabled or
                               request.m_retro.m_palette_enabled or
                               request.m_retro.m_hdr_enabled or
                               native_samples or
                               (m_retro_pass and m_retro_pass->temporalActive());
        out_result.m_grid_staged_tiles = m_grid_pass->stagedTileCount();
        out_result.m_grid_cached_tiles = m_grid_pass->cachedTileCount();

        std::optional<u32> screen_timing_slot;
        u32 screen_timing_mask{};
        u32 material_band{kNoMaterialBand};
        if (lighting and m_terrain_lighting->m_temporal_cascades and not retain_picture) {
            auto &state = *m_terrain_lighting;
            for (u32 slot = 0u; slot < state.m_screen_query_fences.size(); ++slot) {
                if (state.m_screen_query_fences[slot] != kFenceValueNone)
                    continue;

                PPR_RETURN_ERROR_ON_FAIL(
                    Editor, state.m_screen_queries->reset(kScreenTimingStride * slot, kScreenTimingStride));
                screen_timing_slot = slot;
                if (request.m_retro.m_temporal_2x_enabled and
                    GridPass::supportsMaterialTiming(safe_narrowing(retro_extent.x), safe_narrowing(retro_extent.y)) and
                    state.m_material_timing.due(packet.m_frame_id))
                    material_band = state.m_material_timing.m_next_band;

                break;
            }
        }

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
                if (screen_timing_slot)
                    context.m_pass.writeTimestamp(
                        m_terrain_lighting->m_screen_queries, kScreenTimingStride * *screen_timing_slot + 2u);
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderBackground(context));
                if (screen_timing_slot) {
                    context.m_pass.writeTimestamp(
                        m_terrain_lighting->m_screen_queries, kScreenTimingStride * *screen_timing_slot + 3u);
                    screen_timing_mask |= 2u;
                }

                if (screen_timing_slot)
                    context.m_pass.writeTimestamp(
                        m_terrain_lighting->m_screen_queries, kScreenTimingStride * *screen_timing_slot + 4u);
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderGpuIndirect(context));
                if (screen_timing_slot) {
                    context.m_pass.writeTimestamp(
                        m_terrain_lighting->m_screen_queries, kScreenTimingStride * *screen_timing_slot + 5u);
                    screen_timing_mask |= 4u;
                }

                if (screen_timing_slot)
                    context.m_pass.writeTimestamp(
                        m_terrain_lighting->m_screen_queries, kScreenTimingStride * *screen_timing_slot + 6u);
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->renderParticles(context));
                if (screen_timing_slot) {
                    context.m_pass.writeTimestamp(
                        m_terrain_lighting->m_screen_queries, kScreenTimingStride * *screen_timing_slot + 7u);
                    screen_timing_mask |= 8u;
                }
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

            if (request.m_retro.m_temporal_2x_enabled or
                (request.m_capture_final_frame and m_terrain_lighting and m_terrain_lighting->m_direct_window)) {
                captured_scene_signature = static_cast<const RenderPipelineSignature &>(context.m_render_pipeline_key);
                captured_scene_extent = context.m_target_extent;
            }

            return {};
        };

        const auto scene_draw = [&](const DrawContext &context) -> std::error_code {
            if (retain_picture) {
                return retained_picture ? m_retro_pass->render(context) : std::error_code{};
            }
            if (offscreen) {
                return m_retro_pass->render(context);
            }
            return draw_scene_contents(context);
        };

        // Policy branch: retro prepares the grid against the low-res extent
        // and hands the scene encode to the pass's offscreen scene pass;
        // native keeps the framebuffer viewport. Grid preparation is timed
        // separately from the scene encode, whose draw observations are
        // recorded in either mode.
        const bool temporal_world_capture = request.m_capture_final_frame and
                                            lighting and
                                            m_measure_world_radiance and
                                            (m_terrain_lighting->m_bounce_enabled or
                                                m_terrain_lighting->m_temporal_cascades) and
                                            request.m_retro.m_temporal_2x_enabled;
        bool capture_history_ready = not temporal_world_capture;
        if (temporal_world_capture) {
            // This value was latched only on a prior accepted fresh TSR
            // submission. A replacement's first raster frame cannot consume
            // the one-shot; the final frame must still prove actual history use.
            const auto prior = m_retro_pass->lastTemporal2xSubmission();
            capture_history_ready = bound_lighting and
                                    m_terrain_lighting->m_matter_receipt and
                                    bound_lighting->source()->matterIdentity() ==
                                        std::optional{m_terrain_lighting->m_matter_receipt->m_identity} and
                                    bound_lighting->completionFence() ==
                                        m_terrain_lighting->m_matter_receipt->m_field_completion and
                                    prior and
                                    prior->m_submission != kFenceValueNone and
                                    prior->m_lighting_publication == frame_lighting_publication and
                                    prior->m_raster_extent.x == retro_extent.x and
                                    prior->m_raster_extent.y == retro_extent.y and
                                    prior->m_output_extent.x == packet.m_framebuffer_size.x and
                                    prior->m_output_extent.y == packet.m_framebuffer_size.y;
        }

        const bool covered_pending_capture = temporal_world_capture and completed_fallback and capture_history_ready;
        const bool capture = request.m_capture_final_frame and
                             request.m_surface_capture_sequence > m_frame_capture_attempted_sequence and
                             not retain_picture and
                             capture_history_ready and
                             (not out_result.m_lighting_pending or covered_pending_capture) and
                             (not m_terrain_lighting or
                                 (lighting and
                                     (not m_terrain_lighting->m_loading or
                                         (covered_pending_capture and m_terrain_lighting->m_window_update)) and
                                     grid_stage.m_update_actors_on_retained_terrain));

        const auto prepare_background = [&](const PreRenderContext &context, const CameraSnapshot &camera,
                                            const rhi::Viewport viewport,
                                            const rhi::ScissorRect scissor) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->prepareBackground(context, camera, viewport, scissor));
            if (screen_timing_slot) {
                context.m_encoder.writeTimestamp(
                    m_terrain_lighting->m_screen_queries, kScreenTimingStride * *screen_timing_slot + 1u);
                screen_timing_mask |= 1u;
            }
            return {};
        };

        const auto scene_prepare = [&](const PreRenderContext &context) -> std::error_code {
            if (retain_picture) {
                return {}; // UI and the retained texture need no new scene encode.
            }

            encoded_screen_work = lighting and m_terrain_lighting->m_temporal_cascades;
            if (encoded_screen_work and m_measure_world_radiance and not screen_timing_slot) {
                PPR_LOG(Editor, info, "WORLD_SCREEN_TIMING_UNAVAILABLE", {{"frame", packet.m_frame_id}});
            }

            // Include the contact pilot's visibility/mask preparation in its bound.
            if (screen_timing_slot) {
                context.m_encoder.writeTimestamp(
                    m_terrain_lighting->m_screen_queries, kScreenTimingStride * *screen_timing_slot);
                screen_timing_mask |= 32u;
            }

            if (offscreen) {
                {
                    const CpuObservationScope cpu_prepare{clock, cpu.m_grid_prepare};
                    if (screen_contact) {
                        // The centre mask borrows this request's fenced resident
                        // atlas/lookup slot, before either backwall path reads it.
                        PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->prepareGpuVisibility(context));
                    }

                    PPR_RETURN_ERROR_ON_FAIL(
                        Editor, prepare_background(
                                    context, request.m_retro.m_temporal_2x_enabled ? raster_camera : scene_camera,
                                    retro_viewport, retro_scissor));

                    if (not screen_contact) {
                        PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->prepareGpuVisibility(context));
                    }

                    PPR_RETURN_ERROR_ON_FAIL(
                        Editor,
                        m_grid_pass->prepareGpuParticles(
                            context,
                            capture and m_measure_world_radiance ? grid_stage.m_capture_particle_census_edge : 0u));
                }

                if (material_band < kNoMaterialBand) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        Editor,
                        m_grid_pass->measureTerrainMaterial(
                            context, safe_narrowing(retro_extent.x), safe_narrowing(retro_extent.y), material_band,
                            *m_terrain_lighting->m_screen_queries, kScreenTimingStride * *screen_timing_slot + 8u));
                    screen_timing_mask |= 16u;
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
            if (screen_contact) {
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->prepareGpuVisibility(context));
            }

            PPR_RETURN_ERROR_ON_FAIL(Editor, prepare_background(context, scene_camera, grid_viewport, grid_scissor));

            if (not screen_contact) {
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_grid_pass->prepareGpuVisibility(context));
            }

            return m_grid_pass->prepareGpuParticles(
                context, capture and m_measure_world_radiance ? grid_stage.m_capture_particle_census_edge : 0u);
        };

        DrawSubmission scene_submission{"scene", scene_draw};
        if (retain_picture and retained_picture) {
            const auto extent = retained_picture->m_output_extent;
            scene_submission.m_viewport = rhi::Viewport::fromSize(float(extent.x), float(extent.y));
            scene_submission.m_scissor = rhi::ScissorRect::fromSize(
                safe_narrowing(std::min(extent.x, packet.m_framebuffer_size.x)),
                safe_narrowing(std::min(extent.y, packet.m_framebuffer_size.y)));
        }

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
            const std::optional<u32> *m_screen_timing_slot;
            const u32 *m_screen_timing_mask;
            const u32 *m_material_band;
            u64 m_frame_id;
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
                    if (*m_screen_timing_slot and *m_screen_timing_mask != 0u) {
                        auto &state = *m_self->m_terrain_lighting;
                        // Any accepted timestamp needs its fence, even if a later
                        // draw failed and left the measurement incomplete.
                        state.m_screen_query_complete[**m_screen_timing_slot] = (*m_screen_timing_mask & 15u) == 15u;
                        state.m_screen_query_fences[**m_screen_timing_slot] = after;
                        state.m_screen_query_frames[**m_screen_timing_slot] = m_frame_id;
                        state.m_screen_query_material_credit[**m_screen_timing_slot] = state.m_material_timing.credit(
                            *std::ranges::min_element(state.m_screen_terrain_ms));
                        const u32 band = (*m_screen_timing_mask & 16u) != 0u ? *m_material_band : kNoMaterialBand;
                        state.m_material_query_bands[**m_screen_timing_slot] = band;
                        state.m_material_query_epochs[**m_screen_timing_slot] = state.m_material_timing.m_epoch;
                        if (band < kNoMaterialBand and state.m_screen_query_complete[**m_screen_timing_slot]) {
                            state.m_material_timing.accept(band, m_frame_id);
                        }
                    }

                    // Stamping above latches Retro's actual accepted encoding.
                    // Historical receipts from retained/rejected draws cannot
                    // match this fresh successful scene and submission fence.
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
                    if (*m_screen_timing_slot and *m_screen_timing_mask != 0u)
                        m_self->m_terrain_lighting->m_screen_timing_unknown = true;
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
            &captured_scene_extent, &packet.m_framebuffer_size, &frame_lighting_publication, &screen_timing_slot,
            &screen_timing_mask, &material_band, packet.m_frame_id, request.m_retro.m_temporal_2x_enabled};

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
            if (retain_picture) {
                if (retained_picture) {
                    const auto &identity = retained_picture->m_world_identity;
                    out_result.m_presented_grid = {identity[1], identity[0], identity[2]};
                    out_result.m_lighting_camera = retained_picture->m_camera;
                    out_result.m_lighting_extent = retained_picture->m_output_extent;
                    out_result.m_lighting_target_extent = packet.m_framebuffer_size;
                    out_result.m_lighting_world = identity[0];
                    out_result.m_lighting_revision = retained_picture->m_lighting_revision;
                }
            } else {
                out_result.m_presented_grid = m_grid_pass->activeWorldIdentity();
                out_result.m_presented_particles = m_grid_pass->activeParticlePresentation();
                if (lighting) {
                    auto &state = *m_terrain_lighting;
                    bool current_coverage = true;
                    if (state.m_temporal_cascades) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            Editor, bound_lighting ? std::error_code{}
                                                   : std::make_error_code(std::errc::state_not_recoverable));
                        const auto desired = lighting::planWorldRadianceReceiverWindow(
                            *bound_lighting->source(), scene_camera, request.m_lighting_receiver_demands);
                        PPR_RETURN_ERROR_ON_FAIL(Editor, desired);
                        current_coverage = bound_lighting->currentReceiverWindow().contains(*desired);
                    }

                    out_result.m_lighting_provisional = state.m_bounce_enabled or not current_coverage;
                    if (bound_lighting and
                        current_coverage and
                        state.m_matter_receipt and
                        bound_lighting->completionFence() == state.m_matter_receipt->m_field_completion and
                        bound_lighting->source()->matterIdentity() ==
                            std::optional{state.m_matter_receipt->m_identity}) {
                        out_result.m_lighting_matter = state.m_matter_receipt;
                        out_result.m_lighting_provisional = false;

                        if (state.m_matter_frozen == std::optional{state.m_matter_receipt->m_identity}) {
                            const auto age = packet.m_total_elapsed - state.m_matter_frozen_elapsed;
                            const auto message = std::format(
                                "WORLD_MATTER_INCORPORATED frame={} present_fence={} source_fence={} field_fence={} pub={} joint={} device={} "
                                "changed_cells={} request_to_first_present_packet_ms={} cold={} request_age_1s={} queued_after_frozen={} "
                                "job={} world={} content={} terrain={} clock=explicit_frame_request_elapsed full_edit_age_UNMEASURED=true full_budget_qualified=false",
                                packet.m_frame_id, u64{out_result.m_submit_value},
                                u64{state.m_matter_receipt->m_source_completion},
                                u64{state.m_matter_receipt->m_field_completion},
                                state.m_matter_receipt->m_identity.m_particles.m_publication,
                                state.m_matter_receipt->m_identity.m_particles.m_joint_tick,
                                state.m_matter_receipt->m_identity.m_device_epoch,
                                state.m_matter_receipt->m_statistics.m_changed_cells, time::seconds(age) * 1000.0,
                                not state.m_matter_presented_once,
                                not state.m_matter_presented_once ? "COLD_REPORT_ONLY"
                                : age <= std::chrono::seconds{1}  ? "WITHIN_REQUEST_AGE_ONLY"
                                                                  : "NO_GO",
                                state.m_matter_after_frozen_elapsed.has_value(), world_identity.m_job_id,
                                world_identity.m_world_generation, world_identity.m_content_fingerprint,
                                state.m_matter_receipt->m_identity.m_particles.m_terrain_revision);
                            PPR_LOG_RAW(Editor, info, message);

                            // Only an actually displayed incorporated epoch moves
                            // the watermark. Keep the first later request's age.
                            state.m_matter_pending_elapsed = state.m_matter_after_frozen_elapsed;
                            state.m_matter_after_frozen_elapsed.reset();
                            state.m_matter_frozen.reset();
                            if (state.m_matter_presented_once) {
                                const auto duration = packet.m_total_elapsed - state.m_matter_admitted_elapsed;
                                state.m_matter_refresh_times[state.m_matter_service_next] =
                                    state.m_matter_includes_refresh ? duration : TimeSpan{};
                                state.m_matter_service_times[state.m_matter_service_next] =
                                    state.m_matter_includes_refresh ? TimeSpan{} : duration;
                                state.m_matter_service_next = (state.m_matter_service_next + 1u) %
                                                              state.m_matter_service_times.size();
                            }
                            state.m_matter_presented_once = true;
                        }
                    }

                    const bool current_source_presented =
                        bound_lighting and
                        current_coverage and
                        bound_lighting->source().get() == state.m_source->source().get() and
                        (not state.m_bounce_enabled or
                            (bound_lighting->probeMinimum() == state.m_source->probeMinimum() and
                                bound_lighting->probeCounts() == state.m_source->probeCounts()));
                    if (current_source_presented and not state.m_source_first_presented) {
                        PPR_LOG(
                            Editor, info, "World radiance first matching present result",
                            {{"world", state.m_source->cohort().m_world_generation},
                                {"revision", state.m_source->cohort().m_geometry_revision},
                                {"publication_to_first_present_packet_ms",
                                    time::seconds(packet.m_total_elapsed - state.m_publication_elapsed) * 1000.0},
                                {"physical_scanout", "unmeasured"}});
                    }

                    if (current_source_presented)
                        state.m_source_first_presented = true;

                    out_result.m_matter_queued = state.m_matter_pending_elapsed.has_value();
                    out_result.m_matter_pending_age = state.m_matter_pending_elapsed and
                                                              packet.m_total_elapsed >= *state.m_matter_pending_elapsed
                                                          ? packet.m_total_elapsed - *state.m_matter_pending_elapsed
                                                          : TimeSpan{};

                    // This fresh scene may use completed A while B is pending.
                    // Stamp the immutable field actually validated and bound,
                    // never a newer producer donor or discovery result.
                    PPR_RETURN_ERROR_ON_FAIL(
                        Editor,
                        bound_lighting ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));

                    out_result.m_lighting_sky_hour = bound_lighting->skyHour();
                    if (state.m_temporal_cascades and bound_lighting->skyHour() != state.m_temporal_settings.m_sky_hour)
                        out_result.m_lighting_provisional = true;

                    out_result.m_lighting_camera = scene_camera;
                    out_result.m_lighting_extent = packet.m_framebuffer_size;
                    out_result.m_lighting_target_extent = packet.m_framebuffer_size;
                    out_result.m_lighting_world = bound_lighting->cohort().m_world_generation;
                    out_result.m_lighting_revision = bound_lighting->cohort().m_geometry_revision;
                    out_result.m_lighting_receiver_window = state.m_temporal_cascades
                                                                ? bound_lighting->currentReceiverWindow()
                                                                : lighting::WorldRadianceReceiverWindow{
                                                                      bound_lighting->probeMinimum(),
                                                                      bound_lighting->probeCounts()};

                    if (m_terrain_lighting->m_direct_window and
                        out_result.m_surface_capture_attempted and
                        out_result.m_surface_pixels and
                        not out_result.m_surface_capture_error) {
                        const auto &view = grid_stage.m_gpu_particles;
                        const auto &presented = out_result.m_presented_particles;
                        const auto completed = renderer.completedFenceValue();
                        const auto capture_submission = out_result.m_surface_pixels->m_submission;
                        PPR_RETURN_ERROR_ON_FAIL(
                            Editor,
                            view and
                                    view->m_metadata and
                                    presented and
                                    presented->m_world == view->m_world and
                                    presented->m_publication == view->m_publication and
                                    presented->m_terrain_revision == view->m_terrain_revision and
                                    presented->m_joint_tick == view->m_joint_tick and
                                    presented->m_world == out_result.m_presented_grid and
                                    captured_scene_signature and
                                    captured_scene_extent.x > 0 and
                                    captured_scene_extent.y > 0 and
                                    captured_scene_extent.x == retro_extent.x and
                                    captured_scene_extent.y == retro_extent.y and
                                    native_samples and
                                    captured_scene_signature->m_sample_count == SampleCount::x4 and
                                    *captured_scene_signature == (request.m_retro.m_temporal_2x_enabled
                                                                         ? RetroPass::temporal2xScenePipelineSignature()
                                                                         : RetroPass::scenePipelineSignature(true)) and
                                    capture_submission != kFenceValueNone and
                                    capture_submission > previously_submitted and
                                    capture_submission == renderer.lastSubmittedFenceValue() and
                                    renderer.lastSubmitSettlement() == FrameSettlement::kSubmitted and
                                    slotFenceComplete(completed, capture_submission)
                                ? std::error_code{}
                                : std::make_error_code(std::errc::state_not_recoverable));

                        if (m_measure_world_radiance and grid_stage.m_capture_particle_census_edge != 0u) {
                            PPR_RETURN_ERROR_ON_FAIL(
                                Editor, capture_submission == out_result.m_submit_value
                                            ? std::error_code{}
                                            : std::make_error_code(std::errc::state_not_recoverable));
                            auto census = m_grid_pass->readParticleCensus(completed, capture_submission);
                            if (not census) {
                                out_result.m_surface_capture_error = census.error();
                                PPR_RETURN_ERROR_ON_FAIL(Editor, census.error());
                            }

                            out_result.m_particle_census = std::move(*census);
                        }

                        const auto proof = std::format(
                            "Direct-window encoded scene capture proof frame={} capture_sequence={} fence={} "
                            "job={} world={} content={} lighting_revision={} publication={} terrain_revision={} "
                            "joint_tick={} device_epoch={} particle_count={} moving_count={} "
                            "scene_extent=({},{}) scene_samples={} scene_color_format={} scene_color_count={} scene_reactive_format={} "
                            "output_extent=({},{}) pristine_direct_three_point={} source=same_encoded_request_and_successful_retro_scene_draw",
                            out_result.m_frame_id, out_result.m_surface_capture_sequence, u64{capture_submission},
                            presented->m_world.m_job_id, presented->m_world.m_world_generation,
                            presented->m_world.m_content_fingerprint, out_result.m_lighting_revision,
                            presented->m_publication, presented->m_terrain_revision, presented->m_joint_tick,
                            view->m_device_epoch, view->m_metadata->m_count, view->m_metadata->m_moving_count,
                            captured_scene_extent.x, captured_scene_extent.y,
                            static_cast<u32>(captured_scene_signature->m_sample_count),
                            rhi::getFormatInfo(captured_scene_signature->colorFormats().front()).name,
                            captured_scene_signature->colorFormats().size(),
                            rhi::getFormatInfo(
                                captured_scene_signature->colorFormats().size() > 1u
                                    ? captured_scene_signature->colorFormats()[1]
                                    : rhi::Format::Undefined)
                                .name,
                            out_result.m_surface_pixels->m_extent.x, out_result.m_surface_pixels->m_extent.y,
                            covered_pending_capture ? "OMITTED_COVERED_PENDING_UNMEASURED"
                                                    : "existing_nonpending_path");
                        PPR_LOG_RAW(Editor, info, proof);
                    }

                    if (m_measure_world_radiance) {
                        auto &capture_state = *m_terrain_lighting;
                        if (capture_state.m_direct_window and
                            out_result.m_surface_capture_attempted and
                            out_result.m_surface_pixels and
                            not out_result.m_surface_capture_error and
                            not covered_pending_capture and
                            request.m_lighting_receiver_demands.size() == 3u) {
                            // This fresh clean present owns CPU pixels already;
                            // the final producer-page fence must independently
                            // prove that its exact diagnostic mirror is ready.
                            const auto completed = renderer.completedFenceValue();
                            const auto capture_submission = out_result.m_surface_pixels->m_submission;
                            const auto cohort = capture_state.m_source->cohort();
                            PPR_RETURN_ERROR_ON_FAIL(
                                Editor,
                                capture_state.m_stage == TerrainLightingState::Stage::idle and
                                        not capture_state.m_loading and
                                        not capture_state.m_window_update and
                                        not capture_state.m_error and
                                        not out_result.m_lighting_pending and
                                        capture_submission != kFenceValueNone and
                                        capture_submission > previously_submitted and
                                        capture_submission == renderer.lastSubmittedFenceValue() and
                                        renderer.lastSubmitSettlement() == FrameSettlement::kSubmitted and
                                        slotFenceComplete(completed, capture_submission) and
                                        capture_state.m_submission != kFenceValueNone and
                                        slotFenceComplete(completed, capture_state.m_submission) and
                                        cohort.m_job_id == out_result.m_presented_grid.m_job_id and
                                        cohort.m_world_generation == out_result.m_presented_grid.m_world_generation and
                                        cohort.m_content_fingerprint ==
                                            out_result.m_presented_grid.m_content_fingerprint and
                                        cohort.m_geometry_revision == out_result.m_lighting_revision
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::state_not_recoverable));

                            const auto generation = capture_state.m_pass.acquire(completed);
                            PPR_RETURN_ERROR_ON_FAIL(Editor, generation);

                            PPR_RETURN_ERROR_ON_FAIL(
                                Editor, generation->get() == capture_state.m_source.get()
                                            ? std::error_code{}
                                            : std::make_error_code(std::errc::state_not_recoverable));

                            // The existing producer copies coefficients only on
                            // its FINAL page, with no new query/pipeline here.
                            const auto rows = capture_state.m_pass.readDiagnosticCoefficients(completed);
                            PPR_RETURN_ERROR_ON_FAIL(Editor, rows);

                            const auto minimum = (*generation)->probeMinimum();
                            const auto counts = (*generation)->probeCounts();
                            PPR_RETURN_ERROR_ON_FAIL(
                                Editor, rows->size() == u64{counts[0]} * counts[1]
                                            ? std::error_code{}
                                            : std::make_error_code(std::errc::invalid_argument));

                            for (u32 probe = 0u; probe < request.m_lighting_receiver_demands.size(); ++probe) {
                                const auto &demand = request.m_lighting_receiver_demands[probe];
                                PPR_RETURN_ERROR_ON_FAIL(
                                    Editor,
                                    demand.m_counts[0] == 1u and
                                            demand.m_counts[1] == 1u and
                                            demand.m_minimum[0] >= minimum[0] and
                                            demand.m_minimum[1] >= minimum[1] and
                                            u64{static_cast<u32>(demand.m_minimum[0] - minimum[0])} < counts[0] and
                                            u64{static_cast<u32>(demand.m_minimum[1] - minimum[1])} < counts[1]
                                        ? std::error_code{}
                                        : std::make_error_code(std::errc::invalid_argument));

                                const auto index = std::size_t{static_cast<u32>(demand.m_minimum[1] - minimum[1])} *
                                                       counts[0] +
                                                   static_cast<u32>(demand.m_minimum[0] - minimum[0]);
                                const auto &row = (*rows)[index];
                                const auto message = std::format(
                                    "Direct three-point actual probe={} cell=({},{}) DC=({},{},{}) status={} "
                                    "field_min=({},{}) field_counts=({},{}) direct_field_fence={} consumed_field_kind={} consumed_field_fence={} capture_fence={} frame={} "
                                    "capture_sequence={} job={} world={} revision={} content={} "
                                    "diagnostic_readback_bytes={} diagnostic_CPU_capacity_bytes={} "
                                    "last_page_GPU_timing_includes_diagnostic_copy=true full_budget_qualified=false",
                                    probe + 1u, demand.m_minimum[0], demand.m_minimum[1], row.m_coefficients[0],
                                    row.m_coefficients[1], row.m_coefficients[2], row.m_status, minimum[0], minimum[1],
                                    counts[0], counts[1], u64{capture_state.m_submission},
                                    capture_state.m_bounce_enabled ? "single_bounce" : "direct",
                                    capture_state.m_bounce_enabled
                                        ? u64{capture_state.m_combined_source->completionFence()}
                                        : u64{capture_state.m_submission},
                                    u64{capture_submission}, out_result.m_frame_id,
                                    out_result.m_surface_capture_sequence, cohort.m_job_id, cohort.m_world_generation,
                                    cohort.m_geometry_revision, cohort.m_content_fingerprint,
                                    8u * sizeof(u32) + rows->size() * sizeof(lighting::WorldRadianceCoefficients),
                                    rows->capacity() * sizeof(lighting::WorldRadianceCoefficients));
                                PPR_LOG_RAW(Editor, info, message);
                            }
                        }

                        const auto elapsed = getTimerManager().getTotalElapsed();
                        // The earlier bound-source present check owns the
                        // acknowledgment. A fresh old-field draw must not log
                        // the pending source's publication/static age as used.
                        if (current_source_presented and
                            elapsed - capture_state.m_last_age_report_elapsed >= std::chrono::seconds{1}) {
                            capture_state.m_last_age_report_elapsed = elapsed;
                            PPR_LOG(
                                Editor, info, "World radiance static source age",
                                {{"world", capture_state.m_source->cohort().m_world_generation},
                                    {"revision", capture_state.m_source->cohort().m_geometry_revision},
                                    {"age_since_publication_ms",
                                        time::seconds(elapsed - capture_state.m_publication_elapsed) * 1000.0},
                                    {"source_changed", false}, {"pending_edit", false},
                                    {"one_second_edit_age_gate", "unqualified; static source only"}});
                        }
                    }
                }
            }

            if (request.m_gameplay_query and
                request.m_gameplay_query->m_field_world and
                bound_lighting and
                not out_result.m_lighting_provisional and
                not out_result.m_lighting_pending) {
                const auto &field = bound_lighting->gameplayField();
                const auto &query = *request.m_gameplay_query;
                if (field and
                    field->m_demand.m_sequence == query.m_sequence and
                    field->m_cohort == bound_lighting->cohort() and
                    field->m_cohort.m_world_generation == query.m_world and
                    field->m_cohort.m_geometry_revision == query.m_revision and
                    field->m_completion == bound_lighting->completionFence() and
                    m_terrain_lighting->m_delivered_query != query.m_sequence) {
                    WorldRadianceGameplayQueryReceipt receipt{};
                    receipt.m_sequence = query.m_sequence;
                    receipt.m_world = query.m_world;
                    receipt.m_revision = query.m_revision;
                    receipt.m_field = field;
                    out_result.m_gameplay_query = std::move(receipt);
                    m_terrain_lighting->m_delivered_query = query.m_sequence;
                }
            }
            if (request.m_gameplay_query and
                not request.m_gameplay_query->m_field_world and
                request.m_gameplay_query->m_sequence != 0u) {
                // Resolve from the exact immutable source bound to this frame.
                // A retained picture has no bound source, so the receipt carries
                // not_connected rather than a loan of any other generation.
                const std::shared_ptr<const lighting::WorldRadianceSource> bound_source = bound_lighting
                                                                                              ? bound_lighting->source()
                                                                                              : nullptr;
                const std::optional<lighting::TerrainReceiverCohort> bound_cohort =
                    bound_lighting ? std::optional{bound_lighting->cohort()} : std::nullopt;
                out_result.m_gameplay_query = resolveWorldRadianceGameplayQuery(
                    *request.m_gameplay_query, bound_source, bound_cohort);
            }
        }

        return default_value_v;
    }
}
