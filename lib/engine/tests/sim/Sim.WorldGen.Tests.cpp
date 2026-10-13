module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.sim;

import std;

namespace pP::tests::detail {
    namespace WorldGen {
        using namespace pP::sim;

        [[nodiscard]] GenerationRecipe numericRecipe() {
            GenerationRecipe recipe;
            recipe.m_materials.push_back({17u, true, true, EGenerationMatter::solid, 2500.0});
            recipe.m_fields.push_back({101u, {.m_frequency_per_metre = 0.025}});
            GenerationBand band{.m_key = 201u, .m_depth_max = 1.0};
            band.m_choices.push_back({202u, 17u, 1.0});
            GenerationBiome biome{.m_key = 203u, .m_temperature_base_k = 293.0, .m_temperature_variation_k = 8.0};
            biome.m_bands.push_back(std::move(band));
            recipe.m_biomes.push_back(std::move(biome));
            GenerationFeature feature{.m_key = 301u, .m_bounds = {0, 0, 30, 30}};
            feature.m_operations.push_back({.m_key = 302u, .m_carve = true, .m_width = 30u, .m_height = 30u});
            feature.m_markers.push_back({303u, EGenerationMarker::start, 15, 10});
            recipe.m_features.push_back(std::move(feature));
            recipe.m_constraints.push_back({401u, 0u, 1u, 1.0, 0.0, 1.0, 0.0, 2048.0});
            recipe.m_cell_size_m = 1.0 / 3.0;
            recipe.m_reference_thickness_m = 1.0;
            recipe.m_boundary_material = 17u;
            recipe.m_boundary_fine = 3u;
            recipe.m_min_sites = 4u;
            recipe.m_max_sites = 16u;
            recipe.m_site_attempts = 30u;
            recipe.m_site_spacing_m = 100.0;
            recipe.m_start_x_fine = 2000u;
            recipe.m_start_y_fine = 3000u;
            recipe.m_start_clearance_m = 3.0;
            recipe.m_start_route_width_m = 1.0;
            recipe.m_actor_half_width_m = 0.4;
            recipe.m_actor_half_height_m = 0.9;
            return recipe;
        }

        PPR_UNIT_TEST (generation_random_has_fixed_full_width_streams) {
            GenerationRandom random{0u};
            PPR_TEST_ASSERT(random.next() == 0xe220a8397b1dcdafull);
            PPR_TEST_ASSERT(random.next() == 0x6e789e6aa1b965f4ull);
            PPR_TEST_ASSERT(random.next() == 0x06c45d188009454full);
            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                GenerationRandom first{generationStreamSeed(seed, 101u, 7u)};
                GenerationRandom replay{generationStreamSeed(seed, 101u, 7u)};
                GenerationRandom other{generationStreamSeed(seed, 101u, 8u)};
                const u64 first_draw = first.next();
                PPR_TEST_ASSERT(first_draw == replay.next());
                PPR_TEST_ASSERT(first_draw != other.next());
                for (const u64 bound: {1ull, 3ull, 17ull, 0x8000000000000001ull, 0xffffffffffffffffull}) {
                    for (u32 i = 0u; i < 100u; ++i) {
                        PPR_TEST_ASSERT(first.bounded(bound) < bound);
                    }
                }
                for (u32 i = 0u; i < 100u; ++i) {
                    const double value = first.unit();
                    PPR_TEST_ASSERT(value >= 0.0 and value < 1.0);
                }
            }
        };
        PPR_UNIT_TEST (numeric_plan_owns_recipe_and_isolates_field_streams) {
            auto input = numericRecipe();
            auto first = compileGenerationPlan(input, 42u);
            PPR_TEST_ASSERT(first.has_value());
            input.m_fields.insert(input.m_fields.begin(), {999u, {.m_frequency_per_metre = 0.1}});
            input.m_biomes[0u].m_temperature_field = 1u;
            input.m_biomes[0u].m_bands[0u].m_field = 1u;
            auto second = compileGenerationPlan(std::move(input), 42u);
            PPR_TEST_ASSERT(second.has_value());
            PPR_TEST_ASSERT(first->recipe().m_fields.size() == 1u and first->seed() == 42u);
            for (u32 i = 0u; i < 100u; ++i) {
                PPR_TEST_ASSERT(
                    first->fields()[0u].sample(i * 2.75, 17.0) == second->fields()[1u].sample(i * 2.75, 17.0));
            }
        };
        PPR_UNIT_TEST (numeric_plan_rejects_invalid_references_bounds_and_collapsed_weights) {
            const auto rejects = [](const auto &mutate) {
                auto recipe = numericRecipe();
                mutate(recipe);
                PPR_TEST_ASSERT(not compileGenerationPlan(std::move(recipe), 42u));
            };
            rejects([](auto &recipe) { recipe.m_biomes[0u].m_temperature_field = 1u; });
            rejects([](auto &recipe) { recipe.m_biomes[0u].m_erosion_passes = 25u; });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_relaxation_passes = 17u;
                recipe.m_biomes[0u].m_erosion_passes = 8u;
            });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_relaxation_passes = (std::numeric_limits<u32>::max)();
                recipe.m_biomes[0u].m_pore_passes = 1u;
            });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_relaxation_passes = 20u;
                recipe.m_biomes[0u].m_pore_passes = 3u;
                recipe.m_biomes[0u].m_finish_passes = 2u;
            });
            rejects([](auto &recipe) { recipe.m_fields.push_back(recipe.m_fields[0u]); });
            rejects([](auto &recipe) { recipe.m_biomes[0u].m_lining_material = 17u; });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_lining_material = 17u;
                recipe.m_biomes[0u].m_lining_thickness_fine = 4u;
            });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_lining_material = 99u;
                recipe.m_biomes[0u].m_lining_thickness_fine = 1u;
            });
            rejects([](auto &recipe) { recipe.m_materials[0u].m_id = 0u; });
            rejects([](auto &recipe) { recipe.m_biomes[0u].m_bands[0u].m_choices[0u].m_material = 0u; });
            rejects([](auto &recipe) { recipe.m_max_sites = kMaxGenerationSites + 1u; });
            rejects([](auto &recipe) {
                recipe.m_features[0u].m_bounds = {
                    (std::numeric_limits<i32>::min)(), 0, (std::numeric_limits<i32>::min)() + 30, 30};
                recipe.m_start_x_fine = 2147485648u;
            });
            rejects([](auto &recipe) { recipe.m_site_spacing_m = 1.0e-200; });
            rejects([](auto &recipe) { recipe.m_start_x_fine = 4090u; });
            rejects([](auto &recipe) { recipe.m_actor_half_width_m = std::numeric_limits<double>::quiet_NaN(); });
            rejects([](auto &recipe) { recipe.m_features[0u].m_operations[0u].m_width = 31u; });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_bands[0u].m_choices.push_back(
                    {999u, 17u, std::numeric_limits<double>::denorm_min()});
            });
            rejects([](auto &recipe) {
                recipe.m_constraints[0u].m_weight = std::numeric_limits<double>::denorm_min();
                recipe.m_biomes.push_back(recipe.m_biomes[0u]);
                recipe.m_biomes.back().m_key = 999u;
                recipe.m_constraints.push_back(
                    {999u, 1u, 0u, (std::numeric_limits<double>::max)(), 0.0, 1.0, 0.0, 2048.0});
            });
        };
        PPR_UNIT_TEST (numeric_plan_accepts_granular_fills_and_exact_signed_thirds) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({1001u, false, false, EGenerationMatter::solid, 1800.0});
            recipe.m_features[0u].m_bounds.m_min_y = -30;
            recipe.m_features[0u].m_markers.push_back({999u, EGenerationMarker::sealed_pocket, 15, 15});
            recipe.m_rules.push_back(
                {.m_key = 501u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{.m_max_count = 1u, .m_attempts = 1u}});
            recipe.m_rules.push_back(
                {.m_key = 502u,
                    .m_phase = EGenerationPhase::fill,
                    .m_parameters = GenerationFill{
                        .m_marker = 1u,
                        .m_material = 1001u,
                        .m_temperature_k = 290.0,
                        .m_bulk_density_kg_m3 = 1800.0,
                        .m_fill_height_m = 5.0 / 3.0}});
            for (const double height: {5.0 / 3.0, -5.0 / 3.0}) {
                auto &fill = std::get<GenerationFill>(recipe.m_rules[1u].m_parameters);
                fill.m_fill_height_m = height;
                PPR_TEST_ASSERT(not validateGenerationRecipe(recipe));
                fill.m_fill_height_m = std::nextafter(height, 0.0);
                PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::invalid_argument);
            }
            recipe.m_rules.clear();
            recipe.m_rules.push_back(
                {.m_key = 503u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationWorm{
                        .m_count = 1u,
                        .m_carve = true,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 0.1,
                        .m_length_max_m = 0.1,
                        .m_radius_min_m = 0.1,
                        .m_radius_max_m = 0.1}});
            recipe.m_materials[0u].m_reference_density_kg_m3 = 1.0e100;
            recipe.m_biomes[0u].m_temperature_base_k = 1.0e30;
            PPR_TEST_ASSERT(not validateGenerationRecipe(recipe));
        };
        PPR_UNIT_TEST (numeric_plan_preserves_authored_geometry_boundaries) {
            auto recipe = numericRecipe();
            recipe.m_features[0u].m_operations[0u] = {
                .m_key = 302u,
                .m_shape = EGenerationShape::capsule,
                .m_carve = true,
                .m_x = 7,
                .m_y = 7,
                .m_end_x = 20,
                .m_end_y = 20,
                .m_radius_m = 7.0 / 3.0};
            PPR_TEST_ASSERT(not validateGenerationRecipe(recipe));
            recipe.m_features[0u].m_bounds = {0, 0, 2000, 2000};
            auto &operation = recipe.m_features[0u].m_operations[0u];
            operation.m_x = 900;
            operation.m_y = 900;
            operation.m_end_x = 1100;
            operation.m_end_y = 1100;
            operation.m_radius_m = 300.0;
            recipe.m_start_x_fine = 1000u;
            recipe.m_start_y_fine = 1000u;
            recipe.m_start_clearance_m = 300.0;
            recipe.m_start_route_width_m = 300.0;
            recipe.m_features[0u].m_markers.push_back({999u, EGenerationMarker::resource_anchor, 20, 10});
            recipe.m_resources.push_back({999u, 1u, 1u, 17u, 2000.0});
            PPR_TEST_ASSERT(not validateGenerationRecipe(recipe));
        };

        PPR_UNIT_TEST (biome_lining_stops_threshold_caves_and_replays_across_chunks) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_biomes.push_back(recipe.m_biomes.front());
            recipe.m_biomes.back().m_key = 204u;
            recipe.m_constraints.push_back({402u, 1u, 1u, 1.0, 0.0, 1.0, 0.0, 2048.0});
            recipe.m_rules.push_back({501u, 1u, EGenerationPhase::carve, GenerationCave{0u, 0u, 0.0, 1.0, -1.0, 1.0}});
            recipe.m_rules.push_back({502u, 2u, EGenerationPhase::carve, GenerationCave{1u, 0u, 0.0, 1.0, -1.0, 1.0}});
            // Four surrounding sites make a single-cell different-biome island.
            // Diagonal distance two/three cannot be detected by cardinal rays.
            const std::array sites{
                GenerationSite{125.5, 127.5, 0u}, GenerationSite{129.5, 127.5, 0u}, GenerationSite{127.5, 125.5, 0u},
                GenerationSite{127.5, 129.5, 0u}, GenerationSite{127.5, 127.5, 1u}};
            for (const u32 width: {1u, 2u, 3u}) {
                recipe.m_biomes.front().m_lining_material = 17u;
                recipe.m_biomes.front().m_lining_thickness_fine = width;
                recipe.m_biomes.front().m_relaxation_passes = 3u;
                recipe.m_biomes.front().m_relaxation_adoption = 1.0;
                const auto plan = compileGenerationPlan(recipe, 42u);
                PPR_TEST_ASSERT(plan.has_value());
                const auto layout = restoreBiomeLayout(*plan, sites);
                PPR_TEST_ASSERT(layout.has_value());
                const auto geometry = compileGenerationGeometry(*plan, *layout);
                PPR_TEST_ASSERT(geometry.has_value());
                const auto raster = compileGenerationRaster(*plan, *layout, *geometry);
                PPR_TEST_ASSERT(raster.has_value());
                const auto left = raster->chunk({0u, 0u});
                const auto right = raster->chunk({1u, 0u});
                const auto replay = raster->chunk({1u, 0u});
                PPR_TEST_ASSERT(left.has_value() and right.has_value() and replay.has_value());
                PPR_TEST_ASSERT((*left)[126u * kChunkEdge + 126u].m_element == (width >= 2u ? 17u : 0u));
                PPR_TEST_ASSERT((*left)[126u * kChunkEdge + 125u].m_element == (width >= 3u ? 17u : 0u));
                PPR_TEST_ASSERT((*left)[126u * kChunkEdge + 127u].m_element == 17u);
                PPR_TEST_ASSERT((*right)[126u * kChunkEdge].m_element == (width >= 2u ? 17u : 0u));
                PPR_TEST_ASSERT(*right == *replay);
                for (const ChunkPos corner: {ChunkPos{0u, 31u}, ChunkPos{31u, 0u}, ChunkPos{31u, 31u}}) {
                    const auto cells = raster->chunk(corner);
                    PPR_TEST_ASSERT(cells.has_value());
                }
            }
        };
        PPR_UNIT_TEST (particle_seeding_replays_large_regions_with_coherent_positive_mass) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({99u, false, false, EGenerationMatter::liquid, 1000.0});
            ChunkGrid grid;
            InitialMatter matter;
            InitialMatterRegion region{
                .m_identity = 1u, .m_material = 99u, .m_temperature_k = 293.0, .m_bulk_density_kg_m3 = 1000.0};
            for (u32 y = 10u; y < 110u; ++y) {
                region.m_spans.push_back({y, 10u, 1010u});
                for (u32 x = 10u; x < 1010u; ++x) {
                    region.m_volume_m3 += recipe.m_cell_size_m * recipe.m_cell_size_m;
                }
            }
            region.m_mass_kg = region.m_volume_m3 * region.m_bulk_density_kg_m3;
            matter.m_regions.push_back(region);
            matter.m_span_count = 100u;
            const auto seeded = seedMatterParticles(matter, grid, recipe);
            const auto replay = seedMatterParticles(matter, grid, recipe);
            PPR_TEST_ASSERT(seeded.has_value() and replay.has_value());
            double volume{}, mass{};
            u32 count{};
            for (u32 i = 0u; i < seeded->m_chunks.size(); ++i) {
                PPR_TEST_ASSERT(seeded->m_chunks[i].records().size() == replay->m_chunks[i].records().size());
                for (u32 j = 0u; j < seeded->m_chunks[i].records().size(); ++j) {
                    const auto &particle = seeded->m_chunks[i].records()[j];
                    PPR_TEST_ASSERT(particle == replay->m_chunks[i].records()[j]);
                    PPR_TEST_ASSERT(particle.m_volume_m3 > 0.0 and particle.m_mass_kg == particle.m_volume_m3 * 1000.0);
                    volume += particle.m_volume_m3;
                    mass += particle.m_mass_kg;
                    ++count;
                }
            }
            PPR_TEST_ASSERT(count == 100000u);
            PPR_TEST_ASSERT(std::abs(volume - region.m_volume_m3) < 1.0e-8 * region.m_volume_m3);
            PPR_TEST_ASSERT(std::abs(mass - region.m_mass_kg) < 1.0e-8 * region.m_mass_kg);
            matter.m_regions.front().m_volume_m3 *= 2.0;
            PPR_TEST_ASSERT(not seedMatterParticles(matter, grid, recipe));
        };
        PPR_UNIT_TEST (particle_edits_are_local_atomic_and_pure_digs_preserve_frozen_records) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({99u, false, false, EGenerationMatter::gas, 1.0});
            ChunkGrid grid;
            PPR_TEST_ASSERT(not grid.setCell({120u, 40u}, {17u, 293.0f}));
            const double volume = 2.0 * recipe.m_cell_size_m * recipe.m_cell_size_m;
            InitialMatter matter;
            matter.m_regions.push_back(
                {.m_identity = 1u,
                    .m_material = 99u,
                    .m_temperature_k = 293.0,
                    .m_bulk_density_kg_m3 = 1.0,
                    .m_volume_m3 = volume,
                    .m_mass_kg = volume,
                    .m_spans = {{40u, 127u, 129u}}});
            const auto seeded = seedMatterParticles(matter, grid, recipe);
            PPR_TEST_ASSERT(seeded.has_value());
            MatterParticleState state;
            PPR_TEST_ASSERT(not state.restore(*seeded, recipe.m_materials, recipe.m_cell_size_m));
            const auto frozen = state.capture();
            auto original = grid.captureChunk({0u, 0u});
            PPR_TEST_ASSERT(original.has_value());
            PPR_TEST_ASSERT(not grid.setCell({120u, 40u}, {}));
            grid.exchangeChunk(*original);
            std::array dig{std::move(*original)};
            const auto dug = state.prepareTerrainEdit(grid, dig, recipe.m_cell_size_m);
            PPR_TEST_ASSERT(dug.has_value() and dug->empty() and state.count() == 2u);
            auto old = grid.captureChunk({0u, 0u});
            PPR_TEST_ASSERT(old.has_value());
            PPR_TEST_ASSERT(not grid.setCell({127u, 40u}, {17u, 293.0f}));
            grid.exchangeChunk(*old);
            std::array build{std::move(*old)};
            const auto candidate = state.prepareTerrainEdit(grid, build, recipe.m_cell_size_m);
            PPR_TEST_ASSERT(candidate.has_value() and candidate->size() == 1u and state.count() == 2u);
            state.commitTerrainEdit(*candidate);
            PPR_TEST_ASSERT(state.count() == 1u and frozen.m_chunks[0].records().size() == 1u);
            PPR_TEST_ASSERT(state.chunk({1u, 0u})[0].m_id == 2u);
            auto [context, cancel] = context::withCancelClause(context::background());
            cancel(std::make_error_code(std::errc::operation_canceled));
            PPR_TEST_ASSERT(
                state.restore({}, recipe.m_materials, recipe.m_cell_size_m, context.get()) ==
                std::errc::operation_canceled);
            PPR_TEST_ASSERT(state.count() == 1u);
            auto invalid = state.capture();
            invalid.m_next_id = 2u;
            PPR_TEST_ASSERT(
                state.restore(invalid, recipe.m_materials, recipe.m_cell_size_m) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(state.count() == 1u);
        };
        PPR_UNIT_TEST (macro_layout_replays_spacing_and_spatial_nearest_matches_oracle) {
            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                auto plan = compileGenerationPlan(numericRecipe(), seed);
                PPR_TEST_ASSERT(plan.has_value());
                auto first = compileBiomeLayout(*plan), replay = compileBiomeLayout(*plan);
                PPR_TEST_ASSERT(first and replay);
                PPR_TEST_ASSERT(std::ranges::equal(first->sites(), replay->sites()));
                PPR_TEST_ASSERT(first->sites().size() == 16u);
                for (std::size_t a = 0u; a < first->sites().size(); ++a) {
                    PPR_TEST_ASSERT(first->sites()[a].m_biome == 0u);
                    for (std::size_t b = 0u; b < a; ++b) {
                        const double dx = first->sites()[a].m_x_m - first->sites()[b].m_x_m;
                        const double dy = first->sites()[a].m_y_m - first->sites()[b].m_y_m;
                        PPR_TEST_ASSERT(dx * dx + dy * dy >= 10000.0);
                    }
                }
                const auto check = [&](const double x, const double y) {
                    u32 expected = 0u;
                    double best = (std::numeric_limits<double>::max)();
                    for (u32 s = 0u; s < first->sites().size(); ++s) {
                        const double dx = first->sites()[s].m_x_m - x, dy = first->sites()[s].m_y_m - y;
                        const double distance = dx * dx + dy * dy;
                        if (distance < best) {
                            best = distance;
                            expected = s;
                        }
                    }
                    PPR_TEST_ASSERT(first->nearest(x, y) == expected);
                };
                for (i32 y = -200; y < 1600; y += 37) {
                    for (i32 x = -200; x < 1600; x += 43) {
                        check(x, y);
                    }
                }
                const double bin_width = (4096.0 / 3.0) / 64.0;
                for (u32 i = 0u; i < 64u; ++i) {
                    for (const double x:
                        {i * bin_width, std::nextafter(i * bin_width, 0.0), std::nextafter(i * bin_width, 2000.0)}) {
                        check(x, 437.5);
                    }
                }
            }
        };
        PPR_UNIT_TEST (macro_layout_reports_infeasible_constraints_and_cancellation) {
            auto recipe = numericRecipe();
            recipe.m_site_spacing_m = 4096.0 / 3.0;
            auto plan = compileGenerationPlan(std::move(recipe), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto exhausted = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(
                not exhausted and exhausted.error() == make_error_code(EGenerationErrc::constraints_exhausted));
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            auto cancelled = compileBiomeLayout(*plan, context.get());
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
            recipe = numericRecipe();
            recipe.m_constraints[0u].m_distance_min_m = 1.0;
            plan = compileGenerationPlan(std::move(recipe), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            exhausted = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(
                not exhausted and exhausted.error() == make_error_code(EGenerationErrc::constraints_exhausted));
        };
        PPR_UNIT_TEST (macro_layout_satisfies_required_biomes_before_weighted_assignment) {
            auto recipe = numericRecipe();
            recipe.m_max_sites = 64u;
            for (u32 i = 1u; i < 4u; ++i) {
                auto biome = recipe.m_biomes[0u];
                biome.m_key += i;
                recipe.m_biomes.push_back(std::move(biome));
                recipe.m_constraints.push_back({401u + i, i, 4u, 1.0, i == 3u ? 0.5 : 0.0, 1.0, 0.0, 2048.0});
            }
            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                auto plan = compileGenerationPlan(recipe, seed);
                PPR_TEST_ASSERT(plan.has_value());
                auto layout = compileBiomeLayout(*plan);
                PPR_TEST_ASSERT(layout.has_value());
                std::array<u32, 4u> counts{};
                for (const auto &site: layout->sites()) {
                    ++counts[site.m_biome];
                    if (site.m_biome == 3u) {
                        PPR_TEST_ASSERT(1.0 - site.m_y_m / (4096.0 / 3.0) >= 0.5);
                    }
                }
                for (const auto &constraint: recipe.m_constraints) {
                    PPR_TEST_ASSERT(counts[constraint.m_biome] >= constraint.m_required_count);
                }
            }
        };

        PPR_UNIT_TEST (nonlocal_geometry_replays_worms_and_links_only_traversal_rooms) {
            auto recipe = numericRecipe();
            auto room = recipe.m_features[0u];
            room.m_key = 701u;
            room.m_markers = {{702u, EGenerationMarker::traversal_room, 15, 15}};
            recipe.m_features.push_back(room);
            room.m_key = 703u;
            room.m_markers = {{704u, EGenerationMarker::sealed_pocket, 15, 15}};
            room.m_markers.push_back({705u, EGenerationMarker::traversal_room, 15, 15});
            recipe.m_features.push_back(room);
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationWorm{
                        .m_count = 2u,
                        .m_carve = true,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 3.25,
                        .m_length_max_m = 3.25,
                        .m_radius_min_m = 0.5,
                        .m_radius_max_m = 0.5,
                        .m_turn_probability = 0.5}});
            recipe.m_rules.push_back(
                {.m_key = 602u,
                    .m_order = 1u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationConnection{1.0, 1u}});
            recipe.m_rules.push_back(
                {.m_key = 603u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{
                        .m_feature = 1u,
                        .m_min_count = 3u,
                        .m_max_count = 3u,
                        .m_attempts = 64u,
                        .m_depth_max = 1.0,
                        .m_spacing_m = 40.0}});
            recipe.m_rules.push_back(
                {.m_key = 604u,
                    .m_order = 1u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{
                        .m_feature = 2u,
                        .m_min_count = 1u,
                        .m_max_count = 1u,
                        .m_attempts = 64u,
                        .m_depth_max = 1.0,
                        .m_spacing_m = 40.0}});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            auto first = compileGenerationGeometry(*plan, *layout), replay = compileGenerationGeometry(*plan, *layout);
            PPR_TEST_ASSERT(first and replay);
            PPR_TEST_ASSERT(std::ranges::equal(first->m_instances, replay->m_instances));
            PPR_TEST_ASSERT(std::ranges::equal(first->m_segments, replay->m_segments));
            PPR_TEST_ASSERT(first->m_instances.size() == 5u and first->m_segments.size() == 11u);
            PPR_TEST_ASSERT(first->m_omitted_instances.empty());
            PPR_TEST_ASSERT(first->m_worst_placement_attempts > 0u and first->m_worst_placement_attempts <= 64u);
            u32 links = 0u;
            for (const auto &segment: first->m_segments) {
                if (segment.m_rule != 1u) {
                    continue;
                }
                ++links;
                for (const auto point:
                    {std::pair{segment.m_x0_m, segment.m_y0_m}, std::pair{segment.m_x1_m, segment.m_y1_m}}) {
                    PPR_TEST_ASSERT(std::ranges::any_of(first->m_instances, [&](const auto &instance) {
                        return instance.m_feature == 1u and
                               point.first == (instance.m_origin_x + 15.5) / 3.0 and
                               point.second == (instance.m_origin_y + 15.5) / 3.0;
                    }));
                }
            }
            PPR_TEST_ASSERT(links == 3u);
        };

        PPR_UNIT_TEST (nonlocal_geometry_distinguishes_required_exhaustion_optional_omission_and_limits) {
            auto recipe = numericRecipe();
            auto huge = recipe.m_features[0u];
            huge.m_key = 701u;
            huge.m_bounds = {0, 0, 4096, 4096};
            recipe.m_features.push_back(huge);
            recipe.m_rules.push_back(
                {.m_key = 603u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{
                        .m_feature = 1u, .m_min_count = 1u, .m_max_count = 1u, .m_attempts = 64u, .m_depth_max = 1.0}});
            const auto run = [&] {
                auto plan = compileGenerationPlan(recipe, 42u);
                PPR_TEST_ASSERT(plan.has_value());
                auto layout = compileBiomeLayout(*plan);
                PPR_TEST_ASSERT(layout.has_value());
                return compileGenerationGeometry(*plan, *layout);
            };
            auto result = run();
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EGenerationErrc::constraints_exhausted));
            std::get<GenerationPlacement>(recipe.m_rules[0u].m_parameters).m_min_count = 0u;
            result = run();
            PPR_TEST_ASSERT(result and result->m_instances.size() == 1u);
            PPR_TEST_ASSERT(result->m_omitted_instances == Array<u32>{0u});
            recipe.m_rules = {
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationWorm{
                        .m_count = 1024u,
                        .m_carve = true,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 4096.0,
                        .m_length_max_m = 4096.0,
                        .m_radius_min_m = 0.5,
                        .m_radius_max_m = 0.5}}};
            result = run();
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EGenerationErrc::segment_limit));
            std::get<GenerationWorm>(recipe.m_rules.front().m_parameters).m_count = 1025u;
            PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::value_too_large);
        };

        PPR_UNIT_TEST (finite_blobs_and_smooth_worms_are_bounded_and_replay) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            GenerationDeposit deposit{
                .m_key = 501u,
                .m_material = 33u,
                .m_host = 17u,
                .m_depth_max = 1.0,
                .m_threshold_min = -1.0,
                .m_threshold_max = 1.0,
                .m_shape = EGenerationDepositShape::blob,
                .m_blob_count = 2u,
                .m_radius_min_m = 4.0,
                .m_radius_max_m = 8.0,
                .m_stretch_min = 0.5,
                .m_stretch_max = 3.0,
                .m_distortion = 0.3};
            recipe.m_biomes[0u].m_deposits.push_back(deposit);
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationWorm{
                        .m_count = 1u,
                        .m_carve = true,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 80.25,
                        .m_length_max_m = 80.25,
                        .m_radius_min_m = 1.0,
                        .m_radius_max_m = 3.0,
                        .m_turn_probability = 0.5,
                        .m_turn_smoothing = 0.85,
                        .m_radius_variation = 1.0,
                        .m_chamber_probability = 0.1,
                        .m_chamber_scale = 3.0}});
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            const auto first = compileGenerationGeometry(*plan, *layout);
            const auto replay = compileGenerationGeometry(*plan, *layout);
            PPR_TEST_ASSERT(first and replay);
            PPR_TEST_ASSERT(first->m_blobs == replay->m_blobs and first->m_blobs.size() == 2u);
            PPR_TEST_ASSERT(first->m_segments == replay->m_segments and first->m_segments.size() == 81u);
            bool varied_radius = false;
            for (u32 i = 0u; i < first->m_segments.size(); ++i) {
                const auto &segment = first->m_segments[i];
                PPR_TEST_ASSERT(segment.m_radius_m >= 1.0 and segment.m_radius_m <= 9.0);
                if (i == 0u) {
                    continue;
                }
                const auto &previous = first->m_segments[i - 1u];
                PPR_TEST_ASSERT(segment.m_x0_m == previous.m_x1_m and segment.m_y0_m == previous.m_y1_m);
                const double ax = previous.m_x1_m - previous.m_x0_m, ay = previous.m_y1_m - previous.m_y0_m;
                const double bx = segment.m_x1_m - segment.m_x0_m, by = segment.m_y1_m - segment.m_y0_m;
                PPR_TEST_ASSERT(std::abs(std::atan2(ax * by - ay * bx, ax * bx + ay * by)) <= 0.351);
                varied_radius = varied_radius or segment.m_radius_m != previous.m_radius_m;
            }
            PPR_TEST_ASSERT(varied_radius);
            deposit.m_key = 502u;
            deposit.m_order = 1u;
            recipe.m_biomes[0u].m_deposits.push_back(deposit);
            const auto extended_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(extended_plan.has_value());
            const auto extended_layout = compileBiomeLayout(*extended_plan);
            PPR_TEST_ASSERT(extended_layout.has_value());
            const auto extended = compileGenerationGeometry(*extended_plan, *extended_layout);
            PPR_TEST_ASSERT(extended.has_value() and extended->m_blobs.size() == 4u);
            PPR_TEST_ASSERT(
                first->m_blobs[0u] == extended->m_blobs[0u] and first->m_blobs[1u] == extended->m_blobs[1u]);
            for (const auto bounds: {std::pair{80.0, 100.0}, std::pair{90.0, 90.0}}) {
                auto oriented = recipe;
                auto &lens = oriented.m_biomes[0u].m_deposits[0u];
                lens.m_orientation_min_degrees = bounds.first;
                lens.m_orientation_max_degrees = bounds.second;
                const auto oriented_plan = compileGenerationPlan(std::move(oriented), 42u);
                PPR_TEST_ASSERT(oriented_plan.has_value());
                const auto oriented_geometry = compileGenerationGeometry(*oriented_plan, *layout);
                PPR_TEST_ASSERT(oriented_geometry.has_value());
                for (u32 i = 0u; i < 2u; ++i) {
                    const auto &blob = oriented_geometry->m_blobs[i];
                    PPR_TEST_ASSERT(
                        blob.m_angle >= 2.0 * std::numbers::pi * (bounds.first / 360.0) and
                        blob.m_angle <= 2.0 * std::numbers::pi * (bounds.second / 360.0));
                    PPR_TEST_ASSERT(blob.m_x_m == first->m_blobs[i].m_x_m and blob.m_y_m == first->m_blobs[i].m_y_m);
                    PPR_TEST_ASSERT(
                        blob.m_radius_m == first->m_blobs[i].m_radius_m and
                        blob.m_stretch == first->m_blobs[i].m_stretch);
                    PPR_TEST_ASSERT(
                        blob.m_offset_x == first->m_blobs[i].m_offset_x and
                        blob.m_offset_y == first->m_blobs[i].m_offset_y);
                }
                PPR_TEST_ASSERT(compileGenerationRaster(*oriented_plan, *layout, *oriented_geometry).has_value());
                auto malformed = *oriented_geometry;
                malformed.m_blobs[0u].m_angle = 2.0 * std::numbers::pi * (bounds.second / 360.0) + 0.01;
                PPR_TEST_ASSERT(not compileGenerationRaster(*oriented_plan, *layout, malformed).has_value());
            }
            for (const auto bounds:
                {std::pair{100.0, 80.0}, std::pair{-1.0, 90.0}, std::pair{0.0, 361.0},
                    std::pair{0.0, (std::numeric_limits<double>::quiet_NaN)()}}) {
                auto invalid_orientation = recipe;
                auto &lens = invalid_orientation.m_biomes[0u].m_deposits[0u];
                lens.m_orientation_min_degrees = bounds.first;
                lens.m_orientation_max_degrees = bounds.second;
                PPR_TEST_ASSERT(validateGenerationRecipe(invalid_orientation) == std::errc::invalid_argument);
            }
            recipe.m_biomes[0u].m_deposits[0u].m_radius_min_m = 0.0;
            PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::invalid_argument);
            recipe.m_biomes[0u].m_deposits[0u].m_radius_min_m = 4.0;
            recipe.m_biomes[0u].m_relaxation_passes = 25u;
            PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::invalid_argument);
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            const auto cancelled = compileGenerationGeometry(*plan, *layout, context.get());
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);

            // The target occupies a four-metre square in a 4096-metre world.
            // Global rejection sampling cannot reliably admit these origins.
            auto tiny = numericRecipe();
            tiny.m_cell_size_m = 1.0;
            tiny.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            tiny.m_biomes.push_back(tiny.m_biomes.front());
            tiny.m_biomes.back().m_key = 204u;
            tiny.m_constraints.push_back({402u, 1u, 1u, 1.0, 0.0, 1.0, 0.0, 2048.0});
            tiny.m_biomes.back().m_deposits.push_back(
                {.m_key = 701u,
                    .m_material = 33u,
                    .m_host = 17u,
                    .m_depth_min = 1.0 - 1000.25 / kWorldEdge,
                    .m_depth_max = 1.0 - 999.75 / kWorldEdge,
                    .m_shape = EGenerationDepositShape::blob,
                    .m_blob_count = 32u,
                    .m_radius_min_m = 0.5,
                    .m_radius_max_m = 1.0});
            constexpr std::array tiny_sites{
                GenerationSite{1000.0, 1000.0, 1u}, GenerationSite{996.0, 1000.0, 0u},
                GenerationSite{1004.0, 1000.0, 0u}, GenerationSite{1000.0, 996.0, 0u},
                GenerationSite{1000.0, 1004.0, 0u}};
            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                const auto tiny_plan = compileGenerationPlan(tiny, seed);
                PPR_TEST_ASSERT(tiny_plan.has_value());
                const auto tiny_layout = restoreBiomeLayout(*tiny_plan, tiny_sites);
                PPR_TEST_ASSERT(tiny_layout.has_value());
                u64 scratch = 0u;
                const auto admitted = compileGenerationGeometry(*tiny_plan, *tiny_layout, nullptr, &scratch);
                PPR_TEST_ASSERT(admitted.has_value() and admitted->m_blobs.size() == 32u and scratch > 0u);
                for (const auto &blob: admitted->m_blobs) {
                    PPR_TEST_ASSERT(blob.m_x_m >= 998.0 and blob.m_x_m <= 1002.0);
                    PPR_TEST_ASSERT(blob.m_y_m >= 999.75 and blob.m_y_m <= 1000.25);
                    PPR_TEST_ASSERT(generationBiomeAt(*tiny_plan, *tiny_layout, blob.m_x_m, blob.m_y_m) == 1u);
                }
                PPR_TEST_ASSERT(std::ranges::any_of(admitted->m_blobs, [&](const auto &blob) {
                    return blob.m_x_m != admitted->m_blobs.front().m_x_m and
                           blob.m_y_m != admitted->m_blobs.front().m_y_m;
                }));
            }
            tiny.m_biomes.back().m_deposits.front().m_depth_min = 0.1;
            tiny.m_biomes.back().m_deposits.front().m_depth_max = 0.2;
            const auto absent_plan = compileGenerationPlan(tiny, 42u);
            PPR_TEST_ASSERT(absent_plan.has_value());
            const auto absent_layout = restoreBiomeLayout(*absent_plan, tiny_sites);
            PPR_TEST_ASSERT(absent_layout.has_value());
            const auto absent = compileGenerationGeometry(*absent_plan, *absent_layout);
            PPR_TEST_ASSERT(not absent and absent.error() == make_error_code(EGenerationErrc::constraints_exhausted));
        };

        PPR_UNIT_TEST (coherent_vacuum_palette_is_salted_before_bounded_relaxation) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0});
            recipe.m_fields[0u].m_noise.m_algorithm = ENoiseAlgorithm::cellular_value;
            recipe.m_fields[0u].m_noise.m_frequency_per_metre = 0.12;
            auto &biome = recipe.m_biomes[0u];
            biome.m_bands[0u].m_choices[0u].m_weight = 3.0;
            biome.m_bands[0u].m_choices.push_back({204u, 0u, 1.0});
            biome.m_bands[0u].m_choices.push_back({205u, 33u, 1.0});
            biome.m_sprinkle_probability = 0.4;
            const auto salted = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(salted.has_value());
            const auto layout = compileBiomeLayout(*salted);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 70, 80, false});
            const auto raw_raster = compileGenerationRaster(*salted, *layout, geometry);
            PPR_TEST_ASSERT(raw_raster.has_value());
            const auto raw = raw_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(raw.has_value());
            recipe.m_biomes[0u].m_sprinkle_probability = 0.0;
            const auto coherent = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(coherent.has_value());
            const auto coherent_raster = compileGenerationRaster(*coherent, *layout, geometry);
            PPR_TEST_ASSERT(coherent_raster.has_value());
            const auto unsalted = coherent_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(unsalted.has_value() and *raw != *unsalted);
            recipe.m_biomes[0u].m_sprinkle_probability = 0.4;
            recipe.m_biomes[0u].m_relaxation_passes = 1u;
            recipe.m_biomes[0u].m_relaxation_adoption = 0.32;
            recipe.m_biomes[0u].m_finish_passes = 2u;
            recipe.m_biomes[0u].m_finish_adoption = 0.12;
            const auto tweaked_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(tweaked_plan.has_value());
            const auto tweaked_raster = compileGenerationRaster(*tweaked_plan, *layout, geometry);
            PPR_TEST_ASSERT(tweaked_raster.has_value());
            const auto tweaked = tweaked_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(tweaked.has_value());
            recipe.m_biomes[0u].m_erosion_passes = 1u;
            const auto modal_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(modal_plan.has_value());
            const auto modal_raster = compileGenerationRaster(*modal_plan, *layout, geometry);
            PPR_TEST_ASSERT(modal_raster.has_value());
            const auto modal = modal_raster->chunk({0u, 0u});
            const auto modal_replay = modal_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(modal and modal_replay and *modal == *modal_replay);
            constexpr std::array<u16, 3u> labels{0u, 17u, 33u};
            u32 excavated = 0u, filled = 0u, relabeled = 0u, retained = 0u;
            // The reference reads the completed tweaked/finish output, proving
            // erosion is appended. Unique pluralities need no tie assumption.
            for (u32 y = 4u; y < 76u; ++y) {
                for (u32 x = 4u; x < kChunkEdge - 1u; ++x) {
                    std::array<u32, 3u> histogram{};
                    for (i32 dy = -1; dy <= 1; ++dy) {
                        for (i32 dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 and dy == 0) {
                                continue;
                            }
                            const auto neighbor =
                                (*tweaked)[static_cast<std::size_t>((static_cast<i64>(y) + dy) * kChunkEdge + x + dx)]
                                    .m_element;
                            const auto label = std::ranges::find(labels, neighbor);
                            PPR_TEST_ASSERT(label != labels.end());
                            ++histogram[static_cast<std::size_t>(label - labels.begin())];
                        }
                    }
                    const auto maximum = std::ranges::max_element(histogram);
                    if (std::ranges::count(histogram, *maximum) != 1) {
                        continue;
                    }
                    const u16 expected = labels[static_cast<std::size_t>(maximum - histogram.begin())];
                    const auto before = (*tweaked)[y * kChunkEdge + x].m_element;
                    const auto &after = (*modal)[y * kChunkEdge + x];
                    PPR_TEST_ASSERT(after.m_element == expected or after.m_element == before);
                    PPR_TEST_ASSERT(after.m_element == 0u ? after.m_temperature == 0.0f : after.m_temperature > 0.0f);
                    if (before == after.m_element) {
                        retained += before != expected ? 1u : 0u;
                        continue;
                    }
                    excavated += before != 0u and expected == 0u ? 1u : 0u;
                    filled += before == 0u and expected != 0u ? 1u : 0u;
                    relabeled += before != 0u and expected != 0u and before != expected ? 1u : 0u;
                }
            }
            PPR_TEST_ASSERT(excavated > 0u and filled > 0u and relabeled > 0u and retained > 0u);
            recipe.m_biomes[0u].m_relaxation_passes = 21u;
            recipe.m_biomes[0u].m_relaxation_adoption = 0.5;
            recipe.m_biomes[0u].m_finish_passes = 0u;
            recipe.m_biomes[0u].m_erosion_passes = 0u;
            const auto relaxed = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(relaxed.has_value());
            const auto smooth_raster = compileGenerationRaster(*relaxed, *layout, geometry);
            PPR_TEST_ASSERT(smooth_raster.has_value());
            const auto smooth = smooth_raster->chunk({0u, 0u});
            const auto replay = smooth_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(smooth and replay and *smooth == *replay);
            PPR_TEST_ASSERT(std::ranges::any_of(*smooth, [](const Cell &cell) { return cell.m_element == 0u; }));
            const auto edges = [](const Array<Cell> &cells) {
                u32 changes = 0u;
                for (u32 y = 1u; y < kChunkEdge; ++y) {
                    for (u32 x = 1u; x < kChunkEdge; ++x) {
                        const u32 at = y * kChunkEdge + x;
                        changes += cells[at].m_element != cells[at - 1u].m_element ? 1u : 0u;
                        changes += cells[at].m_element != cells[at - kChunkEdge].m_element ? 1u : 0u;
                    }
                }
                return changes;
            };
            PPR_TEST_ASSERT(edges(*smooth) < edges(*raw));
            for (u32 y = 0u; y < kChunkEdge; ++y) {
                PPR_TEST_ASSERT((*smooth)[y * kChunkEdge].m_element == recipe.m_boundary_material);
            }
            for (u32 y = 80u; y < 110u; ++y) {
                for (u32 x = 70u; x < 100u; ++x) {
                    PPR_TEST_ASSERT((*smooth)[y * kChunkEdge + x].m_element == 0u);
                }
            }
            u32 natural_empty = 0u;
            for (u32 y = 3u; y < 80u; ++y) {
                for (u32 x = 3u; x < kChunkEdge; ++x) {
                    natural_empty += (*smooth)[y * kChunkEdge + x].m_element == 0u ? 1u : 0u;
                }
            }
            PPR_TEST_ASSERT(natural_empty > 0u);
            for (const auto &cell: *smooth) {
                PPR_TEST_ASSERT(cell.m_element == 0u ? cell.m_temperature == 0.0f : cell.m_temperature > 0.0f);
            }
            for (const double probability: {-0.1, 1.1, (std::numeric_limits<double>::quiet_NaN)()}) {
                recipe.m_biomes[0u].m_sprinkle_probability = probability;
                PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::invalid_argument);
            }
        };

        PPR_UNIT_TEST (relaxation_crosses_chunk_seams_preserves_reservations_and_precedes_detail) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            recipe.m_materials.push_back({49u, true, true, EGenerationMatter::solid, 2500.0});
            auto &biome = recipe.m_biomes[0u];
            biome.m_relaxation_passes = 1u;
            biome.m_relaxation_adoption = 1.0;
            biome.m_deposits.push_back(
                {.m_key = 501u,
                    .m_material = 33u,
                    .m_host = 17u,
                    .m_depth_max = 1.0,
                    .m_threshold_min = -1.0,
                    .m_threshold_max = 1.0,
                    .m_shape = EGenerationDepositShape::blob,
                    .m_blob_count = 1u,
                    .m_radius_min_m = 1.5,
                    .m_radius_max_m = 1.5});
            biome.m_deposits.push_back(
                {.m_key = 502u,
                    .m_order = 1u,
                    .m_material = 49u,
                    .m_host = 17u,
                    .m_depth_max = 1.0,
                    .m_threshold_min = -1.0,
                    .m_threshold_max = 1.0,
                    .m_shape = EGenerationDepositShape::blob,
                    .m_stage = EGenerationDepositStage::detail,
                    .m_blob_count = 1u,
                    .m_radius_min_m = 0.2,
                    .m_radius_max_m = 0.2});
            recipe.m_rules.push_back(
                {.m_key = 601u, .m_phase = EGenerationPhase::carve, .m_parameters = GenerationConnection{0.2, 0u}});
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 127, 80, false});
            geometry.m_segments.push_back({0u, 120.5, 70.5, 140.5, 70.5, 0.2});
            geometry.m_blobs.push_back({0u, 0u, 0u, 127.5, 64.5, 1.5, 1.0, 0.0, 0.0, 0.0});
            geometry.m_blobs.push_back({0u, 1u, 0u, 128.5, 63.5, 0.2, 1.0, 0.0, 0.0, 0.0});
            const auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            const auto right = raster->chunk({1u, 0u}), left = raster->chunk({0u, 0u});
            const auto replay = raster->chunk({1u, 0u});
            PPR_TEST_ASSERT(left and right and replay and *right == *replay);
            PPR_TEST_ASSERT((*left)[64u * kChunkEdge + 127u].m_element == 33u);
            PPR_TEST_ASSERT((*left)[63u * kChunkEdge + 126u].m_element == 17u);
            PPR_TEST_ASSERT((*right)[64u * kChunkEdge].m_element == 33u);
            PPR_TEST_ASSERT((*right)[63u * kChunkEdge].m_element == 49u);
            PPR_TEST_ASSERT((*left)[70u * kChunkEdge + 127u].m_element == 0u);
            PPR_TEST_ASSERT((*right)[70u * kChunkEdge].m_element == 0u);
            PPR_TEST_ASSERT((*left)[80u * kChunkEdge + 127u].m_element == 0u);
            PPR_TEST_ASSERT((*right)[80u * kChunkEdge].m_element == 0u);
            for (const auto &cell: *right) {
                PPR_TEST_ASSERT(cell.m_element == 0u ? cell.m_temperature == 0.0f : cell.m_temperature > 0.0f);
            }
            geometry.m_blobs[0u].m_stretch = std::numeric_limits<double>::infinity();
            const auto invalid = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (chunk_raster_preserves_boundaries_host_replacement_and_global_seams) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            recipe.m_materials.push_back({49u, true, true, EGenerationMatter::solid, 3000.0});
            recipe.m_materials.push_back({1001u, false, false, EGenerationMatter::liquid, 1000.0});
            recipe.m_biomes[0u].m_deposits.push_back({501u, 0u, 0u, 33u, 17u, 0.0, 1.0, -1.0, 1.0});
            recipe.m_biomes[0u].m_deposits.push_back({502u, 1u, 0u, 49u, 17u, 0.0, 1.0, -1.0, 1.0});
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::deposit,
                    .m_parameters = GenerationWorm{
                        .m_count = 1u,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 1.0,
                        .m_length_max_m = 1.0,
                        .m_radius_min_m = 1.0 / 3.0,
                        .m_radius_max_m = 1.0 / 3.0,
                        .m_material = 49u,
                        .m_host = 33u}});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 2000, 3000, false});
            geometry.m_segments.push_back({0u, 42.0, 64.5 / 3.0, 44.0, 64.5 / 3.0, 1.0 / 3.0});
            auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            const auto first = raster->chunk({0u, 0u}), second = raster->chunk({1u, 0u});
            PPR_TEST_ASSERT(first and second);
            PPR_TEST_ASSERT((*first)[0u].m_element == 17u);
            PPR_TEST_ASSERT((*first)[64u * kChunkEdge + 20u].m_element == 33u);
            PPR_TEST_ASSERT((*first)[64u * kChunkEdge + 127u].m_element == 49u);
            PPR_TEST_ASSERT((*second)[64u * kChunkEdge].m_element == 49u);
            for (const auto &cell: *first) {
                PPR_TEST_ASSERT(cell.m_element == 17u or cell.m_element == 33u or cell.m_element == 49u);
                PPR_TEST_ASSERT(cell.m_temperature >= 285.0f and cell.m_temperature <= 301.0f);
            }
            const auto second_replay = raster->chunk({1u, 0u}), first_replay = raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(first_replay and second_replay);
            PPR_TEST_ASSERT(*first == *first_replay and *second == *second_replay);
            const auto invalid = raster->chunk({32u, 0u});
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::result_out_of_range);
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            const auto cancelled = raster->chunk({0u, 0u}, context.get());
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        };

        PPR_UNIT_TEST (chunk_raster_opens_room_portals_but_preserves_mixed_role_sealed_pockets) {
            auto recipe = numericRecipe();
            auto room = recipe.m_features[0u];
            room.m_key = 701u;
            room.m_operations[0u].m_carve = false;
            room.m_operations[0u].m_material = 17u;
            room.m_markers = {{702u, EGenerationMarker::traversal_room, 15, 15}};
            recipe.m_features.push_back(room);
            room.m_key = 703u;
            room.m_markers.push_back({704u, EGenerationMarker::sealed_pocket, 15, 15});
            recipe.m_features.push_back(room);
            recipe.m_rules.push_back(
                {.m_key = 601u, .m_phase = EGenerationPhase::carve, .m_parameters = GenerationConnection{1.0, 0u}});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances = {
                {0u, kGenerationStartRule, 0u, 2000, 3000, false}, {1u, 1u, 0u, 30, 30, false},
                {2u, 2u, 0u, 90, 30, false}};
            geometry.m_segments.push_back({0u, 1.0, 45.5 / 3.0, 42.0, 45.5 / 3.0, 1.0});
            auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            auto chunk = raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(chunk.has_value());
            PPR_TEST_ASSERT((*chunk)[45u * kChunkEdge + 30u] == Cell{});
            PPR_TEST_ASSERT((*chunk)[45u * kChunkEdge + 59u] == Cell{});
            PPR_TEST_ASSERT((*chunk)[45u * kChunkEdge + 90u].m_element == 17u);
            PPR_TEST_ASSERT((*chunk)[45u * kChunkEdge + 119u].m_element == 17u);
            PPR_TEST_ASSERT((*chunk)[30u * kChunkEdge + 30u].m_element == 17u);
        };

        PPR_UNIT_TEST (chunk_raster_protects_template_reservations_from_cave_fields) {
            auto recipe = numericRecipe();
            recipe.m_features[0u].m_operations[0u].m_carve = false;
            recipe.m_features[0u].m_operations[0u].m_material = 17u;
            recipe.m_features[0u].m_operations.push_back(
                {.m_key = 999u, .m_order = 1u, .m_carve = true, .m_x = 4, .m_y = 4, .m_width = 22u, .m_height = 22u});
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationCave{
                        .m_depth_max = 1.0, .m_threshold_min = -1.0, .m_threshold_max = 1.0}});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            auto geometry = compileGenerationGeometry(*plan, *layout);
            PPR_TEST_ASSERT(geometry.has_value());
            auto raster = compileGenerationRaster(*plan, *layout, *geometry);
            PPR_TEST_ASSERT(raster.has_value());
            auto chunk = raster->chunk({15u, 23u});
            PPR_TEST_ASSERT(chunk.has_value());
            const auto cell = [&](const u32 x, const u32 y) -> const Cell & {
                return (*chunk)[(y - 2944u) * kChunkEdge + x - 1920u];
            };
            PPR_TEST_ASSERT(cell(1999u, 3000u).m_element == 0u);
            PPR_TEST_ASSERT(cell(2000u, 3000u).m_element == 17u);
            PPR_TEST_ASSERT(cell(2015u, 3010u) == Cell{});
        };

        PPR_UNIT_TEST (matter_area_clips_shared_contours_and_horizontal_height) {
            ChunkGrid grid;
            constexpr double h = 1.0 / 3.0;
            const GlobalCellPos cell{64u, 64u};
            auto full = generationEmptyArea(grid, cell, h);
            PPR_TEST_ASSERT(full and *full == h * h);
            auto half = generationEmptyArea(grid, cell, h, 64.5 / 3.0);
            PPR_TEST_ASSERT(half and *half == 0.5 * h * h);
            for (u32 y = 63u; y <= 65u; ++y) {
                for (u32 x = 63u; x <= 65u; ++x) {
                    if (x != 64u or y != 64u) {
                        PPR_TEST_ASSERT(not grid.setCell({x, y}, {17u, 293.0f}));
                    }
                }
            }
            auto isolated = generationEmptyArea(grid, cell, h);
            PPR_TEST_ASSERT(isolated and *isolated == 0.5 * h * h);
            auto clipped = generationEmptyArea(grid, cell, h, 64.5 / 3.0);
            PPR_TEST_ASSERT(clipped and *clipped == 0.25 * h * h);
            PPR_TEST_ASSERT(not grid.setCell(cell, {17u, 293.0f}));
            auto solid = generationEmptyArea(grid, cell, h);
            PPR_TEST_ASSERT(solid and *solid == 0.0);
            PPR_TEST_ASSERT(not generationEmptyArea(grid, {4096u, 0u}, h));
            PPR_TEST_ASSERT(not generationEmptyArea(grid, cell, 0.0));
            PPR_TEST_ASSERT(not generationEmptyArea(grid, cell, (std::numeric_limits<double>::max)()));
            for (u32 mask = 0u; mask < 512u; ++mask) {
                for (u32 bit = 0u; bit < 9u; ++bit) {
                    PPR_TEST_ASSERT(not grid.setCell(
                        {63u + bit % 3u, 63u + bit / 3u}, {(mask & (1u << bit)) != 0u ? u16{17u} : u16{0u}, 293.0f}));
                }
                const auto whole = generationEmptyArea(grid, cell, h);
                const auto lower = generationEmptyArea(grid, cell, h, 64.5 / 3.0);
                PPR_TEST_ASSERT(whole and lower);
                PPR_TEST_ASSERT(*whole >= 0.0 and *whole <= 1.0 / 9.0);
                PPR_TEST_ASSERT(*lower >= 0.0 and *lower <= *whole);
            }
        };

        [[nodiscard]] GenerationRecipe matterRecipe() {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({1001u, false, false, EGenerationMatter::liquid, 1000.0});
            recipe.m_materials.push_back({1009u, false, false, EGenerationMatter::gas, 1.2});
            auto pocket = recipe.m_features[0u];
            pocket.m_key = 701u;
            pocket.m_bounds = {0, 0, 9, 9};
            pocket.m_operations[0u].m_width = 9u;
            pocket.m_operations[0u].m_height = 9u;
            pocket.m_markers = {{702u, EGenerationMarker::sealed_pocket, 4, 4}};
            recipe.m_features.push_back(pocket);
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{
                        .m_feature = 1u, .m_min_count = 1u, .m_max_count = 1u, .m_attempts = 64u, .m_depth_max = 1.0}});
            recipe.m_rules.push_back(
                {.m_key = 602u,
                    .m_phase = EGenerationPhase::fill,
                    .m_parameters = GenerationFill{0u, 0u, 1001u, false, 290.0, 1000.0, 4.0 / 3.0}});
            recipe.m_rules.push_back(
                {.m_key = 603u,
                    .m_order = 1u,
                    .m_phase = EGenerationPhase::fill,
                    .m_parameters = GenerationFill{0u, 0u, 1009u, true, 290.0, 1.2, std::nullopt}});
            return recipe;
        }
        PPR_UNIT_TEST (vacuum_palette_preserves_unstamped_sealed_reservation_substrate) {
            auto recipe = matterRecipe();
            recipe.m_fields[0u].m_noise.m_algorithm = ENoiseAlgorithm::cellular_value;
            recipe.m_fields[0u].m_noise.m_frequency_per_metre = 0.12;
            auto &biome = recipe.m_biomes[0u];
            biome.m_bands[0u].m_choices.push_back({204u, 0u, 999.0});
            biome.m_sprinkle_probability = 0.4;
            biome.m_relaxation_passes = 21u;
            // The inner carve crosses the outer ellipse. Its corners require
            // unpainted substrate from the protected reservation to stay sealed.
            auto &pocket = recipe.m_features[1u];
            pocket.m_operations = {
                {.m_key = 703u,
                    .m_shape = EGenerationShape::ellipse,
                    .m_x = 1,
                    .m_y = 1,
                    .m_width = 7u,
                    .m_height = 7u,
                    .m_material = 17u},
                {.m_key = 704u, .m_order = 1u, .m_carve = true, .m_x = 1, .m_y = 1, .m_width = 7u, .m_height = 7u}};
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({1u, 0u, 0u, 60, 60, false});
            const auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            auto cells = raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(cells.has_value());
            ChunkGrid grid;
            PPR_TEST_ASSERT(not grid.setCells({0u, 0u}, std::move(*cells)));
            const auto matter = resolveInitialMatter(*plan, geometry, grid);
            PPR_TEST_ASSERT(matter and matter->m_regions.size() == 2u and matter->m_largest_component == 49u);
            PPR_TEST_ASSERT(grid.getCell({60u, 60u})->m_element == 17u);
            PPR_TEST_ASSERT(grid.getCell({61u, 61u})->m_element == 0u);
        };

        PPR_UNIT_TEST (open_surface_and_continuous_bedrock_override_every_terrain_phase) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_materials.push_back({633u, true, false, EGenerationMatter::solid, 3100.0});
            recipe.m_open_surface = true;
            recipe.m_surface_elevation_m = 220.0;
            recipe.m_bedrock_thickness_m = 25.0;
            recipe.m_bedrock_material = 633u;
            recipe.m_start_depth_m = 45.0;
            recipe.m_biomes[0u].m_pore_probability = 0.2;
            recipe.m_biomes[0u].m_pore_affinity_min = -1.0;
            recipe.m_biomes[0u].m_pore_passes = 3u;
            recipe.m_biomes[0u].m_relaxation_passes = 8u;
            recipe.m_biomes[0u].m_finish_passes = 3u;
            recipe.m_rules.push_back({501u, 1u, EGenerationPhase::carve, GenerationCave{0u, 0u, 0.0, 1.0, -1.0, 1.0}});
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan and plan->recipe().m_start_y_fine == 175u);
            PPR_TEST_ASSERT(generationDepth(*plan, 70.0, 220.0) == 0.0);
            PPR_TEST_ASSERT(generationDepth(*plan, 70.0, 25.0) == 1.0);
            const auto layout = restoreBiomeLayout(*plan, std::array{GenerationSite{100.0, 150.0, 0u}});
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            // Even a supplied carve template crossing the protected bedrock
            // cannot overwrite it; normal geometry rejects this placement.
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 10, 10, false});
            const auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            const auto bottom = raster->chunk({0u, 0u}), surface = raster->chunk({0u, 1u}),
                       top = raster->chunk({0u, 31u});
            PPR_TEST_ASSERT(bottom and surface and top);
            for (u32 x = 0u; x < kChunkEdge; ++x) {
                for (u32 y = 0u; y < 25u; ++y) {
                    PPR_TEST_ASSERT((*bottom)[y * kChunkEdge + x].m_element == 633u);
                }
                for (u32 y = 92u; y < kChunkEdge; ++y) {
                    PPR_TEST_ASSERT((*surface)[y * kChunkEdge + x] == Cell{});
                }
            }
            PPR_TEST_ASSERT(std::ranges::all_of(*top, [](const Cell &cell) { return cell == Cell{}; }));
            // The canonical metric multiplication differs by one ULP from
            // division through a reciprocal at this exact allowed surface.
            auto edge_recipe = numericRecipe();
            edge_recipe.m_materials.push_back({633u, true, false, EGenerationMatter::solid, 3100.0});
            edge_recipe.m_open_surface = true;
            edge_recipe.m_surface_elevation_m = 152.5 / 3.0;
            edge_recipe.m_bedrock_thickness_m = 25.0;
            edge_recipe.m_bedrock_material = 633u;
            edge_recipe.m_start_depth_m = 20.0;
            const auto edge_plan = compileGenerationPlan(edge_recipe, 42u);
            PPR_TEST_ASSERT(edge_plan.has_value());
            const auto edge_layout = restoreBiomeLayout(*edge_plan, std::array{GenerationSite{20.0, 35.0, 0u}});
            PPR_TEST_ASSERT(edge_layout.has_value());
            const GenerationGeometry edge_geometry;
            const auto edge_raster = compileGenerationRaster(*edge_plan, *edge_layout, edge_geometry);
            PPR_TEST_ASSERT(edge_raster.has_value());
            const auto edge_cells = edge_raster->chunk({0u, 1u});
            PPR_TEST_ASSERT(edge_cells.has_value());
            PPR_TEST_ASSERT((*edge_cells)[24u * kChunkEdge].m_element == 17u);
            PPR_TEST_ASSERT((*edge_cells)[25u * kChunkEdge] == Cell{});
            recipe.m_start_depth_m = 10.0;
            PPR_TEST_ASSERT(not compileGenerationPlan(recipe, 42u));
            recipe.m_start_depth_m = 45.0;
            recipe.m_materials.back().m_diggable = true;
            PPR_TEST_ASSERT(not compileGenerationPlan(recipe, 42u));
        };
        PPR_UNIT_TEST (warped_beds_and_clustered_pores_replay_through_split_filters) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0, 3301u});
            recipe.m_materials[0u].m_key = 1701u;
            auto &biome = recipe.m_biomes[0u];
            auto &band = biome.m_bands[0u];
            band.m_choices.push_back({204u, 33u, 1.0});
            band.m_bed_period_m = 22.0;
            band.m_bed_warp_m = 3.0;
            band.m_bed_dip = 0.05;
            biome.m_pore_probability = 0.2;
            biome.m_pore_affinity_min = -1.0;
            biome.m_sprinkle_probability = 0.2;
            const auto raw_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(raw_plan.has_value());
            const auto layout = restoreBiomeLayout(*raw_plan, std::array{GenerationSite{100.0, 100.0, 0u}});
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 70, 80, false});
            const auto raw_raster = compileGenerationRaster(*raw_plan, *layout, geometry);
            PPR_TEST_ASSERT(raw_raster.has_value());
            const auto raw = raw_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(raw.has_value());
            biome.m_pore_passes = 3u;
            biome.m_relaxation_passes = 8u;
            biome.m_finish_passes = 3u;
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            const auto left = raster->chunk({0u, 0u}), right = raster->chunk({1u, 0u}),
                       replay = raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(left and right and replay and *left == *replay and *left != *raw);
            u32 pores = 0u, pore_changes = 0u, first_material = 0u, second_material = 0u;
            for (u32 y = 3u; y < 70u; ++y) {
                for (u32 x = 3u; x < kChunkEdge; ++x) {
                    const auto &cell = (*left)[y * kChunkEdge + x];
                    pores += cell.m_element == 0u ? 1u : 0u;
                    pore_changes += (cell.m_element == 0u) != ((*raw)[y * kChunkEdge + x].m_element == 0u) ? 1u : 0u;
                    first_material += cell.m_element == 17u ? 1u : 0u;
                    second_material += cell.m_element == 33u ? 1u : 0u;
                    PPR_TEST_ASSERT(cell.m_element == 0u ? cell.m_temperature == 0.0f : cell.m_temperature > 0.0f);
                }
            }
            PPR_TEST_ASSERT(pores > 0u and pore_changes > 0u and first_material > 0u and second_material > 0u);
            for (u32 y = 80u; y < 110u; ++y) {
                for (u32 x = 70u; x < 100u; ++x) {
                    PPR_TEST_ASSERT((*left)[y * kChunkEdge + x].m_element == 0u);
                }
            }
        };
        PPR_UNIT_TEST (supported_formations_use_frozen_contacts_and_preserve_backbone_clearance) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0});
            auto &biome = recipe.m_biomes[0u];
            biome.m_formation_probability = 0.2;
            biome.m_formation_length_m = 5.0;
            biome.m_formation_material = 33u;
            biome.m_rind_material = 33u;
            recipe.m_rules.push_back({501u, 1u, EGenerationPhase::carve, GenerationConnection{2.2, 0u, 1u}});
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto layout = restoreBiomeLayout(*plan, std::array{GenerationSite{100.0, 100.0, 0u}});
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_segments = {{0u, 40.5, 60.5, 180.5, 60.5, 2.2}, {0u, 180.5, 60.5, 180.5, 90.5, 2.2}};
            ChunkGrid grid;
            for (u32 x = 16u; x < 400u; ++x) {
                PPR_TEST_ASSERT(not grid.setCell({x, 32u}, {17u, 293.0f}));
                PPR_TEST_ASSERT(not grid.setCell({x, 96u}, {17u, 293.0f}));
            }
            u64 scratch{};
            PPR_TEST_ASSERT(not finishGenerationTerrain(*plan, *layout, geometry, grid, nullptr, &scratch));
            PPR_TEST_ASSERT(scratch > 0u);
            u32 growth{};
            for (u32 y = 33u; y < 96u; ++y) {
                for (u32 x = 16u; x < 400u; ++x) {
                    growth += grid.getCell({x, y})->m_element == 33u ? 1u : 0u;
                }
            }
            PPR_TEST_ASSERT(growth > 0u);
            PPR_TEST_ASSERT(not validateGenerationBackbone(*plan, geometry, grid));
            PPR_TEST_ASSERT(not grid.setCell({100u, 60u}, {17u, 293.0f}));
            PPR_TEST_ASSERT(
                validateGenerationBackbone(*plan, geometry, grid) ==
                make_error_code(EGenerationErrc::constraints_exhausted));
            PPR_TEST_ASSERT(not grid.setCell({100u, 60u}, {}));
            geometry.m_segments.back().m_x0_m += 0.01;
            PPR_TEST_ASSERT(
                validateGenerationBackbone(*plan, geometry, grid) ==
                make_error_code(EGenerationErrc::constraints_exhausted));
        };

        PPR_UNIT_TEST (initial_matter_is_disjoint_canonical_and_mass_uses_empty_geometry) {
            auto plan = compileGenerationPlan(matterRecipe(), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            ChunkGrid grid;
            for (u32 y = 60u; y < 69u; ++y) {
                for (u32 x = 60u; x < 69u; ++x) {
                    if (x == 60u or x == 68u or y == 60u or y == 68u) {
                        PPR_TEST_ASSERT(not grid.setCell({x, y}, {17u, 293.0f}));
                    }
                }
            }
            GenerationGeometry geometry;
            geometry.m_instances.push_back({1u, 0u, 0u, 60, 60, false});
            auto first = resolveInitialMatter(*plan, geometry, grid),
                 replay = resolveInitialMatter(*plan, geometry, grid);
            PPR_TEST_ASSERT(first and replay);
            PPR_TEST_ASSERT(first->m_regions.size() == 2u and first->m_span_count == 7u);
            PPR_TEST_ASSERT(first->m_searched_cells == 98u and first->m_largest_component == 49u);
            std::array<bool, 81u> assigned{};
            double volume = 0.0;
            for (u32 r = 0u; r < first->m_regions.size(); ++r) {
                const auto &region = first->m_regions[r], &other = replay->m_regions[r];
                PPR_TEST_ASSERT(region.m_identity == other.m_identity and region.m_spans == other.m_spans);
                PPR_TEST_ASSERT(region.m_volume_m3 == other.m_volume_m3 and region.m_mass_kg == other.m_mass_kg);
                PPR_TEST_ASSERT(region.m_mass_kg == region.m_volume_m3 * region.m_bulk_density_kg_m3);
                PPR_TEST_ASSERT(region.m_material == (r == 0u ? 1001u : 1009u));
                for (const auto &span: region.m_spans) {
                    PPR_TEST_ASSERT(span.m_begin_x == 61u and span.m_end_x == 68u);
                    PPR_TEST_ASSERT(r == 0u ? span.m_y < 64u : span.m_y >= 64u);
                    for (u32 x = span.m_begin_x; x < span.m_end_x; ++x) {
                        const auto index = (span.m_y - 60u) * 9u + x - 60u;
                        PPR_TEST_ASSERT(not assigned[index]);
                        assigned[index] = true;
                        PPR_TEST_ASSERT(grid.getCell({x, span.m_y})->m_element == 0u);
                    }
                }
                volume += region.m_volume_m3;
            }
            // Four corner void cells lose1/8 cell each to their diagonal solid
            // neighbour. Whole-cell counting would incorrectly give49/9 m3.
            PPR_TEST_ASSERT(std::abs(volume - 48.5 / 9.0) < 1.0e-12);
            PPR_TEST_ASSERT(first->m_regions[0u].m_fill_height_m == std::optional<double>{64.0 / 3.0});
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            auto cancelled = resolveInitialMatter(*plan, geometry, grid, context.get());
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(not grid.setCell({64u, 64u}, {17u, 293.0f}));
            auto blocked = resolveInitialMatter(*plan, geometry, grid);
            PPR_TEST_ASSERT(not blocked and blocked.error() == make_error_code(EGenerationErrc::matter_anchor_solid));
        };
        PPR_UNIT_TEST (initial_matter_preserves_integer_boundaries_and_gas_is_remaining_volume) {
            for (const bool signed_height: {false, true}) {
                auto recipe = matterRecipe();
                if (signed_height) {
                    recipe.m_features[1u].m_bounds = {0, -9, 9, 0};
                    recipe.m_features[1u].m_operations[0u].m_y = -9;
                    recipe.m_features[1u].m_markers[0u].m_y = -5;
                }
                std::get<GenerationFill>(recipe.m_rules[1u].m_parameters).m_fill_height_m = signed_height ? -8.0 / 3.0
                                                                                                          : 1.0 / 3.0;
                // Gas deliberately appears before liquid in authored order.
                std::swap(recipe.m_rules[1u], recipe.m_rules[2u]);
                recipe.m_rules[1u].m_order = 0u;
                recipe.m_rules[2u].m_order = 1u;
                auto plan = compileGenerationPlan(std::move(recipe), 42u);
                PPR_TEST_ASSERT(plan.has_value());
                ChunkGrid grid;
                // Sample row25 is inside; the solid shell immediately below
                // sits outside the reservation. Its contour ends at boundary25.
                for (u32 y = 24u; y <= 33u; ++y) {
                    for (u32 x = 60u; x <= 68u; ++x) {
                        if (x == 60u or x == 68u or y == 24u or y == 33u) {
                            PPR_TEST_ASSERT(not grid.setCell({x, y}, {17u, 293.0f}));
                        }
                    }
                }
                GenerationGeometry geometry;
                geometry.m_instances.push_back({1u, 0u, 0u, 60, signed_height ? 34 : 25, false});
                auto result = resolveInitialMatter(*plan, geometry, grid);
                PPR_TEST_ASSERT(result and result->m_regions.size() == 2u);
                const auto &liquid = result->m_regions[0u], &gas = result->m_regions[1u];
                PPR_TEST_ASSERT(liquid.m_material == 1001u and gas.m_material == 1009u);
                PPR_TEST_ASSERT(liquid.m_spans.size() == 1u and liquid.m_spans[0u].m_y == 25u);
                PPR_TEST_ASSERT(liquid.m_fill_height_m == std::optional<double>{26.0 / 3.0});
                PPR_TEST_ASSERT(gas.m_spans.size() == 7u and gas.m_spans[0u].m_y == 26u);
            }
        };

        PPR_UNIT_TEST (initial_matter_rejects_diagonal_saddle_leaks) {
            auto plan = compileGenerationPlan(matterRecipe(), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            ChunkGrid grid;
            // Isolate the marker's void sample with solid orthogonal neighbours,
            // but leave a diagonal path to the exterior. Four-neighbour flood
            // would falsely call this sealed; shared saddle geometry is open.
            for (const auto position:
                {GlobalCellPos{63u, 64u}, GlobalCellPos{65u, 64u}, GlobalCellPos{64u, 63u}, GlobalCellPos{64u, 65u}}) {
                PPR_TEST_ASSERT(not grid.setCell(position, {17u, 293.0f}));
            }
            GenerationGeometry geometry;
            geometry.m_instances.push_back({1u, 0u, 0u, 60, 60, false});
            auto result = resolveInitialMatter(*plan, geometry, grid);
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EGenerationErrc::matter_not_sealed));
        };

        PPR_UNIT_TEST (start_validation_proves_supported_body_route_and_resource_components) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            recipe.m_features[0u].m_markers.push_back({304u, EGenerationMarker::resource_anchor, 6, 5});
            recipe.m_features[0u].m_markers.push_back({305u, EGenerationMarker::resource_anchor, 23, 5});
            recipe.m_resources = {{601u, 1u, 9u, 17u, 20.0}, {602u, 2u, 9u, 33u, 20.0}};
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            ChunkGrid grid;
            for (u32 y = 3000u; y < 3030u; ++y) {
                for (u32 x = 2000u; x < 2030u; ++x) {
                    if (x < 2003u or x >= 2027u or y < 3006u or y >= 3027u) {
                        PPR_TEST_ASSERT(
                            not grid.setCell({x, y}, {x >= 2022u and x <= 2024u ? u16{33u} : u16{17u}, 293.0f}));
                    }
                }
            }
            auto start = validateGenerationStart(*plan, grid);
            PPR_TEST_ASSERT(start.has_value());
            PPR_TEST_ASSERT(start->m_spawn_x_m == 2015.5 / 3.0);
            PPR_TEST_ASSERT(start->m_floor_y_m == 1002.0);
            PPR_TEST_ASSERT(std::abs(start->m_spawn_y_m - 1002.9) < 1.0e-12);
            PPR_TEST_ASSERT(start->m_resource_fine_cells.size() == 2u);
            PPR_TEST_ASSERT(start->m_resource_fine_cells[0u] >= 9u and start->m_resource_fine_cells[1u] == 18u);
            // Point-cell clearance would miss the need for continuous support.
            for (u32 y = 3000u; y < 3006u; ++y) {
                PPR_TEST_ASSERT(not grid.setCell({2010u, y}, {}));
            }
            auto gap = validateGenerationStart(*plan, grid);
            PPR_TEST_ASSERT(not gap and gap.error() == make_error_code(EGenerationErrc::start_invalid));
            for (u32 y = 3000u; y < 3006u; ++y) {
                PPR_TEST_ASSERT(not grid.setCell({2010u, y}, {17u, 293.0f}));
            }
            PPR_TEST_ASSERT(not grid.setCell({2015u, 3013u}, {17u, 293.0f}));
            auto ceiling = validateGenerationStart(*plan, grid);
            PPR_TEST_ASSERT(not ceiling and ceiling.error() == make_error_code(EGenerationErrc::start_invalid));
            PPR_TEST_ASSERT(not grid.setCell({2015u, 3013u}, {}));
            recipe.m_resources[1u].m_minimum_fine_cells = 19u;
            auto scarce_plan = compileGenerationPlan(std::move(recipe), 42u);
            PPR_TEST_ASSERT(scarce_plan.has_value());
            auto scarce = validateGenerationStart(*scarce_plan, grid);
            PPR_TEST_ASSERT(not scarce and scarce.error() == make_error_code(EGenerationErrc::start_invalid));
            auto buried_recipe = plan->recipe();
            buried_recipe.m_features[0u].m_markers[1u].m_x = 15;
            buried_recipe.m_features[0u].m_markers[1u].m_y = 4;
            buried_recipe.m_features[0u].m_markers[2u].m_x = 24;
            buried_recipe.m_resources = {{601u, 1u, 1u, 33u, 1.5}, {602u, 2u, 9u, 33u, 20.0}};
            for (u32 x = 2015u; x <= 2024u; ++x) {
                PPR_TEST_ASSERT(not grid.setCell({x, 3004u}, {33u, 293.0f}));
            }
            auto reachable_recipe = buried_recipe;
            reachable_recipe.m_resources[0u].m_max_distance_m = 20.0;
            auto reachable_plan = compileGenerationPlan(std::move(reachable_recipe), 42u);
            PPR_TEST_ASSERT(reachable_plan.has_value());
            PPR_TEST_ASSERT(validateGenerationStart(*reachable_plan, grid).has_value());
            auto buried_plan = compileGenerationPlan(std::move(buried_recipe), 42u);
            PPR_TEST_ASSERT(buried_plan.has_value());
            auto buried = validateGenerationStart(*buried_plan, grid);
            PPR_TEST_ASSERT(not buried and buried.error() == make_error_code(EGenerationErrc::start_invalid));
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            auto cancelled = validateGenerationStart(*plan, grid, context.get());
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        };

        PPR_UNIT_TEST (compiled_noise_algorithms_replay_and_support_concurrent_sampling) {
            const NoiseFieldDesc base_desc{.m_frequency_per_metre = 0.025, .m_octaves = 1u};
            auto billow_desc = base_desc;
            billow_desc.m_fractal = ENoiseFractal::billow;
            auto base = compileNoiseField(base_desc, 42u);
            auto billow = compileNoiseField(billow_desc, 42u);
            PPR_TEST_ASSERT(base and billow);
            for (u32 index = 0u; index < 32u; ++index) {
                const double x = index * 17.25 - 120.0;
                PPR_TEST_ASSERT(
                    std::abs(billow->sample(x, 57.75) - (2.0f * std::abs(base->sample(x, 57.75)) - 1.0f)) <= 1.0e-7f);
            }
            std::array<float, 32u> previous{};
            bool have_previous = false;
            for (const auto algorithm:
                {ENoiseAlgorithm::open_simplex2, ENoiseAlgorithm::perlin, ENoiseAlgorithm::cellular_distance,
                    ENoiseAlgorithm::cellular_value}) {
                for (const auto fractal:
                    {ENoiseFractal::none, ENoiseFractal::fbm, ENoiseFractal::ridged, ENoiseFractal::billow}) {
                    const NoiseFieldDesc desc{
                        .m_algorithm = algorithm,
                        .m_fractal = fractal,
                        .m_frequency_per_metre = 0.025,
                        .m_warp_amplitude_m = 12.0};
                    auto field = compileNoiseField(desc, 0x8000000012345678ull);
                    auto replay = compileNoiseField(desc, 0x8000000012345678ull);
                    auto other = compileNoiseField(desc, 0x12345678ull);
                    PPR_TEST_ASSERT(field and replay and other);
                    const auto samples = [](const NoiseField &noise) {
                        std::array<float, 32u> values{};
                        for (std::size_t index = 0u; index < values.size(); ++index) {
                            values[index] = noise.sample(static_cast<double>(index) * 17.25 - 120.0, 57.75);
                            PPR_TEST_ASSERT(std::isfinite(values[index]));
                        }
                        return values;
                    };
                    const auto expected = samples(*field);
                    if (fractal == ENoiseFractal::billow) {
                        PPR_TEST_ASSERT(std::ranges::all_of(expected, [](const float value) {
                            return value >= -1.0f and value <= 1.0f;
                        }));
                    }
                    auto concurrent = std::async(std::launch::async, [&] { return samples(*field); });
                    PPR_TEST_ASSERT(samples(*field) == expected);
                    PPR_TEST_ASSERT(samples(*replay) == expected);
                    PPR_TEST_ASSERT(concurrent.get() == expected);
                    PPR_TEST_ASSERT(samples(*other) != expected);
                    if (have_previous) {
                        PPR_TEST_ASSERT(previous != expected);
                    }
                    previous = expected;
                    have_previous = true;
                }
            }
        };

        PPR_UNIT_TEST (compiled_noise_samples_coordinate_limits_and_full_seed_domain) {
            for (const auto algorithm:
                {ENoiseAlgorithm::open_simplex2, ENoiseAlgorithm::perlin, ENoiseAlgorithm::cellular_distance,
                    ENoiseAlgorithm::cellular_value}) {
                const NoiseFieldDesc desc{
                    .m_algorithm = algorithm,
                    .m_fractal = ENoiseFractal::fbm,
                    .m_frequency_per_metre = 0.125,
                    .m_octaves = 16u,
                    .m_lacunarity = 1.25,
                    .m_warp_amplitude_m = kNoiseCoordinateLimitMetres};
                for (const u64 seed: {0ull, 0x7fffffffull, 0x80000000ull, 0xffffffffffffffffull}) {
                    auto field = compileNoiseField(desc, seed);
                    PPR_TEST_ASSERT(field.has_value());
                    for (const double x: {-kNoiseCoordinateLimitMetres, 0.0, kNoiseCoordinateLimitMetres}) {
                        for (const double y: {-kNoiseCoordinateLimitMetres, 0.0, kNoiseCoordinateLimitMetres}) {
                            PPR_TEST_ASSERT(std::isfinite(field->sample(x, y)));
                        }
                    }
                }
            }
        };

        PPR_UNIT_TEST (noise_description_rejects_invalid_work_and_numeric_domains) {
            const NoiseFieldDesc valid{.m_frequency_per_metre = 0.025};
            PPR_TEST_ASSERT(compileNoiseField(valid, 42u).has_value());
            auto transformed = valid;
            transformed.m_axis_scale_x = 2.0;
            transformed.m_axis_scale_y = 0.5;
            transformed.m_rotation_degrees = 90.0;
            auto scaled_field = compileNoiseField(transformed, 42u);
            auto base_field = compileNoiseField(valid, 42u);
            PPR_TEST_ASSERT(scaled_field and base_field);
            PPR_TEST_ASSERT(std::abs(scaled_field->sample(12.0, 40.0) - base_field->sample(-20.0, 24.0)) < 1.0e-6f);
            for (const double scale: {0.124, 8.001, std::numeric_limits<double>::quiet_NaN()}) {
                transformed.m_axis_scale_x = scale;
                PPR_TEST_ASSERT(validateNoiseFieldDescription(transformed) == std::errc::invalid_argument);
            }
            for (const double frequency:
                {0.0, -1.0, 1.0e-300, 1.0e300, std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN()}) {
                auto invalid = valid;
                invalid.m_frequency_per_metre = frequency;
                const auto rejected = compileNoiseField(invalid, 42u);
                PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::invalid_argument);
            }
            for (const u32 octaves: {0u, 17u}) {
                auto invalid = valid;
                invalid.m_octaves = octaves;
                PPR_TEST_ASSERT(not compileNoiseField(invalid, 42u));
            }
            auto invalid = valid;
            invalid.m_algorithm = static_cast<ENoiseAlgorithm>(255u);
            PPR_TEST_ASSERT(not compileNoiseField(invalid, 42u));
            invalid = valid;
            invalid.m_warp_amplitude_m = -1.0;
            PPR_TEST_ASSERT(not compileNoiseField(invalid, 42u));
            invalid = valid;
            invalid.m_gain = std::numeric_limits<double>::quiet_NaN();
            PPR_TEST_ASSERT(not compileNoiseField(invalid, 42u));
        };

        // Element ids below are numeric literals matching the colony-owned
        // registry (`game/colony/Colony.Elements.h`): the sim test target must
        // not depend on game code, so the mapping is documented, not included.
        // 0 vacuum, 1 rock, 2 ice, 3 caustic, 4 oil, 5 ruins, 6 abyssal, 7 vent.

        class WorkerCancellation final : public IContext {
            ParallelScheduler &m_pool;
            SharedContext m_context;
            context::CancelFunc m_cancel;
            const u32 m_threshold;
            mutable std::atomic<u32> m_worker_checks{};

        public:
            WorkerCancellation(ParallelScheduler &pool, const u32 threshold) : m_pool(pool), m_threshold(threshold) {
                std::tie(m_context, m_cancel) = context::withCancel(context::background());
            }
            std::error_code error() const noexcept override {
                if (m_pool.isWorkerThread() and m_worker_checks.fetch_add(1u) + 1u >= m_threshold) {
                    m_cancel();
                }
                return m_context->error();
            }
            u32 workerChecks() const noexcept {
                return m_worker_checks.load();
            }
            std::optional<const opaque::Block::Value *> value(const string_literal key) const noexcept override {
                return m_context->value(key);
            }
            TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
                return m_context->subscribeEvent(signal);
            }
            void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
                m_context->unsubscribeEvent(signal, restore);
            }
            bool pollEvent() noexcept override {
                return m_context->pollEvent();
            }
            void resetEvent() noexcept override {
                m_context->resetEvent();
            }
        };

        PPR_UNIT_TEST (owned_terrain_cancels_admitted_work_and_same_pool_worker_without_deadlock) {
            auto plan = compileGenerationPlan(numericRecipe(), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            auto geometry = compileGenerationGeometry(*plan, *layout);
            PPR_TEST_ASSERT(geometry.has_value());
            ParallelScheduler pool{"Terrain cancellation test", 1u, 4096u};
            WorkerCancellation context{pool, 10u};
            auto result = rasterizeGenerationTerrain(
                *plan, *layout, *geometry, {.m_pool = &pool, .m_context = &context});
            PPR_TEST_ASSERT(not result and result.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(context.workerChecks() >= 10u);

            WorkerCancellation nested_context{pool, 10u};
            auto nested = pool.schedule([&](const IContext &) {
                return rasterizeGenerationTerrain(
                    *plan, *layout, *geometry, {.m_pool = &pool, .m_context = &nested_context});
            });
            PPR_TEST_ASSERT(nested.has_value());
            auto nested_result = nested->get();
            PPR_TEST_ASSERT(not nested_result and nested_result.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(nested_context.workerChecks() >= 10u);
            PPR_TEST_ASSERT(not pool.join());
            PPR_TEST_ASSERT(pool.getNumOutstandingTasks() == 0u);
        };

        [[nodiscard]] u64 gridHash(const ChunkGrid &grid) {
            u64 hash = 14695981039346656037ULL;
            for (u32 chunk_y = 0u; chunk_y < kChunksPerEdge; ++chunk_y) {
                for (u32 chunk_x = 0u; chunk_x < kChunksPerEdge; ++chunk_x) {
                    const auto cells = grid.residentCells(ChunkPos{chunk_x, chunk_y});
                    PPR_TEST_ASSERT(cells.has_value());
                    for (const Cell &cell: *cells) {
                        // Single FNV-1a round over the packed (element,
                        // temperature-bits) payload: the same
                        // equality/inequality discrimination at half the
                        // multiplies per cell.
                        const u64 packed = (static_cast<u64>(cell.m_element) << 32u) |
                                           std::bit_cast<u32>(cell.m_temperature);
                        hash ^= packed;
                        hash *= 1099511628211ULL;
                    }
                }
            }

            return hash;
        }

        PPR_UNIT_TEST (owned_terrain_replays_across_serial_and_parallel_execution) {
            struct Clock final : ITimerClock {
                mutable TimePoint m_now{};
                TimePoint now() const noexcept override {
                    m_now += std::chrono::milliseconds{1};
                    return m_now;
                }
            } clock;
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({633u, true, false, EGenerationMatter::solid, 3200.0, 63301u});
            recipe.m_open_surface = true;
            recipe.m_surface_elevation_m = 1120.0;
            recipe.m_bedrock_thickness_m = 25.0;
            recipe.m_bedrock_material = 633u;
            recipe.m_start_depth_m = 120.0;
            recipe.m_features[0u].m_operations[0u].m_x = 3;
            recipe.m_features[0u].m_operations[0u].m_y = 6;
            recipe.m_features[0u].m_operations[0u].m_width = 24u;
            recipe.m_features[0u].m_operations[0u].m_height = 21u;
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0, 3301u});
            recipe.m_biomes[0u].m_pore_probability = 0.05;
            recipe.m_biomes[0u].m_pore_affinity_min = -1.0;
            recipe.m_biomes[0u].m_pore_passes = 1u;
            recipe.m_biomes[0u].m_relaxation_passes = 1u;
            recipe.m_biomes[0u].m_finish_passes = 2u;
            recipe.m_biomes[0u].m_erosion_passes = 2u;
            recipe.m_biomes[0u].m_deposits.push_back(
                {.m_key = 601u,
                    .m_material = 33u,
                    .m_host = 17u,
                    .m_depth_max = 1.0,
                    .m_threshold_min = -0.15,
                    .m_threshold_max = 0.15,
                    .m_stage = EGenerationDepositStage::detail});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            std::atomic<u32> completed{};
            std::atomic<EGenerationStage> stage{EGenerationStage::layout};
            auto serial = generateWorld(std::move(*plan), clock, {.m_completed_chunks = &completed, .m_stage = &stage});
            PPR_TEST_ASSERT(serial.has_value());
            PPR_TEST_ASSERT(serial->m_grid.getCell({0u, 0u})->m_element == 633u);
            PPR_TEST_ASSERT(serial->m_grid.getCell({0u, kWorldEdge - 1u})->m_element == 0u);
            PPR_TEST_ASSERT(serial->m_grid.getCell({0u, 1000u})->m_element == 17u);
            PPR_TEST_ASSERT(completed.load() == kChunkCount and stage.load() == EGenerationStage::complete);
            PPR_TEST_ASSERT(
                serial->m_grid.residentChunkCount() == kChunkCount and serial->m_grid.dirtyChunkCount() == kChunkCount);
            PPR_TEST_ASSERT(
                serial->m_report.m_grid_payload_bytes == static_cast<u64>(kWorldEdge) * kWorldEdge * sizeof(Cell));
            PPR_TEST_ASSERT(serial->m_report.m_retained_known_payload_bytes > serial->m_report.m_grid_payload_bytes);
            PPR_TEST_ASSERT(serial->m_plan.retainedPayloadBytes() > 0u);
            PPR_TEST_ASSERT(serial->m_layout.retainedPayloadBytes() > 0u);
            PPR_TEST_ASSERT(
                serial->m_report.m_known_phase_scratch_bytes[2u] >= static_cast<u64>(kCellsPerChunk) * sizeof(Cell));
            PPR_TEST_ASSERT(
                serial->m_report.m_known_phase_scratch_bytes[3u] >= static_cast<u64>(kWorldEdge) * kWorldEdge / 8u);
            PPR_TEST_ASSERT(serial->m_report.m_known_phase_scratch_bytes[4u] >= 65536u * (sizeof(u8) + sizeof(u64)));
            for (const auto duration: serial->m_report.m_phase_times) {
                PPR_TEST_ASSERT(duration == std::chrono::milliseconds{1});
            }
            u64 cell_count = 0u;
            for (const auto &count: serial->m_report.m_material_counts) {
                cell_count += count.m_cells;
            }
            PPR_TEST_ASSERT(cell_count == static_cast<u64>(kWorldEdge) * kWorldEdge);
            PPR_TEST_ASSERT(serial->m_start.m_floor_y_m == 1002.0);
            ParallelScheduler pool{"Owned terrain test", 3u, 4096u};
            completed.store(0u);
            auto parallel_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(parallel_plan.has_value());
            auto parallel = generateWorld(
                std::move(*parallel_plan), clock, {.m_pool = &pool, .m_completed_chunks = &completed});
            PPR_TEST_ASSERT(parallel.has_value());
            PPR_TEST_ASSERT(completed.load() == kChunkCount);
            PPR_TEST_ASSERT(gridHash(serial->m_grid) == gridHash(parallel->m_grid));
            PPR_TEST_ASSERT(serial->m_report.m_terrain_checksum == parallel->m_report.m_terrain_checksum);
            PPR_TEST_ASSERT(serial->m_report.m_output_checksum == parallel->m_report.m_output_checksum);
            PPR_TEST_ASSERT(
                serial->m_report.m_retained_known_payload_bytes == parallel->m_report.m_retained_known_payload_bytes);
            // Three workers plus coordinator fallback can own at most four
            // chunk buffers; the serial observation includes one already.
            PPR_TEST_ASSERT(
                parallel->m_report.m_known_phase_scratch_bytes[2u] >= serial->m_report.m_known_phase_scratch_bytes[2u]);
            const auto working_raster = compileGenerationRaster(
                parallel->m_plan, parallel->m_layout, parallel->m_geometry);
            PPR_TEST_ASSERT(working_raster.has_value());
            PPR_TEST_ASSERT(
                parallel->m_report.m_known_phase_scratch_bytes[2u] <=
                serial->m_report.m_known_phase_scratch_bytes[2u] + 3u * working_raster->chunkWorkingPayloadBytes());
            PPR_TEST_ASSERT(serial->m_layout.sites().size() == parallel->m_layout.sites().size());
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            completed.store(0u);
            auto cancelled_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(cancelled_plan.has_value());
            auto cancelled = generateWorld(
                std::move(*cancelled_plan), clock,
                {.m_pool = &pool, .m_context = context.get(), .m_completed_chunks = &completed});
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(completed.load() == 0u);
        };


    } // namespace WorldGen
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest world_gen = UnitTest::Named("world_gen") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::WorldGen::generation_random_has_fixed_full_width_streams,
            detail::WorldGen::numeric_plan_owns_recipe_and_isolates_field_streams,
            detail::WorldGen::numeric_plan_rejects_invalid_references_bounds_and_collapsed_weights,
            detail::WorldGen::numeric_plan_accepts_granular_fills_and_exact_signed_thirds,
            detail::WorldGen::numeric_plan_preserves_authored_geometry_boundaries,
            detail::WorldGen::biome_lining_stops_threshold_caves_and_replays_across_chunks,
            detail::WorldGen::particle_seeding_replays_large_regions_with_coherent_positive_mass,
            detail::WorldGen::particle_edits_are_local_atomic_and_pure_digs_preserve_frozen_records,
            detail::WorldGen::macro_layout_replays_spacing_and_spatial_nearest_matches_oracle,
            detail::WorldGen::macro_layout_reports_infeasible_constraints_and_cancellation,
            detail::WorldGen::macro_layout_satisfies_required_biomes_before_weighted_assignment,
            detail::WorldGen::nonlocal_geometry_replays_worms_and_links_only_traversal_rooms,
            detail::WorldGen::nonlocal_geometry_distinguishes_required_exhaustion_optional_omission_and_limits,
            detail::WorldGen::finite_blobs_and_smooth_worms_are_bounded_and_replay,
            detail::WorldGen::coherent_vacuum_palette_is_salted_before_bounded_relaxation,
            detail::WorldGen::relaxation_crosses_chunk_seams_preserves_reservations_and_precedes_detail,
            detail::WorldGen::chunk_raster_preserves_boundaries_host_replacement_and_global_seams,
            detail::WorldGen::chunk_raster_opens_room_portals_but_preserves_mixed_role_sealed_pockets,
            detail::WorldGen::chunk_raster_protects_template_reservations_from_cave_fields,
            detail::WorldGen::compiled_noise_algorithms_replay_and_support_concurrent_sampling,
            detail::WorldGen::matter_area_clips_shared_contours_and_horizontal_height,
            detail::WorldGen::vacuum_palette_preserves_unstamped_sealed_reservation_substrate,
            detail::WorldGen::open_surface_and_continuous_bedrock_override_every_terrain_phase,
            detail::WorldGen::warped_beds_and_clustered_pores_replay_through_split_filters,
            detail::WorldGen::supported_formations_use_frozen_contacts_and_preserve_backbone_clearance,
            detail::WorldGen::initial_matter_is_disjoint_canonical_and_mass_uses_empty_geometry,
            detail::WorldGen::initial_matter_rejects_diagonal_saddle_leaks,
            detail::WorldGen::initial_matter_preserves_integer_boundaries_and_gas_is_remaining_volume,
            detail::WorldGen::start_validation_proves_supported_body_route_and_resource_components,
            detail::WorldGen::compiled_noise_samples_coordinate_limits_and_full_seed_domain,
            detail::WorldGen::noise_description_rejects_invalid_work_and_numeric_domains,
            detail::WorldGen::owned_terrain_replays_across_serial_and_parallel_execution,
            detail::WorldGen::owned_terrain_cancels_admitted_work_and_same_pool_worker_without_deadlock,
        });
    };

    const UnitTest &worldGenTests() noexcept {
        return world_gen;
    }
} // namespace pP::tests
