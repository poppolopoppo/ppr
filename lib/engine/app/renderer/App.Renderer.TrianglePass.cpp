module;
#include "pP/Macros.h"
#include <slang.h>
#include <slang-com-ptr.h>
module engine.app;

import :renderer.triangle_pass;
import :renderer.types;
import :scene.camera;
import std;
import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import engine.image;
import engine.mesh;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(TrianglePass, debug, none)

    namespace {
        struct OrmSource final {
            mem::SharedBufferView m_bytes{};
            u64 m_row_pitch = 0u;
        };

        [[nodiscard]] const image::ImageAsset *imageForSlot_(
            const mesh::MaterialImageSlot &slot, const std::span<const image::ImageAsset> images) noexcept {
            if (not slot.enabled()) {
                return nullptr;
            }
            const std::size_t index = static_cast<std::size_t>(*slot.m_image);
            return index < images.size() ? &images[index] : nullptr;
        }

        [[nodiscard]] Expected<OrmSource> ormSource_(
            const image::ImageAsset *const asset, const u32 width, const u32 height) noexcept {
            if (asset == nullptr) {
                return OrmSource{};
            }
            if (asset->m_dimension != image::ImageDimension::image2d or
                asset->m_format != image::NativeImageFormat::rgba8_linear or
                asset->m_mip_count != 1u or
                asset->m_is_block or
                asset->m_width != width or
                asset->m_height != height or
                asset->m_subresources.size() != 1u)
            [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
            }
            const image::ImageSubresource &subresource = asset->m_subresources.front();
            const mem::SharedBufferView bytes = subresource.m_view.getBufferData();
            const u64 tight_row_pitch = image::rowPitchFor(width, image::BlockTag::none);
            const u64 tight_slice_pitch = image::slicePitchFor(width, height, image::BlockTag::none);
            if (not asset->m_storage.isValid() or
                not asset->m_storage.isMaterialized() or
                not subresource.m_view.isValid() or
                not subresource.m_view.isMaterialized() or
                subresource.m_row_pitch < tight_row_pitch or
                subresource.m_slice_pitch < tight_slice_pitch or
                bytes.size() < subresource.m_slice_pitch)
            [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
            return OrmSource{.m_bytes = bytes, .m_row_pitch = subresource.m_row_pitch};
        }

        [[nodiscard]] Expected<image::ImageAsset> composeOrm_(
            const mesh::MaterialAsset &material, const std::span<const image::ImageAsset> images) {
            const image::ImageAsset *const source_images[] = {
                imageForSlot_(material.m_occlusion_map, images),
                imageForSlot_(material.m_roughness_map, images),
                imageForSlot_(material.m_metallic_map, images),
            };
            const mesh::MaterialImageSlot *const source_slots[] = {
                &material.m_occlusion_map,
                &material.m_roughness_map,
                &material.m_metallic_map,
            };

            const image::ImageAsset *reference = nullptr;
            for (u32 i = 0u; i < 3u; ++i) {
                if (not source_slots[i]->enabled()) {
                    continue;
                }
                if (source_images[i] == nullptr) [[unlikely]] {
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                reference = source_images[i];
                break;
            }
            if (reference == nullptr) [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }

            OrmSource sources[3]{};
            for (u32 i = 0u; i < 3u; ++i) {
                Expected<OrmSource> source = ormSource_(source_images[i], reference->m_width, reference->m_height);
                if (not source.has_value()) [[unlikely]] {
                    return std::unexpected{source.error()};
                }
                sources[i] = *source;
            }

            const u64 slice_pitch = image::slicePitchFor(reference->m_width, reference->m_height, image::BlockTag::none);
            mem::UniqueBuffer storage = mem::UniqueBuffer::allocate(safe_narrowing<std::size_t>(slice_pitch));
            PPR_RETURN_UNEXPECTED_ON_FAIL(TrianglePass, storage.materialize());
            Expected<mem::MutableBufferView> destination = storage.getMutableData();
            if (not destination.has_value()) [[unlikely]] {
                return std::unexpected{destination.error()};
            }

            const auto channel = [](const OrmSource &source, const u32 x, const u32 y, const u32 component, const u8 fallback) {
                if (source.m_bytes.empty()) {
                    return fallback;
                }
                const std::size_t offset = safe_narrowing<std::size_t>(
                    static_cast<u64>(y) * source.m_row_pitch + static_cast<u64>(x) * 4u + component);
                return std::to_integer<u8>(source.m_bytes[offset]);
            };
            for (u32 y = 0u; y < reference->m_height; ++y) {
                for (u32 x = 0u; x < reference->m_width; ++x) {
                    const std::size_t offset = safe_narrowing<std::size_t>((static_cast<u64>(y) * reference->m_width + x) * 4u);
                    (*destination)[offset] = std::byte{channel(sources[0], x, y, 0u, 255u)};
                    (*destination)[offset + 1u] = std::byte{channel(sources[1], x, y, 1u, 255u)};
                    (*destination)[offset + 2u] = std::byte{channel(sources[2], x, y, 2u, 255u)};
                    (*destination)[offset + 3u] = std::byte{255u};
                }
            }

            mem::SharedBuffer frozen{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(TrianglePass, storage.moveToShared(&frozen));
            image::ImageAsset composite{};
            composite.m_width = reference->m_width;
            composite.m_height = reference->m_height;
            composite.m_storage = frozen;
            composite.m_subresources.push_back(image::ImageSubresource{
                .m_view = frozen.subspan(0u, safe_narrowing<std::size_t>(slice_pitch)),
                .m_row_pitch = image::rowPitchFor(reference->m_width, image::BlockTag::none),
                .m_slice_pitch = slice_pitch,
            });
            return composite;
        }

        // CPU mirror of mesh_bindless.slang PushScalars (vertex entry param):
        // scalars only, 20 B exact on both sides (no arrays/matrices, so no
        // uniform-stride traps). The model matrix rides its own param (64 B).
        struct PushScalars {
            u32 m_vb_offset = 0u;
            u32 m_ib_start = 0u;
            u32 m_index_count = 0u;
            u32 m_material = 0u;
            i32 m_base_vertex = 0;
        };

        static_assert(std::is_trivially_copyable_v<PushScalars>);
        static_assert(std::is_standard_layout_v<PushScalars>);
        static_assert(sizeof(PushScalars) == 20u);

        // Explicit full-buffer ranges (the debugger fix): kEntireBuffer is
        // static-const in the slang-rhi header and does NOT survive the module
        // boundary (reads {0,0} in this TU), so a default Binding would build
        // empty SRV views. Every setBinding below goes through makeFullRange —
        // bare Binding() is banned at these sites.
        [[nodiscard]] rhi::BufferRange makeFullRange(rhi::IBuffer *const buffer) noexcept {
            return rhi::BufferRange{0u, buffer->getDesc().size};
        }

        [[nodiscard]] TrianglePipelineVariant variantFor_(const GpuMaterial &gpu) noexcept {
            const u32 alpha_bits = gpu.m_flags.m_bits & kGpuMaterialAlphaModeMask;
            mesh::AlphaMode alpha = mesh::AlphaMode::opaque;
            if (alpha_bits == enumOrd(mesh::AlphaMode::mask)) {
                alpha = mesh::AlphaMode::mask;
            } else if (alpha_bits == enumOrd(mesh::AlphaMode::blend)) {
                // Unreachable: pack rejects blend, pipelineFor_ rejects again.
                alpha = mesh::AlphaMode::blend;
            }
            return {
                .m_twosided = (gpu.m_flags.m_bits & kGpuMaterialDoubleSidedBit) != 0u,
                .m_alpha = alpha,
            };
        }
    }

    namespace fs = std::filesystem;

    std::error_code TrianglePass::initialize(IRhiService &rhi_service, IShaderService &shader_service, const fs::path &content_dir) {
        rhi::IDevice &device = rhi_service.getDevice();

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, createInvariantRenderState_(device));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, createShaderProgram_(shader_service, device, content_dir));

        // Pass-owned GPU caches (§2.4): bag → texture → sampler → material,
        // with per-acquisition rollback in reverse order on partial failure.
        if (const std::error_code err = m_bag_cache.initialize(device)) {
            PPR_LOG(TrianglePass, error, "bag cache init failed", {
                {"message", err.message()},
                });
            return err;
        }
        if (const std::error_code err =
                m_texture_cache.initialize(device, rhi::kBindlessTextureBudget, m_fallback_descriptor)) {
            PPR_LOG(TrianglePass, error, "texture cache init failed", {
                {"message", err.message()},
                });
            std::ignore = m_bag_cache.shutdown();
            return err;
        }

        rhi::SamplerDesc sampler_desc{};
        sampler_desc.minFilter = rhi::TextureFilteringMode::Linear;
        sampler_desc.magFilter = rhi::TextureFilteringMode::Linear;
        sampler_desc.mipFilter = rhi::TextureFilteringMode::Linear;
        sampler_desc.addressU = rhi::TextureAddressingMode::Wrap;
        sampler_desc.addressV = rhi::TextureAddressingMode::Wrap;
        sampler_desc.addressW = rhi::TextureAddressingMode::Wrap;
        sampler_desc.maxAnisotropy = 1;
        if (const std::error_code err =
                make_error_code(device.createSampler(sampler_desc, m_shared_sampler.writeRef()))) {
            PPR_LOG(TrianglePass, error, "shared sampler creation failed", {
                {"message", err.message()},
                });
            std::ignore = m_texture_cache.shutdown();
            std::ignore = m_bag_cache.shutdown();
            return err;
        }
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            m_shared_sampler->getDescriptorHandle(&m_sampler_handle));
        if (const std::error_code err = m_material_cache.initialize(device, m_shared_sampler.get())) {
            PPR_LOG(TrianglePass, error, "material cache init failed", {
                {"message", err.message()},
                });
            m_shared_sampler.setNull();
            std::ignore = m_texture_cache.shutdown();
            std::ignore = m_bag_cache.shutdown();
            return err;
        }
        m_caches_ready = true;

        PPR_LOG(TrianglePass, info, "TrianglePass initialized");
        return default_value_v;
    }

    std::error_code TrianglePass::update([[maybe_unused]] TimeSpan dt, const CameraSnapshot &camera_view) {
        m_camera_view = camera_view;

        return default_value_v;
    }

    Expected<TriangleBagHandle> TrianglePass::uploadMesh(
        const std::span<const mesh::StaticMeshVertex> verts, const std::span<const u32> idx) {
        if (not m_caches_ready) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        if (verts.empty() or idx.empty())
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        static_assert(sizeof(mesh::StaticMeshVertex) == 64u);
        return m_bag_cache.upload(verts, idx);
    }

    Expected<TextureHandle> TrianglePass::uploadTexture(const image::ImageAsset &asset) {
        if (not m_caches_ready) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        return m_texture_cache.upload(asset);
    }

    Expected<MaterialHandle> TrianglePass::packMaterial(
        const mesh::MaterialAsset &asset, const std::span<const TextureHandle> resolved) {
        if (not m_caches_ready) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        // GpuTextureRefs order: albedo, metallic-roughness composite, normal,
        // emissive. Invalid handles map to kNoTexture (shader fallback).
        // ORM compositing (occlusion/roughness/metallic→one slot, R=occl,
        // G=rough, B=metal) happens in uploadScene via composeOrm_; this path
        // only resolves the already-composited handles.
        if (resolved.size() != 4u) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        GpuTextureRefs slots{kNoTexture, kNoTexture, kNoTexture, kNoTexture};
        TextureBindlessIndex *const slot_refs[] = {
            &slots.m_albedo, &slots.m_metallic_roughness, &slots.m_normal, &slots.m_emissive
        };
        for (u32 i = 0u; i < 4u; ++i) {
            if (isValid(resolved[i])) {
                Expected<TextureBindlessIndex> index = m_texture_cache.residentIndex(resolved[i]);
                if (not
                    index.has_value())
                [[unlikely]] {
                    return std::unexpected{index.error()};
                }
                *slot_refs[i] = *index;
            }
        }
        return m_material_cache.pack(asset, slots);
    }

    namespace {
        [[nodiscard]] TextureHandle resolveImageSlot_(
            const mesh::MaterialImageSlot &slot,
            const std::span<const TextureHandle> textures) noexcept {
            if (not slot.enabled()) {
                return TextureHandle{};
            }
            const std::size_t index = static_cast<std::size_t>(*slot.m_image);
            return index < textures.size() ? textures[index] : TextureHandle{};
        }
    }

    Expected<TrianglePass::UploadedScene> TrianglePass::uploadScene(
        const mesh::SceneAsset &scene, const std::span<const image::ImageAsset> images) {
        if (not m_caches_ready) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        if (images.size() != scene.m_images.size()) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        UploadedScene uploaded{};
        const auto rollback = [&]() noexcept {
            for (auto it = uploaded.m_materials.rbegin(); it != uploaded.m_materials.rend(); ++it) {
                std::ignore = m_material_cache.release(*it);
            }
            for (auto it = uploaded.m_textures.rbegin(); it != uploaded.m_textures.rend(); ++it) {
                std::ignore = m_texture_cache.release(*it);
            }
            for (auto it = uploaded.m_prims.rbegin(); it != uploaded.m_prims.rend(); ++it) {
                std::ignore = m_bag_cache.release(it->m_bag);
            }
            uploaded = UploadedScene{};
        };

        // Bags first: one per (mesh, prim) — full mesh verts plus the prim
        // index slice, Mango prim base riding the range into MeshPush.
        for (const mesh::StaticMeshAsset &mesh_asset: scene.m_meshes) {
            for (const mesh::MeshPrimitiveRange &prim: mesh_asset.m_prims) {
                const u64 end = static_cast<u64>(prim.m_start) + static_cast<u64>(prim.m_count);
                if (prim.m_count == 0u or end > static_cast<u64>(mesh_asset.m_indices.size()))
                [[unlikely]] {
                    rollback();
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                if (static_cast<std::size_t>(*prim.m_material) >= scene.m_mats.size()) [[unlikely]] {
                    rollback();
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                if (mesh_asset.m_verts.empty()) [[unlikely]] {
                    rollback();
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                const std::span<const u32> slice(mesh_asset.m_indices.data() + prim.m_start, prim.m_count);
                const std::span<const mesh::StaticMeshVertex> verts(
                    mesh_asset.m_verts.data(), mesh_asset.m_verts.size());
                Expected<TriangleBagHandle> bag = m_bag_cache.upload(verts, slice, prim.m_base);
                if (not
                    bag.has_value())
                [[unlikely]] {
                    rollback();
                    return std::unexpected{bag.error()};
                }
                uploaded.m_prims.push_back(UploadedPrimitive{.m_bag = *bag, .m_material = MaterialHandle{}});
            }
        }

        for (const image::ImageAsset &image: images) {
            Expected<TextureHandle> texture = m_texture_cache.upload(image);
            if (not
                texture.has_value())
            [[unlikely]] {
                rollback();
                return std::unexpected{texture.error()};
            }
            uploaded.m_textures.push_back(*texture);
        }

        for (const mesh::MaterialAsset &material: scene.m_mats) {
            const std::span<const TextureHandle> texture_span(uploaded.m_textures.data(), images.size());
            TextureHandle orm{};
            if (material.m_metallic_map.enabled() or material.m_roughness_map.enabled() or material.m_occlusion_map.enabled()) {
                Expected<image::ImageAsset> composite = composeOrm_(material, images);
                if (not composite.has_value()) [[unlikely]] {
                    rollback();
                    return std::unexpected{composite.error()};
                }
                Expected<TextureHandle> texture = m_texture_cache.upload(*composite);
                if (not texture.has_value()) [[unlikely]] {
                    rollback();
                    return std::unexpected{texture.error()};
                }
                orm = *texture;
                uploaded.m_textures.push_back(*texture);
            }
            const TextureHandle resolved[] = {
                resolveImageSlot_(material.m_base_color_map, texture_span),
                orm,
                resolveImageSlot_(material.m_normal_map, texture_span),
                resolveImageSlot_(material.m_emissive_map, texture_span),
            };
            Expected<MaterialHandle> packed = packMaterial(material, resolved);
            if (not
                packed.has_value())
            [[unlikely]] {
                rollback();
                return std::unexpected{packed.error()};
            }
            uploaded.m_materials.push_back(*packed);
        }

        // Join prims to packed materials by running prim order.
        std::size_t prim_cursor = 0u;
        for (const mesh::StaticMeshAsset &mesh_asset: scene.m_meshes) {
            for (const mesh::MeshPrimitiveRange &prim: mesh_asset.m_prims) {
                uploaded.m_prims[prim_cursor].m_material = uploaded.m_materials[*prim.m_material];
                ++prim_cursor;
            }
        }
        return uploaded;
    }

    std::error_code TrianglePass::releaseScene(const UploadedScene &uploaded) noexcept {
        std::error_code first_err{};
        for (auto it = uploaded.m_materials.rbegin(); it != uploaded.m_materials.rend(); ++it) {
            PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_material_cache.release(*it));
        }
        for (auto it = uploaded.m_textures.rbegin(); it != uploaded.m_textures.rend(); ++it) {
            PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_texture_cache.release(*it));
        }
        for (auto it = uploaded.m_prims.rbegin(); it != uploaded.m_prims.rend(); ++it) {
            PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_bag_cache.release(it->m_bag));
        }
        return first_err;
    }

    std::error_code TrianglePass::submitInstance(
        const TriangleBagHandle bag, const MaterialHandle material, const float4x4 &model) {
        if (not m_caches_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        if (not m_bag_cache.resolve(bag).has_value() or not m_material_cache.materialIndex(material).has_value())
        [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        m_instances.push_back(Instance{.m_bag = bag, .m_material = material, .m_model = model});
        return default_value_v;
    }

    void TrianglePass::clearInstances() noexcept {
        m_instances.clear();
    }

    Expected<rhi::IRenderPipeline *> TrianglePass::pipelineFor_(
        rhi::IDevice &device,
        const RenderPipelineSignature &signature,
        const TrianglePipelineVariant variant) {
        if (const std::error_code err = checkPipelineVariant(variant)) {
            PPR_LOG(TrianglePass, error, "pipeline variant rejected", {
                {"message", err.message()},
                });
            return std::unexpected{err};
        }
        if (not m_render_pipeline_key.has_value() or
            not(static_cast<const RenderPipelineSignature &>(m_render_pipeline_key.value()) == signature)) {
            m_variant_pipelines.clear();
            m_render_pipeline_key.reset();
            m_render_pipeline.setNull();
        }
        if (const auto found = m_variant_pipelines.find(variant); found != m_variant_pipelines.end()) {
            return found->second.get();
        }
        if (signature.m_color_formats.size() != 1u or
            signature.m_depth_stencil_format.has_value() or
            signature.m_sample_count != 1u) {
            PPR_LOG(TrianglePass, error, "unsupported render pipeline signature", {
                {"color_format_count", signature.m_color_formats.size()},
                {"has_depth_stencil", signature.m_depth_stencil_format.has_value()},
                {"sample_count", signature.m_sample_count},
                });
            return std::unexpected{std::make_error_code(std::errc::operation_not_supported)};
        }

        rhi::ColorTargetDesc color_target{};
        color_target.format = signature.m_color_formats.front();
        color_target.enableBlend = false;

        rhi::RenderPipelineDesc pipeline_desc{};
        pipeline_desc.program = m_shader_program.get();
        pipeline_desc.inputLayout = nullptr;
        pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
        pipeline_desc.targets = &color_target;
        pipeline_desc.targetCount = 1u;
        pipeline_desc.multisample.sampleCount = signature.m_sample_count;
        pipeline_desc.rasterizer.cullMode =
                variant.m_twosided ? rhi::CullMode::None : rhi::CullMode::Back;
        pipeline_desc.label = "mesh bindless pipeline";

        rhi::ComPtr<rhi::IRenderPipeline> pipeline{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(TrianglePass, device.createRenderPipeline(pipeline_desc, pipeline.writeRef()));

        rhi::IRenderPipeline *const raw = pipeline.get();
        m_variant_pipelines.emplace(variant, std::move(pipeline));
        m_render_pipeline = raw;
        m_render_pipeline_key.emplace(signature);
        return raw;
    }

    // §6 encode per instance (mirrors render:58-71 + ImGui:409-412 idiom):
    // resolve handle → range, ShaderCursor binds (buffers + per-resident-texture
    // setDescriptorHandle via seam (a)), push setData, NO vertex/index-buffer
    // bindings, NON-INDEXED draw with sv_vertex_id driving the manual lookup.
    std::error_code TrianglePass::render(const DrawContext &draw_context) {
        // The descriptor container is stable for one render invocation. Bind it
        // on the first draw only; subsequent draws only push instance data.
        m_texture_heap_bound = false;
        for (const Instance &instance: m_instances) {
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass, encodeInstance_(draw_context, instance));
        }
        return default_value_v;
    }

    std::error_code TrianglePass::encodeInstance_(const DrawContext &draw_context, const Instance &instance) {
        Expected<TriangleBagRange> range = m_bag_cache.resolve(instance.m_bag);
        if (not
            range.has_value())
        [[unlikely]] {
            return range.error();
        }
        Expected<BagBucketId> bucket = m_bag_cache.bucketOf(instance.m_bag);
        if (not
            bucket.has_value())
        [[unlikely]] {
            return bucket.error();
        }
        rhi::IBuffer *const vertex_buffer = m_bag_cache.vertexBuffer(*bucket);
        rhi::IBuffer *const index_buffer = m_bag_cache.indexBuffer(*bucket);
        if (vertex_buffer == nullptr or index_buffer == nullptr)
        [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        Expected<GpuMaterial> gpu = m_material_cache.material(instance.m_material);
        if (not
            gpu.has_value())
        [[unlikely]] {
            return gpu.error();
        }
        Expected<u32> material_slot = m_material_cache.materialIndex(instance.m_material);
        if (not
            material_slot.has_value())
        [[unlikely]] {
            return material_slot.error();
        }
        rhi::IBuffer *const material_buffer = m_material_cache.materialBuffer();
        rhi::IBuffer *const texture_buffer = m_texture_cache.descriptorBuffer();
        if (material_buffer == nullptr or texture_buffer == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        // CPU↔Slang stride agreement (plan §6 mirror table): the bucket stride
        // recorded at upload and the material stride must match the
        // StructuredBuffer element strides the shader was compiled against.
        // Fail closed — never draw with a reinterpreted buffer.
        if (vertex_buffer->getDesc().elementSize != sizeof(mesh::StaticMeshVertex) or
            material_buffer->getDesc().elementSize != sizeof(GpuMaterial)) [[unlikely]] {
            PPR_LOG(TrianglePass, error, "bag/material stride mismatch vs shader expectation", {
                {"vertex_stride", vertex_buffer->getDesc().elementSize},
                {"material_stride", material_buffer->getDesc().elementSize},
                });
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<rhi::IRenderPipeline *> pipeline =
                pipelineFor_(draw_context.m_device, draw_context.m_render_pipeline_key, variantFor_(*gpu));
        if (not
            pipeline.has_value())
        [[unlikely]] {
            return pipeline.error();
        }

        rhi::ShaderCursor shader_cursor{};
        if (rhi::IShaderObject *const shader_object = draw_context.m_pass.bindPipeline(*pipeline); PPR_ENSURE(shader_object)) {
            shader_cursor = rhi::ShaderCursor(shader_object);
        } else {
            return make_error_code(std::errc::broken_pipe);
        }

        // Dereference the ConstantBuffer field so the data lands in the g_frame
        // sub-object's ordinary data buffer (which is what gets uploaded at draw time),
        // rather than in the root object's own buffer.
        rhi::ShaderCursor frame_cursor{};
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_frame"].getDereferenced(frame_cursor));

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, uploadFrameConstants_(frame_cursor));

        // Full-buffer ranges via makeFullRange (never bare Binding()).
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_vertices"].setBinding(rhi::Binding(vertex_buffer, makeFullRange(vertex_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_indices"].setBinding(rhi::Binding(index_buffer, makeFullRange(index_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_materials"].setBinding(rhi::Binding(material_buffer, makeFullRange(material_buffer))));

        if (not m_texture_heap_bound) {
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                shader_cursor["g_textures"].setBinding(rhi::Binding(texture_buffer, makeFullRange(texture_buffer))));
            m_texture_heap_bound = true;
        }
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_sampler"].setDescriptorHandle(m_sampler_handle));

        // Entry-point params (found via cursor DWIM): model matrix + packed
        // scalars land in per-entry ordinary data, no dereference needed.
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_model"].setData(&instance.m_model, sizeof(float4x4)));
        const PushScalars scalars{
            .m_vb_offset = range->m_vb_offset,
            .m_ib_start = range->m_ib_start,
            .m_index_count = range->m_count,
            .m_material = *material_slot,
            .m_base_vertex = range->m_base,
        };
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_push"].setData(&scalars, sizeof(scalars)));

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
        });

        draw_context.m_pass.draw({.vertexCount = range->m_count});

        return default_value_v;
    }

    std::error_code TrianglePass::shutdown() {
        PPR_LOG(TrianglePass, info, "TrianglePass shut down", {
            {"has_pipeline", m_render_pipeline != nullptr},
            {"instances", m_instances.size()},
            });

        // §2.4 teardown: caches → sampler → pipeline/program/layout/buffers
        // (retain-first-error, best-effort), BEFORE renderer waitOnHost.
        std::error_code first_err{};
        m_instances.clear();
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_material_cache.shutdown());
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_texture_cache.shutdown());
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_bag_cache.shutdown());
        m_shared_sampler.setNull();
        m_caches_ready = false;

        m_variant_pipelines.clear();
        m_render_pipeline_key.reset();
        m_render_pipeline.setNull();
        m_shader_program.setNull();
        m_fallback_view.setNull();
        m_fallback_texture.setNull();
        m_sampler_handle = rhi::DescriptorHandle{};
        m_texture_heap_bound = false;
        return first_err;
    }

    // §6 decided: NO fixed-function InputElementDesc for geometry — pure
    // StructuredBuffer fetch. Invariant state is the 1x1 white fallback texture
    // bound to kNoTexture scalar-handle slots.
    std::error_code TrianglePass::createInvariantRenderState_(rhi::IDevice &device) {
        constexpr u32 kWhitePixel = 0xFFFFFFFFu;
        const rhi::SubresourceData init_data{
            .data = &kWhitePixel,
            .rowPitch = sizeof(kWhitePixel),
            .slicePitch = sizeof(kWhitePixel),
        };

        rhi::TextureDesc fallback_desc{};
        fallback_desc.type = rhi::TextureType::Texture2D;
        fallback_desc.size = {1u, 1u, 1u};
        fallback_desc.arrayLength = 1u;
        fallback_desc.mipCount = 1u;
        fallback_desc.format = rhi::Format::RGBA8Unorm;
        fallback_desc.memoryType = rhi::MemoryType::DeviceLocal;
        fallback_desc.usage = rhi::TextureUsage::ShaderResource;
        fallback_desc.defaultState = rhi::ResourceState::ShaderResource;
        fallback_desc.label = "bindless fallback white";

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            device.createTexture(fallback_desc, &init_data, m_fallback_texture.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, m_fallback_texture->getDefaultView(m_fallback_view.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            m_fallback_view->getDescriptorHandle(rhi::DescriptorHandleAccess::Read, &m_fallback_descriptor));

        return default_value_v;
    }

    std::error_code TrianglePass::createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device, const fs::path &content_dir) {
        shader::SharedModule triangle_shader{};
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_service.loadModuleFromFile(
            content_dir / TEXT("shaders") / TEXT("mesh_bindless.slang"),
            "mesh_bindless",
            triangle_shader.writeRef()));

        shader::ComPtr<slang::IEntryPoint> vertex_ep;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, triangle_shader->findEntryPointByName("vertexMain", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, triangle_shader->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

        slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = triangle_shader.get();
        program_desc.slangEntryPoints = entry_points;
        program_desc.slangEntryPointCount = 2u;

        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, device.createShaderProgram(program_desc, m_shader_program.writeRef(), diagnostics.writeRef()));

        return default_value_v;
    }

    std::error_code TrianglePass::uploadFrameConstants_(rhi::ShaderCursor &frame_cursor) {
        FrameConstants frame{};
        frame.m_view = m_camera_view.m_view;
        frame.m_projection = m_camera_view.m_projection;
        frame.m_view_projection = m_camera_view.m_view_projection;
        frame.m_inverse_view_projection = m_camera_view.m_invert_view_projection;
        frame.m_camera_position = float4{m_camera_view.m_origin, 1.0f}; // point promotion (w == 1).
        frame.m_viewport_size = float4{m_camera_view.m_viewport_size, 0.0f, 0.0f};

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, frame_cursor.setData(&frame, sizeof(FrameConstants)));

        return default_value_v;
    }
}
