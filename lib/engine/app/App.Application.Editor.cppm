module;

export module engine.app:application_editor;

import :application;
import :frame.packet;
import :frame.render_lane;
import :scene.camera;
import :renderer.frame_fence;
import :renderer.grid_pass;
import :renderer.post_process_pass;
import :renderer.presentation;
import :renderer.reconstruction_pass;
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
    class ReconstructionPass;
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

        // Independent post-processing policy: values are validated before
        // backend work; the pixel scale is clamped to P1..8 here so every owned
        // frame request carries a valid resolution control. CPU-only: no GPU
        // work and no frame drain; the next submit picks the stored values up.
        void setPostProcessSettings(const PostProcessSettings &settings, u32 pixel_scale) noexcept;

        // Post-initialize only. Rebuilds the camera input mapping before
        // releasing the previous controller; the editor keeps camera ownership.
        [[nodiscard]] std::error_code replaceMainCameraController(
            std::unique_ptr<ICameraController> controller, ECameraProjection projection);

        // TESTS ONLY: arm the renderer's possibly-executed submit failure for
        // exactly one stage (kRenderStageScene/Resolve/Present) of the next
        // executed frame. The seam is consumed by that stage and forwards to
        // the Renderer's existing injected-submit-failure contract; production
        // never arms it. std::nullopt disarms.
        void setInjectedStageSubmitFailureForTest(const std::optional<u32> stage) noexcept {
            m_test_stage_failure = stage;
        }

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
        };
        void enableGridObservations(const bool enabled) noexcept {
            m_observe_grid = enabled;
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
        u64 m_frame_capture_sequence{}; // Main-side monotonically reserved identity.
        u64 m_frame_capture_success_sequence{}; // Main-side completed clean result.
        u64 m_frame_capture_attempted_sequence{}; // Worker-side duplicate suppression.
        bool m_observe_grid{false};
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
        // Lane-side stage settlement. Every stage records its own submit fate
        // and accepted value; acceptance commits the stage's owned state once,
        // a definite rejection commits nothing, and a possibly-executed submit
        // quarantines dependent resources (never a retry or in-place rollback).
        void settleSceneStage_(RenderFrameResult &out, FenceCompletionValue before, std::error_code error) noexcept;
        void settleResolveStage_(RenderFrameResult &out, FenceCompletionValue before, std::error_code error) noexcept;
        void settlePresentStage_(RenderFrameResult &out, FenceCompletionValue before, std::error_code error) noexcept;
        // Definitely-unsubmitted release for A's armed owners (lighting
        // attempt, reconstruction pending frame, post pending frame).
        void discardUnsubmittedStages_() noexcept;

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

        // Lane-owned stage attachments. Reconstruction owns the padded 4x
        // scene color; the editor owns one completion-gated depth target per
        // ring slot at the same raster extent.
        struct SceneDepthSlot {
            rhi::ComPtr<rhi::ITexture> m_depth{};
            rhi::ComPtr<rhi::ITextureView> m_view{};
            int2 m_extent{zero_v};
            FenceCompletionValue m_last_use{kFenceValueNone};
        };

        [[nodiscard]] std::error_code ensureSceneDepth_(rhi::IDevice &device, int2 raster_extent);
        // Resolves the stage-A/B attachment views for this frame (scene color
        // and post output), recreating a cached view only when its texture
        // identity changed.
        [[nodiscard]] std::error_code resolveStageAttachments_(rhi::IDevice &device, int2 raster_extent);
        void releaseStageTargets_() noexcept;

        std::unique_ptr<Player> m_player{};
        std::unique_ptr<Camera> m_camera{};
        std::unique_ptr<ICameraController> m_camera_controller{};
        std::unique_ptr<WindowInputContext> m_main_input_context{};
        std::unique_ptr<InputMapping> m_camera_input_mapping{};
        std::unique_ptr<IUIService> m_ui_service{};
        std::unique_ptr<WindowViewport> m_main_viewport{};
        std::unique_ptr<TrianglePass> m_triangle_pass{};
        std::unique_ptr<GridPass> m_grid_pass{};
        std::unique_ptr<PostProcessPass> m_post_process{};
        std::unique_ptr<ReconstructionPass> m_reconstruction{};
        PostProcessSettings m_post_settings{};
        u32 m_pixel_scale{3u};
        std::array<SceneDepthSlot, kFrameRingSlots> m_scene_depths{};
        rhi::ComPtr<rhi::ITextureView> m_scene_color_view{};
        rhi::ComPtr<rhi::ITextureView> m_post_output_view{};
        const rhi::ITexture *m_scene_color_texture{};
        const rhi::ITexture *m_post_output_texture{};
        // Main-boundary active-presentation clock (suspension excluded) and the
        // camera-cut generation that resets screen history.
        TimeSpan m_active_elapsed{};
        u64 m_camera_cut_generation{};
        u32 m_scene_depth_cursor{};
        u32 m_scene_depth_selected{};
        // TESTS ONLY: arm the renderer's possibly-executed submit failure for
        // one stage (kRenderStage*) of the next executed frame. Production
        // never sets it; see setInjectedStageSubmitFailureForTest.
        std::optional<u32> m_test_stage_failure{};

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
