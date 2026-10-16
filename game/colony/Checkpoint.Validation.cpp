module;
#include <cstddef> // offsetof is a macro and is not exported by import std.
#include "pP/Macros.h"
module game.colony.checkpoint;
import game.colony.agents;
import game.colony.buildings;
import game.colony.pathfinding;
import game.colony.metric;
import engine.asset;
import std;
namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(CheckpointValidation, info, none)

    // ------------------------------------------------------------------
    // validation helpers
    // ------------------------------------------------------------------

    namespace {
        const auto invalid = std::make_error_code(std::errc::invalid_argument);
        const auto incompatible = std::make_error_code(std::errc::protocol_not_supported);
        bool coordinate(const double value) noexcept {
            return std::isfinite(value) and value >= 0.0 and value <= kWorldExtentMetres;
        }
        std::error_code validateBlobs(
            const sim::GenerationGeometry &geometry, const sim::GenerationRecipe &recipe,
            const IContext *const context) {
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, geometry.m_blobs.size() <= 4096u ? std::error_code{} : invalid);
            Array<std::tuple<u32, u32, u32>> identities;
            identities.reserve(geometry.m_blobs.size());

            for (const auto &blob: geometry.m_blobs) {
                PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, context ? context->error() : std::error_code{});
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation, blob.m_biome < recipe.m_biomes.size() and
                                                  blob.m_deposit < recipe.m_biomes[blob.m_biome].m_deposits.size()
                                              ? std::error_code{}
                                              : invalid);
                const auto &deposit = recipe.m_biomes[blob.m_biome].m_deposits[blob.m_deposit];
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation,
                    deposit.m_shape == sim::EGenerationDepositShape::blob and
                            blob.m_ordinal < deposit.m_blob_count and
                            coordinate(blob.m_x_m) and
                            coordinate(blob.m_y_m) and
                            std::isfinite(blob.m_radius_m) and
                            blob.m_radius_m >= deposit.m_radius_min_m and
                            blob.m_radius_m <= deposit.m_radius_max_m and
                            std::isfinite(blob.m_stretch) and
                            blob.m_stretch >= deposit.m_stretch_min and
                            blob.m_stretch <= deposit.m_stretch_max and
                            std::isfinite(blob.m_angle) and
                            blob.m_angle >= 2.0 * std::numbers::pi * (deposit.m_orientation_min_degrees / 360.0) and
                            blob.m_angle <= 2.0 * std::numbers::pi * (deposit.m_orientation_max_degrees / 360.0) and
                            std::isfinite(blob.m_offset_x) and
                            std::isfinite(blob.m_offset_y) and
                            blob.m_offset_x >= 0.0 and
                            blob.m_offset_x <= 1024.0 and
                            blob.m_offset_y >= 0.0 and
                            blob.m_offset_y <= 1024.0
                        ? std::error_code{}
                        : invalid);
                identities.emplace_back(blob.m_biome, blob.m_deposit, blob.m_ordinal);
            }

            std::ranges::sort(identities);
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation,
                std::ranges::adjacent_find(identities) == identities.end() ? std::error_code{} : invalid);
            return {};
        }
        bool validBooleans(const sim::EcsColumnData &column, const std::initializer_list<std::size_t> offsets) {
            for (std::size_t row = 0u; row < column.m_owners.size(); ++row) {
                for (const auto offset: offsets) {
                    if (column.m_bytes[row * column.m_stride + offset] > 1u) {
                        return false;
                    }
                }
            }
            return true;
        }
        std::error_code validateActors(const WorldCheckpoint &checkpoint, sim::Registry &registry) {
            const auto &columns = checkpoint.m_snapshot.m_ecs.m_columns;
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, columns.size() == registry.componentCount() ? std::error_code{} : invalid);

            const auto &body_column = columns[*registry.componentId<sim::BodyDefinition>()];
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation,
                validBooleans(
                    body_column,
                    {offsetof(sim::BodyDefinition, m_enable_sleep), offsetof(sim::BodyDefinition, m_is_awake),
                        offsetof(sim::BodyDefinition, m_is_bullet), offsetof(sim::BodyDefinition, m_fixed_rotation)})
                    ? std::error_code{}
                    : invalid);

            const auto &footprints = columns[*registry.componentId<Footprint>()];
            const auto &errands = columns[*registry.componentId<BuildErrand>()];
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation,
                validBooleans(footprints, {offsetof(Footprint, m_solid), offsetof(Footprint, m_ladder)}) and
                        validBooleans(
                            errands, {offsetof(BuildErrand, m_print) + offsetof(Footprint, m_solid),
                                         offsetof(BuildErrand, m_print) + offsetof(Footprint, m_ladder)})
                    ? std::error_code{}
                    : invalid);

            for (const auto id:
                {registry.componentId<Plan>(), registry.componentId<PathReq>(), registry.componentId<PathComp>()}) {
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation,
                    columns[*id].m_owners.empty() and columns[*id].m_bytes.empty() ? std::error_code{} : invalid);
            }

            Array<u64> keys;
            for (const auto &[entity, agent]: registry.view<Agent>()) {
                const auto *const needs = registry.get<Needs>(entity);
                const auto *const body = registry.get<sim::BodyDefinition>(entity);
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation, needs and
                                                  body and
                                                  registry.get<sim::BodyState>(entity) and
                                                  agent.m_key != 0u and
                                                  std::isfinite(agent.m_speed) and
                                                  agent.m_speed >= 0.0f and
                                                  agent.m_spawn.m_x < sim::kWorldEdge and
                                                  agent.m_spawn.m_y < sim::kWorldEdge and
                                                  std::isfinite(needs->m_o2) and
                                                  std::isfinite(needs->m_rest)
                                              ? std::error_code{}
                                              : invalid);
                keys.push_back(agent.m_key);
            }

            std::ranges::sort(keys);
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, not keys.empty() and
                                              keys.size() <= kAgentCountMax and
                                              std::ranges::adjacent_find(keys) == keys.end() and
                                              keys.size() == checkpoint.m_snapshot.m_bodies.m_records.size() and
                                              body_column.m_owners.size() == keys.size()
                                          ? std::error_code{}
                                          : invalid);

            for (const auto &[entity, body]: registry.view<sim::BodyDefinition>()) {
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation, registry.get<Agent>(entity) and
                                                  body.m_motion <= sim::EMotionKind::dynamic and
                                                  body.m_geometry <= sim::EBodyGeometry::circle and
                                                  std::isfinite(body.m_half_width) and
                                                  body.m_half_width > 0.0f and
                                                  std::isfinite(body.m_half_height) and
                                                  body.m_half_height > 0.0f and
                                                  std::isfinite(body.m_density) and
                                                  body.m_density >= 0.0f and
                                                  std::isfinite(body.m_friction) and
                                                  body.m_friction >= 0.0f and
                                                  std::isfinite(body.m_restitution) and
                                                  body.m_restitution >= 0.0f
                                              ? std::error_code{}
                                              : invalid);
            }

            if (checkpoint.m_controlled) {
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation,
                    registry.isAlive(*checkpoint.m_controlled) and registry.get<Agent>(*checkpoint.m_controlled)
                        ? std::error_code{}
                        : invalid);
            }
            return {};
        }
    }

    // ------------------------------------------------------------------
    // world checkpoint validation
    // ------------------------------------------------------------------

    std::error_code validateWorldCheckpoint(
        const WorldCheckpoint &checkpoint, const ContentCatalog &catalog, const SharedContext &context) {
        const auto &descriptor = checkpoint.m_descriptor;
        PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, context ? context->error() : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, validateWorldMetric(descriptor.m_metric));
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation, descriptor.m_profile.size() <= asset::QualifiedName::reference_domain.m_max_length and
                                          descriptor.m_profile.find(':') != std::string::npos and
                                          asset::qualifyName(descriptor.m_profile, {}) and
                                          descriptor.m_pack_schema_version == asset::Manifest::schema_version and
                                          descriptor.m_generator_version == sim::kGeneratorAlgorithmVersion and
                                          catalog.contentFingerprint() and
                                          *catalog.contentFingerprint() == descriptor.m_content_fingerprint and
                                          catalog.findWorld(descriptor.m_profile)
                                      ? std::error_code{}
                                      : incompatible);
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation,
            checkpoint.m_materials.size() == catalog.materials().size() + 1u ? std::error_code{} : incompatible);
        u32 previous_id = 0u;
        bool first_id = true;
        for (const auto &material: checkpoint.m_materials) {
            const auto *const authored = catalog.findMaterial(material.m_id);
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, ((material.m_id == 0u and material.m_key == "vacuum") or
                                          (authored and authored->m_key == material.m_key)) and
                                              asset::validLocalName(material.m_key) and
                                              (first_id or material.m_id > previous_id)
                                          ? std::error_code{}
                                          : incompatible);
            first_id = false;
            previous_id = material.m_id;
        }
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation, checkpoint.m_tick_hz != 0u and
                                          TimeSpan{std::chrono::seconds{1}} / checkpoint.m_tick_hz > TimeSpan{} and
                                          checkpoint.m_snapshot.m_seed == descriptor.m_seed and
                                          checkpoint.m_snapshot.m_bodies.m_replay_ticks == checkpoint.m_tick and
                                          checkpoint.m_snapshot.m_ecs.m_generations.size() <= 65536u and
                                          checkpoint.m_snapshot.m_streams.empty()
                                      ? std::error_code{}
                                      : invalid);

        auto plan = compileWorldPlan(catalog, descriptor.m_profile, descriptor.m_seed);
        PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, plan);
        const auto &recipe = plan->recipe();
        PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, validateRuntimeWorldParameters(recipe));
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation, sim::validateMatterParticles(
                                      checkpoint.m_particles, recipe.m_materials, recipe.m_cell_size_m, context,
                                      descriptor.m_metric.m_reference_thickness_m));
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation,
            not checkpoint.m_sites.empty() and checkpoint.m_sites.size() <= sim::kMaxGenerationSites ? std::error_code{}
                                                                                                     : invalid);
        for (const auto &site: checkpoint.m_sites) {
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation,
                coordinate(site.m_x_m) and coordinate(site.m_y_m) and site.m_biome < recipe.m_biomes.size()
                    ? std::error_code{}
                    : invalid);
        }
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation, checkpoint.m_geometry.m_instances.size() <= 1025u and
                                          checkpoint.m_geometry.m_segments.size() <= sim::kMaxGenerationSegments and
                                          checkpoint.m_geometry.m_omitted_instances.size() <= 1024u
                                      ? std::error_code{}
                                      : invalid);
        for (const auto &instance: checkpoint.m_geometry.m_instances) {
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation,
                instance.m_feature < recipe.m_features.size() and
                        (instance.m_rule == sim::kGenerationStartRule or instance.m_rule < recipe.m_rules.size()) and
                        instance.m_origin_x >= 0 and
                        instance.m_origin_y >= 0 and
                        instance.m_origin_x < static_cast<i32>(sim::kWorldEdge) and
                        instance.m_origin_y < static_cast<i32>(sim::kWorldEdge) and
                        (not instance.m_reflected or recipe.m_features[instance.m_feature].m_allow_reflection)
                    ? std::error_code{}
                    : invalid);
            if (instance.m_rule == sim::kGenerationStartRule) {
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation, instance.m_feature == recipe.m_start_feature ? std::error_code{} : invalid);
            } else {
                const auto *const placement = std::get_if<sim::GenerationPlacement>(
                    &recipe.m_rules[instance.m_rule].m_parameters);
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation, placement and
                                                  placement->m_feature == instance.m_feature and
                                                  instance.m_ordinal < placement->m_max_count
                                              ? std::error_code{}
                                              : invalid);
            }
        }
        for (const auto &segment: checkpoint.m_geometry.m_segments) {
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, segment.m_rule < recipe.m_rules.size() and
                                              coordinate(segment.m_x0_m) and
                                              coordinate(segment.m_y0_m) and
                                              coordinate(segment.m_x1_m) and
                                              coordinate(segment.m_y1_m) and
                                              std::isfinite(segment.m_radius_m) and
                                              segment.m_radius_m > 0.0 and
                                              segment.m_radius_m <= kWorldExtentMetres
                                          ? std::error_code{}
                                          : invalid);
        }
        PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, validateBlobs(checkpoint.m_geometry, recipe, context.get()));
        for (const auto omitted: checkpoint.m_geometry.m_omitted_instances) {
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, omitted < recipe.m_rules.size() ? std::error_code{} : invalid);
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation,
                std::holds_alternative<sim::GenerationPlacement>(recipe.m_rules[omitted].m_parameters)
                    ? std::error_code{}
                    : invalid);
        }
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation, coordinate(checkpoint.m_start.m_spawn_x_m) and
                                          coordinate(checkpoint.m_start.m_spawn_y_m) and
                                          coordinate(checkpoint.m_start.m_floor_y_m) and
                                          checkpoint.m_start.m_resource_fine_cells.size() == recipe.m_resources.size()
                                      ? std::error_code{}
                                      : invalid);

        sim::Registry registry;
        PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, registerColonyComponents(registry));
        PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, sim::restore(registry, checkpoint.m_snapshot));
        PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, validateActors(checkpoint, registry));
        for (const auto &[entity, footprint]: registry.view<Footprint>()) {
            (void) entity;
            PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, validateFootprint(footprint, &catalog));
        }

        for (const auto &[entity, errand]: registry.view<BuildErrand>()) {
            (void) entity;
            PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, validateFootprint(errand.m_print, &catalog));
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, errand.m_total == errand.m_print.m_width * errand.m_print.m_height and
                                              errand.m_progress <= errand.m_total
                                          ? std::error_code{}
                                          : invalid);
        }
        std::bitset<65536u> terrain_ids;
        terrain_ids.set(0u);

        for (const auto &material: catalog.materials()) {
            if (material.m_representation == EMaterialRepresentation::terrain and
                material.m_phase == EMaterialPhase::solid) {
                terrain_ids.set(material.m_numeric_id);
            }
        }
        std::array<const sim::ChunkDelta *, sim::kChunkCount> chunks{};

        for (const auto &chunk: checkpoint.m_snapshot.m_deltas) {
            PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, context ? context->error() : std::error_code{});
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, chunk.m_x == 0u and
                                              chunk.m_y == 0u and
                                              chunk.m_width == sim::kChunkEdge and
                                              chunk.m_height == sim::kChunkEdge
                                          ? std::error_code{}
                                          : invalid);
            chunks[chunk.m_chunk] = &chunk;
            for (const auto &cell: chunk.m_cells) {
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation, terrain_ids.test(cell.m_element) ? std::error_code{} : invalid);
            }
        }
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation, checkpoint.m_matter.m_regions.size() <= sim::kMaxInitialMatterRegions and
                                          checkpoint.m_matter.m_span_count <= sim::kMaxInitialMatterSpans and
                                          checkpoint.m_matter.m_searched_cells <= sim::kMaxMatterSearchCells and
                                          checkpoint.m_matter.m_largest_component <= sim::kMaxMatterComponentCells
                                      ? std::error_code{}
                                      : invalid);
        Array<sim::InitialMatterSpan> spans;
        spans.reserve(checkpoint.m_matter.m_span_count);
        Array<u64> identities;

        for (const auto &region: checkpoint.m_matter.m_regions) {
            PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, context ? context->error() : std::error_code{});
            const auto *const material = catalog.findMaterial(region.m_material);
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, material and
                                              material->m_representation == EMaterialRepresentation::particle and
                                              std::isfinite(region.m_temperature_k) and
                                              region.m_temperature_k >= 0.0 and
                                              std::isfinite(region.m_bulk_density_kg_m3) and
                                              region.m_bulk_density_kg_m3 > 0.0 and
                                              std::isfinite(region.m_volume_m3) and
                                              region.m_volume_m3 >= 0.0 and
                                              std::isfinite(region.m_mass_kg) and
                                              region.m_mass_kg >= 0.0 and
                                              (not region.m_fill_height_m or coordinate(*region.m_fill_height_m)) and
                                              region.m_spans.size() <= sim::kMaxInitialMatterSpans - spans.size()
                                          ? std::error_code{}
                                          : invalid);
            const double mass = region.m_volume_m3 * region.m_bulk_density_kg_m3;
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation,
                std::isfinite(mass) and std::abs(mass - region.m_mass_kg) <= 1.0e-8 * std::max(1.0, mass)
                    ? std::error_code{}
                    : invalid);
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, std::ranges::is_sorted(
                                          region.m_spans,
                                          [](const auto &a, const auto &b) {
                                              return std::tie(a.m_y, a.m_begin_x, a.m_end_x) <
                                                     std::tie(b.m_y, b.m_begin_x, b.m_end_x);
                                          })
                                          ? std::error_code{}
                                          : invalid);
            identities.push_back(region.m_identity);
            for (const auto span: region.m_spans) {
                PPR_RETURN_ERROR_ON_FAIL(
                    CheckpointValidation,
                    span.m_y < sim::kWorldEdge and span.m_begin_x < span.m_end_x and span.m_end_x <= sim::kWorldEdge
                        ? std::error_code{}
                        : invalid);
                spans.push_back(span);
                // An invalidated baseline remains structurally inspectable after
                // legitimate edits remove containment or the original start route.
                if (checkpoint.m_initial_matter_valid) {
                    for (u32 x = span.m_begin_x; x < span.m_end_x; ++x) {
                        const auto *const chunk = chunks[sim::chunkIndexOf(sim::chunkOf({x, span.m_y}))];
                        PPR_RETURN_ERROR_ON_FAIL(
                            CheckpointValidation,
                            not chunk or
                                    chunk->m_cells[(span.m_y % sim::kChunkEdge) * sim::kChunkEdge + x % sim::kChunkEdge]
                                            .m_element == 0u
                                ? std::error_code{}
                                : invalid);
                    }
                }
            }
        }
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation, spans.size() == checkpoint.m_matter.m_span_count ? std::error_code{} : invalid);
        std::ranges::sort(identities);
        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation,
            std::ranges::adjacent_find(identities) == identities.end() ? std::error_code{} : invalid);

        std::ranges::sort(spans, [](const auto &a, const auto &b) {
            return std::tie(a.m_y, a.m_begin_x, a.m_end_x) < std::tie(b.m_y, b.m_begin_x, b.m_end_x);
        });
        for (std::size_t index = 1u; index < spans.size(); ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation,
                spans[index - 1u].m_y != spans[index].m_y or spans[index - 1u].m_end_x <= spans[index].m_begin_x
                    ? std::error_code{}
                    : invalid);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation, checkpoint.m_report.m_material_counts.size() <= sim::kMaxGenerationMaterials + 1u
                                      ? std::error_code{}
                                      : invalid);
        for (const auto time: checkpoint.m_report.m_phase_times) {
            PPR_RETURN_ERROR_ON_FAIL(CheckpointValidation, time >= TimeSpan{} ? std::error_code{} : invalid);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            CheckpointValidation, checkpoint.m_report.m_grid_payload_bytes <=
                                          static_cast<u64>(sim::kWorldEdge) * sim::kWorldEdge * sizeof(sim::Cell)
                                      ? std::error_code{}
                                      : invalid);

        for (const auto cells: checkpoint.m_start.m_resource_fine_cells) {
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, cells <= sim::kWorldEdge * sim::kWorldEdge ? std::error_code{} : invalid);
        }

        for (const auto &count: checkpoint.m_report.m_material_counts) {
            PPR_RETURN_ERROR_ON_FAIL(
                CheckpointValidation, terrain_ids.test(count.m_material) and
                                              count.m_cells <= static_cast<u64>(sim::kWorldEdge) * sim::kWorldEdge
                                          ? std::error_code{}
                                          : invalid);
        }
        return {};
    }
}
