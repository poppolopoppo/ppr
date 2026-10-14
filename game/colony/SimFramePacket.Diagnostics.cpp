module;
#include "pP/Macros.h"

module game.colony.sim_packet;
import engine.core;
import engine.app;
import engine.sim;
import game.colony.driver;
import game.colony.worldgen;
import game.colony.content;
import game.colony.metric;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(WorldPreview, info, none)

    // ------------------------------------------------------------------
    // diagnostic presentation
    // ------------------------------------------------------------------

    Expected<GridPass::GridStagedFrame> captureDiagnosticPresentation(
        const ColonyDriver &driver, const sim::GeneratedWorld &generated, const EGridDiagnosticMode mode,
        const u64 revision, const float temperature_min_k, const float temperature_max_k, const IContext &context) {
        const PreparedWorld *const prepared = driver.preparedWorld();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreview, prepared != nullptr and revision != 0u and mode <= EGridDiagnosticMode::initial_matter
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreview, mode != EGridDiagnosticMode::temperature or (std::isfinite(temperature_min_k) and
                                                                          std::isfinite(temperature_max_k) and
                                                                          temperature_min_k >= 0.0f and
                                                                          temperature_max_k > temperature_min_k)
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));

        auto stage = captureWorldPresentation(driver, &context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreview, stage);

        stage->m_diagnostic_mode = mode;
        stage->m_diagnostic_revision = revision;
        stage->m_initial_matter_valid = true;

        if (mode == EGridDiagnosticMode::textured) {
            return stage;
        }

        const ContentCatalog &catalog = *driver.contentCatalog();
        if (mode == EGridDiagnosticMode::material) {
            stage->m_diagnostic_palette.resize(65536u);
            for (const MaterialDefinition &material: catalog.materials()) {
                if (material.m_representation == EMaterialRepresentation::terrain) {
                    stage->m_diagnostic_palette[material.m_numeric_id].m_color_rgba = material.m_color_rgba;
                }
            }
            return stage;
        }
        stage->m_diagnostic_palette.push_back({});
        if (mode == EGridDiagnosticMode::biome) {
            for (const auto &biome: catalog.biomes()) {
                stage->m_diagnostic_palette.push_back({biome.m_preview_color_rgba});
            }
        } else if (mode == EGridDiagnosticMode::temperature) {
            stage->m_diagnostic_palette[0].m_color_rgba = 0x202020ffu;
            for (u32 bin = 0u; bin < 256u; ++bin) {
                const u32 red = bin, blue = 255u - bin;
                const u32 green = 255u - static_cast<u32>(std::abs(2 * static_cast<i32>(bin) - 255));
                stage->m_diagnostic_palette.push_back({(red << 24u) | (green << 16u) | (blue << 8u) | 255u});
            }
        } else {
            stage->m_diagnostic_palette.resize(65536u);
            for (const auto &material: catalog.materials()) {
                if (material.m_representation == EMaterialRepresentation::particle) {
                    stage->m_diagnostic_palette[material.m_numeric_id].m_color_rgba = material.m_color_rgba;
                }
            }
        }

        std::array<GridPass::GridStagedUpload *, sim::kChunkCount> chunks{};
        for (auto &upload: stage->m_uploads) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreview, context.error());
            upload.m_diagnostic_indices.resize(sim::kCellsPerChunk, 0u);
            chunks[upload.m_chunk_id] = &upload;
            if (mode == EGridDiagnosticMode::initial_matter) {
                continue;
            }
            const sim::ChunkPos chunk{upload.m_chunk_id % sim::kChunksPerEdge, upload.m_chunk_id / sim::kChunksPerEdge};
            const auto cells = driver.grid().residentCells(chunk);
            PPR_ASSERT(cells.has_value());
            for (u32 row = 0u; row < sim::kChunkEdge; ++row) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreview, context.error());
                for (u32 col = 0u; col < sim::kChunkEdge; ++col) {
                    const u32 local = row * sim::kChunkEdge + col;
                    if (mode == EGridDiagnosticMode::biome) {
                        const double x = (chunk.m_x * sim::kChunkEdge + col + 0.5) / driver.cellsPerMetre();
                        const double y = (chunk.m_y * sim::kChunkEdge + row + 0.5) / driver.cellsPerMetre();
                        const auto biome = sim::generationBiomeAt(generated.m_plan, generated.m_layout, x, y);
                        PPR_ASSERT(biome < catalog.biomes().size());
                        upload.m_diagnostic_indices[local] = static_cast<u16>(biome + 1u);
                    } else if ((*cells)[local].m_element != 0u) {
                        const double unit = std::clamp(
                            (static_cast<double>((*cells)[local].m_temperature) - temperature_min_k) /
                                (static_cast<double>(temperature_max_k) - temperature_min_k),
                            0.0, 1.0);
                        upload.m_diagnostic_indices[local] = static_cast<u16>(
                            std::min(255u, static_cast<u32>(unit * 256.0)) + 1u);
                    }
                }
            }
        }

        if (mode == EGridDiagnosticMode::initial_matter) {
            const auto particles = driver.captureParticles();
            for (const auto &batch: particles.m_chunks) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreview, context.error());
                const u32 chunk = sim::chunkIndexOf(batch.m_position);
                if (chunks[chunk] == nullptr) {
                    continue;
                }
                for (const auto &particle: batch.records()) {
                    const u32 x = static_cast<u32>(particle.m_x_m * driver.cellsPerMetre());
                    const u32 y = static_cast<u32>(particle.m_y_m * driver.cellsPerMetre());
                    const u32 local = (y % sim::kChunkEdge) * sim::kChunkEdge + x % sim::kChunkEdge;
                    chunks[chunk]->m_diagnostic_indices[local] = particle.m_material;
                }
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreview, context.error());
        return stage;
    }
}
