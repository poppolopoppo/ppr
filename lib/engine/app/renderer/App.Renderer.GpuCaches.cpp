module;
#include "pP/Macros.h"
module engine.app;

import :renderer.gpu_caches;

import std;
import engine.core;
import engine.math;
import engine.rhi;
import engine.image;
import engine.mesh;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(GpuCaches, debug, none)

    const TextureBindlessIndex kNoTexture { 0xFFFFFFFFu };

    namespace {
        // Budgets are exported (kTriangleBagVertexCapacity and friends) so
        // telemetry tests name the same limits the caches enforce.

        [[nodiscard]] bool storageBytesEqual_(
            const mem::SharedBuffer &lhs, const mem::SharedBuffer &rhs) noexcept {
            const mem::SharedBufferView lhs_view = lhs.getBufferData();
            const mem::SharedBufferView rhs_view = rhs.getBufferData();
            if (lhs_view.size() != rhs_view.size()) {
                return false;
            }
            if (lhs_view.empty()) {
                return true;
            }
            return std::memcmp(lhs_view.data(), rhs_view.data(), lhs_view.size()) == 0;
        }
    }

    Expected<GpuMaterial> buildGpuMaterial(
        const mesh::MaterialAsset &asset,
        const GpuTextureRefs resolved) noexcept {
        if (asset.m_alpha_mode == mesh::AlphaMode::blend) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
        }

        mesh::UvSetId shared_set{};
        bool has_set = false;
        const mesh::MaterialImageSlot *const slots[] = {
            &asset.m_base_color_map,
            &asset.m_metallic_map,
            &asset.m_roughness_map,
            &asset.m_normal_map,
            &asset.m_occlusion_map,
            &asset.m_emissive_map,
        };
        for (const mesh::MaterialImageSlot *const slot: slots) {
            if (not
                slot->enabled())
            {
                continue;
            }
            if (not has_set) {
                shared_set = slot->m_texcoord;
                has_set = true;
            } else if (slot->m_texcoord != shared_set) [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
        }

        // Plain-float-array boundary copy (P1 verdict): no math lives in the
        // vertex/material DTOs; conversion happens here only.
        GpuMaterial gpu{};
        gpu.m_base_color[0] = asset.m_base_color.x;
        gpu.m_base_color[1] = asset.m_base_color.y;
        gpu.m_base_color[2] = asset.m_base_color.z;
        gpu.m_base_color[3] = asset.m_base_color.w;
        gpu.m_emissive_metallic[0] = asset.m_emissive.x;
        gpu.m_emissive_metallic[1] = asset.m_emissive.y;
        gpu.m_emissive_metallic[2] = asset.m_emissive.z;
        gpu.m_emissive_metallic[3] = asset.m_metallic;
        gpu.m_rough_alpha_occl_nscale[0] = asset.m_roughness;
        gpu.m_rough_alpha_occl_nscale[1] = asset.m_alpha_cutoff;
        gpu.m_rough_alpha_occl_nscale[2] = asset.m_occlusion_strength;
        gpu.m_rough_alpha_occl_nscale[3] = asset.m_normal_scale;
        gpu.m_textures = resolved;
        gpu.m_texcoord = shared_set;
        gpu.m_flags.m_bits = enumOrd(asset.m_alpha_mode) & kGpuMaterialAlphaModeMask;
        if (asset.m_twosided) {
            gpu.m_flags.m_bits |= kGpuMaterialDoubleSidedBit;
        }
        return gpu;
    }

    std::error_code checkPipelineVariant(const TrianglePipelineVariant variant) noexcept {
        if (variant.m_alpha == mesh::AlphaMode::blend) [[unlikely]] {
            return std::make_error_code(std::errc::function_not_supported);
        }
        return default_value_v;
    }

    // ------------------------------------------------------------------
    // TriangleBagCache
    // ------------------------------------------------------------------

    std::error_code TriangleBagCache::initialize(rhi::IDevice &device) {
        if (m_residency == CacheResidency::device_lost) [[unlikely]] {
            return std::make_error_code(std::errc::device_or_resource_busy);
        }
        if (m_initialized) {
            PPR_LOG(GpuCaches, warning, "TriangleBagCache already initialized");
            return default_value_v;
        }
        if (not
            device.hasFeature(rhi::Feature::Bindless))
        [[unlikely]] {
            PPR_LOG(GpuCaches, error, "TriangleBagCache requires bindless support");
            return std::make_error_code(std::errc::function_not_supported);
        }
        m_device = &device;
        m_owner = std::this_thread::get_id();
        m_initialized = true;
        m_residency = CacheResidency::ready;
        return default_value_v;
    }

    std::error_code TriangleBagCache::shutdown() {
        if (m_initialized and not onRenderThread_())
        [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        for (BagBucket &bucket: m_buckets) {
            bucket.m_vertex_buffer.setNull();
            bucket.m_index_buffer.setNull();
        }
        m_buckets.clear();
        m_ranges.clear();
        m_layout_to_bucket.clear();
        m_owner = std::thread::id{};
        m_device = nullptr;
        m_initialized = false;
        m_residency = CacheResidency::uninitialized;
        return default_value_v;
    }

    CacheResidency TriangleBagCache::residency() const noexcept {
        return m_residency;
    }

    std::error_code TriangleBagCache::notifyDeviceLost() noexcept {
        if (m_residency != CacheResidency::ready) {
            return default_value_v;
        }
        if (not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        dropGpuObjects_();
        m_residency = CacheResidency::device_lost;
        return default_value_v;
    }

    void TriangleBagCache::dropGpuObjects_() noexcept {
        for (BagBucket &bucket: m_buckets) {
            bucket.m_vertex_buffer.setNull();
            bucket.m_index_buffer.setNull();
        }
    }

    bool TriangleBagCache::onRenderThread_() const noexcept {
        return std::this_thread::get_id() == m_owner;
    }

    hash_t TriangleBagCache::layoutKey_(const u64 stride) noexcept {
        return hash::combine(hash_t{hash::default_seed_v}, hash::trivial(&stride, hash::default_seed_v));
    }

    const TriangleBagCache::BagRangeRecord *TriangleBagCache::findRecord_(const SparseHandle key) const noexcept {
        if (not key.isValid() or m_residency == CacheResidency::device_lost)
        [[unlikely]] {
            return nullptr;
        }
        // Composed lookup: the container checks the full 32-bit generation
        // and the record carries the minted identity alongside — a wrapping
        // 8-bit seed alone never resolves.
        if (const BagRangeRecord *const record = m_ranges.tryGet(key);
            record != nullptr and record->m_identity == key)
        [[likely]] {
            return record;
        }
        return nullptr;
    }

    Expected<TriangleBagHandle> TriangleBagCache::uploadBytes_(
        const std::span<const std::byte> vert_bytes,
        const u64 stride,
        const std::span<const u32> idx,
        const i32 base) {
        if (not m_initialized or m_device == nullptr)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        if (m_residency == CacheResidency::device_lost)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_such_device)};
        }
        if (not onRenderThread_())
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }
        if (vert_bytes.empty() or idx.empty() or stride == 0u)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        if (vert_bytes.size() % stride != 0u) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        const u64 vert_count = vert_bytes.size() / stride;
        const u64 vert_bytes_size = vert_bytes.size();
        const u64 index_bytes_size = idx.size_bytes();
        if (vert_count > static_cast<u64>(std::numeric_limits<u32>::max()) or
            idx.size() > static_cast<u64>(std::numeric_limits<u32>::max()))
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        const hash_t layout = layoutKey_(stride);
        BagBucketId bucket_id{0u};
        if (const auto found = m_layout_to_bucket.find(layout); found != m_layout_to_bucket.end()) {
            bucket_id = found->second;
        } else {
            rhi::BufferDesc vb_desc{};
            vb_desc.size = kTriangleBagVertexCapacity;
            vb_desc.elementSize = safe_narrowing(stride);
            vb_desc.memoryType = rhi::MemoryType::Upload;
            vb_desc.usage = rhi::BufferUsage::ShaderResource;
            vb_desc.defaultState = rhi::ResourceState::ShaderResource;
            vb_desc.label = "triangle bag vertices";

            rhi::BufferDesc ib_desc{};
            ib_desc.size = kTriangleBagIndexCapacity;
            ib_desc.elementSize = sizeof(u32);
            ib_desc.memoryType = rhi::MemoryType::Upload;
            ib_desc.usage = rhi::BufferUsage::ShaderResource;
            ib_desc.defaultState = rhi::ResourceState::ShaderResource;
            ib_desc.label = "triangle bag indices";

            BagBucket bucket{};
            bucket.m_stride = safe_narrowing(stride);
            bucket.m_vertex_capacity = kTriangleBagVertexCapacity;
            bucket.m_index_capacity = kTriangleBagIndexCapacity;
            PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches,
                m_device->createBuffer(vb_desc, nullptr, bucket.m_vertex_buffer.writeRef()));
            PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches,
                m_device->createBuffer(ib_desc, nullptr, bucket.m_index_buffer.writeRef()));

            bucket_id = BagBucketId{safe_narrowing(m_buckets.size())};
            m_buckets.push_back(std::move(bucket));
            m_layout_to_bucket.emplace(layout, bucket_id);
        }

        BagBucket &bucket = m_buckets[*bucket_id];
        if (static_cast<u64>(bucket.m_stride) != stride) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        if (bucket.m_vertex_used + vert_bytes_size > bucket.m_vertex_capacity or
            bucket.m_index_used + index_bytes_size > bucket.m_index_capacity)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_buffer_space)};
        }

        void *mapped_verts = nullptr;
        PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches,
            m_device->mapBuffer(bucket.m_vertex_buffer.get(), rhi::CpuAccessMode::Write, &mapped_verts));
        std::memcpy(static_cast<std::byte *>(mapped_verts) + bucket.m_vertex_used, vert_bytes.data(), vert_bytes_size);
        m_device->unmapBuffer(bucket.m_vertex_buffer.get());

        void *mapped_idx = nullptr;
        PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches,
            m_device->mapBuffer(bucket.m_index_buffer.get(), rhi::CpuAccessMode::Write, &mapped_idx));
        std::memcpy(static_cast<std::byte *>(mapped_idx) + bucket.m_index_used, idx.data(), index_bytes_size);
        m_device->unmapBuffer(bucket.m_index_buffer.get());

        const u64 vb_offset = bucket.m_vertex_used / stride;
        const u64 ib_start = bucket.m_index_used / sizeof(u32);
        bucket.m_vertex_used += vert_bytes_size;
        bucket.m_index_used += index_bytes_size;

        BagRangeRecord record{};
        record.m_bucket = bucket_id;
        record.m_range = TriangleBagRange{
            .m_vb_offset = safe_narrowing(vb_offset),
            .m_ib_start = safe_narrowing(ib_start),
            .m_count = safe_narrowing(idx.size()),
            .m_base = base,
        };
        const auto [identity, it] = m_ranges.emplaceHandle(std::move(record));
        it->m_identity = identity;
        return TriangleBagHandle{identity};
    }

    Expected<TriangleBagRange> TriangleBagCache::resolve(const TriangleBagHandle handle) const noexcept {
        if (const BagRangeRecord *const record = findRecord_(*handle)) [[likely]] {
            return record->m_range;
        }
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }

    Expected<BagBucketId> TriangleBagCache::bucketOf(const TriangleBagHandle handle) const noexcept {
        if (const BagRangeRecord *const record = findRecord_(*handle)) [[likely]] {
            return record->m_bucket;
        }
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }

    std::error_code TriangleBagCache::release(const TriangleBagHandle handle) noexcept {
        const SparseHandle key = *handle;
        if (not
            key.isValid())
        [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (not onRenderThread_())
        [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        // Allowed while device_lost (CPU record only; GPU buffers are gone).
        if (not
            m_ranges.erase(key))
        [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return default_value_v;
    }

    rhi::IBuffer *TriangleBagCache::vertexBuffer(const BagBucketId bucket) const noexcept {
        if (m_residency == CacheResidency::device_lost) [[unlikely]] {
            return nullptr;
        }
        return *bucket < m_buckets.size() ? m_buckets[*bucket].m_vertex_buffer.get() : nullptr;
    }

    rhi::IBuffer *TriangleBagCache::indexBuffer(const BagBucketId bucket) const noexcept {
        if (m_residency == CacheResidency::device_lost) [[unlikely]] {
            return nullptr;
        }
        return *bucket < m_buckets.size() ? m_buckets[*bucket].m_index_buffer.get() : nullptr;
    }

    u64 TriangleBagCache::vertexUsed() const noexcept {
        u64 used = 0u;
        for (const BagBucket &bucket: m_buckets) {
            used += bucket.m_vertex_used;
        }
        return used;
    }

    u64 TriangleBagCache::vertexCapacity() const noexcept {
        u64 capacity = 0u;
        for (const BagBucket &bucket: m_buckets) {
            capacity += bucket.m_vertex_capacity;
        }
        return capacity;
    }

    u64 TriangleBagCache::indexUsed() const noexcept {
        u64 used = 0u;
        for (const BagBucket &bucket: m_buckets) {
            used += bucket.m_index_used;
        }
        return used;
    }

    u64 TriangleBagCache::indexCapacity() const noexcept {
        u64 capacity = 0u;
        for (const BagBucket &bucket: m_buckets) {
            capacity += bucket.m_index_capacity;
        }
        return capacity;
    }

    u64 TriangleBagCache::rangeCount() const noexcept {
        return static_cast<u64>(m_ranges.size());
    }

    // ------------------------------------------------------------------
    // BindlessTextureCache
    // ------------------------------------------------------------------

    std::error_code BindlessTextureCache::initialize(
        rhi::IDevice &device, const u32 texture_budget, const rhi::DescriptorHandle fallback_descriptor) {
        if (m_residency == CacheResidency::device_lost) [[unlikely]] {
            return std::make_error_code(std::errc::device_or_resource_busy);
        }
        if (m_initialized) {
            PPR_LOG(GpuCaches, warning, "BindlessTextureCache already initialized");
            return default_value_v;
        }
        if (not
            device.hasFeature(rhi::Feature::Bindless))
        [[unlikely]] {
            PPR_LOG(GpuCaches, error, "BindlessTextureCache requires bindless support");
            return std::make_error_code(std::errc::function_not_supported);
        }
        if (texture_budget == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        rhi::BufferDesc descriptor_desc{};
        descriptor_desc.size = static_cast<u64>(texture_budget) * sizeof(u64);
        descriptor_desc.elementSize = sizeof(u64);
        descriptor_desc.memoryType = rhi::MemoryType::Upload;
        descriptor_desc.usage = rhi::BufferUsage::ShaderResource;
        descriptor_desc.defaultState = rhi::ResourceState::ShaderResource;
        descriptor_desc.label = "bindless texture descriptors";
        PPR_RETURN_ERROR_ON_FAIL(GpuCaches, device.createBuffer(descriptor_desc, nullptr, m_descriptor_buffer.writeRef()));
        const u64 packed_fallback = fallback_descriptor.value;
        void *mapped = nullptr;
        PPR_RETURN_ERROR_ON_FAIL(GpuCaches, device.mapBuffer(m_descriptor_buffer.get(), rhi::CpuAccessMode::Write, &mapped));
        std::memcpy(mapped, &packed_fallback, sizeof(packed_fallback));
        device.unmapBuffer(m_descriptor_buffer.get());

        m_device = &device;
        m_texture_budget = texture_budget;
        m_next_slot = 1u;
        m_owner = std::this_thread::get_id();
        m_initialized = true;
        m_residency = CacheResidency::ready;
        return default_value_v;
    }

    std::error_code BindlessTextureCache::shutdown() {
        if (m_initialized and not onRenderThread_())
        [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        m_dedup.clear();
        m_entries.clear();
        m_descriptor_buffer.setNull();
        m_next_slot = 1u;
        m_texture_budget = 0u;
        m_owner = std::thread::id{};
        m_device = nullptr;
        m_initialized = false;
        m_residency = CacheResidency::uninitialized;
        return default_value_v;
    }

    CacheResidency BindlessTextureCache::residency() const noexcept {
        return m_residency;
    }

    std::error_code BindlessTextureCache::notifyDeviceLost() noexcept {
        if (m_residency != CacheResidency::ready) {
            return default_value_v;
        }
        if (not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        dropGpuObjects_();
        m_residency = CacheResidency::device_lost;
        return default_value_v;
    }

    void BindlessTextureCache::dropGpuObjects_() noexcept {
        m_descriptor_buffer.setNull();
        for (TextureEntry &entry: m_entries) {
            entry.m_texture.setNull();
            entry.m_view.setNull();
        }
    }

    bool BindlessTextureCache::onRenderThread_() const noexcept {
        return std::this_thread::get_id() == m_owner;
    }

    BindlessTextureCache::DedupKey BindlessTextureCache::dedupKey_(const image::ImageAsset &asset) noexcept {
        return DedupKey{
            .m_hash = image::contentHash(asset.m_storage.getBufferData()),
            .m_width = asset.m_width,
            .m_height = asset.m_height,
            .m_format = asset.m_format,
            .m_mips = asset.m_mip_count,
        };
    }

    bool BindlessTextureCache::bytesEqual_(const mem::SharedBuffer &pinned, const image::ImageAsset &asset) noexcept {
        return storageBytesEqual_(pinned, asset.m_storage);
    }

    Expected<rhi::Format> BindlessTextureCache::uploadFormat_(const image::NativeImageFormat format) noexcept {
        switch (format) {
            case image::NativeImageFormat::rgba8_linear: return rhi::Format::RGBA8Unorm;
            case image::NativeImageFormat::rgba8_srgb: return rhi::Format::RGBA8UnormSrgb;
            default: return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
        }
    }

    const BindlessTextureCache::TextureEntry *BindlessTextureCache::findEntry_(const SparseHandle key) const noexcept {
        if (not key.isValid() or m_residency == CacheResidency::device_lost)
        [[unlikely]] {
            return nullptr;
        }
        // Composed lookup: full-generation container check plus the minted
        // identity stored alongside — a wrapping seed alone never resolves.
        if (const TextureEntry *const entry = m_entries.tryGet(key);
            entry != nullptr and entry->m_identity == key) [[likely]] {
            return entry;
        }
        return nullptr;
    }

    Expected<TextureHandle> BindlessTextureCache::upload(const image::ImageAsset &asset) {
        if (not m_initialized or m_device == nullptr)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        if (m_residency == CacheResidency::device_lost)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_such_device)};
        }
        if (not onRenderThread_())
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }
        if (asset.m_dimension != image::ImageDimension::image2d or
            asset.m_width == 0u or
            asset.m_height == 0u or
            asset.m_mip_count == 0u or
            asset.m_subresources.size() != asset.m_mip_count)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        if (not asset.m_storage.isValid() or not asset.m_storage.isMaterialized())
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches, uploadFormat_(asset.m_format));

        const DedupKey key = dedupKey_(asset);
        if (const auto found = m_dedup.find(key); found != m_dedup.end()) {
            // Handle-first: the dedup map names the entry directly, so one
            // generation-checked lookup plus a single confirming byte-compare
            // replaces the old linear scan (hash alone never decides). The
            // composed identity must match as well as the container
            // generation — a recycled slot with a wrapping seed never aliases.
            if (TextureEntry *const entry = m_entries.tryGet(*found->second);
                entry != nullptr and
                entry->m_identity == *found->second and
                bytesEqual_(entry->m_pinned, asset))
            {
                ++entry->m_refcount;
                return found->second;
            }
        }

        if (m_next_slot >= m_texture_budget) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_buffer_space)};
        }
        const rhi::Format format = *uploadFormat_(asset.m_format);
        rhi::FormatSupport support{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches, m_device->getFormatSupport(format, &support));
        if ((static_cast<u32>(support) & static_cast<u32>(rhi::FormatSupport::ShaderSample)) == 0u) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
        }

        Array<rhi::SubresourceData> init_data{};
        init_data.reserve(asset.m_mip_count);
        for (const image::ImageSubresource &sub: asset.m_subresources) {
            if (not sub.m_view.isValid() or not sub.m_view.isMaterialized())
            [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
            const mem::SharedBufferView bytes = sub.m_view.getBufferData();
            if (bytes.empty() or sub.m_row_pitch == 0u or sub.m_slice_pitch == 0u)
            [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
            init_data.push_back(rhi::SubresourceData{
                .data = bytes.data(),
                .rowPitch = safe_narrowing(sub.m_row_pitch),
                .slicePitch = safe_narrowing(sub.m_slice_pitch),
            });
        }

        rhi::TextureDesc texture_desc{};
        texture_desc.type = rhi::TextureType::Texture2D;
        texture_desc.size = {asset.m_width, asset.m_height, 1u};
        texture_desc.arrayLength = 1u;
        texture_desc.mipCount = asset.m_mip_count;
        texture_desc.format = format;
        texture_desc.memoryType = rhi::MemoryType::DeviceLocal;
        texture_desc.usage = rhi::TextureUsage::ShaderResource;
        texture_desc.defaultState = rhi::ResourceState::ShaderResource;
        texture_desc.label = "bindless texture";

        TextureEntry entry{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches,
            m_device->createTexture(texture_desc, init_data.data(), entry.m_texture.writeRef()));
        PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches, entry.m_texture->getDefaultView(entry.m_view.writeRef()));
        PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches,
            entry.m_view->getDescriptorHandle(rhi::DescriptorHandleAccess::Read, &entry.m_descriptor));

        entry.m_slot = TextureBindlessIndex{m_next_slot};
        const u64 packed_descriptor = entry.m_descriptor.value;
        void *mapped = nullptr;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GpuCaches, m_device->mapBuffer(m_descriptor_buffer.get(), rhi::CpuAccessMode::Write, &mapped));
        std::memcpy(
            static_cast<std::byte *>(mapped) + static_cast<u64>(*entry.m_slot) * sizeof(u64),
            &packed_descriptor,
            sizeof(packed_descriptor));
        m_device->unmapBuffer(m_descriptor_buffer.get());
        ++m_next_slot;
        entry.m_pinned = asset.m_storage;
        entry.m_refcount = 1u;
        entry.m_key = key;

        const auto [identity, it] = m_entries.emplaceHandle(std::move(entry));
        it->m_identity = identity;
        const TextureHandle handle{identity};
        m_dedup.emplace(key, handle);
        return handle;
    }

    std::error_code BindlessTextureCache::release(const TextureHandle handle) noexcept {
        const SparseHandle key = *handle;
        if (not
            key.isValid())
        [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (not onRenderThread_())
        [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        // Allowed while device_lost (CPU record only; GPU objects are gone).
        if (TextureEntry *const entry = m_entries.tryGet(key);
            entry != nullptr and entry->m_identity == key)
        {
            if (entry->m_refcount == 0u) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }
            if (--entry->m_refcount == 0u) {
                std::ignore = m_dedup.erase(entry->m_key);
                std::ignore = m_entries.erase(key);
            }
            return default_value_v;
        }
        return std::make_error_code(std::errc::invalid_argument);
    }

    Expected<TextureBindlessIndex> BindlessTextureCache::residentIndex(const TextureHandle handle) const noexcept {
        if (const TextureEntry *const entry = findEntry_(*handle)) [[likely]] {
            return entry->m_slot;
        }
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }

    rhi::ITextureView *BindlessTextureCache::view(const TextureHandle handle) const noexcept {
        if (const TextureEntry *const entry = findEntry_(*handle)) [[likely]] {
            return entry->m_view.get();
        }
        return nullptr;
    }

    rhi::IBuffer *BindlessTextureCache::descriptorBuffer() const noexcept {
        if (m_residency == CacheResidency::device_lost) [[unlikely]] {
            return nullptr;
        }
        return m_descriptor_buffer.get();
    }

    u32 BindlessTextureCache::textureUsed() const noexcept {
        if (not m_initialized or m_next_slot == 0u)
        [[unlikely]] {
            return 0u;
        }
        return m_next_slot - 1u;
    }

    u32 BindlessTextureCache::textureBudget() const noexcept {
        return m_texture_budget;
    }

    u64 BindlessTextureCache::entryCount() const noexcept {
        return static_cast<u64>(m_entries.size());
    }

    // ------------------------------------------------------------------
    // BindlessMaterialCache
    // ------------------------------------------------------------------

    std::error_code BindlessMaterialCache::initialize(rhi::IDevice &device, rhi::ISampler *const shared_sampler) {
        if (m_residency == CacheResidency::device_lost) [[unlikely]] {
            return std::make_error_code(std::errc::device_or_resource_busy);
        }
        if (m_initialized) {
            PPR_LOG(GpuCaches, warning, "BindlessMaterialCache already initialized");
            return default_value_v;
        }
        if (shared_sampler == nullptr) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (not
            device.hasFeature(rhi::Feature::Bindless))
        [[unlikely]] {
            PPR_LOG(GpuCaches, error, "BindlessMaterialCache requires bindless support");
            return std::make_error_code(std::errc::function_not_supported);
        }
        m_device = &device;
        m_shared_sampler = shared_sampler;
        // One capacity-sized buffer for the cache lifetime (512×80 B);
        // pack/release rewrite a single slot in place — never re-create.
        rhi::BufferDesc buffer_desc{};
        buffer_desc.size = kBindlessMaterialCapacity * sizeof(GpuMaterial);
        buffer_desc.elementSize = sizeof(GpuMaterial);
        buffer_desc.memoryType = rhi::MemoryType::Upload;
        buffer_desc.usage = rhi::BufferUsage::ShaderResource;
        buffer_desc.defaultState = rhi::ResourceState::ShaderResource;
        buffer_desc.label = "bindless materials";
        if (const std::error_code err = make_error_code(
            device.createBuffer(buffer_desc, nullptr, m_material_buffer.writeRef()))) [[unlikely]] {
            m_material_buffer.setNull();
            m_device = nullptr;
            m_shared_sampler = nullptr;
            PPR_LOG(GpuCaches, error, "material buffer creation failed", {{"message", err.message()}});
            return err;
        }
        m_initialized = true;
        m_owner = std::this_thread::get_id();
        m_residency = CacheResidency::ready;
        return default_value_v;
    }

    std::error_code BindlessMaterialCache::shutdown() {
        if (m_initialized and not onRenderThread_())
        [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        m_material_buffer.setNull();
        m_entries.clear();
        m_next_slot = 0u;
        m_shared_sampler = nullptr;
        m_owner = std::thread::id{};
        m_device = nullptr;
        m_initialized = false;
        m_residency = CacheResidency::uninitialized;
        return default_value_v;
    }

    CacheResidency BindlessMaterialCache::residency() const noexcept {
        return m_residency;
    }

    std::error_code BindlessMaterialCache::notifyDeviceLost() noexcept {
        if (m_residency != CacheResidency::ready) {
            return default_value_v;
        }
        if (not onRenderThread_()) [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        dropGpuObjects_();
        m_residency = CacheResidency::device_lost;
        return default_value_v;
    }

    void BindlessMaterialCache::dropGpuObjects_() noexcept {
        m_material_buffer.setNull();
    }

    bool BindlessMaterialCache::onRenderThread_() const noexcept {
        return std::this_thread::get_id() == m_owner;
    }

    const BindlessMaterialCache::MaterialEntry *BindlessMaterialCache::findEntry_(const SparseHandle key) const noexcept {
        if (not key.isValid() or m_residency == CacheResidency::device_lost)
        [[unlikely]] {
            return nullptr;
        }
        // Composed lookup: full-generation container check plus the minted
        // identity stored alongside — a wrapping seed alone never resolves.
        if (const MaterialEntry *const entry = m_entries.tryGet(key);
            entry != nullptr and entry->m_identity == key)
        [[likely]] {
            return entry;
        }
        return nullptr;
    }

    std::error_code BindlessMaterialCache::writeSlot_(const u32 slot, const GpuMaterial &gpu) {
        if (slot >= kBindlessMaterialCapacity or m_material_buffer == nullptr)
        [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        void *mapped = nullptr;
        PPR_RETURN_ERROR_ON_FAIL(GpuCaches,
            m_device->mapBuffer(m_material_buffer.get(), rhi::CpuAccessMode::Write, &mapped));
        std::memcpy(static_cast<std::byte *>(mapped) + static_cast<std::size_t>(slot) * sizeof(GpuMaterial),
            &gpu, sizeof(GpuMaterial));
        m_device->unmapBuffer(m_material_buffer.get());
        return default_value_v;
    }

    Expected<MaterialHandle> BindlessMaterialCache::pack(
        const mesh::MaterialAsset &asset, const GpuTextureRefs resolved) {
        if (not m_initialized or m_device == nullptr)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        if (m_residency == CacheResidency::device_lost)
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_such_device)};
        }
        if (not onRenderThread_())
        [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
        }
        Expected<GpuMaterial> gpu = buildGpuMaterial(asset, resolved);
        if (not
            gpu.has_value())
        [[unlikely]] {
            return std::unexpected{gpu.error()};
        }
        if (m_next_slot >= kBindlessMaterialCapacity) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::no_buffer_space)};
        }
        const u32 slot = m_next_slot;
        PPR_RETURN_UNEXPECTED_ON_FAIL(GpuCaches, writeSlot_(slot, *gpu));
        ++m_next_slot;
        MaterialEntry entry{.m_gpu = *gpu, .m_slot = slot};
        const auto [identity, it] = m_entries.emplaceHandle(std::move(entry));
        it->m_identity = identity;
        return MaterialHandle{identity};
    }

    std::error_code BindlessMaterialCache::release(const MaterialHandle handle) noexcept {
        const SparseHandle key = *handle;
        if (not
            key.isValid())
        [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (not onRenderThread_())
        [[unlikely]] {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        // Allowed while device_lost (CPU record only); the slot rewrite is
        // skipped then — the GPU buffer is gone, so there is nothing to zero.
        if (const MaterialEntry *const entry = m_entries.tryGet(key);
            entry != nullptr and entry->m_identity == key)
        {
            const u32 slot = entry->m_slot;
            std::ignore = m_entries.erase(key);
            if (m_residency == CacheResidency::device_lost) {
                return default_value_v;
            }
            return writeSlot_(slot, GpuMaterial{});
        }
        return std::make_error_code(std::errc::invalid_argument);
    }

    Expected<u32> BindlessMaterialCache::materialIndex(const MaterialHandle handle) const noexcept {
        if (const MaterialEntry *const entry = findEntry_(*handle)) [[likely]] {
            return entry->m_slot;
        }
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }

    Expected<GpuMaterial> BindlessMaterialCache::material(const MaterialHandle handle) const noexcept {
        if (const MaterialEntry *const entry = findEntry_(*handle)) [[likely]] {
            return entry->m_gpu;
        }
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }

    rhi::IBuffer *BindlessMaterialCache::materialBuffer() const noexcept {
        if (m_residency == CacheResidency::device_lost) [[unlikely]] {
            return nullptr;
        }
        return m_material_buffer.get();
    }

    u32 BindlessMaterialCache::materialUsed() const noexcept {
        return m_next_slot;
    }

    u32 BindlessMaterialCache::materialCapacity() const noexcept {
        return kBindlessMaterialCapacity;
    }

    u64 BindlessMaterialCache::entryCount() const noexcept {
        return static_cast<u64>(m_entries.size());
    }
}
