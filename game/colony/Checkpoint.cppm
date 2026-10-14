module;
export module game.colony.checkpoint;
import engine.core;
import engine.sim;
import game.colony.content;
import game.colony.driver;
import game.colony.worldgen;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // world checkpoint
    // ------------------------------------------------------------------

    inline constexpr u32 kWorldCheckpointVersion = 3u;
    inline constexpr u64 kMaxWorldCheckpointBytes = 256u * 1024u * 1024u;

    // ------------------------------------------------------------------
    // checkpoint material
    // ------------------------------------------------------------------

    struct CheckpointMaterial {
        u16 m_id{};
        std::string m_key{};
        [[nodiscard]] bool operator==(const CheckpointMaterial &) const = default;
    };

    /// Owned full checkpoint; no live session, held input or derived cache is borrowed.
    struct WorldCheckpoint {
        WorldDescriptor m_descriptor{};
        Array<CheckpointMaterial> m_materials{};
        Array<sim::GenerationSite> m_sites{};
        sim::GenerationGeometry m_geometry{};
        sim::InitialMatter m_matter{}; // Historical generation metadata only.
        sim::MatterParticleSnapshot m_particles{};
        sim::GenerationStart m_start{};
        sim::GenerationReport m_report{};
        sim::Snapshot m_snapshot{};
        std::optional<sim::Entity> m_controlled{};
        u64 m_tick{};
        u32 m_tick_hz{60u};
        bool m_initial_matter_valid{true};
    };

    // ------------------------------------------------------------------
    // checkpoint operations
    // ------------------------------------------------------------------

    /// Coordinator-only: driver and generated metadata are exclusively frozen at
    /// one completed tick. Return the session owner before encoding/writing this value.
    [[nodiscard]] Expected<WorldCheckpoint> captureWorldCheckpoint(
        const ColonyDriver &driver, const sim::GeneratedWorld &generated, const IContext *context = nullptr);
    [[nodiscard]] std::error_code validateWorldCheckpoint(
        const WorldCheckpoint &checkpoint, const ContentCatalog &catalog, const IContext *context = nullptr);
    [[nodiscard]] Expected<Array<u8>> encodeWorldCheckpoint(
        const WorldCheckpoint &checkpoint, const ContentCatalog &catalog, const IContext *context = nullptr);
    [[nodiscard]] Expected<WorldCheckpoint> decodeWorldCheckpoint(
        std::span<const u8> bytes, const ContentCatalog &catalog, const IContext *context = nullptr);
    /// Builds a fresh unpublished owner from saved cells; no terrain regeneration.
    [[nodiscard]] Expected<std::unique_ptr<ColonyDriver>> restoreWorldCheckpoint(
        ContentCatalog catalog, WorldCheckpoint checkpoint, u64 world_generation,
        const sim::GenerationExecution &execution = {});
}
