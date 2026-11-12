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

    [[nodiscard]] constexpr sim::definition::GenerationCompileParameters worldCompileParameters(const u32 world_edge) {
        auto parameters = kWorldCompileParameters;
        parameters.m_world_edge = world_edge;
        return parameters;
    }

    /// Runtime driver/physics use the canonical application metric and actor.
    /// Standalone numeric selection and previews retain their explicit parameters.
    [[nodiscard]] std::error_code validateRuntimeWorldParameters(const sim::GenerationRecipe &recipe);

    // ------------------------------------------------------------------
    // prepared numeric world and optional ready presentation
    // ------------------------------------------------------------------

    struct WorldDescriptor {
        std::string m_profile{};
        u64 m_seed{};
        u64 m_content_fingerprint{};
        WorldMetric m_metric{};
        u32 m_pack_schema_version{asset::Manifest::schema_version};
        u32 m_generator_version{sim::kGeneratorAlgorithmVersion};
        u32 m_world_edge{sim::kWorldEdge};

        [[nodiscard]] bool operator==(const WorldDescriptor &) const noexcept = default;
    };

    // One session's immutable authoring inputs and owned numeric baseline.
    // Declaration order keeps the catalog alive until the result is destroyed.
    class PreparedWorld {
        std::shared_ptr<const ContentCatalog> m_catalog;
        std::shared_ptr<const ContentPresentation> m_presentation;
        WorldDescriptor m_descriptor;
        sim::GeneratedWorld m_world;
        bool m_lighting_showcase{};

        // construction:
        PreparedWorld(
            std::shared_ptr<const ContentCatalog> catalog, WorldDescriptor descriptor, sim::GeneratedWorld world,
            bool lighting_showcase);

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
        [[nodiscard]] bool lightingShowcase() const noexcept;

        // exclusive preparation:
        [[nodiscard]] std::error_code attachPresentation(
            std::shared_ptr<const ContentPresentation> presentation, asset::RequestGeneration generation);
        [[nodiscard]] sim::ChunkGrid &grid() noexcept;
    };

    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        const std::filesystem::path &root, std::string_view profile, u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution = {},
        u32 world_edge = sim::kWorldEdge);
    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        std::span<const std::filesystem::path> roots, std::string_view profile, u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution = {},
        u32 world_edge = sim::kWorldEdge);
    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        ContentCatalog catalog, std::string_view profile, u64 seed, const ITimerClock &clock,
        const sim::GenerationExecution &execution = {});
    [[nodiscard]] Expected<PreparedWorld> prepareWorld(
        std::shared_ptr<const ContentCatalog> catalog, std::string_view profile, u64 seed, const ITimerClock &clock,
        const sim::GenerationExecution &execution = {});

    // ------------------------------------------------------------------
    // rendered world admission and the authored Lighting Lab
    // ------------------------------------------------------------------

    /// The lab is one fixed 1024-square authored recipe. Initial overview
    /// framing uses these cell constants as ordinary client camera values.
    /// The lab's fixed square extent coincides with rendered admission.
    inline constexpr u32 kLightingLabEdge = kRenderedWorldEdge;
    inline constexpr float kLightingLabOverviewCentreCell = 512.0f;
    inline constexpr float kLightingLabOverviewVerticalSpanCells = 1056.0f;

    /// True for the local name of the authored lab profile (qualified or not).
    [[nodiscard]] bool isLightingLabProfile(std::string_view profile) noexcept;

    /// True only when the selected catalog also carries the complete lab
    /// semantic material set, so another pack's same-named world still takes
    /// ordinary generation.
    [[nodiscard]] bool isLightingLabWorld(const ContentCatalog &catalog, std::string_view profile) noexcept;
    [[nodiscard]] std::error_code validateRenderedWorldEdge(u32 world_edge) noexcept;

    /// Fills the ordinary grid, initial matter and start of the fixed Lighting
    /// Lab recipe. Catalog semantic keys supply every numeric material ID and
    /// physical property; the recipe authors no GPU rows and no palette.
    [[nodiscard]] std::error_code populateLightingLab(sim::GeneratedWorld &world, const ContentCatalog &catalog);

    /// Builds the fixed lab candidate from a compiled plan without procedural
    /// rasterization or matter that the recipe would discard. The layout still
    /// carries the ordinary start site and the recipe owns every cell/seed.
    [[nodiscard]] Expected<sim::GeneratedWorld> buildLightingLabWorld(
        sim::GenerationPlan plan, const ContentCatalog &catalog);

    // ------------------------------------------------------------------
    // shared bounded lighting scene recipe
    // ------------------------------------------------------------------

    inline constexpr u32 kLightingSceneWidth = 64u, kLightingSceneHeight = 36u;

    /// Palette order is sediment/sand/regolith/iron/copper/lamp[/violet/cyan].
    /// Capture supplies its existing proxy IDs; gameplay supplies catalog IDs.
    [[nodiscard]] Expected<std::vector<u16>> lightingSceneLabels(
        std::span<const u16> material_ids, bool door_open, bool environment);

    /// Interactive service bay only. The first eight roles are the unchanged
    /// environment recipe; housing/tread/horizontal-pipe/vertical-pipe follow.
    /// Every obstruction is an actual native cell consumed by Driver and Grid.
    [[nodiscard]] Expected<std::vector<u16>> lightingShowcaseLabels(std::span<const u16> material_ids);

    /// Authored start admission uses the canonical contour table, not the
    /// procedural landing prefab/resource guarantees. Initial showcase only.
    [[nodiscard]] Expected<sim::GenerationStart> validateLightingShowcaseStart(
        const sim::ChunkGrid &grid, u64 *known_scratch_peak = nullptr);

    [[nodiscard]] Expected<PreparedWorld> prepareLightingShowcase(
        std::span<const std::filesystem::path> roots, u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution = {},
        u32 world_edge = sim::kWorldEdge);

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
