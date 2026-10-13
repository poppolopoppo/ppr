module;
#include "pP/Macros.h"
module game.colony.worldgen_preview;
import game.colony.worldgen;
import std;

namespace pP::colony {
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
            checksumInteger(checksum, 0x62696f73616d706cull, 8u);
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
                                                    result.m_material_counts.capacity() *
                                                        sizeof(sim::GenerationMaterialCount);
            return {};
        }
    }
    Expected<WorldgenPreviewResult> prepareWorldgenPreview(
        ContentCatalog catalog, WorldgenPreviewSpec spec, const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, context ? context->error() : std::error_code{});
        if (spec.m_origin.m_x > sim::kWorldEdge - kWorldgenPreviewEdge or
            spec.m_origin.m_y > sim::kWorldEdge - kWorldgenPreviewEdge) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, std::errc::result_out_of_range);
        }
        auto plan = compileWorldPlan(catalog, spec.m_profile, spec.m_seed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, plan);
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
        const sim::GenerationCellBounds visible{
            spec.m_origin.m_x, spec.m_origin.m_y, spec.m_origin.m_x + kWorldgenPreviewEdge,
            spec.m_origin.m_y + kWorldgenPreviewEdge};
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
        for (const auto &entry: result.m_plan.recipe().m_biomes) {
            result.m_filter_halo_fine = std::max(result.m_filter_halo_fine, entry.m_lining_thickness_fine);
        }
        u32 pore{}, relaxation{}, finish{}, erosion{};
        for (const auto &entry: result.m_plan.recipe().m_biomes) {
            pore = std::max(pore, entry.m_pore_passes);
            relaxation = std::max(relaxation, entry.m_relaxation_passes);
            finish = std::max(finish, entry.m_finish_passes);
            erosion = std::max(erosion, entry.m_erosion_passes);
        }
        result.m_filter_halo_fine += 1u + pore + relaxation + finish + erosion;
        const auto *const profile = result.m_catalog.findWorld(result.m_spec.m_profile);
        for (u32 i = 0u; i < result.m_plan.recipe().m_rules.size(); ++i) {
            const auto &rule = result.m_plan.recipe().m_rules[i];
            const auto *const cave = std::get_if<sim::GenerationCave>(&rule.m_parameters);
            const auto *const worm = std::get_if<sim::GenerationWorm>(&rule.m_parameters);
            if ((cave != nullptr and cave->m_biome == biome_index) or
                (worm != nullptr and worm->m_biome == biome_index)) {
                result.m_participating_rules.push_back(profile->m_rules[i].m_key);
            }
        }
        result.m_omissions = {
            "macro biome boundaries", "start reservation", "feature placements", "traversal connections",
            "initial matter", "backbone and playability validation"};
        PPR_RETURN_UNEXPECTED_ON_FAIL(BiomeSamplePreview, summarize(result, context));
        return result;
    }
}
