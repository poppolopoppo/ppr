module;

export module engine.app:application_editor;

import :application;
import :frame.packet;
import :frame.render_lane;
import :scene.camera;
import :renderer.grid_pass;
import :renderer.retro_pass;
import :renderer.triangle_pass;
import :service.client;
import :service.input;
import :ui.imgui;

import engine.core;
import engine.image;
import engine.mesh;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    class GridPass;
    class ICameraController;
    class IInputDevice;
    class InputBackgroundLatch;
    class InputMapping;
    class IUIService;
    class TrianglePass;
    class Window;
    class WindowInputContext;

    class ApplicationEditor : public Application, protected IClientService {
    public:
        ApplicationEditor(std::string_view name, std::span<const char *const> argv);

        // B1' fail-stop injection passthrough (see Application).
        explicit ApplicationEditor(
            std::string_view name, std::span<const char *const> argv,
            GpuTeardownFailureHandler on_gpu_teardown_failure);

        ~ApplicationEditor() noexcept override;

        // IClientService
        [[nodiscard]] safe_ptr<const Camera> getMainCamera() const noexcept override;

        [[nodiscard]] safe_ptr<const Player> getMainPlayer() const noexcept override;

        [[nodiscard]] safe_ptr<const WindowViewport> getMainViewport() const noexcept override;

        [[nodiscard]] safe_ptr<Camera> getMainCamera() noexcept override;

        [[nodiscard]] safe_ptr<Player> getMainPlayer() noexcept override;

        [[nodiscard]] safe_ptr<WindowViewport> getMainViewport() noexcept override;

        [[nodiscard]] safe_ptr<const InputContext> getMainInputContext() const noexcept override;

        [[nodiscard]] safe_ptr<InputContext> getMainInputContext() noexcept override;

    public:
        // §7 scene flow: editor-owned SceneAsset + decoded ImageAssets +
        // pass-uploaded GPU handles. loadScene imports, decodes (mapFile for
        // file refs, embedded bytes otherwise) and uploads with partial
        // rollback; update() submits one instance per (scene instance, prim)
        // with the node world matrix; unloadScene releases in reverse order.
        [[nodiscard]] std::error_code loadScene(const std::filesystem::path &dir, std::string_view file);

        [[nodiscard]] std::error_code setLoadedScene(const std::filesystem::path &dir, mesh::SceneAsset &&loaded_scene);

        [[nodiscard]] std::error_code unloadScene();

        // Main-side one-shot request. Loading/held pictures are never captured;
        // the worker latches the first eligible attempt, including failure.
        [[nodiscard]] std::error_code requestFrameCapture();

        [[nodiscard]] TrianglePass &getTrianglePass() noexcept {
            return *m_triangle_pass;
        }
        [[nodiscard]] GridPass &getGridPass() noexcept {
            return *m_grid_pass;
        }

        // Retro scene-lane policy: the pixel scale is clamped here so every
        // owned frame request carries a valid value. CPU-only: no GPU work
        // and no frame drain; the next submit picks the stored settings up.
        void setRetroSettings(const RetroSettings &settings) noexcept;

        // Returns true only for a retained-only odd spatial target. The old
        // picture extent is a completed-image proof supplied by the caller;
        // the owner lane rechecks the actual pass before drawing it.
        [[nodiscard]] static Expected<bool> admitPictureFrame(
            const RetroSettings &settings, int2 target_extent, std::optional<int2> retained_extent = {});

        // Post-initialize only. Rebuilds the camera input mapping before
        // releasing the previous controller; the editor keeps camera ownership.
        [[nodiscard]] std::error_code replaceMainCameraController(
            std::unique_ptr<ICameraController> controller, ECameraProjection projection);

    protected:
        // Explicit diagnostic sequence extension. Only a clean, completed owned
        // pixel result can rearm; ordinary requestFrameCapture remains one-shot.
        [[nodiscard]] std::error_code rearmFrameCapture();
        [[nodiscard]] u64 currentFrameCaptureSequence() const noexcept {
            return m_frame_capture_sequence;
        }

        // Main-side semantic eligibility for the frame being built. The armed
        // one-shot remains pending while false; only the existing request bit
        // crosses the lane. Other Editor callers retain their default true.
        void setFrameCaptureEligible(const bool eligible) noexcept {
            m_frame_capture_eligible = eligible;
        }

        // Client's committed optical revision. Pending newer fields cannot be
        // drawn by an older queued packet before joint simulation commit.
        void setExpectedLightingRevision(const u64 revision) noexcept {
            m_expected_lighting_revision = revision;
        }

        /// Main-owned offscreen gameplay demands, copied into each render request.
        /// They extend the visible receiver rectangle; none may be dropped.
        void setWorldRadianceReceiverDemands(std::span<const lighting::WorldRadianceReceiverWindow> demands);

        /// Main-owned on-demand gameplay query request, copied into each render
        /// request. The Game never supplies or retains a WorldRadianceSource.
        void setWorldRadianceGameplayQuery(std::optional<WorldRadianceGameplayQueryRequest> request) noexcept {
            m_gameplay_query = std::move(request);
        }

        // ReSharper disable once CppOverrideWithDifferentVisibility
        [[nodiscard]] safe_ptr<const Application> getApplication() const noexcept override {
            return this;
        }

        // Test-seam construction: scripted platform + injected clock (see
        // Application). The Editor domain is fixed; windowed stages stay
        // null unless the derived fixture builds them.
        ApplicationEditor(
            std::string_view name, std::span<const char *const> argv, std::unique_ptr<IPlatform> platform,
            const ITimerClock &clock, GpuTeardownFailureHandler on_gpu_teardown_failure = nullptr);

        // Application:
        [[nodiscard]] std::error_code initialize() override;

        [[nodiscard]] std::error_code update(TimeSpan dt) override;

        [[nodiscard]] std::error_code render() override;

        [[nodiscard]] std::error_code shutdown() override;

        // Durable slot outcome settlement: the diagnostics mirror that
        // clients (Game HUD) read is stamped here and shared by both
        // inline and threaded execution. Derived classes that override
        // must chain this override inline (e.g. super_t::...) to keep the
        // mirror consistent.
        void onLaneFrameConsumed_(const RenderFrameResult &result) override;

    private:
        struct TerrainLightingState;

        [[nodiscard]] std::error_code beginTerrainLighting_(const GridWorldCommand &command);
        [[nodiscard]] Expected<bool> advanceTerrainLighting_();
        [[nodiscard]] std::error_code submitTerrainLighting_(
            bool discovery = false, u64 actual_frame = 0u, bool bounce = false, bool matter = false,
            std::optional<TimeSpan> elapsed = {});
        [[nodiscard]] Expected<bool> cancelTerrainDiscovery_();
        [[nodiscard]] Expected<bool> updateTerrainBounce_(
            TimeSpan elapsed, lighting::WorldRadianceReceiverWindow window, u64 actual_frame);
        [[nodiscard]] Expected<bool> updateTerrainDiscovery_(
            const CameraSnapshot &camera, TimeSpan elapsed, lighting::WorldRadianceReceiverWindow window,
            u64 actual_frame);
        [[nodiscard]] Expected<bool> updateTerrainMatter_(
            const std::optional<GridParticleGpuView> &view, const CameraSnapshot &camera, int2 extent, TimeSpan elapsed,
            std::span<const lighting::WorldRadianceReceiverWindow> demands, u64 actual_frame);

        [[nodiscard]] Expected<bool> updateTerrainLightingView_(
            const CameraSnapshot &camera, int2 extent, TimeSpan elapsed,
            std::span<const lighting::WorldRadianceReceiverWindow> demands, u64 actual_frame);
        [[nodiscard]] std::error_code releaseTerrainLighting_(GpuQuiescence quiescence);

        [[nodiscard]] std::error_code observeScreenLighting_();
        void observeTerrainLighting_();

        void onMainWindowFocused_(const Window &window, bool focused);

        // Phase 3 focus policy: preserves raw focus loss; the legacy
        // opt-out and display.throttle_unfocused apply centrally in
        // Application::getTargetFrameTime/displayView, so enabling
        // throttling while already blurred takes effect without another
        // focus event. Focus detection and input routing are unchanged.
        [[nodiscard]] bool backgroundThrottleWanted_(bool focused) const noexcept;

        [[nodiscard]] std::error_code submitSceneInstances_();

        // P2 client/lane split (explicit hooks, no wholesale virtual move):
        // submitRenderWork_ runs on the CLIENT lane (main): it detaches the
        // UI frame and either enqueues the owned packet (worker) or runs it
        // inline (reference). executeRenderWork_ runs on ANY lane (worker
        // or inline): render preparation, translator-adjacent pass GPU
        // work, encode/submit/present. Derived hosts (TurboLarbin) extend
        // executeRenderWork_ for lane-side work instead of moving virtuals.
        [[nodiscard]] std::error_code submitRenderWork_(const FramePacket &packet);

        // Owned per-frame pass staging payloads, built on main (scene
        // instances by the editor, world tiles by the game's present
        // phase, debug instances by the game). Transported to the lane
        // inside RenderFrameRequest and applied there — the client never
        // touches pass state directly.
        TrianglePass::TriangleStagedFrame m_triangle_stage{};
        GridPass::GridStagedFrame m_grid_stage{};

    public:
        // Pass staging diagnostics published by the shared inline/threaded
        // settlement handler: every result (honouring applied, drop,
        // obsolescence, and human-surface settling) can be distinguished.
        // The derived client reads this mirror — never the live pass
        // counters once the lane owns them.
        struct RenderDiagnostics {
            std::optional<GridPassObservations> m_grid_observations{};
            u32 m_grid_staged_tiles{0u};
            u32 m_grid_cached_tiles{0u};
            // Frame's retirement sweep outcome (CPU-side retirement
            // milestone; distinct from the fence's GPU completion).
            std::error_code m_last_sweep_error{};
            // Font atlas version this frame's encode applied (ownership
            // acknowledgement for the client-published atlas).
            u64 m_ui_font_version_applied{0u};
            // Submit fence value this frame rode (submission acceptance,
            // independent of present verdict).
            FenceCompletionValue m_last_submit_value{kFenceValueNone};
            std::optional<CameraSnapshot> m_lighting_camera{};
            // Native picture viewport and real presented surface may differ
            // while a retained image is clipped/letterboxed across resize.
            int2 m_lighting_extent{zero_v};
            int2 m_lighting_target_extent{zero_v};
            u64 m_lighting_world{};
            u64 m_lighting_revision{};
            TimeSpan m_lighting_update_age{};
            std::error_code m_lighting_error{};
            bool m_lighting_pending{};
        };
        void enableGridObservations(const bool enabled) noexcept {
            m_observe_grid = enabled;
        }

        // Startup-only, before the render lane starts. World-field timing is
        // independent of CPU world-generation observations and surface capture.
        void enableWorldRadianceMeasurements(const bool enabled) noexcept {
            m_measure_world_radiance = enabled;
        }

        [[nodiscard]] RenderDiagnostics renderDiagnostics() const noexcept {
            return m_diagnostics;
        }

        // Single handoff point for derived hosts: replaces the pending
        // staged payloads for this frame. The game's present/submit phases
        // must run between super_t::update()/render() to structurally
        // minimize the lifetime of the editor's private staging buffer.
        void submitGridStage(GridPass::GridStagedFrame stage) noexcept {
            m_grid_stage = std::move(stage);
        }
        [[nodiscard]] std::error_code submitGridWorld(std::shared_ptr<const GridWorldCommand> command);
        void submitTriangleStage(TrianglePass::TriangleStagedFrame stage) noexcept {
            m_triangle_stage = std::move(stage);
        }

    private:
        std::vector<lighting::WorldRadianceReceiverWindow> m_lighting_receiver_demands{};
        std::optional<WorldRadianceGameplayQueryRequest> m_gameplay_query{};
        u64 m_expected_lighting_revision{};

        u64 m_frame_capture_sequence{}; // Main-side monotonically reserved identity.
        u64 m_frame_capture_success_sequence{}; // Main-side completed clean result.
        u64 m_frame_capture_attempted_sequence{}; // Worker-side duplicate suppression.
        bool m_observe_grid{false};
        bool m_measure_world_radiance{};
        bool m_frame_capture_requested{}; // Main-side request transport.
        bool m_frame_capture_eligible{true}; // Main-side current-frame UI readiness.
        bool m_frame_capture_armed{}; // Main-side once-per-owner admission.

        [[nodiscard]] std::error_code executeGridWorld_(const GridWorldCommand &command, GridWorldResult &result);

        RenderDiagnostics m_diagnostics{};

    private:
        // Lane-side execution. Never touches the live Window, camera,
        // input, services (resolved at install), or the live ImGui
        // frontend — only the owned packet, the detached UI data, and
        // lane-confined GPU state. Ownership never transfers: passes stay
        // editor-owned, the scene stays client-owned, snapshots stay game-owned.
        [[nodiscard]] virtual std::error_code executeRenderWork_(
            const RenderFrameRequest &request, RenderFrameResult &out_result);

        // Submission acceptance is independent of present: every participant
        // stamps off the accepted SUBMIT value (lastSubmitted advancement),
        // never off the present verdict.
        void stampSubmittedPasses_(FenceCompletionValue accepted_value) noexcept;

        // Owned packet staged by update() for render(); cleared on consume.
        std::optional<FramePacket> m_frame_packet{};

        // Startup-boundary service captures for the lane executor (resolved
        // once at initialize, never mid-frame on the worker).
        safe_ptr<IRhiService> m_lane_rhi_service{};
        safe_ptr<IShaderService> m_lane_shader_service{};
        u64 m_lane_dropped_frames{0u};

        std::optional<mesh::SceneAsset> m_scene{};
        Array<image::ImageAsset> m_images{};
        TrianglePass::UploadedScene m_uploaded_scene{};

        std::unique_ptr<Player> m_player{};
        std::unique_ptr<Camera> m_camera{};
        std::unique_ptr<ICameraController> m_camera_controller{};
        std::unique_ptr<WindowInputContext> m_main_input_context{};
        std::unique_ptr<InputMapping> m_camera_input_mapping{};
        std::unique_ptr<IUIService> m_ui_service{};
        std::unique_ptr<WindowViewport> m_main_viewport{};
        std::unique_ptr<TrianglePass> m_triangle_pass{};
        std::unique_ptr<GridPass> m_grid_pass{};
        std::unique_ptr<RetroPass> m_retro_pass{};
        std::unique_ptr<TerrainLightingState> m_terrain_lighting{};
        bool m_scene_passes_encoded{};
        RetroSettings m_retro_settings{};

        // Background-drag actuator @ detector priority; registrar == owner.
        // Routing-owned latch; actuator borrows it (detach-before-destroy).
        std::unique_ptr<InputBackgroundLatch> m_input_background_latch{};

        IInputService::DeviceCallback::Handle m_device_disconnected_handle{};
        // Lifetime rule: the device-disconnected broadcast stores a
        // non-owning function_ref view, so this member owns the callable;
        // initialize() assigns it before subscribing a view of it, and
        // shutdown() resets the handle before member destruction.
        std23::move_only_function<std::error_code(const IInputDevice &)> m_on_device_disconnected{};
        bool m_input_latch_initialized{false};
    };
}
