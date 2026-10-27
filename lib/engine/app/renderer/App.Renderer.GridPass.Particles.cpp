module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;
import :renderer.grid_pass;
import std;
import engine.core;
import engine.rhi;
import engine.shader;

namespace pP {
    using pP::make_error_code;

    // ------------------------------------------------------------------
    // immutable particle identity and gas admission
    // ------------------------------------------------------------------

    namespace {
        rhi::BufferRange particleRange(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        std::error_code particleBuffer(
            rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial,
            rhi::ComPtr<rhi::IBuffer> &result, const bool indirect = false) {
            rhi::BufferDesc desc{};
            desc.size = std::max<u64>(bytes, stride);
            desc.elementSize = stride;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = static_cast<rhi::BufferUsage>(
                static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                static_cast<u32>(rhi::BufferUsage::CopySource) |
                static_cast<u32>(rhi::BufferUsage::CopyDestination) |
                (indirect ? static_cast<u32>(rhi::BufferUsage::IndirectArgument) : 0u));
            desc.defaultState = rhi::ResourceState::UnorderedAccess;
            desc.label = "GridPass bounded resident particle storage";
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createBuffer(desc, initial, result.writeRef()));
            return {};
        }

        struct alignas(16) ParticlePrepareFrame {
            float m_planes[6][4]{};
            float m_cell_metres{}, m_origin{};
            u32 m_count{}, m_moving_count{};
            u32 m_block_count{}, m_padding[3]{};
        };

        static_assert(sizeof(ParticlePrepareFrame) == 128u and alignof(ParticlePrepareFrame) == 16u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_cell_metres) == 96u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_count) == 104u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_block_count) == 112u);
    }

    Expected<std::shared_ptr<const GridParticleMetadataResource>> prepareGridParticleMetadata(
        rhi::IDevice &device, const std::span<const GridParticleMetadataRow> rows, const u32 moving_count,
        const double cell_metres, const u64 version) {

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, rows.size() <= kGridParticleCapacity and
                              moving_count <= rows.size() and
                              version != 0u and
                              (cell_metres == 1.0 or cell_metres == 1.0 / 3.0)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));

        auto metadata = std::make_shared<GridParticleMetadataResource>();
        Array<u8> seen(moving_count, u8{});
        u64 prior_id{};
        for (const auto &row: rows) {
            const u64 id = (static_cast<u64>(row.m_id_high) << 32u) | row.m_id_low;
            const bool gas = row.m_source_row == 0xffff'ffffu;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass, id > prior_id and
                                  row.m_material > 0u and
                                  row.m_material < 0xffffu and
                                  row.m_reserved == 0u and
                                  std::isfinite(row.m_radius) and
                                  row.m_radius > 0.0f
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));

            if (gas) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    GridPass, std::isfinite(row.m_gas_x) and
                                      std::isfinite(row.m_gas_y) and
                                      row.m_gas_x >= 0.0f and
                                      row.m_gas_y >= 0.0f and
                                      row.m_gas_x <= static_cast<float>(4096.0 * cell_metres) and
                                      row.m_gas_y <= static_cast<float>(4096.0 * cell_metres)
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));
            } else {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    GridPass, row.m_source_row < moving_count and seen[row.m_source_row] == 0u
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));
                seen[row.m_source_row] = 1u;
            }

            metadata->m_materials.push_back(row.m_material);
            prior_id = id;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, std::ranges::all_of(seen, [](const u8 value) { return value == 1u; })
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));

        std::ranges::sort(metadata->m_materials);
        const auto unique = std::ranges::unique(metadata->m_materials);
        metadata->m_materials.erase(unique.begin(), unique.end());

        const GridParticleMetadataRow empty{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, particleBuffer(
                          device, std::max<u64>(1u, rows.size()) * sizeof(GridParticleMetadataRow),
                          sizeof(GridParticleMetadataRow), rows.empty() ? &empty : rows.data(), metadata->m_rows));


        metadata->m_version = version;
        metadata->m_cell_metres = cell_metres;
        metadata->m_count = safe_narrowing(rows.size());
        metadata->m_moving_count = moving_count;
        return std::shared_ptr<const GridParticleMetadataResource>{std::move(metadata)};
    }

    // ------------------------------------------------------------------
    // one-time initial inventory upload into the sole GPU circle path
    // ------------------------------------------------------------------

    std::error_code GridPass::admitParticleSeeds_(rhi::IDevice &device) {
        if (not m_world.m_particle_seeds_dirty)
            return {};

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, not m_world.m_gpu_particles or m_world.m_initial_particle_epoch
                          ? std::error_code{}
                          : std::make_error_code(std::errc::operation_not_permitted));
        if (m_world.m_cell_publication_deferred or
            std::ranges::any_of(m_world.m_cell_cache, [](const auto &entry) { return entry.second.m_pending; }))
            return {};

        const hal::ProfileScope scope{"App.Grid.Particles.AdmitSeeds"};
        Array<const GridParticleInstance *> seeds;
        for (const auto &chunk: m_world.m_particles) {
            if (chunk.m_source) {
                for (const auto &seed: *chunk.m_source)
                    seeds.push_back(std::addressof(seed));
            }
        }

        std::ranges::sort(seeds, {}, [](const auto *seed) { return seed->m_id; });

        struct HotRow {
            std::array<float, 16u> m_fields{};
        };
        struct ColdRow {
            float m_mass{}, m_volume{}, m_material{}, m_r{}, m_g{}, m_b{};
        };
        static_assert(sizeof(HotRow) == kGridParticleNumericalStride and alignof(HotRow) == 4u);
        static_assert(sizeof(ColdRow) == kGridParticleReadonlyStride and alignof(ColdRow) == 4u);
        static_assert(PPR_OFFSETOF(ColdRow, m_r) == 12u);
        Array<HotRow> hot(std::max<std::size_t>(1u, seeds.size()));
        Array<ColdRow> cold(std::max<std::size_t>(1u, seeds.size()));
        Array<GridParticleMetadataRow> rows(seeds.size());
        const double cell = m_world.m_cell_size_m == 1.0f ? 1.0 : 1.0 / 3.0;
        const double volume_unit = cell * cell * m_world.m_reference_thickness_m;
        for (u32 row = 0u; row < seeds.size(); ++row) {
            const auto &seed = *seeds[row];
            hot[row].m_fields[0u] = static_cast<float>(seed.m_position_x_m / cell + 3.0);
            hot[row].m_fields[1u] = static_cast<float>(seed.m_position_y_m / cell + 3.0);
            hot[row].m_fields[15u] = 1.0f;
            cold[row].m_volume = static_cast<float>(seed.m_volume_m3 / volume_unit);
            const auto visual = std::ranges::find(
                m_world.m_terrain_visuals, seed.m_material_id, &GridTerrainVisual::m_material_id);
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, visual != m_world.m_terrain_visuals.end()
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
            cold[row].m_r = static_cast<float>((visual->m_color_rgba >> 24u) & 255u) / 255.0f;
            cold[row].m_g = static_cast<float>((visual->m_color_rgba >> 16u) & 255u) / 255.0f;
            cold[row].m_b = static_cast<float>((visual->m_color_rgba >> 8u) & 255u) / 255.0f;
            rows[row].m_id_low = static_cast<u32>(seed.m_id);
            rows[row].m_id_high = static_cast<u32>(seed.m_id >> 32u);
            rows[row].m_source_row = row;
            rows[row].m_material = seed.m_material_id;
            rows[row].m_radius = static_cast<float>(
                std::sqrt(seed.m_volume_m3 / m_world.m_reference_thickness_m / std::numbers::pi));
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_world.m_particle_seed_version != std::numeric_limits<u64>::max()
                          ? std::error_code{}
                          : std::make_error_code(std::errc::value_too_large));
        const u64 version = m_world.m_particle_seed_version + 1u;
        rhi::ComPtr<rhi::IBuffer> hot_buffer, cold_buffer;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, particleBuffer(device, hot.size() * sizeof(HotRow), sizeof(HotRow), hot.data(), hot_buffer));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, particleBuffer(device, cold.size() * sizeof(ColdRow), sizeof(ColdRow), cold.data(), cold_buffer));
        const auto metadata = prepareGridParticleMetadata(device, rows, safe_narrowing(seeds.size()), cell, version);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, metadata);
        auto epoch = std::make_shared<std::atomic<u64>>(version);
        GridParticleGpuView view{};
        view.m_numerical = std::move(hot_buffer);
        view.m_readonly = std::move(cold_buffer);
        view.m_metadata = *metadata;
        view.m_live_epoch = epoch;
        view.m_world = {m_world.m_job_id, m_world.m_world_generation, m_world.m_content_fingerprint};
        view.m_device_epoch = version;
        view.m_publication = version;
        if (not m_world.m_gpu_particles)
            m_world.m_gpu_particles = std::make_unique<GpuParticleStorage>();

        // All fallible preparation completed. Encoded readers retain both old
        // buffers through their slot fence; unencoded old seed leases revoke.
        if (m_world.m_initial_particle_epoch)
            m_world.m_initial_particle_epoch->fetch_add(1u, std::memory_order_release);

        m_world.m_initial_particle_epoch = std::move(epoch);
        m_world.m_particle_seed_version = version;
        m_world.m_gpu_particles->m_view = std::move(view);
        m_world.m_particle_seeds_dirty = false;
        for (auto &chunk: m_world.m_particles)
            chunk.m_dirty = false;

        ++m_particle_uploads;
        m_particle_uploaded_bytes += hot.size() * sizeof(HotRow) + cold.size() * sizeof(ColdRow);
        return {};
    }

    // ------------------------------------------------------------------
    // shader programs and reflected scalar protocols
    // ------------------------------------------------------------------

    std::error_code GridPass::createGpuParticlePrograms_(
        IShaderService &service, rhi::IDevice &device, const std::filesystem::path &content_dir) {
        shader::SharedModule module;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            service.loadModuleFromFile(
                content_dir / "shaders" / "grid_particle_prepare.slang", "grid_particle_prepare", module.writeRef()));
        auto *const layout = module->getLayout();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, layout != nullptr ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
        const auto field = [&](const std::string_view name) -> slang::TypeLayoutReflection * {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (name == parameter->getName()) {
                    return parameter->getTypeLayout()->getElementTypeLayout();
                }
            }

            return nullptr;
        };

        auto *const numerical = field("g_numerical");
        auto *const metadata = field("g_metadata");
        auto *const readonly = field("g_readonly");
        auto *const packed = field("g_packed");
        auto *const frame = field("g_particle_prepare");
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            numerical and
                    metadata and
                    readonly and
                    packed and
                    frame and
                    numerical->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == kGridParticleNumericalStride and
                    readonly->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == kGridParticleReadonlyStride and
                    metadata->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(GridParticleMetadataRow) and
                    packed->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(ParticleGpu) and
                    frame->getSize() == sizeof(ParticlePrepareFrame) and
                    frame->getFieldByIndex(1u)->getOffset() == PPR_OFFSETOF(ParticlePrepareFrame, m_cell_metres) and
                    frame->getFieldByIndex(3u)->getOffset() == PPR_OFFSETOF(ParticlePrepareFrame, m_count)
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));

        constexpr std::array<const char *, 4u> entries{
            "packParticles", "rankParticles", "scanParticleBlocks", "scatterParticles"};
        for (u32 index = 0u; index < entries.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, module->findEntryPointByName(entries[index], entry.writeRef()));
            slang::IComponentType *entry_points[]{entry.get()};
            rhi::ShaderProgramDesc desc{};
            desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            desc.slangGlobalScope = module.get();
            desc.slangEntryPoints = entry_points;
            desc.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            auto &program = m_particle_prepare_programs[index];
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, device.createShaderProgram(desc, program.m_program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = program.m_program;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createComputePipeline(pipeline, program.m_pipeline.writeRef()));
        }

        return {};
    }

    std::error_code GridPass::ensureParticleMask_(rhi::IDevice &device) {
        if (m_particle_mask_view)
            return {};

        constexpr u32 edge = 128u;
        std::array<u32, 21'845u> pixels{};
        std::array<rhi::SubresourceData, 8u> levels{};
        for (u32 y = 0u; y < edge; ++y) {
            for (u32 x = 0u; x < edge; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - 64.0f;
                const float dy = static_cast<float>(y) + 0.5f - 64.0f;
                const float alpha = std::clamp(62.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                pixels[y * edge + x] = 0x00ffffffu | (static_cast<u32>(std::lround(alpha * 255.0f)) << 24u);
            }
        }

        u32 offset = 0u;
        u32 previous = 0u;
        for (u32 mip = 0u, size = edge; mip < levels.size(); ++mip, size /= 2u) {
            if (mip != 0u) {
                for (u32 y = 0u; y < size; ++y) {
                    for (u32 x = 0u; x < size; ++x) {
                        const u32 start = previous + (2u * y) * (2u * size) + 2u * x;
                        const u32 alpha = (pixels[start] >> 24u) +
                                          (pixels[start + 1u] >> 24u) +
                                          (pixels[start + 2u * size] >> 24u) +
                                          (pixels[start + 2u * size + 1u] >> 24u);
                        pixels[offset + y * size + x] = 0x00ffffffu | (((alpha + 2u) / 4u) << 24u);
                    }
                }
            }

            levels[mip] = {
                .data = pixels.data() + offset,
                .rowPitch = size * sizeof(u32),
                .slicePitch = size * size * sizeof(u32)};
            previous = offset;
            offset += size * size;
        }

        rhi::TextureDesc desc{};
        desc.type = rhi::TextureType::Texture2D;
        desc.size = {.width = edge, .height = edge, .depth = 1u};
        desc.arrayLength = 1u;
        desc.mipCount = safe_narrowing(levels.size());
        desc.format = rhi::Format::RGBA8Unorm;
        desc.memoryType = rhi::MemoryType::DeviceLocal;
        desc.usage = rhi::TextureUsage::ShaderResource;
        desc.defaultState = rhi::ResourceState::ShaderResource;
        desc.label = "particle circle alpha mask";
        rhi::ComPtr<rhi::ITexture> texture;
        rhi::ComPtr<rhi::ITextureView> view;
        rhi::ComPtr<rhi::ISampler> sampler;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createTexture(desc, levels.data(), texture.writeRef()));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, texture->getDefaultView(view.writeRef()));

        rhi::SamplerDesc sampling{};
        sampling.minFilter = rhi::TextureFilteringMode::Linear;
        sampling.magFilter = rhi::TextureFilteringMode::Linear;
        sampling.mipFilter = rhi::TextureFilteringMode::Linear;
        sampling.addressU = rhi::TextureAddressingMode::ClampToEdge;
        sampling.addressV = rhi::TextureAddressingMode::ClampToEdge;
        sampling.addressW = rhi::TextureAddressingMode::ClampToEdge;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, device.createSampler(sampling, sampler.writeRef()));

        m_particle_mask = std::move(texture);
        m_particle_mask_view = std::move(view);
        m_particle_mask_sampler = std::move(sampler);
        return {};
    }

    // ------------------------------------------------------------------
    // bounded source selection and owner-fenced storage
    // ------------------------------------------------------------------

    std::optional<GridParticlePresentationIdentity> GridPass::activeParticlePresentation() const noexcept {
        if (not m_world.m_gpu_particles) {
            return std::nullopt;
        }

        const auto &view = m_world.m_gpu_particles->m_view;
        if (not view.current()) {
            return std::nullopt;
        }

        return GridParticlePresentationIdentity{
            view.m_world, view.m_publication, view.m_terrain_revision, view.m_joint_tick};
    }

    bool GridPass::gpuParticleFrameCurrent(const std::optional<GridParticleGpuView> &view) const noexcept {
        const auto *const selected = view                      ? &*view
                                     : m_world.m_gpu_particles ? &m_world.m_gpu_particles->m_view
                                                               : nullptr;
        return selected == nullptr or (selected->current() and
                                          selected->m_world.m_job_id == m_world.m_job_id and
                                          selected->m_world.m_world_generation == m_world.m_world_generation and
                                          selected->m_world.m_content_fingerprint == m_world.m_content_fingerprint);
    }

    std::error_code GridPass::applyGpuParticleView(const GridParticleGpuView &view) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_particle_prepare_programs[0u].m_program != nullptr
                          ? std::error_code{}
                          : std::make_error_code(std::errc::not_supported));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, gpuParticleFrameCurrent(view) and
                              view.m_metadata and
                              view.m_numerical and
                              view.m_readonly and
                              view.m_layout == kGridParticleNumericalLayout and
                              view.m_stride == kGridParticleNumericalStride and
                              view.m_readonly_stride == kGridParticleReadonlyStride and
                              view.m_origin == 3.0 and
                              view.m_metadata->m_count <= kGridParticleCapacity and
                              view.m_metadata->m_rows and
                              view.m_numerical->getDesc().elementSize == kGridParticleNumericalStride and
                              view.m_numerical->getDesc().size >=
                                  std::max<u64>(1u, view.m_metadata->m_moving_count) * kGridParticleNumericalStride and
                              view.m_readonly->getDesc().elementSize == kGridParticleReadonlyStride and
                              view.m_readonly->getDesc().size >=
                                  std::max<u64>(1u, view.m_metadata->m_moving_count) * kGridParticleReadonlyStride and
                              view.m_metadata->m_cell_metres == (m_world.m_cell_size_m == 1.0f ? 1.0 : 1.0 / 3.0)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        if (not m_world.m_gpu_particles or m_world.m_gpu_particles->m_view.m_metadata != view.m_metadata) {
            for (const u32 material: view.m_metadata->m_materials) {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, std::ranges::any_of(
                                  m_world.m_terrain_visuals,
                                  [material](const auto &visual) { return visual.m_material_id == material; })
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));
            }
        }

        if (not m_world.m_gpu_particles) {
            m_world.m_gpu_particles = std::make_unique<GpuParticleStorage>();
        }

        if (m_world.m_initial_particle_epoch and view.m_live_epoch != m_world.m_initial_particle_epoch) {
            // A real committed source supersedes initial admission once. It may
            // never fall back to CPU seeds after revocation or operational error.
            m_world.m_initial_particle_epoch->fetch_add(1u, std::memory_order_release);
            m_world.m_initial_particle_epoch.reset();
            m_world.m_particles = {};
            m_world.m_particle_seeds_dirty = false;
        }

        m_world.m_gpu_particles->m_view = view;
        return {};
    }

    std::error_code GridPass::ensureGpuParticleStorage_(rhi::IDevice &device, const u32 count) {
        auto &storage = *m_world.m_gpu_particles;
        if (storage.m_capacity >= std::max(1u, count)) {
            return {};
        }

        // Allocate the complete replacement before relinquishing the usable
        // cohort. Its strong source owners survive until the exact reader fence.
        auto replacement = std::make_unique<GpuParticleStorage>();
        replacement->m_view = storage.m_view;
        replacement->m_capacity = std::max(1u, count);
        const u64 indices_bytes = static_cast<u64>(replacement->m_capacity) * sizeof(u32);
        const u32 blocks = (replacement->m_capacity + 255u) / 256u;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, particleBuffer(
                          device, static_cast<u64>(replacement->m_capacity) * sizeof(ParticleGpu), sizeof(ParticleGpu),
                          nullptr, replacement->m_packed));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, particleBuffer(device, indices_bytes, sizeof(u32), nullptr, replacement->m_ranks));

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, particleBuffer(device, blocks * sizeof(u32), sizeof(u32), nullptr, replacement->m_counts));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, particleBuffer(device, (blocks + 1u) * sizeof(u32), sizeof(u32), nullptr, replacement->m_prefix));
        for (u32 slot = 0u; slot < kTileRingSize; ++slot) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                particleBuffer(device, indices_bytes, sizeof(u32), nullptr, replacement->m_slots[slot].m_ids));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, particleBuffer(
                              device, sizeof(rhi::IndirectDrawArguments), sizeof(u32), nullptr,
                              replacement->m_slots[slot].m_args, true));
            for (u32 program = 0u; program < m_particle_prepare_programs.size(); ++program) {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, device.createRootShaderObject(
                                  m_particle_prepare_programs[program].m_program,
                                  replacement->m_roots[program][slot].writeRef()));
            }
        }

        auto retired = std::move(m_world.m_gpu_particles);
        m_world.m_gpu_particles = std::move(replacement);
        m_deferred_releases.defer(m_world.m_last_reader, [held = std::move(retired)]() mutable { held.reset(); });
        return {};
    }

    // ------------------------------------------------------------------
    // GPU circle packing and stable visible row IDs
    // ------------------------------------------------------------------

    std::error_code GridPass::prepareGpuParticles(const PreRenderContext &context) {
        PPR_RETURN_ERROR_ON_FAIL(GridPass, admitParticleSeeds_(context.m_device));
        if (not m_world.m_gpu_particles) {
            return {};
        }

        const hal::ProfileScope scope{"App.Grid.Particles.PrepareGpu"};
        context.m_encoder.pushDebugGroup("Particles.Prepare", {});
        PPR_DEFER {
            context.m_encoder.popDebugGroup();
        };

        reclaimCompleted_(context.m_completed_fence_value);
        // Resident terrain already selected its slot in this encoder. Leave its
        // prepared IDs/args current; seed/reference callers select here instead.
        if (not m_visibility_prepared) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, selectSlot_(context.m_completed_fence_value, context.m_wait_for_fence));
        }
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, gpuParticleFrameCurrent({}) ? std::error_code{}
                                                  : std::make_error_code(std::errc::resource_unavailable_try_again));
        const u32 count = m_world.m_gpu_particles->m_view.m_metadata->m_count;
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureGpuParticleStorage_(context.m_device, count));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureSurfaceBuffers_(context.m_device));
        const auto visuals = prepareTerrainVisuals_(context.m_device, 0u);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, visuals);
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, *visuals ? std::error_code{} : std::make_error_code(std::errc::operation_in_progress));

        auto &storage = *m_world.m_gpu_particles;
        storage.m_selected_slot = m_tile_ring_cursor;
        auto &slot = storage.m_slots[storage.m_selected_slot];
        storage.m_prepared = false;
        storage.m_pending_pack = false;
        // A later reference terrain draw may rotate its own cursor. This source
        // lease and every particle root remain on the captured preparation slot.
        slot.m_reader = storage.m_view;
        const auto &view = *slot.m_reader;
        const auto &metadata = *view.m_metadata;
        const u32 blocks = std::max(1u, (count + 255u) / 256u);

        ParticlePrepareFrame frame{};
        for (u32 plane = 0u; plane < 6u; ++plane) {
            std::memcpy(
                frame.m_planes[plane], m_camera_view.m_frustum.clip[plane].data(), sizeof(frame.m_planes[plane]));
        }

        frame.m_cell_metres = static_cast<float>(metadata.m_cell_metres);
        frame.m_origin = static_cast<float>(view.m_origin);
        frame.m_count = count;
        frame.m_moving_count = metadata.m_moving_count;
        frame.m_block_count = blocks;

        for (auto *const buffer:
            {storage.m_packed.get(), storage.m_ranks.get(), storage.m_counts.get(), storage.m_prefix.get(),
                slot.m_ids.get(), slot.m_args.get()}) {
            context.m_encoder.setBufferState(buffer, rhi::ResourceState::UnorderedAccess);
        }

        const auto encode = [&](const u32 phase) -> std::error_code {
            constexpr std::array<const char *, 4u> names{
                "Particles.Pack", "Particles.Cull", "Particles.CullPrefix", "Particles.CullCompact"};
            context.m_encoder.pushDebugGroup(names[phase], {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };

            auto *const root = storage.m_roots[phase][storage.m_selected_slot].get();
            rhi::ShaderCursor cursor{root};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_prepare"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, constants.setData(&frame, sizeof(frame)));
            const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, cursor[name].setBinding(rhi::Binding(buffer, particleRange(buffer))));
                return {};
            };

            if (phase == 0u) {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_numerical", view.m_numerical));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_metadata", metadata.m_rows));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_readonly", view.m_readonly));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_particle_palette", m_world.m_terrain_palette));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_packed", storage.m_packed));
            } else if (phase == 2u) {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_block_counts", storage.m_counts));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_block_prefix", storage.m_prefix));
            } else {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_ranks", storage.m_ranks));
                if (phase == 1u) {
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_packed", storage.m_packed));
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_block_counts", storage.m_counts));
                } else {
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_block_prefix", storage.m_prefix));
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_particle_ids", slot.m_ids));
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_particle_args", slot.m_args));
                }
            }

            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };
                pass->bindPipeline(m_particle_prepare_programs[phase].m_pipeline, root);
                pass->dispatchCompute(phase == 2u ? 1u : blocks, 1u, 1u);
                ++m_dispatch_commands;
            }

            context.m_encoder.globalBarrier();
            return {};
        };

        const bool pack = storage.m_packed_owner != view.m_live_epoch or
                          storage.m_packed_epoch != view.m_device_epoch or
                          storage.m_packed_metadata != metadata.m_version;
        if (pack) {
            context.m_encoder.setBufferState(view.m_numerical, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(metadata.m_rows, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(view.m_readonly, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(m_world.m_terrain_palette, rhi::ResourceState::ShaderResource);
            PPR_RETURN_ERROR_ON_FAIL(GridPass, encode(0u));

            storage.m_pending_owner = view.m_live_epoch;
            storage.m_pending_epoch = view.m_device_epoch;
            storage.m_pending_metadata = metadata.m_version;
            storage.m_pending_terrain = view.m_terrain_revision;
            storage.m_pending_pack = true;
        }

        PPR_RETURN_ERROR_ON_FAIL(GridPass, encode(1u));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, encode(2u));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, encode(3u));
        context.m_encoder.setBufferState(storage.m_packed, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(slot.m_ids, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(slot.m_args, rhi::ResourceState::IndirectArgument);
        storage.m_prepared = true;
        storage.m_slot_encoded = true;
        return {};
    }

    std::error_code GridPass::renderGpuParticles_(const DrawContext &context) {
        context.m_pass.pushDebugGroup("Particles.Draw", {});
        PPR_DEFER {
            context.m_pass.popDebugGroup();
        };

        auto &storage = *m_world.m_gpu_particles;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            storage.m_prepared ? std::error_code{} : std::make_error_code(std::errc::operation_not_permitted));

        storage.m_prepared = false;
        const u32 particle_slot = storage.m_selected_slot;
        const auto pipeline = opticalPipeline_(
            context.m_device, context.m_render_pipeline_key, m_gpu_particle_program, m_gpu_particle_key,
            m_gpu_particle_pipeline, true);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, pipeline);

        auto *const root = (*pipeline)->m_slot_roots[particle_slot].get();
        rhi::ShaderCursor cursor{root};
        PPR_RETURN_ERROR_ON_FAIL(GridPass, uploadFrameConstants_((*pipeline)->m_slot_frame_cursors[particle_slot]));
        // Billboard directions and centers must use the same current snapshot,
        // including worlds whose background preparation returns early.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, (*pipeline)->m_slot_frame_cursors[particle_slot]["m_inverse_view_projection"].setData(
                          &m_camera_view.m_invert_view_projection, sizeof(m_camera_view.m_invert_view_projection)));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindSurfaceBuffers_(context.m_device, cursor, m_surface_retained_table));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleMask_(context.m_device));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_mask"].setBinding(rhi::Binding(m_particle_mask_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_mask_sampler"].setBinding(rhi::Binding(m_particle_mask_sampler)));
        const bool has_cells = m_surface_retained_table ? m_world.m_visibility_has_cells
                                                        : m_world.m_cell_atlas != nullptr;
        auto *const cells = has_cells ? m_world.m_cell_atlas.get() : m_world.m_cell_dummy.get();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_cell_materials"].setBinding(rhi::Binding(cells, particleRange(cells))));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            cursor["g_particles"].setBinding(rhi::Binding(storage.m_packed, particleRange(storage.m_packed))));
        auto &slot = storage.m_slots[particle_slot];
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_ids"].setBinding(rhi::Binding(slot.m_ids, particleRange(slot.m_ids))));
        (*pipeline)->m_slot_particle_buffers[particle_slot] = storage.m_packed;
        context.m_pass.setRenderState(
            {.viewports = {context.m_viewport},
                .viewportCount = 1u,
                .scissorRects = {context.m_scissor},
                .scissorRectCount = 1u,
                .vertexBuffers = {},
                .indexBuffer = {}});
        context.m_pass.bindPipeline((*pipeline)->m_pipeline, root);
        markPipelineUse_(**pipeline, particle_slot);
        context.m_pass.drawIndirect(1u, rhi::BufferOffsetPair{slot.m_args});
        ++m_draw_commands;
        return {};
    }
}
