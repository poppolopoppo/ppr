module;

export module game.colony.sim_packet;

import engine.app;
import engine.core;
import engine.sim;
import game.colony.driver;
import game.colony.pathfinding;
import game.colony.translator;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // P0 serial-explicit owned sim packet: values and owned storage only,
    // built from the live ColonyDriver after update() and consumed on the
    // same thread by the present/overlay phases. Lives in game/ because
    // engine.app must not link sim/physics. No borrowed spans escape:
    // chunk lists AND chunk cell contents are copied. Same-thread first —
    // P2 moves this packet.
    // ------------------------------------------------------------------

    // Owned cell payload of one chunk: the render content captured for a
    // dirty chunk. Row-major sim::Cell array of exactly sim::kCellsPerChunk.
    struct SimChunkCells {
        sim::ChunkPos m_pos{};
        Array<sim::Cell> m_cells{};
    };

    struct SimFramePacket {
        u64 m_frame_id{};

        // World epoch + committed tick at capture time.
        u64 m_seed{};
        u64 m_tick{};

        // Panel-safe cached counters (owned copies, not live reads).
        u32 m_dig_total{};
        u32 m_agent_count{};
        PathCounts m_path_counts{};
        u32 m_errands{};

        // Owned copy of the complete pending presentation-dirty set at
        // capture time (drives the overlay outlines and the upload set).
        // Consumption acknowledges exactly this set; dirt that arrived
        // after capture stays pending for the next frame.
        Array<sim::ChunkPos> m_present_chunks{};

        // Owned copy of every resident chunk coordinate at capture time.
        // Present preparation stages exactly this set: a post-capture
        // residency change (new resident chunk) cannot alter the frame.
        Array<sim::ChunkPos> m_resident_chunks{};

        // Owned cell contents for the chunks the frame uploads: the present
        // chunks normally, every resident chunk after a translator reset
        // (world regeneration or a previous present failure cleared the
        // pass cache, so the next present must re-upload the whole world).
        Array<SimChunkCells> m_chunk_cells{};

        // True when m_chunk_cells covers every resident chunk (see above).
        // Set by the caller that reset the translator this frame.
        bool m_full_refresh{false};
    };

    // Captures tick/epoch/counters plus the render payload: resident coords
    // always, cell contents for the present chunks (or for every resident
    // chunk when full_refresh). Never touches translator/pass state.
    [[nodiscard]] SimFramePacket makeSimFramePacket(
        u64 frame_id, const ColonyDriver &driver, bool full_refresh = false);

    // Fail-closed coherence: the packet authorizes presentation only while
    // the live tick AND world epoch still match capture time. A regeneration
    // (seed change) or a tick advance after capture invalidates the payload.
    [[nodiscard]] bool isSimPacketCoherent(const SimFramePacket &packet, u64 live_tick, u64 live_seed) noexcept;

    // Same-thread present preparation consuming ONLY the owned packet (never
    // the live grid): marks and acknowledges exactly the packet's present
    // set at this defined consumption point, stages every packet-resident
    // tile, and uploads the packet-captured cells for dirty chunks. Dirt
    // that arrived after capture stays pending (unmarked, unacknowledged),
    // so a post-capture producer mutation cannot alter the output.
    // Error semantics mirror ColonyTranslator::submit: any staging/upload
    // failure resets the translator/pass pair and returns the error, so the
    // caller must full-refresh the NEXT frame to re-upload the world.
    [[nodiscard]] std::error_code presentSimPacket(
        const SimFramePacket &packet, ColonyDriver &driver, ColonyTranslator &translator, GridPass &pass);
}
