export module game.colony.worldgen;

import engine.core;
import engine.sim;
import game.colony.content;
import game.colony.metric;

import std;

export namespace pP::colony {
    struct WorldDescriptor {
        u32 m_pack_schema_version{kContentSchemaVersion};
        u32 m_generator_version{sim::kGeneratorAlgorithmVersion};
        std::string m_profile{};
        u64 m_seed{};
        u64 m_content_fingerprint{};
        WorldMetric m_metric{};
        [[nodiscard]] bool operator==(const WorldDescriptor &) const noexcept = default;
    };

    // One session's immutable authoring inputs and owned numeric baseline.
    // Declaration order keeps the catalog alive until the result is destroyed.
    class PreparedWorld {
        ContentCatalog m_catalog;
        WorldDescriptor m_descriptor;
        sim::GeneratedWorld m_world;
        PreparedWorld(ContentCatalog catalog, WorldDescriptor descriptor, sim::GeneratedWorld world);
        friend Expected<PreparedWorld> restorePreparedWorld(ContentCatalog, WorldDescriptor, sim::GeneratedWorld);
        friend Expected<PreparedWorld> prepareWorld(
            ContentCatalog, std::string_view, u64, const ITimerClock &, const sim::GenerationExecution &);

    public:
        PreparedWorld(PreparedWorld &&) noexcept;
        PreparedWorld &operator=(PreparedWorld &&) noexcept;
        ~PreparedWorld();
        PreparedWorld(const PreparedWorld &) = delete;
        PreparedWorld &operator=(const PreparedWorld &) = delete;
        [[nodiscard]] const ContentCatalog &catalog() const noexcept;
        [[nodiscard]] const WorldDescriptor &descriptor() const noexcept;
        [[nodiscard]] const sim::GeneratedWorld &generated() const noexcept;
        [[nodiscard]] sim::ChunkGrid &grid() noexcept;
    };
    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        const std::filesystem::path &root, std::string_view profile, u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution = {});
    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        ContentCatalog catalog, std::string_view profile, u64 seed, const ITimerClock &clock,
        const sim::GenerationExecution &execution = {});

    /// Checkpoint assembly seam: metadata/grid were fully validated by the game
    /// envelope decoder; this verifies compatible descriptor/compiled-plan identity.
    [[nodiscard]] Expected<PreparedWorld> restorePreparedWorld(
        ContentCatalog catalog, WorldDescriptor descriptor, sim::GeneratedWorld world);

    /// Resolves authoring references once into an owned, validated numeric plan.
    [[nodiscard]] Expected<sim::GenerationPlan> compileWorldPlan(
        const ContentCatalog &catalog, std::string_view profile, u64 seed);
    /// Flat CPU diagnostics for an exclusively owned validated generated world.
    /// Blocking: coordinator/headless caller only. Failure may leave partial files.
    [[nodiscard]] std::error_code exportWorldDiagnostics(
        const ContentCatalog &catalog, const sim::GeneratedWorld &world, const WorldDescriptor &descriptor,
        const std::filesystem::path &directory, const IContext *context = nullptr);
    /// Explicit legacy smoke/many-agent fixture only; authored sessions use prepareWorld.
    /// Kept game-owned so engine generation remains numeric-plan driven.
    [[nodiscard]] std::error_code generateLegacyFixture(
        u64 seed, sim::ChunkGrid &grid, const sim::GenerationExecution *execution = nullptr);
}
