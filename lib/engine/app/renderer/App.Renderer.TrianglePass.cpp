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
                    m_indirect_pipelines.clear();
                    m_render_pipeline_key.reset();
                    m_render_pipeline.setNull();
                    m_compute_pipeline.setNull();
                    m_compute_program.setNull();
                    m_indirect_program.setNull();
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
        // index slice, Mango prim base riding the range into MeshPush.
        for (const mesh::StaticMeshAsset &mesh_asset: scene.m_meshes) {
            for (const mesh::MeshPrimitiveRange &prim: mesh_asset.m_prims) {
                const u64 end = static_cast<u64>(prim.m_start) + prim.m_count;
                if (prim.m_count == 0u or end > mesh_asset.m_indices.size()) [[unlikely]] {
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
                const std::span slice(mesh_asset.m_indices.data() + prim.m_start, prim.m_count);
                const std::span verts(
                    mesh_asset.m_verts.data(), mesh_asset.m_verts.size());
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
        if (not m_bag_cache.resolve(bag).has_value() or not m_material_cache.materialIndex(material).has_value()) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        m_instances.push_back(Instance{.m_bag = bag, .m_material = material, .m_model = model});
        return default_value_v;
    }

    void TrianglePass::clearInstances() noexcept {
        m_instances.clear();
    }

    // ------------------------------------------------------------------
    // indirect planning and pipeline lookup
    // ------------------------------------------------------------------

    // Phase 7 L1 indirect planning (pure, CPU-only — unit-tested without a
    // device): groups per-(prim, instance) draws into one contiguous args
    // slice per non-empty (variant, bag-bucket) range. Variant buckets stay
    // ≤4 (opaque/mask × cull); ranges multiply only across bag buckets (one
    // layout per MVP scene → identical). Records carry instanceCount 1 with
    // startInstanceLocation = payload index, so SV_InstanceID indexes
    // g_payloads directly.
    Expected<TrianglePass::IndirectPlan> TrianglePass::planIndirectDraws(
        const std::span<const TrianglePipelineVariant> variants,
        const std::span<const BagBucketId> bag_buckets,
        const std::span<const u32> vertex_counts,
        const u32 payload_capacity) {
        if (variants.size() != bag_buckets.size() or variants.size() != vertex_counts.size()) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        IndirectPlan plan{};
        Array<TrianglePipelineVariant> seen_variants{};
        struct PendingDraw {
            TrianglePipelineVariant m_variant{};
            BagBucketId m_bag_bucket{};
            u32 m_count = 0u;
            u32 m_payload_index = 0u;
        };
        Array<PendingDraw> pending{};
        for (std::size_t i = 0u; i < variants.size(); ++i) {
            if (vertex_counts[i] == 0u) {
                continue;
            }
            if (plan.m_total_count >= payload_capacity) {
                break;
            }
            const TrianglePipelineVariant variant = variants[i];
            bool known_variant = false;
            for (const TrianglePipelineVariant seen: seen_variants) {
                if (seen == variant) {
                    known_variant = true;
                    break;
                }
            }
            if (not known_variant) {
                if (seen_variants.size() >= kMaxIndirectBuckets) [[unlikely]] {
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                seen_variants.push_back(variant);
            }
            pending.push_back(PendingDraw{
                .m_variant = variant,
                .m_bag_bucket = bag_buckets[i],
                .m_count = vertex_counts[i],
                .m_payload_index = plan.m_total_count,
            });
            ++plan.m_total_count;
        }
        // Group args by (variant, bag-bucket) in first-seen order so each
        // bucket's [first, first+count) slice is contiguous: drawIndirect
        // draws the whole slice with one pipeline, so interleaved emission
        // order must not leak a foreign record into a bucket's range.
        // startInstanceLocation still carries the payload index, so
        // SV_InstanceID indexes g_payloads regardless of args order.
        for (const PendingDraw &first: pending) {
            bool emitted_group = false;
            for (const IndirectBucket &bucket: plan.m_buckets) {
                if (bucket.m_variant == first.m_variant and bucket.m_bag_bucket == first.m_bag_bucket) {
                    emitted_group = true;
                    break;
                }
            }
            if (emitted_group) {
                continue;
            }
            const u32 first_arg = static_cast<u32>(plan.m_args.size());
            u32 group_count = 0u;
            for (const PendingDraw &draw: pending) {
                if (draw.m_variant == first.m_variant and draw.m_bag_bucket == first.m_bag_bucket) {
                    plan.m_args.push_back(rhi::IndirectDrawArguments{
                        .vertexCountPerInstance = draw.m_count,
                        .instanceCount = 1u,
                        .startVertexLocation = 0u,
                        .startInstanceLocation = draw.m_payload_index,
                    });
                    ++group_count;
                }
            }
            plan.m_buckets.push_back(IndirectBucket{
                .m_variant = first.m_variant,
                .m_bag_bucket = first.m_bag_bucket,
                .m_first_arg = first_arg,
                .m_arg_count = group_count,
            });
        }
        return plan;
    }

    Expected<rhi::IRenderPipeline *> TrianglePass::pipelineFor_(
        rhi::IDevice &device,
        const RenderPipelineSignature &signature,
        const TrianglePipelineVariant variant) {
        return pipelineForImpl_(device, signature, variant, m_shader_program.get(), m_variant_pipelines);
    }

    Expected<rhi::IRenderPipeline *> TrianglePass::pipelineForIndirect_(
        rhi::IDevice &device,
        const RenderPipelineSignature &signature,
        const TrianglePipelineVariant variant) {
        return pipelineForImpl_(device, signature, variant, m_indirect_program.get(), m_indirect_pipelines);
    }

    Expected<rhi::IRenderPipeline *> TrianglePass::pipelineForImpl_(
        rhi::IDevice &device,
        const RenderPipelineSignature &signature,
        const TrianglePipelineVariant variant,
        rhi::IShaderProgram *const program,
        FlatMap<TrianglePipelineVariant, rhi::ComPtr<rhi::IRenderPipeline> > &pipelines) {
        if (program == nullptr) [[unlikely]] {
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
            m_indirect_pipelines.clear();
            m_render_pipeline_key.reset();
            m_render_pipeline.setNull();
        }
        if (const auto found = pipelines.find(variant); found != pipelines.end()) {
            PPR_LOG(TrianglePass, debug, "pipeline cache hit");
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
        pipeline_desc.program = program;
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
        pipelines.emplace(variant, std::move(pipeline));
        m_render_pipeline = raw;
        m_render_pipeline_key.emplace(signature);
        return raw;
    }

    // ------------------------------------------------------------------
    // direct instance encoding
    // ------------------------------------------------------------------

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
        if (not range.has_value()) [[unlikely]] {
            return range.error();
        }
        Expected<BagBucketId> bucket = m_bag_cache.bucketOf(instance.m_bag);
        if (not bucket.has_value()) [[unlikely]] {
            return bucket.error();
        }
        rhi::IBuffer *const vertex_buffer = m_bag_cache.vertexBuffer(*bucket);
        rhi::IBuffer *const index_buffer = m_bag_cache.indexBuffer(*bucket);
        if (vertex_buffer == nullptr or index_buffer == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        Expected<GpuMaterial> gpu = m_material_cache.material(instance.m_material);
        if (not gpu.has_value()) [[unlikely]] {
            return gpu.error();
        }
        Expected<u32> material_slot = m_material_cache.materialIndex(instance.m_material);
        if (not material_slot.has_value()) [[unlikely]] {
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
        if (not pipeline.has_value()) [[unlikely]] {
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
            .indexBuffer = {},
        });

        draw_context.m_pass.draw({.vertexCount = range->m_count});

        return default_value_v;
    }

    // Shared resolve for the L1 CPU path and the L2b compute path: same
    // fail-closed validation as encodeInstance_ (stale handles, stride
    // mismatch → error, no partial staging). Zero-count prims are skipped
    // here, so both paths agree on the staged set.
    Expected<Array<TrianglePass::StagedDraw> > TrianglePass::stageDraws_() const {
        Array<StagedDraw> resolved{};
        for (const Instance &instance: m_instances) {
            Expected<TriangleBagRange> range = m_bag_cache.resolve(instance.m_bag);
            if (not range.has_value()) [[unlikely]] {
                return std::unexpected{range.error()};
            }
            if (range->m_count == 0u) {
                continue;
            }
            Expected<BagBucketId> bucket = m_bag_cache.bucketOf(instance.m_bag);
            if (not bucket.has_value()) [[unlikely]] {
                return std::unexpected{bucket.error()};
            }
            rhi::IBuffer *const vertex_buffer = m_bag_cache.vertexBuffer(*bucket);
            rhi::IBuffer *const index_buffer = m_bag_cache.indexBuffer(*bucket);
            if (vertex_buffer == nullptr or index_buffer == nullptr) [[unlikely]] {
                return std::unexpected{make_error_code(std::errc::invalid_argument)};
            }
            Expected<GpuMaterial> gpu = m_material_cache.material(instance.m_material);
            if (not gpu.has_value()) [[unlikely]] {
                return std::unexpected{gpu.error()};
            }
            Expected<u32> material_slot = m_material_cache.materialIndex(instance.m_material);
            if (not material_slot.has_value()) [[unlikely]] {
                return std::unexpected{material_slot.error()};
            }
            if (vertex_buffer->getDesc().elementSize != sizeof(mesh::StaticMeshVertex) or
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
            payload.m_vb_offset = range->m_vb_offset;
            payload.m_ib_start = range->m_ib_start;
            payload.m_index_count = range->m_count;
            payload.m_material = *material_slot;
            payload.m_base_vertex = range->m_base;
            const float *const model_data = instance.m_model.data();
            for (u32 component = 0u; component < 16u; ++component) {
                payload.m_model[component] = model_data[component];
            }
            resolved.push_back(StagedDraw{
                .m_payload = payload,
                .m_variant = variantFor_(*gpu),
                .m_bag_bucket = *bucket,
                .m_count = range->m_count,
                .m_vertex_buffer = vertex_buffer,
                .m_index_buffer = index_buffer,
            });
        }
        return resolved;
    }

    // ------------------------------------------------------------------
    // indirect draw encoding
    // ------------------------------------------------------------------

    // L1 CPU-staged indirect draw: resolve staged instances (same fail-closed
    // validation as encodeInstance_ — no partial staging), plan buckets,
    // rewrite the Upload scratch, one drawIndirect per non-empty bucket.
    // Pixel-identical to render() for the same instance list (parity gate).
    std::error_code TrianglePass::renderIndirect(const DrawContext &draw_context) {
        if (not m_caches_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        if (m_instances.empty()) {
            PPR_LOG_ONCE_KEY(TrianglePass, debug,
                Log::Once::combine(__LINE__, reinterpret_cast<u64>(this)),
                "render skipped: empty scene");
            return default_value_v;
        }
        Expected<Array<StagedDraw> > staged = stageDraws_();
        if (not staged.has_value()) [[unlikely]] {
            return staged.error();
        }
        Array<StagedDraw> &resolved = *staged;
        if (resolved.empty()) {
            return default_value_v;
        }
        Array<TrianglePipelineVariant> variants{};
        Array<BagBucketId> buckets{};
        Array<u32> counts{};
        for (const StagedDraw &draw: resolved) {
            variants.push_back(draw.m_variant);
            buckets.push_back(draw.m_bag_bucket);
            counts.push_back(draw.m_count);
        }
        const auto staged_count = safe_narrowing(resolved.size());
        Expected<IndirectPlan> plan = planIndirectDraws(
            std::span<const TrianglePipelineVariant>{variants.data(), variants.size()},
            std::span<const BagBucketId>{buckets.data(), buckets.size()},
            std::span<const u32>{counts.data(), counts.size()},
            staged_count);
        if (not plan.has_value()) [[unlikely]] {
            return plan.error();
        }
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            ensureIndirectScratch_(draw_context.m_device, plan->m_total_count));
        void *mapped_payloads = nullptr;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            draw_context.m_device.mapBuffer(m_indirect_payloads.get(), rhi::CpuAccessMode::Write, &mapped_payloads));
        for (u32 i = 0u; i < plan->m_total_count; ++i) {
            std::memcpy(
                static_cast<std::byte *>(mapped_payloads) + static_cast<u64>(i) * sizeof(InstancePayload),
                &resolved[i].m_payload,
                sizeof(InstancePayload));
        }
        draw_context.m_device.unmapBuffer(m_indirect_payloads.get());
        void *mapped_args = nullptr;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            draw_context.m_device.mapBuffer(m_indirect_args.get(), rhi::CpuAccessMode::Write, &mapped_args));
        std::memcpy(
            mapped_args,
            plan->m_args.data(),
            static_cast<std::size_t>(plan->m_total_count) * sizeof(rhi::IndirectDrawArguments));
        draw_context.m_device.unmapBuffer(m_indirect_args.get());

        m_texture_heap_bound = false;
        for (const IndirectBucket &bucket: plan->m_buckets) {
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass, encodeIndirectBucket_(draw_context, bucket, plan->m_total_count));
        }
        return default_value_v;
    }

    // Upload scratch ring of one: (re)created when the staged draw count
    // outgrows capacity, rewritten every renderIndirect. Pass-owned, so the
    // buffers outlive the encoded draws; released in shutdown.
    std::error_code TrianglePass::ensureIndirectScratch_(rhi::IDevice &device, const u32 payload_count) {
        if (payload_count == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        const u64 payload_bytes = static_cast<u64>(payload_count) * sizeof(InstancePayload);
        if (m_indirect_payloads == nullptr or m_indirect_payload_capacity < payload_bytes) {
            rhi::BufferDesc payload_desc{};
            payload_desc.size = payload_bytes;
            payload_desc.elementSize = sizeof(InstancePayload);
            payload_desc.memoryType = rhi::MemoryType::Upload;
            payload_desc.usage = rhi::BufferUsage::ShaderResource;
            payload_desc.defaultState = rhi::ResourceState::ShaderResource;
            payload_desc.label = "indirect instance payloads";
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                device.createBuffer(payload_desc, nullptr, m_indirect_payloads.writeRef()));
            m_indirect_payload_capacity = payload_bytes;
        }
        const u64 args_bytes = static_cast<u64>(payload_count) * sizeof(rhi::IndirectDrawArguments);
        if (m_indirect_args == nullptr or m_indirect_arg_capacity < args_bytes) {
            // L0 carry: BufferUsage operator| does not cross the module
            // boundary (ADL), so combined flags compose as u32 and cast back.
            // Keep this shape when L2 adds the counter-UAV binding there —
            // which additionally needs the explicit BufferRange{0, 16}: the
            // default range reaches D3D12 as NumElements 0 and is rejected.
            // The L2 compute kernel itself spells append as Append()
            // (HLSL case), never append().
            constexpr u32 args_usage = static_cast<u32>(rhi::BufferUsage::IndirectArgument);
            rhi::BufferDesc args_desc{};
            args_desc.size = args_bytes;
            args_desc.elementSize = sizeof(rhi::IndirectDrawArguments);
            args_desc.memoryType = rhi::MemoryType::Upload;
            args_desc.usage = static_cast<rhi::BufferUsage>(args_usage);
            args_desc.defaultState = rhi::ResourceState::IndirectArgument;
            args_desc.label = "indirect draw arguments";
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                device.createBuffer(args_desc, nullptr, m_indirect_args.writeRef()));
            m_indirect_arg_capacity = args_bytes;
        }
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // compute-publish indirect path
    // ------------------------------------------------------------------

    u32 TrianglePass::ringSlotVersion(const u32 slot) const noexcept {
        return slot < kIndirectRingFrames ? m_ring[slot].m_version : 0u;
    }

    u64 TrianglePass::ringSlotFence(const u32 slot) const noexcept {
        return slot < kIndirectRingFrames ? m_ring[slot].m_fence_value : 0u;
    }

    void TrianglePass::clearPublished_() noexcept {
        m_published_buckets.clear();
        m_published_count = 0u;
        m_published_slot = kInvalidIndirectSlot;
    }

    void TrianglePass::clearRing_() noexcept {
        for (IndirectRingSlot &entry: m_ring) {
            entry.m_scratch.setNull();
            entry.m_payloads.setNull();
            entry.m_args.setNull();
            entry.m_counter.setNull();
            entry.m_version = 0u;
            entry.m_fence_value = 0u;
            entry.m_busy = false;
        }
        m_publish_fence.setNull();
        m_next_fence_value = 1u;
        clearPublished_();
    }

    // Fixed-capacity device ring (payloads 96 B stride, args 16 B stride,
    // 16 B counter) + per-slot Upload scratch. Buffers are created once;
    // over-count clamps at publish, never grows the slot.
    std::error_code TrianglePass::ensureComputeState_(rhi::IDevice &device) {
        if (m_publish_fence == nullptr) {
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                device.createFence(rhi::FenceDesc{.initialValue = 0u}, m_publish_fence.writeRef()));
        }
        constexpr u64 payload_bytes = static_cast<u64>(kIndirectRingCapacity) * sizeof(InstancePayload);
        constexpr u64 args_bytes = static_cast<u64>(kIndirectRingCapacity) * sizeof(rhi::IndirectDrawArguments);
        for (IndirectRingSlot &entry: m_ring) {
            if (entry.m_scratch == nullptr) {
                rhi::BufferDesc scratch_desc{};
                scratch_desc.size = payload_bytes;
                scratch_desc.elementSize = sizeof(InstancePayload);
                scratch_desc.memoryType = rhi::MemoryType::Upload;
                scratch_desc.usage = rhi::BufferUsage::ShaderResource;
                scratch_desc.defaultState = rhi::ResourceState::ShaderResource;
                scratch_desc.label = "indirect compute scratch";
                PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                    device.createBuffer(scratch_desc, nullptr, entry.m_scratch.writeRef()));
            }
            if (entry.m_payloads == nullptr) {
                rhi::BufferDesc payload_desc{};
                payload_desc.size = payload_bytes;
                payload_desc.elementSize = sizeof(InstancePayload);
                payload_desc.memoryType = rhi::MemoryType::DeviceLocal;
                payload_desc.usage =
                        enumCombine(rhi::BufferUsage::ShaderResource, rhi::BufferUsage::UnorderedAccess);
                payload_desc.defaultState = rhi::ResourceState::ShaderResource;
                payload_desc.label = "indirect device payloads";
                PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                    device.createBuffer(payload_desc, nullptr, entry.m_payloads.writeRef()));
            }
            if (entry.m_args == nullptr) {
                rhi::BufferDesc args_desc{};
                args_desc.size = args_bytes;
                args_desc.elementSize = sizeof(rhi::IndirectDrawArguments);
                args_desc.memoryType = rhi::MemoryType::DeviceLocal;
                args_desc.usage =
                        enumCombine(rhi::BufferUsage::IndirectArgument, rhi::BufferUsage::UnorderedAccess);
                args_desc.defaultState = rhi::ResourceState::IndirectArgument;
                args_desc.label = "indirect device args";
                PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                    device.createBuffer(args_desc, nullptr, entry.m_args.writeRef()));
            }
            if (entry.m_counter == nullptr) {
                rhi::BufferDesc counter_desc{};
                counter_desc.size = kIndirectCounterBytes;
                counter_desc.elementSize = sizeof(u32);
                counter_desc.memoryType = rhi::MemoryType::DeviceLocal;
                counter_desc.usage = rhi::BufferUsage::UnorderedAccess;
                // CopySource resting state: readPublishedCount copies without
                // a transition barrier (the publish submit returns every
                // touched buffer to its default state at close).
                counter_desc.defaultState = rhi::ResourceState::CopySource;
                counter_desc.label = "indirect args counter";
                PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                    device.createBuffer(counter_desc, nullptr, entry.m_counter.writeRef()));
            }
        }
        return default_value_v;
    }

    // A3 retire-by-completed-fence on ring slots only: first reclaim slots
    // whose signal value the fence has passed, then hand out the first free
    // one with a bumped version. Full ring → try-again (GPU still draining).
    Expected<u32> TrianglePass::acquireRingSlot_(const u64 completed) {
        for (IndirectRingSlot &entry: m_ring) {
            if (entry.m_busy and entry.m_fence_value <= completed) {
                entry.m_busy = false;
            }
        }
        for (u32 slot = 0u; slot < kIndirectRingFrames; ++slot) {
            IndirectRingSlot &entry = m_ring[slot];
            if (not entry.m_busy) {
                entry.m_busy = true;
                if (++entry.m_version == 0u) [[unlikely]] {
                    PPR_LOG(TrianglePass, warning, "indirect ring slot version wrapped", {{"slot", slot}});
                    ++entry.m_version;
                }
                entry.m_fence_value = m_next_fence_value;
                ++m_next_fence_value;
                return slot;
            }
        }
        PPR_LOG(TrianglePass, warning, "indirect ring full — GPU still draining",
            {{"completed_fence", completed}, {"frames", kIndirectRingFrames}});
        return std::unexpected{std::make_error_code(std::errc::resource_unavailable_try_again)};
    }

    // Host dispatch/publish: stage scratch on CPU, clear the UAV counter,
    // dispatch (instanceCount,1,1), UAV-barrier the publish, transition args
    // to indirect-read before draw, submit with a fence signal. Barrier
    // points: (B1) counter clear, (B2) UAV-write declare, (B3) global UAV
    // barrier, (B4) args/payload read declare — all on the publish encoder,
    // temporally before any draw of the slot.
    std::error_code TrianglePass::publishIndirectCompute(rhi::IDevice &device) {
        if (not m_caches_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        clearPublished_();
        if (m_instances.empty()) {
            PPR_LOG_ONCE_KEY(TrianglePass, debug,
                Log::Once::combine(__LINE__, reinterpret_cast<u64>(this)),
                "publish skipped: empty scene");
            return default_value_v;
        }
        Expected<Array<StagedDraw> > staged = stageDraws_();
        if (not staged.has_value()) [[unlikely]] {
            return staged.error();
        }
        if (staged->empty()) {
            return default_value_v;
        }
        u32 dispatch_count = static_cast<u32>(staged->size());
        bool over_count = false;
        if (dispatch_count > kIndirectRingCapacity) {
            dispatch_count = kIndirectRingCapacity;
            over_count = true;
        }
        Array<TrianglePipelineVariant> variants{};
        Array<BagBucketId> buckets{};
        Array<u32> counts{};
        for (u32 i = 0u; i < dispatch_count; ++i) {
            variants.push_back((*staged)[i].m_variant);
            buckets.push_back((*staged)[i].m_bag_bucket);
            counts.push_back((*staged)[i].m_count);
        }
        Expected<IndirectPlan> plan = planIndirectDraws(
            std::span<const TrianglePipelineVariant>{variants.data(), variants.size()},
            std::span<const BagBucketId>{buckets.data(), buckets.size()},
            std::span<const u32>{counts.data(), counts.size()},
            kIndirectRingCapacity);
        if (not plan.has_value()) [[unlikely]] {
            return plan.error();
        }
        if (plan->m_total_count == 0u) [[unlikely]] {
            PPR_LOG(TrianglePass, warning, "publish planned zero draws for a non-empty stage",
                {{"staged", staged->size()}});
            return std::make_error_code(std::errc::invalid_argument);
        }
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, ensureComputeState_(device));
        if (m_compute_pipeline == nullptr) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        u64 completed = 0u;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, m_publish_fence->getCurrentValue(&completed));
        Expected<u32> acquired = acquireRingSlot_(completed);
        if (not acquired.has_value()) [[unlikely]] {
            return acquired.error();
        }
        IndirectRingSlot &entry = m_ring[*acquired];
        // A publish that fails after acquire must not leak the slot: the
        // fence value was never signaled, so drop it back to reclaimable.
        bool committed = false;
        PPR_DEFER {
            if (not committed) {
                entry.m_busy = false;
                entry.m_fence_value = 0u;
            }
        };
        void *mapped_scratch = nullptr;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            device.mapBuffer(entry.m_scratch.get(), rhi::CpuAccessMode::Write, &mapped_scratch));
        for (u32 i = 0u; i < dispatch_count; ++i) {
            std::memcpy(
                static_cast<std::byte *>(mapped_scratch) + static_cast<u64>(i) * sizeof(InstancePayload),
                &(*staged)[i].m_payload,
                sizeof(InstancePayload));
        }
        device.unmapBuffer(entry.m_scratch.get());
        rhi::ComPtr<rhi::ICommandQueue> queue{};
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, device.getQueue(rhi::QueueType::Graphics, queue.writeRef()));
        rhi::ComPtr<rhi::ICommandEncoder> encoder{};
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, queue->createCommandEncoder(encoder.writeRef()));
        // (B1) UAV-counter host clear: the kernel appends from zero, so a
        // count-0 dispatch (never issued — empty returns above) would read 0.
        encoder->clearBuffer(entry.m_counter.get(), rhi::BufferRange{
            .offset = 0u,
            .size = kIndirectCounterBytes,
        });
        // (B2) declare the UAV write before the compute pass.
        encoder->setBufferState(entry.m_payloads.get(), rhi::ResourceState::UnorderedAccess);
        encoder->setBufferState(entry.m_args.get(), rhi::ResourceState::UnorderedAccess);
        rhi::IComputePassEncoder *const compute_pass = encoder->beginComputePass();
        if (compute_pass == nullptr) [[unlikely]] {
            return std::make_error_code(std::errc::io_error);
        }
        if (rhi::IShaderObject *const shader_object = compute_pass->bindPipeline(m_compute_pipeline.get());
            PPR_ENSURE(shader_object)) {
            const rhi::ShaderCursor compute_cursor{shader_object};
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                compute_cursor["g_scratch"].setBinding(
                    rhi::Binding(entry.m_scratch.get(), makeFullRange(entry.m_scratch.get()))));
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                compute_cursor["g_device_payloads"].setBinding(
                    rhi::Binding(entry.m_payloads.get(), makeFullRange(entry.m_payloads.get()))));
            // Counter-UAV: explicit full-args range (a default Binding reads
            // {0,0} across the module boundary and D3D12 rejects NumElements
            // 0); the counter resource itself rides resource2.
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                compute_cursor["g_args"].setBinding(rhi::Binding(
                    entry.m_args.get(),
                    entry.m_counter.get(),
                    rhi::BufferRange{
                    .offset = 0u,
                    .size = static_cast<u64>(kIndirectRingCapacity) * sizeof(rhi::IndirectDrawArguments),
                    })));
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                compute_cursor["g_instance_count"].setData(&dispatch_count, sizeof(dispatch_count)));
            constexpr u32 max_count = kIndirectRingCapacity;
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                compute_cursor["g_max_count"].setData(&max_count, sizeof(max_count)));
            compute_pass->dispatchCompute(dispatch_count, 1u, 1u);
        } else {
            return std::make_error_code(std::errc::broken_pipe);
        }
        compute_pass->end();
        // (B3) UAV barrier: scratch→payloads/args publish completes before
        // any later read. (B4) args/payload read states declared before draw
        // (the submit close returns them to default = read states anyway).
        encoder->globalBarrier();
        encoder->setBufferState(entry.m_args.get(), rhi::ResourceState::IndirectArgument);
        encoder->setBufferState(entry.m_payloads.get(), rhi::ResourceState::ShaderResource);
        rhi::ComPtr<rhi::ICommandBuffer> commands{};
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, encoder->finish(commands.writeRef()));
        rhi::ICommandBuffer *commands_raw = commands.get();
        rhi::IFence *fence_raw = m_publish_fence.get();
        u64 signal_value = entry.m_fence_value;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            queue->submit(rhi::SubmitDesc{
                .commandBuffers = &commands_raw,
                .commandBufferCount = 1u,
                .signalFences = &fence_raw,
                .signalFenceValues = &signal_value,
                .signalFenceCount = 1u,
                }));
        committed = true;
        m_published_buckets.clear();
        for (const IndirectBucket &bucket: plan->m_buckets) {
            rhi::IBuffer *vertex_buffer = nullptr;
            rhi::IBuffer *index_buffer = nullptr;
            for (u32 i = 0u; i < dispatch_count; ++i) {
                if ((*staged)[i].m_bag_bucket == bucket.m_bag_bucket) {
                    vertex_buffer = (*staged)[i].m_vertex_buffer;
                    index_buffer = (*staged)[i].m_index_buffer;
                    break;
                }
            }
            if (vertex_buffer == nullptr or index_buffer == nullptr) [[unlikely]] {
                clearPublished_();
                entry.m_busy = false;
                entry.m_fence_value = 0u;
                return std::make_error_code(std::errc::invalid_argument);
            }
            m_published_buckets.push_back(PublishedBucket{
                .m_variant = bucket.m_variant,
                .m_bag_bucket = bucket.m_bag_bucket,
                .m_vertex_buffer = vertex_buffer,
                .m_index_buffer = index_buffer,
                .m_first_arg = bucket.m_first_arg,
                .m_arg_count = bucket.m_arg_count,
            });
        }
        m_published_count = plan->m_total_count;
        m_published_slot = *acquired;
        if (over_count) {
            // Warning, not error: the clamp is the handled contract (the
            // caller still gets invalid_argument), and the test harness
            // fails cases on error-level logs.
            PPR_LOG(TrianglePass, warning, "publish clamped to ring capacity", {
                {"staged", staged->size()},
                {"capacity", kIndirectRingCapacity},
                });
            return std::make_error_code(std::errc::invalid_argument);
        }
        return default_value_v;
    }

    // GPU-written count: the kernel's UAV counter after the publish submit
    // drains (caller waits on host first). Must equal the CPU staged count
    // (clamped) — any divergence is a publish bug, never drift.
    Expected<u32> TrianglePass::readPublishedCount(rhi::IDevice &device) {
        if (not m_caches_ready) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        if (m_published_slot >= kIndirectRingFrames) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        const IndirectRingSlot &entry = m_ring[m_published_slot];
        u32 count = 0u;
        PPR_RETURN_UNEXPECTED_ON_FAIL(TrianglePass,
            device.readBuffer(entry.m_counter.get(), 0u, sizeof(u32), &count));
        return count;
    }

    // Compute-path draw: one drawIndirect per published bucket over the
    // kernel-published device args; payloads come from the published ring
    // slot via SV_InstanceID. No publication (empty scene or count-0)
    // draws nothing.
    std::error_code TrianglePass::renderIndirectCompute(const DrawContext &draw_context) {
        if (not m_caches_ready) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        if (m_published_slot >= kIndirectRingFrames or m_published_count == 0u or
            m_published_buckets.empty()) {
            PPR_LOG_ONCE_KEY(TrianglePass, debug,
                Log::Once::combine(__LINE__, reinterpret_cast<u64>(this)),
                "draw skipped: nothing published");
            return default_value_v;
        }
        m_texture_heap_bound = false;
        for (const PublishedBucket &bucket: m_published_buckets) {
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass, encodeIndirectComputeBucket_(draw_context, bucket));
        }
        return default_value_v;
    }

    std::error_code TrianglePass::encodeIndirectComputeBucket_(
        const DrawContext &draw_context, const PublishedBucket &bucket) {
        if (bucket.m_arg_count == 0u or m_published_slot >= kIndirectRingFrames) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        const IndirectRingSlot &entry = m_ring[m_published_slot];
        rhi::IBuffer *const material_buffer = m_material_cache.materialBuffer();
        rhi::IBuffer *const texture_buffer = m_texture_cache.descriptorBuffer();
        if (bucket.m_vertex_buffer == nullptr or bucket.m_index_buffer == nullptr or material_buffer == nullptr or
            texture_buffer == nullptr or entry.m_payloads == nullptr or entry.m_args == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<rhi::IRenderPipeline *> pipeline =
                pipelineForIndirect_(draw_context.m_device, draw_context.m_render_pipeline_key, bucket.m_variant);
        if (not pipeline.has_value()) [[unlikely]] {
            return pipeline.error();
        }

        rhi::ShaderCursor shader_cursor{};
        if (rhi::IShaderObject *const shader_object = draw_context.m_pass.bindPipeline(*pipeline); PPR_ENSURE(shader_object)) {
            shader_cursor = rhi::ShaderCursor(shader_object);
        } else {
            return make_error_code(std::errc::broken_pipe);
        }

        rhi::ShaderCursor frame_cursor{};
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_frame"].getDereferenced(frame_cursor));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, uploadFrameConstants_(frame_cursor));

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_vertices"].setBinding(
                rhi::Binding(bucket.m_vertex_buffer, makeFullRange(bucket.m_vertex_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_indices"].setBinding(
                rhi::Binding(bucket.m_index_buffer, makeFullRange(bucket.m_index_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_materials"].setBinding(rhi::Binding(material_buffer, makeFullRange(material_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_payloads"].setBinding(
                rhi::Binding(entry.m_payloads.get(), makeFullRange(entry.m_payloads.get()))));

        if (not m_texture_heap_bound) {
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                shader_cursor["g_textures"].setBinding(rhi::Binding(texture_buffer, makeFullRange(texture_buffer))));
            m_texture_heap_bound = true;
        }
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_sampler"].setDescriptorHandle(m_sampler_handle));

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
        });

        // Kernel-published records: maxDrawCount == published arg count, no
        // count buffer (count-0 publishes never reach here).
        draw_context.m_pass.drawIndirect(
            bucket.m_arg_count,
            rhi::BufferOffsetPair{
                entry.m_args.get(), static_cast<u64>(bucket.m_first_arg) * sizeof(rhi::IndirectDrawArguments)
            });
        return default_value_v;
    }

    // One drawIndirect over the bucket's contiguous args slice. Buffers come
    // from the bucket's first record (plan groups by bag bucket, so every
    // record in the range shares them); the payload buffer selects the
    // per-draw instance via startInstanceLocation → SV_InstanceID.
    std::error_code TrianglePass::encodeIndirectBucket_(
        const DrawContext &draw_context, const IndirectBucket &bucket, const u32 payload_count) {
        if (bucket.m_arg_count == 0u or bucket.m_first_arg + bucket.m_arg_count > payload_count) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }
        rhi::IBuffer *vertex_buffer = nullptr;
        rhi::IBuffer *index_buffer = nullptr;
        for (const Instance &instance: m_instances) {
            Expected<BagBucketId> instance_bucket = m_bag_cache.bucketOf(instance.m_bag);
            if (instance_bucket.has_value() and *instance_bucket == bucket.m_bag_bucket) {
                vertex_buffer = m_bag_cache.vertexBuffer(*instance_bucket);
                index_buffer = m_bag_cache.indexBuffer(*instance_bucket);
                break;
            }
        }
        rhi::IBuffer *const material_buffer = m_material_cache.materialBuffer();
        rhi::IBuffer *const texture_buffer = m_texture_cache.descriptorBuffer();
        if (vertex_buffer == nullptr or index_buffer == nullptr or material_buffer == nullptr or
            texture_buffer == nullptr or m_indirect_payloads == nullptr or m_indirect_args == nullptr) [[unlikely]] {
            return make_error_code(std::errc::invalid_argument);
        }

        Expected<rhi::IRenderPipeline *> pipeline =
                pipelineForIndirect_(draw_context.m_device, draw_context.m_render_pipeline_key, bucket.m_variant);
        if (not pipeline.has_value()) [[unlikely]] {
            return pipeline.error();
        }

        rhi::ShaderCursor shader_cursor{};
        if (rhi::IShaderObject *const shader_object = draw_context.m_pass.bindPipeline(*pipeline); PPR_ENSURE(shader_object)) {
            shader_cursor = rhi::ShaderCursor(shader_object);
        } else {
            return make_error_code(std::errc::broken_pipe);
        }

        rhi::ShaderCursor frame_cursor{};
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_frame"].getDereferenced(frame_cursor));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, uploadFrameConstants_(frame_cursor));

        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_vertices"].setBinding(rhi::Binding(vertex_buffer, makeFullRange(vertex_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_indices"].setBinding(rhi::Binding(index_buffer, makeFullRange(index_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_materials"].setBinding(rhi::Binding(material_buffer, makeFullRange(material_buffer))));
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            shader_cursor["g_payloads"].setBinding(
                rhi::Binding(m_indirect_payloads.get(), makeFullRange(m_indirect_payloads.get()))));

        if (not m_texture_heap_bound) {
            PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
                shader_cursor["g_textures"].setBinding(rhi::Binding(texture_buffer, makeFullRange(texture_buffer))));
            m_texture_heap_bound = true;
        }
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, shader_cursor["g_sampler"].setDescriptorHandle(m_sampler_handle));

        draw_context.m_pass.setRenderState({
            .viewports = {draw_context.m_viewport},
            .viewportCount = 1u,
            .scissorRects = {draw_context.m_scissor},
            .scissorRectCount = 1u,
        });

        // No count buffer in L1 (CPU-staged count == maxCount); L2 wires the
        // kernel-published u32 header through countBuffer. Count-0 plans never
        // reach here — renderIndirect returns before encoding.
        draw_context.m_pass.drawIndirect(
            bucket.m_arg_count,
            rhi::BufferOffsetPair{
                m_indirect_args.get(), static_cast<u64>(bucket.m_first_arg) * sizeof(rhi::IndirectDrawArguments)
            });
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // teardown and resource initialization
    // ------------------------------------------------------------------

    std::error_code TrianglePass::shutdown() {
        PPR_LOG(TrianglePass, info, "TrianglePass shut down", {
            {"has_pipeline", m_render_pipeline != nullptr},
            {"instances", m_instances.size()},
            });

        // §2.4 teardown: caches → sampler → pipeline/program/layout/buffers
        // (retain-first-error, best-effort), BEFORE renderer waitOnHost.
        std::error_code first_err{};
        m_instances.clear();
        m_live_scenes.clear();
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_material_cache.shutdown());
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_texture_cache.shutdown());
        PPR_RETAIN_ERROR_ON_FAIL(TrianglePass, first_err, m_bag_cache.shutdown());
        m_shared_sampler.setNull();
        m_caches_ready = false;

        m_variant_pipelines.clear();
        m_indirect_pipelines.clear();
        m_render_pipeline_key.reset();
        m_render_pipeline.setNull();
        m_compute_pipeline.setNull();
        m_compute_program.setNull();
        m_indirect_program.setNull();
        m_shader_program.setNull();
        clearRing_();
        m_indirect_payloads.setNull();
        m_indirect_payload_capacity = 0u;
        m_indirect_args.setNull();
        m_indirect_arg_capacity = 0u;
        m_fallback_view.setNull();
        m_fallback_texture.setNull();
        m_sampler_handle = rhi::DescriptorHandle{};
        m_texture_heap_bound = false;
        return first_err;
    }

    std::error_code TrianglePass::notifyDeviceLost() noexcept {
        // §2.4 order (material → texture → bag), retain-first-error like
        // shutdown. Live-scene receipts are kept: releaseScene still drains
        // the retained CPU records; only GPU-touching calls park.
        std::error_code first_err{};
        m_instances.clear();
        m_indirect_pipelines.clear();
        // Device loss discards ring retirement outright (GPU objects are
        // gone; nothing in flight is worth waiting for) — programs survive
        // like the render programs, pipelines do not.
        m_compute_pipeline.setNull();
        clearRing_();
        m_indirect_payloads.setNull();
        m_indirect_payload_capacity = 0u;
        m_indirect_args.setNull();
        m_indirect_arg_capacity = 0u;
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
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, triangle_shader->findEntryPointByName("vertexMain", vertex_ep.writeRef()));

        shader::ComPtr<slang::IEntryPoint> fragment_ep;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, triangle_shader->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

        // Phase 7 L1 indirect entry: same fragment, payload-indexed vertex.
        shader::ComPtr<slang::IEntryPoint> indirect_ep;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, triangle_shader->findEntryPointByName("vertexIndirectMain", indirect_ep.writeRef()));

        slang::IComponentType *entry_points[] = {vertex_ep.get(), fragment_ep.get()};

        rhi::ShaderProgramDesc program_desc{};
        program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        program_desc.slangGlobalScope = triangle_shader.get();
        program_desc.slangEntryPoints = entry_points;
        program_desc.slangEntryPointCount = 2u;

        shader::Diagnose diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, device.createShaderProgram(program_desc, m_shader_program.writeRef(), diagnostics.writeRef()));

        slang::IComponentType *indirect_entries[] = {indirect_ep.get(), fragment_ep.get()};
        rhi::ShaderProgramDesc indirect_desc{};
        indirect_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        indirect_desc.slangGlobalScope = triangle_shader.get();
        indirect_desc.slangEntryPoints = indirect_entries;
        indirect_desc.slangEntryPointCount = 2u;

        shader::Diagnose indirect_diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            device.createShaderProgram(indirect_desc, m_indirect_program.writeRef(), indirect_diagnostics.writeRef()));

        // Phase 7 L2b compute entry: publishes device payloads + args
        // records from Upload scratch (prepareInstancesMain, entry params
        // only — the shader file itself is untouched by this lane).
        shader::ComPtr<slang::IEntryPoint> compute_ep;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass, triangle_shader->findEntryPointByName("prepareInstancesMain", compute_ep.writeRef()));

        slang::IComponentType *compute_entries[] = {compute_ep.get()};
        rhi::ShaderProgramDesc compute_desc{};
        compute_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
        compute_desc.slangGlobalScope = triangle_shader.get();
        compute_desc.slangEntryPoints = compute_entries;
        compute_desc.slangEntryPointCount = 1u;

        shader::Diagnose compute_diagnostics;
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            device.createShaderProgram(compute_desc, m_compute_program.writeRef(), compute_diagnostics.writeRef()));

        rhi::ComputePipelineDesc compute_pipeline_desc{};
        compute_pipeline_desc.program = m_compute_program.get();
        compute_pipeline_desc.label = "prepare instances pipeline";
        PPR_RETURN_ERROR_ON_FAIL(TrianglePass,
            device.createComputePipeline(compute_pipeline_desc, m_compute_pipeline.writeRef()));

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
