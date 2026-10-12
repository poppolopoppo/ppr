module;
#include "pP/Macros.h"
module engine.sim;
import :worldgen.raster;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldRaster, info, none);
    namespace {
        [[nodiscard]] std::error_code canceled(const IContext *const context) noexcept {
            return context != nullptr ? context->error() : std::error_code{};
        }
        [[nodiscard]] bool depthContains(const double depth, const double low, const double high) noexcept {
            return depth >= low and (depth < high or high == 1.0);
        }
        [[nodiscard]] bool reserved(
            const GenerationRecipe &recipe, const GenerationInstance &instance, const u32 x, const u32 y) noexcept {
            const auto &bounds = recipe.m_features[instance.m_feature].m_bounds;
            const i64 local_x = static_cast<i64>(x) - instance.m_origin_x;
            const i64 local_y = static_cast<i64>(y) - instance.m_origin_y;
            return local_x >= bounds.m_min_x and
                   local_x < bounds.m_max_x and
                   local_y >= bounds.m_min_y and
                   local_y < bounds.m_max_y;
        }
        [[nodiscard]] bool capsule(
            const double x, const double y, const double x0, const double y0, const double x1, const double y1,
            const double radius) noexcept {
            const double dx = x1 - x0, dy = y1 - y0;
            const double length_squared = dx * dx + dy * dy;
            const double t = length_squared == 0.0
                                 ? 0.0
                                 : std::clamp(((x - x0) * dx + (y - y0) * dy) / length_squared, 0.0, 1.0);
            return std::hypot(x - x0 - t * dx, y - y0 - t * dy) <= radius;
        }
        [[nodiscard]] bool operationContains(
            const GenerationRecipe &recipe, const GenerationInstance &instance, const GenerationOperation &operation,
            const u32 x, const u32 y) noexcept {
            const auto &bounds = recipe.m_features[instance.m_feature].m_bounds;
            double local_x = static_cast<double>(x) + 0.5 - instance.m_origin_x;
            const double local_y = static_cast<double>(y) + 0.5 - instance.m_origin_y;
            if (instance.m_reflected) {
                local_x = static_cast<double>(bounds.m_min_x) + bounds.m_max_x - local_x;
            }
            if (operation.m_shape == EGenerationShape::capsule) {
                return capsule(
                    local_x, local_y, operation.m_x, operation.m_y, operation.m_end_x, operation.m_end_y,
                    operation.m_radius_m * (1.0 / recipe.m_cell_size_m));
            }
            if (local_x < operation.m_x or
                local_y < operation.m_y or
                local_x >= operation.m_x + static_cast<double>(operation.m_width) or
                local_y >= operation.m_y + static_cast<double>(operation.m_height)) {
                return false;
            }
            if (operation.m_shape == EGenerationShape::rectangle) {
                return true;
            }
            const double nx = 2.0 * (local_x - operation.m_x) / operation.m_width - 1.0;
            const double ny = 2.0 * (local_y - operation.m_y) / operation.m_height - 1.0;
            return nx * nx + ny * ny <= 1.0;
        }
        [[nodiscard]] bool traversalRoom(const GenerationFeature &feature) noexcept {
            const bool sealed = std::ranges::any_of(feature.m_markers, [](const auto &marker) {
                return marker.m_role == EGenerationMarker::sealed_pocket;
            });
            return not sealed and std::ranges::any_of(feature.m_markers, [](const auto &marker) {
                return marker.m_role == EGenerationMarker::traversal_room;
            });
        }
        [[nodiscard]] double fieldValue(
            const GenerationPlan &plan, const u32 field, const double x, const double y) noexcept {
            return std::clamp(static_cast<double>(plan.fields()[field].sample(x, y)), -1.0, 1.0);
        }
        [[nodiscard]] u16 baseMaterial(
            const GenerationPlan &plan, const GenerationBiome &biome, const double depth, const double x,
            const double y) noexcept {
            u16 material = 0u;
            for (const auto &band: biome.m_bands) {
                if (not depthContains(depth, band.m_depth_min, band.m_depth_max)) {
                    continue;
                }
                const double draw = 0.5 * (fieldValue(plan, band.m_field, x, y) + 1.0);
                double cumulative = 0.0;
                material = band.m_choices.back().m_material;
                for (const auto &choice: band.m_choices) {
                    cumulative += choice.m_weight;
                    if (draw < cumulative / band.m_total_weight) {
                        material = choice.m_material;
                        break;
                    }
                }
                break;
            }
            PPR_ASSERT(material != 0u);
            for (const auto &deposit: biome.m_deposits) {
                if (material != deposit.m_host or not depthContains(depth, deposit.m_depth_min, deposit.m_depth_max)) {
                    continue;
                }
                const double value = fieldValue(plan, deposit.m_field, x, y);
                if (value >= deposit.m_threshold_min and value <= deposit.m_threshold_max) {
                    material = deposit.m_material;
                }
            }
            return material;
        }
        [[nodiscard]] Cell sample(
            const GenerationPlan &plan, const GenerationGeometry &geometry, const std::span<const u32> segments,
            const std::span<const u32> instances, const u32 x, const u32 y, const u32 biome_index,
            const bool lining) noexcept {
            const auto &recipe = plan.recipe();
            const double scale = 1.0 / recipe.m_cell_size_m;
            const double xm = (x + 0.5) / scale, ym = (y + 0.5) / scale;
            const double depth = 1.0 - (y + 0.5) / kWorldEdge;
            const auto &biome = recipe.m_biomes[biome_index];
            const float temperature = static_cast<float>(
                biome.m_temperature_base_k +
                biome.m_temperature_variation_k * fieldValue(plan, biome.m_temperature_field, xm, ym));
            if (x < recipe.m_boundary_fine or
                y < recipe.m_boundary_fine or
                x >= kWorldEdge - recipe.m_boundary_fine or
                y >= kWorldEdge - recipe.m_boundary_fine) {
                return {recipe.m_boundary_material, temperature};
            }
            const GenerationInstance *reservation = nullptr;
            for (const u32 i: instances) {
                if (reserved(recipe, geometry.m_instances[i], x, y)) {
                    reservation = &geometry.m_instances[i];
                    break;
                }
            }
            u16 material = lining and reservation == nullptr ? biome.m_lining_material
                                                             : baseMaterial(plan, biome, depth, xm, ym);
            bool portal = false;
            for (u32 r = 0u; r < recipe.m_rules.size(); ++r) {
                const auto &rule = recipe.m_rules[r];
                if (const auto *const cave = std::get_if<GenerationCave>(&rule.m_parameters)) {
                    if (lining or
                        reservation != nullptr or
                        cave->m_biome != biome_index or
                        not depthContains(depth, cave->m_depth_min, cave->m_depth_max)) {
                        continue;
                    }
                    const double value = fieldValue(plan, cave->m_field, xm, ym);
                    if (value >= cave->m_threshold_min and value <= cave->m_threshold_max) {
                        material = 0u;
                    }
                    continue;
                }
                const auto *const worm = std::get_if<GenerationWorm>(&rule.m_parameters);
                const bool connection = std::holds_alternative<GenerationConnection>(rule.m_parameters);
                if (worm == nullptr and not connection) {
                    continue;
                }
                if (worm != nullptr and (reservation != nullptr or
                                            worm->m_biome != biome_index or
                                            not depthContains(depth, worm->m_depth_min, worm->m_depth_max) or
                                            (not worm->m_carve and material != worm->m_host))) {
                    continue;
                }
                if (connection and
                    reservation != nullptr and
                    (reservation->m_rule == kGenerationStartRule or
                        not traversalRoom(recipe.m_features[reservation->m_feature]))) {
                    continue;
                }
                for (const u32 s: segments) {
                    const auto &segment = geometry.m_segments[s];
                    if (segment.m_rule != r or not capsule(
                                                   xm, ym, segment.m_x0_m, segment.m_y0_m, segment.m_x1_m,
                                                   segment.m_y1_m, segment.m_radius_m)) {
                        continue;
                    }
                    material = worm != nullptr and not worm->m_carve ? worm->m_material : 0u;
                    portal = portal or connection;
                    break;
                }
            }
            if (reservation != nullptr) {
                for (const auto &operation: recipe.m_features[reservation->m_feature].m_operations) {
                    if (not operationContains(recipe, *reservation, operation, x, y)) {
                        continue;
                    }
                    // A traversal-room marker authorizes its planned connection
                    // portals. Sealed pockets and the start reservation never do.
                    if (portal and not operation.m_carve) {
                        continue;
                    }
                    material = operation.m_carve ? 0u : operation.m_material;
                }
            }
            return {material, material == 0u ? 0.0f : temperature};
        }
        [[nodiscard]] std::error_code indexBounds(
            Array<Array<u32>> &bins, u32 &references, const u32 index, const double min_x, const double min_y,
            const double max_x, const double max_y) {
            if (max_x < 0.0 or max_y < 0.0 or min_x >= kWorldEdge or min_y >= kWorldEdge) {
                return {};
            }
            const u32 left = static_cast<u32>(
                                 std::clamp(std::floor(min_x), 0.0, static_cast<double>(kWorldEdge - 1u))) /
                             kChunkEdge;
            const u32 bottom = static_cast<u32>(
                                   std::clamp(std::floor(min_y), 0.0, static_cast<double>(kWorldEdge - 1u))) /
                               kChunkEdge;
            const u32 right = static_cast<u32>(
                                  std::clamp(std::floor(max_x), 0.0, static_cast<double>(kWorldEdge - 1u))) /
                              kChunkEdge;
            const u32 top = static_cast<u32>(std::clamp(std::floor(max_y), 0.0, static_cast<double>(kWorldEdge - 1u))) /
                            kChunkEdge;
            const u32 count = (right - left + 1u) * (top - bottom + 1u);
            if (count > kMaxGenerationRasterReferences - references) {
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, make_error_code(EGenerationErrc::raster_reference_limit));
            }
            references += count;
            for (u32 y = bottom; y <= top; ++y) {
                for (u32 x = left; x <= right; ++x) {
                    bins[y * kChunksPerEdge + x].push_back(index);
                }
            }
            return {};
        }
    }
    GenerationRaster::GenerationRaster(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry,
        Array<Array<u32>> segments, Array<Array<u32>> instances) noexcept
        : m_plan(plan), m_layout(layout), m_geometry(geometry), m_segments(std::move(segments)),
          m_instances(std::move(instances)) {
    }
    u64 GenerationRaster::indexPayloadBytes() const noexcept {
        u64 bytes = (m_segments.capacity() + m_instances.capacity()) * sizeof(Array<u32>);
        for (const auto &bucket: m_segments) {
            bytes += bucket.capacity() * sizeof(u32);
        }
        for (const auto &bucket: m_instances) {
            bytes += bucket.capacity() * sizeof(u32);
        }
        return bytes;
    }
    u64 GenerationRaster::chunkWorkingPayloadBytes() const noexcept {
        u32 padding = 0u;
        for (const auto &biome: m_plan.recipe().m_biomes) {
            padding = std::max(padding, biome.m_lining_thickness_fine);
        }
        const u64 edge = kChunkEdge + 2u * padding;
        return kCellsPerChunk * sizeof(Cell) + edge * edge * sizeof(u32);
    }
    Expected<Array<Cell>> GenerationRaster::chunk(const ChunkPos position, const IContext *const context) const {
        if (position.m_x >= kChunksPerEdge or position.m_y >= kChunksPerEdge) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::result_out_of_range);
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
        const auto &recipe = m_plan.recipe();
        u32 padding = 0u;
        for (const auto &biome: recipe.m_biomes) {
            padding = std::max(padding, biome.m_lining_thickness_fine);
        }
        const u32 edge = kChunkEdge + 2u * padding;
        Array<u32> biomes(edge * edge);
        for (u32 y = 0u; y < edge; ++y) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            for (u32 x = 0u; x < edge; ++x) {
                const double gx = std::clamp(
                    static_cast<double>(position.m_x * kChunkEdge + x) - padding + 0.5, 0.5, kWorldEdge - 0.5);
                const double gy = std::clamp(
                    static_cast<double>(position.m_y * kChunkEdge + y) - padding + 0.5, 0.5, kWorldEdge - 0.5);
                biomes[y * edge + x] = generationBiomeAt(
                    m_plan, m_layout, gx * recipe.m_cell_size_m, gy * recipe.m_cell_size_m);
            }
        }
        Array<Cell> cells(kCellsPerChunk);
        const u32 bucket = position.m_y * kChunksPerEdge + position.m_x;
        for (u32 y = 0u; y < kChunkEdge; ++y) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            for (u32 x = 0u; x < kChunkEdge; ++x) {
                const u32 at = (y + padding) * edge + x + padding;
                const u32 biome_index = biomes[at];
                const auto &biome = recipe.m_biomes[biome_index];
                bool lining = false;
                const i32 radius = static_cast<i32>(biome.m_lining_thickness_fine);
                for (i32 dy = -radius; dy <= radius and not lining; ++dy) {
                    const i32 horizontal = radius - std::abs(dy);
                    for (i32 dx = -horizontal; dx <= horizontal; ++dx) {
                        const i64 neighbor = static_cast<i64>(at) + dy * static_cast<i64>(edge) + dx;
                        if (biomes[static_cast<std::size_t>(neighbor)] != biome_index) {
                            lining = true;
                            break;
                        }
                    }
                }
                // Full bounded Manhattan distance, including diagonal neighbors.
                // The immutable biome halo makes province separators independent of
                // chunk scheduling. Threshold caves stop here; deliberate carves cross.
                cells[y * kChunkEdge + x] = sample(
                    m_plan, m_geometry, m_segments[bucket], m_instances[bucket], position.m_x * kChunkEdge + x,
                    position.m_y * kChunkEdge + y, biome_index, lining);
            }
        }
        return cells;
    }
    namespace {
        [[nodiscard]] std::error_code rasterChunks(
            const GenerationRaster &raster, ChunkGrid &grid, const GenerationExecution &execution,
            u64 *const known_scratch_peak) {
            std::atomic<u32> active{}, peak{};
            PPR_DEFER {
                if (known_scratch_peak != nullptr) {
                    *known_scratch_peak = std::max(
                        *known_scratch_peak,
                        raster.indexPayloadBytes() +
                            static_cast<u64>(peak.load(std::memory_order_relaxed)) * raster.chunkWorkingPayloadBytes());
                }
            };
            const auto fill = [&](const u32 c) -> std::error_code {
                // One requested chunk payload per live task. Include the short
                // handoff to grid ownership; this is a conservative task bound.
                if (known_scratch_peak != nullptr) {
                    const u32 count = active.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    auto previous = peak.load(std::memory_order_relaxed);
                    while (count > previous and
                           not peak.compare_exchange_weak(previous, count, std::memory_order_relaxed)) {
                    }
                }
                PPR_DEFER {
                    if (known_scratch_peak != nullptr) {
                        active.fetch_sub(1u, std::memory_order_relaxed);
                    }
                };
                const ChunkPos position{c % kChunksPerEdge, c / kChunksPerEdge};
                auto cells = raster.chunk(position, execution.m_context);
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, cells);
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, grid.setCells(position, std::move(*cells)));
                if (execution.m_completed_chunks != nullptr) {
                    execution.m_completed_chunks->fetch_add(1u, std::memory_order_relaxed);
                }
                return {};
            };
            ParallelScheduler *const pool = execution.m_pool;
            const bool parallel = pool != nullptr and not pool->isWorkerThread();
            constexpr u32 batch = 32u;
            for (u32 first = 0u; first < kChunkCount; first += batch) {
                std::array<std::future<std::error_code>, batch> pending{};
                u32 admitted = 0u;
                std::error_code error{};
                std::exception_ptr exception{}, fatal_allocation{};
                try {
                    for (u32 c = first; c < std::min(first + batch, kChunkCount); ++c) {
                        if (error = canceled(execution.m_context); error) {
                            break;
                        }
                        if (not parallel) {
                            error = fill(c);
                        } else {
                            auto task = pool->schedule(
                                [&, c](const IContext &context) -> std::error_code {
                                    PPR_RETURN_ERROR_ON_FAIL(WorldRaster, context.error());
                                    return fill(c);
                                },
                                std::nullopt, ParallelScheduler::drop_if_full);
                            if (task) {
                                pending[admitted++] = std::move(*task);
                            } else if (task.error() == make_error_code(RawChannel::error_full)) {
                                // Only the coordinator runs queue-pressure fallback.
                                error = fill(c);
                            } else {
                                error = task.error();
                            }
                        }
                        if (error) {
                            break;
                        }
                    }
                } catch (const std::bad_alloc &) {
                    fatal_allocation = std::current_exception();
                } catch (...) {
                    exception = std::current_exception();
                }
                // Borrowed inputs, raster bins and grid slots remain alive until
                // all accepted tasks complete, even after admission fails.
                for (u32 i = 0u; i < admitted; ++i) {
                    try {
                        const auto task_error = pending[i].get();
                        if (not error) {
                            error = task_error;
                        }
                    } catch (const std::bad_alloc &) {
                        fatal_allocation = std::current_exception();
                    } catch (...) {
                        if (not exception) {
                            exception = std::current_exception();
                        }
                    }
                }
                if (fatal_allocation) {
                    std::rethrow_exception(fatal_allocation);
                }
                if (exception) {
                    std::rethrow_exception(exception);
                }
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, error);
            }
            return {};
        }
    }
    Expected<ChunkGrid> rasterizeGenerationTerrain(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry,
        const GenerationExecution &execution, u64 *const known_scratch_peak) {
        try {
            auto raster = compileGenerationRaster(plan, layout, geometry, execution.m_context);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, raster);
            ChunkGrid grid;
            if (execution.m_stage != nullptr) {
                execution.m_stage->store(EGenerationStage::terrain, std::memory_order_release);
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, rasterChunks(*raster, grid, execution, known_scratch_peak));
            return grid;
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::system_error &error) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, error.code());
            // An exception carrying a success code still represents failure.
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::state_not_recoverable);
        } catch (...) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::state_not_recoverable);
        }
        std::unreachable();
    }
    Expected<GenerationRaster> compileGenerationRaster(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry,
        const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
        if (geometry.m_segments.size() > kMaxGenerationSegments) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, make_error_code(EGenerationErrc::segment_limit));
        }
        if (geometry.m_instances.size() > 1025u) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::value_too_large);
        }
        Array<Array<u32>> segments(kChunkCount), instances(kChunkCount);
        u32 references = 0u;
        const auto &recipe = plan.recipe();
        const double scale = 1.0 / recipe.m_cell_size_m;
        const double extent = kWorldEdge / scale;
        for (u32 s = 0u; s < geometry.m_segments.size(); ++s) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            const auto &segment = geometry.m_segments[s];
            if (segment.m_rule >= recipe.m_rules.size() or
                not std::isfinite(segment.m_x0_m) or
                not std::isfinite(segment.m_y0_m) or
                not std::isfinite(segment.m_x1_m) or
                not std::isfinite(segment.m_y1_m) or
                not std::isfinite(segment.m_radius_m) or
                segment.m_radius_m <= 0.0 or
                segment.m_x0_m < 0.0 or
                segment.m_y0_m < 0.0 or
                segment.m_x1_m < 0.0 or
                segment.m_y1_m < 0.0 or
                segment.m_x0_m > extent or
                segment.m_y0_m > extent or
                segment.m_x1_m > extent or
                segment.m_y1_m > extent or
                segment.m_radius_m > extent) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::invalid_argument);
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRaster,
                indexBounds(
                    segments, references, s, (std::min(segment.m_x0_m, segment.m_x1_m) - segment.m_radius_m) * scale,
                    (std::min(segment.m_y0_m, segment.m_y1_m) - segment.m_radius_m) * scale,
                    (std::max(segment.m_x0_m, segment.m_x1_m) + segment.m_radius_m) * scale,
                    (std::max(segment.m_y0_m, segment.m_y1_m) + segment.m_radius_m) * scale));
        }
        for (u32 i = 0u; i < geometry.m_instances.size(); ++i) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            const auto &instance = geometry.m_instances[i];
            if (instance.m_feature >= recipe.m_features.size()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::invalid_argument);
            }
            const auto &bounds = recipe.m_features[instance.m_feature].m_bounds;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRaster, indexBounds(
                                 instances, references, i, instance.m_origin_x + static_cast<double>(bounds.m_min_x),
                                 instance.m_origin_y + static_cast<double>(bounds.m_min_y),
                                 instance.m_origin_x + static_cast<double>(bounds.m_max_x) - 1.0,
                                 instance.m_origin_y + static_cast<double>(bounds.m_max_y) - 1.0));
        }
        return GenerationRaster{plan, layout, geometry, std::move(segments), std::move(instances)};
    }
}
