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

        struct SubmittedInstance {
            TriangleBagHandle m_bag{};
            MaterialHandle m_material{};
            float4x4 m_model = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
        };

        // Per-instance GPU payload: today's SubmittedInstance (model +
        // bag/material handles) with the handles resolved to draw scalars
        // (vb_offset/ib_start/index_count/base_vertex) + the resolved material
        // slot. Lives in a StructuredBuffer<g_payloads>; the vertex entry
        // indexes it at g_payload_base + SV_InstanceID, where g_payload_base
        // is bound per draw group.
        // Plain-float-array POD (P1 vertex verdict): mango float4x4 carries
        // user-provided copy/dtor and can never be trivially copyable, so the
        // model rides float[16] with a component copy at the staging boundary.
        // Contract: m_model is a rigid transform or a uniform scale — the
        // shader rotates normals with the model matrix, which is exact only
        // in that case.
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

        // Draw planning: each group has one fixed geometry range and pipeline
        // variant, while its contiguous payload interval carries the
        // per-instance model, material, and resolved range. The group keys on
        // its resolved buffer pair, so geometry and payloads can never come
        // from different bags.
        struct DrawGroup {
            TrianglePipelineVariant m_variant{};
            rhi::IBuffer *m_vertex_buffer = nullptr;
            rhi::IBuffer *m_index_buffer = nullptr;
            u32 m_vb_offset = 0u;
            u32 m_ib_start = 0u;
            u32 m_count = 0u;
            i32 m_base_vertex = 0;
            u32 m_first_payload = 0u;
            u32 m_instance_count = 0u;
            Array<u32> m_source_indices{};
        };

        struct DrawPlan {
            Array<DrawGroup> m_groups{};
            u32 m_payload_count = 0u;
        };

        // ------------------------------------------------------------------
        // scene upload products
        // ------------------------------------------------------------------

        // Pure CPU planner: groups the per-instance draws that share a pipeline
        // variant, a resolved buffer pair, and an identical geometry range,
        // then assigns each group a contiguous payload interval. Renders
        // nothing, so it is unit-testable without a device.
        [[nodiscard]] static Expected<DrawPlan> planDraws(
            std::span<const TrianglePipelineVariant> variants,
            std::span<rhi::IBuffer *const> vertex_buffers,
            std::span<rhi::IBuffer *const> index_buffers,
            std::span<const TriangleBagRange> ranges);

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

        // The one draw path: resolve → plan → upload payloads → one
        // drawInstanced per group. Failure is fail-closed but NOT an atomic
        // frame: resolve and payload upload are all-or-nothing, while encoding
        // stops at the first group that cannot be encoded and the groups
        // already encoded into the pass stand, so the command list may hold a
        // prefix of the plan. uploadScene has its own all-or-none rollback.
        [[nodiscard]] std::error_code render(const DrawContext &draw_context);

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
        // resolve, plan, and encode
        // ------------------------------------------------------------------

        // One submitted instance with its bag and material handles resolved to
        // drawable state: the GPU payload plus the geometry it draws from. The
        // buffers are raw and non-owning views of the bag cache's buckets.
        struct ResolvedInstance {
            InstancePayload m_payload{};
            TrianglePipelineVariant m_variant{};
            u32 m_count = 0u;
            rhi::IBuffer *m_vertex_buffer = nullptr;
            rhi::IBuffer *m_index_buffer = nullptr;
        };

        std::error_code createInvariantRenderState_(rhi::IDevice &device);

        std::error_code createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);

        // Pipeline-variant cache: target signature + twosided cull +
        // opaque/mask alpha. Blend is REJECTED with function_not_supported.
        [[nodiscard]] Expected<rhi::IRenderPipeline *> pipelineFor_(
            rhi::IDevice &device,
            const RenderPipelineSignature &signature,
            TrianglePipelineVariant variant);

        [[nodiscard]] std::error_code uploadFrameConstants_(rhi::ShaderCursor &frame_cursor);

        // Fail-closed resolve of every submitted instance, all-or-nothing: one
        // stale handle or stride mismatch fails the whole frame before any
        // encode. Zero-count bags resolve but are dropped by the caller.
        [[nodiscard]] Expected<Array<ResolvedInstance, mem::ScratchPad> > resolveInstances_() const;

        [[nodiscard]] Expected<ResolvedInstance> resolveOne_(const SubmittedInstance &instance) const;

        // Project resolved instances onto the planner's parallel spans.
        [[nodiscard]] Expected<DrawPlan> buildPlan_(
            const Array<ResolvedInstance, mem::ScratchPad> &resolved_instances);

        // All-or-nothing payload upload: the whole compacted interval or fail.
        [[nodiscard]] std::error_code uploadPayloads_(
            rhi::IDevice &device,
            const Array<ResolvedInstance, mem::ScratchPad> &resolved_instances,
            const DrawPlan &plan);

        [[nodiscard]] std::error_code encodeGroup_(
            const DrawContext &draw_context,
            const Array<ResolvedInstance, mem::ScratchPad> &resolved_instances,
            const DrawGroup &group);

        [[nodiscard]] std::error_code ensureDirectPayloads_(rhi::IDevice &device, u32 payload_count);

        // ------------------------------------------------------------------
        // pass-owned GPU resources
        // ------------------------------------------------------------------

        // The one render program: mesh_bindless's payload-indexed vertex entry
        // plus the fragment entry. The pipeline-variant cache is keyed on the
        // render pipeline signature, so a signature change drops every variant.
        rhi::ComPtr<rhi::IShaderProgram> m_shader_program{};
        std::optional<RenderPipelineKey> m_render_pipeline_key;
        FlatMap<TrianglePipelineVariant, rhi::ComPtr<rhi::IRenderPipeline> > m_variant_pipelines{};

        // Upload payload buffer at InstancePayload (96 B) stride, rewritten
        // every frame by mapBuffer and grown on demand. Pass-owned so it
        // outlives the encoded draws; released in shutdown before waitOnHost.
        rhi::ComPtr<rhi::IBuffer> m_direct_payloads{};
        u64 m_direct_payload_capacity = 0u;

        // Pass-owned GPU caches + the ONE shared sampler. m_shared_sampler is a
        // raw non-owning view lent to BindlessMaterialCache, so the teardown
        // ORDER below is the ownership contract between the two members:
        // m_material_cache must be shut down (which nulls its view) BEFORE
        // m_shared_sampler is released. Both shutdown and notifyDeviceLost
        // perform exactly that order; the initialize rollback does too.
        // m_fallback_* is the 1x1 white texture bound to kNoTexture slots in
        // the TEXTURE heap only (heap slot 0); material slots start at 0 and
        // are real materials, so they have no fallback entry.
        TriangleBagCache m_bag_cache{};
        BindlessTextureCache m_texture_cache{};
        BindlessMaterialCache m_material_cache{};
        rhi::ComPtr<rhi::ISampler> m_shared_sampler{};
        rhi::DescriptorHandle m_sampler_handle{};
        rhi::ComPtr<rhi::ITexture> m_fallback_texture{};
        rhi::ComPtr<rhi::ITextureView> m_fallback_view{};
        rhi::DescriptorHandle m_fallback_descriptor{};
        bool m_caches_ready = false;

        Array<SubmittedInstance> m_submitted_instances{};

        // Live-scene receipts: one nonce per successful
        // uploadScene; releaseScene consumes it before touching the caches
        // so a double release fails closed without decrementing the
        // refcounted texture entry a surviving scene still holds.
        u64 m_next_scene_receipt = 1u;
        FlatSet<u64> m_live_scenes{};

        CameraSnapshot m_camera_view;
    };
}
