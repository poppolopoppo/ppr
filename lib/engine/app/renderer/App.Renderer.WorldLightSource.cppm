module;

export module engine.app:renderer.world_light_source;

import :renderer.world_light_types;

import engine.core;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // dense world source input types
    // ------------------------------------------------------------------

    // One palette material, indexed by cell id: id 0 is the vacuum/empty cell and
    // ids 1..N address m_materials[id - 1]. Optical terms live only in this global
    // palette; they are never duplicated per chunk or per cell.
    struct WorldLightMaterial {
        u32 m_kind{};
        std::array<float, 3> m_source{};
        std::array<float, 3> m_sigma{};
        std::array<float, 3> m_emission{};
        std::array<float, 3> m_albedo{};
    };

    // One committed dirty 128x128 chunk: a row-major chunk id and the 128*128
    // native cell ids (row-major) to copy at that chunk's offset.
    struct WorldLightChunkUpdate {
        u32 m_chunk_id{};
        std::span<const u16> m_cell_ids{};
    };

    // One dense Matter overlay cell, addressed by m_cell in the source update.
    struct WorldLightOverlayCell {
        u32 m_cell{};
        float m_coverage{};
        std::array<float, 3> m_sigma{};
        std::array<float, 3> m_emission{};
    };

    // One raw Matter particle for the dense GPU scatter: an absolute native cell,
    // a fill fraction and a global palette material id. The GPU scatters these
    // directly (no host sort/unique), accumulating coverage and fill-weighted
    // sigma/emission into the dense overlay.
    struct WorldLightMatterParticle {
        u32 m_cell{};
        float m_fill{};
        u32 m_material{};
    };

    // A committed world/overlay/particle/sky edit. m_revision is the producer's
    // source revision; the source adopts it only when the edit changes something.
    struct WorldLightSourceUpdate {
        std::span<const WorldLightChunkUpdate> m_chunks{};
        std::span<const WorldLightOverlayCell> m_overlay{};
        std::span<const WorldLightMatterParticle> m_particles{};
        u64 m_revision{};
        i32 m_sky_hour{-1};
        std::array<float, 3> m_sky{};
    };

    // ------------------------------------------------------------------
    // dirty rectangle
    // ------------------------------------------------------------------

    // Half-open integer cell rectangle [m_min, m_max) in native cell units,
    // unioned across the changed chunks and overlay cells of one edit. It is
    // per-update, not cumulative: applyUpdate starts from an empty rect and
    // reports only the current edit's changed region, so a later edit to one
    // region never re-invalidates probes touched by an earlier edit. The field
    // consumes at most one update per frame (it resets/reprocesses the dirty
    // rect only on a revision change), so an edit already consumed by the field
    // keeps its accumulated history.
    struct WorldLightDirtyRect {
        i32 m_min_x{}, m_min_y{}, m_max_x{}, m_max_y{};

        [[nodiscard]] bool empty() const noexcept {
            return m_min_x >= m_max_x or m_min_y >= m_max_y;
        }

        void include(const i32 x0, const i32 y0, const i32 x1, const i32 y1) noexcept {
            if (x0 >= x1 or y0 >= y1) {
                return;
            }
            if (empty()) {
                m_min_x = x0;
                m_min_y = y0;
                m_max_x = x1;
                m_max_y = y1;
                return;
            }
            m_min_x = std::min(m_min_x, x0);
            m_min_y = std::min(m_min_y, y0);
            m_max_x = std::max(m_max_x, x1);
            m_max_y = std::max(m_max_y, y1);
        }
    };

    // ------------------------------------------------------------------
    // dense CPU world source
    // ------------------------------------------------------------------

    // Dense, RHI-free native-cell source. Owns the full width*height cell ids, the
    // global material palette, a dense Matter overlay indexed by cell, the last
    // committed revision, sky and a dirty rectangle. Occupancy is a single bitset
    // level with one bit per kOccupancyBlockEdge x kOccupancyBlockEdge block, set
    // when the block contains any occupied cell (id != 0, or overlay coverage,
    // sigma or emission > 0); blocks are row-major over ceil(width/edge) x
    // ceil(height/edge), packed into u32 words with ceil(blocks_per_row / 32)
    // words per block row.
    class WorldLightSource {
    public:
        static constexpr u32 kOccupancyBlockEdge = 8u;

        [[nodiscard]] static Expected<WorldLightSource> create(
            WorldLightPlan plan, std::span<const WorldLightMaterial> materials);

        [[nodiscard]] std::error_code applyUpdate(const WorldLightSourceUpdate &update);

        [[nodiscard]] u16 cellId(u32 x, u32 y) const noexcept;
        [[nodiscard]] WorldLightOverlayCell overlayCell(u32 x, u32 y) const noexcept;
        [[nodiscard]] u64 revision() const noexcept;
        [[nodiscard]] WorldLightDirtyRect dirtyRect() const noexcept;

        // block selects the occupancy block row, word the u32 word index within
        // that row. Out-of-range block/word reads return zero.
        [[nodiscard]] u32 occupancyWord(u32 block, u32 word) const noexcept;

    private:
        void recomputeOccupancyBlock_(u32 block_x, u32 block_y) noexcept;

        WorldLightPlan m_plan{};
        u64 m_revision{};
        std::vector<WorldLightMaterial> m_materials{};
        std::vector<u16> m_cell_ids{};
        std::vector<WorldLightOverlayCell> m_overlay{};
        std::vector<u32> m_occupancy{};
        WorldLightDirtyRect m_dirty{};
        std::array<float, 3> m_sky{};
        i32 m_sky_hour{-1};
        u32 m_blocks_x{}, m_blocks_y{};
        u32 m_words_per_row{};
    };
}
