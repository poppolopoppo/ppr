module;
#include "pP/Macros.h"
export module engine.app:renderer.gpu_caches;

import engine.core;
import engine.math;
import engine.rhi;
import engine.image;
import engine.mesh;

import std;

// Pass-owned GPU caches (docs/plans/asset-pipeline.md §2.4). Render-thread
// confined: no internal mutex; concurrent upload is a caller bug. Handles are
// SparseVector keys and fail closed on stale/double release (tryGet miss or
// key-scan miss → invalid_argument, never silent success).

export namespace pP {
    // Composed GPU-data-layer identity (Phase 6 A3): the wide handle is
    // composed HERE from the container-minted SparseHandle (u32 index + u32
    // monotonic generation), not by widening SparseVector itself — the
    // container stays a general-purpose u32-index/u8-seed store with both key
    // and handle paths intact, and no GPU-protocol field changes size.
    // ABA proof: generations are container-monotonic, never reset (not even
    // by clear), skip 0, and start from a nonzero mix — 2^32 reuse cycles per
    // slot per container lifetime cannot wrap in practice, so no historical
    // handle revalidates; every lookup goes through the generation-checked
    // tryGet(SparseHandle) path, never the wrapping 8-bit seed. Handles are
    // CPU-side only (the GPU sees slots/indices/offsets, all still 4 B).
    struct TextureHandleTag final {
    };

    using TextureHandle = Numeric<SparseHandle, TextureHandleTag>;

    struct MaterialHandleTag final {
    };

    using MaterialHandle = Numeric<SparseHandle, MaterialHandleTag>;

    struct TriangleBagHandleTag final {
    };

    using TriangleBagHandle = Numeric<SparseHandle, TriangleBagHandleTag>;

    [[nodiscard]] bool isValid(const TextureHandle handle) noexcept {
        return (*handle).isValid();
    }

    [[nodiscard]] bool isValid(const MaterialHandle handle) noexcept {
        return (*handle).isValid();
    }

    [[nodiscard]] bool isValid(const TriangleBagHandle handle) noexcept {
        return (*handle).isValid();
    }

    static_assert(std::is_standard_layout_v<TextureHandle>);
    static_assert(sizeof(TextureHandle) == 8u);
    static_assert(std::is_standard_layout_v<MaterialHandle>);
    static_assert(sizeof(MaterialHandle) == 8u);
    static_assert(std::is_standard_layout_v<TriangleBagHandle>);
    static_assert(sizeof(TriangleBagHandle) == 8u);

    struct BagBucketIdTag final {
    };

    using BagBucketId = Numeric<u32, BagBucketIdTag>;

    struct TextureBindlessIndexTag final {
    };

    using TextureBindlessIndex = Numeric<u32, TextureBindlessIndexTag>;

    // GPU none-sentinel (shader compares slots to this). Declared here and
    // defined in App.Renderer.GpuCaches.cpp: an inline constexpr variable of
    // imported template type in namespace pP trips MSVC C1001 (same family as
    // the extern-umbrella workaround in engine.tests suites).
    extern const TextureBindlessIndex kNoTexture;

    static_assert(std::is_standard_layout_v<BagBucketId>);
    static_assert(sizeof(BagBucketId) == 4u);
    static_assert(std::is_standard_layout_v<TextureBindlessIndex>);
    static_assert(sizeof(TextureBindlessIndex) == 4u);

    // Plain draw metadata passed by value — carries NO identity and must not
    // be used to release. m_vb_offset is u32: the shader push takes uint and
    // BufferDesc sizes are narrowed with safe_narrowing at upload.
    struct TriangleBagRange {
        u32 m_vb_offset = 0u;
        u32 m_ib_start = 0u;
        u32 m_count = 0u;
        i32 m_base = 0;
        float m_bounds_min[3]{};
        float m_bounds_max[3]{};
    };

    // Plain-float-array POD (Gate 3 C2): Box carries user-provided copy/dtor,
    // so bounds ride float arrays like the P1 vertex verdict.
    static_assert(std::is_trivially_copyable_v<TriangleBagRange>);
    static_assert(std::is_standard_layout_v<TriangleBagRange>);

    // 4 named GPU-protocol slots, no raw-uint quartet.
    struct GpuTextureRefs {
        TextureBindlessIndex m_albedo{};
        TextureBindlessIndex m_metallic_roughness{};
        TextureBindlessIndex m_normal{};
        TextureBindlessIndex m_emissive{};
    };

    static_assert(std::is_trivially_copyable_v<GpuTextureRefs>);
    static_assert(sizeof(GpuTextureRefs) == 16u);

    // Explicit mask, NO C++ bitfields on a GPU protocol type.
    struct GpuMaterialFlags {
        u32 m_bits = 0u;
    };

    inline constexpr u32 kGpuMaterialAlphaModeMask = 0x3u;
    inline constexpr u32 kGpuMaterialDoubleSidedBit = 0x4u;

    static_assert(sizeof(GpuMaterialFlags) == 4u);

    // Slang mirror: float4 rows + uint rows, row-major. 5 rows × 16 B keeps
    // StructuredBuffer stride clean (80 B stride, 4 B alignment). Occlusion
    // shares the composited m_mr texture (ORM: R=occlusion-or-1, G=rough,
    // B=metal) — no 5th slot. Plain-float-array POD (Gate 3 C2): float4
    // carries user-provided copy/dtor, so rows ride float arrays like the P1
    // vertex verdict; math lives at the pack boundary only.
    struct GpuMaterial {
        float m_base_color[4]{1.0f, 1.0f, 1.0f, 1.0f};
        float m_emissive_metallic[4]{0.0f, 0.0f, 0.0f, 1.0f};
        float m_rough_alpha_occl_nscale[4]{1.0f, 0.5f, 1.0f, 1.0f};
        GpuTextureRefs m_textures{};
        mesh::UvSetId m_texcoord{};
        GpuMaterialFlags m_flags{};
        u32 m_pad[2]{};
    };

    static_assert(std::is_trivially_copyable_v<GpuMaterial>);
    static_assert(std::is_standard_layout_v<GpuMaterial>);
    static_assert(sizeof(GpuMaterial) == 80u);
    static_assert(alignof(GpuMaterial) == 4u);
    static_assert(PPR_OFFSETOF(GpuMaterial, m_textures) == 48u);

    // §2.4 range layout: offsets/count/base/bounds (4+4+4+4+12+12) = 40 B.
    static_assert(sizeof(TriangleBagRange) == 40u);

    // Pure-CPU material packing (plan §2.4 mapping): base_color+alpha →
    // m_base_color; emissive.rgb+metallic → m_emissive_metallic;
    // roughness/alpha_cutoff/occlusion_strength/normal_scale →
    // m_rough_alpha_occl_nscale; resolved TextureHandles → residentIndex →
    // m_textures slots (missing → kNoTexture); shared texcoord set →
    // m_texcoord; alpha_mode/twosided → m_flags bits. Enabled slots must agree
    // on the texcoord set; blend is REJECTED with function_not_supported.
    // Shader fallbacks when slot = kNoTexture: albedo→m_base_color, mr→factors,
    // normal→geometric normal, emissive→m_emissive factor over black.
    [[nodiscard]] Expected<GpuMaterial> buildGpuMaterial(
        const mesh::MaterialAsset &asset,
        GpuTextureRefs resolved) noexcept;

    // Pipeline-variant key (plan §7): target signature + twosided cull +
    // opaque/mask alpha. BLEND is deferred: rejected with
    // function_not_supported until a sorting + depth-write policy exists.
    struct TrianglePipelineVariant {
        bool m_twosided = false;
        mesh::AlphaMode m_alpha = mesh::AlphaMode::opaque;

        // Hand-written (no defaulted comparisons: MSVC module ICE family).
        [[nodiscard]] constexpr bool operator==(const TrianglePipelineVariant &other) const noexcept {
            return m_twosided == other.m_twosided and m_alpha == other.m_alpha;
        }

        [[nodiscard]] constexpr bool operator<(const TrianglePipelineVariant &other) const noexcept {
            if (m_twosided != other.m_twosided) {
                return m_twosided < other.m_twosided;
            }
            return enumOrd(m_alpha) < enumOrd(other.m_alpha);
        }
    };

    [[nodiscard]] inline hash_t hashValue(const TrianglePipelineVariant variant) noexcept {
        return hash::combine(hash::trivial(&variant.m_twosided, hash::default_seed_v),
            enumOrd(variant.m_alpha));
    }

    [[nodiscard]] std::error_code checkPipelineVariant(TrianglePipelineVariant variant) noexcept;

    // Capacity telemetry (§2.4 preflight): single-load budgets. Overflow is
    // deterministic fail-closed (no_buffer_space); the usage accessors on each
    // cache report against these exact limits. The texture budget lives in
    // rhi::kBindlessTextureBudget (4096) and is taken as the initialize arg.
    inline constexpr u64 kTriangleBagVertexCapacity = 4u * 1024u * 1024u;
    inline constexpr u64 kTriangleBagIndexCapacity = 1u * 1024u * 1024u;
    inline constexpr u32 kBindlessMaterialCapacity = 512u;

    // Residency state machine (Phase 6 A3, per pass-owned cache):
    // uninitialized → ready (initialize) → device_lost (notifyDeviceLost) →
    // uninitialized (shutdown; the only exit from device_lost — restart is an
    // explicit shutdown + initialize pair, never an in-place recreate).
    // notifyDeviceLost releases GPU objects only and retains every CPU record
    // (ranges/entries/dedup/slots/counters), so telemetry and release() keep
    // working while every GPU-touching op fails closed with no_such_device;
    // initialize while device_lost fails closed with device_or_resource_busy.
    // Partial init is uninitialized by definition: a failed initialize leaves
    // the cache exactly as it found it (rollback before reporting).
    enum class CacheResidency : u8 {
        uninitialized,
        ready,
        device_lost,
    };

    // ONE class, all vertex types; bucket = hash(stride, index_type). MVP has
    // static buckets only by construction. Bag upload keeps TYPED-span
    // signatures (verts stay typed Arrays) — buffers appear only for staging
    // copies. GPU memory is bump-allocated per bucket; free-list deferred.
    // A bag that does not fit u32 offsets is rejected with invalid_argument.
    // Single-load contract (MVP): release() retires records without
    // reclaiming GPU bytes and slots never reuse — the pass loads one scene
    // per cache lifetime and drops everything at shutdown. No eviction, no
    // streaming; a free-list waits for a streaming phase that needs it.
    class TriangleBagCache {
    public:
        [[nodiscard]] std::error_code initialize(rhi::IDevice &device);

        [[nodiscard]] std::error_code shutdown();

        // Residency (Phase 6 A3): current lifecycle state; notifyDeviceLost
        // drops GPU buffers, retains CPU records, and parks the cache in
        // device_lost until shutdown (idempotent on uninitialized/device_lost).
        [[nodiscard]] CacheResidency residency() const noexcept;

        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        template<typename V>
        [[nodiscard]] Expected<TriangleBagHandle> upload(
            const std::span<const V> verts, const std::span<const u32> idx) {
            return upload(verts, idx, 0);
        }

        // Prim-slice upload: indices stay file-order while the Mango prim base
        // (file-local + base in all cases) rides the range into MeshPush.
        template<typename V>
        [[nodiscard]] Expected<TriangleBagHandle> upload(
            const std::span<const V> verts, const std::span<const u32> idx, const i32 base) {
            static_assert(std::is_trivially_copyable_v<V>);
            if (verts.empty() or idx.empty())
            [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
            return uploadBytes_(std::as_bytes(verts), sizeof(V), idx, base);
        }

        [[nodiscard]] Expected<TriangleBagRange> resolve(TriangleBagHandle handle) const noexcept;

        [[nodiscard]] Expected<BagBucketId> bucketOf(TriangleBagHandle handle) const noexcept;

        [[nodiscard]] std::error_code release(TriangleBagHandle handle) noexcept;

        [[nodiscard]] rhi::IBuffer *vertexBuffer(BagBucketId bucket) const noexcept;

        [[nodiscard]] rhi::IBuffer *indexBuffer(BagBucketId bucket) const noexcept;

        // Usage telemetry (bytes summed over buckets; ranges never reclaim).
        [[nodiscard]] u64 vertexUsed() const noexcept;

        [[nodiscard]] u64 vertexCapacity() const noexcept;

        [[nodiscard]] u64 indexUsed() const noexcept;

        [[nodiscard]] u64 indexCapacity() const noexcept;

        [[nodiscard]] u64 rangeCount() const noexcept;

    private:
        struct BagBucket {
            u32 m_stride = 0u;
            u64 m_vertex_capacity = 0u;
            u64 m_vertex_used = 0u;
            u64 m_index_capacity = 0u;
            u64 m_index_used = 0u;
            rhi::ComPtr<rhi::IBuffer> m_vertex_buffer{};
            rhi::ComPtr<rhi::IBuffer> m_index_buffer{};
        };

        struct BagRangeRecord {
            BagBucketId m_bucket{};
            TriangleBagRange m_range{};
            // Composed identity: full-generation handle minted at upload.
            // Lookups verify this alongside the container generation, so a
            // wrapping 8-bit seed can never alias a recycled slot.
            SparseHandle m_identity{};
        };

        [[nodiscard]] Expected<TriangleBagHandle> uploadBytes_(
            std::span<const std::byte> vert_bytes, u64 stride, std::span<const u32> idx, i32 base);

        [[nodiscard]] static hash_t layoutKey_(u64 stride) noexcept;

        [[nodiscard]] const BagRangeRecord *findRecord_(SparseHandle key) const noexcept;

        // Render-thread affinity: initialize captures the calling thread;
        // every mutating call fails closed (operation_not_permitted) elsewhere.
        // A plain error return (no PPR_ASSERT): Assertion::onFailure throws,
        // which cannot cross the noexcept mutators and would make the contract
        // untestable; determinism is the enforcement.
        [[nodiscard]] bool onRenderThread_() const noexcept;

        // Device-loss helper (Phase 6 A3): release GPU buffers, retain CPU
        // records and counters. Called by notifyDeviceLost; shutdown clears
        // the rest. Render-thread confined like every other mutator.
        void dropGpuObjects_() noexcept;

        bool m_initialized = false;
        CacheResidency m_residency = CacheResidency::uninitialized;
        rhi::IDevice *m_device = nullptr;
        std::thread::id m_owner{};
        FlatMap<hash_t, BagBucketId> m_layout_to_bucket{};
        // Append-only: bucket indices (BagBucketId) stay stable for the cache
        // lifetime; buckets are only destroyed at shutdown.
        Array<BagBucket> m_buckets{};
        SparseVector<BagRangeRecord> m_ranges{};
    };

    // Dedup FlatMap<DedupKey, TextureHandle>, refcount, pin-while-held.
    // DedupKey = {contentHash, width/height/format/mips/dimension}; lookup is
    // hash-bucket + FULL byte equality on pinned bytes (hash alone NEVER
    // decides). Sampler state is NEVER part of the dedup key: the pass owns
    // ONE shared sampler.
    class BindlessTextureCache {
    public:
        [[nodiscard]] std::error_code initialize(
            rhi::IDevice &device, u32 texture_budget, rhi::DescriptorHandle fallback_descriptor);

        [[nodiscard]] std::error_code shutdown();

        // Residency (Phase 6 A3): see TriangleBagCache.
        [[nodiscard]] CacheResidency residency() const noexcept;

        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        // NO SamplerDesc: the pass owns one shared sampler.
        [[nodiscard]] Expected<TextureHandle> upload(const image::ImageAsset &asset);

        [[nodiscard]] std::error_code release(TextureHandle handle) noexcept;

        [[nodiscard]] Expected<TextureBindlessIndex> residentIndex(TextureHandle handle) const noexcept;

        [[nodiscard]] rhi::ITextureView *view(TextureHandle handle) const noexcept;

        // §6 texture heap: slot 0 is fallback-white; real texture slots begin at 1.
        // The pass binds this container once per render invocation.
        [[nodiscard]] rhi::IBuffer *descriptorBuffer() const noexcept;

        // Usage telemetry (slots are bump-allocated; release never reuses).
        [[nodiscard]] u32 textureUsed() const noexcept;

        [[nodiscard]] u32 textureBudget() const noexcept;

        [[nodiscard]] u64 entryCount() const noexcept;

    private:
        struct DedupKey {
            hash_t m_hash{};
            u32 m_width = 0u;
            u32 m_height = 0u;
            image::NativeImageFormat m_format = image::NativeImageFormat::rgba8_linear;
            u32 m_mips = 0u;

            // Hand-written (no defaulted comparisons: MSVC module ICE family).
            [[nodiscard]] bool operator==(const DedupKey &other) const noexcept {
                return m_hash == other.m_hash and
                    m_width == other.m_width and
                    m_height == other.m_height and
                    m_format == other.m_format and
                    m_mips == other.m_mips;
            }

            [[nodiscard]] bool operator<(const DedupKey &other) const noexcept {
                if (m_hash != other.m_hash) {
                    return m_hash.m_value < other.m_hash.m_value;
                }
                if (m_width != other.m_width) {
                    return m_width < other.m_width;
                }
                if (m_height != other.m_height) {
                    return m_height < other.m_height;
                }
                if (m_format != other.m_format) {
                    return enumOrd(m_format) < enumOrd(other.m_format);
                }
                return m_mips < other.m_mips;
            }
        };

        struct TextureEntry {
            rhi::ComPtr<rhi::ITexture> m_texture{};
            rhi::ComPtr<rhi::ITextureView> m_view{};
            rhi::DescriptorHandle m_descriptor{};
            TextureBindlessIndex m_slot{};
            mem::SharedBuffer m_pinned{};
            u32 m_refcount = 0u;
            DedupKey m_key{};
            // Composed identity (Phase 6 A3): full-generation handle minted
            // at upload; verified alongside the container generation.
            SparseHandle m_identity{};
        };

        [[nodiscard]] const TextureEntry *findEntry_(SparseHandle key) const noexcept;

        [[nodiscard]] static DedupKey dedupKey_(const image::ImageAsset &asset) noexcept;

        [[nodiscard]] static bool bytesEqual_(const mem::SharedBuffer &pinned, const image::ImageAsset &asset) noexcept;

        [[nodiscard]] static Expected<rhi::Format> uploadFormat_(image::NativeImageFormat format) noexcept;

        [[nodiscard]] bool onRenderThread_() const noexcept;

        // Device-loss helper (Phase 6 A3): release GPU objects, retain CPU
        // records and counters. Called by notifyDeviceLost; shutdown clears
        // the rest. Render-thread confined like every other mutator.
        void dropGpuObjects_() noexcept;

        bool m_initialized = false;
        CacheResidency m_residency = CacheResidency::uninitialized;
        rhi::IDevice *m_device = nullptr;
        std::thread::id m_owner{};
        rhi::ComPtr<rhi::IBuffer> m_descriptor_buffer{};
        u32 m_texture_budget = 0u;
        u32 m_next_slot = 1u;
        FlatMap<DedupKey, TextureHandle> m_dedup{};
        SparseVector<TextureEntry> m_entries{};
    };

    // Pass-owned; stable slots into StructuredBuffer<GpuMaterial>. The caller
    // resolves ImageAssetId→TextureHandle first, then residentIndex →
    // GpuTextureRefs, and passes the slots in. The GPU buffer is allocated
    // once at capacity (512×80 B); pack/release rewrite one slot in place and
    // slots never reuse (single-load contract, same as the bag buckets).
    class BindlessMaterialCache {
    public:
        // shared_sampler is a non-owning view: the pass creates and owns ONE
        // shared ISampler and destroys it AFTER this cache shuts down.
        [[nodiscard]] std::error_code initialize(rhi::IDevice &device, rhi::ISampler *shared_sampler);

        [[nodiscard]] std::error_code shutdown();

        // Residency (Phase 6 A3): see TriangleBagCache.
        [[nodiscard]] CacheResidency residency() const noexcept;

        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        [[nodiscard]] Expected<MaterialHandle> pack(
            const mesh::MaterialAsset &asset, GpuTextureRefs resolved);

        [[nodiscard]] std::error_code release(MaterialHandle handle) noexcept;

        [[nodiscard]] Expected<u32> materialIndex(MaterialHandle handle) const noexcept;

        // §6 encode: resolved GpuMaterial for slot/texture + variant lookup.
        [[nodiscard]] Expected<GpuMaterial> material(MaterialHandle handle) const noexcept;

        [[nodiscard]] rhi::IBuffer *materialBuffer() const noexcept;

        // Usage telemetry (slots are bump-allocated; release never reuses).
        [[nodiscard]] u32 materialUsed() const noexcept;

        [[nodiscard]] u32 materialCapacity() const noexcept;

        [[nodiscard]] u64 entryCount() const noexcept;

    private:
        struct MaterialEntry {
            GpuMaterial m_gpu{};
            u32 m_slot = 0u;
            // Composed identity (Phase 6 A3): full-generation handle minted
            // at pack; verified alongside the container generation.
            SparseHandle m_identity{};
        };

        [[nodiscard]] const MaterialEntry *findEntry_(SparseHandle key) const noexcept;

        [[nodiscard]] std::error_code writeSlot_(u32 slot, const GpuMaterial &gpu);

        [[nodiscard]] bool onRenderThread_() const noexcept;

        // Device-loss helper (Phase 6 A3): release GPU objects, retain CPU
        // records and counters. Called by notifyDeviceLost; shutdown clears
        // the rest. Render-thread confined like every other mutator.
        void dropGpuObjects_() noexcept;

        bool m_initialized = false;
        CacheResidency m_residency = CacheResidency::uninitialized;
        rhi::IDevice *m_device = nullptr;
        std::thread::id m_owner{};
        rhi::ISampler *m_shared_sampler = nullptr;
        u32 m_next_slot = 0u;
        SparseVector<MaterialEntry> m_entries{};
        rhi::ComPtr<rhi::IBuffer> m_material_buffer{};
    };

    // Phase 7 slot dependency / fence-retirement API (SPECIFICATION ONLY —
    // no implementation in this phase; shapes the residency/streaming work).
    //
    // Motivation: today's release() retires the CPU record immediately while
    // GPU bytes/slots never reuse (bump-allocated). Phase 7 introduces
    // fence-delayed free lists, so a released slot must stay unrecycled until
    // the GPU has drained every submission that references it.
    //
    // Planned surface (names indicative, exact spelling decided in Phase 7):
    // - `SlotVersion` (u32, monotonic per slot): bumped on every recycle so a
    //   GPU-side slot index alone never names a unique allocation; composed
    //   with the slot into the container rewrite (see below).
    // - `FenceValue` (u64, RHI fence counter): captured at release time from
    //   the submitting queue; a slot is recyclable only once the fence passes
    //   that value (fence-delayed free list per cache).
    // - `release(handle, FenceValue retired_at)` overload: moves the record to
    //   a pending-retirement list instead of erasing it; lookups keep failing
    //   closed from the moment of release (CPU identity retires eagerly, GPU
    //   storage retires lazily).
    // - `reclaimCompleted(FenceValue completed)`: returns pending slots at or
    //   below the completed fence to the free list; called from the
    //   render-thread boundary after wait/poll, never from inside encode.
    // - Container rewrite before publication: the descriptor/material buffer
    //   rewrite for a recycled slot happens on the render thread BEFORE the
    //   slot is handed out again, so no in-flight draw ever observes a torn
    //   slot. Slot 0 stays pinned to the white fallback across all of this.
    // - Device-loss interaction: notifyDeviceLost discards pending-retirement
    //   lists outright (GPU objects are gone; nothing is in flight worth
    //   waiting for); shutdown after device loss needs no fence polling.
    // Non-goals: cross-queue dependencies, timeline-semaphore abstraction in
    // the caches (that lives behind the RHI fence seam), CPU-backing spill.
}
