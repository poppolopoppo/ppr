module;

export module game.colony.sim_packet;

import engine.app;
import engine.core;
import engine.image;
import engine.sim;
import game.colony.driver;
import game.colony.worldgen;
import game.colony.content;
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
        float m_cell_size_m{1.0f};
        bool m_has_catalog{false};
        bool m_initial_matter_valid{true};
        u32 m_boundary_material_id{1u};
        u64 m_content_fingerprint{};
        Array<GridTerrainVisual> m_terrain_visuals{};
        Array<image::ImageAsset> m_terrain_images{};
        u64 m_frame_id{};

        // World epoch + committed tick at capture time. m_seed identifies
        // the user seed; m_world_generation is the true world identity
        // (bumped per init/regenerate — a same-seed regeneration differs).
        u64 m_seed{};
        u64 m_world_generation{};
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

        // Parallel capture for ack: the router's edit generation captured
        // per chunk. ackPresentChunks ties a clear to the same generation,
        // so the lane ack of a stale payload cannot accidentally clear a
        // newer ad-hoc dirtied row sharing the same chunk.
        Array<u64> m_present_revisions{};

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

    /// Initial graphics preparation copies at most eight chunks per frame.
    /// Only the prepared prefix is submitted, so uncached terrain is not drawn.
    inline constexpr u32 kInitialPresentationBatchChunks = 8u;
    [[nodiscard]] Expected<SimFramePacket> makeInitialPresentationBatch(
        u64 frame_id, const ColonyDriver &driver, u32 first_chunk, u32 chunk_count);

    /// Coordinator-only capture from an exclusively owned prepared session.
    /// Converts resident cells directly to owned u16 uploads, with no full sim
    /// snapshot or live-session borrow crossing into the rendering lane.
    [[nodiscard]] Expected<GridPass::GridStagedFrame> captureWorldPresentation(
        const ColonyDriver &driver, const IContext *context = nullptr);
    /// Whole-world preview preparation from an exclusively frozen session.
    /// Generated metadata belongs to this driver's exclusively frozen prepared world.
    /// The caller supplies a monotonic revision; terrain IDs remain unchanged.
    [[nodiscard]] Expected<GridPass::GridStagedFrame> captureDiagnosticPresentation(
        const ColonyDriver &driver, const sim::GeneratedWorld &generated, EGridDiagnosticMode mode, u64 revision,
        float temperature_min_k, float temperature_max_k, const IContext &context);

    /// Client-owned initial publication cursor. At most one upload batch is
    /// outstanding; clean frames expose only the confirmed prepared prefix.
    class InitialPresentation {
        u32 m_next_chunk{};
        bool m_reset_applied{};
        std::optional<u64> m_frame{};
        Array<GridTileSubmission> m_visible{};
        Array<GridTileSubmission> m_pending{};

    public:
        [[nodiscard]] u32 completedChunks() const noexcept;
        [[nodiscard]] Expected<GridPass::GridStagedFrame> stage(u64 frame_id, const ColonyDriver &driver);
        /// Returns true only when the final batch was successfully presented.
        [[nodiscard]] bool consume(const RenderFrameResult &result, u64 world_generation);
    };

    // Fail-closed coherence: the packet authorizes presentation only while
    // the live tick AND world epoch still match capture time. A regeneration
    // (seed change) or a tick advance after capture invalidates the payload.
    [[nodiscard]] bool isSimPacketCoherent(const SimFramePacket &packet, u64 live_tick, u64 live_seed) noexcept;

    // Builds the owned GridStagedFrame from the owned packet only (never the
    // live grid); the lane replays it via GridPass::applyStagedFrame on the
    // owner lane. The driver/dev-side present set is acknowledged at lane
    // consumption (see onLaneFrameConsumed_), not here. On an invalid
    // payload the stage is cleared and the error returned fail-closed; the
    // client sets up a full-refresh + reset-caches retry.
    [[nodiscard]] std::error_code buildGridStagedFrame(const SimFramePacket &packet, GridPass::GridStagedFrame &stage);
}
