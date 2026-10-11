module;
#include "pP/Macros.h"
module engine.sim;
import :worldgen.result;
import :worldgen.layout;
import :worldgen.geometry;
import :worldgen.plan;
import :worldgen;
import :chunk_grid;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldResult, info, none);
    namespace {
        struct Checksum {
            u64 m_value{14695981039346656037ull};
            void integer(u64 value, const u32 bytes) noexcept {
                for (u32 i = 0u; i < bytes; ++i) {
                    m_value = (m_value ^ (value & 255u)) * 1099511628211ull;
                    value >>= 8u;
                }
            }
            void number(const double value) noexcept {
                integer(std::bit_cast<u64>(value), 8u);
            }
        };
        [[nodiscard]] std::error_code canceled(const GenerationExecution &execution) noexcept {
            return execution.m_context != nullptr ? execution.m_context->error() : std::error_code{};
        }
        void stage(const GenerationExecution &execution, const EGenerationStage value) noexcept {
            if (execution.m_stage != nullptr) {
                execution.m_stage->store(value, std::memory_order_release);
            }
        }
        [[nodiscard]] Expected<GenerationReport> summarize(
            const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry,
            const ChunkGrid &grid, const InitialMatter &matter, const GenerationStart &start,
            const GenerationExecution &execution, u64 &known_scratch_peak) {
            const auto &recipe = plan.recipe();
            Array<u8> terrain(65536u, 0u);
            Array<u64> counts(65536u, 0ull);
            known_scratch_peak = std::max(
                known_scratch_peak, terrain.capacity() * sizeof(u8) + counts.capacity() * sizeof(u64));
            terrain[0u] = 1u;
            for (const auto &material: recipe.m_materials) {
                terrain[material.m_id] = material.m_terrain ? 1u : 0u;
            }
            Checksum cells;
            // Domain/version followed by chunk-major, local row-major payload.
            cells.integer(0x7070722e67726964ull, 8u);
            cells.integer(kGeneratorAlgorithmVersion, 4u);
            for (u32 cy = 0u; cy < kChunksPerEdge; ++cy) {
                for (u32 cx = 0u; cx < kChunksPerEdge; ++cx) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, canceled(execution));
                    const auto payload = grid.residentCells({cx, cy});
                    if (not payload) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, std::errc::invalid_argument);
                    }
                    for (u32 i = 0u; i < kCellsPerChunk; ++i) {
                        const auto cell = (*payload)[i];
                        const u32 x = cx * kChunkEdge + i % kChunkEdge;
                        const u32 y = cy * kChunkEdge + i / kChunkEdge;
                        const bool boundary = x < recipe.m_boundary_fine or
                                              y < recipe.m_boundary_fine or
                                              x >= kWorldEdge - recipe.m_boundary_fine or
                                              y >= kWorldEdge - recipe.m_boundary_fine;
                        if (terrain[cell.m_element] == 0u or
                            not std::isfinite(cell.m_temperature) or
                            (cell.m_element != 0u and cell.m_temperature <= 0.0f) or
                            (boundary and cell.m_element != recipe.m_boundary_material)) {
                            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, std::errc::invalid_argument);
                        }
                        ++counts[cell.m_element];
                        cells.integer(cell.m_element, 2u);
                        cells.integer(std::bit_cast<u32>(cell.m_temperature), 4u);
                    }
                }
            }
            Checksum output;
            output.integer(0x7070722e776f726cull, 8u);
            output.integer(kGeneratorAlgorithmVersion, 4u);
            output.integer(plan.seed(), 8u);
            output.number(recipe.m_cell_size_m);
            output.number(recipe.m_reference_thickness_m);
            output.integer(cells.m_value, 8u);
            output.integer(layout.sites().size(), 8u);
            for (const auto &site: layout.sites()) {
                output.number(site.m_x_m);
                output.number(site.m_y_m);
                output.integer(recipe.m_biomes[site.m_biome].m_key, 8u);
            }
            output.integer(geometry.m_instances.size(), 8u);
            for (const auto &instance: geometry.m_instances) {
                output.integer(recipe.m_features[instance.m_feature].m_key, 8u);
                output.integer(
                    instance.m_rule == kGenerationStartRule ? 0ull : recipe.m_rules[instance.m_rule].m_key, 8u);
                output.integer(instance.m_ordinal, 4u);
                output.integer(static_cast<u32>(instance.m_origin_x), 4u);
                output.integer(static_cast<u32>(instance.m_origin_y), 4u);
                output.integer(instance.m_reflected ? 1u : 0u, 1u);
            }
            output.integer(geometry.m_segments.size(), 8u);
            for (const auto &segment: geometry.m_segments) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, canceled(execution));
                output.integer(recipe.m_rules[segment.m_rule].m_key, 8u);
                output.number(segment.m_x0_m);
                output.number(segment.m_y0_m);
                output.number(segment.m_x1_m);
                output.number(segment.m_y1_m);
                output.number(segment.m_radius_m);
            }
            output.integer(geometry.m_omitted_instances.size(), 8u);
            for (const auto omitted: geometry.m_omitted_instances) {
                output.integer(recipe.m_rules[omitted].m_key, 8u);
            }
            output.number(start.m_spawn_x_m);
            output.number(start.m_spawn_y_m);
            output.number(start.m_floor_y_m);
            output.integer(start.m_resource_fine_cells.size(), 8u);
            for (const auto count: start.m_resource_fine_cells) {
                output.integer(count, 4u);
            }
            output.integer(matter.m_regions.size(), 8u);
            for (const auto &region: matter.m_regions) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, canceled(execution));
                output.integer(region.m_identity, 8u);
                output.integer(region.m_material, 2u);
                output.number(region.m_temperature_k);
                output.number(region.m_bulk_density_kg_m3);
                output.number(region.m_volume_m3);
                output.number(region.m_mass_kg);
                output.integer(region.m_fill_height_m.has_value() ? 1u : 0u, 1u);
                if (region.m_fill_height_m) {
                    output.number(*region.m_fill_height_m);
                }
                output.integer(region.m_spans.size(), 8u);
                for (const auto &span: region.m_spans) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, canceled(execution));
                    output.integer(span.m_y, 4u);
                    output.integer(span.m_begin_x, 4u);
                    output.integer(span.m_end_x, 4u);
                }
            }
            GenerationReport report;
            report.m_terrain_checksum = cells.m_value;
            report.m_output_checksum = output.m_value;
            report.m_grid_payload_bytes = static_cast<u64>(grid.residentChunkCount()) * kCellsPerChunk * sizeof(Cell);
            for (u32 id = 0u; id < counts.size(); ++id) {
                if (counts[id] > 0u) {
                    report.m_material_counts.push_back({static_cast<u16>(id), counts[id]});
                }
            }
            report.m_retained_known_payload_bytes = plan.retainedPayloadBytes() +
                                                    layout.retainedPayloadBytes() +
                                                    grid.retainedPayloadBytes() +
                                                    geometry.m_instances.capacity() * sizeof(GenerationInstance) +
                                                    geometry.m_segments.capacity() * sizeof(GenerationSegment) +
                                                    geometry.m_omitted_instances.capacity() * sizeof(u32) +
                                                    matter.m_regions.capacity() * sizeof(InitialMatterRegion) +
                                                    start.m_resource_fine_cells.capacity() * sizeof(u32) +
                                                    report.m_material_counts.capacity() *
                                                        sizeof(GenerationMaterialCount);
            for (const auto &region: matter.m_regions) {
                report.m_retained_known_payload_bytes += region.m_spans.capacity() * sizeof(InitialMatterSpan);
            }
            return report;
        }
    }
    Expected<GeneratedWorld> generateWorld(
        GenerationPlan plan, const ITimerClock &clock, const GenerationExecution &execution) {
        try {
            std::array<TimeSpan, 5u> timings;
            std::array<u64, 5u> scratch{};
            auto beginning = clock.now();
            const auto finish = [&](const u32 phase) {
                const auto end = clock.now();
                timings[phase] = end - beginning;
                beginning = end;
            };
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, canceled(execution));
            if (execution.m_completed_chunks != nullptr) {
                execution.m_completed_chunks->store(0u, std::memory_order_relaxed);
            }
            stage(execution, EGenerationStage::layout);
            auto layout = compileBiomeLayout(plan, execution.m_context, &scratch[0u]);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, layout);
            finish(0u);
            auto geometry = compileGenerationGeometry(plan, *layout, execution.m_context, &scratch[1u]);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, geometry);
            finish(1u);
            auto grid = rasterizeGenerationTerrain(plan, *layout, *geometry, execution, &scratch[2u]);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, grid);
            finish(2u);
            stage(execution, EGenerationStage::initial_matter);
            auto matter = resolveInitialMatter(plan, *geometry, *grid, execution.m_context, &scratch[3u]);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, matter);
            finish(3u);
            stage(execution, EGenerationStage::validation);
            auto start = validateGenerationStart(plan, *grid, execution.m_context, &scratch[4u]);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, start);
            auto report = summarize(plan, *layout, *geometry, *grid, *matter, *start, execution, scratch[4u]);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, report);
            finish(4u);
            report->m_phase_times = timings;
            report->m_known_phase_scratch_bytes = scratch;
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, canceled(execution));
            stage(execution, EGenerationStage::complete);
            return GeneratedWorld{
                std::move(plan), std::move(*layout), std::move(*geometry), std::move(*grid), std::move(*matter),
                std::move(*start), std::move(*report)};
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::system_error &error) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, error.code());
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, std::errc::state_not_recoverable);
        } catch (...) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldResult, std::errc::state_not_recoverable);
        }
        std::unreachable();
    }
}
