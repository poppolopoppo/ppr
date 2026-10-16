module;
#include "pP/Macros.h"
module game.colony.worldgen;

import engine.core;
import engine.sim;

import std;

namespace pP::colony {
    // ------------------------------------------------------------------
    // private factory construction
    // ------------------------------------------------------------------

    class PreparedWorldConstruction {
    public:
        [[nodiscard]] static PreparedWorld make(
            std::shared_ptr<const ContentCatalog> catalog, WorldDescriptor descriptor, sim::GeneratedWorld world) {
            return PreparedWorld{std::move(catalog), std::move(descriptor), std::move(world)};
        }
    };

    // ------------------------------------------------------------------
    // prepared world
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(WorldPreparation, info, none)

    PreparedWorld::PreparedWorld(
        std::shared_ptr<const ContentCatalog> catalog, WorldDescriptor descriptor, sim::GeneratedWorld world)
        : m_catalog(std::move(catalog)), m_descriptor(std::move(descriptor)), m_world(std::move(world)) {
    }

    PreparedWorld::PreparedWorld(PreparedWorld &&) noexcept = default;
    PreparedWorld &PreparedWorld::operator=(PreparedWorld &&) noexcept = default;

    PreparedWorld::~PreparedWorld() = default;

    const ContentCatalog &PreparedWorld::catalog() const noexcept {
        return *m_catalog;
    }

    const std::shared_ptr<const ContentCatalog> &PreparedWorld::catalogOwner() const noexcept {
        return m_catalog;
    }

    const std::shared_ptr<const ContentPresentation> &PreparedWorld::presentation() const noexcept {
        return m_presentation;
    }

    std::error_code PreparedWorld::attachPresentation(
        std::shared_ptr<const ContentPresentation> presentation, const asset::RequestGeneration generation) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, not presentation or
                                      presentation->catalog() != m_catalog or
                                      presentation->generation() != generation or
                                      presentation->catalog()->worldId() != m_catalog->worldId() or
                                      presentation->catalog()->contentFingerprint() !=
                                          std::optional<u64>{m_descriptor.m_content_fingerprint}
                                  ? std::make_error_code(std::errc::invalid_argument)
                                  : std::error_code{});

        m_presentation = std::move(presentation);
        return {};
    }

    const WorldDescriptor &PreparedWorld::descriptor() const noexcept {
        return m_descriptor;
    }

    const sim::GeneratedWorld &PreparedWorld::generated() const noexcept {
        return m_world;
    }

    sim::ChunkGrid &PreparedWorld::grid() noexcept {
        return m_world.m_grid;
    }

    // ------------------------------------------------------------------
    // world preparation
    // ------------------------------------------------------------------

    std::error_code validateRuntimeWorldParameters(const sim::GenerationRecipe &recipe) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            recipe.m_cell_size_m == kWorldCompileParameters.m_cell_size_m and
                    recipe.m_reference_thickness_m == kWorldCompileParameters.m_reference_thickness_m and
                    recipe.m_actor_half_width_m == kWorldCompileParameters.m_actor_half_width_m and
                    recipe.m_actor_half_height_m == kWorldCompileParameters.m_actor_half_height_m
                ? std::error_code{}
                : std::make_error_code(std::errc::not_supported));
        return {};
    }

    Expected<PreparedWorld> prepareWorld(
        const std::filesystem::path &root, const std::string_view profile, const u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution) {
        const std::filesystem::path roots[] = {root};
        return prepareWorld(roots, profile, seed, clock, diagnostics, execution);
    }

    Expected<PreparedWorld> prepareWorld(
        const std::span<const std::filesystem::path> roots, const std::string_view profile, const u64 seed,
        const ITimerClock &clock, config::Diagnostics &diagnostics, const sim::GenerationExecution &execution) {
        auto catalog = loadContentCatalog(roots, diagnostics, execution.m_context.get());
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, catalog);

        auto owner = std::make_shared<const ContentDescriptorCatalog>(std::move(*catalog));
        auto world = resolveContentWorld(*owner, profile, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, world);

        auto selected = selectWorldCatalog(std::move(owner), *world, kWorldCompileParameters, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, selected);

        const auto qualified_profile = selected->worlds().front().m_key;
        return prepareWorld(std::move(*selected), qualified_profile, seed, clock, execution);
    }

    Expected<PreparedWorld> prepareWorld(
        ContentCatalog catalog, const std::string_view profile, const u64 seed, const ITimerClock &clock,
        const sim::GenerationExecution &execution) {
        return prepareWorld(
            std::make_shared<const ContentCatalog>(std::move(catalog)), profile, seed, clock, execution);
    }

    Expected<PreparedWorld> prepareWorld(
        std::shared_ptr<const ContentCatalog> catalog, const std::string_view profile, const u64 seed,
        const ITimerClock &clock, const sim::GenerationExecution &execution) {
        if (execution.m_context != nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, execution.m_context->error());
        }

        if (not catalog or not catalog->contentFingerprint()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, std::errc::invalid_argument);
        }

        auto plan = compileWorldPlan(*catalog, profile, seed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, plan);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, validateRuntimeWorldParameters(plan->recipe()));

        auto world = sim::generateWorld(std::move(*plan), clock, execution);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, world);

        const WorldDescriptor descriptor{
            .m_profile = std::string{profile}, .m_seed = seed, .m_content_fingerprint = *catalog->contentFingerprint()};
        return PreparedWorldConstruction::make(std::move(catalog), descriptor, std::move(*world));
    }

    // ------------------------------------------------------------------
    // world restoration
    // ------------------------------------------------------------------

    Expected<PreparedWorld> restorePreparedWorld(
        ContentCatalog catalog, WorldDescriptor descriptor, sim::GeneratedWorld world) {
        auto restored = restorePreparedWorld(
            std::make_shared<const ContentCatalog>(std::move(catalog)), std::move(descriptor), std::move(world));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, restored);
        return restored;
    }

    Expected<PreparedWorld> restorePreparedWorld(
        std::shared_ptr<const ContentCatalog> catalog, WorldDescriptor descriptor, sim::GeneratedWorld world) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, validateWorldMetric(descriptor.m_metric));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, validateRuntimeWorldParameters(world.m_plan.recipe()));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreparation, catalog and
                                      descriptor.m_pack_schema_version == asset::Manifest::schema_version and
                                      descriptor.m_generator_version == sim::kGeneratorAlgorithmVersion and
                                      catalog->contentFingerprint() and
                                      *catalog->contentFingerprint() == descriptor.m_content_fingerprint and
                                      catalog->findWorld(descriptor.m_profile) and
                                      world.m_plan.seed() == descriptor.m_seed
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::protocol_not_supported));
        return PreparedWorldConstruction::make(std::move(catalog), std::move(descriptor), std::move(world));
    }
}
