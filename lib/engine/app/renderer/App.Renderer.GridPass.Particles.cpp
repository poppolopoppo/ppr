module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;
import :renderer.grid_pass;
import :renderer.world_lighting;
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
            u32 m_block_count{}, m_census_world_edge{}, m_padding[2]{};
        };

        static_assert(sizeof(ParticlePrepareFrame) == 128u and alignof(ParticlePrepareFrame) == 16u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_cell_metres) == 96u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_count) == 104u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_block_count) == 112u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_census_world_edge) == 116u);
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
        u32 unrepresentable_materials{};
        for (const auto &row: rows) {
            const u64 id = (static_cast<u64>(row.m_id_high) << 32u) | row.m_id_low;
            const bool gas = row.m_source_row == 0xffff'ffffu;
            const u32 material = row.m_material_temperature & 0xffffu;
            // The presentation row carries the material in 12 bits, so a committed
            // id at or above kGridParticleMaterialLimit cannot be represented. The
            // publication is admitted unchanged (the palette still addresses 16
            // bits) and the pack shader rejects such a row instead of truncating
            // it into a different valid material; it is counted here so the
            // rejection is observable at the host boundary.
            if (material >= kGridParticleMaterialLimit) {
                ++unrepresentable_materials;
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass, id > prior_id and
                                  material > 0u and
                                  material < 0xffffu and
                                  (row.m_physical_fill_bits == 0u or
                                      (std::isnormal(std::bit_cast<float>(row.m_physical_fill_bits)) and
                                          std::bit_cast<float>(row.m_physical_fill_bits) > 0.0f)) and
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

            metadata->m_materials.push_back(material);
            prior_id = id;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, std::ranges::all_of(seen, [](const u8 value) { return value == 1u; })
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));

        // The pack shader is the last line of defence and writes an all-zero row
        // for such a material, which the compaction then excludes. Report it once
        // per publication from the host that already sees every row, so a rejected
        // instance is observable without a GPU counter, readback or per-row spam.
        if (unrepresentable_materials != 0u) {
            PPR_LOG(
                GridPass, warning,
                "particle publication carries materials outside the 12-bit presentation domain; those rows are "
                "rejected by the packer",
                {{"rows", unrepresentable_materials}, {"material_limit", kGridParticleMaterialLimit}});
        }

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
    // completion-governed test timing queries
    // ------------------------------------------------------------------

    std::error_code GridPass::setParticleTimingQueriesForTest(
        rhi::ComPtr<rhi::IQueryPool> queries, const FenceCompletionValue completed) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            not queries or (queries->getDesc().type == rhi::QueryType::Timestamp and queries->getDesc().count >= 12u)
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, slotFenceComplete(completed, m_last_submitted_value) and
                              (not m_world.m_gpu_particles or not m_world.m_gpu_particles->m_slot_encoded)
                          ? std::error_code{}
                          : std::make_error_code(std::errc::device_or_resource_busy));

        m_particle_timing_queries = std::move(queries);
        return {};
    }

    // ------------------------------------------------------------------
    // one-time initial inventory upload into the sole GPU circle path
    // ------------------------------------------------------------------

    // Seeds are an admission path, not a position authority. Once a solver view
    // is published (`GridParticleGpuView`), the GPU hot row is authoritative for
    // every moving particle and the packet position is only the admission seed
    // that placed the row here. The staged seed position is therefore consumed
    // once, at admission, and never re-read by the draw path.
    //
    // `m_radius` is metres only: the gas path draws it directly and the packer
    // re-derives the moving radius in metres from the cold volume. No cell-space
    // radius crosses this seam.
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

        // Hot64 mirrors the solver's 64-byte ParticleWrite by field order
        // (position, displacement, F, D, liquid scalars, enabled); only the
        // admission seed writes position[0..1] and enabled here.
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
        const double volume_unit = cell * cell * static_cast<double>(m_world.m_reference_thickness_m);
        for (u32 row = 0u; row < seeds.size(); ++row) {
            const auto &seed = *seeds[row];
            // The presentation row carries the material in 12 bits. A seed outside
            // that domain cannot be represented and is rejected here, before any
            // write, instead of being truncated into a different valid material.
            // (A solver publication is checked the same way by the pack shader.)
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, seed.m_material_id < kGridParticleMaterialLimit
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
            // Admission seed coordinates in native cells; the published hot row
            // supersedes them as soon as a solver view exists.
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
            // Lossless: the 12-bit domain was admitted above, so the u16 parameter
            // of the canonical cell codec cannot truncate.
            const auto packed = lighting::packCell(static_cast<u16>(seed.m_material_id), seed.m_temperature_k);
            PPR_RETURN_ERROR_ON_FAIL(GridPass, packed);
            rows[row].m_material_temperature = *packed;
            // Metres, not cells: the presentation row's radius unit for both the
            // gas path and the moving path.
            rows[row].m_radius = static_cast<float>(
                std::sqrt(seed.m_volume_m3 / static_cast<double>(m_world.m_reference_thickness_m) / std::numbers::pi));
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
        auto *const palette = field("g_particle_palette");
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, palette != nullptr and
                              palette->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(GridTerrainVisual) and
                              palette->getFieldCount() == 3u
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));
        const std::array<const char *, 3> palette_fields{"material", "rgba", "carried"};
        for (unsigned index = 0u; index < palette_fields.size(); ++index) {
            auto *const member = palette->getFieldByIndex(index);
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, std::string_view{member->getName()} == palette_fields[index] and
                                  member->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == u64{index} * 4u
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
        }

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

        // The 24-byte presentation row is a reflected ABI contract, not an
        // assumption: a target that lays out `half2 motion` or the material word
        // differently fails program creation here instead of misreading every
        // instance. The writer's names are the field contract grid_tiles decodes.
        constexpr std::array<std::pair<const char *, u32>, 5u> packed_fields{{
            {"position", 0u},
            {"radius", 8u},
            {"material_flags_temperature", 12u},
            {"color_rgb9e5", 16u},
            {"motion", 20u},
        }};
        bool packed_layout_matches = packed != nullptr and
                                     packed->getFieldCount() == static_cast<unsigned>(packed_fields.size());
        for (const auto &[name, offset]: packed_fields) {
            bool matched = false;
            for (unsigned index = 0u; packed_layout_matches and index < packed->getFieldCount(); ++index) {
                auto *const member = packed->getFieldByIndex(index);
                if (name != std::string_view{member->getName()}) {
                    continue;
                }

                matched = member->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                break;
            }

            packed_layout_matches = packed_layout_matches and matched;
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, packed_layout_matches ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

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
        // A frame that offers no view carries CPU admission seeds or a hold.
        // Neither reads solver storage, so neither rejects the frame; a hold is
        // the renderer's own retained presentation (plan §3).
        if (not view) {
            return true;
        }

        return view->m_world.m_job_id == m_world.m_job_id and
               view->m_world.m_world_generation == m_world.m_world_generation and
               view->m_world.m_content_fingerprint == m_world.m_content_fingerprint;
    }

    bool GridPass::retainedGpuParticleCurrent_() const noexcept {
        if (not m_world.m_gpu_particles) {
            return false;
        }

        const auto &view = m_world.m_gpu_particles->m_view;
        return view.current() and
               view.m_world.m_job_id == m_world.m_job_id and
               view.m_world.m_world_generation == m_world.m_world_generation and
               view.m_world.m_content_fingerprint == m_world.m_content_fingerprint;
    }

    std::error_code GridPass::holdGpuParticles_() {
        ++m_world.m_particle_hold_attempts;
        m_world.m_particle_presentation = EParticlePresentation::held;

        // Only a completed pack may be presented. Before the first pack there is
        // nothing to retain, so a never-packed hold presents no particles: the
        // degenerate case after attach, not an error.
        if (m_world.m_gpu_particles) {
            auto &particles = *m_world.m_gpu_particles;
            particles.m_prepared = particles.m_packed_owner != nullptr;

            // The retained slot is read by this submission, so fence it with the
            // hold frame's value: a later scatter must wait for that read before
            // it rewrites the ids or arguments. A capture-only census of the
            // superseded epoch is withdrawn rather than read back torn.
            particles.m_slot_encoded = particles.m_prepared;
            auto &slot = particles.m_slots[particles.m_selected_slot];
            if (slot.m_census) {
                slot.m_census->m_encoded = false;
                slot.m_census->m_submission = kFenceValueNone;
            }

            if (m_world.m_particle_hold_attempts == 1u) {
                const auto &view = particles.m_view;
                PPR_LOG(
                    GridPass, info, "particle publication is not current; presenting the last packed rows",
                    {
                        {"device_epoch", view.m_device_epoch},
                        {"live_epoch", view.m_live_epoch ? view.m_live_epoch->load(std::memory_order_acquire) : u64{}},
                        {"packed", particles.m_packed_owner != nullptr},
                        {"packed_epoch", particles.m_packed_epoch},
                    });
            }
        } else if (m_world.m_particle_hold_attempts == 1u) {
            PPR_LOG(
                GridPass, info, "particle publication is not current; no rows are retained yet", {{"packed", false}});
        }

        return {};
    }

    std::error_code GridPass::applyGpuParticleView(const GridParticleGpuView &view) {
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_particle_prepare_programs[0u].m_program != nullptr
                          ? std::error_code{}
                          : std::make_error_code(std::errc::not_supported));

        // A view from another world is a defect, not a transient: it cannot be
        // presented as this world's rows even when its own epoch is live.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            gpuParticleFrameCurrent(view) ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        // Liveness (finding D10): the renderer decides for itself whether the
        // committed rows are still its own. `publishTick` swaps the pairs in
        // place and bumps the live epoch, so installing a not-current view would
        // pack a torn buffer. It is the same hold the packet reports, so the two
        // layers cannot disagree about liveness (finding D11). The frame's held
        // preparation counts and logs it once in prepareGpuParticles, which owns
        // the per-frame decision.
        if (not view.current()) {
            m_world.m_particle_presentation = EParticlePresentation::held;
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, view.m_metadata and
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
        m_world.m_particle_presentation = EParticlePresentation::current;
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

        // The callable fallback requires a copyable owner through the reader fence.
        std::shared_ptr<GpuParticleStorage> retired{std::move(m_world.m_gpu_particles)};
        m_world.m_gpu_particles = std::move(replacement);
        m_deferred_releases.defer(m_world.m_last_reader, [held = std::move(retired)]() mutable { held.reset(); });
        return {};
    }

    // ------------------------------------------------------------------
    // GPU circle packing and stable visible row IDs
    // ------------------------------------------------------------------

    std::error_code GridPass::prepareGpuParticles(const PreRenderContext &context, const u32 census_world_edge) {
        if (m_spatial_msaa) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSpatialContent());
            PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureSpatialPrograms_(context.m_device));
        }

        PPR_RETURN_ERROR_ON_FAIL(GridPass, admitParticleSeeds_(context.m_device));
        if (not m_world.m_gpu_particles) {
            // No committed view has ever been installed. A held stage still
            // presents no particles and is counted; a seed-only world keeps its
            // CPU admission path. Neither is an error.
            return m_world.m_particle_presentation == EParticlePresentation::held ? holdGpuParticles_()
                                                                                  : std::error_code{};
        }

        const hal::ProfileScope scope{"App.Grid.Particles.PrepareGpu"};
        context.m_encoder.pushDebugGroup("Particles.Prepare", {});
        PPR_DEFER {
            context.m_encoder.popDebugGroup();
        };

        reclaimCompleted_(context.m_completed_fence_value);

        // Publication liveness (plan §3, findings D10-D12): a committed view is
        // alive but no longer owned once `publishTick` swaps the committed pair
        // and bumps the live epoch. That is a hold, not a failure: encode no
        // pack, read no solver buffer, and keep drawing the renderer's own last
        // packed rows and stable IDs. A retryable state must never stop the loop.
        if (not retainedGpuParticleCurrent_()) {
            return holdGpuParticles_();
        }

        // A current publication ends the hold: this frame re-packs from the new
        // epoch and resumes. The exit is one log for the whole hold, not one per
        // frame, and there is no timeout (expiry is a publication or a world
        // event).
        if (m_world.m_particle_hold_attempts != 0u) {
            PPR_LOG(
                GridPass, info, "particle publication is current again; resuming the packed presentation",
                {
                    {"held_attempts", m_world.m_particle_hold_attempts},
                    {"device_epoch", m_world.m_gpu_particles->m_view.m_device_epoch},
                });
            m_world.m_particle_hold_attempts = 0u;
        }

        // The retained publication is live: this frame packs and draws it.
        m_world.m_particle_presentation = EParticlePresentation::current;

        // Resident terrain already selected its slot in this encoder. Leave its
        // prepared IDs/args current; seed/reference callers select here instead.
        if (not m_visibility_prepared) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, selectSlot_(context.m_completed_fence_value, context.m_wait_for_fence));
        }

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

        // Capture resources belong to this completion-selected reader slot.
        // A rejected frame keeps them here; unknown submission uses the existing
        // renderer quarantine and pass teardown proof, never a stack-owned lease.
        if (slot.m_census) {
            slot.m_census->m_encoded = false;
            slot.m_census->m_submission = kFenceValueNone;
        }

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

        if (census_world_edge != 0u) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                census_world_edge <= 4096u ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

            if (not m_particle_census_program.m_program) {
                shader::SharedModule module;
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, m_shader_service->loadModuleFromFile(
                                  m_content_dir / "shaders" / "grid_particle_prepare.slang", "grid_particle_prepare",
                                  module.writeRef()));

                shader::ComPtr<slang::IEntryPoint> entry;
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, module->findEntryPointByName("projectParticleCensus", entry.writeRef()));

                slang::IComponentType *entries[]{entry.get()};
                rhi::ShaderProgramDesc description{};
                description.linkingStyle = rhi::LinkingStyle::SingleProgram;
                description.slangGlobalScope = module.get();
                description.slangEntryPoints = entries;
                description.slangEntryPointCount = 1u;
                GpuParticleProgram candidate;
                shader::Diagnose diagnostics;
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, context.m_device.createShaderProgram(
                                  description, candidate.m_program.writeRef(), diagnostics.writeRef()));

                rhi::ComputePipelineDesc pipeline{};
                pipeline.program = candidate.m_program;
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, context.m_device.createComputePipeline(pipeline, candidate.m_pipeline.writeRef()));

                m_particle_census_program = std::move(candidate);
            }

            const u64 bytes = std::max<u64>(1u, count) * sizeof(GridParticleCensusRow);
            if (not slot.m_census or slot.m_census->m_projected->getDesc().size < bytes) {
                auto candidate = std::make_unique<ParticleCensusStorage>();
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass,
                    particleBuffer(
                        context.m_device, bytes, sizeof(GridParticleCensusRow), nullptr, candidate->m_projected));

                rhi::BufferDesc readback{};
                readback.size = bytes;
                readback.elementSize = sizeof(GridParticleCensusRow);
                readback.memoryType = rhi::MemoryType::ReadBack;
                readback.usage = rhi::BufferUsage::CopyDestination;
                readback.defaultState = rhi::ResourceState::CopyDestination;
                readback.label = "Grid capture-only particle census readback";
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, context.m_device.createBuffer(readback, nullptr, candidate->m_readback.writeRef()));

                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, context.m_device.createRootShaderObject(
                                  m_particle_census_program.m_program, candidate->m_root.writeRef()));

                candidate->m_device = &context.m_device;
                slot.m_census = std::move(candidate);
            }

            auto &census = *slot.m_census;
            census.m_identity = {view.m_world, view.m_publication, view.m_terrain_revision, view.m_joint_tick};
            census.m_device_epoch = view.m_device_epoch;
            census.m_count = count;
            census.m_world_edge = census_world_edge;
            frame.m_census_world_edge = census_world_edge;
            rhi::ShaderCursor cursor{census.m_root.get()};
            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_prepare"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, constants.setData(&frame, sizeof(frame)));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                cursor["g_numerical"].setBinding(rhi::Binding(view.m_numerical, particleRange(view.m_numerical))));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                cursor["g_metadata"].setBinding(rhi::Binding(metadata.m_rows, particleRange(metadata.m_rows))));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particle_palette"].setBinding(
                              rhi::Binding(m_world.m_terrain_palette, particleRange(m_world.m_terrain_palette))));
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, cursor["g_particle_census"].setBinding(
                              rhi::Binding(census.m_projected, particleRange(census.m_projected))));

            context.m_encoder.setBufferState(view.m_numerical, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(metadata.m_rows, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(m_world.m_terrain_palette, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(census.m_projected, rhi::ResourceState::UnorderedAccess);
            if (count != 0u) {
                auto *const pass = context.m_encoder.beginComputePass();
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));

                pass->bindPipeline(m_particle_census_program.m_pipeline, census.m_root);
                pass->dispatchCompute(blocks, 1u, 1u);
                pass->end();

                ++m_dispatch_commands;

                context.m_encoder.globalBarrier();
                context.m_encoder.setBufferState(census.m_projected, rhi::ResourceState::CopySource);
                context.m_encoder.setBufferState(census.m_readback, rhi::ResourceState::CopyDestination);
                context.m_encoder.copyBuffer(
                    census.m_readback, 0u, census.m_projected, 0u, u64{count} * sizeof(GridParticleCensusRow));
            }

            census.m_encoded = true;
            storage.m_slot_encoded = true;
        }

        if (m_particle_timing_queries) {
            context.m_encoder.writeTimestamp(m_particle_timing_queries, 0u);
        }

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
            // D9: the material id is the shading key. Rows whose material the
            // palette does not carry discard at shading time, so count those
            // distinct published materials here, once per pack, where the host
            // already holds the whole catalogue. No GPU counter and no readback.
            const std::span<const GridTerrainVisual> palette{
                m_world.m_terrain_visuals.data(), m_world.m_terrain_visuals.size()};
            const u64 rejected = static_cast<u64>(
                std::ranges::count_if(metadata.m_materials, [palette](const u32 material) {
                    return particleMaterialVisual(palette, material) == nullptr;
                }));
            m_particle_description_rejections += rejected;
            if (rejected != 0u) {
                PPR_LOG(
                    GridPass, debug, "particle presentation carries materials the palette does not define",
                    {{"materials", rejected}, {"rejected_total", m_particle_description_rejections},
                        {"publication_materials", metadata.m_materials.size()}});
            }

            context.m_encoder.setBufferState(view.m_numerical, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(metadata.m_rows, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(view.m_readonly, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(m_world.m_terrain_palette, rhi::ResourceState::ShaderResource);
            PPR_RETURN_ERROR_ON_FAIL(GridPass, encode(0u));
            ++m_particle_pack_encodes;

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
        if (m_particle_timing_queries) {
            context.m_encoder.writeTimestamp(m_particle_timing_queries, 1u);
        }

        storage.m_prepared = true;
        storage.m_slot_encoded = true;
        return {};
    }

    Expected<GridParticleCensus> GridPass::readParticleCensus(
        const FenceCompletionValue completed, const FenceCompletionValue capture_submission) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, capture_submission != kFenceValueNone and
                              slotFenceComplete(completed, capture_submission) and
                              m_world.m_gpu_particles
                          ? std::error_code{}
                          : std::make_error_code(std::errc::invalid_argument));

        const auto &storage = *m_world.m_gpu_particles;
        const auto &slot = storage.m_slots[storage.m_selected_slot];
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, slot.m_reader and slot.m_census and slot.m_census->m_encoded and slot.m_census->m_device
                          ? std::error_code{}
                          : std::make_error_code(std::errc::state_not_recoverable));

        const auto &census = *slot.m_census;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, census.m_submission == capture_submission ? std::error_code{}
                                                                : std::make_error_code(std::errc::invalid_argument));

        const auto &view = *slot.m_reader;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GridPass, census.m_identity ==
                                  GridParticlePresentationIdentity{
                                      view.m_world, view.m_publication, view.m_terrain_revision, view.m_joint_tick} and
                              census.m_device_epoch == view.m_device_epoch and
                              census.m_count == view.m_metadata->m_count
                          ? std::error_code{}
                          : std::make_error_code(std::errc::state_not_recoverable));

        rhi::IDevice &device = *census.m_device;
        GridParticleCensus result{};
        result.m_metadata_owner = view.m_metadata;
        result.m_identity = census.m_identity;
        result.m_device_epoch = census.m_device_epoch;
        result.m_submission = capture_submission;
        result.m_world_edge = census.m_world_edge;

        // Include every live diagnostic slot, not only this captured vector.
        for (const auto &owned: storage.m_slots) {
            if (owned.m_census) {
                result.m_device_bytes += owned.m_census->m_projected->getDesc().size;
                result.m_readback_bytes += owned.m_census->m_readback->getDesc().size;
            }
        }

        result.m_rows.resize(census.m_count);
        result.m_cpu_bytes = result.m_rows.capacity() * sizeof(GridParticleCensusRow);
        if (not result.m_rows.empty()) {
            void *mapped{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GridPass, device.mapBuffer(census.m_readback, rhi::CpuAccessMode::Read, &mapped));
            std::memcpy(result.m_rows.data(), mapped, result.m_rows.size() * sizeof(GridParticleCensusRow));
            PPR_RETURN_UNEXPECTED_ON_FAIL(GridPass, device.unmapBuffer(census.m_readback));
        }

        return result;
    }

    std::error_code GridPass::renderGpuParticleClass_(
        const DrawContext &context, const EGridParticleDrawClass draw_class) {
        auto &storage = *m_world.m_gpu_particles;
        const u32 particle_slot = storage.m_selected_slot;

        // Plan §3: the class pipeline declares its own depth/blend state. The
        // gas lane keeps the retained depth-disabled lane with the blend state
        // its fragment entry implies; the restored opaque and optical classes
        // test depth, and only the opaque one writes it.
        const auto pipeline = particlePipeline_(context.m_device, context.m_render_pipeline_key, draw_class);
        PPR_RETURN_ERROR_ON_FAIL(GridPass, pipeline);

        auto *const root = (*pipeline)->m_slot_roots[particle_slot].get();
        rhi::ShaderCursor cursor{root};
        // The full surface set the descriptions need: g_terrain_visuals,
        // g_source_materials, g_textures/g_sampler, g_brdf_lut/g_brdf_sampler,
        // g_resolved_cells/g_fine_ch and g_backdrop. A missing binding fails
        // here rather than shading from a default. Binding first also derives
        // the frame availability word from the actual terrain-ownership binding
        // outcome for this submission.
        u32 availability{};
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, bindSurfaceBuffers_(
                          context.m_device, cursor, m_surface_retained_table, availability, *pipeline, particle_slot));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, uploadFrameConstants_((*pipeline)->m_slot_frame_cursors[particle_slot], availability));
        // Billboard directions and centers must use the same current snapshot,
        // including worlds whose background preparation returns early.
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, (*pipeline)->m_slot_frame_cursors[particle_slot]["m_inverse_view_projection"].setData(
                          &m_camera_view.m_invert_view_projection, sizeof(m_camera_view.m_invert_view_projection)));
        // The class is this draw's constant; bindSurfaceBuffers_ wrote the
        // terrain pass selector in the same buffer, so set both together.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindGridPass_(cursor, 0u, draw_class));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureParticleMask_(context.m_device));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_mask"].setBinding(rhi::Binding(m_particle_mask_view)));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, cursor["g_particle_mask_sampler"].setBinding(rhi::Binding(m_particle_mask_sampler)));
        // The restored order draws the particle classes before terrain, so the
        // terrain staging flag is not yet this frame's value here. The cell
        // source is chosen from the world's own state: the retained visibility
        // atlas when it carries cells, otherwise the legacy cell atlas.
        const bool has_cells = m_world.m_visibility_has_cells or m_world.m_cell_atlas != nullptr;
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

    std::error_code GridPass::renderSolidLiquidParticles(const DrawContext &context) {
        if (m_spatial_msaa) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSpatialContent());
        }

        if (m_world.m_diagnostic_mode != EGridDiagnosticMode::textured or not m_world.m_gpu_particles) {
            return {};
        }

        context.m_pass.pushDebugGroup("Particles.SolidLiquid", {});
        PPR_DEFER {
            context.m_pass.popDebugGroup();
        };

        // One preparation feeds every particle class of this frame. `update()`
        // reset it, so all three class draws share the ids and indirect
        // arguments that preparation produced: no second compaction and no
        // extra buffer. A held publication that has never packed retains no
        // rows, so it presents no particles; any other unprepared draw is a
        // caller that skipped preparation.
        if (not m_world.m_gpu_particles->m_prepared) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, m_world.m_particle_presentation == EParticlePresentation::held
                              ? std::error_code{}
                              : std::make_error_code(std::errc::operation_not_permitted));
            return {};
        }

        // The particle interval opens at the first class draw...
        if (m_particle_timing_queries) {
            context.m_pass.writeTimestamp(m_particle_timing_queries, 10u);
        }

        PPR_RETURN_ERROR_ON_FAIL(GridPass, renderGpuParticleClass_(context, EGridParticleDrawClass::opaque));
        return renderGpuParticleClass_(context, EGridParticleDrawClass::optical);
    }

    std::error_code GridPass::renderGasParticles(const DrawContext &context) {
        if (m_spatial_msaa) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSpatialContent());
        }

        if (m_world.m_diagnostic_mode != EGridDiagnosticMode::textured or not m_world.m_gpu_particles) {
            return {};
        }

        context.m_pass.pushDebugGroup("Particles.Gas", {});
        PPR_DEFER {
            context.m_pass.popDebugGroup();
        };

        // Same preparation and the same held-publication rule as the solid and
        // liquid classes: nothing retained presents no particles.
        if (not m_world.m_gpu_particles->m_prepared) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, m_world.m_particle_presentation == EParticlePresentation::held
                              ? std::error_code{}
                              : std::make_error_code(std::errc::operation_not_permitted));
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(GridPass, renderGpuParticleClass_(context, EGridParticleDrawClass::gas));
        // ...and closes after the last class draw, so the pair spans the three
        // classes even though the production order draws terrain opaque between
        // the solid/liquid and gas lanes.
        if (m_particle_timing_queries) {
            context.m_pass.writeTimestamp(m_particle_timing_queries, 11u);
        }

        return {};
    }
}
