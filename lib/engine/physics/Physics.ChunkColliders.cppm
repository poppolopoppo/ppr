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

    /// Row-major element IDs (edge * edge); nonzero IDs are solid. Missing chunks are empty.
    struct ColliderChunkView {
        ColliderChunkPos m_pos{};
        std::span<const u16> m_elements;
        bool m_needed{}; // Active body, query or swept-motion interest in this chunk.
    };

    class ChunkColliders {
        std::map<ColliderChunkPos, std::vector<ChainHandle> > m_chains;
        std::set<ColliderChunkPos> m_pending;

    public:
        /// Dirty positions are delta notifications; caller resubmits only on a new edit.
        /// Per-chunk replacement is atomic; processed receives number of chunks this call.
        [[nodiscard]] std::error_code rebuildDirtyChunks(Scene &scene, std::span<const ColliderChunkView> chunks,
                                                         std::span<const ColliderChunkPos> dirty, u32 edge,
                                                         u32 max_chunks, u32 &processed) noexcept;

        /// Explicit query activates this chunk even if it has no active-body interest.
        [[nodiscard]] std::error_code materializeForQuery(Scene &scene, std::span<const ColliderChunkView> chunks,
                                                          ColliderChunkPos pos, u32 edge) noexcept;

        [[nodiscard]] std::size_t chainCount(ColliderChunkPos pos) const noexcept;

        [[nodiscard]] std::size_t pendingCount() const noexcept;
    };
}
