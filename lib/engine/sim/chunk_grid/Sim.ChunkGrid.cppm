module;

export module engine.sim:chunk_grid;

import engine.core;

import std;

export namespace pP::sim {
    // ------------------------------------------------------------------
    // world and chunk geometry
    // ------------------------------------------------------------------

    /// Edge length of one chunk in cells. Square by construction.
    inline constexpr u32 kChunkEdge = 128u;

    /// Number of chunks along one world axis.
    inline constexpr u32 kChunksPerEdge = 32u;

    /// Edge length of the whole world in cells: kChunkEdge * kChunksPerEdge.
    inline constexpr u32 kWorldEdge = kChunkEdge * kChunksPerEdge;

    /// Total chunk count: kChunksPerEdge * kChunksPerEdge.
    inline constexpr u32 kChunkCount = kChunksPerEdge * kChunksPerEdge;

    /// Cell count of one chunk: kChunkEdge * kChunkEdge.
    inline constexpr u32 kCellsPerChunk = kChunkEdge * kChunkEdge;

    /// World-space cell coordinate; valid range is [0, kWorldEdge).
    struct GlobalCellPos {
        u32 m_x{};
        u32 m_y{};

        [[nodiscard]] constexpr bool operator==(const GlobalCellPos &) const noexcept = default;
    };

    /// Chunk coordinate inside the grid; valid range is [0, kChunksPerEdge).
    struct ChunkPos {
        u32 m_x{};
        u32 m_y{};

        [[nodiscard]] constexpr bool operator==(const ChunkPos &) const noexcept = default;
    };

    /// Cell coordinate inside a chunk; valid range is [0, kChunkEdge).
    struct LocalCellPos {
        u32 m_x{};
        u32 m_y{};

        [[nodiscard]] constexpr bool operator==(const LocalCellPos &) const noexcept = default;
    };

    [[nodiscard]] constexpr bool isInsideWorld(const GlobalCellPos pos) noexcept {
        return pos.m_x < kWorldEdge and pos.m_y < kWorldEdge;
    }

    [[nodiscard]] constexpr bool isInsideChunkGrid(const ChunkPos pos) noexcept {
        return pos.m_x < kChunksPerEdge and pos.m_y < kChunksPerEdge;
    }

    [[nodiscard]] constexpr bool isInsideChunk(const LocalCellPos pos) noexcept {
        return pos.m_x < kChunkEdge and pos.m_y < kChunkEdge;
    }

    [[nodiscard]] constexpr ChunkPos chunkOf(const GlobalCellPos pos) noexcept {
        return {pos.m_x / kChunkEdge, pos.m_y / kChunkEdge};
    }

    [[nodiscard]] constexpr LocalCellPos localOf(const GlobalCellPos pos) noexcept {
        return {pos.m_x % kChunkEdge, pos.m_y % kChunkEdge};
    }

    [[nodiscard]] constexpr GlobalCellPos globalOf(const ChunkPos chunk, const LocalCellPos local) noexcept {
        return {chunk.m_x * kChunkEdge + local.m_x, chunk.m_y * kChunkEdge + local.m_y};
    }

    /// Row-major chunk index into flat per-chunk arrays.
    [[nodiscard]] constexpr u32 chunkIndexOf(const ChunkPos pos) noexcept {
        return pos.m_y * kChunksPerEdge + pos.m_x;
    }

    // ------------------------------------------------------------------
    // cell and chunk state vocabulary
    // ------------------------------------------------------------------

    /// The only per-cell payload in phase 1: what the cell is and how hot it is.
    /// Deliberately content-free — no game-specific material or entity handles.
    struct Cell {
        u16 m_element{};
        float m_temperature{};

        [[nodiscard]] constexpr bool operator==(const Cell &) const noexcept = default;
    };

    /// Active chunks are stepped; quiescent chunks are skipped by simulation work
    /// but keep their payload and dirty state.
    enum class EChunkActivity : u8 {
        active,
        quiescent,
    };

    // ------------------------------------------------------------------
    // ChunkGrid — fixed cell world with lazy chunk payloads
    // ------------------------------------------------------------------

    /// Fixed 4096x4096 cell world stored as 32x32 chunks of 128x128 cells.
    /// Every chunk carries metadata (dirty flag + activity) from construction;
    /// cell payloads materialize on first write and are never evicted in phase 1,
    /// so residency is monotonic: dirty implies resident, and a non-resident chunk
    /// reads back as `Cell{}`.
    class ChunkGrid {
    private:
        struct ChunkState {
            bool m_dirty{false};
            EChunkActivity m_activity{EChunkActivity::active};
        };

        Array<ChunkState> m_states;
        Array<std::optional<Array<Cell> > > m_payloads;

    public:
        ChunkGrid();

        /// Writes a cell and marks its chunk dirty, materializing the payload on
        /// first write. Returns `result_out_of_range` when `pos` is outside the
        /// world and `invalid_argument` for a non-finite temperature.
        [[nodiscard]] std::error_code setCell(GlobalCellPos pos, Cell cell);

        /// Reads a cell. A non-resident chunk reads back as `Cell{}`.
        [[nodiscard]] Expected<Cell> getCell(GlobalCellPos pos) const;

        /// Borrowed view of a resident chunk payload; empty optional when the
        /// chunk has never been written.
        [[nodiscard]] std::optional<std::span<const Cell> > residentCells(ChunkPos pos) const noexcept;

        [[nodiscard]] bool isResident(ChunkPos pos) const noexcept;

        /// Marks a chunk dirty; implies payload residency. `markDirty` asserts
        /// that the content changed: the payload materializes (zero-filled when
        /// absent), so `capture` snapshots the chunk densely as whatever it
        /// currently holds.
        void markDirty(ChunkPos pos);

        void clearDirty(ChunkPos pos) noexcept;

        void clearAllDirty() noexcept;

        [[nodiscard]] EChunkActivity activity(ChunkPos pos) const noexcept;

        void setActivity(ChunkPos pos, EChunkActivity activity) noexcept;

        /// All dirty chunks in row-major order.
        [[nodiscard]] Array<ChunkPos> dirtyChunks() const;

        /// Dirty chunks restricted to `EChunkActivity::active`, row-major order.
        [[nodiscard]] Array<ChunkPos> activeDirtyChunks() const;

        [[nodiscard]] u32 residentChunkCount() const noexcept;

        [[nodiscard]] u32 dirtyChunkCount() const noexcept;

    private:
        [[nodiscard]] Array<ChunkPos> gatherDirty(bool activeOnly) const;

        /// Ensures the chunk payload exists; the backing store of residency.
        void materialize(ChunkPos pos);

        /// Payload slot for `pos`, materializing its chunk when absent.
        [[nodiscard]] Cell &cellAt(GlobalCellPos pos);
    };
}
