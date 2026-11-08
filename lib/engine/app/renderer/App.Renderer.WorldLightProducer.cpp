module;
#include "pP/Macros.h"

module engine.app;

import :renderer.world_light_producer;

import engine.core;
import std;

namespace pP::lighting {
    namespace {
        // Metre-to-native-cell conversion, clamped to the world edge. A particle
        // just outside the world clamps to the boundary rather than being dropped.
        [[nodiscard]] u32 clampCell_(const double coord_m, const double cell_size_m, const u32 edge) noexcept {
            if (cell_size_m <= 0.0) {
                return 0u;
            }
            const double cell = std::floor(coord_m / cell_size_m);
            if (cell < 0.0) {
                return 0u;
            }
            if (cell >= static_cast<double>(edge)) {
                return edge - 1u;
            }
            return static_cast<u32>(cell);
        }

        // The staged chunk id rides the sim's chunkIndexOf domain: row-major over
        // the maximum 4096-edge stride (kChunksPerEdge == 32), even for smaller
        // worlds. The dense world light source addresses chunks over
        // world_edge/128 per edge, so decode the max-stride id and re-encode.
        [[nodiscard]] u32 denseChunkId_(const u32 stage_chunk_id, const u32 world_edge) noexcept {
            constexpr u32 kChunkIdStride = 32u;
            constexpr u32 kChunkEdge = 128u;
            const u32 chunk_x = stage_chunk_id % kChunkIdStride;
            const u32 chunk_y = stage_chunk_id / kChunkIdStride;
            const u32 chunks_per_edge = world_edge / kChunkEdge;
            return chunk_y * chunks_per_edge + chunk_x;
        }

        // Simple finite hour->sky ramp (no analytic sun). A negative hour disables
        // the sky (black terminal contribution). Solar hours [0, 24) map through a
        // day arc: a dim blue night, a warm tone near sunrise/sunset and a bright
        // blue day. Every channel stays finite for every admitted hour.
        [[nodiscard]] std::array<float, 3> skyColour_(const i32 hour) noexcept {
            if (hour < 0) {
                return {0.0f, 0.0f, 0.0f};
            }

            // Normalize into [0, 24) so out-of-range hours wrap instead of driving
            // a non-finite solar elevation.
            const float h = std::fmod(std::fmod(static_cast<float>(hour), 24.0f) + 24.0f, 24.0f);

            // Solar elevation proxy: +1 at noon, -1 at midnight.
            const float elevation = std::cos((h - 12.0f) * 6.28318530718f / 24.0f);
            const float day = std::clamp(elevation * 1.25f, 0.0f, 1.0f);

            // Warm horizon term peaked at sunrise (6) and sunset (18).
            const float horizon = std::exp(-0.5f * std::pow((h - 6.0f) / 2.5f, 2.0f)) +
                                  std::exp(-0.5f * std::pow((h - 18.0f) / 2.5f, 2.0f));

            return {
                0.02f + 0.35f * day + 0.35f * horizon,
                0.03f + 0.55f * day + 0.10f * horizon,
                0.06f + 0.90f * day + 0.05f * horizon,
            };
        }
    } // namespace

    WorldLightSourceUpdateStorage buildWorldLightSourceUpdate(
        const GridStagedFrame &stage, const u32 world_edge, const u64 revision, const i32 sky_hour) {
        WorldLightSourceUpdateStorage storage{};

        // Dirty chunks only: one WorldLightChunkUpdate per staged upload, copying
        // the cell ids so the update owns its storage. A full-refresh stage still
        // carries only the uploaded chunks, never the whole resident set.
        storage.m_chunk_cell_ids.reserve(stage.m_uploads.size());
        storage.m_chunks.reserve(stage.m_uploads.size());
        for (const GridStagedUpload &upload: stage.m_uploads) {
            storage.m_chunk_cell_ids.emplace_back(upload.m_cell_materials.begin(), upload.m_cell_materials.end());
            storage.m_chunks.push_back(
                WorldLightChunkUpdate{
                    .m_chunk_id = denseChunkId_(upload.m_chunk_id, world_edge),
                    .m_cell_ids = storage.m_chunk_cell_ids.back(),
                });
        }

        // Committed particle instances scatter into the dense overlay at their
        // native cell. The cell id is 1-based (0 = vacuum); the Matter particle
        // palette id is 0-based (index into the dense palette), so the game
        // material id shifts down by one. Vacuum particles contribute nothing.
        storage.m_particles.reserve(stage.m_particle_batches.size() * 4u);
        for (const GridParticleBatch &batch: stage.m_particle_batches) {
            if (not batch.m_particles) {
                continue;
            }
            for (const GridParticleInstance &particle: *batch.m_particles) {
                if (particle.m_material_id == 0u) {
                    continue;
                }
                const u32 x = clampCell_(particle.m_position_x_m, double{stage.m_cell_size_m}, world_edge);
                const u32 y = clampCell_(particle.m_position_y_m, double{stage.m_cell_size_m}, world_edge);
                storage.m_particles.push_back(
                    WorldLightMatterParticle{
                        .m_cell = y * world_edge + x,
                        .m_fill = 1.0f,
                        .m_material = particle.m_material_id - 1u,
                    });
            }
        }

        storage.m_update = WorldLightSourceUpdate{
            .m_chunks = storage.m_chunks,
            .m_particles = storage.m_particles,
            .m_revision = revision,
            .m_sky_hour = sky_hour,
            .m_sky = skyColour_(sky_hour),
        };
        return storage;
    }
} // namespace pP::lighting
