module;
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
        };

        [[nodiscard]] std::error_code initialize(IRhiService &rhi_service, IShaderService &shader_service, const std::filesystem::path &content_dir);

        [[nodiscard]] std::error_code update(TimeSpan dt, const CameraSnapshot &camera_view);

        [[nodiscard]] std::error_code render(const DrawContext &draw_context);

        [[nodiscard]] std::error_code shutdown();

        // Narrow P2 asset APIs (plan §7): uploadMesh/uploadTexture/packMaterial
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
        // pack per material (metallic else roughness for the mr slot; occlusion
        // rides the strength factor, no 5th slot). Partial rollback in reverse
        // order; images[i] joins SceneAsset::m_images[i].
        [[nodiscard]] Expected<UploadedScene> uploadScene(
            const mesh::SceneAsset &scene, std::span<const image::ImageAsset> images);

        [[nodiscard]] std::error_code releaseScene(const UploadedScene &uploaded) noexcept;

        [[nodiscard]] TriangleBagCache &bagCache() noexcept { return m_bag_cache; }
        [[nodiscard]] BindlessTextureCache &textureCache() noexcept { return m_texture_cache; }
        [[nodiscard]] BindlessMaterialCache &materialCache() noexcept { return m_material_cache; }

    private:
        std::error_code createInvariantRenderState_(rhi::IDevice &device);

        std::error_code createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device, const std::filesystem::path &content_dir);

        // Pipeline-variant cache (plan §7): target signature + twosided cull +
        // opaque/mask alpha. Blend is REJECTED with function_not_supported.
        [[nodiscard]] Expected<rhi::IRenderPipeline *> pipelineFor_(
            rhi::IDevice &device,
            const RenderPipelineSignature &signature,
            TrianglePipelineVariant variant);

        [[nodiscard]] std::error_code uploadFrameConstants_(rhi::ShaderCursor &frame_cursor);

        [[nodiscard]] std::error_code encodeInstance_(const DrawContext &draw_context, const Instance &instance);

        rhi::ComPtr<rhi::IShaderProgram> m_shader_program{};

        rhi::ComPtr<rhi::IRenderPipeline> m_render_pipeline{};
        std::optional<RenderPipelineKey> m_render_pipeline_key;
        FlatMap<TrianglePipelineVariant, rhi::ComPtr<rhi::IRenderPipeline> > m_variant_pipelines{};

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
        bool m_texture_heap_bound = false;
        bool m_caches_ready = false;

        Array<Instance> m_instances{};

        CameraSnapshot m_camera_view;
    };
}
