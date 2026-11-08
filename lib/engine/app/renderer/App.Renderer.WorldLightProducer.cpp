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
                    .m_chunk_id = upload.m_chunk_id,
                    .m_cell_ids = storage.m_chunk_cell_ids.back(),
                });
        }

        // Committed particle instances scatter into the dense overlay at their
        // native cell. The fill is a per-cell presence (1.0); palette remapping is
        // the owning catalog's concern, so the material id travels verbatim.
        storage.m_particles.reserve(stage.m_particle_batches.size() * 4u);
        for (const GridParticleBatch &batch: stage.m_particle_batches) {
            if (not batch.m_particles) {
                continue;
            }
            for (const GridParticleInstance &particle: *batch.m_particles) {
                const u32 x = clampCell_(particle.m_position_x_m, double{stage.m_cell_size_m}, world_edge);
                const u32 y = clampCell_(particle.m_position_y_m, double{stage.m_cell_size_m}, world_edge);
                storage.m_particles.push_back(
                    WorldLightMatterParticle{
                        .m_cell = y * world_edge + x,
                        .m_fill = 1.0f,
                        .m_material = particle.m_material_id,
                    });
            }
        }

        storage.m_update = WorldLightSourceUpdate{
            .m_chunks = storage.m_chunks,
            .m_particles = storage.m_particles,
            .m_revision = revision,
            .m_sky_hour = sky_hour,
        };
        return storage;
    }
} // namespace pP::lighting
