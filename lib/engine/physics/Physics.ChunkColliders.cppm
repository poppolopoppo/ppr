module;

export module engine.physics:chunk_colliders;

import :scene;
import engine.core;
import std;

export namespace pP::physics {
    struct ColliderChunkPos {
        i32 m_x{};
        i32 m_y{};

        [[nodiscard]] constexpr auto operator<=>(const ColliderChunkPos &) const noexcept = default;
    };

    /// Row-major occupancy (edge * edge); nonzero samples are solid. Missing interior chunks are empty.
    struct ColliderChunkView {
        ColliderChunkPos m_pos{};
        std::span<const u16> m_elements;
        bool m_needed{}; // Active body, query or swept-motion interest in this chunk.
    };

    struct ColliderGridGeometry {
        float m_cell_size_m{1.0f};
        float m_origin_x_m{};
        float m_origin_y_m{};
        // Both zero select an unbounded fixture lattice. Bounded grids use
        // protected-solid samples outside this half-open sample domain.
        u32 m_width_cells{};
        u32 m_height_cells{};
    };

    class ChunkColliders {
        ColliderGridGeometry m_geometry{};
        // H4b/Slice 6: per-chunk payload snapshot shadowing m_chains. Scene
        // exposes no chain-content query, so the previous drain's geometry is
        // cached here to diff against; m_shapes[pos][i] describes the chain
        // held by m_chains[pos][i]. Entries are committed only after the
        // Scene splice succeeds, so every failure path leaves both maps
        // untouched exactly like the legacy code.
        struct StoredChain {
            std::vector<ChainPoint> m_points;
            ChainPoint m_before{};
            ChainPoint m_after{};
            bool m_loop{};
        };

        std::map<ColliderChunkPos, std::vector<ChainHandle>> m_chains;
        std::map<ColliderChunkPos, std::vector<StoredChain>> m_shapes;
        std::set<ColliderChunkPos> m_pending;

    public:
        explicit ChunkColliders(ColliderGridGeometry geometry = {}) noexcept;

        /// Dirty positions are delta notifications; caller resubmits only on a new edit.
        /// Per-chunk replacement is atomic; processed receives number of chunks this call.
        [[nodiscard]] std::error_code rebuildDirtyChunks(
            Scene &scene, std::span<const ColliderChunkView> chunks, std::span<const ColliderChunkPos> dirty, u32 edge,
            u32 max_chunks, u32 &processed);

        /// Explicit query activates this chunk even if it has no active-body interest.
        [[nodiscard]] std::error_code materializeForQuery(
            Scene &scene, std::span<const ColliderChunkView> chunks, ColliderChunkPos pos, u32 edge);

        [[nodiscard]] std::size_t chainCount(ColliderChunkPos pos) const noexcept;

        [[nodiscard]] std::size_t pendingCount() const noexcept;
    };
}
