module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <cstdio>

module game.tests;

import engine.app;
import engine.asset;
import engine.core;
import engine.sim;
import game.colony.checkpoint;
import game.colony.content;
import game.colony.driver;
import game.colony.matter;
import game.colony.metric;
import game.colony.worldgen;
import game.config;
import std;

namespace pP::game::tests::detail::lighting_lab {
    // ------------------------------------------------------------------
    // production lab preparation
    // ------------------------------------------------------------------

    using namespace colony;

    [[nodiscard]] std::filesystem::path productionRoot() {
        return hal::process::currentExecutablePath().parent_path() / "worldgen" / "default";
    }

    [[nodiscard]] Expected<PreparedWorld> prepareLab(const u64 seed = 42u) {
        config::Diagnostics diagnostics{64u};
        return prepareWorld(
            productionRoot(), "lighting_lab", seed, ITimerClock::steady(), diagnostics, {}, kLightingLabEdge);
    }

    [[nodiscard]] u16 materialId(const PreparedWorld &world, const std::string_view key) {
        const auto *const material = world.catalog().findMaterial(key);
        PPR_TEST_ASSERT(material != nullptr);
        return material->m_numeric_id;
    }

    [[nodiscard]] sim::Cell cellAt(const PreparedWorld &world, const u32 x, const u32 y) {
        const auto cell = world.generated().m_grid.getCell({x, y});
        PPR_TEST_ASSERT(cell.has_value());
        return *cell;
    }

    void expectCell(const PreparedWorld &world, const u32 x, const u32 y, const u16 material, const float temperature) {
        const auto cell = cellAt(world, x, y);
        PPR_TEST_ASSERT(cell.m_element == material);
        PPR_TEST_ASSERT(std::abs(cell.m_temperature - temperature) < 1.0e-3f);
    }

    void expectVacuum(const PreparedWorld &world, const u32 x, const u32 y) {
        PPR_TEST_ASSERT(cellAt(world, x, y).m_element == 0u);
    }

    [[nodiscard]] std::vector<sim::MatterParticle> sortedParticles(const sim::MatterParticleSnapshot &snapshot) {
        std::vector<sim::MatterParticle> result;
        for (const auto &batch: snapshot.m_chunks) {
            for (const auto &particle: batch.records()) {
                result.push_back(particle);
            }
        }
        std::ranges::sort(result, {}, &sim::MatterParticle::m_id);
        return result;
    }

    // ------------------------------------------------------------------
    // exact recipe and material authoring
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (recipe_rooms_apertures_swatches_and_material_values_are_exact) {
        auto prepared = prepareLab();
        PPR_TEST_ASSERT(prepared.has_value());
        const auto &world = *prepared;
        PPR_TEST_ASSERT(isLightingLabProfile(world.descriptor().m_profile));
        PPR_TEST_ASSERT(
            world.descriptor().m_profile.ends_with(":lighting_lab") or world.descriptor().m_profile == "lighting_lab");
        PPR_TEST_ASSERT(world.descriptor().m_world_edge == kLightingLabEdge);
        PPR_TEST_ASSERT(world.descriptor().m_metric.m_cells_per_metre == kFineCellsPerMetre);

        const u16 wall = materialId(world, "lab_wall");
        const u16 red = materialId(world, "lab_red");
        const u16 green = materialId(world, "lab_green");
        const u16 emitter = materialId(world, "lab_emitter");
        const u16 glass = materialId(world, "lab_glass");
        const u16 hot = materialId(world, "lab_hot_metal");
        const u16 pbr[4]{
            materialId(world, "lab_pbr_0"), materialId(world, "lab_pbr_1"), materialId(world, "lab_pbr_2"),
            materialId(world, "lab_pbr_3")};

        // Sealed dark room: two-cell shell, empty interior.
        expectCell(world, 32u, 768u, wall, 300.0f);
        expectCell(world, 33u, 943u, wall, 300.0f);
        expectCell(world, 239u, 768u, wall, 300.0f);
        expectVacuum(world, 34u, 770u);
        expectVacuum(world, 237u, 941u);

        // Matching roof aperture: 16 x 2 cells removed from the roof only.
        expectVacuum(world, 368u, 942u);
        expectVacuum(world, 383u, 943u);
        expectCell(world, 367u, 942u, wall, 300.0f);
        expectCell(world, 384u, 942u, wall, 300.0f);
        expectVacuum(world, 368u, 941u);

        // Emitter room: one authored 8 x 8 emitter block.
        expectCell(world, 536u, 844u, emitter, 300.0f);
        expectCell(world, 543u, 851u, emitter, 300.0f);
        expectVacuum(world, 544u, 851u);
        expectVacuum(world, 536u, 852u);

        // Colored L: lower corridor, vertical leg, corner mass and colored walls.
        expectVacuum(world, 754u, 770u);
        expectVacuum(world, 957u, 817u);
        expectVacuum(world, 910u, 941u);
        expectCell(world, 909u, 941u, wall, 300.0f);
        expectCell(world, 800u, 900u, wall, 300.0f);
        expectCell(world, 800u, 818u, red, 300.0f);
        expectCell(world, 908u, 819u, red, 300.0f);
        expectCell(world, 908u, 820u, green, 300.0f);
        expectCell(world, 909u, 919u, green, 300.0f);
        expectCell(world, 772u, 788u, emitter, 300.0f);
        expectCell(world, 779u, 795u, emitter, 300.0f);

        // Matched doors: identical shells/sources, only the doorway differs.
        for (const auto &origin: {std::pair{32u, 512u}, std::pair{272u, 512u}, std::pair{512u, 512u}}) {
            const u32 x = origin.first;
            const u32 y = origin.second;
            expectCell(world, x + 103u, y + 2u, wall, 300.0f);
            expectCell(world, x + 104u, y + 173u, wall, 300.0f);
            expectCell(world, x + 24u, y + 76u, emitter, 300.0f);
            expectCell(world, x + 31u, y + 83u, emitter, 300.0f);
            expectVacuum(world, x + 102u, y + 80u);
            expectVacuum(world, x + 105u, y + 80u);
        }
        expectVacuum(world, 135u, 576u);
        expectVacuum(world, 136u, 607u);
        expectCell(world, 375u, 576u, wall, 300.0f);
        expectCell(world, 376u, 607u, wall, 300.0f);
        expectCell(world, 615u, 576u, glass, 300.0f);
        expectCell(world, 616u, 607u, glass, 300.0f);

        // Separate one-cell enclosure with an exterior emitter.
        expectCell(world, 752u, 512u, wall, 300.0f);
        expectVacuum(world, 753u, 513u);
        expectCell(world, 752u, 687u, wall, 300.0f);
        expectCell(world, 959u, 687u, wall, 300.0f);
        expectCell(world, 740u, 588u, emitter, 300.0f);
        expectCell(world, 743u, 603u, emitter, 300.0f);
        expectVacuum(world, 751u, 588u);

        // Chunk-crossing barrier cells and the local source.
        expectCell(world, 127u, 300u, wall, 300.0f);
        expectCell(world, 128u, 423u, wall, 300.0f);
        expectCell(world, 224u, 383u, wall, 300.0f);
        expectCell(world, 415u, 384u, wall, 300.0f);
        expectCell(world, 64u, 328u, emitter, 300.0f);

        // Basins: two-cell U shells with no roof.
        for (const auto &basin: {std::pair{32u, 32u}, std::pair{272u, 32u}, std::pair{512u, 32u}}) {
            const u32 x = basin.first;
            const u32 y = basin.second;
            expectCell(world, x, y, wall, 300.0f);
            expectVacuum(world, x + 2u, y + 2u);
            expectCell(world, x, y + 175u, wall, 300.0f);
            expectVacuum(world, x + 100u, y + 174u);
            expectVacuum(world, x + 100u, y + 175u);
        }

        // Thermal and PBR swatches, slab and floor.
        expectCell(world, 512u, 256u, wall, 300.0f);
        expectCell(world, 959u, 257u, wall, 300.0f);
        expectVacuum(world, 512u, 258u);
        constexpr std::array<float, 5u> hot_temperatures{300.0f, 1500.0f, 3000.0f, 6500.0f, 10000.0f};
        for (u32 index = 0u; index < hot_temperatures.size(); ++index) {
            expectCell(world, 528u + 80u * index, 288u, hot, hot_temperatures[index]);
            expectCell(world, 543u + 80u * index, 303u, hot, hot_temperatures[index]);
            expectVacuum(world, 544u + 80u * index, 303u);
        }
        for (u32 index = 0u; index < 4u; ++index) {
            expectCell(world, 528u + 104u * index, 368u, pbr[index], 300.0f);
            expectCell(world, 551u + 104u * index, 391u, pbr[index], 300.0f);
            expectVacuum(world, 552u + 104u * index, 391u);
        }
        expectCell(world, 0u, 0u, wall, 300.0f);
        expectCell(world, 1023u, 1u, wall, 300.0f);
        expectVacuum(world, 0u, 2u);
        expectVacuum(world, 512u, 1023u);

        // Normal controlled-actor start above the floor in the open passage.
        const double cell_metres = kFineCellMetres;
        PPR_TEST_ASSERT(std::abs(world.generated().m_start.m_spawn_x_m - 744.5 * cell_metres) < 1.0e-9);
        PPR_TEST_ASSERT(std::abs(world.generated().m_start.m_spawn_y_m - 16.5 * cell_metres) < 1.0e-9);
        PPR_TEST_ASSERT(std::abs(world.generated().m_start.m_floor_y_m - 2.0 * cell_metres) < 1.0e-9);

        // Material authoring values from the design specification.
        const auto &catalog = world.catalog();
        const auto *const wall_material = catalog.findMaterial("lab_wall");
        const auto *const red_material = catalog.findMaterial("lab_red");
        const auto *const green_material = catalog.findMaterial("lab_green");
        const auto *const emitter_material = catalog.findMaterial("lab_emitter");
        const auto *const glass_material = catalog.findMaterial("lab_glass");
        const auto *const hot_material = catalog.findMaterial("lab_hot_metal");
        const auto *const magma_material = catalog.findMaterial("lab_magma");
        PPR_TEST_ASSERT(
            wall_material and
            red_material and
            green_material and
            emitter_material and
            glass_material and
            hot_material and
            magma_material);

        PPR_TEST_ASSERT((wall_material->m_color_rgba >> 24u & 0xffu) == 0x99u);
        PPR_TEST_ASSERT(wall_material->m_metallic == 0.0 and wall_material->m_roughness == 0.8);
        PPR_TEST_ASSERT(wall_material->m_ior == 1.5 and wall_material->m_alpha_mode == EMaterialAlphaMode::opaque);
        PPR_TEST_ASSERT(wall_material->m_albedo_texture and wall_material->m_albedo_texture->ends_with(":lab_white"));
        PPR_TEST_ASSERT(
            wall_material->m_normal_texture and wall_material->m_normal_texture->ends_with(":lab_flat_normal"));
        PPR_TEST_ASSERT(wall_material->m_emissive_strength == 1.0 and wall_material->m_emissive_rgb[0u] == 0.0);

        PPR_TEST_ASSERT((red_material->m_color_rgba >> 24u & 0xffu) == 0xb3u);
        PPR_TEST_ASSERT((red_material->m_color_rgba >> 16u & 0xffu) == 0x0du);
        PPR_TEST_ASSERT((green_material->m_color_rgba >> 16u & 0xffu) == 0xb3u);
        PPR_TEST_ASSERT((green_material->m_color_rgba >> 24u & 0xffu) == 0x0du);

        PPR_TEST_ASSERT(emitter_material->m_emissive_rgb == std::array{1.0, 0.7, 0.35});
        PPR_TEST_ASSERT(emitter_material->m_emissive_strength == 8.0);
        PPR_TEST_ASSERT(
            emitter_material->m_numeric_id == emitter and
            wall_material->m_numeric_id == wall and
            red_material->m_numeric_id == red and
            green_material->m_numeric_id == green);

        PPR_TEST_ASSERT(glass_material->m_alpha_mode == EMaterialAlphaMode::blend);
        PPR_TEST_ASSERT((glass_material->m_color_rgba & 0xffu) == 0x40u);
        PPR_TEST_ASSERT(glass_material->m_roughness == 0.1 and glass_material->m_metallic == 0.0);
        PPR_TEST_ASSERT(glass_material->m_ior == 1.5);
        PPR_TEST_ASSERT(glass_material->m_transport.has_value());
        PPR_TEST_ASSERT(glass_material->m_transport->m_model == EMaterialOpticalModel::absorbing);
        PPR_TEST_ASSERT(
            glass_material->m_transport->m_sigma_r_per_m == 0.0 and
            glass_material->m_transport->m_sigma_g_per_m == 0.0 and
            glass_material->m_transport->m_sigma_b_per_m == 0.0);

        PPR_TEST_ASSERT(hot_material->m_metallic == 1.0 and hot_material->m_roughness == 0.4);
        PPR_TEST_ASSERT((hot_material->m_color_rgba >> 24u & 0xffu) == 0xb3u);

        constexpr std::array<std::pair<double, double>, 4u> pbr_values{
            std::pair{0.2, 0.0}, std::pair{0.8, 0.0}, std::pair{0.2, 1.0}, std::pair{0.8, 1.0}};
        for (u32 index = 0u; index < pbr_values.size(); ++index) {
            const auto *const material = catalog.findMaterial("lab_pbr_" + std::to_string(index));
            PPR_TEST_ASSERT(material != nullptr);
            PPR_TEST_ASSERT(std::abs(material->m_roughness - pbr_values[index].first) < 1.0e-12);
            PPR_TEST_ASSERT(std::abs(material->m_metallic - pbr_values[index].second) < 1.0e-12);
            PPR_TEST_ASSERT(material->m_albedo_texture and material->m_albedo_texture->ends_with(":bay_tread_albedo"));
            PPR_TEST_ASSERT(material->m_normal_texture and material->m_normal_texture->ends_with(":bay_tread_normal"));
            PPR_TEST_ASSERT(material->m_texture_period_m == 1.0 and material->m_normal_scale == 1.0);
            PPR_TEST_ASSERT((material->m_color_rgba >> 24u & 0xffu) == 0xffu);
        }

        PPR_TEST_ASSERT(magma_material->m_representation == EMaterialRepresentation::particle);
        PPR_TEST_ASSERT(magma_material->m_emissive_rgb == std::array{1.0, 0.25, 0.02});
        PPR_TEST_ASSERT(magma_material->m_emissive_strength == 8.0);
        PPR_TEST_ASSERT(magma_material->m_transport.has_value());
        PPR_TEST_ASSERT(magma_material->m_transport->m_model == EMaterialOpticalModel::opaque);
    };

    // ------------------------------------------------------------------
    // particle seeds
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (basins_and_gas_seed_exact_particle_columns) {
        auto prepared = prepareLab();
        PPR_TEST_ASSERT(prepared.has_value());
        const auto &world = *prepared;
        const auto &recipe = world.generated().m_plan.recipe();
        auto seeded = sim::seedMatterParticles(world.generated().m_matter, world.generated().m_grid, recipe);
        PPR_TEST_ASSERT(seeded.has_value());

        const auto particles = sortedParticles(*seeded);
        PPR_TEST_ASSERT(particles.size() == 3u * 3072u + 64u);
        PPR_TEST_ASSERT(seeded->m_chunks.size() > 0u);

        const u16 sand = materialId(world, "sand");
        const u16 water = materialId(world, "water");
        const u16 magma = materialId(world, "lab_magma");
        const u16 oxygen = materialId(world, "oxygen");
        const double cell = recipe.m_cell_size_m;
        const double sub_volume = cell * cell * recipe.m_reference_thickness_m / 4.0;
        constexpr std::array<double, 4u> offsets_x{0.25, 0.75, 0.25, 0.75};
        constexpr std::array<double, 4u> offsets_y{0.25, 0.25, 0.75, 0.75};

        std::size_t cursor = 0u;
        const std::array<u16, 3u> basin_materials{sand, water, magma};
        const std::array<float, 3u> basin_temperatures{300.0f, 300.0f, 1800.0f};
        const std::array<std::pair<u32, u32>, 3u> basin_origins{
            std::pair{32u, 32u}, std::pair{272u, 32u}, std::pair{512u, 32u}};
        u64 expected_id = 1u;
        for (u32 basin = 0u; basin < basin_origins.size(); ++basin) {
            const auto [origin_x, origin_y] = basin_origins[basin];
            for (u32 row = 0u; row < 16u; ++row) {
                for (u32 column = 0u; column < 48u; ++column) {
                    for (u32 sub = 0u; sub < 4u; ++sub) {
                        const auto &particle = particles[cursor++];
                        PPR_TEST_ASSERT(particle.m_id == expected_id++);
                        PPR_TEST_ASSERT(particle.m_material == basin_materials[basin]);
                        PPR_TEST_ASSERT(
                            std::abs(particle.m_temperature_k - static_cast<double>(basin_temperatures[basin])) <
                            1.0e-9);
                        PPR_TEST_ASSERT(std::abs(particle.m_volume_m3 - sub_volume) < 1.0e-9);
                        const double expected_x = (static_cast<double>(origin_x + 8u + column) + offsets_x[sub]) * cell;
                        const double expected_y = (static_cast<double>(origin_y + 80u + row) + offsets_y[sub]) * cell;
                        PPR_TEST_ASSERT(std::abs(particle.m_x_m - expected_x) < 1.0e-12);
                        PPR_TEST_ASSERT(std::abs(particle.m_y_m - expected_y) < 1.0e-12);
                        PPR_TEST_ASSERT(
                            particle.m_phase == sim::EGenerationMatter::solid or
                            particle.m_phase == sim::EGenerationMatter::liquid);
                    }
                }
            }
        }

        const double gas_volume = cell * cell * recipe.m_reference_thickness_m;
        for (u32 row = 0u; row < 8u; ++row) {
            for (u32 column = 0u; column < 8u; ++column) {
                const auto &particle = particles[cursor++];
                PPR_TEST_ASSERT(particle.m_id == expected_id++);
                PPR_TEST_ASSERT(particle.m_material == oxygen);
                PPR_TEST_ASSERT(particle.m_phase == sim::EGenerationMatter::gas);
                PPR_TEST_ASSERT(std::abs(particle.m_temperature_k - 300.0) < 1.0e-9);
                PPR_TEST_ASSERT(std::abs(particle.m_volume_m3 - gas_volume) < 1.0e-12);
                PPR_TEST_ASSERT(std::abs(particle.m_x_m - (776.0 + 2.0 * column + 0.5) * cell) < 1.0e-12);
                PPR_TEST_ASSERT(std::abs(particle.m_y_m - (80.0 + 2.0 * row + 0.5) * cell) < 1.0e-12);
            }
        }
        PPR_TEST_ASSERT(cursor == particles.size());

        std::vector<u64> ids;
        ids.reserve(particles.size());
        for (const auto &particle: particles) {
            ids.push_back(particle.m_id);
        }
        PPR_TEST_ASSERT(std::ranges::adjacent_find(ids) == ids.end());

        // The ordinary one-cell guard rejects the four-fold basin cells;
        // admitted moving inventory accepts them.
        PPR_TEST_ASSERT(
            sim::validateMovingMatterParticles(
                *seeded, recipe.m_materials, recipe.m_cell_size_m, nullptr, recipe.m_reference_thickness_m,
                recipe.m_world_edge) == std::error_code{});
        PPR_TEST_ASSERT(
            sim::validateMatterParticles(
                *seeded, recipe.m_materials, recipe.m_cell_size_m, nullptr, recipe.m_reference_thickness_m,
                recipe.m_world_edge) != std::error_code{});
    };

    // ------------------------------------------------------------------
    // determinism
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (grid_matter_and_start_are_deterministic_across_seeds) {
        auto first = prepareLab(42u);
        auto second = prepareLab(7u);
        PPR_TEST_ASSERT(first.has_value() and second.has_value());
        const auto &a = *first;
        const auto &b = *second;
        PPR_TEST_ASSERT(a.descriptor().m_seed == 42u and b.descriptor().m_seed == 7u);

        const auto &grid_a = a.generated().m_grid;
        const auto &grid_b = b.generated().m_grid;
        for (u32 cy = 0u; cy < grid_a.chunksPerEdge(); ++cy) {
            for (u32 cx = 0u; cx < grid_a.chunksPerEdge(); ++cx) {
                const auto cells_a = grid_a.residentCells({cx, cy});
                const auto cells_b = grid_b.residentCells({cx, cy});
                PPR_TEST_ASSERT(cells_a.has_value() == cells_b.has_value());
                if (cells_a) {
                    PPR_TEST_ASSERT(std::ranges::equal(*cells_a, *cells_b));
                }
            }
        }

        PPR_TEST_ASSERT(a.generated().m_matter.m_regions.size() == b.generated().m_matter.m_regions.size());
        PPR_TEST_ASSERT(a.generated().m_matter.m_span_count == b.generated().m_matter.m_span_count);
        for (std::size_t index = 0u; index < a.generated().m_matter.m_regions.size(); ++index) {
            const auto &region_a = a.generated().m_matter.m_regions[index];
            const auto &region_b = b.generated().m_matter.m_regions[index];
            PPR_TEST_ASSERT(region_a.m_identity == region_b.m_identity);
            PPR_TEST_ASSERT(region_a.m_material == region_b.m_material);
            PPR_TEST_ASSERT(region_a.m_temperature_k == region_b.m_temperature_k);
            PPR_TEST_ASSERT(region_a.m_subdivision == region_b.m_subdivision);
            PPR_TEST_ASSERT(std::ranges::equal(region_a.m_spans, region_b.m_spans));
            PPR_TEST_ASSERT(region_a.m_volume_m3 == region_b.m_volume_m3);
            PPR_TEST_ASSERT(region_a.m_mass_kg == region_b.m_mass_kg);
        }

        PPR_TEST_ASSERT(
            a.generated().m_start.m_spawn_x_m == b.generated().m_start.m_spawn_x_m and
            a.generated().m_start.m_spawn_y_m == b.generated().m_start.m_spawn_y_m and
            a.generated().m_start.m_floor_y_m == b.generated().m_start.m_floor_y_m);
        PPR_TEST_ASSERT(a.generated().m_report.m_terrain_checksum == b.generated().m_report.m_terrain_checksum);
        // The output checksum records the explicitly requested seed identity.
        PPR_TEST_ASSERT(a.generated().m_report.m_output_checksum != b.generated().m_report.m_output_checksum);
        PPR_TEST_ASSERT(a.generated().m_plan.seed() == 42u and b.generated().m_plan.seed() == 7u);
    };

    // ------------------------------------------------------------------
    // checkpoint roundtrip
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (checkpoint_roundtrip_preserves_dug_rebuilt_and_evolved_state) {
        auto prepared = prepareLab();
        PPR_TEST_ASSERT(prepared.has_value());
        const auto catalog = prepared->catalogOwner();
        const u16 wall = materialId(*prepared, "lab_wall");

        auto driver = std::make_unique<ColonyDriver>();
        PPR_TEST_ASSERT(not driver->initPrepared(
            {.m_seed = 42u, .m_agent_count = 1u, .m_world_generation = 1u, .m_world_edge = kLightingLabEdge},
            std::move(*prepared)));
        PPR_DEFER {
            PPR_TEST_ASSERT(not driver->shutdown());
        };
        PPR_TEST_ASSERT(driver->particleCount() == 3u * 3072u + 64u);
        PPR_TEST_ASSERT(driver->initialMatterBaselineValid());

        // Ordinary dig of one half of the closed doorway, then settle.
        driver->requireEditPublication();
        constexpr sim::GlobalCellPos doorway_min{375u, 576u};
        PPR_TEST_ASSERT(not driver->requestDig(doorway_min, 2u, 16u));
        const auto period = TimeSpan{std::chrono::seconds{1}} / 60u;
        PPR_TEST_ASSERT(not driver->update(period * 3u));
        for (u32 batch = 0u; batch < 256u and not driver->editReadyForPresentation(); ++batch) {
            PPR_TEST_ASSERT(not driver->update(period * 5u));
        }
        PPR_TEST_ASSERT(driver->editReadyForPresentation());
        Array<ColonyDriver::DirtyChunk> manifest(driver->editManifest().begin(), driver->editManifest().end());
        PPR_TEST_ASSERT(not driver->settleEdit(manifest));
        for (u32 slice = 0u; slice < 64u and not driver->captureReady(); ++slice) {
            PPR_TEST_ASSERT(not driver->update(period));
        }
        PPR_TEST_ASSERT(driver->captureReady());
        PPR_TEST_ASSERT(driver->grid().getCell(doorway_min)->m_element == 0u);
        PPR_TEST_ASSERT(driver->grid().getCell({376u, 591u})->m_element == 0u);

        auto checkpoint = captureWorldCheckpoint(*driver, driver->preparedWorld()->generated());
        PPR_TEST_ASSERT(checkpoint.has_value());
        const auto initial_particles = sortedParticles(checkpoint->m_particles);

        auto bytes = encodeWorldCheckpoint(*checkpoint, *catalog);
        PPR_TEST_ASSERT(bytes.has_value());
        auto decoded = decodeWorldCheckpoint(*bytes, *catalog);
        PPR_TEST_ASSERT(decoded.has_value());
        auto restored = restoreWorldCheckpoint(catalog, std::move(*decoded), 2u);
        PPR_TEST_ASSERT(restored.has_value());
        auto &dug = **restored;
        PPR_DEFER {
            PPR_TEST_ASSERT(not dug.shutdown());
        };
        PPR_TEST_ASSERT(dug.grid().getCell(doorway_min)->m_element == 0u);
        PPR_TEST_ASSERT(dug.grid().getCell({376u, 591u})->m_element == 0u);
        const auto restored_particles = sortedParticles(*dug.captureParticles());
        PPR_TEST_ASSERT(restored_particles.size() == initial_particles.size());
        for (std::size_t index = 0u; index < restored_particles.size(); ++index) {
            PPR_TEST_ASSERT(
                restored_particles[index].m_id == initial_particles[index].m_id and
                restored_particles[index].m_x_m == initial_particles[index].m_x_m and
                restored_particles[index].m_y_m == initial_particles[index].m_y_m and
                restored_particles[index].m_temperature_k == initial_particles[index].m_temperature_k);
        }

        // Rebuild the same doorway through the ordinary grid edit primitive.
        for (u32 y = doorway_min.m_y; y < doorway_min.m_y + 16u; ++y) {
            for (u32 x = doorway_min.m_x; x < doorway_min.m_x + 2u; ++x) {
                PPR_TEST_ASSERT(not dug.grid().setCell({x, y}, {wall, 300.0f}));
            }
        }
        PPR_TEST_ASSERT(dug.grid().getCell(doorway_min)->m_element == wall);

        auto rebuilt = captureWorldCheckpoint(dug, dug.preparedWorld()->generated());
        PPR_TEST_ASSERT(rebuilt.has_value());

        // Stored evolved state: displace and heat a deterministic subset through
        // the same moving-particle columns a settled tick would publish.
        sim::MatterParticleSnapshot evolved{.m_next_id = rebuilt->m_particles.m_next_id};
        for (const auto &batch: rebuilt->m_particles.m_chunks) {
            Array<sim::MatterParticle> records;
            records.reserve(batch.records().size());
            for (auto particle: batch.records()) {
                if (particle.m_id % 257u == 0u) {
                    particle.m_x_m += 0.05;
                    particle.m_y_m += 0.02;
                    particle.m_temperature_k = 450.0;
                }
                records.push_back(particle);
            }
            evolved.m_chunks.push_back(
                {batch.m_position, std::make_shared<const Array<sim::MatterParticle>>(std::move(records))});
        }
        auto evolved_matter = makeMatterState(
            evolved, kFineCellMetres, kReferenceThicknessMetres, kMatterGravityY, 2u, kLightingLabEdge);
        PPR_TEST_ASSERT(evolved_matter.has_value());
        evolved_matter->m_identity.m_terrain_revision = 1u;
        auto expected = sortedParticles(evolved);
        rebuilt->m_particles = evolved;
        rebuilt->m_current_matter = std::move(*evolved_matter);

        auto rebuilt_bytes = encodeWorldCheckpoint(*rebuilt, *catalog);
        PPR_TEST_ASSERT(rebuilt_bytes.has_value());
        auto rebuilt_decoded = decodeWorldCheckpoint(*rebuilt_bytes, *catalog);
        PPR_TEST_ASSERT(rebuilt_decoded.has_value());
        auto evolved_restored = restoreWorldCheckpoint(catalog, std::move(*rebuilt_decoded), 3u);
        PPR_TEST_ASSERT(evolved_restored.has_value());
        auto &evolved_driver = **evolved_restored;
        PPR_DEFER {
            PPR_TEST_ASSERT(not evolved_driver.shutdown());
        };
        PPR_TEST_ASSERT(evolved_driver.grid().getCell(doorway_min)->m_element == wall);
        PPR_TEST_ASSERT(evolved_driver.grid().getCell({376u, 591u})->m_element == wall);

        auto current = evolved_driver.captureParticles();
        PPR_TEST_ASSERT(current.has_value());
        const auto current_particles = sortedParticles(*current);
        PPR_TEST_ASSERT(current_particles.size() == expected.size());
        for (std::size_t index = 0u; index < expected.size(); ++index) {
            PPR_TEST_ASSERT(current_particles[index].m_id == expected[index].m_id);
            PPR_TEST_ASSERT(current_particles[index].m_x_m == expected[index].m_x_m);
            PPR_TEST_ASSERT(current_particles[index].m_y_m == expected[index].m_y_m);
            PPR_TEST_ASSERT(current_particles[index].m_temperature_k == expected[index].m_temperature_k);
        }
        PPR_TEST_ASSERT(std::ranges::find_if(current_particles, [](const sim::MatterParticle &particle) {
            return particle.m_temperature_k == 450.0;
        }) != current_particles.end());
        PPR_TEST_ASSERT(std::ranges::find_if(current_particles, [](const sim::MatterParticle &particle) {
            return particle.m_temperature_k == 1800.0;
        }) != current_particles.end());
    };

    // ------------------------------------------------------------------
    // rendered admission
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (rendered_admission_rejects_unsupported_extents_before_preparation) {
        PPR_TEST_ASSERT(game::GameSettings{}.m_map_size == kRenderedWorldEdge);
        PPR_TEST_ASSERT(validateRenderedWorldEdge(kRenderedWorldEdge) == std::error_code{});
        PPR_TEST_ASSERT(validateRenderedWorldEdge(2048u) == std::errc::not_supported);
        PPR_TEST_ASSERT(validateRenderedWorldEdge(4096u) == std::errc::not_supported);
        // General sim/authoring extents stay available.
        PPR_TEST_ASSERT(sim::isSupportedWorldEdge(1024u));
        PPR_TEST_ASSERT(sim::isSupportedWorldEdge(2048u));
        PPR_TEST_ASSERT(sim::isSupportedWorldEdge(4096u));

        // A checkpoint descriptor carries the same rendered bound.
        PPR_TEST_ASSERT(
            validateRenderedWorldEdge(WorldDescriptor{.m_profile = "frontier", .m_world_edge = 4096u}.m_world_edge) ==
            std::errc::not_supported);
    };

    // ------------------------------------------------------------------
    // group
    // ------------------------------------------------------------------

} // namespace pP::game::tests::detail::lighting_lab

namespace pP::game::tests {
    const UnitTest lighting_lab = UnitTest::Named("lighting_lab") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::lighting_lab::recipe_rooms_apertures_swatches_and_material_values_are_exact,
                detail::lighting_lab::basins_and_gas_seed_exact_particle_columns,
                detail::lighting_lab::grid_matter_and_start_are_deterministic_across_seeds,
                detail::lighting_lab::checkpoint_roundtrip_preserves_dug_rebuilt_and_evolved_state,
                detail::lighting_lab::rendered_admission_rejects_unsupported_extents_before_preparation});
    };

    const UnitTest &gameLightingLabTests() noexcept {
        return lighting_lab;
    }
} // namespace pP::game::tests
