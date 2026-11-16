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
    // value-only accepted-history observation
    // ------------------------------------------------------------------

    /// Copyable proof of the reconstruction history role committed by one
    /// accepted stage-B submission. `m_reset` is true only when that frame's
    /// beginFrame chose a fresh history (first frame, world/camera cut or any
    /// W/P/R/S/B change); it never transfers GPU storage.
    struct ReconstructionHistoryStamp {
        GridWorldIdentity m_world{};
        u64 m_frame{};
        PresentationGrid m_grid{};
        u32 m_index{};
        u32 m_phase{};
        bool m_established{};
        bool m_reset{};
    };
    static_assert(sizeof(ReconstructionHistoryStamp) == 80u and alignof(ReconstructionHistoryStamp) == 8u);

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

        // ------------------------------------------------------------------
        // interior dispatch contract
        // ------------------------------------------------------------------
        // A resolved pixel is interior when every source sample its lattice
        // block references is a valid sample, so it cannot reach the border
        // fallback. The rectangle is conservative sufficient coverage: it admits
        // one pixel of lattice neighbourhood at all four sub-pixel positions, so
        // some fallback-free edge pixels remain in the border domain. The border
        // kernel owns every backing pixel outside the rectangle, hence the two
        // dispatch domains are disjoint and exhaustive, including when the
        // rectangle is empty.
        //
        // Mango vectors have a user-provided empty default constructor, so `{}`
        // leaves their components indeterminate; the zero values below are
        // explicit and every `InteriorRegion{}` is a canonical empty rectangle.
        struct InteriorRegion {
            int2 m_origin{0, 0};
            int2 m_extent{0, 0};

            [[nodiscard]] bool empty() const noexcept {
                return m_extent.x <= 0 or m_extent.y <= 0;
            }

            [[nodiscard]] bool contains(const int2 pixel) const noexcept {
                const int2 offset{pixel.x - m_origin.x, pixel.y - m_origin.y};
                return offset.x >= 0 and offset.y >= 0 and offset.x < m_extent.x and offset.y < m_extent.y;
            }
        };

        /// Conservative phase-dependent interior rectangle in backing
        /// coordinates for one frame's jitter, or a canonical zero origin/extent
        /// when no axis admits a fully valid lattice neighbourhood. `prepare`
        /// dispatches it ahead of the border domain; the reconstruction tests
        /// prove it against the independent lattice derivation.
        [[nodiscard]] static InteriorRegion interiorRegion(const PresentationGrid &grid, float2 jitter) noexcept;

        static constexpr u32 kBorderRegionCount = 4u;

        /// Disjoint top/bottom/left/right complement of an interiorRegion result.
        /// Empty bands are canonical empty rectangles; an empty interior returns
        /// the full backing in the first slot and three empty slots.
        [[nodiscard]] static std::array<InteriorRegion, kBorderRegionCount> borderRegions(
            int2 backing, const InteriorRegion &interior) noexcept;

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

        // ------------------------------------------------------------------
        // tests-only committed-state observations
        // ------------------------------------------------------------------
        // Accepted stage-B state. It moves only on notifySubmitted, so a test
        // can prove an accepted resolve commits exactly once and a later
        // rejected attempt commits nothing.
        /// Value-only committed history observation for the frame result.
        /// Must be called after `notifySubmitted`/`discardUnsubmitted` has
        /// settled the frame; it reads no GPU resource.
        [[nodiscard]] ReconstructionHistoryStamp historyStamp() const noexcept {
            return ReconstructionHistoryStamp{
                .m_world = m_history_world,
                .m_grid = m_history_grid,
                .m_index = m_history_index,
                .m_phase = m_phase,
                .m_established = m_history_established,
                .m_reset = m_last_begin_reset};
        }
        [[nodiscard]] bool historyEstablishedForTest() const noexcept {
            return m_history_established;
        }
        [[nodiscard]] u32 acceptedPhaseForTest() const noexcept {
            return m_phase;
        }
        [[nodiscard]] u32 historyIndexForTest() const noexcept {
            return m_history_index;
        }
        [[nodiscard]] GridWorldIdentity historyWorldForTest() const noexcept {
            return m_history_world;
        }
        [[nodiscard]] FenceCompletionValue historyUseForTest() const noexcept {
            return m_committed.m_history_use;
        }
        // Read-only committed textures for non-contamination evidence. The
        // caller must prove GPU completion (renderer/host wait) before reading.
        // The scene target is 4x MSAA and has no buffer-readable form; only the
        // single-sample history is a readback source.
        [[nodiscard]] rhi::ITexture *sceneTextureForTest() const noexcept {
            return m_history_established ? m_committed.m_scene.get() : nullptr;
        }
        [[nodiscard]] rhi::ITexture *historyTextureForTest() const noexcept {
            return m_history_established ? m_committed.m_history[m_history_index].get() : nullptr;
        }

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
            FenceCompletionValue m_scene_use{kFenceValueNone};
            FenceCompletionValue m_history_use{kFenceValueNone};
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
            bool m_tentative{};
            bool m_active{};
            bool m_prepared{};
        };

        [[nodiscard]] static CameraSnapshot makeSceneCamera_(
            const CameraSnapshot &camera, const PresentationGrid &grid, float2 jitter) noexcept;
        [[nodiscard]] static float2 phaseJitter_(u32 phase) noexcept;
        [[nodiscard]] static bool matchesExtents_(
            const FrameResources &resources, const PresentationGrid &grid) noexcept;

        [[nodiscard]] std::error_code createPrograms_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);
        [[nodiscard]] Expected<FrameResources> createResources_(
            rhi::IDevice &device, const PresentationGrid &grid) const;
        void deferResources_(FrameResources &&resources) noexcept;
        [[nodiscard]] FrameResources &pendingResources_() noexcept;
        [[nodiscard]] const FrameResources &pendingResources_() const noexcept;
        void releaseGpuResources_() noexcept;

        // ------------------------------------------------------------------
        // program and pass state
        // ------------------------------------------------------------------
        // One kernel per dispatch domain: the interior entry carries the fixed
        // stencil without border handling, the border entry the unchanged
        // generic lattice. Each owns its pipeline and its root objects for the
        // frame ring slots both kernels share.
        struct ResolveKernel {
            rhi::ComPtr<rhi::IShaderProgram> m_program{};
            rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
            // Allocated once at initialization: one interior or four border
            // roots per fence-protected frame slot, never mutated after encoding.
            std::vector<rhi::ComPtr<rhi::IShaderObject>> m_roots{};
        };

        [[nodiscard]] static std::error_code encodeDispatch_(
            const ResolveKernel &kernel, u32 root, int2 extent, const PreRenderContext &context);

        ResolveKernel m_interior{};
        ResolveKernel m_border{};

        DeferredReleaseQueue m_deferred_releases{};

        // Accepted committed pair. A pending frame with different extents uses
        // a tentative pair instead; the committed pair is retired only after
        // acceptance, and a definite discard retires only the tentative pair.
        FrameResources m_committed{};
        std::optional<FrameResources> m_tentative{};

        // Committed accepted state. The phase and previous camera only move on
        // an accepted stage-B submission.
        PresentationGrid m_history_grid{};
        CameraSnapshot m_history_camera{};
        GridWorldIdentity m_history_world{};
        u64 m_history_camera_cut{};
        u32 m_history_index{};
        u32 m_phase{};
        bool m_history_established{};
        bool m_last_begin_reset{};

        // Per-frame root objects and the retirement cursor. Nothing is
        // destroyed before its readers complete.
        std::array<FenceCompletionValue, kFrameRingSlots> m_root_use{};
        u32 m_root_cursor{};

        PendingFrame m_pending{};
        bool m_ready{};
    };
} // namespace pP
