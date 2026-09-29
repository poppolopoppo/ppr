module;
#include "pP/Macros.h"

module engine.sim;

import :chunk_grid;
import :ecs;
import :footprint;
import :snapshot;

import engine.core;
import engine.math;

import std;

namespace pP::sim {
    // ------------------------------------------------------------------
    // snapshot error category
    // ------------------------------------------------------------------

    namespace {
        class SnapshotErrorCategory final : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override {
                return "sim";
            }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<errc>(ev)) {
                    case errc::ok: return "indicates success";
                    case errc::version_mismatch: return "snapshot was written by an incompatible version";
                    case errc::truncated: return "snapshot ended before its declared payload";
                    case errc::invalid_payload: return "snapshot payload is malformed";
                    default: return "unknown sim result (" + std::to_string(ev) + ")";
                }
            }
        };

        /// Minimum bytes a delta header occupies: chunk index + cell count.
        constexpr u32 kMinimumDeltaBytes = 8u;

        /// Minimum bytes a stream occupies: system id + rng state.
        constexpr u32 kMinimumStreamBytes = 12u;

        void putU16(Array<u8> &out, const u16 value) {
            out.push_back(static_cast<u8>(value & 0xFFu));
            out.push_back(static_cast<u8>((value >> 8u) & 0xFFu));
        }

        void putU32(Array<u8> &out, const u32 value) {
            for (u32 shift = 0u; shift < 32u; shift += 8u) {
                out.push_back(static_cast<u8>((value >> shift) & 0xFFu));
            }
        }

        void putU64(Array<u8> &out, const u64 value) {
            for (u32 shift = 0u; shift < 64u; shift += 8u) {
                out.push_back(static_cast<u8>((value >> shift) & 0xFFu));
            }
        }

        /// Sequential little-endian reader; every read either consumes its bytes
        /// or fails without moving the offset.
        class Reader {
        public:
            explicit Reader(const std::span<const u8> bytes) noexcept
                : m_bytes(bytes) {
            }

            [[nodiscard]] std::size_t remaining() const noexcept {
                return m_bytes.size() - m_offset;
            }

            [[nodiscard]] bool readU16(u16 &out) noexcept {
                return read(out);
            }

            [[nodiscard]] bool readU8(u8 &out) noexcept {
                return read(out);
            }

            [[nodiscard]] bool readU32(u32 &out) noexcept {
                return read(out);
            }

            [[nodiscard]] bool readU64(u64 &out) noexcept {
                return read(out);
            }

        private:
            template<std::unsigned_integral T>
            [[nodiscard]] bool read(T &out) noexcept {
                if (remaining() < sizeof(T)) {
                    return false;
                }

                u64 value = 0u;
                for (std::size_t index = 0u; index < sizeof(T); ++index) {
                    value |= static_cast<u64>(m_bytes[m_offset + index]) << (index * 8u);
                }

                m_offset += sizeof(T);
                out = safe_narrowing(value);

                return true;
            }

            std::span<const u8> m_bytes;
            std::size_t m_offset{0u};
        };

        [[nodiscard]] std::error_code validateEcs(const EcsSection &section) {
            const std::error_code invalid = make_error_code(errc::invalid_payload);
            if (section.m_ecs_version != 1u or
                section.m_generations.size() != section.m_alive.size() or
                section.m_generations.size() > std::numeric_limits<u32>::max() or
                section.m_columns.size() > kMaxComponents) {
                return invalid;
            }

            for (std::size_t index = 0u; index < section.m_alive.size(); ++index) {
                if (section.m_generations[index] == 0u or section.m_alive[index] > 1u) {
                    return invalid;
                }
            }

            Array<u8> free_seen(section.m_generations.size(), 0u);
            std::size_t dead_count = 0u;
            for (const u8 alive: section.m_alive) {
                dead_count += alive == 0u;
            }
            if (section.m_free_order.size() != dead_count) {
                return invalid;
            }
            for (const u32 index: section.m_free_order) {
                if (index >= section.m_generations.size() or section.m_alive[index] != 0u or free_seen[index] != 0u) {
                    return invalid;
                }
                free_seen[index] = 1u;
            }

            for (std::size_t id = 0u; id < section.m_columns.size(); ++id) {
                const EcsColumnData &column = section.m_columns[id];
                if (column.m_id != id or
                    column.m_stride == 0u or
                    column.m_owners.size() > std::numeric_limits<u32>::max() or
                    column.m_bytes.size() % column.m_stride != 0u or
                    column.m_bytes.size() / column.m_stride != column.m_owners.size()) {
                    return invalid;
                }

                Array<u8> seen(section.m_generations.size(), 0u);
                for (const Entity owner: column.m_owners) {
                    if (owner.m_index >= section.m_generations.size() or
                        section.m_alive[owner.m_index] != 1u or
                        section.m_generations[owner.m_index] != owner.m_generation or
                        seen[owner.m_index] != 0u) {
                        return invalid;
                    }
                    seen[owner.m_index] = 1u;
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code validateBodies(const Snapshot &snapshot) {
            const BodiesSection &bodies = snapshot.m_bodies;
            if (bodies.m_version != 1u or bodies.m_records.size() > snapshot.m_ecs.m_generations.size()) {
                return make_error_code(errc::invalid_payload);
            }
            u32 previous = 0u;
            bool first = true;
            for (const BodyRecord &record: bodies.m_records) {
                const Entity entity = record.m_entity;
                const BodyState &state = record.m_state;
                const bool invalid_key = entity.m_index >= snapshot.m_ecs.m_generations.size();
                if (invalid_key) {
                    return make_error_code(errc::invalid_payload);
                }
                const bool invalid_generation = snapshot.m_ecs.m_generations[entity.m_index] != entity.m_generation;
                const bool dead = snapshot.m_ecs.m_alive[entity.m_index] != 1u;
                const bool unordered = not first and entity.m_index <= previous;
                const bool invalid_state = not std::isfinite(state.m_x) or not std::isfinite(state.m_y) or
                    not std::isfinite(state.m_angle) or not std::isfinite(state.m_velocity_x) or
                    not std::isfinite(state.m_velocity_y) or not std::isfinite(state.m_angular_velocity);
                if (invalid_generation or dead or unordered or invalid_state) {
                    return make_error_code(errc::invalid_payload);
                }
                previous = entity.m_index;
                first = false;
            }
            return {};
        }

        /// Fail-closed gate shared by save, apply, restore, and load: version agreement,
        /// dense well-formed deltas, duplicate-free chunk indices, and finite
        /// temperatures. Duplicates are rejected instead of resolved last-wins:
        /// only `capture` builds snapshots and it lists every dirty chunk once.
        [[nodiscard]] std::error_code validate(const Snapshot &snapshot) {
            if (snapshot.m_version != kSnapshotVersion) {
                return make_error_code(errc::version_mismatch);
            }

            // More deltas than chunks must repeat an index; bound the scan below.
            if (snapshot.m_deltas.size() > kChunkCount) [[unlikely]] {
                return make_error_code(errc::invalid_payload);
            }

            Array<u32> seen{};
            seen.reserve(snapshot.m_deltas.size());

            for (const ChunkDelta &delta: snapshot.m_deltas) {
                if (delta.m_chunk >= kChunkCount or
                    delta.m_cells.size() != kCellsPerChunk or
                    std::ranges::find(seen, delta.m_chunk) != seen.end()) {
                    return make_error_code(errc::invalid_payload);
                }

                seen.push_back(delta.m_chunk);

                for (const Cell &cell: delta.m_cells) {
                    if (not isFinite<float>(cell.m_temperature)) {
                        return make_error_code(errc::invalid_payload);
                    }
                }
            }

            if (const std::error_code error = validateEcs(snapshot.m_ecs); error) {
                return error;
            }
            return validateBodies(snapshot);
        }
    }

    const std::error_category &error_category() noexcept {
        static const SnapshotErrorCategory g_snapshot_error_category{};
        return g_snapshot_error_category;
    }

    std::error_code make_error_code(const errc err) noexcept {
        return std::error_code{static_cast<int>(err), error_category()};
    }

    Snapshot capture(const ChunkGrid &grid, const u64 seed) {
        Snapshot snapshot{};
        snapshot.m_version = kSnapshotVersion;
        snapshot.m_seed = seed;

        for (const ChunkPos chunk: grid.dirtyChunks()) {
            const std::optional<std::span<const Cell> > payload = grid.residentCells(chunk);

            // Invariant: dirty chunks are resident.
            PPR_ASSERT(payload.has_value());

            ChunkDelta delta{};
            delta.m_chunk = chunkIndexOf(chunk);
            delta.m_cells.assign(payload->begin(), payload->end());

            snapshot.m_deltas.push_back(std::move(delta));
        }

        return snapshot;
    }

    Snapshot capture(const ChunkGrid &grid, const Registry &registry, const u64 seed) {
        Snapshot snapshot = capture(grid, seed);
        snapshot.m_ecs = registry.dump();
        // MSVC C1001 ICEs on const-View and const Registry::get<BodyState> across partitions;
        // traverse the already-dumped column instead of instantiating either.
        const auto body_id = registry.componentId<BodyState>();
        if (body_id.has_value()) {
            const EcsColumnData &column = snapshot.m_ecs.m_columns[*body_id];
            for (std::size_t row = 0u; row < column.m_owners.size(); ++row) {
                BodyState state{};
                PPR_ASSERT(column.m_stride == sizeof(BodyState));
                std::memcpy(&state, column.m_bytes.data() + row * column.m_stride, column.m_stride);
                snapshot.m_bodies.m_records.push_back({column.m_owners[row], state, true});
            }
        }
        std::ranges::sort(snapshot.m_bodies.m_records, [](const BodyRecord &left, const BodyRecord &right) {
            return left.m_entity.m_index < right.m_entity.m_index;
        });
        return snapshot;
    }

    std::error_code restore(Registry &registry, const Snapshot &snapshot) {
        if (const std::error_code error = validate(snapshot); error) {
            return error;
        }

        if (not snapshot.m_bodies.m_records.empty()) {
            const auto body_id = registry.componentId<BodyState>();
            if (not body_id or *body_id >= snapshot.m_ecs.m_columns.size()) {
                return make_error_code(errc::invalid_payload);
            }
            const EcsColumnData &column = snapshot.m_ecs.m_columns[*body_id];
            if (column.m_stride != sizeof(BodyState) or column.m_owners.size() != snapshot.m_bodies.m_records.size()) {
                return make_error_code(errc::invalid_payload);
            }
            for (std::size_t row = 0u; row < column.m_owners.size(); ++row) {
                const auto record = std::ranges::find_if(snapshot.m_bodies.m_records, [&](const BodyRecord &entry) {
                    return entry.m_entity == column.m_owners[row];
                });
                if (record == snapshot.m_bodies.m_records.end()) {
                    return make_error_code(errc::invalid_payload);
                }
                BodyState value{};
                std::memcpy(&value, column.m_bytes.data() + row * sizeof(BodyState), sizeof(BodyState));
                const BodyRecord observed{column.m_owners[row], value, record->m_awake};
                if (not (observed == *record)) {
                    return make_error_code(errc::invalid_payload);
                }
            }
        }

        if (registry.restore(snapshot.m_ecs)) {
            return make_error_code(errc::invalid_payload);
        }
        return {};
    }

    std::error_code apply(ChunkGrid &grid, const Snapshot &snapshot) {
        if (const std::error_code error = validate(snapshot); error) {
            return error;
        }

        // Validation above guarantees every cell below is in-world and finite, so
        // setCell cannot fail halfway through a delta.
        for (const ChunkDelta &delta: snapshot.m_deltas) {
            const ChunkPos chunk{delta.m_chunk % kChunksPerEdge, delta.m_chunk / kChunksPerEdge};

            for (u32 index = 0u; index < kCellsPerChunk; ++index) {
                const LocalCellPos local{index % kChunkEdge, index / kChunkEdge};

                if (const std::error_code error = grid.setCell(globalOf(chunk, local), delta.m_cells[index]); error) {
                    return error;
                }
            }
        }

        return {};
    }

    Expected<Array<u8> > save(const Snapshot &snapshot) {
        if (const std::error_code error = validate(snapshot); error) {
            return std::unexpected{error};
        }

        Array<u8> bytes{};

        putU32(bytes, snapshot.m_version);
        putU64(bytes, snapshot.m_seed);
        putU32(bytes, safe_narrowing(snapshot.m_deltas.size()));

        for (const ChunkDelta &delta: snapshot.m_deltas) {
            putU32(bytes, delta.m_chunk);
            putU32(bytes, safe_narrowing(delta.m_cells.size()));

            for (const Cell &cell: delta.m_cells) {
                putU16(bytes, cell.m_element);
                putU32(bytes, std::bit_cast<u32>(cell.m_temperature));
            }
        }

        putU32(bytes, safe_narrowing(snapshot.m_streams.size()));

        for (const RngStream &stream: snapshot.m_streams) {
            putU32(bytes, stream.m_system);
            putU64(bytes, stream.m_state);
        }

        putU32(bytes, snapshot.m_ecs.m_ecs_version);
        putU32(bytes, safe_narrowing(snapshot.m_ecs.m_generations.size()));
        for (std::size_t index = 0u; index < snapshot.m_ecs.m_generations.size(); ++index) {
            putU32(bytes, snapshot.m_ecs.m_generations[index]);
            bytes.push_back(snapshot.m_ecs.m_alive[index]);
        }
        putU32(bytes, safe_narrowing(snapshot.m_ecs.m_free_order.size()));
        for (const u32 index: snapshot.m_ecs.m_free_order) {
            putU32(bytes, index);
        }
        putU32(bytes, safe_narrowing(snapshot.m_ecs.m_columns.size()));
        for (const EcsColumnData &column: snapshot.m_ecs.m_columns) {
            putU32(bytes, column.m_id);
            putU32(bytes, column.m_stride);
            putU32(bytes, safe_narrowing(column.m_owners.size()));
            for (const Entity owner: column.m_owners) {
                putU32(bytes, owner.m_index);
                putU32(bytes, owner.m_generation);
            }
            for (const u8 value: column.m_bytes) {
                bytes.push_back(value);
            }
        }

        putU32(bytes, snapshot.m_bodies.m_version);
        putU64(bytes, snapshot.m_bodies.m_replay_ticks);
        putU32(bytes, safe_narrowing(snapshot.m_bodies.m_records.size()));
        for (const BodyRecord &record: snapshot.m_bodies.m_records) {
            putU32(bytes, record.m_entity.m_index);
            putU32(bytes, record.m_entity.m_generation);
            const BodyState &state = record.m_state;
            for (const float value: {
                     state.m_x, state.m_y, state.m_angle,
                     state.m_velocity_x, state.m_velocity_y, state.m_angular_velocity
                 }) {
                putU32(bytes, std::bit_cast<u32>(value));
            }
            bytes.push_back(static_cast<u8>(record.m_awake));
        }

        return bytes;
    }

    Expected<Snapshot> load(const std::span<const u8> bytes) {
        Reader reader{bytes};

        u32 version{};
        if (not reader.readU32(version)) {
            return std::unexpected{make_error_code(errc::truncated)};
        }

        if (version != kSnapshotVersion) {
            return std::unexpected{make_error_code(errc::version_mismatch)};
        }

        Snapshot snapshot{};
        snapshot.m_version = version;

        u32 delta_count{};
        if (not reader.readU64(snapshot.m_seed) or not reader.readU32(delta_count)) {
            return std::unexpected{make_error_code(errc::truncated)};
        }

        // Bound the reserve by what the remaining bytes could possibly describe.
        if (delta_count > reader.remaining() / kMinimumDeltaBytes) {
            return std::unexpected{make_error_code(errc::invalid_payload)};
        }

        snapshot.m_deltas.reserve(delta_count);

        for (u32 index = 0u; index < delta_count; ++index) {
            u32 chunk{};
            u32 cell_count{};
            if (not reader.readU32(chunk) or not reader.readU32(cell_count)) {
                return std::unexpected{make_error_code(errc::truncated)};
            }

            if (chunk >= kChunkCount or cell_count != kCellsPerChunk) {
                return std::unexpected{make_error_code(errc::invalid_payload)};
            }

            ChunkDelta delta{};
            delta.m_chunk = chunk;
            delta.m_cells.resize(cell_count);

            for (Cell &cell: delta.m_cells) {
                u16 element{};
                u32 temperature_bits{};
                if (not reader.readU16(element) or not reader.readU32(temperature_bits)) {
                    return std::unexpected{make_error_code(errc::truncated)};
                }

                cell.m_element = element;
                cell.m_temperature = std::bit_cast<float>(temperature_bits);
            }

            snapshot.m_deltas.push_back(std::move(delta));
        }

        u32 stream_count{};
        if (not reader.readU32(stream_count)) {
            return std::unexpected{make_error_code(errc::truncated)};
        }

        if (stream_count > reader.remaining() / kMinimumStreamBytes) {
            return std::unexpected{make_error_code(errc::invalid_payload)};
        }

        snapshot.m_streams.reserve(stream_count);

        for (u32 index = 0u; index < stream_count; ++index) {
            u32 system{};
            u64 state{};
            if (not reader.readU32(system) or not reader.readU64(state)) {
                return std::unexpected{make_error_code(errc::truncated)};
            }

            snapshot.m_streams.push_back(RngStream{system, state});
        }

        u32 slot_count{};
        if (not reader.readU32(snapshot.m_ecs.m_ecs_version) or not reader.readU32(slot_count)) {
            return std::unexpected{make_error_code(errc::truncated)};
        }
        if (slot_count > reader.remaining() / 5u) {
            return std::unexpected{make_error_code(errc::invalid_payload)};
        }
        snapshot.m_ecs.m_generations.reserve(slot_count);
        snapshot.m_ecs.m_alive.reserve(slot_count);
        for (u32 index = 0u; index < slot_count; ++index) {
            u32 generation{};
            u8 alive{};
            if (not reader.readU32(generation) or not reader.readU8(alive)) {
                return std::unexpected{make_error_code(errc::truncated)};
            }
            snapshot.m_ecs.m_generations.push_back(generation);
            snapshot.m_ecs.m_alive.push_back(alive);
        }

        u32 free_count{};
        if (not reader.readU32(free_count)) {
            return std::unexpected{make_error_code(errc::truncated)};
        }
        if (free_count > slot_count or free_count > reader.remaining() / 4u) {
            return std::unexpected{make_error_code(errc::invalid_payload)};
        }
        snapshot.m_ecs.m_free_order.reserve(free_count);
        for (u32 entry = 0u; entry < free_count; ++entry) {
            u32 index{};
            if (not reader.readU32(index)) {
                return std::unexpected{make_error_code(errc::truncated)};
            }
            snapshot.m_ecs.m_free_order.push_back(index);
        }

        u32 column_count{};
        if (not reader.readU32(column_count)) {
            return std::unexpected{make_error_code(errc::truncated)};
        }
        if (column_count > kMaxComponents or column_count > reader.remaining() / 12u) {
            return std::unexpected{make_error_code(errc::invalid_payload)};
        }
        snapshot.m_ecs.m_columns.reserve(column_count);
        for (u32 index = 0u; index < column_count; ++index) {
            EcsColumnData column{};
            u32 row_count{};
            if (not reader.readU32(column.m_id) or
                not reader.readU32(column.m_stride) or
                not reader.readU32(row_count)) {
                return std::unexpected{make_error_code(errc::truncated)};
            }
            if (column.m_id != index or
                column.m_stride == 0u or
                row_count > reader.remaining() / 8u or
                (row_count != 0u and
                    column.m_stride > (reader.remaining() - static_cast<std::size_t>(row_count) * 8u) / row_count)) {
                return std::unexpected{make_error_code(errc::invalid_payload)};
            }

            column.m_owners.reserve(row_count);
            for (u32 row = 0u; row < row_count; ++row) {
                Entity owner{};
                if (not reader.readU32(owner.m_index) or not reader.readU32(owner.m_generation)) {
                    return std::unexpected{make_error_code(errc::truncated)};
                }
                column.m_owners.push_back(owner);
            }
            const std::size_t byte_count = static_cast<std::size_t>(row_count) * column.m_stride;
            column.m_bytes.reserve(byte_count);
            for (std::size_t offset = 0u; offset < byte_count; ++offset) {
                u8 value{};
                if (not reader.readU8(value)) {
                    return std::unexpected{make_error_code(errc::truncated)};
                }
                column.m_bytes.push_back(value);
            }
            snapshot.m_ecs.m_columns.push_back(std::move(column));
        }

        u32 body_count{};
        if (not reader.readU32(snapshot.m_bodies.m_version) or
            not reader.readU64(snapshot.m_bodies.m_replay_ticks) or
            not reader.readU32(body_count)) {
            return std::unexpected{make_error_code(errc::truncated)};
        }
        if (body_count > snapshot.m_ecs.m_generations.size() or body_count > reader.remaining() / 33u) {
            return std::unexpected{make_error_code(errc::invalid_payload)};
        }
        snapshot.m_bodies.m_records.reserve(body_count);
        for (u32 index = 0u; index < body_count; ++index) {
            BodyRecord record{};
            u32 values[6]{};
            u8 awake{};
            if (not reader.readU32(record.m_entity.m_index) or not reader.readU32(record.m_entity.m_generation)) {
                return std::unexpected{make_error_code(errc::truncated)};
            }
            for (u32 &value: values) {
                if (not reader.readU32(value)) {
                    return std::unexpected{make_error_code(errc::truncated)};
                }
            }
            if (not reader.readU8(awake) or awake > 1u) {
                return std::unexpected{make_error_code(errc::invalid_payload)};
            }
            record.m_state = {
                std::bit_cast<float>(values[0]), std::bit_cast<float>(values[1]),
                std::bit_cast<float>(values[2]), std::bit_cast<float>(values[3]),
                std::bit_cast<float>(values[4]), std::bit_cast<float>(values[5])
            };
            record.m_awake = awake != 0u;
            snapshot.m_bodies.m_records.push_back(record);
        }

        if (reader.remaining() != 0u) {
            return std::unexpected{make_error_code(errc::invalid_payload)};
        }

        // Shared gate also rejects duplicate chunk indices and non-finite
        // temperatures in the payload.
        if (const std::error_code error = validate(snapshot); error) {
            return std::unexpected{error};
        }

        return snapshot;
    }
}
