module;
#include "pP/Macros.h"
module game.colony.worldgen_preview;
import game.colony.worldgen;
import std;

namespace pP::colony {
    // ------------------------------------------------------------------
    // worldgen preview
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(BiomeSamplePreview, info, none);
    namespace {
        sim::GenerationCellBounds expand(const sim::GenerationCellBounds bounds, const u32 padding) noexcept {
            return {
                bounds.m_left > padding ? bounds.m_left - padding : 0u,
                bounds.m_bottom > padding ? bounds.m_bottom - padding : 0u,
                std::min(sim::kWorldEdge, bounds.m_right + padding), std::min(sim::kWorldEdge, bounds.m_top + padding)};
        }
        sim::GenerationCellBounds align(const sim::GenerationCellBounds bounds, const u32 edge) noexcept {
            return {
                bounds.m_left / edge * edge, bounds.m_bottom / edge * edge,
                std::min(sim::kWorldEdge, (bounds.m_right + edge - 1u) / edge * edge),
                std::min(sim::kWorldEdge, (bounds.m_top + edge - 1u) / edge * edge)};
        }
        u64 geometryPayload(const sim::GenerationGeometry &geometry) noexcept {
            return geometry.m_instances.capacity() * sizeof(sim::GenerationInstance) +
                   geometry.m_segments.capacity() * sizeof(sim::GenerationSegment) +
                   geometry.m_blobs.capacity() * sizeof(sim::GenerationGeometry::Blob) +
                   geometry.m_omitted_instances.capacity() * sizeof(u32);
        }
        u64 matterPayload(const sim::InitialMatter &matter) noexcept {
            u64 bytes = matter.m_regions.capacity() * sizeof(sim::InitialMatterRegion);

            for (const auto &region: matter.m_regions) {
                bytes += region.m_spans.capacity() * sizeof(sim::InitialMatterSpan);
            }
            return bytes;
        }
        u64 particlePayload(const sim::MatterParticleSnapshot &particles) noexcept {
            u64 bytes = particles.m_chunks.capacity() * sizeof(sim::MatterParticleBatch);

            for (const auto &chunk: particles.m_chunks) {
                if (chunk.m_particles) {
                    bytes += chunk.m_particles->capacity() * sizeof(sim::MatterParticle);
                }
            }
            return bytes;
        }
        u32 filterHalo(const sim::GenerationPlan &plan) noexcept {
            u32 lining{}, pore{}, relaxation{}, finish{}, erosion{};

            for (const auto &biome: plan.recipe().m_biomes) {
                lining = std::max(lining, biome.m_lining_thickness_fine);
                pore = std::max(pore, biome.m_pore_passes);
                relaxation = std::max(relaxation, biome.m_relaxation_passes);
                finish = std::max(finish, biome.m_finish_passes);
                erosion = std::max(erosion, biome.m_erosion_passes);
            }
            return lining + 1u + pore + relaxation + finish + erosion;
        }
        Expected<sim::InitialMatter> cropMatter(
            const WorldgenPreviewWorld &prepared, const sim::GenerationCellBounds visible,
            const IContext *const context, u64 &known_scratch_peak) {
            const auto &world = prepared.m_world;
            Array<Array<sim::InitialMatterSpan>> spans(world.m_matter.m_regions.size());

            for (u32 cy = visible.m_bottom / sim::kChunkEdge; cy <= (visible.m_top - 1u) / sim::kChunkEdge; ++cy) {
                for (u32 cx = visible.m_left / sim::kChunkEdge; cx <= (visible.m_right - 1u) / sim::kChunkEdge; ++cx) {
                    for (const auto &entry: prepared.m_matter_chunks[sim::chunkIndexOf({cx, cy})]) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            BiomeSamplePreview, context ? context->error() : std::error_code{});
                        const auto span = entry.m_span;
                        if (span.m_y >= visible.m_bottom and span.m_y < visible.m_top) {
                            spans[entry.m_region].push_back(
                                {span.m_y, std::max(span.m_begin_x, visible.m_left),
                                    std::min(span.m_end_x, visible.m_right)});
                        }
                    }
                }
            }

            u64 scratch = spans.capacity() * sizeof(Array<sim::InitialMatterSpan>);
            for (const auto &region: spans) {
                scratch += region.capacity() * sizeof(sim::InitialMatterSpan);
            }

            known_scratch_peak = std::max(known_scratch_peak, scratch);

            sim::InitialMatter result;
            for (std::size_t region_index = 0u; region_index < spans.size(); ++region_index) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, context ? context->error() : std::error_code{});
                auto &clipped = spans[region_index];
                if (clipped.empty()) {
                    continue;
                }
                std::ranges::sort(clipped, [](const auto a, const auto b) {
                    return std::tie(a.m_y, a.m_begin_x) < std::tie(b.m_y, b.m_begin_x);
                });
                const auto &source = world.m_matter.m_regions[region_index];
                sim::InitialMatterRegion region{
                    .m_identity = source.m_identity,
                    .m_material = source.m_material,
                    .m_temperature_k = source.m_temperature_k,
                    .m_bulk_density_kg_m3 = source.m_bulk_density_kg_m3,
                    .m_fill_height_m = source.m_fill_height_m};
                for (const auto span: clipped) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, context ? context->error() : std::error_code{});
                    const u32 begin = span.m_begin_x, end = span.m_end_x;
                    if (begin >= end) {
                        continue;
                    }
                    if (not region.m_spans.empty() and
                        region.m_spans.back().m_y == span.m_y and
                        region.m_spans.back().m_end_x == begin) {
                        region.m_spans.back().m_end_x = end;
                    } else {
                        region.m_spans.push_back({span.m_y, begin, end});
                        ++result.m_span_count;
                    }
                    for (u32 x = begin; x < end; ++x) {
                        const auto area = sim::generationEmptyArea(
                            world.m_grid, {x, span.m_y}, world.m_plan.recipe().m_cell_size_m);
                        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, area);
                        region.m_volume_m3 += *area * world.m_plan.recipe().m_reference_thickness_m;
                    }
                }
                if (not region.m_spans.empty()) {
                    region.m_mass_kg = region.m_volume_m3 * region.m_bulk_density_kg_m3;
                    result.m_regions.push_back(std::move(region));
                }
            }
            return result;
        }
        void checksumInteger(u64 &checksum, const u64 value, const u32 bytes) noexcept {
            for (u32 byte = 0u; byte < bytes; ++byte) {
                checksum ^= static_cast<u8>(value >> (8u * byte));
                checksum *= 1099511628211ull;
            }
        }
        std::error_code summarize(WorldgenPreviewResult &result, const IContext *const context) {
            Array<u64> counts(65536u, 0u);
            result.m_known_scratch_peak_bytes = std::max(
                result.m_known_scratch_peak_bytes, counts.capacity() * sizeof(u64));

            u64 checksum = 14695981039346656037ull;
            checksumInteger(
                checksum,
                result.m_spec.m_mode == EWorldgenPreviewMode::biome_sample ? 0x62696f73616d706cull
                                                                           : 0x776f726c64637270ull,
                8u);
            checksumInteger(checksum, sim::kGeneratorAlgorithmVersion, 4u);
            checksumInteger(checksum, result.m_visible.m_left, 4u);
            checksumInteger(checksum, result.m_visible.m_bottom, 4u);

            for (u32 y = result.m_visible.m_bottom; y < result.m_visible.m_top; ++y) {
                PPR_RETURN_ERROR_ON_FAIL(BiomeSamplePreview, context ? context->error() : std::error_code{});
                for (u32 x = result.m_visible.m_left; x < result.m_visible.m_right; ++x) {
                    const auto cell = result.m_grid.getCell({x, y});
                    PPR_RETURN_ERROR_ON_FAIL(BiomeSamplePreview, cell);
                    const auto *const material = result.m_catalog.findMaterial(cell->m_element);
                    if (not std::isfinite(cell->m_temperature) or
                        (cell->m_element != 0u and
                            (material == nullptr or material->m_representation != EMaterialRepresentation::terrain))) {
                        PPR_RETURN_ERROR_ON_FAIL(BiomeSamplePreview, std::errc::invalid_argument);
                    }
                    ++counts[cell->m_element];
                    result.m_solid_cells += cell->m_element != 0u ? 1u : 0u;
                    checksumInteger(checksum, cell->m_element, 2u);
                    checksumInteger(checksum, std::bit_cast<u32>(cell->m_temperature), 4u);
                }
            }
            result.m_checksum = checksum;

            for (u32 material = 0u; material < counts.size(); ++material) {
                if (counts[material] != 0u) {
                    result.m_material_counts.push_back({static_cast<u16>(material), counts[material]});
                }
            }

            result.m_known_retained_payload_bytes = result.m_plan.retainedPayloadBytes() +
                                                    result.m_layout.retainedPayloadBytes() +
                                                    geometryPayload(result.m_geometry) +
                                                    result.m_grid.retainedPayloadBytes() +
                                                    matterPayload(result.m_matter) +
                                                    result.m_particles.m_chunks.capacity() *
                                                        sizeof(sim::MatterParticleBatch) +
                                                    result.m_material_counts.capacity() *
                                                        sizeof(sim::GenerationMaterialCount);
            return {};
        }
    }

    // ------------------------------------------------------------------
    // preview result geometry
    // ------------------------------------------------------------------

    const sim::GenerationGeometry &WorldgenPreviewResult::geometry() const noexcept {
        return m_exact_world ? m_exact_world->m_world.m_geometry : m_geometry;
    }

    // ------------------------------------------------------------------
    // preview world matching
    // ------------------------------------------------------------------

    bool WorldgenPreviewWorld::matches(const ContentCatalog &catalog, const WorldgenPreviewSpec &spec) const noexcept {
        return m_profile == spec.m_profile and
               m_world.m_plan.seed() == spec.m_seed and
               catalog.contentFingerprint() == std::optional<u64>{m_content_fingerprint};
    }

    // ------------------------------------------------------------------
    // preview world preparation
    // ------------------------------------------------------------------

    Expected<std::shared_ptr<const WorldgenPreviewWorld>> prepareWorldgenPreviewWorld(
        const ContentCatalog &catalog, const WorldgenPreviewSpec &spec, const ITimerClock &clock,
        const IContext *const context, ParallelScheduler *const generation_pool) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, context ? context->error() : std::error_code{});
        const auto fingerprint = catalog.contentFingerprint();
        if (not fingerprint) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, std::errc::invalid_argument);
        }

        auto plan = compileWorldPlan(catalog, spec.m_profile, spec.m_seed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, plan);

        auto world = sim::generateWorld(std::move(*plan), clock, {.m_pool = generation_pool, .m_context = context});
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, world);

        auto particles = sim::seedMatterParticles(world->m_matter, world->m_grid, world->m_plan.recipe(), context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, particles);
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, context ? context->error() : std::error_code{});

        auto prepared = std::make_shared<WorldgenPreviewWorld>(
            WorldgenPreviewWorld{spec.m_profile, *fingerprint, std::move(*world), std::move(*particles)});

        for (u32 region = 0u; region < prepared->m_world.m_matter.m_regions.size(); ++region) {
            for (const auto span: prepared->m_world.m_matter.m_regions[region].m_spans) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, context ? context->error() : std::error_code{});
                for (u32 x = span.m_begin_x; x < span.m_end_x;) {
                    const u32 end = std::min(span.m_end_x, (x / sim::kChunkEdge + 1u) * sim::kChunkEdge);
                    prepared->m_matter_chunks[sim::chunkIndexOf(sim::chunkOf({x, span.m_y}))].push_back(
                        {region, {span.m_y, x, end}});
                    x = end;
                }
            }
        }

        prepared->m_known_retained_payload_bytes = prepared->m_world.m_report.m_retained_known_payload_bytes +
                                                   particlePayload(prepared->m_particles) +
                                                   sizeof(prepared->m_matter_chunks);
        for (const auto &chunk: prepared->m_matter_chunks) {
            prepared->m_known_retained_payload_bytes += chunk.capacity() * sizeof(WorldgenPreviewWorld::MatterSpan);
        }

        prepared->m_known_preparation_scratch_peak_bytes = *std::ranges::max_element(
            prepared->m_world.m_report.m_known_phase_scratch_bytes);
        // The particle API does not expose its transient validation capacities;
        // those allocations are explicitly excluded from the reported scratch.
        return std::shared_ptr<const WorldgenPreviewWorld>{std::move(prepared)};
    }

    // ------------------------------------------------------------------
    // preview preparation
    // ------------------------------------------------------------------

    Expected<WorldgenPreviewResult> prepareWorldgenPreview(
        ContentCatalog catalog, WorldgenPreviewSpec spec, const IContext *const context,
        std::shared_ptr<const WorldgenPreviewWorld> world) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, context ? context->error() : std::error_code{});
        if (spec.m_origin.m_x > sim::kWorldEdge - kWorldgenPreviewEdge or
            spec.m_origin.m_y > sim::kWorldEdge - kWorldgenPreviewEdge) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, std::errc::result_out_of_range);
        }

        auto plan = compileWorldPlan(catalog, spec.m_profile, spec.m_seed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, plan);

        const sim::GenerationCellBounds visible{
            spec.m_origin.m_x, spec.m_origin.m_y, spec.m_origin.m_x + kWorldgenPreviewEdge,
            spec.m_origin.m_y + kWorldgenPreviewEdge};

        if (spec.m_mode == EWorldgenPreviewMode::whole_world_crop) {
            if (not world or not world->matches(catalog, spec)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, std::errc::invalid_argument);
            }

            auto layout = sim::restoreBiomeLayout(*plan, world->m_world.m_layout.sites());
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, layout);

            const auto support = align(expand(visible, 1u), sim::kChunkEdge);
            sim::ChunkGrid grid;

            for (u32 cy = support.m_bottom / sim::kChunkEdge; cy < support.m_top / sim::kChunkEdge; ++cy) {
                for (u32 cx = support.m_left / sim::kChunkEdge; cx < support.m_right / sim::kChunkEdge; ++cx) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, context ? context->error() : std::error_code{});
                    const auto cells = world->m_world.m_grid.residentCells({cx, cy});
                    if (not cells) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, std::errc::invalid_argument);
                    }
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        BiomeSamplePreview, grid.setCells({cx, cy}, Array<sim::Cell>{cells->begin(), cells->end()}));
                }
            }

            u64 scratch{};
            auto matter = cropMatter(*world, visible, context, scratch);
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, matter);

            const u32 biome_index = sim::generationBiomeAt(
                *plan, *layout, (visible.m_left + 0.5) * plan->recipe().m_cell_size_m,
                (visible.m_bottom + 0.5) * plan->recipe().m_cell_size_m);

            WorldgenPreviewResult result{
                std::move(catalog), std::move(spec), std::move(*plan), std::move(*layout), {}, std::move(grid), visible,
                support, {16u, 16u, sim::kWorldEdge - 16u, sim::kWorldEdge - 16u}, biome_index};
            result.m_matter = std::move(*matter);
            result.m_known_world_cache_payload_bytes = world->m_known_retained_payload_bytes;
            result.m_exact_world = world;
            result.m_particles.m_next_id = world->m_particles.m_next_id;

            for (const auto &batch: world->m_particles.m_chunks) {
                const u32 x = batch.m_position.m_x * sim::kChunkEdge, y = batch.m_position.m_y * sim::kChunkEdge;
                if (x >= support.m_left and x < support.m_right and y >= support.m_bottom and y < support.m_top) {
                    result.m_particles.m_chunks.push_back(batch);
                }
            }

            result.m_filter_halo_fine = filterHalo(result.m_plan);
            result.m_known_scratch_peak_bytes = std::max(scratch, world->m_known_preparation_scratch_peak_bytes);

            for (const auto &key: result.m_catalog.findWorld(result.m_spec.m_profile)->m_rule_keys) {
                result.m_participating_rules.push_back(key);
            }

            result.m_omissions = {"simulation actors and runtime state"};
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, summarize(result, context));
            return result;
        }

        if (spec.m_mode != EWorldgenPreviewMode::biome_sample) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, std::errc::invalid_argument);
        }

        const auto biome = std::ranges::find(catalog.biomes(), spec.m_biome, &BiomeDefinition::m_key);
        if (biome == catalog.biomes().end()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, std::errc::invalid_argument);
        }

        const u32 biome_index = static_cast<u32>(biome - catalog.biomes().begin());

        auto layout = sim::compileBiomeSampleLayout(*plan, biome_index, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, layout);

        u64 scratch{};
        auto geometry = sim::compileBiomeSampleGeometry(*plan, *layout, context, &scratch);
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, geometry);

        // Numeric recipes cap formations at eight metres: 24 fine cells at 3:1.
        // Candidate roots include every footprint reaching the interior. A second
        // expansion supplies complete support and footprint validation for those roots.
        u32 formation_reach = 2u;
        for (const auto &entry: plan->recipe().m_biomes) {
            formation_reach = std::max(
                formation_reach,
                static_cast<u32>(std::ceil(entry.m_formation_length_m / plan->recipe().m_cell_size_m)));
        }
        formation_reach += 4u;

        const auto domain = align(expand(visible, formation_reach), 16u);
        const auto support = align(expand(domain, formation_reach + 1u), sim::kChunkEdge);
        const u32 chunks = (support.m_right - support.m_left) /
                           sim::kChunkEdge *
                           ((support.m_top - support.m_bottom) / sim::kChunkEdge);
        if (chunks > kWorldgenPreviewMaxResidentChunks) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, std::errc::value_too_large);
        }

        sim::ChunkGrid grid;
        {
            auto raster = sim::compileGenerationRaster(*plan, *layout, *geometry, context);
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, raster);
            scratch = std::max(scratch, raster->indexPayloadBytes() + raster->chunkWorkingPayloadBytes());
            for (u32 cy = support.m_bottom / sim::kChunkEdge; cy < support.m_top / sim::kChunkEdge; ++cy) {
                for (u32 cx = support.m_left / sim::kChunkEdge; cx < support.m_right / sim::kChunkEdge; ++cx) {
                    auto cells = raster->chunk({cx, cy}, context);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, cells);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, grid.setCells({cx, cy}, std::move(*cells)));
                }
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            BiomeSamplePreview,
            sim::finishGenerationTerrainRegion(*plan, *layout, *geometry, grid, domain, context, &scratch));

        WorldgenPreviewResult result{
            std::move(catalog), std::move(spec), std::move(*plan), std::move(*layout), std::move(*geometry),
            std::move(grid), visible, support, domain, biome_index};
        result.m_known_scratch_peak_bytes = scratch;
        result.m_filter_halo_fine = filterHalo(result.m_plan);
        const auto *const profile = result.m_catalog.findWorld(result.m_spec.m_profile);
        for (u32 i = 0u; i < result.m_plan.recipe().m_rules.size(); ++i) {
            const auto &rule = result.m_plan.recipe().m_rules[i];
            const auto *const cave = std::get_if<sim::GenerationCave>(&rule.m_parameters);
            const auto *const worm = std::get_if<sim::GenerationWorm>(&rule.m_parameters);
            if ((cave != nullptr and cave->m_biome == biome_index) or
                (worm != nullptr and worm->m_biome == biome_index)) {
                result.m_participating_rules.push_back(profile->m_rule_keys[i]);
            }
        }

        result.m_omissions = {
            "macro biome boundaries", "start reservation", "feature placements", "traversal connections",
            "initial matter", "backbone and playability validation"};
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, summarize(result, context));
        return result;
    }
}
