module;
#include "pP/Macros.h"
export module engine.app:renderer.triangle_pass;

import :renderer.types;
import :renderer.frame_fence;
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
    // The Renderer stays content-free; scene content lives here. Viewport and
    // scissor travel per-draw on DrawSubmission; camera data comes from
    // CameraSnapshot and a mutable Camera is never consulted.

    class TrianglePass {
    public:
        // ------------------------------------------------------------------
        // frame data
        // ------------------------------------------------------------------

        struct FrameConstants {
            float4x4 m_view = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_projection = float4x4{
                float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_view_projection = float4x4{
                float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            float4x4 m_inverse_view_projection = float4x4{
                float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
            // Zero default; uploadFrameConstants_ promotes origin to a point via float4{origin, 1}.
            float4 m_camera_position = float4{0, 0, 0, 0};
            float4 m_viewport_size = float4{0, 0, 0, 0};
        };

        static_assert(
            sizeof(FrameConstants) == 288, "FrameConstants must match the HLSL layout (4 * float4x4 + 2 * float4)");

        struct SubmittedInstance {
            TriangleBagHandle m_bag{};
            MaterialHandle m_material{};
            float4x4 m_model = float4x4{float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0}, float4{0, 0, 0, 1}};
        };

        // Owned per-frame staging payload, built on the CLIENT lane and
        // applied on the pass's owner lane (encoded pass-affinity: the
        // client never touches pass CPU state directly anymore). Plain
        // values + owned storage only. clearInstances+submitInstance is
        // exactly what the client staged, in order; applied on the owner
        // lane, never from the client.
        struct TriangleStagedFrame {
            Array<SubmittedInstance> m_instances{};
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

        // ------------------------------------------------------------------
        // scene upload products
        // ------------------------------------------------------------------

        // Pure CPU planner: groups the per-instance draws that share a pipeline
        // variant, a resolved buffer pair, and an identical geometry range,
        // then assigns each group a contiguous payload interval. Renders
        // nothing, so it is unit-testable without a device.
        [[nodiscard]] static Expected<DrawPlan> planDraws(std::span<const ResolvedInstance> instances);

        // §7 uploadScene product: one bag per (mesh, prim) — full mesh verts
        // plus the prim index slice, Mango prim base riding the range —
        // materials parallel to SceneAsset::m_mats, textures parallel to the
        // decoded image span. m_mesh_prim_base is the mesh-major join table:
        // one base per mesh, so instance submission shares meshes via
        // base + slot instead of a running cursor.
        struct UploadedPrimitive {
            TriangleBagHandle m_bag{};
            MaterialHandle m_material{};
        };

        struct UploadedScene {
            Array<UploadedPrimitive> m_primitives{};
            Array<TextureHandle> m_textures{};
            Array<MaterialHandle> m_materials{};
            // Mesh-major prim bases parallel to SceneAsset::m_meshes:
            // prims of mesh i occupy m_primitives[m_mesh_prim_base[i] ..]
            // contiguously. Invariant: size == scene mesh count.
            Array<u32> m_mesh_prim_base{};
            // Scene receipt: issued by uploadScene, consumed by
            // releaseScene. Copies share the receipt — the first release wins
            // and repeats fail closed (invalid_argument) even when a
            // refcounted texture entry outlives the releasing scene.
            u64 m_receipt = 0u;
        };

        // §7 instance→prim join product: the (mesh, prim) bag with the
        // instance's material (prim material, or the override when set).
        struct JoinedInstancePrim {
            TriangleBagHandle m_bag{};
            MaterialHandle m_material{};
        };

        // ------------------------------------------------------------------
        // lifecycle and rendering
        // ------------------------------------------------------------------

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);

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

        // P1 fence settlement: records the frame SUBMIT's fence value for
        // the payload-ring slot this frame rotated onto (shared with every
        // variant's per-slot roots: one submission, one slot, one value).
        // Called after a successful submit only; a failed submit keeps the
        // older value, which is conservative and safe.
        void notifyFrameSubmitted(FenceCompletionValue submit_value) noexcept;

        // Gate 1 telemetry: ring-growth waits and per-slot last-use values.
        [[nodiscard]] u64 growthWaitCount() const noexcept {
            return m_growth_wait_count;
        }
        [[nodiscard]] std::array<FenceCompletionValue, 3u> slotCompletionValues() const noexcept {
            return m_payload_slot_values;
        }

        // ------------------------------------------------------------------
        // asset residency and instance submission
        // ------------------------------------------------------------------

        // Narrow asset APIs: uploadMesh/uploadTexture/packMaterial
        // acquire cache entries (partial rollback in uploadScene, P3);
        // submitInstance validates handles and snapshots {bag, material, model};
        // clearInstances drops the per-frame list (GPU entries stay cached).
        [[nodiscard]] Expected<TriangleBagHandle> uploadMesh(
            std::span<const mesh::StaticMeshVertex> vertices, std::span<const u32> indices);

        [[nodiscard]] Expected<TextureHandle> uploadTexture(const image::ImageAsset &asset);

        [[nodiscard]] Expected<MaterialHandle> packMaterial(
            const mesh::MaterialAsset &asset, std::span<const TextureHandle> resolved);

        [[nodiscard]] std::error_code submitInstance(
            TriangleBagHandle bag, MaterialHandle material, const float4x4 &model);

        void clearInstances() noexcept;

        // Applies one owned staging payload on the pass's owner lane:
        // clear then replay the staged instances in order (same validation
        // as submitInstance). A rejected handle leaves the per-frame list
        // cleared, mirroring the old synchronous stage-then-validate edge.
        [[nodiscard]] std::error_code applyStagedFrame(const TriangleStagedFrame &stage);

        // §7 scene upload: bag per (mesh, prim) → texture per image (dedup) →
        // ORM composite per material (R=occlusion-or-1, G=rough, B=metal,
        // fail-closed on size/format mismatch) → pack per material.
        // Partial rollback in reverse order; images[i] joins SceneAsset::m_images[i].
        [[nodiscard]] Expected<UploadedScene> uploadScene(
            const mesh::SceneAsset &scene, std::span<const image::ImageAsset> images);

        // §7 (mesh, prim) → UploadedPrimitive join for shared meshes:
        // uploaded.m_primitives[mesh_base[instance.m_mesh] + prim_slot],
        // with the instance material override applied. Fail-closed
        // (invalid_argument) on mesh/table/slot mismatch. Pure CPU lookup,
        // so it is unit-testable without a device.
        [[nodiscard]] static Expected<JoinedInstancePrim> joinInstancePrim(
            const mesh::SceneAsset &scene, const UploadedScene &uploaded, const mesh::SceneInstance &instance,
            std::size_t prim_slot) noexcept;

        [[nodiscard]] std::error_code releaseScene(const UploadedScene &uploaded) noexcept;

        [[nodiscard]] TriangleBagCache &bagCache() noexcept {
            return m_bag_cache;
        }
        [[nodiscard]] BindlessTextureCache &textureCache() noexcept {
            return m_texture_cache;
        }
        [[nodiscard]] BindlessMaterialCache &materialCache() noexcept {
            return m_material_cache;
        }

    private:
        // ------------------------------------------------------------------
        // resolve, plan, and encode
        // ------------------------------------------------------------------

        // Payload-ring depth: 3 slots > 2 frame credits, so the selected
        // slot is complete without waiting whenever submissions retired in
        // order; the wait fallback covers the rest.
        static constexpr u32 kPayloadRingSize{3u};
        static_assert(kPayloadRingSize == kFrameRingSlots);

        std::error_code createInvariantRenderState_(rhi::IDevice &device);

        std::error_code createShaderProgram_(
            IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);

        // Pipeline-variant cache: target signature + twosided cull +
        // opaque/mask alpha. Blend is REJECTED with function_not_supported.
        // Each entry pairs the pipeline with per-slot persistent ROOT shader
        // objects (bound through the 2-arg bindPipeline overload) carrying
        // the cache-lifetime bindings — materials, textures, sampler — plus
        // the resolved g_frame cursor per slot, all fixed at creation. A
        // frame binds and writes only its selected slot's root, so per-frame
        // constants never mutate an object an in-flight submission still
        // references. encodeGroup_ writes only what varies: geometry, the
        // payload ring slot, g_payload_base, and FrameConstants.
        struct CachedVariantPipeline {
            rhi::ComPtr<rhi::IRenderPipeline> m_pipeline{};
            std::array<rhi::ComPtr<rhi::IShaderObject>, kPayloadRingSize> m_slot_roots{};
            // g_frame's dereferenced ConstantBuffer cursors, one per slot,
            // resolved once so the per-frame path skips the field lookup and
            // sub-object walk.
            std::array<rhi::ShaderCursor, kPayloadRingSize> m_slot_frame_cursors{};
        };

        [[nodiscard]] Expected<CachedVariantPipeline *> pipelineFor_(
            rhi::IDevice &device, const RenderPipelineSignature &signature, TrianglePipelineVariant variant);

        [[nodiscard]] std::error_code uploadFrameConstants_(rhi::ShaderCursor &frame_cursor);

        // Fail-closed resolve of every submitted instance, all-or-nothing: one
        // stale handle or stride mismatch fails the whole frame before any
        // encode. Zero-count bags resolve but are dropped by the caller.
        [[nodiscard]] Expected<Array<ResolvedInstance>> resolveInstances_() const;

        [[nodiscard]] Expected<ResolvedInstance> resolveOne_(const SubmittedInstance &instance) const;

        [[nodiscard]] Expected<DrawPlan> buildPlan_(const Array<ResolvedInstance> &resolved_instances);

        // Revision-stamped plan cache: the plan is camera-independent, so the
        // key is the submitted-sequence CONTENT (count + exact compare over
        // the SubmittedInstance array, handles and model included) plus the
        // resolved plan inputs it produced. A per-frame submit bump would
        // never hit under clear + resubmit, so no counter is kept. Any
        // difference in either key means a full replan; a hit reuses the
        // cached groups, whose source indices still name the same order.
        // Resolve still runs every frame ahead of the lookup, so a stale
        // handle or stride mismatch fails closed even on a key hit.
        [[nodiscard]] bool planCacheHit_(const Array<ResolvedInstance> &resolved_instances) const noexcept;

        void updatePlanCache_(const Array<ResolvedInstance> &resolved_instances, const DrawPlan &plan);

        void invalidatePlanCache_() noexcept;

        // All-or-nothing payload upload: the whole compacted interval or fail.
        [[nodiscard]] std::error_code uploadPayloads_(
            rhi::IDevice &device, const Array<ResolvedInstance> &resolved_instances, const DrawPlan &plan,
            FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

        [[nodiscard]] std::error_code encodeGroup_(
            const DrawContext &draw_context, const Array<ResolvedInstance> &resolved_instances, const DrawGroup &group);

        [[nodiscard]] std::error_code ensureDirectPayloads_(
            rhi::IDevice &device, u32 payload_count, FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

        // Reclaims deferred variant pipelines/roots at or below `completed`.
        void reclaimCompleted_(FenceCompletionValue completed) noexcept;

        // Rotates onto a completion-selected slot and waits it out when
        // stale. Records m_last_used_slot for notifyFrameSubmitted.
        [[nodiscard]] std::error_code selectSlot_(FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

        // Drops the whole payload ring without touching a device: unmap only
        // runs when one is supplied (the growth/replacement path), while
        // shutdown and device loss release still-mapped buffers as-is.
        void releaseDirectPayloads_(rhi::IDevice *device) noexcept;

        // ------------------------------------------------------------------
        // pass-owned GPU resources
        // ------------------------------------------------------------------

        // The one render program: mesh_bindless's payload-indexed vertex entry
        // plus the fragment entry. The pipeline-variant cache is keyed on the
        // render pipeline signature, so a signature change drops every variant
        // (pipelines and their persistent root objects alike).
        rhi::ComPtr<rhi::IShaderProgram> m_shader_program{};
        std::optional<RenderPipelineKey> m_render_pipeline_key;
        FlatMap<TrianglePipelineVariant, CachedVariantPipeline> m_variant_pipelines{};

        // The payload ring: kPayloadRingSize upload buffers at InstancePayload
        // (96 B) stride, each created and mapped ONCE and kept mapped for its
        // whole life. The frame selects its slot by per-slot completion
        // values, never by blind cursor parity (see selectSlot_):
        // uploadPayloads_ only advances onto a slot no in-flight frame is
        // still reading. Rotation is per render() submit and assumes a SINGLE
        // render per frame (backpressured by acquireNextImage); a second
        // per-frame render (e.g. a future TSR velocity/depth prepass) must
        // revisit this — move rotation to the frame boundary or grow the
        // ring. Pass-owned, so it outlives the encoded draws; released in
        // shutdown before waitOnHost.
        std::array<rhi::ComPtr<rhi::IBuffer>, kPayloadRingSize> m_direct_payloads{};
        std::array<void *, kPayloadRingSize> m_direct_payload_mapped{};
        u32 m_payload_ring_cursor{0u};
        u64 m_direct_payload_capacity = 0u;
        // Per-slot last-use completion values, one array for the single
        // payload ring. Set by notifyFrameSubmitted for the slot the frame
        // rotated onto; the slot is reusable once the fence passes it.
        std::array<FenceCompletionValue, kPayloadRingSize> m_payload_slot_values{};
        u32 m_last_used_slot{0u};

        // Retired variant pipelines/roots referenced by in-flight
        // submissions, keyed by last-use fence value. Reclaimed at every
        // render entry; drained unconditionally at shutdown/device-loss.
        DeferredReleaseQueue m_deferred_releases{};
        FenceCompletionValue m_last_submitted_value{kFenceValueNone};

        u64 m_growth_wait_count{0u};

        // Pass-owned GPU caches + the ONE shared sampler. m_shared_sampler is a
        // raw non-owning view lent to BindlessMaterialCache, so the teardown
        // ORDER below is the ownership contract between the two members:
        // m_material_cache must be shut down (which nulls its view) BEFORE
        // m_shared_sampler is released. Both shutdown and notifyDeviceLost
        // perform exactly that order; the initialize rollback does too.
        // m_fallback_* is the 1x1 white texture bound to none_v slots in
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

        // Revision-stamped plan cache (see planCacheHit_): retained content
        // keys plus the built plan. N x ~80 B is trivial; exact retained
        // copies keep the cache conservative by construction.
        Array<SubmittedInstance> m_plan_submitted_key{};
        Array<ResolvedInstance> m_plan_resolved_key{};
        DrawPlan m_cached_plan{};
        bool m_plan_cache_valid = false;

        // Live-scene receipts: one nonce per successful
        // uploadScene; releaseScene consumes it before touching the caches
        // so a double release fails closed without decrementing the
        // refcounted texture entry a surviving scene still holds.
        u64 m_next_scene_receipt = 1u;
        FlatSet<u64> m_live_scenes{};

        CameraSnapshot m_camera_view;
    };
}
