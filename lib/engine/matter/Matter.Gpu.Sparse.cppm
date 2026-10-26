module;

export module engine.matter:gpu.sparse;

import :gpu.types;
import engine.core;
import engine.rhi;
import engine.shader;
import std;

export namespace pP::matter {
    // ------------------------------------------------------------------
    // canonical sparse storage and immutable dispatch layout
    // ------------------------------------------------------------------

    struct SparseIndex {
        u32 m_generation{}, m_slot{};
    };
    struct SparseBucket {
        u32 m_key{}, m_count{}, m_row_start{}, m_inserted{};
    };
    struct SparseGroup {
        u32 m_start{}, m_count{}, m_x{}, m_y{};
    };
    struct SparseConstants {
        u32 m_width{}, m_height{}, m_rows{}, m_bucket_columns{};
        u32 m_page_columns{}, m_bucket_capacity{}, m_page_capacity{}, m_group_capacity{};
        u32 m_generation{}, m_source_generation{}, m_dispatch_width{}, m_destination_a{};
        u32 m_bucket_keys{}, m_page_keys{}, m_reserved0{}, m_reserved1{};
    };

    struct SparseCapacity {
        u64 m_lookup_bytes{}, m_dynamic_bytes{};
        u32 m_width{}, m_height{}, m_admitted_row_capacity{};
        u32 m_bucket_keys{}, m_page_keys{}, m_bucket_columns{}, m_page_columns{};
        u32 m_buckets{}, m_pages{}, m_groups{}, m_dispatch_width{}, m_dispatch_height{};
    };

    [[nodiscard]] Expected<SparseCapacity> sparseCapacity(
        u32 admitted_row_capacity, u32 width, u32 height, const rhi::DeviceLimits &limits);

    struct SparseBuffers {
        std::array<rhi::ComPtr<rhi::IBuffer>, 2u> m_page_index{}, m_payload{};
        rhi::ComPtr<rhi::IBuffer> m_bucket_index{}, m_active_buckets{}, m_page_keys{}, m_budget{};
        rhi::ComPtr<rhi::IBuffer> m_groups{}, m_rows{}, m_counters{}, m_arguments{}, m_errors{};
    };

    // ------------------------------------------------------------------
    // pass-local maintenance resource owner
    // ------------------------------------------------------------------

    struct SparseGridState;
    enum class SparseEncodingDisposition : u8 {
        executed,
        not_submitted
    };

    /// Encodes into the caller's existing queue/encoder protocol. The caller must
    /// retain this owner and every returned immutable root through its fence.
    /// No submission, CPU active-set enumeration, or committed publication occurs.
    class SparseGridStorage {
        std::unique_ptr<SparseGridState> m_state;

    public:
        SparseGridStorage();
        ~SparseGridStorage();
        SparseGridStorage(SparseGridStorage &&) noexcept;
        SparseGridStorage &operator=(SparseGridStorage &&) noexcept;
        SparseGridStorage(const SparseGridStorage &) = delete;
        SparseGridStorage &operator=(const SparseGridStorage &) = delete;

        [[nodiscard]] std::error_code initialize(
            rhi::IDevice &device, IShaderService &shaders, const std::filesystem::path &content_root,
            u32 admitted_row_capacity, u32 width, u32 height);
        /// Admission is independent of any spare capacity in the Hot/Cold rows.
        [[nodiscard]] bool canAdmit(u32 required_rows) const noexcept;
        [[nodiscard]] const SparseCapacity &capacity() const noexcept;
        [[nodiscard]] const SparseBuffers &buffers() const noexcept;
        [[nodiscard]] SparseConstants constants(bool destination_a = false) const noexcept;

        /// Caller has established prior completion before encoding a new tick.
        /// Rollover clears only lookup metadata at the logically empty frontier.
        [[nodiscard]] std::error_code beginTick(rhi::ICommandEncoder &encoder, u32 substeps);
        /// executed requires the caller's matching fence proof. not_submitted
        /// requires discarding the definitely unsubmitted encoder. Unknown
        /// submission retains this open recording and every referenced owner/root.
        [[nodiscard]] std::error_code settleEncoding(SparseEncodingDisposition disposition);
        [[nodiscard]] std::error_code discover(
            rhi::ICommandEncoder &encoder, rhi::IBuffer &hot, rhi::IBuffer &cold, GpuParameters profile,
            Array<rhi::ComPtr<rhi::IShaderObject>> &retained_roots);
        /// B before iteration0; A after iteration0. Both use identical list slots
        /// and the single shared absolute-budget pool. Each call ends in a barrier.
        [[nodiscard]] std::error_code initializeDestination(
            rhi::ICommandEncoder &encoder, bool destination_a, Array<rhi::ComPtr<rhi::IShaderObject>> &retained_roots);

        /// Focused adversarial seams; caller proves quiescence before mutation.
        void forceNextGenerationForTest(u32 generation) noexcept;
        void reduceCapacitiesForTest(u32 buckets, u32 pages, u32 groups) noexcept;
    };
}
