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
    struct TextureHandleTag final {
    };

    using TextureHandle = Numeric<SparseKeyId, TextureHandleTag>;

    struct MaterialHandleTag final {
    };

    using MaterialHandle = Numeric<SparseKeyId, MaterialHandleTag>;

    struct TriangleBagHandleTag final {
    };

    using TriangleBagHandle = Numeric<SparseKeyId, TriangleBagHandleTag>;

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
    static_assert(sizeof(TextureHandle) == 4u);
    static_assert(std::is_standard_layout_v<MaterialHandle>);
    static_assert(sizeof(MaterialHandle) == 4u);
    static_assert(std::is_standard_layout_v<TriangleBagHandle>);
    static_assert(sizeof(TriangleBagHandle) == 4u);

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
        };

        [[nodiscard]] Expected<TriangleBagHandle> uploadBytes_(
            std::span<const std::byte> vert_bytes, u64 stride, std::span<const u32> idx, i32 base);

        [[nodiscard]] static hash_t layoutKey_(u64 stride) noexcept;

        [[nodiscard]] const BagRangeRecord *findRecord_(SparseKeyId key) const noexcept;

        bool m_initialized = false;
        rhi::IDevice *m_device = nullptr;
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
        [[nodiscard]] std::error_code initialize(rhi::IDevice &device, u32 texture_budget);

        [[nodiscard]] std::error_code shutdown();

        // NO SamplerDesc: the pass owns one shared sampler.
        [[nodiscard]] Expected<TextureHandle> upload(const image::ImageAsset &asset);

        [[nodiscard]] std::error_code release(TextureHandle handle) noexcept;

        [[nodiscard]] Expected<TextureBindlessIndex> residentIndex(TextureHandle handle) const noexcept;

        [[nodiscard]] rhi::ITextureView *view(TextureHandle handle) const noexcept;

        // §6 encode: slot → descriptor for per-instance setDescriptorHandle.
        // kNoTexture never matches; stale slots fail with invalid_argument.
        [[nodiscard]] Expected<rhi::DescriptorHandle> descriptorForSlot(TextureBindlessIndex slot) const noexcept;

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
        };

        [[nodiscard]] const TextureEntry *findEntry_(SparseKeyId key) const noexcept;

        [[nodiscard]] static DedupKey dedupKey_(const image::ImageAsset &asset) noexcept;

        [[nodiscard]] static bool bytesEqual_(const mem::SharedBuffer &pinned, const image::ImageAsset &asset) noexcept;

        [[nodiscard]] static Expected<rhi::Format> uploadFormat_(image::NativeImageFormat format) noexcept;

        bool m_initialized = false;
        rhi::IDevice *m_device = nullptr;
        u32 m_texture_budget = 0u;
        u32 m_next_slot = 0u;
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

        [[nodiscard]] Expected<MaterialHandle> pack(
            const mesh::MaterialAsset &asset, GpuTextureRefs resolved);

        [[nodiscard]] std::error_code release(MaterialHandle handle) noexcept;

        [[nodiscard]] Expected<u32> materialIndex(MaterialHandle handle) const noexcept;

        // §6 encode: resolved GpuMaterial for slot/texture + variant lookup.
        [[nodiscard]] Expected<GpuMaterial> material(MaterialHandle handle) const noexcept;

        [[nodiscard]] rhi::IBuffer *materialBuffer() const noexcept;

    private:
        struct MaterialEntry {
            GpuMaterial m_gpu{};
            u32 m_slot = 0u;
        };

        [[nodiscard]] const MaterialEntry *findEntry_(SparseKeyId key) const noexcept;

        [[nodiscard]] std::error_code writeSlot_(u32 slot, const GpuMaterial &gpu);

        bool m_initialized = false;
        rhi::IDevice *m_device = nullptr;
        rhi::ISampler *m_shared_sampler = nullptr;
        u32 m_next_slot = 0u;
        SparseVector<MaterialEntry> m_entries{};
        rhi::ComPtr<rhi::IBuffer> m_material_buffer{};
    };
}
