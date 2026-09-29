module;

export module engine.sim:snapshot;

import engine.core;

// Deliberately a plain import, never `export import`: the `engine.sim` umbrella
// is the sole public entry point, and the exported signatures below name
// `:chunk_grid` types that only the umbrella re-exports.
import :chunk_grid;
import :ecs;
import :footprint;

import std;

export namespace pP::sim {
    // ------------------------------------------------------------------
    // snapshot version and errors
    // ------------------------------------------------------------------

    /// Serialization contract of `save`/`load`. Bump on any byte-layout change;
    /// `load` fail-closes on any other value before touching the payload.
    inline constexpr u32 kSnapshotVersion = 3u;

    enum class errc : int {
        ok = 0,
        version_mismatch = 1,
        truncated = 2,
        invalid_payload = 3,
    };

    [[nodiscard]] const std::error_category &error_category() noexcept;

    [[nodiscard]] std::error_code make_error_code(errc err) noexcept;

    // ------------------------------------------------------------------
    // snapshot payload
    // ------------------------------------------------------------------

    /// Per-system deterministic RNG state carried across save/load.
    struct RngStream {
        u32 m_system{};
        u64 m_state{};

        [[nodiscard]] constexpr bool operator==(const RngStream &) const noexcept = default;
    };

    /// One chunk captured as a dense run of exactly `kCellsPerChunk` cells.
    /// Sub-chunk tracking is a later phase.
    struct ChunkDelta {
        u32 m_chunk{};
        Array<Cell> m_cells{};

        [[nodiscard]] bool operator==(const ChunkDelta &) const = default;
    };

    /// Body values are keyed by slot and generation; runtime Box2D IDs never persist.
    struct BodyRecord {
        Entity m_entity{};
        BodyState m_state{};
        bool m_awake{true};

        [[nodiscard]] bool operator==(const BodyRecord &other) const noexcept {
            const BodyState &a = m_state;
            const BodyState &b = other.m_state;
            return m_entity == other.m_entity and
                m_awake == other.m_awake and
                a.m_x == b.m_x and
                a.m_y == b.m_y and
                a.m_angle == b.m_angle and
                a.m_velocity_x == b.m_velocity_x and
                a.m_velocity_y == b.m_velocity_y and
                a.m_angular_velocity == b.m_angular_velocity;
        }
    };

    struct BodiesSection {
        u32 m_version{1u};
        Array<BodyRecord> m_records{};
        // A replay driver must reconstruct the original ordered world and run
        // this many fixed ticks before it can claim solver continuation.
        u64 m_replay_ticks{};

        [[nodiscard]] bool operator==(const BodiesSection &) const = default;
    };

    struct Snapshot {
        u32 m_version{kSnapshotVersion};
        u64 m_seed{};
        Array<ChunkDelta> m_deltas{};
        Array<RngStream> m_streams{};
        EcsSection m_ecs{};
        BodiesSection m_bodies{};

        [[nodiscard]] bool operator==(const Snapshot &) const = default;
    };

    /// Reads the currently dirty chunks of `grid` into a snapshot. Purely
    /// observational: `grid` keeps its dirty state. Phase 1 registers no RNG
    /// systems, so the captured stream list is empty; consumers may still carry
    /// streams through `save`/`load`.
    [[nodiscard]] Snapshot capture(const ChunkGrid &grid, u64 seed);

    /// Captures dirty grid chunks and the complete ECS slot/column state.
    [[nodiscard]] Snapshot capture(const ChunkGrid &grid, const Registry &registry, u64 seed);

    /// Validates the whole snapshot first and mutates `grid` only when every
    /// delta is well-formed; a rejected snapshot leaves the grid untouched.
    /// `apply` MERGES the deltas into the target: chunks absent from the
    /// snapshot keep their content and their dirty state (there is no implicit
    /// reset — `ChunkGrid::clearAllDirty` is the reset path). Consequently
    /// `capture(apply(g)) == snapshot` holds only for a clean `g`.
    [[nodiscard]] std::error_code apply(ChunkGrid &grid, const Snapshot &snapshot);

    /// Replaces ECS state without altering the grid. Validates before mutation.
    [[nodiscard]] std::error_code restore(Registry &registry, const Snapshot &snapshot);

    /// Little-endian v3 encoding: the ECS tail is followed by a versioned,
    /// entity-keyed bodies section and a fixed-tick replay cursor.
    [[nodiscard]] Expected<Array<u8> > save(const Snapshot &snapshot);

    /// Fail-closed inverse of `save`: version mismatch, short input, and
    /// malformed payloads are distinguishable through `errc`.
    [[nodiscard]] Expected<Snapshot> load(std::span<const u8> bytes);
}

export template<>
struct std::is_error_code_enum<pP::sim::errc> : true_type { // NOLINT(*-dcl58-cpp)
};
