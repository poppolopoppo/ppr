module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;
import :renderer.grid_pass;
import :renderer.world_lighting;
import :renderer.world_lighting_pass;
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
            u32 m_block_count{}, m_census_world_edge{};
            // Orientation publication: one candidate update per newly accepted
            // publication, the identity remap only on a changed metadata owner, the
            // accepted row count it searches, and this attempt's ownership binding
            // outcome. Host tokens, never a clock: there is no dt.
            u32 m_orientation_update{}, m_orientation_remap{};
            u32 m_orientation_previous_count{}, m_orientation_owned{};
            // Density-lane admission padding: one density pixel in world metres and
            // the one admitted lane material. The homogeneous guard expands only
            // that material's visibility margin, so no other row's cull changes.
            float m_surface_guard{};
            u32 m_surface_material{0xffff'ffffu};
        };

        static_assert(sizeof(ParticlePrepareFrame) == 144u and alignof(ParticlePrepareFrame) == 16u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_cell_metres) == 96u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_count) == 104u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_block_count) == 112u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_census_world_edge) == 116u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_orientation_update) == 120u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_orientation_remap) == 124u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_orientation_previous_count) == 128u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_orientation_owned) == 132u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_surface_guard) == 136u);
        static_assert(PPR_OFFSETOF(ParticlePrepareFrame, m_surface_material) == 140u);

        // Consecutive held preparations before the hold is reported at warning.
        // While the solver runs, the committed publication is normally one epoch
        // behind the capture, so a one-frame hold is the ordinary liveness lag and
        // stays at debug. Eight consecutive frames (~130 ms at 60 Hz) is far beyond
        // that lag while still bounded, so a persistent hold is visible once
        // without turning a normal solve cadence into log noise.
        inline constexpr u64 kParticleHoldWarnFrames{8u};
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
        auto *const orientation = field("g_orientation_current");
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
                    orientation and
                    orientation->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(ParticleOrientationGpu) and
                    orientation->getFieldCount() == 1u and
                    frame->getSize() == sizeof(ParticlePrepareFrame) and
                    frame->getFieldByIndex(1u)->getOffset() == PPR_OFFSETOF(ParticlePrepareFrame, m_cell_metres) and
                    frame->getFieldByIndex(3u)->getOffset() == PPR_OFFSETOF(ParticlePrepareFrame, m_count) and
                    frame->getFieldByIndex(7u)->getOffset() ==
                        PPR_OFFSETOF(ParticlePrepareFrame, m_orientation_update) and
                    frame->getFieldByIndex(9u)->getOffset() ==
                        PPR_OFFSETOF(ParticlePrepareFrame, m_orientation_previous_count)
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));

        // The orientation record's 4-byte stride is the canonical angle/bias/stretch
        // word at offset 0. A target that widens the record fails program creation
        // here instead of misreading every row.
        constexpr std::array<std::pair<const char *, u32>, 1u> orientation_fields{{
            {"angle_bias_stretch", 0u},
        }};
        bool orientation_layout_matches = orientation != nullptr and
                                          orientation->getFieldCount() ==
                                              static_cast<unsigned>(orientation_fields.size());
        for (const auto &[name, offset]: orientation_fields) {
            bool matched = false;
            for (unsigned index = 0u; orientation_layout_matches and index < orientation->getFieldCount(); ++index) {
                auto *const member = orientation->getFieldByIndex(index);
                if (name != std::string_view{member->getName()}) {
                    continue;
                }

                matched = member->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == offset;
                break;
            }

            orientation_layout_matches = orientation_layout_matches and matched;
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            orientation_layout_matches ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        // The 24-byte presentation row is a reflected ABI contract, not an
        // assumption: a target that lays out `half2 motion` or the material word
        // differently fails program creation here instead of misreading every
        // instance. The writer's names are the field contract grid_tiles decodes.
        constexpr std::array<std::pair<const char *, u32>, 5u> packed_fields{{
            {"position", 0u},
            {"radius", 8u},
            {"material_flags_temperature", 12u},
            {"uv_seed", 16u},
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

    auto GridPass::particleCullKey_(const u32 count) const noexcept -> ParticleCullKey {
        // The one definition of "what the compaction was encoded against": the
        // six clip planes, the density-lane guard and material, and the row
        // count. Both the pack and the held recull derive their constants and
        // this key from the same call, so the reuse decision cannot disagree with
        // the encoded cull.
        ParticleCullKey key{};
        for (u32 plane = 0u; plane < 6u; ++plane) {
            const auto &source = m_camera_view.m_frustum.clip[plane];
            std::memcpy(key.m_planes[plane].data(), source.data(), sizeof(key.m_planes[plane]));
        }

        key.m_guard = m_world.m_particle_surface.admitted() ? particleSurfacePixelWorld_(m_camera_view) : 0.0f;
        key.m_material = m_world.m_particle_surface.m_material;
        key.m_count = count;
        return key;
    }

    std::error_code GridPass::holdGpuParticles_(const PreRenderContext &context) {
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

            // A held frame draws the retained rows against the current camera, so
            // the retained visible-ID list is only valid while the camera it was
            // culled against is unchanged. When the cull inputs move - a pan, a
            // zoom or a raster change - rerasterise the retained rows from the
            // retained packed buffer; rerasterising the previous ID list would
            // keep the rows that left the view and could never reveal a row that
            // entered it. The retained cull is the last *committed* one, in the
            // slot it was committed for.
            if (particles.m_prepared and particles.m_packed_count != 0u) {
                const ParticleCullKey key = particleCullKey_(particles.m_packed_count);
                const bool retained = particles.m_cull_valid and
                                      particles.m_cull_slot == particles.m_selected_slot and
                                      particles.m_cull == key;
                if (not retained) {
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, recullGpuParticles_(context));
                }
            }
        }

        // While the solver runs, the committed publication is normally one epoch
        // behind the capture, so a short hold is the ordinary liveness lag, not an
        // exceptional state. The per-frame detail is therefore debug; only a hold
        // that persists is reported, once, at warning.
        PPR_LOG(
            GridPass, debug, "particle publication is not current; presenting the retained packed rows",
            {
                {"held_frames", m_world.m_particle_hold_attempts},
                {"device_epoch", m_world.m_gpu_particles ? m_world.m_gpu_particles->m_view.m_device_epoch : u64{}},
                {"live_epoch", m_world.m_gpu_particles and m_world.m_gpu_particles->m_view.m_live_epoch
                                   ? m_world.m_gpu_particles->m_view.m_live_epoch->load(std::memory_order_acquire)
                                   : u64{}},
                {"packed", m_world.m_gpu_particles ? m_world.m_gpu_particles->m_packed_owner != nullptr : false},
                {"packed_epoch", m_world.m_gpu_particles ? m_world.m_gpu_particles->m_packed_epoch : u64{}},
            });
        if (m_world.m_particle_hold_attempts == kParticleHoldWarnFrames) {
            PPR_LOG(
                GridPass, warning, "particle publication is still held; presenting the retained packed rows",
                {
                    {"held_frames", m_world.m_particle_hold_attempts},
                    {"threshold", kParticleHoldWarnFrames},
                    {"device_epoch", m_world.m_gpu_particles ? m_world.m_gpu_particles->m_view.m_device_epoch : u64{}},
                    {"live_epoch", m_world.m_gpu_particles and m_world.m_gpu_particles->m_view.m_live_epoch
                                       ? m_world.m_gpu_particles->m_view.m_live_epoch->load(std::memory_order_acquire)
                                       : u64{}},
                    {"packed", m_world.m_gpu_particles ? m_world.m_gpu_particles->m_packed_owner != nullptr : false},
                    {"packed_epoch", m_world.m_gpu_particles ? m_world.m_gpu_particles->m_packed_epoch : u64{}},
                });
        }

        return {};
    }

    // ------------------------------------------------------------------
    // held-view recull
    // ------------------------------------------------------------------

    std::error_code GridPass::recullGpuParticles_(const PreRenderContext &context) {
        auto &particles = *m_world.m_gpu_particles;
        const u32 count = particles.m_packed_count;
        // The retained rows' own metadata: a rejected attempt may have left a
        // different incoming owner installed in `m_view`.
        const auto &retained_metadata = particles.m_packed_metadata_owner;
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, particles.m_packed and retained_metadata and count != 0u
                          ? std::error_code{}
                          : std::make_error_code(std::errc::state_not_recoverable));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, m_particle_prepare_programs[1u].m_program != nullptr and
                              m_particle_prepare_programs[2u].m_program != nullptr and
                              m_particle_prepare_programs[3u].m_program != nullptr
                          ? std::error_code{}
                          : std::make_error_code(std::errc::not_supported));

        // The draw source the hold presents: the retained accepted history, never
        // this frame's candidate and never an unwritten role.
        rhi::IBuffer *const orientation = particles.m_orientation_draw.get();
        rhi::IBuffer *const orientation_candidate =
            particles.m_orientation[particles.m_orientation_accepted_role ^ 1u].get();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass, orientation and orientation_candidate ? std::error_code{}
                                                            : std::make_error_code(std::errc::state_not_recoverable));

        // The rank root declares the ownership grids even though no target is
        // computed this frame; the zero fallback keeps it complete and is never
        // read because the frame's ownership bit stays clear.
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureLightingFallback_(context.m_device));

        const hal::ProfileScope scope{"App.Grid.Particles.Recull"};

        // Completion-selected slot: the same ring, roots and fence protocol the
        // packed preparation uses, so a recull never rewrites ids or arguments a
        // submitted frame may still read.
        if (not m_visibility_prepared) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, selectSlot_(context.m_completed_fence_value, context.m_wait_for_fence));
        }

        particles.m_selected_slot = m_tile_ring_cursor;
        auto &slot = particles.m_slots[particles.m_selected_slot];
        // This slot carries no capture of a superseded epoch.
        slot.m_reader.reset();
        if (slot.m_census) {
            slot.m_census->m_encoded = false;
            slot.m_census->m_submission = kFenceValueNone;
        }

        const ParticleCullKey key = particleCullKey_(count);
        const auto &metadata = *retained_metadata;
        ParticlePrepareFrame frame{};
        for (u32 plane = 0u; plane < 6u; ++plane) {
            for (u32 component = 0u; component < 4u; ++component) {
                frame.m_planes[plane][component] = key.m_planes[plane][component];
            }
        }

        frame.m_cell_metres = static_cast<float>(metadata.m_cell_metres);
        frame.m_origin = static_cast<float>(particles.m_view.m_origin);
        frame.m_count = count;
        // Only the capture census reads this; the recull never encodes it.
        frame.m_moving_count = metadata.m_moving_count;
        frame.m_block_count = std::max(1u, (count + 255u) / 256u);
        frame.m_surface_guard = key.m_guard;
        frame.m_surface_material = key.m_material;
        // A hold advances no cue slew and writes no history: rank derives the
        // conic from the retained accepted word into the newly selected slot and
        // writes nothing to either orientation role.
        frame.m_orientation_update = 0u;
        frame.m_orientation_remap = 0u;
        frame.m_orientation_previous_count = 0u;
        frame.m_orientation_owned = 0u;

        for (auto *const buffer:
            {particles.m_ranks.get(), particles.m_counts.get(), particles.m_prefix.get(), slot.m_ids.get(),
                slot.m_args.get(), slot.m_conic.get()}) {
            context.m_encoder.setBufferState(buffer, rhi::ResourceState::UnorderedAccess);
        }
        context.m_encoder.setBufferState(particles.m_packed, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(orientation, rhi::ResourceState::ShaderResource);

        rhi::IBuffer *const previous_metadata = particles.m_orientation_metadata
                                                    ? particles.m_orientation_metadata->m_rows.get()
                                                    : metadata.m_rows.get();
        const u32 blocks = frame.m_block_count;
        constexpr std::array<const char *, 3u> names{
            "Particles.Recull", "Particles.RecullPrefix", "Particles.RecullCompact"};
        for (u32 phase = 0u; phase < 3u; ++phase) {
            context.m_encoder.pushDebugGroup(names[phase], {});
            PPR_DEFER {
                context.m_encoder.popDebugGroup();
            };

            const u32 program = phase + 1u;
            auto *const root = particles.m_roots[program][particles.m_selected_slot].get();
            rhi::ShaderCursor cursor{root};
            const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, cursor[name].setBinding(rhi::Binding(buffer, particleRange(buffer))));
                return {};
            };

            if (phase == 0u) {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_ranks", particles.m_ranks));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_packed", particles.m_packed));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_block_counts", particles.m_counts));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_metadata", metadata.m_rows));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_previous_metadata", previous_metadata));
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass,
                    cursor["g_orientation_previous"].setBinding(rhi::Binding(orientation, particleRange(orientation))));
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, cursor["g_orientation_current"].setBinding(
                                  rhi::Binding(orientation_candidate, particleRange(orientation_candidate))));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_particle_conic", slot.m_conic));
                // The orientation target is not computed, so no fill or winner
                // grid is read; the fallback keeps the root complete.
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bindOwnershipFallback_(cursor));
            } else if (phase == 1u) {
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_block_counts", particles.m_counts));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_block_prefix", particles.m_prefix));
            } else {
                // The scatter root is a separate root object from the rank phase
                // (per program AND slot), so it must bind every field the program
                // reads. `scatterParticles` reads g_ranks to decide whether and
                // where each ID is stored; omitting it makes a held recull's stores
                // depend on a previous frame that happened to visit this root. The
                // indirect count comes straight from the block prefix, so correct
                // arguments never prove correct ID stores.
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_ranks", particles.m_ranks));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_block_prefix", particles.m_prefix));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_particle_ids", slot.m_ids));
                PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_particle_args", slot.m_args));
            }

            rhi::ShaderCursor constants;
            PPR_RETURN_ERROR_ON_FAIL(GridPass, cursor["g_particle_prepare"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, constants.setData(&frame, sizeof(frame)));

            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, pass != nullptr ? std::error_code{} : std::make_error_code(std::errc::io_error));
            {
                PPR_DEFER {
                    pass->end();
                };
                pass->bindPipeline(m_particle_prepare_programs[program].m_pipeline, root);
                pass->dispatchCompute(program == 2u ? 1u : blocks, 1u, 1u);
                ++m_dispatch_commands;
            }

            context.m_encoder.globalBarrier();
        }

        // Order this frame's reads after the recull's writes: the draw reads the
        // retained rows, the ids/arguments just produced and the conic record
        // that was re-derived from the retained canonical word.
        context.m_encoder.setBufferState(particles.m_packed, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(slot.m_ids, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(slot.m_args, rhi::ResourceState::IndirectArgument);
        context.m_encoder.setBufferState(slot.m_conic, rhi::ResourceState::ShaderResource);

        particles.m_pending_cull = key;
        particles.m_pending_cull_valid = true;
        particles.m_slot_encoded = true;
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

        // A view of a different world/reset lineage cannot inherit the accepted
        // orientation history, even when its ID words numerically repeat. The
        // ordinary world-change path also replaces the whole particle storage, so
        // this is the defensive case of a reused container.
        if (m_world.m_gpu_particles->m_orientation_valid and
            m_world.m_gpu_particles->m_orientation_world != view.m_world) {
            resetParticleOrientation_();
        }

        m_world.m_gpu_particles->m_view = view;
        m_world.m_particle_presentation = EParticlePresentation::current;
        return {};
    }

    std::error_code GridPass::ensureGpuParticleStorage_(const PreRenderContext &context, const u32 count) {
        auto &storage = *m_world.m_gpu_particles;
        if (storage.m_capacity >= std::max(1u, count)) {
            return {};
        }

        rhi::IDevice &device = context.m_device;

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
            // Six-byte conic draw records, pair-packed into uints: 12 bytes per
            // two capacity rows. Written by the rank invocation that also
            // advances the canonical history, so every pair containing a drawn
            // row is fully written before the slot is drawn; an odd final record
            // is neutral and unused.
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, particleBuffer(
                              device, gridParticleConicBytes(replacement->m_capacity), sizeof(u32), nullptr,
                              replacement->m_slots[slot].m_conic));
            for (u32 program = 0u; program < m_particle_prepare_programs.size(); ++program) {
                PPR_RETURN_ERROR_ON_FAIL(
                    GridPass, device.createRootShaderObject(
                                  m_particle_prepare_programs[program].m_program,
                                  replacement->m_roots[program][slot].writeRef()));
            }
        }

        // Both orientation roles are allocated with the replacement. There is no
        // old-to-new copy: the complete accepted history stays readable through
        // `m_orientation_history` (which aliases the retired storage's accepted
        // allocation until the first post-growth candidate submission succeeds).
        // Survivors may reset on a capacity increase (accepted trade), and no fresh
        // role is ever read before it has been fully written by an accepted update.
        const u64 orientation_bytes = static_cast<u64>(replacement->m_capacity) * sizeof(ParticleOrientationGpu);
        for (auto &role: replacement->m_orientation) {
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass, particleBuffer(device, orientation_bytes, sizeof(ParticleOrientationGpu), nullptr, role));
        }

        replacement->m_orientation_accepted_role = storage.m_orientation_accepted_role;
        replacement->m_orientation_role_valid = false;
        replacement->m_orientation_history = storage.m_orientation_history;
        if (not replacement->m_orientation_history and storage.m_orientation_valid) {
            replacement->m_orientation_history = storage.m_orientation[storage.m_orientation_accepted_role];
        }
        // A history is only valid when a complete allocation actually survived.
        replacement->m_orientation_valid = storage.m_orientation_valid and
                                           replacement->m_orientation_history.get() != nullptr;
        replacement->m_orientation_count = replacement->m_orientation_valid ? storage.m_orientation_count : 0u;
        replacement->m_orientation_metadata = storage.m_orientation_metadata;
        replacement->m_orientation_identity = storage.m_orientation_identity;
        replacement->m_orientation_world = storage.m_orientation_world;
        replacement->m_orientation_version = storage.m_orientation_version;
        replacement->m_orientation_draw = replacement->m_orientation_history;
        if (not replacement->m_orientation_draw) {
            replacement->m_orientation_draw = replacement->m_orientation[replacement->m_orientation_accepted_role];
        }

        // The callable fallback requires a copyable owner through the reader fence.
        std::shared_ptr<GpuParticleStorage> retired{std::move(m_world.m_gpu_particles)};
        m_world.m_gpu_particles = std::move(replacement);
        m_deferred_releases.defer(m_world.m_last_reader, [held = std::move(retired)]() mutable { held.reset(); });
        return {};
    }

    // ------------------------------------------------------------------
    // orientation history lifetime
    // ------------------------------------------------------------------

    void GridPass::resetParticleOrientation_() noexcept {
        if (not m_world.m_gpu_particles) {
            return;
        }

        auto &particles = *m_world.m_gpu_particles;
        // In-flight submissions may still read a replaced allocation; the deferred
        // owner retires it past the last reader. A history that aliases a role holds
        // an extra reference, so releasing both is safe. The holder is passed by
        // reference because ComPtr deletes address-of on an owning pointer.
        const auto retire = [this](rhi::ComPtr<rhi::IBuffer> &holder) noexcept {
            if (not holder) {
                return;
            }

            auto held = std::move(holder);
            m_deferred_releases.defer(m_world.m_last_reader, [kept = std::move(held)]() mutable { kept.setNull(); });
        };
        for (auto &role: particles.m_orientation) {
            retire(role);
        }
        retire(particles.m_orientation_history);
        retire(particles.m_orientation_draw);

        particles.m_orientation_accepted_role = 0u;
        particles.m_orientation_count = 0u;
        particles.m_pending_orientation_count = 0u;
        particles.m_orientation_role_valid = false;
        particles.m_orientation_valid = false;
        particles.m_orientation_pending = false;
        particles.m_orientation_metadata.reset();
        particles.m_pending_orientation_metadata.reset();
        particles.m_orientation_world = {};
        particles.m_pending_orientation_world = {};
        particles.m_orientation_identity = {};
        particles.m_pending_orientation_identity = {};
        particles.m_orientation_version = 0u;
        particles.m_pending_orientation_version = 0u;
    }

    void GridPass::settleParticleAttempt_() noexcept {
        // A definitely-unsubmitted frame never fenced its density targets and its
        // composite must not read them; the flag lives outside the particle
        // storage so it clears even before the first publication.
        m_particle_surface_encoded = false;
        m_particle_surface_ready = false;
        if (not m_world.m_gpu_particles) {
            return;
        }

        auto &particles = *m_world.m_gpu_particles;
        // A pending token can only survive a frame that was definitely not
        // submitted: an accepted submission commits in notifyFrameSubmitted, and an
        // ambiguous one quarantines admission so no later frame reaches here.
        particles.m_prepared = false;
        particles.m_pending_pack = false;
        particles.m_pending_owner.reset();
        particles.m_pending_epoch = 0u;
        particles.m_pending_metadata = 0u;
        particles.m_pending_terrain = 0u;
        particles.m_pending_count = 0u;
        // A pending compaction key can only survive a frame that was definitely
        // not submitted. Its encoded ids, arguments and conic never reached the
        // device, so the retained compaction is still the last committed one:
        // clear the pending key and put the selected slot back on the committed
        // cull, which is what a later hold must reuse.
        particles.m_pending_cull_valid = false;
        if (particles.m_cull_valid) {
            particles.m_selected_slot = particles.m_cull_slot;
        }
        // A rejected attempt's metadata owner is dropped; the accepted owner of the
        // rows that are still drawable is retained.
        particles.m_pending_packed_metadata_owner.reset();
        particles.m_orientation_pending = false;
        particles.m_pending_orientation_metadata.reset();
        particles.m_pending_orientation_identity = {};
        particles.m_pending_orientation_world = {};
        particles.m_pending_orientation_version = 0u;
        particles.m_pending_orientation_count = 0u;
        particles.m_orientation_draw = particles.m_orientation_role_valid
                                           ? particles.m_orientation[particles.m_orientation_accepted_role]
                                           : particles.m_orientation_history;
    }

    // ------------------------------------------------------------------
    // GPU circle packing and stable visible row IDs
    // ------------------------------------------------------------------

    std::error_code GridPass::prepareGpuParticles(const PreRenderContext &context, const u32 census_world_edge) {
        // A previous frame that prepared particles but was definitely not submitted
        // leaves its candidate token and draw source behind; clear them before any
        // early return, including a hold, so a stale candidate can never be drawn or
        // committed later.
        settleParticleAttempt_();

        // The density lane is derived before every early return, including a hold:
        // a held frame draws the retained rows directly and must exclude the same
        // material the density pass reconstructs, or the two representations would
        // overlap.
        deriveParticleSurfaceLane_();

        if (m_spatial_msaa) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, validateSpatialContent());
            PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureSpatialPrograms_(context.m_device));
        }

        PPR_RETURN_ERROR_ON_FAIL(GridPass, admitParticleSeeds_(context.m_device));
        if (not m_world.m_gpu_particles) {
            // No committed view has ever been installed. A held stage still
            // presents no particles and is counted; a seed-only world keeps its
            // CPU admission path. Neither is an error.
            return m_world.m_particle_presentation == EParticlePresentation::held ? holdGpuParticles_(context)
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
            return holdGpuParticles_(context);
        }

        // A current publication ends the hold: this frame re-packs from the new
        // epoch and resumes. The normal one-frame lag exits silently (debug);
        // recovering from a persistent hold is reported once at info. There is no
        // timeout: expiry is a publication or a world event.
        if (m_world.m_particle_hold_attempts != 0u) {
            PPR_LOG(
                GridPass, debug, "particle publication is current again; resuming the packed presentation",
                {
                    {"held_frames", m_world.m_particle_hold_attempts},
                    {"device_epoch", m_world.m_gpu_particles->m_view.m_device_epoch},
                });
            if (m_world.m_particle_hold_attempts >= kParticleHoldWarnFrames) {
                PPR_LOG(
                    GridPass, info,
                    "particle publication recovered after a persistent hold; resuming the packed presentation",
                    {
                        {"held_frames", m_world.m_particle_hold_attempts},
                        {"threshold", kParticleHoldWarnFrames},
                        {"device_epoch", m_world.m_gpu_particles->m_view.m_device_epoch},
                    });
            }
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
        PPR_RETURN_ERROR_ON_FAIL(GridPass, ensureGpuParticleStorage_(context, count));
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

        ParticleCullKey cull = particleCullKey_(count);
        ParticlePrepareFrame frame{};
        for (u32 plane = 0u; plane < 6u; ++plane) {
            for (u32 component = 0u; component < 4u; ++component) {
                frame.m_planes[plane][component] = cull.m_planes[plane][component];
            }
        }

        frame.m_cell_metres = static_cast<float>(metadata.m_cell_metres);
        frame.m_origin = static_cast<float>(view.m_origin);
        frame.m_count = count;
        frame.m_moving_count = metadata.m_moving_count;
        frame.m_block_count = blocks;
        frame.m_surface_guard = cull.m_guard;
        frame.m_surface_material = cull.m_material;

        // Orientation publication planning. The orientation clock is the accepted
        // presentation identity (publication, terrain revision, joint tick) plus the
        // metadata owner, not the pack token: a terrain-only accepted publication can
        // change the fill/winners without a new particle source and must still
        // update. `pack` stays the source-row clock and is independent, so an
        // orientation-only update never forces a redundant pack. There is no dt and
        // no render-frame counter in this decision.
        const GridParticlePresentationIdentity presentation{
            view.m_world, view.m_publication, view.m_terrain_revision, view.m_joint_tick};
        const bool pack = storage.m_packed_owner != view.m_live_epoch or
                          storage.m_packed_epoch != view.m_device_epoch or
                          storage.m_packed_metadata != metadata.m_version;
        const bool owner_changed = storage.m_orientation_valid and
                                   storage.m_orientation_metadata.get() != view.m_metadata.get();
        // World replacement, a checkpoint restore (a metadata owner whose version
        // does not advance past the accepted one), and device recreation start a
        // new lineage: zero history even if the ID words repeat. Replacing the
        // storage itself is the world-change reset; this covers the reused case.
        const bool lineage_reset = not storage.m_orientation_valid or
                                   storage.m_orientation_world != view.m_world or
                                   (owner_changed and metadata.m_version <= storage.m_orientation_version);
        const bool remap = owner_changed and not lineage_reset;
        const bool orientation_update = lineage_reset or
                                        owner_changed or
                                        storage.m_orientation_identity != presentation;
        const u32 previous_count = storage.m_orientation_valid and not lineage_reset ? storage.m_orientation_count : 0u;
        const u32 orientation_write_role = storage.m_orientation_accepted_role ^ 1u;

        frame.m_orientation_update = orientation_update ? 1u : 0u;
        frame.m_orientation_remap = remap ? 1u : 0u;
        frame.m_orientation_previous_count = previous_count;
        // This frame's draw reads what this frame encoded; a repeated publication
        // reads the accepted role (or the retained history after growth) and
        // performs no write at all.
        storage.m_orientation_draw = orientation_update
                                         ? storage.m_orientation[orientation_write_role]
                                         : (storage.m_orientation_role_valid
                                                   ? storage.m_orientation[storage.m_orientation_accepted_role]
                                                   : storage.m_orientation_history);
        storage.m_orientation_pending = false;

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
                slot.m_ids.get(), slot.m_args.get(), slot.m_conic.get()}) {
            context.m_encoder.setBufferState(buffer, rhi::ResourceState::UnorderedAccess);
        }
        // The retained complete history is read and the candidate role is written;
        // they are distinct allocations (except that a growth's fresh roles are
        // never the history), so the descriptors never overlap.
        rhi::IBuffer *const orientation_read = storage.m_orientation_history
                                                   ? storage.m_orientation_history.get()
                                                   : storage.m_orientation[orientation_write_role].get();
        context.m_encoder.setBufferState(orientation_read, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(
            storage.m_orientation[orientation_write_role], rhi::ResourceState::UnorderedAccess);

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
                    // The derived six-byte conic record: rank derives each pair
                    // from the canonical word it just wrote (or read, on a repeated
                    // publication) into this prepared slot.
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_particle_conic", slot.m_conic));
                    // Orientation history: this publication's candidate is written
                    // from the retained complete history (which may be a retired
                    // allocation after growth). The metadata ID search reads the
                    // accepted metadata owner only when the inventory changed;
                    // otherwise row identity is direct and no per-row ID is read.
                    rhi::IBuffer *const previous = orientation_read;
                    rhi::IBuffer *const candidate = storage.m_orientation[orientation_write_role];
                    PPR_RETURN_ERROR_ON_FAIL(
                        GridPass,
                        cursor["g_orientation_previous"].setBinding(rhi::Binding(previous, particleRange(previous))));
                    PPR_RETURN_ERROR_ON_FAIL(
                        GridPass,
                        cursor["g_orientation_current"].setBinding(rhi::Binding(candidate, particleRange(candidate))));
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_metadata", metadata.m_rows));
                    rhi::IBuffer *const previous_metadata = storage.m_orientation_metadata
                                                                ? storage.m_orientation_metadata->m_rows.get()
                                                                : metadata.m_rows.get();
                    PPR_RETURN_ERROR_ON_FAIL(
                        GridPass, cursor["g_previous_metadata"].setBinding(
                                      rhi::Binding(previous_metadata, particleRange(previous_metadata))));

                    // The committed per-cell grids come from the one ownership
                    // authority, which binds this submission's projection attempt
                    // (the fill and winners this very publication was projected
                    // from) or reports that none is available. With no ownership
                    // the tangent confidence is zero and the row's own motion is
                    // the fallback; a fallback grid is never read. The authority is
                    // only consulted for a world that publishes its categorical
                    // cells, exactly as the terrain binder gates it; a capture or
                    // reference caller without lighting keeps the fallback.
                    bool ownership_bound = false;
                    const bool published = m_world.m_lighting and
                                           m_world.m_lighting->hasPublishedCells() and
                                           m_world.m_world_edge == WorldLightingPass::kWorldEdge;
                    if (published) {
                        const std::error_code ownership = m_world.m_lighting->bindTerrainOwnership(
                            cursor, ownership_bound);
                        if (ownership == std::errc::resource_unavailable_try_again) {
                            ownership_bound = false;
                        } else {
                            PPR_RETURN_ERROR_ON_FAIL(GridPass, ownership);
                        }
                    }
                    if (not ownership_bound) {
                        PPR_RETURN_ERROR_ON_FAIL(GridPass, bindOwnershipFallback_(cursor));
                    }
                    frame.m_orientation_owned = ownership_bound ? 1u : 0u;
                } else {
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_block_prefix", storage.m_prefix));
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_particle_ids", slot.m_ids));
                    PPR_RETURN_ERROR_ON_FAIL(GridPass, bind("g_particle_args", slot.m_args));
                }
            }

            // Constants are written after the bindings because the rank phase
            // derives its ownership bit from the actual binding outcome above.
            PPR_RETURN_ERROR_ON_FAIL(GridPass, constants.setData(&frame, sizeof(frame)));

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
            storage.m_pending_count = count;
            // Retain the exact owner these rows were packed from; it becomes the
            // accepted metadata of the drawable cohort on submission.
            storage.m_pending_packed_metadata_owner = view.m_metadata;
            storage.m_pending_pack = true;
        }

        PPR_RETURN_ERROR_ON_FAIL(GridPass, encode(1u));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, encode(2u));
        PPR_RETURN_ERROR_ON_FAIL(GridPass, encode(3u));
        context.m_encoder.setBufferState(storage.m_packed, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(slot.m_ids, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(slot.m_args, rhi::ResourceState::IndirectArgument);
        // The draw reads the conic record this frame's rank derived into the same
        // prepared slot.
        context.m_encoder.setBufferState(slot.m_conic, rhi::ResourceState::ShaderResource);
        // The draw reads what this frame resolved (the candidate it encoded, or the
        // accepted role/history on a repeated publication); order that read after
        // the rank write.
        context.m_encoder.setBufferState(storage.m_orientation_draw.get(), rhi::ResourceState::ShaderResource);

        // The compaction inputs this slot's ids, arguments and conic were encoded
        // from. Committed with the submission, so a rejected encoding never
        // describes the retained slot.
        storage.m_pending_cull = cull;
        storage.m_pending_cull_valid = true;

        // The candidate becomes accepted only with the scene-A submission: this
        // pending record is committed by notifyFrameSubmitted, never here, and only
        // when this frame encoded an orientation update. A rejected encoding leaves
        // the accepted history, its metadata owner and its presentation identity
        // untouched.
        if (orientation_update) {
            storage.m_orientation_pending = true;
            storage.m_pending_orientation_metadata = view.m_metadata;
            storage.m_pending_orientation_identity = presentation;
            storage.m_pending_orientation_world = view.m_world;
            storage.m_pending_orientation_version = metadata.m_version;
            storage.m_pending_orientation_count = count;
        }
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
        // Orientation history: the vertex stage decodes the stored angle and the two
        // amplitudes. Gas decodes nothing but still binds the resolved source so the
        // root has no unbound field.
        rhi::IBuffer *const orientation = storage.m_orientation_draw.get();
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            orientation != nullptr ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
        PPR_RETURN_ERROR_ON_FAIL(
            GridPass,
            cursor["g_particle_orientation"].setBinding(rhi::Binding(orientation, particleRange(orientation))));
        auto &slot = storage.m_slots[particle_slot];
        // The direct classes never read the six-byte conic record; the shared
        // program declares it, so bind the same prepared slot the density lane
        // uses when this program kept the field.
        const rhi::ShaderCursor conic_field = cursor["g_particle_conic"];
        if (conic_field.isValid()) {
            rhi::IBuffer *const conic = slot.m_conic.get();
            PPR_RETURN_ERROR_ON_FAIL(
                GridPass,
                conic != nullptr ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));
            PPR_RETURN_ERROR_ON_FAIL(GridPass, conic_field.setBinding(rhi::Binding(conic, particleRange(conic))));
        }
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

        // The admitted density lane is composited at its own class slot: after that
        // class's direct draw and before the other class. The direct draw already
        // rejected the lane material (bindGridPass_ writes it only when this frame
        // accumulated the field), so exactly one representation is visible.
        const bool surface_lane = m_particle_surface_ready and m_world.m_particle_surface.admitted();
        const EGridParticleDrawClass surface_class = m_world.m_particle_surface.m_draw_class;

        PPR_RETURN_ERROR_ON_FAIL(GridPass, renderGpuParticleClass_(context, EGridParticleDrawClass::opaque));
        if (surface_lane and surface_class == EGridParticleDrawClass::opaque) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, renderParticleSurface(context, EGridParticleDrawClass::opaque));
        }

        PPR_RETURN_ERROR_ON_FAIL(GridPass, renderGpuParticleClass_(context, EGridParticleDrawClass::optical));
        if (surface_lane and surface_class == EGridParticleDrawClass::optical) {
            PPR_RETURN_ERROR_ON_FAIL(GridPass, renderParticleSurface(context, EGridParticleDrawClass::optical));
        }

        return {};
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
