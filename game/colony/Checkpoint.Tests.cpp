module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <cstdio>

module game.tests;

import engine.asset;
import engine.core;
import engine.sim;
import engine.app;
import engine.image;
import engine.matter;
import engine.shader;
import engine.rhi;
import game.colony.matter;
import game.colony.metric;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import game.colony.checkpoint;
import game.colony.content;
import game.colony.driver;
import game.colony.digtool;
import game.colony.worldgen;
import game.colony.sim_packet;
import game.application;
import game.colony.panel;
import game.ui.model;
import std;

#include "Checkpoint.Tests.Support.h"

namespace pP::game::tests::detail::checkpoint {
    PPR_DEFINE_LOG_CATEGORY(CheckpointTests, debug, none)

    // ------------------------------------------------------------------
    // checkpoint restore, frozen capture and edit publication
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (terrain_masks_match_independent_scatter_for_residency_materials_seams_and_borders) {
        constexpr u32 edge = 1024u;
        constexpr u32 terrain_edge = edge + 1u;
        constexpr u32 absent = 65536u;
        constexpr std::array<u16, 4u> palette{0u, 1u, 32768u, 65535u};
        sim::ChunkGrid grid{edge};
        Array<u32> labels(static_cast<std::size_t>(edge) * edge, absent);

        const auto put = [&](const u32 x, const u32 y, const u16 material) {
            const sim::ChunkPos chunk = sim::chunkOf({x, y});
            if (not grid.isResident(chunk)) {
                for (u32 row = chunk.m_y * sim::kChunkEdge; row < (chunk.m_y + 1u) * sim::kChunkEdge; ++row) {
                    for (u32 column = chunk.m_x * sim::kChunkEdge; column < (chunk.m_x + 1u) * sim::kChunkEdge;
                        ++column) {
                        labels[static_cast<std::size_t>(row) * edge + column] = 0u;
                    }
                }
            }

            PPR_TEST_ASSERT(not grid.setCell({x, y}, {material, 300.0f}));
            labels[static_cast<std::size_t>(y) * edge + x] = material;
        };

        // Four distinct IDs occupy each contour corner, including a four-chunk
        // intersection. The distant row distinguishes compact and maximum stride.
        for (const auto [x, y]: std::array<std::pair<u32, u32>, 3u>{{{20u, 20u}, {127u, 127u}, {383u, 639u}}}) {
            put(x, y, palette[0u]);
            put(x + 1u, y, palette[1u]);
            put(x + 1u, y + 1u, palette[2u]);
            put(x, y + 1u, palette[3u]);
        }

        put(0u, 0u, palette[1u]);
        put(edge - 1u, 0u, palette[2u]);
        put(0u, edge - 1u, palette[3u]);
        put(edge - 1u, edge - 1u, palette[0u]);
        const u32 resident_count = grid.residentChunkCount();

        for (u32 membership = 0u; membership < 16u; ++membership) {
            Array<u16> materials;
            for (u32 index = 0u; index < palette.size(); ++index) {
                if ((membership & (1u << index)) != 0u) {
                    materials.push_back(palette[index]);
                }
            }

            Array<u32> expected(static_cast<std::size_t>(terrain_edge) * terrain_edge, 0u);
            for (u32 coordinate = 0u; coordinate < terrain_edge; ++coordinate) {
                expected[coordinate] |= 3u;
                expected[static_cast<std::size_t>(edge) * terrain_edge + coordinate] |= 12u;
                expected[static_cast<std::size_t>(coordinate) * terrain_edge] |= 9u;
                expected[static_cast<std::size_t>(coordinate) * terrain_edge + edge] |= 6u;
            }

            // Scatter each known occupied cell to its four neighboring vertices;
            // absent chunks never become material zero through a default Cell.
            for (u32 y = 0u; y < edge; ++y) {
                for (u32 x = 0u; x < edge; ++x) {
                    const u32 label = labels[static_cast<std::size_t>(y) * edge + x];
                    if (label == absent or std::ranges::find(materials, label) == materials.end()) {
                        continue;
                    }

                    const std::size_t vertex = static_cast<std::size_t>(y) * terrain_edge + x;
                    expected[vertex] |= 4u;
                    expected[vertex + 1u] |= 8u;
                    expected[vertex + terrain_edge] |= 2u;
                    expected[vertex + terrain_edge + 1u] |= 1u;
                }
            }

            const auto actual = makeMatterTerrain(grid, materials);
            PPR_TEST_ASSERT(actual.has_value());
            PPR_TEST_ASSERT(actual->size() == expected.size());
            PPR_TEST_ASSERT(std::ranges::equal(*actual, expected));
            PPR_TEST_ASSERT(grid.residentChunkCount() == resident_count);
        }
    };

    PPR_UNIT_TEST (destroyed_solids_use_calibrated_families_and_initial_history) {
        Fixture fixture{"destroyed_families", "frontier", 1024u, true, true};
        const auto source = editedCheckpoint(*fixture.m_catalog).m_current_matter;
        sim::ChunkGrid grid{1024u};
        using Family = matter::ESourceMaterial;
        const std::pair<const char *, Family> cases[]{
            {"sandstone", Family::sand}, {"obsidian", Family::elastic}, {"slime", Family::visco},
            {"clay", Family::cohesive}, {"snow", Family::compacting}, {"pumice", Family::compacting}};
        for (u32 index = 0u; index < std::size(cases); ++index) {
            const auto *const material = fixture.m_catalog->findMaterial(cases[index].first);
            PPR_TEST_ASSERT(material != nullptr and material->m_representation == EMaterialRepresentation::terrain);
            PPR_TEST_ASSERT(not grid.setCell({60u + index, 60u}, {material->m_numeric_id, 300.0f}));
        }

        auto candidate = grid.captureChunk({0u, 0u});
        PPR_TEST_ASSERT(candidate.has_value());
        for (u32 index = 0u; index < std::size(cases); ++index)
            PPR_TEST_ASSERT(not grid.setCell({60u + index, 60u}, {}));

        grid.exchangeChunk(*candidate);
        const std::array edits{std::move(*candidate)};
        const auto prepared = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(prepared and prepared->has_value());
        const auto &state = **prepared;
        PPR_TEST_ASSERT(state.m_particles.size() == source.m_particles.size() + std::size(cases));
        const auto indices = matterPropertyIndices(state);
        PPR_TEST_ASSERT(indices.has_value());

        for (u32 index = 0u; index < std::size(cases); ++index) {
            const auto offset = source.m_particles.size() + index;
            const auto [key, family] = cases[index];
            const auto &row = state.m_properties[(*indices)[offset]];
            const auto &particle = state.m_particles[offset];
            const auto *const material = fixture.m_catalog->findMaterial(key);
            PPR_TEST_ASSERT(state.m_records[offset].m_material == material->m_numeric_id);
            PPR_TEST_ASSERT(row.m_family == family and particle.m_material == static_cast<float>(family));
            PPR_TEST_ASSERT(
                static_cast<double>(row.m_reference_density) == material->m_reference_density_kg_m3 / 1000.0);
            const bool history = family == Family::cohesive or family == Family::compacting;
            const float phi = std::string_view{key} == "pumice" ? 0.4f : 0.3f;
            PPR_TEST_ASSERT(particle.m_log_jp == (history ? 0.0f : 1.0f));
            PPR_TEST_ASSERT(particle.m_compaction == 0.0f and particle.m_solid_fraction == phi);
            PPR_TEST_ASSERT(particle.m_displacement_x == 0.0f and particle.m_displacement_y == 0.0f);
            PPR_TEST_ASSERT(particle.m_liquid_density == 1.0f);
            PPR_TEST_ASSERT(row.m_solid_fraction0 == phi);

            if (family == Family::sand) {
                PPR_TEST_ASSERT(row.m_friction_angle == 34.0f and row.m_sand_yield_ratio == 1.1f);
            } else if (family == Family::elastic or family == Family::visco) {
                PPR_TEST_ASSERT(row.m_elasticity_ratio == 1.0f and row.m_elastic_relaxation == 1.5f);
                PPR_TEST_ASSERT(row.m_plasticity == (family == Family::visco ? 0.9f : 0.0f));
            } else if (family == Family::cohesive) {
                PPR_TEST_ASSERT(row.m_cohesion == 0.12f and row.m_cap == 0.5f and row.m_cohesion_hardening == 0.4f);
            }
        }

        // Reseeding through the production snapshot adapter must use the same phi0,
        // especially pumice's 0.4 rather than the generic particle's 0.3.
        const auto snapshot = captureMovingMatter(state, state.m_particles);
        PPR_TEST_ASSERT(snapshot.has_value());
        const auto seeded = makeMatterState(
            *snapshot, state.m_cell_metres, state.m_thickness_metres, kMatterGravityY, 8u, 1024u,
            fixture.m_catalog.get());
        PPR_TEST_ASSERT(seeded.has_value());
        for (std::size_t index = source.m_particles.size(); index < state.m_particles.size(); ++index) {
            PPR_TEST_ASSERT(seeded->m_particles[index].m_material == state.m_particles[index].m_material);
            PPR_TEST_ASSERT(seeded->m_particles[index].m_log_jp == state.m_particles[index].m_log_jp);
            PPR_TEST_ASSERT(seeded->m_particles[index].m_solid_fraction == state.m_particles[index].m_solid_fraction);
        }
    };

    PPR_UNIT_TEST (dug_and_seeded_material_families_initialize_from_properties) {
        Fixture fixture{"dug_families", "frontier", 1024u, true, true};
        const auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        auto source = checkpoint.m_current_matter;
        sim::ChunkGrid grid{1024u};

        u32 x = 60u;
        for (const char *const key: {"snow", "clay", "sand", "water"}) {
            const auto *const material = fixture.m_catalog->findMaterial(key);
            PPR_TEST_ASSERT(material != nullptr);
            PPR_TEST_ASSERT(not grid.setCell({x++, 60u}, {material->m_numeric_id, 300.0f}));
        }

        auto captured = grid.captureChunk({0u, 0u});
        PPR_TEST_ASSERT(captured.has_value());
        for (u32 column = 60u; column < 64u; ++column) {
            PPR_TEST_ASSERT(not grid.setCell({column, 60u}, {}));
        }

        grid.exchangeChunk(*captured);
        const std::array edits{std::move(*captured)};
        for (const bool saved_properties: {false, true}) {
            if (saved_properties) {
                for (const char *const key: {"snow", "clay"}) {
                    const auto *const material = fixture.m_catalog->findMaterial(key);
                    matter::GpuMaterialProperties row{};
                    row.m_substance_id = material->m_numeric_id;
                    row.m_family = matterFamily(*material);
                    row.m_solid_fraction0 = 0.45f;
                    source.m_properties.push_back(row);
                }
            }

            auto prepared = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
            PPR_TEST_ASSERT(prepared and prepared->has_value());
            auto &state = **prepared;
            PPR_TEST_ASSERT(not completeMatterProperties(state, fixture.m_catalog.get()));
            // Water is not terrain: removing its cell must not emit a dug particle.
            PPR_TEST_ASSERT(state.m_particles.size() == source.m_particles.size() + 3u);
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 24u>>(state.m_particles.front()) ==
                std::bit_cast<std::array<u32, 24u>>(source.m_particles.front()));

            const auto indices = matterPropertyIndices(state);
            PPR_TEST_ASSERT(indices.has_value());
            for (std::size_t index = source.m_particles.size(); index < state.m_particles.size(); ++index) {
                const auto &row = state.m_properties[(*indices)[index]];
                const auto &particle = state.m_particles[index];
                const bool new_family = row.m_family != matter::ESourceMaterial::sand;
                PPR_TEST_ASSERT(particle.m_log_jp == (new_family ? 0.0f : 1.0f));
                PPR_TEST_ASSERT(particle.m_compaction == 0.0f);
                PPR_TEST_ASSERT(particle.m_solid_fraction == (new_family ? row.m_solid_fraction0 : 0.3f));
            }

            const auto snapshot = captureMovingMatter(state, state.m_particles);
            PPR_TEST_ASSERT(snapshot.has_value());
            auto seeded = makeMatterState(
                *snapshot, state.m_cell_metres, state.m_thickness_metres, kMatterGravityY, 8u, 1024u,
                fixture.m_catalog.get());
            PPR_TEST_ASSERT(seeded.has_value());
            PPR_TEST_ASSERT(not completeMatterProperties(*seeded, fixture.m_catalog.get()));
            for (const auto &particle: seeded->m_particles) {
                const bool new_family = particle.m_material == static_cast<float>(matter::ESourceMaterial::cohesive) or
                                        particle.m_material == static_cast<float>(matter::ESourceMaterial::compacting);
                PPR_TEST_ASSERT(particle.m_log_jp == (new_family ? 0.0f : 1.0f));
                PPR_TEST_ASSERT(particle.m_compaction == 0.0f and particle.m_solid_fraction == 0.3f);
            }
        }
    };

    PPR_UNIT_TEST (dig_candidate_preserves_carried_columns_and_emits_original_seam_inventory_once) {
        Fixture fixture;
        const auto *const oxygen = fixture.m_catalog->findMaterial("oxygen");
        PPR_TEST_ASSERT(oxygen != nullptr);

        auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        auto &source = checkpoint.m_current_matter;
        source.m_identity.m_publication = 7u;
        source.m_particles[0u].m_displacement_x = 0.125f;
        source.m_particles[0u].m_deformation_gradient = {1.0f, 0.25f, -0.125f, 0.75f};
        source.m_particles[0u].m_deformation_displacement = {0.125f, 0.25f, -0.5f, -0.125f};
        source.m_particles[0u].m_liquid_density = 0.5f;
        source.m_particles[0u].m_log_jp = 0.75f;
        source.m_gas.push_back(
            {5u, 100.5 * source.m_cell_metres, 100.5 * source.m_cell_metres, 0.1, 0.1, 300.0, 1.0, oxygen->m_numeric_id,
                sim::EGenerationMatter::gas});
        const auto original = source;

        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not sim::apply(grid, checkpoint.m_snapshot));
        Array<sim::ChunkGrid::CapturedChunk> edits;
        for (u32 x: {127u, 128u}) {
            auto captured = grid.captureChunk(sim::chunkOf({x, 40u}));
            PPR_TEST_ASSERT(captured.has_value());
            PPR_TEST_ASSERT(not grid.setCell({x, 40u}, {0u, 99.0f}));
            grid.exchangeChunk(*captured);
            edits.push_back(std::move(*captured));
        }

        const auto prepared = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(prepared and prepared->has_value());
        const auto *const candidate = &**prepared;
        PPR_TEST_ASSERT(candidate->m_next_id == 14u and candidate->m_records.size() == 3u);
        PPR_TEST_ASSERT(candidate->m_identity.m_publication == 8u and candidate->m_identity.m_terrain_revision == 2u);
        PPR_TEST_ASSERT(candidate->m_gas == original.m_gas);
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 24u>>(candidate->m_particles[0u]) ==
            std::bit_cast<std::array<u32, 24u>>(source.m_particles[0u]));

        for (std::size_t i = 0u; i < 2u; ++i) {
            const u32 x = 127u + static_cast<u32>(i);
            const auto before = *grid.getCell({x, 40u});
            const auto &record = candidate->m_records[i + 1u];
            const double density = fixture.m_catalog->findMaterial(before.m_element)->m_reference_density_kg_m3;
            const double volume = source.m_cell_metres * source.m_cell_metres * source.m_thickness_metres;

            PPR_TEST_ASSERT(record.m_id == 12u + i and record.m_material == before.m_element);
            PPR_TEST_ASSERT(record.m_mass_kg == density * volume and record.m_volume_m3 == volume);
            PPR_TEST_ASSERT(record.m_temperature_k == static_cast<double>(before.m_temperature));
            PPR_TEST_ASSERT(record.m_x_m == (x + 0.5) * source.m_cell_metres);
            PPR_TEST_ASSERT(
                candidate->m_particles[i + 1u].m_material == static_cast<float>(matter::ESourceMaterial::sand));
            PPR_TEST_ASSERT(candidate->m_particles[i + 1u].m_displacement_x == 0.0f);
        }

        const auto retry = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(retry and retry->has_value());
        PPR_TEST_ASSERT((**retry).m_next_id == candidate->m_next_id);
        PPR_TEST_ASSERT((**retry).m_records == candidate->m_records);
        PPR_TEST_ASSERT(source.m_next_id == original.m_next_id and source.m_identity == original.m_identity);
        PPR_TEST_ASSERT(source.m_records == original.m_records);
        PPR_TEST_ASSERT(grid.getCell({127u, 40u})->m_element != 0u and grid.getCell({128u, 40u})->m_element != 0u);
    };

    PPR_UNIT_TEST (new_contour_coverage_rejects_moving_and_gas_but_preserves_prior_overlap) {
        Fixture fixture;
        const auto *const oxygen = fixture.m_catalog->findMaterial("oxygen");
        PPR_TEST_ASSERT(oxygen != nullptr);

        const auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        const auto solid = fixture.m_catalog->findMaterial("basalt")->m_numeric_id;

        for (u32 gas = 0u; gas < 2u; ++gas) {
            auto source = checkpoint.m_current_matter;
            auto record = source.m_records[0u];
            record.m_x_m = 128.0 * source.m_cell_metres;
            record.m_y_m = 40.5 * source.m_cell_metres;
            PPR_TEST_ASSERT(record.m_x_m / source.m_cell_metres == 128.0);
            PPR_TEST_ASSERT(record.m_y_m / source.m_cell_metres == 40.5);
            source.m_records[0u] = record;
            source.m_particles[0u].m_position_x = 131.0f;
            source.m_particles[0u].m_position_y = 43.5f;

            if (gas != 0u) {
                record.m_material = oxygen->m_numeric_id;
                record.m_phase = sim::EGenerationMatter::gas;
                source.m_gas = {record};
                source.m_records.clear();
                source.m_particles.clear();
                source.m_profile.m_particle_count = 0u;
            }

            sim::ChunkGrid grid;
            auto chunk = grid.captureChunk({1u, 0u});
            PPR_TEST_ASSERT(chunk.has_value());
            PPR_TEST_ASSERT(not grid.setCell({128u, 40u}, {solid, 300.0f}));
            grid.exchangeChunk(*chunk);
            Array<sim::ChunkGrid::CapturedChunk> edits;
            edits.push_back(std::move(*chunk));

            const auto rejected = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
            PPR_TEST_ASSERT(rejected and not rejected->has_value());
            PPR_TEST_ASSERT(source.m_next_id == 12u and source.m_identity.m_publication == 0u);
            PPR_TEST_ASSERT(grid.getCell({128u, 40u})->m_element == 0u);
        }

        auto source = checkpoint.m_current_matter;
        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not grid.setCell({130u, 40u}, {solid, 300.0f}));
        auto chunk = grid.captureChunk({0u, 0u});
        PPR_TEST_ASSERT(chunk.has_value());
        PPR_TEST_ASSERT(not grid.setCell({5u, 5u}, {solid, 300.0f}));
        grid.exchangeChunk(*chunk);
        Array<sim::ChunkGrid::CapturedChunk> edits;
        edits.push_back(std::move(*chunk));

        const auto unrelated = prepareMatterTerrainEdit(source, grid, edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(unrelated and unrelated->has_value());
        PPR_TEST_ASSERT((**unrelated).m_records == source.m_records);
        PPR_TEST_ASSERT((**unrelated).m_particles.size() == source.m_particles.size());

        // A nonzero catalog particle material is not solid contour occupancy.
        sim::ChunkGrid nonsolid;
        auto non_solid_chunk = nonsolid.captureChunk({1u, 0u});
        PPR_TEST_ASSERT(non_solid_chunk.has_value());
        PPR_TEST_ASSERT(not nonsolid.setCell({130u, 40u}, {oxygen->m_numeric_id, 300.0f}));
        nonsolid.exchangeChunk(*non_solid_chunk);
        Array<sim::ChunkGrid::CapturedChunk> non_solid_edits;
        non_solid_edits.push_back(std::move(*non_solid_chunk));

        const auto ignored = prepareMatterTerrainEdit(source, nonsolid, non_solid_edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(ignored and ignored->has_value());
        PPR_TEST_ASSERT((**ignored).m_records == source.m_records);

        // The rejected cohort includes a locally reserved dug-cell emission;
        // neither its reserved ID nor any carried column escapes rejection.
        sim::ChunkGrid cohort;
        PPR_TEST_ASSERT(not sim::apply(cohort, checkpoint.m_snapshot));
        auto removed = cohort.captureChunk({0u, 0u});
        auto added = cohort.captureChunk({1u, 0u});
        PPR_TEST_ASSERT(removed and added);
        PPR_TEST_ASSERT(not cohort.setCell({127u, 40u}, {0u, 0.0f}));
        PPR_TEST_ASSERT(not cohort.setCell({130u, 40u}, {solid, 300.0f}));
        cohort.exchangeChunk(*removed);
        cohort.exchangeChunk(*added);
        Array<sim::ChunkGrid::CapturedChunk> cohort_edits;
        cohort_edits.push_back(std::move(*removed));
        cohort_edits.push_back(std::move(*added));

        const auto deferred = prepareMatterTerrainEdit(source, cohort, cohort_edits, *fixture.m_catalog);
        PPR_TEST_ASSERT(deferred and not deferred->has_value());
        PPR_TEST_ASSERT(source.m_next_id == checkpoint.m_current_matter.m_next_id);
        PPR_TEST_ASSERT(source.m_identity == checkpoint.m_current_matter.m_identity);
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 24u>>(source.m_particles[0u]) ==
            std::bit_cast<std::array<u32, 24u>>(checkpoint.m_current_matter.m_particles[0u]));
        PPR_TEST_ASSERT(cohort.getCell({127u, 40u})->m_element != 0u and cohort.getCell({130u, 40u})->m_element == 0u);
    };

    PPR_UNIT_TEST (current_round_trip_preserves_all_words_and_rebinds_only_world) {
        Fixture fixture;
        auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        auto &state = checkpoint.m_current_matter;
        state.m_identity = {91u, 7u, 4u};
        auto &particle = state.m_particles[0u];
        particle.m_displacement_x = 0.125f;
        particle.m_displacement_y = -0.25f;
        particle.m_deformation_gradient = {1.0f, 0.25f, -0.125f, 0.75f};
        particle.m_deformation_displacement = {0.125f, 0.25f, -0.5f, -0.125f};
        particle.m_liquid_density = 0.5f;
        particle.m_lambda = 0.375f;
        particle.m_log_jp = 0.75f;
        particle.m_compaction = 0.25f;
        particle.m_solid_fraction = 0.6f;
        const auto words = std::bit_cast<std::array<u32, 24u>>(particle);

        state.m_properties[0u].m_liquid_viscosity = 0.375f;
        state.m_properties[0u].m_liquid_relaxation = 0.75f;
        state.m_properties[0u].m_cohesion = 0.125f;
        state.m_properties[0u].m_compaction_rate = 0.75f;
        const auto property_words = std::bit_cast<std::array<u32, 19u>>(state.m_properties[0u]);

        const auto bytes = encodeWorldCheckpoint(checkpoint, *fixture.m_catalog);
        PPR_TEST_ASSERT(bytes.has_value());
        const auto decoded = decodeWorldCheckpoint(*bytes, *fixture.m_catalog);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(std::bit_cast<std::array<u32, 24u>>(decoded->m_current_matter.m_particles[0u]) == words);
        PPR_TEST_ASSERT(decoded->m_current_matter.m_identity == state.m_identity);
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 19u>>(decoded->m_current_matter.m_properties[0u]) == property_words);

        const auto snapshot_bytes = sim::save(checkpoint.m_snapshot);
        PPR_TEST_ASSERT(snapshot_bytes.has_value());
        // v9 appends policy (4), identity (8), count (4), and nineteen words per material.
        const std::size_t table_bytes = 16u + state.m_properties.size() * 76u;
        const auto profile_offset = bytes->size() - snapshot_bytes->size() - 8u - table_bytes - 96u - 4u - 64u;
        auto corrupt_profile = *bytes;
        corrupt_profile[profile_offset] ^= 1u;
        const auto rejected_profile = decodeWorldCheckpoint(corrupt_profile, *fixture.m_catalog);
        PPR_TEST_ASSERT(not rejected_profile and rejected_profile.error() == std::errc::invalid_argument);

        const auto incomplete = decodeWorldCheckpoint(
            std::span<const u8>{bytes->data(), profile_offset + 63u}, *fixture.m_catalog);
        PPR_TEST_ASSERT(not incomplete and incomplete.error() == sim::errc::truncated);

        auto restored = restoreWorldCheckpoint(fixture.m_catalog, *decoded, 92u);
        PPR_TEST_ASSERT(restored.has_value());
        PPR_DEFER {
            PPR_TEST_ASSERT(not (*restored)->shutdown());
        };

        const auto &current = *(*restored)->matterState();
        PPR_TEST_ASSERT(current.m_identity == MatterIdentity{92u, 7u, 4u});
        PPR_TEST_ASSERT(std::bit_cast<std::array<u32, 19u>>(current.m_properties[0u]) == property_words);
        PPR_TEST_ASSERT(std::bit_cast<std::array<u32, 24u>>(current.m_particles[0u]) == words);
        PPR_TEST_ASSERT((*restored)->update(TimeSpan{}) == std::errc::operation_not_permitted);

        const auto captured = captureWorldCheckpoint(**restored, (*restored)->preparedWorld()->generated());
        PPR_TEST_ASSERT(captured.has_value());
        PPR_TEST_ASSERT(std::bit_cast<std::array<u32, 24u>>(captured->m_current_matter.m_particles[0u]) == words);

        for (u32 scenario = 0u; scenario < 8u; ++scenario) {
            auto bad = checkpoint;
            auto &invalid = bad.m_current_matter;
            switch (scenario) {
                case 0u: --invalid.m_profile.m_grid_width; break;
                case 1u: invalid.m_profile.m_delta_time *= 2.0f; break;
                case 2u: invalid.m_cell_metres *= 2.0; break;
                case 3u: invalid.m_particles[0u].m_mass *= 2.0f; break;
                case 4u: invalid.m_particles[0u].m_volume *= 2.0f; break;
                case 5u: invalid.m_particles[0u].m_material = 2.0f; break;
                case 6u:
                    invalid.m_particles[0u].m_deformation_gradient.m_c0y = std::numeric_limits<float>::quiet_NaN();
                    break;
                case 7u: invalid.m_particles.clear(); break;
            }

            PPR_TEST_ASSERT(validateWorldCheckpoint(bad, *fixture.m_catalog) == std::errc::invalid_argument);
        }

        std::fprintf(
            stderr, "[checkpoint-layout] WorldCheckpoint=%zu/%zu MatterState=%zu/%zu ColonyDriver=%zu/%zu\n",
            sizeof(WorldCheckpoint), alignof(WorldCheckpoint), sizeof(MatterState), alignof(MatterState),
            sizeof(ColonyDriver), alignof(ColonyDriver));
    };


    // ------------------------------------------------------------------
    // checkpoint envelope and frozen capture
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (current_wire_rejects_unsupported_liquid_relaxation) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        const auto checkpoint = editedCheckpoint(catalog);
        PPR_TEST_ASSERT(checkpoint.m_current_matter.m_particles.size() == 1u);
        PPR_TEST_ASSERT(checkpoint.m_current_matter.m_profile.m_liquid_relaxation == 1.0f);

        const auto encoded = encodeWorldCheckpoint(checkpoint, catalog);
        const auto snapshot = sim::save(checkpoint.m_snapshot);
        PPR_TEST_ASSERT(encoded.has_value() and snapshot.has_value());
        const auto valid = decodeWorldCheckpoint(*encoded, catalog);
        PPR_TEST_ASSERT(valid.has_value());

        // v8 tail: 64-byte profile, count, one 88-byte particle, material table,
        // snapshot length and snapshot. The table has a 16-byte header and 40-byte rows.
        const std::size_t table_bytes = 16u + checkpoint.m_current_matter.m_properties.size() * 76u;
        const std::size_t profile_offset = encoded->size() - snapshot->size() - 8u - table_bytes - 96u - 4u - 64u;
        const std::size_t relaxation_offset = profile_offset + 7u * 4u;
        // The eighth little-endian profile word is exactly 1.0f before corruption.
        PPR_TEST_ASSERT((*encoded)[relaxation_offset] == 0u and (*encoded)[relaxation_offset + 1u] == 0u);
        PPR_TEST_ASSERT((*encoded)[relaxation_offset + 2u] == 0x80u and (*encoded)[relaxation_offset + 3u] == 0x3fu);
        for (const float value: {2.0f, 3.0f}) {
            auto bytes = *encoded;
            const u32 bits = std::bit_cast<u32>(value);
            for (u32 byte = 0u; byte < sizeof(u32); ++byte) {
                bytes[relaxation_offset + byte] = static_cast<u8>(bits >> (byte * 8u));
            }

            const auto rejected = decodeWorldCheckpoint(bytes, catalog);
            PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::invalid_argument);
        }
    };

    PPR_UNIT_TEST (current_particle_wire_payload_is_the_exact_semantic_word_sequence) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto checkpoint = editedCheckpoint(catalog);
        auto &particle = checkpoint.m_current_matter.m_particles[0u];
        particle.m_displacement_x = 0.125f;
        particle.m_displacement_y = -0.25f;
        particle.m_deformation_gradient = {1.0f, 0.25f, -0.125f, 0.75f};
        particle.m_deformation_displacement = {0.125f, 0.25f, -0.5f, -0.125f};
        particle.m_liquid_density = 0.5f;
        particle.m_lambda = 0.375f;
        particle.m_log_jp = 0.75f;
        particle.m_color_r = 0.25f;
        particle.m_color_g = -0.5f;
        particle.m_color_b = 0.375f;
        particle.m_enabled = 1.0f;

        const auto words = std::bit_cast<std::array<u32, 24u>>(particle);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, catalog));

        const auto encoded = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(encoded.has_value());
        const auto repeated = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(repeated.has_value() and *repeated == *encoded);

        // v8 places policy/identity/count (16 bytes) and ten words per material
        // after the 22-word particle payload and before the snapshot length.
        const auto snapshot_bytes = sim::save(checkpoint.m_snapshot);
        PPR_TEST_ASSERT(snapshot_bytes.has_value());
        const std::size_t table_bytes = 16u + checkpoint.m_current_matter.m_properties.size() * 76u;
        const std::size_t payload_offset = encoded->size() - snapshot_bytes->size() - 8u - table_bytes - 24u * 4u;
        PPR_TEST_ASSERT(payload_offset + 24u * 4u + table_bytes + 8u + snapshot_bytes->size() == encoded->size());
        constexpr std::array<u8, 4u> particle_count{1u, 0u, 0u, 0u};
        PPR_TEST_ASSERT(std::ranges::equal(std::span{encoded->data() + payload_offset - 4u, 4u}, particle_count));

        for (std::size_t index = 0u; index < words.size(); ++index) {
            u32 actual{};
            for (u32 byte = 0u; byte < sizeof(u32); ++byte) {
                actual |= static_cast<u32>((*encoded)[payload_offset + index * sizeof(u32) + byte]) << (byte * 8u);
            }

            PPR_TEST_ASSERT(actual == words[index]);
        }

        const auto read_integer = [&](const std::size_t offset, const u32 width) {
            u64 value{};
            for (u32 byte = 0u; byte < width; ++byte) {
                value |= static_cast<u64>((*encoded)[offset + byte]) << (byte * 8u);
            }

            return value;
        };

        const std::size_t table_offset = payload_offset + 96u;
        const auto &properties = checkpoint.m_current_matter.m_properties;
        PPR_TEST_ASSERT(read_integer(table_offset, 4u) == kMatterMaterialTablePolicy);
        PPR_TEST_ASSERT(read_integer(table_offset + 4u, 8u) == matterMaterialTableIdentity(properties));
        PPR_TEST_ASSERT(read_integer(table_offset + 12u, 4u) == properties.size());
        for (std::size_t row = 0u; row < properties.size(); ++row) {
            const auto property_words = std::bit_cast<std::array<u32, 19u>>(properties[row]);
            for (std::size_t word = 0u; word < property_words.size(); ++word) {
                PPR_TEST_ASSERT(read_integer(table_offset + 16u + row * 76u + word * 4u, 4u) == property_words[word]);
            }
        }

        PPR_TEST_ASSERT(read_integer(table_offset + table_bytes, 8u) == snapshot_bytes->size());
        PPR_TEST_ASSERT(
            std::ranges::equal(
                std::span{encoded->data() + table_offset + table_bytes + 8u, snapshot_bytes->size()}, *snapshot_bytes));
    };

    PPR_UNIT_TEST (generator_seven_checkpoint_is_incompatible_before_material_id_restore) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto checkpoint = editedCheckpoint(catalog);
        PPR_TEST_ASSERT(sim::kGeneratorAlgorithmVersion == 8u);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, catalog));

        checkpoint.m_descriptor.m_generator_version = 7u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::protocol_not_supported);
        // Even an unknown saved cell ID cannot pass the identity boundary.
        checkpoint.m_snapshot.m_deltas.front().m_cells.front().m_element = 60000u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::protocol_not_supported);
        const auto restored = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 1u);
        PPR_TEST_ASSERT(not restored and restored.error() == std::errc::protocol_not_supported);

        checkpoint.m_descriptor.m_generator_version = sim::kGeneratorAlgorithmVersion;
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::invalid_argument);
    };

    PPR_UNIT_TEST (ui_model_preserves_authored_metadata_and_nearest_agent_for_unresident_cells) {
        Fixture fixture;
        auto restored = restoreWorldCheckpoint(fixture.m_catalog, editedCheckpoint(*fixture.m_catalog), 2u);
        PPR_TEST_ASSERT(restored.has_value());
        auto &driver = **restored;
        PPR_DEFER {
            std::ignore = driver.shutdown();
        };

        // Retain the authored recipe/layout and actor while terrain residency
        // is absent. Inspection must not fabricate a vacuum/temperature read.
        driver.grid() = sim::ChunkGrid{};
        const sim::GlobalCellPos target{30u, 45u};
        ColonyPresentation snapshot{};
        pP::game::ui::captureColonyPresentation(snapshot, driver, target, target);

        PPR_TEST_ASSERT(snapshot.m_inspected.m_presence == ECellPresence::unresident);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_material.empty());
        PPR_TEST_ASSERT(snapshot.m_inspected.m_temperature_k == 0.0f);
        PPR_TEST_ASSERT(driver.grid().residentChunkCount() == 0u);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_generated);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_biome == "Hearth");
        PPR_TEST_ASSERT(snapshot.m_inspected.m_nearest.has_value());
        PPR_TEST_ASSERT(snapshot.m_inspected.m_nearest->m_key == 100u);

        const sim::GlobalCellPos outside{sim::kWorldEdge, 45u};
        pP::game::ui::captureColonyPresentation(snapshot, driver, outside, outside);
        PPR_TEST_ASSERT(snapshot.m_inspected.m_presence == ECellPresence::unavailable);
        PPR_TEST_ASSERT(not snapshot.m_inspected.m_generated and not snapshot.m_inspected.m_nearest);
        PPR_TEST_ASSERT(driver.grid().residentChunkCount() == 0u);
    };

    PPR_UNIT_TEST (round_trip_restores_edited_cells_actor_identity_and_frozen_full_capture) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto checkpoint = editedCheckpoint(catalog);
        checkpoint.m_report.m_retained_known_payload_bytes = 123456u;
        checkpoint.m_report.m_known_phase_scratch_bytes.fill(654321u);

        PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, catalog));

        const auto bytes = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(bytes.has_value());

        auto decoded = decodeWorldCheckpoint(*bytes, catalog);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(decoded->m_report.m_retained_known_payload_bytes == 0u);
        PPR_TEST_ASSERT(std::ranges::all_of(decoded->m_report.m_known_phase_scratch_bytes, [](const u64 bytes) {
            return bytes == 0u;
        }));
        PPR_TEST_ASSERT(decoded->m_descriptor == checkpoint.m_descriptor);
        PPR_TEST_ASSERT(decoded->m_snapshot == checkpoint.m_snapshot);
        PPR_TEST_ASSERT(decoded->m_geometry.m_blobs == checkpoint.m_geometry.m_blobs);
        PPR_TEST_ASSERT(decoded->m_particles.m_next_id == 12u and decoded->m_particles.m_chunks.size() == 1u);
        PPR_TEST_ASSERT(
            decoded->m_particles.m_chunks[0].records()[0] == checkpoint.m_particles.m_chunks[0].records()[0]);

        const auto images = fixture.presentation(17u);
        auto restored = restoreWorldCheckpoint(fixture.m_catalog, std::move(*decoded), 17u, {}, images);
        PPR_TEST_ASSERT(restored.has_value());
        auto &driver = **restored;

        PPR_TEST_ASSERT(driver.preparedWorld()->generated().m_geometry.m_blobs == checkpoint.m_geometry.m_blobs);
        const auto presentation = makeSimFramePacket(1u, driver);
        PPR_TEST_ASSERT(presentation.has_value());

        for (const auto &material: catalog.materials()) {
            const auto found = std::ranges::find(
                presentation->m_terrain_visuals, material.m_numeric_id, &GridTerrainVisual::m_material_id);
            PPR_TEST_ASSERT(found != presentation->m_terrain_visuals.end());
            const u32 expected = (material.m_alpha_mode == EMaterialAlphaMode::blend ? kGridSurfaceOpticalBlend : 0u) |
                                 (material.m_texture_sampling == EMaterialTextureSampling::repeat
                                         ? kGridSurfaceTextureRepeat
                                         : 0u) |
                                 (material.m_phase == EMaterialPhase::solid       ? kGridSurfaceParticleSolid
                                     : material.m_phase == EMaterialPhase::liquid ? kGridSurfaceParticleLiquid
                                                                                  : 0u);
            PPR_TEST_ASSERT(found->m_surface_flags == expected);
        }

        PPR_TEST_ASSERT(driver.tickCount() == 19u and driver.worldGeneration() == 17u);
        PPR_TEST_ASSERT(driver.controlledEntity() == checkpoint.m_controlled);
        PPR_TEST_ASSERT(not driver.initialMatterBaselineValid());
        const auto particles = driver.captureParticles();
        PPR_TEST_ASSERT(particles.has_value());
        PPR_TEST_ASSERT(particles->m_next_id == 12u and particles->m_chunks.size() == 1u);
        PPR_TEST_ASSERT(particles->m_chunks[0].records()[0].m_id == 11u);
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element == catalog.findMaterial("basalt")->m_numeric_id);
        PPR_TEST_ASSERT(driver.grid().getCell({128u, 40u})->m_temperature == 345.0f);

        const auto actor = *driver.controlledEntity();
        PPR_TEST_ASSERT(driver.registry().get<sim::BodyState>(actor)->m_velocity_x == 1.25f);
        PPR_TEST_ASSERT(driver.registry().get<Needs>(actor)->m_rest == 73.0f);
        PPR_TEST_ASSERT(driver.registry().get<Plan>(actor) == nullptr);

        driver.registry().emplace<Plan>(actor, {});
        driver.grid().clearAllDirty();
        PPR_TEST_ASSERT(not driver.grid().setCell({128u, 41u}, {catalog.findMaterial("basalt")->m_numeric_id, 310.0f}));
        const auto before = sim::capture(driver.grid(), 42u);

        ParallelScheduler chunks{"Checkpoint capture chunks", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldJob job{
            chunks,
            [](const ColonyDriverDesc &, const sim::GenerationExecution &,
                config::Diagnostics &) -> Expected<std::unique_ptr<ColonyDriver>> {
                // Capture/override-only fixtures must never admit a default generation.
                return std::unexpected{std::make_error_code(std::errc::operation_not_permitted)};
            }};
        const auto address = restored->get();
        PPR_TEST_ASSERT(not job.requestCheckpoint(*restored));
        PPR_TEST_ASSERT(not *restored);
        PPR_TEST_ASSERT(not job.update(false));
        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::queued);

        do {
            PPR_TEST_ASSERT(not job.update(true));
            std::this_thread::yield();
        } while (job.view().m_state != EWorldJobState::prepared);

        PPR_TEST_ASSERT(not job.view().m_error);

        auto captured = job.takeCheckpoint();
        auto owner = job.takeCaptured();
        PPR_TEST_ASSERT(owner.get() == address);
        PPR_TEST_ASSERT(sim::capture(owner->grid(), 42u) == before);
        PPR_TEST_ASSERT(captured.m_snapshot.m_deltas.size() == 2u);
        sim::Registry scratch;
        PPR_TEST_ASSERT(not registerColonyComponents(scratch));
        PPR_TEST_ASSERT(not sim::restore(scratch, captured.m_snapshot));
        PPR_TEST_ASSERT(scratch.get<Plan>(actor) == nullptr);
        PPR_TEST_ASSERT(not validateWorldCheckpoint(captured, catalog));

        while (job.busy()) {
            PPR_TEST_ASSERT(not job.update(true));
            std::this_thread::yield();
        }

        PPR_TEST_ASSERT(not job.requestCheckpoint(owner));

        do {
            PPR_TEST_ASSERT(not job.update(true));
            std::this_thread::yield();
        } while (job.view().m_state != EWorldJobState::prepared);

        PPR_TEST_ASSERT(not job.view().m_error);

        job.cancel(); // Completed but untaken payload must be disposed on coordinator.
        PPR_TEST_ASSERT(job.view().m_error == std::errc::operation_canceled);

        owner = job.takeCaptured();
        PPR_TEST_ASSERT(owner.get() == address);

        while (job.busy()) {
            PPR_TEST_ASSERT(not job.update(true));
            std::this_thread::yield();
        }

        PPR_TEST_ASSERT(job.view().m_state == EWorldJobState::cancelled);
        PPR_TEST_ASSERT(sim::capture(owner->grid(), 42u) == before);

        PPR_TEST_ASSERT(not job.shutdown());
        PPR_TEST_ASSERT(not owner->shutdown());
    };

    PPR_UNIT_TEST (diagnostic_export_rejects_cancellation_and_unwritable_directory) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto restored = restoreWorldCheckpoint(fixture.m_catalog, editedCheckpoint(catalog), 2u);
        PPR_TEST_ASSERT(restored.has_value());
        const auto *const prepared = (*restored)->preparedWorld();
        PPR_TEST_ASSERT(prepared != nullptr);
        PPR_TEST_ASSERT(not prepared->presentation());

        auto [capture_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        PPR_TEST_ASSERT(
            exportWorldDiagnostics(catalog, prepared->generated(), prepared->descriptor(), {}, capture_context.get()) ==
            std::errc::operation_canceled);

        const auto path = fixture.m_root / "checkpoint-export-file-fixture";
        PPR_TEST_ASSERT(not std::filesystem::exists(path));
        PPR_DEFER {
            // Best-effort removal of a test-owned file, also on assertion unwind.
            std::error_code cleanup;
            std::filesystem::remove(path, cleanup);
        };

        {
            std::ofstream file{path, std::ios::binary};
            file << "This regular file cannot be an output directory.";
            PPR_TEST_ASSERT(file.good());
        }

        const auto error = exportWorldDiagnostics(catalog, prepared->generated(), prepared->descriptor(), path);
        PPR_TEST_ASSERT(error != std::error_code{});
        PPR_TEST_ASSERT(not (*restored)->shutdown());
    };

    PPR_UNIT_TEST (restored_current_matter_is_frozen_until_attached_and_keeps_physical_inventory) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto restored = restoreWorldCheckpoint(fixture.m_catalog, editedCheckpoint(catalog), 3u);
        PPR_TEST_ASSERT(restored.has_value());
        auto &driver = **restored;
        driver.setPaused(true);
        const auto frozen = driver.captureParticles();
        PPR_TEST_ASSERT(frozen.has_value());

        PPR_TEST_ASSERT(not driver.requestDig({127u, 40u}, 2u, 1u));
        PPR_TEST_ASSERT(driver.update(TimeSpan{}) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(not driver.editPending());
        PPR_TEST_ASSERT(driver.grid().getCell({127u, 40u})->m_element == catalog.findMaterial("basalt")->m_numeric_id);
        PPR_TEST_ASSERT(driver.particleCount() == 1u);
        const auto current = driver.captureParticles();
        PPR_TEST_ASSERT(current and current->m_chunks[0].m_particles == frozen->m_chunks[0].m_particles);

        PPR_TEST_ASSERT(driver.stepOne() == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(driver.tickCount() == 19u);
        PPR_TEST_ASSERT(driver.matterState() != nullptr and not driver.matterAttached());
        PPR_TEST_ASSERT(frozen->m_chunks[0].records()[0].m_id == 11u);

        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (checkpoint_blob_references_ranges_and_wire_counts_are_checked) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        const auto original = editedCheckpoint(catalog);
        const auto plan = compileWorldPlan(catalog, catalog.worlds().front().m_key, 42u);
        PPR_TEST_ASSERT(plan.has_value());
        const auto &reference = original.m_geometry.m_blobs.front();
        const auto &deposit = plan->recipe().m_biomes[reference.m_biome].m_deposits[reference.m_deposit];

        for (u32 scenario = 0u; scenario < 13u; ++scenario) {
            auto bad = original;
            auto &blob = bad.m_geometry.m_blobs.front();
            switch (scenario) {
                case 0u: blob.m_biome = 10000u; break;
                case 1u: blob.m_deposit = 10000u; break;
                case 2u: blob.m_ordinal = 10000u; break;
                case 3u: blob.m_radius_m = 0.0; break;
                case 4u: blob.m_stretch = 100.0; break;
                case 5u: blob.m_angle = std::numeric_limits<double>::quiet_NaN(); break;
                case 6u: blob.m_x_m = -1.0; break;
                case 7u: blob.m_offset_y = 1025.0; break;
                case 8u: bad.m_geometry.m_blobs.push_back(blob); break;
                case 9u: bad.m_geometry.m_blobs.resize(4097u, blob); break;
                case 10u: blob.m_angle = -0.1; break;
                case 11u: blob.m_angle = 2.0 * std::numbers::pi + 0.1; break;
                case 12u:
                    blob.m_angle = 2.0 * std::numbers::pi * (deposit.m_orientation_max_degrees / 360.0) + 0.01;
                    break;
            }

            PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);
        }

        const auto encoded = encodeWorldCheckpoint(original, catalog);
        PPR_TEST_ASSERT(encoded.has_value());
        auto old_version = *encoded;
        old_version[8u] = 2u;
        const auto rejected_old = decodeWorldCheckpoint(old_version, catalog);
        PPR_TEST_ASSERT(not rejected_old and rejected_old.error() == std::errc::protocol_not_supported);

        std::array<u8, 16u> position{};
        const auto &first = original.m_geometry.m_blobs.front();
        for (u32 byte = 0u; byte < 8u; ++byte) {
            position[byte] = static_cast<u8>(std::bit_cast<u64>(first.m_x_m) >> (byte * 8u));
            position[8u + byte] = static_cast<u8>(std::bit_cast<u64>(first.m_y_m) >> (byte * 8u));
        }
        const auto record = std::ranges::search(*encoded, position);
        PPR_TEST_ASSERT(not record.empty());
        const auto offset = static_cast<std::size_t>(record.begin() - encoded->begin());
        PPR_TEST_ASSERT(offset >= 16u);

        auto excessive = *encoded;
        // Blob count precedes three u32 references and seven doubles.
        excessive[offset - 16u] = 1u;
        excessive[offset - 15u] = 16u;
        excessive[offset - 14u] = 0u;
        excessive[offset - 13u] = 0u;
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(excessive, catalog));

        auto truncated = *encoded;
        truncated.resize(offset + 8u);
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(truncated, catalog));
    };

    PPR_UNIT_TEST (malformed_identity_metric_counts_and_payload_are_rejected_before_restore) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        const auto original = editedCheckpoint(catalog);
        auto bad = original;
        ++bad.m_descriptor.m_content_fingerprint;

        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::protocol_not_supported);

        bad = original;
        bad.m_descriptor.m_metric.m_cells_per_metre = 1u;
        PPR_TEST_ASSERT(not restoreWorldCheckpoint(fixture.m_catalog, std::move(bad), 2u));

        bad = original;
        ++bad.m_controlled->m_generation;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_materials.back().m_key = "wrong-key";
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::protocol_not_supported);

        bad = original;
        bad.m_snapshot.m_deltas[0].m_cells[0].m_element = 60000u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_initial_matter_valid = true;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_matter.m_regions[0].m_spans.push_back({40u, 128u, 130u});
        ++bad.m_matter.m_span_count;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_matter.m_regions[0].m_spans = {{41u, 127u, 129u}, {40u, 127u, 129u}};
        bad.m_matter.m_span_count = 2u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        bad.m_particles.m_next_id = 11u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        bad = original;
        auto duplicate = *bad.m_particles.m_chunks[0].m_particles;
        duplicate.push_back(duplicate[0]);
        bad.m_particles.m_chunks[0].m_particles = std::make_shared<const Array<sim::MatterParticle>>(
            std::move(duplicate));
        PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);

        for (u32 invalid = 0u; invalid < 6u; ++invalid) {
            bad = original;
            auto particles = *bad.m_particles.m_chunks[0].m_particles;
            if (invalid == 0u) {
                particles[0].m_x_m = std::numeric_limits<double>::quiet_NaN();
            }

            if (invalid == 1u) {
                particles[0].m_x_m = -1.0;
            }

            if (invalid == 2u) {
                particles[0].m_phase = sim::EGenerationMatter::gas;
            }

            if (invalid == 3u) {
                particles[0].m_bulk_density_kg_m3 = -1.0;
            }

            if (invalid == 4u) {
                particles[0].m_mass_kg = 10.0;
            }

            if (invalid == 5u) {
                particles[0].m_volume_m3 = 1.0;
                particles[0].m_mass_kg = 1.0;
            }

            bad.m_particles.m_chunks[0].m_particles = std::make_shared<const Array<sim::MatterParticle>>(
                std::move(particles));
            PPR_TEST_ASSERT(validateWorldCheckpoint(bad, catalog) == std::errc::invalid_argument);
        }

        const auto encoded = encodeWorldCheckpoint(original, catalog);
        PPR_TEST_ASSERT(encoded.has_value());
        auto corrupt = *encoded;
        corrupt[8u] = 255u;
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(corrupt, catalog));

        corrupt = *encoded;
        corrupt[12u] = 0u; // Explicit full-checkpoint discriminator.
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(corrupt, catalog));

        // Locate this fixture's unique particle ID and continuous position in
        // the explicit wire record, then corrupt its preceding count field.
        std::array<u8, 16u> particle_prefix{};
        particle_prefix[0u] = 11u;
        const u64 x_bits = std::bit_cast<u64>(original.m_particles.m_chunks[0].records()[0].m_x_m);
        for (u32 byte = 0u; byte < 8u; ++byte) {
            particle_prefix[8u + byte] = static_cast<u8>(x_bits >> (byte * 8u));
        }
        const auto record = std::ranges::search(*encoded, particle_prefix);
        PPR_TEST_ASSERT(not record.empty());
        const auto record_offset = static_cast<std::size_t>(record.begin() - encoded->begin());
        PPR_TEST_ASSERT(record_offset >= 4u);

        corrupt = *encoded;
        const u32 oversized = sim::kMaxMatterParticles + 1u;
        for (u32 byte = 0u; byte < 4u; ++byte) {
            corrupt[record_offset - 4u + byte] = static_cast<u8>(oversized >> (byte * 8u));
        }
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(corrupt, catalog));

        corrupt = *encoded;
        corrupt.push_back(0u);
        PPR_TEST_ASSERT(not decodeWorldCheckpoint(corrupt, catalog));

        for (const std::size_t size: std::array<std::size_t, 4u>{0u, 12u, 64u, encoded->size() - 1u}) {
            PPR_TEST_ASSERT(not decodeWorldCheckpoint(std::span<const u8>{encoded->data(), size}, catalog));
        }

        auto [capture_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled_decode = decodeWorldCheckpoint(*encoded, catalog, capture_context);
        PPR_TEST_ASSERT(not cancelled_decode and cancelled_decode.error() == std::errc::operation_canceled);
        const auto cancelled_encode = encodeWorldCheckpoint(original, catalog, capture_context);
        PPR_TEST_ASSERT(not cancelled_encode and cancelled_encode.error() == std::errc::operation_canceled);
        const auto cancelled_restore = restoreWorldCheckpoint(
            fixture.m_catalog, original, 4u, {.m_context = capture_context});
        PPR_TEST_ASSERT(not cancelled_restore and cancelled_restore.error() == std::errc::operation_canceled);

        PPR_TEST_ASSERT(not validateWorldCheckpoint(original, catalog));
    };

    // ------------------------------------------------------------------
    // named pack envelope and qualified profile wire contract
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (envelope_seven_manifest_one_and_raw_snapshot_five_have_distinct_headers) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        const auto checkpoint = editedCheckpoint(catalog);
        PPR_TEST_ASSERT(kWorldCheckpointVersion == 9u and asset::Manifest::schema_version == 1u);
        PPR_TEST_ASSERT(sim::kSnapshotVersion == 5u and checkpoint.m_descriptor.m_pack_schema_version == 1u);

        const auto encoded = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(encoded.has_value());

        constexpr std::array<u8, 25u> header{
            0x50u, 0x50u, 0x52u, 0x57u, 0x47u, 0x45u, 0x4eu, 0x31u, 0x09u, 0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x00u,
            0x00u, 0x00u, 0x08u, 0x00u, 0x00u, 0x00u, 0x13u, 0x00u, 0x00u, 0x00u};
        PPR_TEST_ASSERT(encoded->size() > header.size());
        PPR_TEST_ASSERT(std::ranges::equal(std::span{encoded->data(), header.size()}, header));
        constexpr std::string_view profile = "checkpoint:frontier";
        PPR_TEST_ASSERT(std::ranges::equal(std::span{encoded->data() + header.size(), profile.size()}, profile));

        const auto raw = sim::save(checkpoint.m_snapshot);
        PPR_TEST_ASSERT(raw.has_value());
        constexpr std::array<u8, 4u> raw_header{5u, 0u, 0u, 0u};
        PPR_TEST_ASSERT(std::ranges::equal(std::span{raw->data(), raw_header.size()}, raw_header));

        for (const u32 old_version: {0u, 1u, 2u, 3u, 4u, 5u}) {
            auto earlier = *encoded;
            earlier[8u] = static_cast<u8>(old_version);
            earlier.resize(12u);
            const auto rejected = decodeWorldCheckpoint(earlier, catalog);
            PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::protocol_not_supported);
        }

        auto flattened_schema = checkpoint;
        flattened_schema.m_descriptor.m_pack_schema_version = 3u;
        PPR_TEST_ASSERT(validateWorldCheckpoint(flattened_schema, catalog) == std::errc::protocol_not_supported);

        auto wrong_magic = *encoded;
        wrong_magic.front() ^= 1u;
        const auto unknown = decodeWorldCheckpoint(wrong_magic, catalog);
        PPR_TEST_ASSERT(not unknown and unknown.error() == std::errc::protocol_not_supported);
    };

    PPR_UNIT_TEST (saved_schedule_migrates_v6_and_rejects_invalid_counts_without_mutating_live_world) {
        Fixture fixture;
        const auto &catalog = *fixture.m_catalog;
        auto checkpoint = editedCheckpoint(catalog);
        checkpoint.m_current_matter.m_iterations = 5u;

        // Independent FNV-1a fixture: little-endian policy=1, count=2, then
        // <II8f> rows (11/12, liquid, 1, 1.5, 1, .125/.5, 30, 0, 1, 1).
        // Computed from those explicit wire bytes, not the production identity helper.
        std::array<matter::GpuMaterialProperties, 2u> identity_rows{
            {{11u, matter::ESourceMaterial::liquid, 1.0f, 1.5f, 1.0f, 0.125f, 30.0f, 0.0f, 1.0f, 1.0f},
                {12u, matter::ESourceMaterial::liquid, 1.0f, 1.5f, 1.0f, 0.5f, 30.0f, 0.0f, 1.0f, 1.0f}}};
        constexpr u64 expected_identity = 0xd505ee7f0438172eull;
        PPR_TEST_ASSERT(matterMaterialTableIdentity(identity_rows, 1u) == expected_identity);
        const auto current_identity = matterMaterialTableIdentity(identity_rows);
        std::swap(identity_rows[0u], identity_rows[1u]);
        PPR_TEST_ASSERT(matterMaterialTableIdentity(identity_rows, 1u) == expected_identity);
        PPR_TEST_ASSERT(matterMaterialTableIdentity(identity_rows) == current_identity);

        const auto encoded = encodeWorldCheckpoint(checkpoint, catalog);
        const auto raw = sim::save(checkpoint.m_snapshot);
        PPR_TEST_ASSERT(encoded and raw);

        // CPU and GPU fixtures share the suffix-derived offsets and historical
        // policy/identity/record reconstruction, including pre-decode size guards.
        const std::size_t table_bytes = 16u + checkpoint.m_current_matter.m_properties.size() * 76u;
        const auto offsets = checkpointWireOffsets(encoded->size(), raw->size(), checkpoint.m_current_matter);
        const std::size_t table_offset = offsets.m_properties;
        const std::size_t schedule_offset = offsets.m_schedule;
        const auto v8 = historicalCheckpointBytes(*encoded, raw->size(), checkpoint.m_current_matter, 8u);
        const auto migrated_v8 = decodeWorldCheckpoint(v8, catalog);
        PPR_TEST_ASSERT(migrated_v8 and migrated_v8->m_current_matter.m_iterations == 5u);

        const auto v7 = historicalCheckpointBytes(*encoded, raw->size(), checkpoint.m_current_matter, 7u);
        const auto migrated_v7 = decodeWorldCheckpoint(v7, catalog);
        PPR_TEST_ASSERT(migrated_v7 and migrated_v7->m_current_matter.m_iterations == 5u);

        auto current = checkpoint;
        current.m_current_matter.m_iterations = 10u;
        PPR_TEST_ASSERT(current.m_current_matter.m_iterations != matter::kDefaultIterations);
        const auto v9 = encodeWorldCheckpoint(current, catalog);
        PPR_TEST_ASSERT(v9.has_value());
        const auto migrated_v9 = decodeWorldCheckpoint(*v9, catalog);
        PPR_TEST_ASSERT(migrated_v9 and migrated_v9->m_current_matter.m_iterations == 10u);

        const auto legacy = historicalCheckpointBytes(*encoded, raw->size(), checkpoint.m_current_matter, 6u);
        const auto migrated = decodeWorldCheckpoint(legacy, catalog);
        PPR_TEST_ASSERT(migrated.has_value());
        PPR_TEST_ASSERT(migrated->m_current_matter.m_iterations == 2u);
        const auto *const material = catalog.findMaterial(checkpoint.m_current_matter.m_records[0u].m_material);
        PPR_TEST_ASSERT(material != nullptr);

        // editedCheckpoint carries water (liquid), with the fixture's 1 kg/m^3
        // inventory density. Coefficients are the legacy GpuParameters defaults;
        // the pre-B1 sand yield law used elasticity_ratio, also 1.0.
        const matter::GpuMaterialProperties legacy_row{
            .m_substance_id = material->m_numeric_id,
            .m_family = matter::ESourceMaterial::liquid,
            .m_elasticity_ratio = 1.0f,
            .m_elastic_relaxation = 1.5f,
            .m_liquid_relaxation = 1.0f,
            .m_liquid_viscosity = 0.01f,
            .m_friction_angle = 30.0f,
            .m_plasticity = 0.0f,
            .m_reference_density = static_cast<float>(material->m_reference_density_kg_m3 / 1000.0),
            .m_sand_yield_ratio = 1.0f};

        for (const auto *const migrated_state:
            {&migrated->m_current_matter, &migrated_v7->m_current_matter, &migrated_v8->m_current_matter,
                &migrated_v9->m_current_matter}) {
            PPR_TEST_ASSERT(migrated_state->m_properties.size() == 1u);
            // Word 1 is the solver family, not the table-level policy or identity.
            PPR_TEST_ASSERT(migrated_state->m_properties[0u].m_family == matter::ESourceMaterial::liquid);
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 19u>>(migrated_state->m_properties[0u]) ==
                std::bit_cast<std::array<u32, 19u>>(legacy_row));
            PPR_TEST_ASSERT(migrated_state->m_records == checkpoint.m_current_matter.m_records);
            PPR_TEST_ASSERT(migrated_state->m_particles.size() == checkpoint.m_current_matter.m_particles.size());
            PPR_TEST_ASSERT(
                std::bit_cast<std::array<u32, 24u>>(migrated_state->m_particles[0u]) ==
                std::bit_cast<std::array<u32, 24u>>(checkpoint.m_current_matter.m_particles[0u]));
        }

        for (const auto *const migrated_checkpoint: {&*migrated, &*migrated_v7, &*migrated_v8, &*migrated_v9}) {
            const auto recoded = encodeWorldCheckpoint(*migrated_checkpoint, catalog);
            PPR_TEST_ASSERT(recoded and (*recoded)[8u] == 9u);
        }

        PPR_TEST_ASSERT(migrated->m_current_matter.m_records == checkpoint.m_current_matter.m_records);
        PPR_TEST_ASSERT(
            std::bit_cast<std::array<u32, 24u>>(migrated->m_current_matter.m_particles[0u]) ==
            std::bit_cast<std::array<u32, 24u>>(checkpoint.m_current_matter.m_particles[0u]));

        const auto upgraded = encodeWorldCheckpoint(*migrated, catalog);
        PPR_TEST_ASSERT(
            upgraded and
            upgraded->size() == legacy.size() + 4u + table_bytes + checkpoint.m_current_matter.m_particles.size() * 8u);
        const auto upgraded_state = decodeWorldCheckpoint(*upgraded, catalog);
        PPR_TEST_ASSERT(upgraded_state and upgraded_state->m_current_matter.m_iterations == 2u);

        auto live = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 93u);
        PPR_TEST_ASSERT(live.has_value());
        PPR_DEFER {
            PPR_TEST_ASSERT(not (*live)->shutdown());
        };

        const auto before = captureWorldCheckpoint(**live, (*live)->preparedWorld()->generated());
        PPR_TEST_ASSERT(before.has_value());
        const auto before_bytes = encodeWorldCheckpoint(*before, catalog);
        PPR_TEST_ASSERT(before_bytes.has_value());

        for (const std::size_t offset: {table_offset, table_offset + 4u}) {
            auto mismatch = *encoded;
            mismatch[offset] ^= 1u;
            const auto rejected = decodeWorldCheckpoint(mismatch, catalog);
            PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::protocol_not_supported);
        }

        auto roundtrip = decodeWorldCheckpoint(*encoded, catalog);
        PPR_TEST_ASSERT(roundtrip.has_value());
        PPR_TEST_ASSERT(
            matterMaterialTableIdentity(roundtrip->m_current_matter.m_properties) ==
            matterMaterialTableIdentity(checkpoint.m_current_matter.m_properties));
        PPR_TEST_ASSERT(
            std::memcmp(
                roundtrip->m_current_matter.m_properties.data(), checkpoint.m_current_matter.m_properties.data(),
                checkpoint.m_current_matter.m_properties.size() * sizeof(matter::GpuMaterialProperties)) == 0);

        for (u32 invalid_case = 0u; invalid_case < 3u; ++invalid_case) {
            auto candidate = checkpoint.m_current_matter;
            auto rows = candidate.m_properties;
            if (invalid_case == 0u)
                rows[0u].m_substance_id = std::numeric_limits<u32>::max();
            if (invalid_case == 1u)
                rows.push_back(rows.front());
            if (invalid_case == 2u)
                rows[0u].m_friction_angle = 90.0f;

            PPR_TEST_ASSERT(setMatterProperties(candidate, rows, catalog) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(
                matterMaterialTableIdentity(candidate.m_properties) ==
                matterMaterialTableIdentity(checkpoint.m_current_matter.m_properties));
            PPR_TEST_ASSERT(candidate.m_identity == checkpoint.m_current_matter.m_identity);

            auto invalid = checkpoint;
            invalid.m_current_matter.m_properties = rows;
            const auto refused = restoreWorldCheckpoint(fixture.m_catalog, std::move(invalid), 94u);
            PPR_TEST_ASSERT(not refused and refused.error() == std::errc::invalid_argument);

            // This is a separate candidate restore: it proves that the live
            // driver's capture is unchanged, not live GPU admission or rollback.
            const auto after = captureWorldCheckpoint(**live, (*live)->preparedWorld()->generated());
            PPR_TEST_ASSERT(after.has_value());
            const auto after_bytes = encodeWorldCheckpoint(*after, catalog);
            PPR_TEST_ASSERT(after_bytes and *after_bytes == *before_bytes);
        }

        for (const u32 iterations: {0u, 1u, 3u, 7u}) {
            auto malformed = *encoded;
            malformed[schedule_offset] = static_cast<u8>(iterations);
            const auto decoded = decodeWorldCheckpoint(malformed, catalog);
            PPR_TEST_ASSERT(not decoded and decoded.error() == std::errc::invalid_argument);

            auto invalid = checkpoint;
            invalid.m_current_matter.m_iterations = iterations;
            PPR_TEST_ASSERT(validateWorldCheckpoint(invalid, catalog) == std::errc::invalid_argument);
            const auto rejected = restoreWorldCheckpoint(fixture.m_catalog, std::move(invalid), 94u);
            PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::invalid_argument);

            const auto after = captureWorldCheckpoint(**live, (*live)->preparedWorld()->generated());
            PPR_TEST_ASSERT(after.has_value());
            const auto after_bytes = encodeWorldCheckpoint(*after, catalog);
            PPR_TEST_ASSERT(after_bytes and *after_bytes == *before_bytes);
            PPR_TEST_ASSERT((*live)->matterState()->m_iterations == 5u);
        }
    };

    PPR_UNIT_TEST (selected_world_checkpoint_preserves_extent_and_rejects_mismatched_owners) {
        // This edited sparse fixture proves codec/restore coherence, not full authored generation.
        Array<u64> fingerprints;
        for (const u32 edge: {1024u, 2048u, 4096u}) {
            Fixture fixture{"checkpoint_size", "frontier", edge};
            const auto &catalog = *fixture.m_catalog;
            const auto checkpoint = editedCheckpoint(catalog);
            fingerprints.push_back(checkpoint.m_descriptor.m_content_fingerprint);
            PPR_TEST_ASSERT(checkpoint.m_descriptor.m_world_edge == edge);
            PPR_TEST_ASSERT(checkpoint.m_snapshot.m_world_edge == edge);
            PPR_TEST_ASSERT(checkpoint.m_current_matter.m_world_edge == edge);
            PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, catalog));
            const auto encoded = encodeWorldCheckpoint(checkpoint, catalog);
            PPR_TEST_ASSERT(encoded.has_value());
            const auto decoded = decodeWorldCheckpoint(*encoded, catalog);
            PPR_TEST_ASSERT(decoded and decoded->m_descriptor == checkpoint.m_descriptor);
            PPR_TEST_ASSERT(decoded->m_snapshot == checkpoint.m_snapshot);
            PPR_TEST_ASSERT(decoded->m_current_matter.m_world_edge == edge);
            PPR_TEST_ASSERT(decoded->m_current_matter.m_profile.m_grid_width == edge + 7u);

            auto restored = restoreWorldCheckpoint(fixture.m_catalog, *decoded, 101u);
            PPR_TEST_ASSERT(restored.has_value());
            PPR_DEFER {
                std::ignore = (*restored)->shutdown();
            };
            PPR_TEST_ASSERT((*restored)->grid().edge() == edge);
            PPR_TEST_ASSERT(
                (*restored)->grid().getCell({127u, 40u})->m_element == catalog.findMaterial("basalt")->m_numeric_id);
            PPR_TEST_ASSERT(not (*restored)->grid().getCell({edge, 0u}));
            PPR_TEST_ASSERT((*restored)->matterState()->m_world_edge == edge);
            PPR_TEST_ASSERT(not (*restored)->shutdown());

            const u32 other_edge = edge == 1024u ? 2048u : 1024u;
            auto bad = checkpoint;
            bad.m_snapshot.m_world_edge = other_edge;
            PPR_TEST_ASSERT(not encodeWorldCheckpoint(bad, catalog));
            PPR_TEST_ASSERT(not restoreWorldCheckpoint(fixture.m_catalog, bad, 102u));
            bad = checkpoint;
            bad.m_current_matter.m_world_edge = other_edge;
            PPR_TEST_ASSERT(not encodeWorldCheckpoint(bad, catalog));
            bad = checkpoint;
            bad.m_descriptor.m_world_edge = other_edge;
            PPR_TEST_ASSERT(not encodeWorldCheckpoint(bad, catalog));
            bad = checkpoint;
            bad.m_descriptor.m_world_edge = 1536u;
            PPR_TEST_ASSERT(not encodeWorldCheckpoint(bad, catalog));

            // The descriptor edge follows the two u64 identities and metric block.
            const std::size_t edge_offset =
                25u + checkpoint.m_descriptor.m_profile.size() + 2u * sizeof(u64) + 2u * sizeof(u32) + sizeof(double);
            for (u32 byte = 0u; byte < sizeof(u32); ++byte) {
                PPR_TEST_ASSERT((*encoded)[edge_offset + byte] == static_cast<u8>(edge >> (byte * 8u)));
            }

            auto malformed = *encoded;
            for (u32 byte = 0u; byte < sizeof(u32); ++byte) {
                malformed[edge_offset + byte] = static_cast<u8>(1536u >> (byte * 8u));
            }

            PPR_TEST_ASSERT(not decodeWorldCheckpoint(malformed, catalog));
            PPR_TEST_ASSERT(not validateWorldCheckpoint(checkpoint, catalog));
        }

        std::ranges::sort(fingerprints);
        PPR_TEST_ASSERT(std::ranges::adjacent_find(fingerprints) == fingerprints.end());
    };

    PPR_UNIT_TEST (direct_restored_driver_rejects_matter_and_snapshot_extent_mismatch_before_publication) {
        Fixture fixture{"checkpoint_direct", "frontier", 1024u};
        const auto checkpoint = editedCheckpoint(*fixture.m_catalog);
        const auto prepare = [&]() -> PreparedWorld {
            auto plan = compileWorldPlan(
                *fixture.m_catalog, checkpoint.m_descriptor.m_profile, checkpoint.m_descriptor.m_seed);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = sim::restoreBiomeLayout(*plan, checkpoint.m_sites);
            PPR_TEST_ASSERT(layout.has_value());
            sim::ChunkGrid grid{1024u};
            PPR_TEST_ASSERT(not sim::apply(grid, checkpoint.m_snapshot));

            sim::GeneratedWorld world{
                std::move(*plan), std::move(*layout), checkpoint.m_geometry, std::move(grid), checkpoint.m_matter,
                checkpoint.m_start, checkpoint.m_report};
            auto prepared = restorePreparedWorld(fixture.m_catalog, checkpoint.m_descriptor, std::move(world));
            PPR_TEST_ASSERT(prepared.has_value());
            return std::move(*prepared);
        };

        auto larger_matter = makeMatterState(
            checkpoint.m_particles, kFineCellMetres, kReferenceThicknessMetres, kMatterGravityY, 1u, 4096u);
        PPR_TEST_ASSERT(larger_matter.has_value());
        larger_matter->m_identity.m_terrain_revision = 1u;
        PPR_TEST_ASSERT(not validateMatterState(
            *larger_matter, fixture.m_catalog->selectedRecipe()->m_recipe.m_materials, kFineCellMetres,
            kReferenceThicknessMetres, kMatterGravityY));

        ColonyDriver driver;
        PPR_DEFER {
            std::ignore = driver.shutdown();
        };

        const ColonyDriverDesc description{
            .m_seed = checkpoint.m_descriptor.m_seed,
            .m_tick_hz = checkpoint.m_tick_hz,
            .m_agent_count = 1u,
            .m_world_generation = 201u,
            .m_world_edge = 1024u};
        const auto wrong_matter = driver.initRestored(
            description, prepare(), checkpoint.m_snapshot, checkpoint.m_particles, *larger_matter, checkpoint.m_tick,
            checkpoint.m_initial_matter_valid, checkpoint.m_controlled);
        PPR_TEST_ASSERT(wrong_matter == std::errc::invalid_argument);
        PPR_TEST_ASSERT(driver.preparedWorld() == nullptr and driver.matterState() == nullptr);
        PPR_TEST_ASSERT(driver.contentCatalog() == nullptr);

        auto larger_snapshot = checkpoint.m_snapshot;
        larger_snapshot.m_world_edge = 2048u;
        const auto wrong_snapshot = driver.initRestored(
            description, prepare(), larger_snapshot, checkpoint.m_particles, checkpoint.m_current_matter,
            checkpoint.m_tick, checkpoint.m_initial_matter_valid, checkpoint.m_controlled);
        PPR_TEST_ASSERT(wrong_snapshot == std::errc::invalid_argument);
        PPR_TEST_ASSERT(driver.preparedWorld() == nullptr and driver.matterState() == nullptr);
        PPR_TEST_ASSERT(driver.contentCatalog() == nullptr);

        PPR_TEST_ASSERT(not driver.shutdown());
    };

    PPR_UNIT_TEST (qualified_world_profiles_round_trip_at_the_authoritative_name_limit) {
        Fixture fixture{
            std::string(asset::QualifiedName::maximum_namespace_bytes, 'p'), std::string(config::kMaxKeyBytes, 'w')};
        const auto &catalog = *fixture.m_catalog;
        auto checkpoint = editedCheckpoint(catalog);
        PPR_TEST_ASSERT(
            checkpoint.m_descriptor.m_profile.size() == asset::QualifiedName::reference_domain.m_max_length);

        const auto bytes = encodeWorldCheckpoint(checkpoint, catalog);
        PPR_TEST_ASSERT(bytes.has_value());

        const auto decoded = decodeWorldCheckpoint(*bytes, catalog);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(decoded->m_descriptor.m_profile == checkpoint.m_descriptor.m_profile);

        auto oversized = *bytes;
        const auto length = static_cast<u32>(asset::QualifiedName::reference_domain.m_max_length + 1u);
        for (u32 byte = 0u; byte < 4u; ++byte) {
            oversized[21u + byte] = static_cast<u8>(length >> (byte * 8u));
        }

        const auto rejected = decodeWorldCheckpoint(oversized, catalog);
        PPR_TEST_ASSERT(not rejected and rejected.error() == sim::errc::invalid_payload);

        checkpoint.m_descriptor.m_profile += 'w';
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::protocol_not_supported);

        checkpoint.m_descriptor.m_profile = fixture.m_world_key;
        PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, catalog) == std::errc::protocol_not_supported);
    };

    // ------------------------------------------------------------------
    // selected runtime parameters and optional ready presentation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (runtime_generation_and_restore_reject_each_noncanonical_compile_parameter) {
        struct Clock final : ITimerClock {
            mutable u32 m_calls{};

            TimePoint now() const noexcept override {
                ++m_calls;
                return {};
            }
        } clock;

        Fixture fixture;
        std::array alternatives{
            kWorldCompileParameters, kWorldCompileParameters, kWorldCompileParameters, kWorldCompileParameters};
        alternatives[0].m_cell_size_m *= 1.5;
        alternatives[1].m_reference_thickness_m *= 2.0;
        alternatives[2].m_actor_half_width_m *= 0.75;
        alternatives[3].m_actor_half_height_m *= 0.75;
        config::Diagnostics diagnostics;

        for (const auto parameters: alternatives) {
            auto selected = selectWorldCatalog(
                fixture.m_catalog->descriptorOwner(), fixture.m_catalog->worldId(), parameters, diagnostics);
            PPR_TEST_ASSERT(selected.has_value());

            auto catalog = std::make_shared<const ContentCatalog>(std::move(*selected));
            auto checkpoint = editedCheckpoint(*catalog);

            auto plan = compileWorldPlan(*catalog, checkpoint.m_descriptor.m_profile, checkpoint.m_descriptor.m_seed);
            PPR_TEST_ASSERT(plan.has_value());
            PPR_TEST_ASSERT(plan->recipe().m_cell_size_m == parameters.m_cell_size_m);
            PPR_TEST_ASSERT(plan->recipe().m_reference_thickness_m == parameters.m_reference_thickness_m);
            PPR_TEST_ASSERT(plan->recipe().m_actor_half_width_m == parameters.m_actor_half_width_m);
            PPR_TEST_ASSERT(plan->recipe().m_actor_half_height_m == parameters.m_actor_half_height_m);

            const auto generated = prepareWorld(catalog, checkpoint.m_descriptor.m_profile, 42u, clock);
            PPR_TEST_ASSERT(not generated and generated.error() == std::errc::not_supported);
            PPR_TEST_ASSERT(clock.m_calls == 0u);
            PPR_TEST_ASSERT(validateWorldCheckpoint(checkpoint, *catalog) == std::errc::not_supported);

            // Unsupported runtime parameters precede saved-layout and cell restoration.
            checkpoint.m_sites.clear();
            checkpoint.m_snapshot.m_deltas.front().m_cells.front().m_element = 60000u;
            const auto restored = restoreWorldCheckpoint(catalog, checkpoint, 1u);
            PPR_TEST_ASSERT(not restored and restored.error() == std::errc::not_supported);

            auto layout = sim::compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());

            sim::GeneratedWorld numeric{std::move(*plan), std::move(*layout), {}, {}, {}, {}, {}};
            const auto prepared = restorePreparedWorld(catalog, checkpoint.m_descriptor, std::move(numeric));
            PPR_TEST_ASSERT(not prepared and prepared.error() == std::errc::not_supported);
        }
    };

    PPR_UNIT_TEST (numeric_restore_omits_images_and_render_restore_requires_matching_owner_and_generation) {
        std::optional<SimFramePacket> retained;
        std::shared_ptr<const ContentPresentation> retained_images;

        {
            Fixture fixture;
            const auto checkpoint = editedCheckpoint(*fixture.m_catalog);
            auto numeric = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 5u);
            PPR_TEST_ASSERT(numeric.has_value());
            PPR_TEST_ASSERT(not (*numeric)->preparedWorld()->presentation());

            const auto unavailable = makeSimFramePacket(1u, **numeric);
            PPR_TEST_ASSERT(not unavailable and unavailable.error() == std::errc::invalid_argument);

            const auto uncaptured = captureWorldPresentation(**numeric);
            PPR_TEST_ASSERT(not uncaptured and uncaptured.error() == std::errc::invalid_argument);

            PPR_TEST_ASSERT(not (*numeric)->shutdown());

            const auto images = fixture.presentation(5u);
            const auto stale = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 6u, {}, images);
            PPR_TEST_ASSERT(not stale and stale.error() == std::errc::invalid_argument);

            const auto equivalent = std::make_shared<const ContentCatalog>(*fixture.m_catalog);
            const auto foreign_images = fixture.presentation(5u, equivalent);
            const auto foreign = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 5u, {}, foreign_images);
            PPR_TEST_ASSERT(not foreign and foreign.error() == std::errc::invalid_argument);

            auto ready = restoreWorldCheckpoint(fixture.m_catalog, checkpoint, 5u, {}, images);
            PPR_TEST_ASSERT(ready.has_value());
            PPR_TEST_ASSERT((*ready)->preparedWorld()->catalogOwner() == fixture.m_catalog);

            auto packet = makeSimFramePacket(2u, **ready);
            PPR_TEST_ASSERT(packet.has_value() and not packet->m_terrain_images.empty());
            PPR_TEST_ASSERT(packet->m_world_generation == 5u);

            retained.emplace(std::move(*packet));
            retained_images = images;
            PPR_TEST_ASSERT(not (*ready)->shutdown());
        }

        PPR_TEST_ASSERT(retained->m_terrain_images.front().m_storage.isOwned());
        PPR_TEST_ASSERT(not retained->m_terrain_images.front().m_storage.getBufferData().empty());
        PPR_TEST_ASSERT(retained_images->catalog()->worlds().front().m_key == "checkpoint:frontier");
    };
}

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // checkpoint tests
    // ------------------------------------------------------------------

    const UnitTest checkpoint_tests = UnitTest::Named("checkpoint") / [](UnitTest::IRun &_) {
        using namespace detail::checkpoint;

        _.recurse(
            {terrain_masks_match_independent_scatter_for_residency_materials_seams_and_borders,
                generator_seven_checkpoint_is_incompatible_before_material_id_restore,
                diagnostic_export_rejects_cancellation_and_unwritable_directory,
                ui_model_preserves_authored_metadata_and_nearest_agent_for_unresident_cells,
                round_trip_restores_edited_cells_actor_identity_and_frozen_full_capture,
                restored_current_matter_is_frozen_until_attached_and_keeps_physical_inventory,
                dig_candidate_preserves_carried_columns_and_emits_original_seam_inventory_once,
                dug_and_seeded_material_families_initialize_from_properties,
                destroyed_solids_use_calibrated_families_and_initial_history,
                new_contour_coverage_rejects_moving_and_gas_but_preserves_prior_overlap,
                current_round_trip_preserves_all_words_and_rebinds_only_world,
                current_particle_wire_payload_is_the_exact_semantic_word_sequence,
                current_wire_rejects_unsupported_liquid_relaxation,
                malformed_identity_metric_counts_and_payload_are_rejected_before_restore,
                checkpoint_blob_references_ranges_and_wire_counts_are_checked,
                envelope_seven_manifest_one_and_raw_snapshot_five_have_distinct_headers,
                saved_schedule_migrates_v6_and_rejects_invalid_counts_without_mutating_live_world,
                qualified_world_profiles_round_trip_at_the_authoritative_name_limit,
                selected_world_checkpoint_preserves_extent_and_rejects_mismatched_owners,
                direct_restored_driver_rejects_matter_and_snapshot_extent_mismatch_before_publication,
                runtime_generation_and_restore_reject_each_noncanonical_compile_parameter,
                numeric_restore_omits_images_and_render_restore_requires_matching_owner_and_generation});
    };

    const UnitTest &gameCheckpointTests() noexcept {
        return checkpoint_tests;
    }
}
