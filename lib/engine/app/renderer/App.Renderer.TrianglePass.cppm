module;
#include "pP/Macros.h"
export module engine.app:renderer.triangle_pass;

import :renderer.types;
import :renderer.gpu_caches;
import :scene.camera;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import engine.image;
import engine.mesh;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // triangle pass: owns the triangle shader/pipeline/resources
    // ------------------------------------------------------------------
    // The Renderer stays content-free; scene content lives here. Viewport
    // geometry always derives from SceneView::m_render_view, camera data
    // always from the snapshot — mutable Camera is never consulted.

    class TrianglePass {
    public:
        // ------------------------------------------------------------------
        // frame data
        // ------------------------------------------------------------------

        struct FrameConstants {
            float4x4 m_view = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_view_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_inverse_view_projection = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            // Zero default; uploadFrameConstants_ promotes origin to a point via float4{origin, 1}.
            float4 m_camera_position = float4{0, 0, 0, 0};
            float4 m_viewport_size = float4{0, 0, 0, 0};
        };

        static_assert(sizeof(FrameConstants) == 288, "FrameConstants must match the HLSL layout (4 * float4x4 + 2 * float4)");

        struct Instance {
            TriangleBagHandle m_bag{};
            MaterialHandle m_material{};
            float4x4 m_model = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
        };

        // Per-instance GPU payload: today's Instance
        // (model + bag/material handles) with the handles resolved to draw
        // scalars (vb_offset/ib_start/index_count/base_vertex) + the resolved
        // material slot. Lives in a StructuredBuffer<g_payloads>; the
        // indirect vertex entry indexes it with SV_InstanceID. L2's
        // PrepareInstances kernel publishes the same layout; the CPU-staged
        // L1 path writes it via Upload scratch in renderIndirect.
        // Plain-float-array POD (P1 vertex verdict): mango float4x4 carries
        // user-provided copy/dtor and can never be trivially copyable, so the
        // model rides float[16] with a component copy at the staging boundary.
        struct alignas(16) InstancePayload {
            float m_model[16]{};
            u32 m_vb_offset = 0u;
            u32 m_ib_start = 0u;
            u32 m_index_count = 0u;
            u32 m_material = 0u;
            i32 m_base_vertex = 0;
            u32 m_pad[3]{};
        };

        static_assert(std::is_trivially_copyable_v<InstancePayload>);
        static_assert(std::is_standard_layout_v<InstancePayload>);
        static_assert(sizeof(InstancePayload) == 96u);
        static_assert(alignof(InstancePayload) == 16u);
        static_assert(PPR_OFFSETOF(InstancePayload, m_model) == 0u);
        static_assert(PPR_OFFSETOF(InstancePayload, m_vb_offset) == 64u);
        static_assert(PPR_OFFSETOF(InstancePayload, m_material) == 76u);

        // ------------------------------------------------------------------
        // indirect draw contracts
        // ------------------------------------------------------------------

        // One indirect range per non-empty variant bucket: at most 4 buckets
        // (opaque/mask × cull), each drawn with one drawIndirect call over its
        // contiguous args slice. Empty buckets are never emitted.
        struct IndirectBucket {
            TrianglePipelineVariant m_variant{};
            BagBucketId m_bag_bucket{};
            u32 m_first_arg = 0u;
            u32 m_arg_count = 0u;
        };

        static constexpr u32 kMaxIndirectBuckets{4u};

        // Compute-publish ring: 3 payload/args/counter slots
        // retiring by completed fence value. SlotVersion/FenceValue semantics
        // (Phase 6 A3) apply to THESE ring slots only — texture heap slot 0
        // stays pinned to the white fallback and material slots never recycle
        // (single-load contract); the ring never touches them.
        static constexpr u32 kIndirectRingFrames{3u};
        static constexpr u32 kIndirectRingCapacity{256u};
        static constexpr u64 kIndirectCounterBytes{16u};
        static constexpr u32 kInvalidIndirectSlot{0xFFFFFFFFu};

        // CPU-staged indirect plan: one IndirectDrawArguments record per
        // (prim, instance) plus a u32 draw-count header (m_total_count; L2's
        // kernel publishes it from the GPU via the count buffer). maxCount is
        // clamped to the payload capacity; count-0 prims emit no record and an
        // empty plan draws nothing.
        struct IndirectPlan {
            Array<rhi::IndirectDrawArguments> m_args{};
            Array<IndirectBucket> m_buckets{};
            u32 m_total_count = 0u;
        };

        // ------------------------------------------------------------------
        // scene upload products
        // ------------------------------------------------------------------

        [[nodiscard]] static Expected<IndirectPlan> planIndirectDraws(
            std::span<const TrianglePipelineVariant> variants,
            std::span<const BagBucketId> bag_buckets,
            std::span<const u32> vertex_counts,
            u32 payload_capacity);

        // §7 uploadScene product: one bag per (mesh, prim) — full mesh verts
        // plus the prim index slice, Mango prim base riding the range —
        // materials parallel to SceneAsset::m_mats, textures parallel to the
        // decoded image span.
        struct UploadedPrimitive {
            TriangleBagHandle m_bag{};
            MaterialHandle m_material{};
        };

        struct UploadedScene {
            Array<UploadedPrimitive> m_prims{};
            Array<TextureHandle> m_textures{};
            Array<MaterialHandle> m_materials{};
            // Scene receipt: issued by uploadScene, consumed by
            // releaseScene. Copies share the receipt — the first release wins
            // and repeats fail closed (invalid_argument) even when a
            // refcounted texture entry outlives the releasing scene.
            u64 m_receipt = 0u;
        };

        // ------------------------------------------------------------------
        // lifecycle and rendering
        // ------------------------------------------------------------------

        [[nodiscard]] std::error_code initialize(IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);

        [[nodiscard]] std::error_code update(TimeSpan dt, const CameraSnapshot &camera_view);

        [[nodiscard]] std::error_code render(const DrawContext &draw_context);

        // Phase 7 L1 CPU-staged indirect path: stages submitInstance data
        // into Upload payload/args scratch, then one drawIndirect per
        // non-empty variant bucket. Same fail-closed validation as the
        // direct path (stale handles, stride mismatch → error, no partial
        // draw). Pixel-identical to render() for the same instance list.
        [[nodiscard]] std::error_code renderIndirect(const DrawContext &draw_context);

        // Phase 7 L2b compute-publish indirect path (plan §6): stages
        // submitInstance data into the current ring slot's Upload scratch,
        // dispatches prepareInstancesMain (instanceCount,1,1) to publish
        // device-local payloads + appended args records, then
        // renderIndirectCompute draws the published slot. The UAV counter is
        // host-cleared before dispatch; readPublishedCount reads it back.
        // Over-count clamps to kIndirectRingCapacity and returns
        // invalid_argument; empty staging publishes nothing and draws
        // nothing (both return success).
        [[nodiscard]] std::error_code publishIndirectCompute(rhi::IDevice &device);

        [[nodiscard]] Expected<u32> readPublishedCount(rhi::IDevice &device);

        [[nodiscard]] std::error_code renderIndirectCompute(const DrawContext &draw_context);

        // Last publish probes (tests only): CPU-side staged count, ring slot
        // index (kInvalidIndirectSlot when nothing published), per-slot
        // version/fence values (0 = never acquired since init/device-loss).
        [[nodiscard]] u32 publishedInstanceCount() const noexcept { return m_published_count; }

        [[nodiscard]] u32 publishedSlot() const noexcept { return m_published_slot; }

        [[nodiscard]] u32 ringSlotVersion(u32 slot) const noexcept;

        [[nodiscard]] u64 ringSlotFence(u32 slot) const noexcept;

        [[nodiscard]] std::error_code shutdown();

        // Device-loss hook (editor lifecycle): fans out
        // notifyDeviceLost to the three caches in shutdown order, drops
        // per-frame instances, and parks uploads (m_caches_ready = false) so
        // GPU-touching calls fail closed while releaseScene still drains CPU
        // records. Restart is an explicit shutdown + initialize pair.
        [[nodiscard]] std::error_code notifyDeviceLost() noexcept;

        // ------------------------------------------------------------------
        // asset residency and instance submission
        // ------------------------------------------------------------------

        // Narrow asset APIs: uploadMesh/uploadTexture/packMaterial
        // acquire cache entries (partial rollback in uploadScene, P3);
        // submitInstance validates handles and snapshots {bag, material, model};
        // clearInstances drops the per-frame list (GPU entries stay cached).
        [[nodiscard]] Expected<TriangleBagHandle> uploadMesh(
            std::span<const mesh::StaticMeshVertex> verts, std::span<const u32> idx);

        [[nodiscard]] Expected<TextureHandle> uploadTexture(const image::ImageAsset &asset);

        [[nodiscard]] Expected<MaterialHandle> packMaterial(
            const mesh::MaterialAsset &asset, std::span<const TextureHandle> resolved);

        [[nodiscard]] std::error_code submitInstance(
            TriangleBagHandle bag, MaterialHandle material, const float4x4 &model);

        void clearInstances() noexcept;

        // §7 scene upload: bag per (mesh, prim) → texture per image (dedup) →
        // ORM composite per material (R=occlusion-or-1, G=rough, B=metal,
        // fail-closed on size/format mismatch) → pack per material.
        // Partial rollback in reverse order; images[i] joins SceneAsset::m_images[i].
        [[nodiscard]] Expected<UploadedScene> uploadScene(
            const mesh::SceneAsset &scene, std::span<const image::ImageAsset> images);

        [[nodiscard]] std::error_code releaseScene(const UploadedScene &uploaded) noexcept;

        [[nodiscard]] TriangleBagCache &bagCache() noexcept { return m_bag_cache; }
        [[nodiscard]] BindlessTextureCache &textureCache() noexcept { return m_texture_cache; }
        [[nodiscard]] BindlessMaterialCache &materialCache() noexcept { return m_material_cache; }

    private:
        // ------------------------------------------------------------------
        // pipeline and direct encoding
        // ------------------------------------------------------------------

        std::error_code createInvariantRenderState_(rhi::IDevice &device);

        std::error_code createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);

        // Pipeline-variant cache: target signature + twosided cull +
        // opaque/mask alpha. Blend is REJECTED with function_not_supported.
        [[nodiscard]] Expected<rhi::IRenderPipeline *> pipelineFor_(
            rhi::IDevice &device,
            const RenderPipelineSignature &signature,
            TrianglePipelineVariant variant);

        [[nodiscard]] std::error_code uploadFrameConstants_(rhi::ShaderCursor &frame_cursor);

        [[nodiscard]] std::error_code encodeInstance_(const DrawContext &draw_context, const Instance &instance);

        [[nodiscard]] Expected<rhi::IRenderPipeline *> pipelineForImpl_(
            rhi::IDevice &device,
            const RenderPipelineSignature &signature,
            TrianglePipelineVariant variant,
            rhi::IShaderProgram *program,
            FlatMap<TrianglePipelineVariant, rhi::ComPtr<rhi::IRenderPipeline> > &pipelines);

        [[nodiscard]] Expected<rhi::IRenderPipeline *> pipelineForIndirect_(
            rhi::IDevice &device,
            const RenderPipelineSignature &signature,
            TrianglePipelineVariant variant);

        // ------------------------------------------------------------------
        // indirect draw state
        // ------------------------------------------------------------------

        [[nodiscard]] std::error_code ensureIndirectScratch_(rhi::IDevice &device, u32 payload_count);

        [[nodiscard]] std::error_code encodeIndirectBucket_(
            const DrawContext &draw_context,
            const IndirectBucket &bucket,
            u32 payload_count);

        // L2b compute-publish staging (shared resolve for the CPU L1 path and
        // the compute L2b path — identical fail-closed validation, no partial
        // staging in either).
        struct StagedDraw {
            InstancePayload m_payload{};
            TrianglePipelineVariant m_variant{};
            BagBucketId m_bag_bucket{};
            u32 m_count = 0u;
            rhi::IBuffer *m_vertex_buffer = nullptr;
            rhi::IBuffer *m_index_buffer = nullptr;
        };

        [[nodiscard]] Expected<Array<StagedDraw> > stageDraws_() const;

        // One published compute bucket: draw geometry resolved at publish
        // time (never re-scanned from m_instances at draw — the instance list
        // may change between publish and draw).
        struct PublishedBucket {
            TrianglePipelineVariant m_variant{};
            BagBucketId m_bag_bucket{};
            rhi::IBuffer *m_vertex_buffer = nullptr;
            rhi::IBuffer *m_index_buffer = nullptr;
            u32 m_first_arg = 0u;
            u32 m_arg_count = 0u;
        };

        // One ring slot: per-publish Upload scratch + device-local payloads,
        // args, and UAV counter. m_version bumps on every acquire
        // (SlotVersion); m_fence_value is the publishing submit's signal
        // value (FenceValue) — the slot is recyclable once the completed
        // fence passes it. Slot 0 of THIS ring rotates normally; the pinned
        // slot 0 is the texture heap's white fallback, never rewritten here.
        struct IndirectRingSlot {
            rhi::ComPtr<rhi::IBuffer> m_scratch{};
            rhi::ComPtr<rhi::IBuffer> m_payloads{};
            rhi::ComPtr<rhi::IBuffer> m_args{};
            rhi::ComPtr<rhi::IBuffer> m_counter{};
            u32 m_version = 0u;
            u64 m_fence_value = 0u;
            bool m_busy = false;
        };

        std::error_code ensureComputeState_(rhi::IDevice &device);

        [[nodiscard]] Expected<u32> acquireRingSlot_(u64 completed);

        void clearPublished_() noexcept;

        void clearRing_() noexcept;

        [[nodiscard]] std::error_code encodeIndirectComputeBucket_(
            const DrawContext &draw_context,
            const PublishedBucket &bucket);

        // ------------------------------------------------------------------
        // pass-owned GPU resources
        // ------------------------------------------------------------------

        rhi::ComPtr<rhi::IShaderProgram> m_shader_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_indirect_program{};
        rhi::ComPtr<rhi::IShaderProgram> m_compute_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_compute_pipeline{};

        rhi::ComPtr<rhi::IRenderPipeline> m_render_pipeline{};
        std::optional<RenderPipelineKey> m_render_pipeline_key;
        FlatMap<TrianglePipelineVariant, rhi::ComPtr<rhi::IRenderPipeline> > m_variant_pipelines{};
        FlatMap<TrianglePipelineVariant, rhi::ComPtr<rhi::IRenderPipeline> > m_indirect_pipelines{};

        // L1 CPU-staged indirect scratch (Upload ring of one): payloads at
        // 96 B stride + args at 16 B stride, rewritten every renderIndirect
        // via mapBuffer. Pass-owned like the caches, so the buffers outlive
        // the encoded draws; released in shutdown before waitOnHost.
        rhi::ComPtr<rhi::IBuffer> m_indirect_payloads{};
        u64 m_indirect_payload_capacity = 0u;
        rhi::ComPtr<rhi::IBuffer> m_indirect_args{};
        u64 m_indirect_arg_capacity = 0u;

        // L2b compute-publish ring: per-slot scratch/payloads/args/counter,
        // rotating per publish; m_publish_fence orders publishes (signal per
        // submit, retire by completed value). m_published_* is the last
        // publish consumed by renderIndirectCompute.
        IndirectRingSlot m_ring[kIndirectRingFrames]{};
        rhi::ComPtr<rhi::IFence> m_publish_fence{};
        u64 m_next_fence_value = 1u;
        Array<PublishedBucket> m_published_buckets{};
        u32 m_published_count = 0u;
        u32 m_published_slot = kInvalidIndirectSlot;

        // Pass-owned GPU caches + ONE shared sampler lent to the material
        // cache (non-owning view; destroyed AFTER the material cache).
        // m_fallback_* is the 1x1 white texture bound to kNoTexture slots so
        // every scalar handle uniform stays valid; the shader takes the factor
        // path for those slots.
        TriangleBagCache m_bag_cache{};
        BindlessTextureCache m_texture_cache{};
        BindlessMaterialCache m_material_cache{};
        rhi::ComPtr<rhi::ISampler> m_shared_sampler{};
        rhi::DescriptorHandle m_sampler_handle{};
        rhi::ComPtr<rhi::ITexture> m_fallback_texture{};
        rhi::ComPtr<rhi::ITextureView> m_fallback_view{};
        rhi::DescriptorHandle m_fallback_descriptor{};
        bool m_caches_ready = false;

        Array<Instance> m_instances{};

        // Live-scene receipts: one nonce per successful
        // uploadScene; releaseScene consumes it before touching the caches
        // so a double release fails closed without decrementing the
        // refcounted texture entry a surviving scene still holds.
        u64 m_next_scene_receipt = 1u;
        FlatSet<u64> m_live_scenes{};

        CameraSnapshot m_camera_view;
    };
}
