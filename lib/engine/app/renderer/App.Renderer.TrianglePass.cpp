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

    // ------------------------------------------------------------------
    // local upload and shader helpers
    // ------------------------------------------------------------------

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
            const auto index = *slot.m_image;
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
                asset->m_subresources.size() != 1u) [[unlikely]] {
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
                bytes.size() < subresource.m_slice_pitch) [[unlikely]] {
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
            for (const auto [index, slot]: std::views::enumerate(source_slots)) {
                if (not slot->enabled()) {
                    continue;
                }
                if (source_images[index] == nullptr) [[unlikely]] {
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                reference = source_images[index];
                break;
            }
            if (reference == nullptr) [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }

            OrmSource sources[3]{};
            for (const auto [index, source_image]: std::views::enumerate(source_images)) {
                Expected<OrmSource> source = ormSource_(source_image, reference->m_width, reference->m_height);
                if (not source.has_value()) [[unlikely]] {
                    return std::unexpected{source.error()};
                }
                sources[index] = *source;
            }

            const u64 slice_pitch = image::slicePitchFor(reference->m_width, reference->m_height, image::BlockTag::none);
            mem::UniqueBuffer storage = mem::UniqueBuffer::allocate(safe_narrowing(slice_pitch));
            PPR_RETURN_UNEXPECTED_ON_FAIL(TrianglePass, storage.materialize());
            Expected<mem::MutableBufferView> destination = storage.getMutableData();
            if (not destination.has_value()) [[unlikely]] {
                return std::unexpected{destination.error()};
            }

            const auto channel = [](const OrmSource &source, const u32 x, const u32 y, const u32 component, const u8 fallback) {
                if (source.m_bytes.empty()) {
                    return fallback;
                }
                const std::size_t offset = safe_narrowing(
                    static_cast<u64>(y) * source.m_row_pitch + static_cast<u64>(x) * 4u + component);
                return std::to_integer<u8>(source.m_bytes[offset]);
            };

            for (u32 y = 0u; y < reference->m_height; ++y) {
                for (u32 x = 0u; x < reference->m_width; ++x) {
                    const std::size_t offset = safe_narrowing((static_cast<u64>(y) * reference->m_width + x) * 4u);
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
                .m_view = frozen.subspan(0u, safe_narrowing(slice_pitch)),
                .m_row_pitch = image::rowPitchFor(reference->m_width, image::BlockTag::none),
                .m_slice_pitch = slice_pitch,
            });
            return composite;
        }

        // Never bind a bare rhi::Binding(): the default range does not survive
        // the module boundary and would build an empty SRV view. Every
        // setBinding below goes through makeFullRange.
        [[nodiscard]] rhi::BufferRange makeFullRange(rhi::IBuffer *const buffer) noexcept {
            return rhi::BufferRange{
                .offset = 0u,
                .size = buffer->getDesc().size,
            };
        }

        [[nodiscard]] TrianglePipelineVariant variantFor_(const GpuMaterial &gpu) noexcept {
            const u32 alpha_bits = gpu.m_flags.m_bits & kGpuMaterialAlphaModeMask;
            auto alpha = mesh::AlphaMode::opaque;
            if (alpha_bits == enumOrd(mesh::AlphaMode::mask)) {
                alpha = mesh::AlphaMode::mask;
            } else if (alpha_bits == enumOrd(mesh::AlphaMode::blend)) {
                // Already rejected at pack and again in pipelineFor_; mapped
                // faithfully so a hand-built material cannot silently render
                // as opaque.
                alpha = mesh::AlphaMode::blend;
            }
            return {
                .m_twosided = (gpu.m_flags.m_bits & kGpuMaterialDoubleSidedBit) != 0u,
                .m_alpha = alpha,
            };
        }
    }

    namespace fs = std::filesystem;

    // ------------------------------------------------------------------
    // pass lifecycle and asset APIs
    // ------------------------------------------------------------------

    std::error_code TrianglePass::initialize(IRhiService &rhi_service, IShaderService &shader_service, const fs::path &content_dir) {
        rhi::IDevice &device = rhi_service.getDevice();

        bool invariant_state_started = false;
        bool shader_state_started = false;
        bool bag_cache_ready = false;
        bool texture_cache_ready = false;
        bool sampler_started = false;
        bool material_cache_ready = false;
        bool initialization_complete = false;
        PPR_DEFER {
            if (not initialization_complete) {
                if (material_cache_ready) {
                    std::ignore = m_material_cache.shutdown();
                }
                if (sampler_started) {
                    m_shared_sampler.setNull();
                    m_sampler_handle = rhi::DescriptorHandle{};
                }
                if (texture_cache_ready) {
                    std::ignore = m_texture_cache.shutdown();
                }
                if (bag_cache_ready) {
                    std::ignore = m_bag_cache.shutdown();
                }
                if (shader_state_started) {
                    m_variant_pipelines.clear();
                    m_render_pipeline_key.reset();
                    m_shader_program.setNull();
                }
                if (invariant_state_started) {
                    m_fallback_view.setNull();
                    m_fallback_texture.setNull();
                    m_fallback_descriptor = rhi::DescriptorHandle{};
                }
                m_caches_ready = false;
            }
        };

        invariant_state_started = true;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, createInvariantRenderState_(device));
        shader_state_started = true;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, createShaderProgram_(shader_service, device, content_dir));

        // Pass-owned GPU caches (§2.4): bag → texture → sampler → material,
        // with per-acquisition rollback in reverse order on partial failure.
        if (const std::error_code err = m_bag_cache.initialize(device)) {
            PPR_LOG(TrianglePass, error, "bag cache init failed", {
                {"message", err.message()},
                });
            return err;
        }
        bag_cache_ready = true;
        if (const std::error_code err =
            m_texture_cache.initialize(device, rhi::kBindlessTextureBudget, m_fallback_descriptor)) {
            PPR_LOG(TrianglePass, error, "texture cache init failed", {
                {"message", err.message()},
                });
            return err;
        }
        texture_cache_ready = true;

        rhi::SamplerDesc sampler_desc{};
        sampler_desc.minFilter = rhi::TextureFilteringMode::Linear;
        sampler_desc.magFilter = rhi::TextureFilteringMode::Linear;
        sampler_desc.mipFilter = rhi::TextureFilteringMode::Linear;
        sampler_desc.addressU = rhi::TextureAddressingMode::Wrap;
        sampler_desc.addressV = rhi::TextureAddressingMode::Wrap;
        sampler_desc.addressW = rhi::TextureAddressingMode::Wrap;
        sampler_desc.maxAnisotropy = 1;
        sampler_started = true;
        if (const std::error_code err =
            make_error_code(device.createSampler(sampler_desc, m_shared_sampler.writeRef()))) {
            PPR_LOG(TrianglePass, error, "shared sampler creation failed", {
                {"message", err.message()},
                });
            return err;
        }
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            m_shared_sampler->getDescriptorHandle(&m_sampler_handle));
        if (const std::error_code err = m_material_cache.initialize(device, m_shared_sampler.get())) {
            PPR_LOG(TrianglePass, error, "material cache init failed", {
                {"message", err.message()},
                });
            return err;
        }
        material_cache_ready = true;
        m_caches_ready = true;
        initialization_complete = true;

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
        if (verts.empty() or idx.empty()) [[unlikely]] {
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
        GpuTextureRefs slots{
            .m_albedo = kNoTexture,
            .m_metallic_roughness = kNoTexture,
            .m_normal = kNoTexture,
            .m_emissive = kNoTexture,
        };
        TextureBindlessIndex *const slot_refs[] = {
            &slots.m_albedo, &slots.m_metallic_roughness, &slots.m_normal, &slots.m_emissive
        };
        for (u32 i = 0u; i < 4u; ++i) {
            if (isValid(resolved[i])) {
                Expected<TextureBindlessIndex> index = m_texture_cache.residentIndex(resolved[i]);
                if (not index.has_value()) [[unlikely]] {
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
            const auto index = *slot.m_image;
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
        // index slice, Mango prim base riding TriangleBagRange::m_base.
        for (const mesh::StaticMeshAsset &mesh_asset: scene.m_meshes) {
            for (const mesh::MeshPrimitiveRange &prim: mesh_asset.m_prims) {
                const u64 end = static_cast<u64>(prim.m_start) + prim.m_count;
                if (prim.m_count == 0u or end > mesh_asset.m_indices.size()) [[unlikely]] {
                    rollback();
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                if (static_cast<std::size_t>(*prim.m_material) >= scene.m_materials.size()) [[unlikely]] {
                    rollback();
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                if (mesh_asset.m_vertices.empty()) [[unlikely]] {
                    rollback();
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                const std::span slice(mesh_asset.m_indices.data() + prim.m_start, prim.m_count);
                const std::span verts(
                    mesh_asset.m_vertices.data(), mesh_asset.m_vertices.size());
                Expected<TriangleBagHandle> bag = m_bag_cache.upload(verts, slice, prim.m_base);
                if (not bag.has_value()) [[unlikely]] {
                    rollback();
                    return std::unexpected{bag.error()};
                }
                uploaded.m_prims.push_back(UploadedPrimitive{.m_bag = *bag, .m_material = MaterialHandle{}});
            }
        }

        for (const image::ImageAsset &image: images) {
            Expected<TextureHandle> texture = m_texture_cache.upload(image);
            if (not texture.has_value()) [[unlikely]] {
                rollback();
                return std::unexpected{texture.error()};
            }
            uploaded.m_textures.push_back(*texture);
        }

        for (const mesh::MaterialAsset &material: scene.m_materials) {
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
            if (not packed.has_value()) [[unlikely]] {
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

        u64 receipt = m_next_scene_receipt++;
        if (receipt == 0u) [[unlikely]] {
            receipt = m_next_scene_receipt++;
        }
        uploaded.m_receipt = receipt;
        m_live_scenes.emplace(receipt);
        return uploaded;
    }

    std::error_code TrianglePass::releaseScene(const UploadedScene &uploaded) noexcept {
        // Receipt first: stale/double releases fail closed here, before the
        // caches — a shared (deduped, refcounted) texture would otherwise
        // absorb the repeat release and invalidate the surviving scene.
        const auto live = m_live_scenes.find(uploaded.m_receipt);
        if (uploaded.m_receipt == 0u or live == m_live_scenes.end()) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        m_live_scenes.erase(live);
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
        if (not m_bag_cache.resolveForDraw(bag).has_value() or
            not m_material_cache.materialIndex(material).has_value()) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        m_submitted_instances.push_back(SubmittedInstance{.m_bag = bag, .m_material = material, .m_model = model});
        return default_value_v;
    }

    void TrianglePass::clearInstances() noexcept {
        m_submitted_instances.clear();
    }

    // Pure, CPU-only, and unit-tested without a device: batches the
    // per-instance draws that share a pipeline variant, a resolved buffer pair,
    // and an identical geometry range, then assigns each group a contiguous
    // payload interval.
    Expected<TrianglePass::DrawPlan> TrianglePass::planDraws(
        const std::span<const TrianglePipelineVariant> variants,
        const std::span<rhi::IBuffer *const> vertex_buffers,
        const std::span<rhi::IBuffer *const> index_buffers,
        const std::span<const TriangleBagRange> ranges) {
        if (variants.size() != vertex_buffers.size() or
            variants.size() != index_buffers.size() or
            variants.size() != ranges.size()) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        DrawPlan plan{};
        for (std::size_t index = 0u; index < variants.size(); ++index) {
            const TriangleBagRange &range = ranges[index];
            if (range.m_count == 0u) {
                continue;
            }

            DrawGroup *group = nullptr;
            for (DrawGroup &candidate: plan.m_groups) {
                if (candidate.m_variant == variants[index] and
                    candidate.m_vertex_buffer == vertex_buffers[index] and
                    candidate.m_index_buffer == index_buffers[index] and
                    candidate.m_vb_offset == range.m_vb_offset and
                    candidate.m_ib_start == range.m_ib_start and
                    candidate.m_count == range.m_count and
                    candidate.m_base_vertex == range.m_base) {
                    group = &candidate;
                    break;
                }
            }
            if (group == nullptr) {
                plan.m_groups.push_back(DrawGroup{
                    .m_variant = variants[index],
                    .m_vertex_buffer = vertex_buffers[index],
                    .m_index_buffer = index_buffers[index],
                    .m_vb_offset = range.m_vb_offset,
                    .m_ib_start = range.m_ib_start,
                    .m_count = range.m_count,
                    .m_base_vertex = range.m_base,
                    .m_instance_count = 0u,
                });
                group = &plan.m_groups.back();
            }
            group->m_source_indices.push_back(static_cast<u32>(index));
            ++group->m_instance_count;
        }
        for (DrawGroup &group: plan.m_groups) {
            group.m_first_payload = plan.m_payload_count;
            plan.m_payload_count += group.m_instance_count;
        }
        return plan;
    }

    // Target signature + twosided cull + opaque/mask alpha. Blend is rejected
    // here as well as at pack time, so a material that slipped through can
    // never reach a pipeline. A signature change invalidates every cached
    // variant, because the cached pipelines were built for the old signature.
    Expected<rhi::IRenderPipeline *> TrianglePass::pipelineFor_(
        rhi::IDevice &device,
        const RenderPipelineSignature &signature,
        const TrianglePipelineVariant variant) {
        if (m_shader_program == nullptr) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        if (const std::error_code err = checkPipelineVariant(variant)) {
            PPR_LOG(TrianglePass, error, "pipeline variant rejected", {
                {"message", err.message()},
                });
            return std::unexpected{err};
        }

        if (not m_render_pipeline_key.has_value() or
            static_cast<const RenderPipelineSignature &>(m_render_pipeline_key.value()) != signature) {
            PPR_LOG(TrianglePass, debug, "pipeline cache cleared on signature change");
            m_variant_pipelines.clear();
            m_render_pipeline_key.reset();
        }

        if (const auto found = m_variant_pipelines.find(variant); found != m_variant_pipelines.end()) {
            return found->second.get();
        }

        if (signature.m_color_formats.size() != 1u or signature.m_sample_count != 1u) {
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
        pipeline_desc.depthStencil.format = signature.m_depth_stencil_format.value_or(rhi::Format::Undefined);
        // Slang RHI's RenderState only carries vertex/scissor state. Depth
        // testing and writes therefore belong to the bound pipeline object.
        pipeline_desc.depthStencil.depthTestEnable = signature.m_depth_stencil_format.has_value();
        pipeline_desc.depthStencil.depthWriteEnable = signature.m_depth_stencil_format.has_value();
        pipeline_desc.depthStencil.depthFunc = rhi::ComparisonFunc::LessEqual;
        pipeline_desc.rasterizer.cullMode =
            variant.m_twosided ? rhi::CullMode::None : rhi::CullMode::Back;
        pipeline_desc.label = "mesh bindless pipeline";

        rhi::ComPtr<rhi::IRenderPipeline> pipeline{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(TrianglePass, device.createRenderPipeline(pipeline_desc, pipeline.writeRef()));

        rhi::IRenderPipeline *const raw = pipeline.get();
        m_variant_pipelines.emplace(variant, std::move(pipeline));
        m_render_pipeline_key.emplace(signature);
        return raw;
    }

    // ------------------------------------------------------------------
    // direct instance encoding
    // ------------------------------------------------------------------

    // Failure contract, phase by phase — never a whole-frame guarantee:
    //   staging  all-or-nothing (resolveInstances_ resolves every submitted
    //            instance before any encode, so no group is half-built);
    //   upload   all-or-nothing (uploadPayloads_ writes the whole payload
    //            interval or fails before any draw is encoded);
    //   encoding fail-closed per group: a group that cannot be encoded returns
    //            its error and stops the loop, and the groups already encoded
    //            into the pass stand. uploadScene keeps its own named rollback.
    std::error_code TrianglePass::render(const DrawContext &draw_context) {
        if (not m_caches_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }

        Expected<Array<ResolvedInstance, mem::ScratchPad> > resolved_instances = resolveInstances_();
        if (not resolved_instances.has_value()) [[unlikely]] {
            return resolved_instances.error();
        }
        if (resolved_instances->empty()) {
            return default_value_v;
        }

        Expected<DrawPlan> plan = buildPlan_(*resolved_instances);
        if (not plan.has_value()) [[unlikely]] {
            return plan.error();
        }
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            uploadPayloads_(draw_context.m_device, *resolved_instances, *plan));

        // Per-frame state, identical for every group: uploaded and applied
        // once, not re-done per draw.
        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
            .indexBuffer = {},
        });

        for (const DrawGroup &group: plan->m_groups) {
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                encodeGroup_(draw_context, *resolved_instances, group));
        }
        return default_value_v;
    }

    // Project resolved instances onto the planner's four parallel spans. The
    // planner itself stays a pure static function so it is unit-testable
    // without a device; this is the only place that knows the projection.
    Expected<TrianglePass::DrawPlan> TrianglePass::buildPlan_(
        const Array<ResolvedInstance, mem::ScratchPad> &resolved_instances) {
        Array<TrianglePipelineVariant, mem::ScratchPad> variants{};
        Array<rhi::IBuffer *, mem::ScratchPad> vertex_buffers{};
        Array<rhi::IBuffer *, mem::ScratchPad> index_buffers{};
        Array<TriangleBagRange, mem::ScratchPad> ranges{};
        for (const ResolvedInstance &instance: resolved_instances) {
            variants.push_back(instance.m_variant);
            vertex_buffers.push_back(instance.m_vertex_buffer);
            index_buffers.push_back(instance.m_index_buffer);
            ranges.push_back(TriangleBagRange{
                .m_vb_offset = instance.m_payload.m_vb_offset,
                .m_ib_start = instance.m_payload.m_ib_start,
                .m_count = instance.m_payload.m_index_count,
                .m_base = instance.m_payload.m_base_vertex,
            });
        }

        return planDraws(
            std::span<const TrianglePipelineVariant>{variants.data(), variants.size()},
            std::span<rhi::IBuffer *const>{vertex_buffers.data(), vertex_buffers.size()},
            std::span<rhi::IBuffer *const>{index_buffers.data(), index_buffers.size()},
            std::span<const TriangleBagRange>{ranges.data(), ranges.size()});
    }

    std::error_code TrianglePass::uploadPayloads_(
        rhi::IDevice &device,
        const Array<ResolvedInstance, mem::ScratchPad> &resolved_instances,
        const DrawPlan &plan) {
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, ensureDirectPayloads_(device, plan.m_payload_count));

        void *mapped_payloads = nullptr;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            device.mapBuffer(m_direct_payloads.get(), rhi::CpuAccessMode::Write, &mapped_payloads));

        // Compaction: each group owns a contiguous payload interval, and its
        // source indices name the resolved instances copied into it. Any source
        // index out of range is a planner bug — fail closed and unmap.
        for (const DrawGroup &group: plan.m_groups) {
            for (u32 offset = 0u; offset < group.m_instance_count; ++offset) {
                const u32 source_index = group.m_source_indices[offset];
                if (source_index >= resolved_instances.size()) [[unlikely]] {
                    device.unmapBuffer(m_direct_payloads.get());
                    return make_error_code(std::errc::invalid_argument);
                }

                std::memcpy(
                    static_cast<std::byte *>(mapped_payloads) +
                    static_cast<u64>(group.m_first_payload + offset) * sizeof(InstancePayload),
                    &resolved_instances[source_index].m_payload,
                    sizeof(InstancePayload));
            }
        }
        device.unmapBuffer(m_direct_payloads.get());
        return default_value_v;
    }

    std::error_code TrianglePass::ensureDirectPayloads_(rhi::IDevice &device, const u32 payload_count) {
        if (payload_count == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 payload_bytes = static_cast<u64>(payload_count) * sizeof(InstancePayload);
        if (m_direct_payloads != nullptr and m_direct_payload_capacity >= payload_bytes) {
            return default_value_v;
        }

        rhi::BufferDesc payload_desc{};
        payload_desc.size = payload_bytes;
        payload_desc.elementSize = sizeof(InstancePayload);
        payload_desc.memoryType = rhi::MemoryType::Upload;
        payload_desc.usage = rhi::BufferUsage::ShaderResource;
        payload_desc.defaultState = rhi::ResourceState::ShaderResource;
        payload_desc.label = "direct instance payloads";
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, device.createBuffer(payload_desc, nullptr, m_direct_payloads.writeRef()));
        m_direct_payload_capacity = payload_bytes;
        return default_value_v;
    }

    std::error_code TrianglePass::encodeGroup_(
        const DrawContext &draw_context,
        const Array<ResolvedInstance, mem::ScratchPad> &resolved_instances,
        const DrawGroup &group) {
        if (group.m_instance_count == 0u or
            group.m_source_indices.empty() or
            group.m_source_indices.size() != group.m_instance_count) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        for (const u32 source_index: group.m_source_indices) {
            if (source_index >= resolved_instances.size()) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }
        }

        rhi::IBuffer *const vertex_buffer = group.m_vertex_buffer;
        rhi::IBuffer *const index_buffer = group.m_index_buffer;
        rhi::IBuffer *const material_buffer = m_material_cache.materialBuffer();
        rhi::IBuffer *const texture_buffer = m_texture_cache.descriptorBuffer();
        if (vertex_buffer == nullptr or index_buffer == nullptr or material_buffer == nullptr or
            texture_buffer == nullptr or m_direct_payloads == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        // CPU↔Slang stride agreement: the bucket strides recorded at upload, the
        // payload stride, and the material stride must match the
        // StructuredBuffer element strides the shader was compiled against.
        // Fail closed — never draw with a reinterpreted buffer.
        if (vertex_buffer->getDesc().elementSize != sizeof(mesh::StaticMeshVertex) or
            index_buffer->getDesc().elementSize != sizeof(u32) or
            m_direct_payloads->getDesc().elementSize != sizeof(InstancePayload) or
            material_buffer->getDesc().elementSize != sizeof(GpuMaterial)) [[unlikely]] {
            PPR_LOG(TrianglePass, error, "bag/payload/material stride mismatch vs shader expectation", {
                {"vertex_stride", vertex_buffer->getDesc().elementSize},
                {"index_stride", index_buffer->getDesc().elementSize},
                {"payload_stride", m_direct_payloads->getDesc().elementSize},
                {"material_stride", material_buffer->getDesc().elementSize},
                });
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<rhi::IRenderPipeline *> pipeline = pipelineFor_(
            draw_context.m_device,
            draw_context.m_render_pipeline_key,
            group.m_variant);
        if (not pipeline.has_value()) [[unlikely]] {
            return pipeline.error();
        }

        rhi::ShaderCursor shader_cursor{};
        if (rhi::IShaderObject *const shader_object = draw_context.m_pass.bindPipeline(*pipeline);
            PPR_ENSURE(shader_object)) {
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

        // SV_InstanceID is draw-local on D3D12, SPIR-V, and Metal alike, so the
        // startInstanceLocation never reaches the shader. The group's payload
        // base rides the vertex entry's uniform param instead and the draw
        // starts at instance 0; binding both would double-count the offset for
        // any non-zero base.
        const u32 payload_base = group.m_first_payload;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_payload_base"].setData(&payload_base, sizeof(payload_base)));

        // Full-buffer ranges via makeFullRange (never bare Binding()).
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_vertices"].setBinding(
            rhi::Binding(vertex_buffer, makeFullRange(vertex_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_indices"].setBinding(
            rhi::Binding(index_buffer, makeFullRange(index_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_materials"].setBinding(
            rhi::Binding(material_buffer, makeFullRange(material_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_payloads"].setBinding(
            rhi::Binding(m_direct_payloads.get(), makeFullRange(m_direct_payloads.get()))));

        // bindPipeline creates a new shader object per draw. The texture
        // descriptor buffer is stable, but its binding is object-local.
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_textures"].setBinding(
            rhi::Binding(texture_buffer, makeFullRange(texture_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_sampler"].setDescriptorHandle(m_sampler_handle));

        draw_context.m_pass.draw({
            .vertexCount = group.m_count,
            .instanceCount = group.m_instance_count,
            .startInstanceLocation = 0u,
        });

        return default_value_v;
    }

    // Fail-closed resolve of every submitted instance, all-or-nothing: a stale
    // handle, a missing material, or a stride mismatch fails the whole frame
    // before anything is encoded.
    Expected<Array<TrianglePass::ResolvedInstance, mem::ScratchPad> > TrianglePass::resolveInstances_() const {
        Array<ResolvedInstance, mem::ScratchPad> resolved{};

        for (const SubmittedInstance &instance: m_submitted_instances) {
            Expected<ResolvedInstance> resolved_instance = resolveOne_(instance);
            if (not resolved_instance.has_value()) [[unlikely]] {
                return std::unexpected{resolved_instance.error()};
            }
            if (resolved_instance->m_count == 0u) {
                continue;
            }
            resolved.push_back(*resolved_instance);
        }
        return resolved;
    }

    Expected<TrianglePass::ResolvedInstance> TrianglePass::resolveOne_(const SubmittedInstance &instance) const {
        Expected<ResolvedBag> bag = m_bag_cache.resolveForDraw(instance.m_bag);
        if (not bag.has_value()) [[unlikely]] {
            return std::unexpected{bag.error()};
        }

        Expected<GpuMaterial> gpu = m_material_cache.material(instance.m_material);
        if (not gpu.has_value()) [[unlikely]] {
            return std::unexpected{gpu.error()};
        }

        Expected<u32> material_slot = m_material_cache.materialIndex(instance.m_material);
        if (not material_slot.has_value()) [[unlikely]] {
            return std::unexpected{material_slot.error()};
        }
        if (bag->m_vertex_buffer->getDesc().elementSize != sizeof(mesh::StaticMeshVertex) or
            m_material_cache.materialBuffer() == nullptr or
            m_material_cache.materialBuffer()->getDesc().elementSize != sizeof(GpuMaterial)) [[unlikely]] {
            PPR_LOG(TrianglePass, error, "bag/material stride mismatch vs shader expectation", {});
            return std::unexpected{make_error_code(std::errc::invalid_argument)};
        }

        // Plain-array boundary copy (P1 verdict): mango float4x4 is
        // never trivially copyable, so the model crosses into the POD
        // payload component-wise; the 64 bytes match the direct path's
        // setData(&model) upload exactly (row-major, translation row 3).
        InstancePayload payload{};
        payload.m_vb_offset = bag->m_range.m_vb_offset;
        payload.m_ib_start = bag->m_range.m_ib_start;
        payload.m_index_count = bag->m_range.m_count;
        payload.m_material = *material_slot;
        payload.m_base_vertex = bag->m_range.m_base;
        const float *const model_data = instance.m_model.data();
        for (u32 component = 0u; component < 16u; ++component) {
            payload.m_model[component] = model_data[component];
        }

        return ResolvedInstance{
            .m_payload = payload,
            .m_variant = variantFor_(*gpu),
            .m_count = bag->m_range.m_count,
            .m_vertex_buffer = bag->m_vertex_buffer,
            .m_index_buffer = bag->m_index_buffer,
        };
    }


    // ------------------------------------------------------------------
    // teardown and resource initialization
    // ------------------------------------------------------------------

    std::error_code TrianglePass::shutdown() {
        PPR_LOG(TrianglePass, info, "TrianglePass shut down", {
            {"instances", m_submitted_instances.size()},
            });

        // Teardown order: the three caches, then the shared sampler they borrow
        // (m_material_cache must release its non-owning view first), then the
        // pass's own programs, pipelines, and buffers. Retain-first-error,
        // best-effort, and all of it BEFORE the renderer's waitOnHost.
        std::error_code first_err{};
        m_submitted_instances.clear();
        m_live_scenes.clear();
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_material_cache.shutdown());
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_texture_cache.shutdown());
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_bag_cache.shutdown());
        m_shared_sampler.setNull();
        m_caches_ready = false;

        m_variant_pipelines.clear();
        m_render_pipeline_key.reset();
        m_shader_program.setNull();
        m_direct_payloads.setNull();
        m_direct_payload_capacity = 0u;
        m_fallback_view.setNull();
        m_fallback_texture.setNull();
        m_sampler_handle = rhi::DescriptorHandle{};
        return first_err;
    }

    std::error_code TrianglePass::notifyDeviceLost() noexcept {
        // Same order as shutdown (material → texture → bag), retain-first-error.
        // Live-scene receipts are kept: releaseScene still drains the retained
        // CPU records; only GPU-touching calls park. Programs survive device
        // loss, cached pipelines do not.
        std::error_code first_err{};
        m_submitted_instances.clear();
        m_variant_pipelines.clear();
        m_render_pipeline_key.reset();
        m_direct_payloads.setNull();
        m_direct_payload_capacity = 0u;
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_material_cache.notifyDeviceLost());
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_texture_cache.notifyDeviceLost());
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_bag_cache.notifyDeviceLost());
        if (not first_err) {
            m_caches_ready = false;
        }
        return first_err;
    }

    // §6 decided: NO fixed-function InputElementDesc for geometry — pure
    // StructuredBuffer fetch. Invariant state is the 1x1 white fallback texture
    // written to slot 0 of the TEXTURE descriptor heap, which is what a
    // kNoTexture material slot resolves to; material slots themselves start
    // at 0 and are always real materials.
    std::error_code TrianglePass::createInvariantRenderState_(rhi::IDevice &device) {
        constexpr u32 kWhitePixel = 0xFFFFFFFFu;
        const rhi::SubresourceData init_data{
            .data = &kWhitePixel,
            .rowPitch = sizeof(kWhitePixel),
            .slicePitch = sizeof(kWhitePixel),
        };

        rhi::TextureDesc fallback_desc{};
        fallback_desc.type = rhi::TextureType::Texture2D;
        fallback_desc.size = {
            .width = 1u,
            .height = 1u,
            .depth = 1u,
        };
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
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            triangle_shader->findEntryPointByName("vertexIndirectMain", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            triangle_shader->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

        // One program: the payload-indexed vertex entry reads
        // g_payloads[g_payload_base + SV_InstanceID], so the group's payload
        // base arrives as a global rather than through the draw arguments.
        slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = triangle_shader.get();
        program_desc.slangEntryPoints = entry_points;
        program_desc.slangEntryPointCount = 2u;

        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            device.createShaderProgram(program_desc, m_shader_program.writeRef(), diagnostics.writeRef()));

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
