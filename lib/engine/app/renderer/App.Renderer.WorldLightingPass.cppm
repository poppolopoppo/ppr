module;
#include "pP/Macros.h"
export module engine.app:renderer.world_lighting_pass;

import :renderer.frame_fence;
import :renderer.types;
import :renderer.world_lighting;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP {
    PPR_DECLARE_LOG_CATEGORY(WorldLightingPass)

    // ------------------------------------------------------------------
    // owned world-lighting owner values
    // ------------------------------------------------------------------

    // One accepted lighting attempt's input values. The projection is
    // camera-free and time-independent; the transport stage consumes the
    // selected sky hour. m_active_elapsed is the application's cumulative
    // active presentation time at this attempt, injected at the existing frame
    // boundary. Suspended/minimized intervals are excluded by the caller. The
    // pass commits this baseline only on accepted submission, so dropped
    // attempts contribute their elapsed active time to the next accepted
    // lighting update. Production callers do not yet supply a non-default hour,
    // so the terminal sky is the black boundary sky (-1) until later renderer
    // wiring; 0..23 rotate it.
    struct WorldLightingFrame {
        TimeSpan m_active_elapsed{};
        i32 m_sky_hour{-1};
    };

    // ------------------------------------------------------------------
    // numeric status records
    // ------------------------------------------------------------------
    // Byte-identical to assets/shaders/numeric_status.slang. One record per
    // numeric-producing dispatch/draw in its in-flight attempt's status buffer.
    // UINT32_MAX means "no failure"; the engine never writes a failure through
    // a compound payload or a nondeterministic first-error-thread choice.
    inline constexpr u32 kNumericStatusWords = 8u;
    // Four projection records (clear/keys/rows/cells), three transport records
    // (source preparation, terminal L5, lower cascades) and fine reconstruction.
    inline constexpr u32 kNumericStatusRecords = 8u;
    inline constexpr u32 kNoNumericIndex = 0xffff'ffffu;

    // Fixed numeric-stage order shared by the GPU records and the CPU ticket.
    inline constexpr u32 kNumericStageClearWinners = 0u;
    inline constexpr u32 kNumericStageReduceKeys = 1u;
    inline constexpr u32 kNumericStageReduceRows = 2u;
    inline constexpr u32 kNumericStageResolveCells = 3u;
    inline constexpr u32 kNumericStageSource = 4u;
    inline constexpr u32 kNumericStageTerminal = 5u;
    inline constexpr u32 kNumericStageCascade = 6u;
    inline constexpr u32 kNumericStageFine = 7u;

    struct NumericStatusRecord {
        u32 m_invalid_index{kNoNumericIndex};
        u32 m_range_index{kNoNumericIndex};
        u32 m_reserved[6]{};
    };

    static_assert(sizeof(NumericStatusRecord) == 32u and alignof(NumericStatusRecord) == 4u);
    static_assert(
        PPR_OFFSETOF(NumericStatusRecord, m_invalid_index) == 0u and
        PPR_OFFSETOF(NumericStatusRecord, m_range_index) == 4u and
        PPR_OFFSETOF(NumericStatusRecord, m_reserved) == 8u);
    static_assert(std::is_standard_layout_v<NumericStatusRecord> and std::is_trivially_copyable_v<NumericStatusRecord>);

    class WorldLightingPass {
    public:
        // Native projection domain: one resolved u32 per cell, row-major.
        static constexpr u32 kWorldEdge{1024u};
        static constexpr u32 kCellCount{kWorldEdge * kWorldEdge};
        static constexpr u32 kResolvedCellBytes{kCellCount * sizeof(u32)};
        static constexpr u32 kScratchGridBytes{kResolvedCellBytes};

        // Full-domain acceleration: three visibility levels in one structured
        // uint2 buffer (stride 8) and one R8_UINT jump texel per native cell.
        static constexpr u32 kVisibilityRecordCount{lighting::kVisibilityRecordCount};
        static constexpr u64 kVisibilityBufferBytes{lighting::kVisibilityBufferBytes};
        static constexpr u32 kTransportEntryCount{2u};

        // Coarse cascade transport: one persistent 40 MiB interval store, one
        // 4 MiB RGB9E5 source cache and the single in-place 8 MiB fine CH field.
        static constexpr u32 kCascadeLevels{lighting::kCascadeLevels};
        static constexpr u32 kCascadeRecordCount{lighting::kCascadeRecordCount};
        static constexpr u64 kCascadeBufferBytes{lighting::kCascadeBufferBytes};
        static constexpr u32 kSourceCellBytes{lighting::kSourceCellBytes};
        static constexpr u32 kFineCHEdge{lighting::kFineCHEdge};
        static constexpr u32 kFineCHRecordCount{lighting::kFineCHRecordCount};
        static constexpr u64 kFineCHBytes{lighting::kFineCHBytes};
        static constexpr u32 kCascadeEntryCount{3u};
        static constexpr u32 kFineEntryCount{1u};
        static constexpr u32 kFineDispatchThreads{8u};

        // Fixed projection dispatch order. The status record index is the
        // numeric stage constant, not this count: the transport and fine stages
        // own the additional records.
        static constexpr u32 kNumericDispatchCount{4u};
        static constexpr u32 kStatusRingSize{kFrameRingSlots};
        static constexpr u32 kStatusAttemptBytes{kNumericStatusRecords * sizeof(NumericStatusRecord)};
        static_assert(kNumericDispatchCount == 4u);
        static_assert(
            kNumericDispatchCount == kNumericStageResolveCells + 1u and
            kNumericStatusRecords == kNumericStageFine + 1u);
        static_assert(kFineCHEdge * kFineCHEdge == kFineCHRecordCount);

        WorldLightingPass() = default;
        WorldLightingPass(const WorldLightingPass &) = delete;
        WorldLightingPass &operator=(const WorldLightingPass &) = delete;
        ~WorldLightingPass();

        // Loads the projection, acceleration, source, cascade and fine
        // reconstruction shader entries and creates their compute pipelines.
        // Idempotent for one device/shader generation.
        [[nodiscard]] std::error_code initialize(IShaderService &shaders, const std::filesystem::path &root);

        // Retains the exact input owners for this attempt, allocates the
        // world's resolved cells, and encodes clearWinners, reduceKeys,
        // reduceRows, resolveCells, the full-domain acceleration rebuild, the
        // cached source, the scheduled L5-L1 cascade transport and the in-place
        // fine CH reconstruction in that order. Ring exhaustion is ordinary
        // non-admission: no pending status slot is overwritten. A second prepare
        // before the armed attempt is submitted or discarded is rejected
        // (operation_not_permitted) without touching that attempt.
        [[nodiscard]] std::error_code prepare(
            const PreRenderContext &context, const lighting::WorldLightingInputs &inputs,
            const WorldLightingFrame &frame);

        // Binds the current resolved cells and numeric status for material and
        // inspection shaders. The cursor names are owner-private.
        [[nodiscard]] std::error_code bind(rhi::ShaderCursor &cursor) const;

        // Encodes the stage-end copy of this attempt's status records into its
        // completion-fenced readback storage. Called in the same submission
        // after the prepared dispatches.
        [[nodiscard]] std::error_code finalize(const PreRenderContext &context);

        // Accepted submission: the attempt's write and status-copy fences.
        void notifySubmitted(FenceCompletionValue value) noexcept;
        // Stage-C diagnostic readers extend the attempt's retention.
        void notifyReadSubmitted(FenceCompletionValue value) noexcept;

        // Definitely-unsubmitted attempts release their slot; a submission
        // whose completion is unknown stays quarantined (never released).
        void discardUnsubmitted() noexcept;

        // Inspects every attempt whose submission completed at or below
        // `completed`. A completed failure stops further admission by returning
        // result_out_of_range (representational overflow) or
        // state_not_recoverable (invalid GPU arithmetic/state) after recording
        // category/index/frame diagnostics.
        [[nodiscard]] std::error_code collectCompleted(FenceCompletionValue completed);

        // Teardown: drains and inspects outstanding tickets. A pending or
        // failed attempt is reported instead of a successful teardown.
        [[nodiscard]] std::error_code shutdown();
        void notifyDeviceLost() noexcept;

        // ------------------------------------------------------------------
        // tests-only observations
        // ------------------------------------------------------------------

        [[nodiscard]] rhi::IBuffer *resolvedCellsForTest() const noexcept;
        // Current attempt slot's visibility masks and jump table. They are
        // allocated per slot, so a snapshot stays readable after a later
        // attempt selects another slot.
        [[nodiscard]] rhi::IBuffer *accelerationVisibilityForTest() const noexcept;
        [[nodiscard]] rhi::ITexture *accelerationJumpForTest() const noexcept;
        // Mapped fixed-record status readback of the last armed attempt, or
        // empty when no attempt was armed. Used to observe minima and inject
        // faults.
        [[nodiscard]] std::span<u32> pendingStatusWordsForTest() noexcept;
        [[nodiscard]] bool statusSlotPendingForTest(u32 slot) const noexcept;
        [[nodiscard]] bool statusSlotSubmittedForTest(u32 slot) const noexcept;
        // Persistent coarse cascades (5x1,048,576 uint2), the RGB9E5 source
        // cache and the single in-place fine CH field.
        [[nodiscard]] rhi::IBuffer *cascadeIntervalsForTest() const noexcept;
        [[nodiscard]] rhi::IBuffer *sourceCellsForTest() const noexcept;
        [[nodiscard]] rhi::IBuffer *fineCHForTest() const noexcept;
        // Accepted lighting-frame index this world is currently on. The seam
        // exercises schedule identity far beyond 2^32 and counter exhaustion.
        [[nodiscard]] u64 acceptedFrameForTest() const noexcept;
        void setAcceptedFrameForTest(u64 frame) noexcept;

    private:
        // ------------------------------------------------------------------
        // projection ownership
        // ------------------------------------------------------------------

        struct ProjectionProgram {
            rhi::ComPtr<rhi::IShaderProgram> m_program{};
            rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
        };

        // One in-flight attempt's status storage and exact input owners. The
        // slot is reusable only after its submission completed AND collection
        // consumed it; m_pending is that single occupancy fact.
        struct ProjectionStatusSlot {
            rhi::ComPtr<rhi::IBuffer> m_status{};
            rhi::ComPtr<rhi::IBuffer> m_readback{};
            // Per-slot acceleration storage. A slot is reused only after its
            // submission (and any read) completed, so the previous trace readers
            // precede these next writers without a host wait.
            rhi::ComPtr<rhi::IBuffer> m_visibility{};
            rhi::ComPtr<rhi::ITexture> m_jump{};
            lighting::WorldLightingInputs m_inputs{};
            void *m_readback_mapped{};
            FenceCompletionValue m_submission{kFenceValueNone};
            FenceCompletionValue m_read_submission{kFenceValueNone};
            TimeSpan m_active_elapsed{};
            u64 m_world_generation{};
            u64 m_accepted_frame{};
            bool m_pending{};
            bool m_copy_encoded{};
        };

        struct ProjectionResources {
            std::array<rhi::ComPtr<rhi::IBuffer>, 2u> m_scratch{};
            rhi::ComPtr<rhi::IBuffer> m_resolved{};
            // One zero metadata row keeps the constant resolve binding valid for
            // a zero-particle world without inventing a publication.
            rhi::ComPtr<rhi::IBuffer> m_empty_metadata{};
            std::array<ProjectionProgram, kNumericDispatchCount> m_programs{};
            std::array<std::array<rhi::ComPtr<rhi::IShaderObject>, kStatusRingSize>, kNumericDispatchCount> m_roots{};
            std::array<ProjectionProgram, kTransportEntryCount> m_transport_programs{};
            std::array<std::array<rhi::ComPtr<rhi::IShaderObject>, kStatusRingSize>, kTransportEntryCount>
                m_transport_roots{};
            std::array<ProjectionProgram, kCascadeEntryCount> m_cascade_programs{};
            std::array<std::array<rhi::ComPtr<rhi::IShaderObject>, kStatusRingSize>, kCascadeEntryCount>
                m_cascade_roots{};
            std::array<ProjectionProgram, kFineEntryCount> m_fine_programs{};
            std::array<std::array<rhi::ComPtr<rhi::IShaderObject>, kStatusRingSize>, kFineEntryCount> m_fine_roots{};
            // One ordered in-place 40 MiB interval store, one 4 MiB source cache
            // and the single in-place 8 MiB fine CH field. They are created once
            // per world and never reallocated per frame; there is no history
            // copy or ping-pong field.
            rhi::ComPtr<rhi::IBuffer> m_cascades{};
            rhi::ComPtr<rhi::IBuffer> m_source{};
            rhi::ComPtr<rhi::IBuffer> m_fine_ch{};
            std::array<ProjectionStatusSlot, kStatusRingSize> m_status{};
            GridWorldIdentity m_world{};
            u64 m_accepted_attempts{};
            // Cumulative active presentation time committed by the last accepted
            // submission. Rejected/unsubmitted work advances neither this
            // baseline nor m_accepted_attempts.
            TimeSpan m_accepted_active_elapsed{};
            u64 m_attempt_sequence{};
            u32 m_status_cursor{};
            u32 m_attempt_slot{};
            bool m_armed{};
        };

        [[nodiscard]] std::error_code ensureResources_(rhi::IDevice &device);
        [[nodiscard]] std::error_code ensureTransportResources_(rhi::IDevice &device);
        [[nodiscard]] std::error_code ensureCascadeResources_(rhi::IDevice &device);
        [[nodiscard]] std::error_code selectStatusSlot_(FenceCompletionValue completed) noexcept;
        [[nodiscard]] std::error_code encodeProjection_(const PreRenderContext &context);
        [[nodiscard]] std::error_code encodeAcceleration_(const PreRenderContext &context);
        [[nodiscard]] std::error_code encodeCascades_(const PreRenderContext &context, const WorldLightingFrame &frame);
        [[nodiscard]] std::error_code encodeFine_(const PreRenderContext &context);
        [[nodiscard]] std::error_code inspectAttempt_(ProjectionStatusSlot &slot);
        void clearAttempt_(ProjectionStatusSlot &slot) noexcept;
        void releaseResources_() noexcept;

        safe_ptr<IShaderService> m_shaders{};
        std::filesystem::path m_root{};
        rhi::IDevice *m_device{};
        std::unique_ptr<ProjectionResources> m_resources{};
        bool m_ready{};
    };
}
