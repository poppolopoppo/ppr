export module engine.app:renderer.reconstruction_pass;

import :renderer.frame_fence;
import :renderer.presentation;
import :renderer.types;
import :scene.camera;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // always-on temporal Hable reconstruction
    // ------------------------------------------------------------------
    // Owns the padded 4x scene target and the single-sample resolved history
    // pair. `beginFrame` proposes the next phase and the resources for one
    // frame; `prepare` reconstructs into the proposed write history in the
    // same stage-B command buffer post-processing reads. Only an accepted
    // stage-B `notifySubmitted` commits the camera, padded grid, camera-cut
    // generation, history role and phase. Ordinary pan/zoom reprojects;
    // world replacement, an explicit camera cut or any W/P/R/S/B change
    // resets. Exposure, bloom and sky-hour live outside this pass and never
    // reset history.

    class ReconstructionPass final {
    public:
        // Standard 4x sample pattern in raster top-left pixel coordinates.
        // Mirrored by hable_temporal.slang.
        static constexpr u32 kSampleCount{4u};
        static constexpr u32 kPhaseCount{4u};
        static constexpr float kMotionFloorPixels{0.1f};
        static constexpr float kMotionRangePixels{0.4f};
        static constexpr float kMinCurrentWeight{0.05f};
        static constexpr float kMaxCurrentWeight{0.25f};

        [[nodiscard]] std::error_code initialize(
            IShaderService &shader_service, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code shutdown();
        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        // Chooses the frame extents/resources and proposes the next phase
        // without committing history. `camera` is the unjittered snapshot
        // whose framing describes the window. Zero framebuffer extents are
        // caller suspension and are rejected rather than fabricated.
        [[nodiscard]] std::error_code beginFrame(
            rhi::IDevice &device, const CameraSnapshot &camera, const PresentationGrid &grid, GridWorldIdentity world,
            u64 camera_cut, FenceCompletionValue completed);

        // Padded, phase-jittered snapshot for the scene pass. Valid after a
        // successful beginFrame.
        [[nodiscard]] const CameraSnapshot &sceneCamera() const noexcept;
        // Sx by Sy, RGBA16F, 4-sample scene color target written by stage A.
        [[nodiscard]] rhi::ITexture &sceneTexture() const noexcept;

        // Encodes the Hable lattice reconstruction into the proposed write
        // history. `resolved()` is readable by post-processing later in the
        // same stage-B command buffer.
        [[nodiscard]] std::error_code prepare(const PreRenderContext &context);
        [[nodiscard]] ResolvedHdrView resolved() const;
        // Returns the written history and scene target to their reader/writer
        // states for post-processing and the next stage A.
        [[nodiscard]] std::error_code finalize(const PreRenderContext &context);

        // Pins the stage-A scene writer even when stage B is rejected.
        void notifySceneSubmitted(FenceCompletionValue submit_value) noexcept;
        // Accepted stage-B settlement: commits camera, padded grid, camera
        // cut, history role and phase. Definitely unsubmitted work commits
        // nothing.
        void notifySubmitted(FenceCompletionValue submit_value) noexcept;
        void discardUnsubmitted() noexcept;
        [[nodiscard]] std::error_code collectCompleted(FenceCompletionValue completed);

    private:
        // ------------------------------------------------------------------
        // frame allocations and pending/committed history
        // ------------------------------------------------------------------
        struct FrameResources {
            rhi::ComPtr<rhi::ITexture> m_scene{};
            rhi::ComPtr<rhi::ITextureView> m_scene_view{};
            std::array<rhi::ComPtr<rhi::ITexture>, 2> m_history{};
            std::array<rhi::ComPtr<rhi::ITextureView>, 2> m_history_views{};
            int2 m_scene_extent{zero_v};
            int2 m_backing_extent{zero_v};
        };

        // beginFrame proposes these values; prepare consumes them. Nothing here
        // changes the committed history until notifySubmitted.
        struct PendingFrame {
            CameraSnapshot m_scene_camera{};
            CameraSnapshot m_camera{};
            PresentationGrid m_grid{};
            GridWorldIdentity m_world{};
            u64 m_camera_cut{};
            u32 m_root{};
            u32 m_write_index{};
            u32 m_previous_index{1u};
            u32 m_phase{};
            bool m_history_valid{};
            bool m_active{};
            bool m_prepared{};
        };

        [[nodiscard]] static CameraSnapshot makeSceneCamera_(
            const CameraSnapshot &camera, const PresentationGrid &grid, float2 jitter) noexcept;
        [[nodiscard]] static float2 phaseJitter_(u32 phase) noexcept;

        [[nodiscard]] std::error_code createPrograms_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] std::error_code ensureResources_(rhi::IDevice &device, const PresentationGrid &grid);
        void releaseGpuResources_() noexcept;

        // ------------------------------------------------------------------
        // program and pass state
        // ------------------------------------------------------------------

        rhi::ComPtr<rhi::IShaderProgram> m_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
        std::array<rhi::ComPtr<rhi::IShaderObject>, kFrameRingSlots> m_roots{};

        DeferredReleaseQueue m_deferred_releases{};

        FrameResources m_resources{};

        // Committed accepted state. The phase and previous camera only move on
        // an accepted stage-B submission.
        PresentationGrid m_history_grid{};
        CameraSnapshot m_history_camera{};
        GridWorldIdentity m_history_world{};
        u64 m_history_camera_cut{};
        u32 m_history_index{};
        u32 m_phase{};
        bool m_history_established{};

        // Last-use values for the retired-allocation queue and the per-frame
        // root objects. Nothing is destroyed before its readers complete.
        std::array<FenceCompletionValue, kFrameRingSlots> m_root_use{};
        FenceCompletionValue m_scene_use{kFenceValueNone};
        FenceCompletionValue m_history_use{kFenceValueNone};
        u32 m_root_cursor{};

        PendingFrame m_pending{};
        bool m_ready{};
    };
} // namespace pP
