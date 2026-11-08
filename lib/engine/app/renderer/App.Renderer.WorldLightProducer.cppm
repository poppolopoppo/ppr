module;

export module engine.app:renderer.world_light_producer;

import :renderer.world_light_source;
import :renderer.grid_pass;

import engine.core;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // world light source update production
    // ------------------------------------------------------------------

    // Owns the storage backing a WorldLightSourceUpdate built from a committed
    // GridStagedFrame, so the update's spans stay valid without borrowing the
    // (possibly shorter-lived) stage. The producer copies only the dirty chunk
    // uploads and the committed particle instances; it never reconstructs the
    // whole world. A full-refresh stage still contributes only the uploaded
    // chunks, so an ordinary edit stages exactly the dirty set.
    struct WorldLightSourceUpdateStorage {
        std::vector<WorldLightChunkUpdate> m_chunks{};
        std::vector<std::vector<u16>> m_chunk_cell_ids{};
        std::vector<WorldLightMatterParticle> m_particles{};
        WorldLightSourceUpdate m_update{};
    };

    // Builds a source update from the committed stage. m_chunks come only from
    // stage.m_uploads (the dirty chunks); committed particle instances become
    // WorldLightMatterParticle entries with metre-to-native-cell conversion
    // driven by stage.m_cell_size_m and world_edge. The cell ids carried by each
    // chunk are the stage's cell material ids copied verbatim; remapping those to
    // the dense source palette is the owning catalog's concern. revision and sky
    // hour are carried verbatim. An empty stage yields an empty update.
    [[nodiscard]] WorldLightSourceUpdateStorage buildWorldLightSourceUpdate(
        const GridStagedFrame &stage, u32 world_edge, u64 revision, i32 sky_hour);
} // namespace pP::lighting
