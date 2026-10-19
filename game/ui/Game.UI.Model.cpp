module;

#include "pP/Macros.h"
#include "../colony/Colony.Elements.h"

module game.ui.model;

import engine.core;
import engine.sim;
import game.colony.content;
import game.colony.worldgen;
import std;

namespace pP::game::ui {
    namespace {
        // ------------------------------------------------------------------
        // queried cell presentation
        // ------------------------------------------------------------------

        void captureCell(
            colony::ColonyCellPresentation &out, const colony::ColonyDriver &driver, const sim::GlobalCellPos position,
            const bool metadata) {
            out.m_features.clear();
            out.m_particle.reset();
            out.m_nearest.reset();
            out.m_material = {};
            out.m_biome = {};
            out.m_position = position;
            out.m_temperature_k = 0.0f;
            out.m_material_id = 0u;
            out.m_generated = false;
            out.m_presence = colony::ECellPresence::unavailable;

            const auto &grid = driver.grid();
            if (position.m_x >= sim::kWorldEdge or position.m_y >= sim::kWorldEdge) {
                return;
            }

            if (not grid.isResident(sim::chunkOf(position))) {
                out.m_presence = colony::ECellPresence::unresident;
            } else {
                const auto cell = grid.getCell(position);
                if (cell) {
                    out.m_material_id = cell->m_element;
                    out.m_temperature_k = cell->m_temperature;
                    out.m_presence = cell->m_element == colony::kElementVacuum ? colony::ECellPresence::vacuum
                                                                               : colony::ECellPresence::material;
                    if (const auto *const catalog = driver.contentCatalog()) {
                        if (const auto *const material = catalog->findMaterial(cell->m_element)) {
                            out.m_material = material->m_display_name;
                        } else if (cell->m_element != colony::kElementVacuum) {
                            out.m_presence = colony::ECellPresence::invalid_material;
                        }
                    } else {
                        switch (cell->m_element) {
                            case colony::kElementVacuum: out.m_material = "Vacuum"; break;
                            case colony::kElementRock: out.m_material = "Rock"; break;
                            case colony::kElementIce: out.m_material = "Ice"; break;
                            case colony::kElementCaustic: out.m_material = "Caustic"; break;
                            case colony::kElementOil: out.m_material = "Oil"; break;
                            case colony::kElementRuins: out.m_material = "Ruins"; break;
                            case colony::kElementAbyssal: out.m_material = "Abyssal"; break;
                            case colony::kElementVent: out.m_material = "Vent"; break;
                            default: out.m_material = "Unknown material"; break;
                        }
                    }
                }
            }

            if (not metadata) {
                return;
            }

            const float target_x = (static_cast<float>(position.m_x) + 0.5f) /
                                   static_cast<float>(driver.cellsPerMetre());
            const float target_y = (static_cast<float>(position.m_y) + 0.5f) /
                                   static_cast<float>(driver.cellsPerMetre());
            float nearest_squared = 64.0f;
            for (const colony::AgentSummary &agent: driver.agentSummaries()) {
                const float dx = agent.m_x - target_x;
                const float dy = agent.m_y - target_y;
                const float squared = dx * dx + dy * dy;
                if (squared <= nearest_squared and (not out.m_nearest or squared < nearest_squared)) {
                    nearest_squared = squared;
                    out.m_nearest = agent;
                }
            }

            const auto *const prepared = driver.preparedWorld();
            if (prepared == nullptr) {
                return;
            }

            out.m_generated = true;
            const auto &world = prepared->generated();
            const auto &catalog = prepared->catalog();
            const auto &recipe = world.m_plan.recipe();
            const double x_m = (static_cast<double>(position.m_x) + 0.5) * recipe.m_cell_size_m;
            const double y_m = (static_cast<double>(position.m_y) + 0.5) * recipe.m_cell_size_m;
            const u32 biome = sim::generationBiomeAt(world.m_plan, world.m_layout, x_m, y_m);
            if (biome < catalog.biomes().size()) {
                out.m_biome = catalog.biomes()[biome].m_display_name;
            }

            for (const auto &instance: world.m_geometry.m_instances) {
                if (instance.m_feature >= recipe.m_features.size() or instance.m_feature >= catalog.features().size()) {
                    continue;
                }

                const auto bounds = recipe.m_features[instance.m_feature].m_bounds;
                const i64 x = static_cast<i64>(position.m_x) - instance.m_origin_x;
                const i64 y = static_cast<i64>(position.m_y) - instance.m_origin_y;
                if (x >= bounds.m_min_x and x < bounds.m_max_x and y >= bounds.m_min_y and y < bounds.m_max_y) {
                    out.m_features.push_back(catalog.features()[instance.m_feature].m_display_name);
                }
            }

            for (const auto &particle: driver.particleChunk(sim::chunkOf(position))) {
                const u32 x = static_cast<u32>(particle.m_x_m / recipe.m_cell_size_m);
                const u32 y = static_cast<u32>(particle.m_y_m / recipe.m_cell_size_m);
                if (x != position.m_x or y != position.m_y) {
                    continue;
                }

                const auto *const material = catalog.findMaterial(particle.m_material);
                out.m_particle = colony::ColonyParticlePresentation{
                    .m_material = material ? std::string_view{material->m_display_name}
                                           : std::string_view{"Unknown material"},
                    .m_id = particle.m_id,
                    .m_temperature_k = particle.m_temperature_k,
                    .m_volume_m3 = particle.m_volume_m3,
                    .m_mass_kg = particle.m_mass_kg};
                break;
            }
        }
    }

    // ------------------------------------------------------------------
    // frame-local driver snapshot
    // ------------------------------------------------------------------

    void captureColonyPresentation(
        colony::ColonyPresentation &out, const colony::ColonyDriver &driver,
        const std::optional<sim::GlobalCellPos> hover, const std::optional<sim::GlobalCellPos> inspected) {
        const hal::ProfileScope scope{"Game.UI.Model"};
        out.m_seed = driver.seed();
        out.m_tick_count = driver.tickCount();
        out.m_simulated_ms = driver.simMs();
        out.m_speed = driver.speed();
        out.m_paused = driver.paused();
        out.m_resident_chunks = driver.grid().residentChunkCount();
        out.m_agents = driver.agentSummaries();
        out.m_has_hover = hover.has_value();
        out.m_has_inspected = inspected.has_value();

        if (hover) {
            captureCell(out.m_hover, driver, *hover, false);
        }

        if (inspected) {
            captureCell(out.m_inspected, driver, *inspected, true);
        } else {
            out.m_inspected.m_features.clear();
            out.m_inspected.m_particle.reset();
            out.m_inspected.m_nearest.reset();
        }

        out.m_world = {};
        const auto *const prepared = driver.preparedWorld();
        if (prepared == nullptr) {
            return;
        }

        const auto &descriptor = prepared->descriptor();
        const auto &world = prepared->generated();
        auto &summary = out.m_world;
        summary.m_pack = prepared->catalog().packId();
        summary.m_profile = descriptor.m_profile;
        summary.m_seed = descriptor.m_seed;
        summary.m_content_fingerprint = descriptor.m_content_fingerprint;
        summary.m_generator_version = descriptor.m_generator_version;
        summary.m_retained_bytes = world.m_report.m_retained_known_payload_bytes;
        summary.m_sites = world.m_layout.sites().size();
        summary.m_features = world.m_geometry.m_instances.size();
        summary.m_matter_regions = world.m_matter.m_regions.size();
        summary.m_particles = driver.particleCount();
        summary.m_available = true;

        for (std::size_t index = 0u; index < summary.m_phase_seconds.size(); ++index) {
            summary.m_phase_seconds[index] = time::seconds(world.m_report.m_phase_times[index]);
            summary.m_scratch_bytes[index] = world.m_report.m_known_phase_scratch_bytes[index];
        }
    }
}
