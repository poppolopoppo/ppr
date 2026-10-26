export module game.colony.worldgen;

import engine.core;
import engine.asset;
import engine.sim;
import game.colony.content;
import game.colony.metric;

import std;

namespace pP::colony {
    class PreparedWorldConstruction;
}

export namespace pP::colony {
    // ------------------------------------------------------------------
    // application world compilation parameters
    // ------------------------------------------------------------------

    inline constexpr sim::definition::GenerationCompileParameters kWorldCompileParameters{
        kFineCellMetres, kReferenceThicknessMetres, 0.4, 0.9};

    /// Runtime driver/physics use the canonical application metric and actor.
    /// Standalone numeric selection and previews retain their explicit parameters.
    [[nodiscard]] std::error_code validateRuntimeWorldParameters(const sim::GenerationRecipe &recipe);

    // ------------------------------------------------------------------
    // prepared numeric world and optional ready presentation
    // ------------------------------------------------------------------

    struct WorldDescriptor {
        u32 m_pack_schema_version{asset::Manifest::schema_version};
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
        std::shared_ptr<const ContentCatalog> m_catalog;
        std::shared_ptr<const ContentPresentation> m_presentation;
        WorldDescriptor m_descriptor;
        sim::GeneratedWorld m_world;

        // construction:
        PreparedWorld(
            std::shared_ptr<const ContentCatalog> catalog, WorldDescriptor descriptor, sim::GeneratedWorld world);

        // factories:
        friend class PreparedWorldConstruction;

    public:
        PreparedWorld(PreparedWorld &&) noexcept;
        PreparedWorld &operator=(PreparedWorld &&) noexcept;

        ~PreparedWorld();

        PreparedWorld(const PreparedWorld &) = delete;
        PreparedWorld &operator=(const PreparedWorld &) = delete;

        // queries:
        [[nodiscard]] const ContentCatalog &catalog() const noexcept;
        [[nodiscard]] const std::shared_ptr<const ContentCatalog> &catalogOwner() const noexcept;
        [[nodiscard]] const std::shared_ptr<const ContentPresentation> &presentation() const noexcept;
        [[nodiscard]] const WorldDescriptor &descriptor() const noexcept;
        [[nodiscard]] const sim::GeneratedWorld &generated() const noexcept;

        // exclusive preparation:
        [[nodiscard]] std::error_code attachPresentation(
            std::shared_ptr<const ContentPresentation> presentation, asset::RequestGeneration generation);
        [[nodiscard]] sim::ChunkGrid &grid() noexcept;
    };

    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        const std::filesystem::path &root, std::string_view profile, u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution = {});
    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        std::span<const std::filesystem::path> roots, std::string_view profile, u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution = {});
    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        ContentCatalog catalog, std::string_view profile, u64 seed, const ITimerClock &clock,
        const sim::GenerationExecution &execution = {});
    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        std::shared_ptr<const ContentCatalog> catalog, std::string_view profile, u64 seed, const ITimerClock &clock,
        const sim::GenerationExecution &execution = {});

    /// Checkpoint assembly seam: metadata/grid were fully validated by the game
    /// envelope decoder; this verifies compatible descriptor/compiled-plan identity.
    [[nodiscard]] Expected<PreparedWorld> restorePreparedWorld(
        ContentCatalog catalog, WorldDescriptor descriptor, sim::GeneratedWorld world);
    [[nodiscard]] Expected<PreparedWorld> restorePreparedWorld(
        std::shared_ptr<const ContentCatalog> catalog, WorldDescriptor descriptor, sim::GeneratedWorld world);

    /// Resolves authoring references once into an owned, validated numeric plan.
    [[nodiscard]] Expected<sim::GenerationPlan> compileWorldPlan(
        const ContentCatalog &catalog, std::string_view profile, u64 seed);
    /// Flat CPU diagnostics for an exclusively owned validated generated world.
    /// Blocking: coordinator/headless caller only. Failure may leave partial files.
    [[nodiscard]] std::error_code exportWorldDiagnostics(
        const ContentCatalog &catalog, const sim::GeneratedWorld &world, const WorldDescriptor &descriptor,
        const std::filesystem::path &directory, const IContext *context = nullptr);
    /// Streams one P6 pixel per cell from an owned uniform-biome region, +Y-up.
    [[nodiscard]] std::error_code exportWorldgenRegionImages(
        const ContentCatalog &catalog, const sim::ChunkGrid &grid, sim::GenerationCellBounds bounds, u32 biome_index,
        const std::filesystem::path &directory, const IContext *context = nullptr);
}
