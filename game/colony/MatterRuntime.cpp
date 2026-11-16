module;
#include "pP/Macros.h"

module game.colony.matter;

import std;
import engine.math;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(GameMatter, info, none)

    // ------------------------------------------------------------------
    // catalog substance properties and stable identity
    // ------------------------------------------------------------------

    namespace {
        matter::ESourceMaterial catalogFamily(const std::string_view key, const sim::EGenerationMatter phase) {
            using Family = matter::ESourceMaterial;
            static constexpr std::pair<std::string_view, Family> entries[]{
                {"water", Family::liquid}, {"brine", Family::liquid}, {"oil", Family::liquid},
                {"crude_oil", Family::liquid}, {"magma", Family::liquid}, {"sand", Family::sand},
                {"sandstone", Family::sand}, {"basalt", Family::sand}, {"coal", Family::sand},
                {"copper_ore", Family::sand}, {"iron_ore", Family::sand}, {"uranium_ore", Family::sand},
                {"wolframite", Family::sand}, {"autunite", Family::sand}, {"azurite", Family::sand},
                {"clay", Family::cohesive}, {"wet_sand", Family::cohesive}, {"snow", Family::compacting},
                {"pebbles", Family::sand}, {"gravel", Family::sand}};

            for (const auto &[name, family]: entries) {
                if (key == name)
                    return family;
            }

            return phase == sim::EGenerationMatter::liquid ? Family::liquid : Family::sand;
        }
    }

    matter::ESourceMaterial matterFamily(const MaterialDefinition &material) {
        return catalogFamily(
            material.m_key, material.m_phase == EMaterialPhase::liquid ? sim::EGenerationMatter::liquid
                                                                       : sim::EGenerationMatter::solid);
    }

    u64 matterMaterialTableIdentity(const std::span<const matter::GpuMaterialProperties> rows, const u32 policy) {
        Array<matter::GpuMaterialProperties> canonical(rows.begin(), rows.end());
        std::ranges::sort(canonical, {}, &matter::GpuMaterialProperties::m_substance_id);

        u64 hash = 14695981039346656037ull;
        const auto append = [&](const u32 word) {
            for (u32 byte = 0u; byte < 4u; ++byte) {
                hash ^= (word >> (byte * 8u)) & 255u;
                hash *= 1099511628211ull;
            }
        };

        append(policy);
        append(static_cast<u32>(canonical.size()));
        for (const auto &row: canonical) {
            const auto words = std::bit_cast<std::array<u32, 19u>>(row);
            for (u32 index = 0u; index < (policy == 1u ? 10u : 19u); ++index)
                append(words[index]);
        }

        return hash;
    }

    Expected<Array<u32>> matterPropertyIndices(const MatterState &state) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, matter::validateMaterialTable(state.m_properties));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameMatter, state.m_records.size() == state.m_particles.size()
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));
        std::unordered_map<u32, u32> indices;
        for (u32 index = 0u; index < state.m_properties.size(); ++index) {
            indices.emplace(state.m_properties[index].m_substance_id, index);
        }

        Array<u32> result;
        result.reserve(state.m_records.size());
        for (std::size_t index = 0u; index < state.m_records.size(); ++index) {
            const auto found = indices.find(state.m_records[index].m_material);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GameMatter,
                found != indices.end() and
                        state.m_particles[index].m_material ==
                            static_cast<float>(state.m_properties[found->second].m_family) and
                        matter::validMaterialState(state.m_particles[index], state.m_properties[found->second])
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
            result.push_back(found->second);
        }

        return result;
    }

    std::error_code setMatterProperties(
        MatterState &state, const std::span<const matter::GpuMaterialProperties> rows, const ContentCatalog &catalog) {
        PPR_RETURN_ERROR_ON_FAIL(GameMatter, matter::validateMaterialTable(rows));
        for (const auto &row: rows) {
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter, row.m_substance_id <= std::numeric_limits<u16>::max() and
                                    catalog.findMaterial(static_cast<u16>(row.m_substance_id)) != nullptr
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GameMatter, state.m_records.size() == state.m_particles.size()
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));
        std::unordered_map<u32, const matter::GpuMaterialProperties *> families;
        for (const auto &row: rows)
            families.emplace(row.m_substance_id, &row);

        for (std::size_t index = 0u; index < state.m_records.size(); ++index) {
            const auto found = families.find(state.m_records[index].m_material);
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter, found != families.end() and
                                    state.m_particles[index].m_material == static_cast<float>(found->second->m_family)
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));

            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter, matter::validMaterialState(state.m_particles[index], *found->second)
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));
        }

        Array<matter::GpuMaterialProperties> owned(rows.begin(), rows.end());
        state.m_properties = std::move(owned);
        return {};
    }

    std::error_code completeMatterProperties(
        MatterState &state, const ContentCatalog *const catalog, const bool legacy) {
        auto rows = state.m_properties;
        PPR_RETURN_ERROR_ON_FAIL(GameMatter, matter::validateMaterialTable(rows));
        std::unordered_set<u32> ids;
        for (const auto &row: rows)
            ids.insert(row.m_substance_id);

        for (const auto &record: state.m_records) {
            if (not ids.insert(record.m_material).second)
                continue;

            const auto *const definition = catalog ? catalog->findMaterial(record.m_material) : nullptr;
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter,
                not catalog or definition ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            matter::GpuMaterialProperties row{};
            row.m_substance_id = record.m_material;
            row.m_family = catalogFamily(definition ? definition->m_key : std::string_view{}, record.m_phase);
            if (legacy) {
                row.m_family = record.m_phase == sim::EGenerationMatter::liquid ? matter::ESourceMaterial::liquid
                                                                                : matter::ESourceMaterial::sand;
                // Fixed v6/v7 migration defaults, independent of future tuning.
                row.m_cohesion = 0.05f;
                row.m_cap = 0.2f;
                row.m_tension = 0.02f;
                row.m_cohesion_hardening = 0.2f;
                row.m_cap_hardening = 0.5f;
                row.m_solid_fraction0 = 0.3f;
                row.m_compaction_rate = 0.5f;
                row.m_solid_exponent = 2.0f;
                row.m_dense_epsilon = 0.01f;
            }

            row.m_elasticity_ratio = state.m_profile.m_elasticity_ratio;
            row.m_elastic_relaxation = state.m_profile.m_elastic_relaxation;
            row.m_liquid_relaxation = state.m_profile.m_liquid_relaxation;
            row.m_liquid_viscosity = state.m_profile.m_liquid_viscosity;
            row.m_friction_angle = state.m_profile.m_friction_angle;
            row.m_plasticity = state.m_profile.m_plasticity;
            row.m_sand_yield_ratio = state.m_profile.m_elasticity_ratio;
            row.m_reference_density = static_cast<float>(
                (definition ? definition->m_reference_density_kg_m3 : record.m_bulk_density_kg_m3) / 1000.0);
            if (not legacy and definition and (definition->m_key == "pebbles" or definition->m_key == "gravel")) {
                row.m_friction_angle = 38.0f;
                row.m_sand_yield_ratio = 1.25f;
            }

            if (not legacy and definition and definition->m_key == "wet_sand")
                row.m_cohesion = 0.02f;

            rows.push_back(row);
        }

        PPR_RETURN_ERROR_ON_FAIL(GameMatter, matter::validateMaterialTable(rows));
        if (catalog) {
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, setMatterProperties(state, rows, *catalog));
            return {};
        }

        state.m_properties = std::move(rows);
        return {};
    }

    // ------------------------------------------------------------------
    // physical inventory and source units
    // ------------------------------------------------------------------

    Expected<MatterState> makeMatterState(
        const sim::MatterParticleSnapshot &snapshot, const double h, const double thickness, const float gravity_y,
        const u64 world, const u32 world_edge, const ContentCatalog *const catalog) {
        if (not std::isfinite(h) or
            h <= 0.0 or
            not std::isfinite(thickness) or
            thickness <= 0.0 or
            not std::isfinite(gravity_y) or
            world == 0u or
            not sim::isSupportedWorldEdge(world_edge)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        MatterState result{};
        result.m_world_edge = world_edge;
        result.m_cell_metres = h;
        result.m_thickness_metres = thickness;
        result.m_next_id = snapshot.m_next_id;
        result.m_identity.m_world = world;
        result.m_profile.m_grid_width = world_edge + 7u;
        result.m_profile.m_grid_height = world_edge + 7u;
        result.m_profile.m_gravity_strength = static_cast<float>(
            static_cast<double>(std::abs(gravity_y)) / ((world_edge + 7u) * h));
        const double volume_unit = h * h * thickness;
        const double mass_unit = 1000.0 * volume_unit;
        if (not std::isfinite(mass_unit) or
            mass_unit <= 0.0 or
            not std::isfinite(result.m_profile.m_gravity_strength)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::result_out_of_range);
        }

        for (const auto &batch: snapshot.m_chunks) {
            if (batch.records().size() > sim::kMaxMatterParticles - result.m_records.size() - result.m_gas.size()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::value_too_large);
            }

            for (const auto &record: batch.records()) {
                if (not std::isfinite(record.m_x_m) or
                    not std::isfinite(record.m_y_m) or
                    record.m_x_m < 0.0 or
                    record.m_y_m < 0.0 or
                    record.m_x_m > world_edge * h or
                    record.m_y_m > world_edge * h) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::result_out_of_range);
                }

                if (record.m_phase == sim::EGenerationMatter::gas) {
                    result.m_gas.push_back(record);
                } else if (record.m_phase == sim::EGenerationMatter::solid or
                           record.m_phase == sim::EGenerationMatter::liquid) {
                    result.m_records.push_back(record);
                } else {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::not_supported);
                }
            }
        }

        std::ranges::sort(result.m_records, {}, &sim::MatterParticle::m_id);
        std::ranges::sort(result.m_gas, {}, &sim::MatterParticle::m_id);
        result.m_particles.reserve(result.m_records.size());
        for (const auto &record: result.m_records) {
            matter::GpuParticle particle{};
            particle.m_position_x = static_cast<float>(record.m_x_m / h + static_cast<double>(kMatterOrigin));
            particle.m_position_y = static_cast<float>(record.m_y_m / h + static_cast<double>(kMatterOrigin));
            particle.m_mass = static_cast<float>(record.m_mass_kg / mass_unit);
            particle.m_volume = static_cast<float>(record.m_volume_m3 / volume_unit);
            const auto *const definition = catalog ? catalog->findMaterial(record.m_material) : nullptr;
            matter::GpuMaterialProperties property{};
            property.m_family = catalogFamily(definition ? definition->m_key : std::string_view{}, record.m_phase);
            matter::initializeMaterialState(particle, property);

            if (not std::isfinite(particle.m_mass) or
                particle.m_mass <= 0.0f or
                not std::isfinite(particle.m_volume) or
                particle.m_volume <= 0.0f or
                not std::isfinite(particle.m_position_x) or
                not std::isfinite(particle.m_position_y)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::result_out_of_range);
            }

            result.m_particles.push_back(particle);
        }

        result.m_profile.m_particle_count = static_cast<u32>(result.m_particles.size());
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, completeMatterProperties(result, catalog));
        return result;
    }

    Expected<sim::MatterParticleSnapshot> captureMovingMatter(
        const MatterState &state, const std::span<const matter::GpuParticle> particles) {
        if (not sim::isSupportedWorldEdge(state.m_world_edge) or particles.size() != state.m_records.size()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        const double edge = state.m_world_edge * state.m_cell_metres;
        std::array<Array<sim::MatterParticle>, sim::kChunkCount> chunks{};
        const auto append = [&](sim::MatterParticle record) -> std::error_code {
            if (not std::isfinite(record.m_x_m) or
                not std::isfinite(record.m_y_m) or
                record.m_x_m < 0.0 or
                record.m_y_m < 0.0 or
                record.m_x_m > edge or
                record.m_y_m > edge) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::result_out_of_range);
            }

            // Closed world endpoints belong to the final presentation chunk.
            // Only the bin changes; the physical coordinate is preserved.
            const sim::GlobalCellPos cell{
                std::min(static_cast<u32>(record.m_x_m / state.m_cell_metres), state.m_world_edge - 1u),
                std::min(static_cast<u32>(record.m_y_m / state.m_cell_metres), state.m_world_edge - 1u)};
            chunks[sim::chunkIndexOf(sim::chunkOf(cell))].push_back(record);
            return {};
        };

        for (std::size_t row = 0u; row < particles.size(); ++row) {
            auto record = state.m_records[row];
            record.m_x_m = (static_cast<double>(particles[row].m_position_x) - static_cast<double>(kMatterOrigin)) *
                           state.m_cell_metres;
            record.m_y_m = (static_cast<double>(particles[row].m_position_y) - static_cast<double>(kMatterOrigin)) *
                           state.m_cell_metres;
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, append(record));
        }

        for (const auto &record: state.m_gas) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, append(record));
        }

        sim::MatterParticleSnapshot result{.m_next_id = state.m_next_id};
        for (u32 index = 0u; index < sim::kChunkCount; ++index) {
            if (chunks[index].empty()) {
                continue;
            }

            std::ranges::sort(chunks[index], {}, &sim::MatterParticle::m_id);
            result.m_chunks.push_back(
                {{index % sim::kChunksPerEdge, index / sim::kChunksPerEdge},
                    std::make_shared<const Array<sim::MatterParticle>>(std::move(chunks[index]))});
        }
        return result;
    }

    // ------------------------------------------------------------------
    // diagnostic physical inventory joined to an exact committed GPU reader
    // ------------------------------------------------------------------

    Expected<MatterCensus> aggregateMatterCensus(
        const MatterPhysicalLedger &ledger, const GridParticleCensus &projected,
        const std::shared_ptr<const GridParticleMetadataResource> &metadata,
        const GridParticlePresentationIdentity &expected, const u64 device_epoch, const u32 world_edge) {
        const auto ledger_owner = ledger.m_metadata_owner.lock();
        const auto projected_owner = projected.m_metadata_owner.lock();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameMatter, metadata and ledger_owner == metadata and projected_owner == metadata
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameMatter, projected.m_identity == expected and
                                projected.m_device_epoch == device_epoch and
                                projected.m_submission != kFenceValueNone and
                                projected.m_world_edge == world_edge and
                                world_edge != 0u and
                                world_edge <= 4096u and
                                projected.m_rows.size() == ledger.m_rows.size()
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));

        MatterCensus result{};
        result.m_cells.reserve(ledger.m_rows.size());
        u64 prior_id{};
        for (std::size_t index = 0u; index < ledger.m_rows.size(); ++index) {
            const auto &physical = ledger.m_rows[index];
            const auto &row = projected.m_rows[index];
            const u64 id = (u64{row.m_id_high} << 32u) | row.m_id_low;
            constexpr u32 known = kGridParticleCensusInvalid |
                                  kGridParticleCensusOutside |
                                  kGridParticleCensusInactive |
                                  kGridParticleCensusVacuum;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GameMatter,
                physical.m_id > prior_id and
                        id == physical.m_id and
                        row.m_material == physical.m_material and
                        std::isfinite(physical.m_volume_m3) and
                        physical.m_volume_m3 > 0.0 and
                        (row.m_status & ~known) == 0u and
                        (((row.m_status & kGridParticleCensusOutside) == 0u and
                             row.m_cell_x < world_edge and
                             row.m_cell_y < world_edge) or
                            (row.m_cell_x == 0xffff'ffffu and
                                row.m_cell_y == 0xffff'ffffu and
                                (row.m_status & (kGridParticleCensusInvalid | kGridParticleCensusOutside)) != 0u))
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
            prior_id = id;

            result.m_cells.push_back(
                {physical.m_volume_m3, 1u, row.m_cell_x, row.m_cell_y, row.m_material, row.m_status});
            result.m_total_volume_m3 += physical.m_volume_m3;
            ++result.m_particle_count;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GameMatter, std::isfinite(result.m_total_volume_m3)
                                ? std::error_code{}
                                : std::make_error_code(std::errc::result_out_of_range));
        }

        // One owned vector is both grouping workspace and output; no hidden
        // tree/hash allocation or mutable physical state participates.
        const auto key = [](const MatterCensusCell &cell) {
            return std::tuple{cell.m_cell_x, cell.m_cell_y, cell.m_material, cell.m_status};
        };

        std::ranges::sort(result.m_cells, {}, key);

        std::size_t groups{};
        for (const auto cell: result.m_cells) {
            if (groups != 0u and key(result.m_cells[groups - 1u]) == key(cell)) {
                auto &prior = result.m_cells[groups - 1u];
                prior.m_volume_m3 += cell.m_volume_m3;
                prior.m_count += cell.m_count;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    GameMatter, std::isfinite(prior.m_volume_m3)
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::result_out_of_range));
            } else {
                result.m_cells[groups++] = cell;
            }
        }

        result.m_cells.resize(groups);
        return result;
    }

    // ------------------------------------------------------------------
    // carried-state admission
    // ------------------------------------------------------------------

    std::error_code validateMatterProfile(
        const matter::GpuParameters &profile, const double h, const double thickness, const float gravity_y,
        const u32 world_edge) {
        const double mass_unit = 1000.0 * h * h * thickness;
        if (not std::isfinite(h) or
            h <= 0.0 or
            not std::isfinite(thickness) or
            thickness <= 0.0 or
            not std::isfinite(gravity_y) or
            not std::isfinite(mass_unit) or
            mass_unit <= 0.0 or
            profile.m_particle_count > sim::kMaxMatterParticles or
            not sim::isSupportedWorldEdge(world_edge)) {
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        matter::GpuParameters expected{};
        expected.m_grid_width = world_edge + 7u;
        expected.m_grid_height = world_edge + 7u;
        expected.m_particle_count = profile.m_particle_count;
        expected.m_gravity_strength = static_cast<float>(
            static_cast<double>(std::abs(gravity_y)) / ((world_edge + 7u) * h));
        if (not std::isfinite(expected.m_gravity_strength) or
            std::bit_cast<std::array<u32, 16u>>(profile) != std::bit_cast<std::array<u32, 16u>>(expected)) {
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }
        return {};
    }

    std::error_code validateMatterState(
        const MatterState &state, const std::span<const sim::GenerationMaterial> materials, const double h,
        const double thickness, const float gravity_y) {
        if (not matter::isSupportedIterations(state.m_iterations) or
            state.m_cell_metres != h or
            state.m_thickness_metres != thickness or
            state.m_identity.m_world == 0u or
            state.m_identity.m_terrain_revision == 0u or
            state.m_particles.size() != state.m_profile.m_particle_count) {
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            GameMatter, validateMatterProfile(state.m_profile, h, thickness, gravity_y, state.m_world_edge));

        auto property_indices = matterPropertyIndices(state);
        PPR_RETURN_ERROR_ON_FAIL(GameMatter, property_indices);
        for (const auto &property: state.m_properties) {
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter,
                std::ranges::any_of(
                    materials, [&](const auto &material) { return material.m_id == property.m_substance_id; })
                    ? std::error_code{}
                    : std::make_error_code(std::errc::invalid_argument));
        }

        auto snapshot = captureMovingMatter(state, state.m_particles);
        PPR_RETURN_ERROR_ON_FAIL(GameMatter, snapshot);
        PPR_RETURN_ERROR_ON_FAIL(
            GameMatter, sim::validateMovingMatterParticles(*snapshot, materials, h, {}, thickness, state.m_world_edge));

        const double volume_unit = h * h * thickness;
        const double mass_unit = 1000.0 * volume_unit;

        for (std::size_t row = 0u; row < state.m_particles.size(); ++row) {
            const auto &particle = state.m_particles[row];
            const auto &record = state.m_records[row];
            for (const float field: std::bit_cast<std::array<float, 24u>>(particle)) {
                if (not std::isfinite(field)) {
                    PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
                }
            }

            const float material = static_cast<float>(state.m_properties[(*property_indices)[row]].m_family);
            if (record.m_phase == sim::EGenerationMatter::gas or
                particle.m_mass != static_cast<float>(record.m_mass_kg / mass_unit) or
                particle.m_volume != static_cast<float>(record.m_volume_m3 / volume_unit) or
                particle.m_material != material or
                particle.m_enabled != 1.0f or
                particle.m_liquid_density <= 0.0f or
                particle.m_mass <= 0.0f or
                particle.m_volume <= 0.0f or
                not std::isfinite(record.m_x_m) or
                not std::isfinite(record.m_y_m) or
                record.m_x_m < 0.0 or
                record.m_y_m < 0.0 or
                record.m_x_m > state.m_world_edge * h or
                record.m_y_m > state.m_world_edge * h or
                static_cast<float>(record.m_x_m / h + static_cast<double>(kMatterOrigin)) != particle.m_position_x or
                static_cast<float>(record.m_y_m / h + static_cast<double>(kMatterOrigin)) != particle.m_position_y or
                (row != 0u and record.m_id <= state.m_records[row - 1u].m_id)) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
            }
        }

        for (const auto &record: state.m_gas) {
            if (record.m_phase != sim::EGenerationMatter::gas) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
            }
        }
        return {};
    }

    // ------------------------------------------------------------------
    // transactional terrain inventory and coverage
    // ------------------------------------------------------------------

    Expected<std::optional<MatterState>> prepareMatterTerrainEdit(
        const MatterState &source, const sim::ChunkGrid &original,
        const std::span<const sim::ChunkGrid::CapturedChunk> candidate, const ContentCatalog &catalog) {
        if (not std::isfinite(source.m_cell_metres) or
            source.m_cell_metres <= 0.0 or
            not std::isfinite(source.m_thickness_metres) or
            source.m_thickness_metres <= 0.0 or
            source.m_world_edge != original.edge() or
            source.m_records.size() != source.m_particles.size() or
            source.m_records.size() + source.m_gas.size() > sim::kMaxMatterParticles) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        for (const auto rows:
            {std::span<const sim::MatterParticle>{source.m_records},
                std::span<const sim::MatterParticle>{source.m_gas}}) {
            for (const auto &record: rows) {
                if (not std::isfinite(record.m_x_m) or
                    not std::isfinite(record.m_y_m) or
                    record.m_x_m < 0.0 or
                    record.m_y_m < 0.0 or
                    record.m_x_m > source.m_world_edge * source.m_cell_metres or
                    record.m_y_m > source.m_world_edge * source.m_cell_metres) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
                }
            }
        }

        if (source.m_identity.m_publication == std::numeric_limits<u64>::max() or
            source.m_identity.m_terrain_revision == std::numeric_limits<u64>::max()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::value_too_large);
        }

        std::bitset<65536u> solids;
        for (const auto &material: catalog.materials()) {
            if (material.m_representation == EMaterialRepresentation::terrain and
                material.m_phase == EMaterialPhase::solid)
                solids.set(material.m_numeric_id);
        }

        std::array<const sim::ChunkGrid::CapturedChunk *, sim::kChunkCount> overlays{};
        for (const auto &chunk: candidate) {
            const auto position = chunk.position();
            if (not original.contains(position)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
            }

            auto &overlay = overlays[sim::chunkIndexOf(position)];
            if (overlay != nullptr) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
            }

            overlay = &chunk;
        }

        const auto cell = [&](const u32 x, const u32 y, const bool edited) {
            const sim::ChunkPos position{x / sim::kChunkEdge, y / sim::kChunkEdge};
            const auto *const overlay = overlays[sim::chunkIndexOf(position)];
            const auto cells = edited and overlay ? overlay->residentCells() : original.residentCells(position);
            return cells ? (*cells)[(y % sim::kChunkEdge) * sim::kChunkEdge + x % sim::kChunkEdge] : sim::Cell{};
        };

        const auto covered = [&](const sim::MatterParticle &record, const bool edited) {
            const double x = record.m_x_m / source.m_cell_metres - 0.5;
            const double y = record.m_y_m / source.m_cell_metres - 0.5;
            const i32 ix = static_cast<i32>(std::floor(x)), iy = static_cast<i32>(std::floor(y));
            const double px = x - ix, py = y - iy;
            constexpr std::array<std::pair<i32, i32>, 4u> offsets{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
            u32 mask{};
            for (u32 corner = 0u; corner < 4u; ++corner) {
                const i32 sx = ix + offsets[corner].first, sy = iy + offsets[corner].second;
                if (sx < 0 or
                    sy < 0 or
                    sx >= static_cast<i32>(original.edge()) or
                    sy >= static_cast<i32>(original.edge()) or
                    solids.test(cell(static_cast<u32>(sx), static_cast<u32>(sy), edited).m_element))
                    mask |= 1u << corner;
            }

            const auto &shape = math::contourCases()[mask];
            const auto points = math::contourPoints();
            for (u32 triangle = 0u; triangle < shape.m_triangle_count; ++triangle) {
                const auto indices = shape.m_triangles[triangle];
                const auto cross = [&](const auto from, const auto to) {
                    return static_cast<double>(to.m_x - from.m_x) * (py - static_cast<double>(from.m_y)) -
                           static_cast<double>(to.m_y - from.m_y) * (px - static_cast<double>(from.m_x));
                };

                const auto a = points[indices.m_a], b = points[indices.m_b], c = points[indices.m_c];
                const double ab = cross(a, b), bc = cross(b, c), ca = cross(c, a);
                if ((ab >= 0.0 and bc >= 0.0 and ca >= 0.0) or (ab <= 0.0 and bc <= 0.0 and ca <= 0.0))
                    return true;
            }
            return false;
        };

        Array<sim::GlobalCellPos> removed;
        for (const auto &chunk: candidate) {
            const auto position = chunk.position();
            for (u32 y = position.m_y * sim::kChunkEdge; y < (position.m_y + 1u) * sim::kChunkEdge; ++y) {
                for (u32 x = position.m_x * sim::kChunkEdge; x < (position.m_x + 1u) * sim::kChunkEdge; ++x) {
                    if (solids.test(cell(x, y, false).m_element) and not solids.test(cell(x, y, true).m_element))
                        removed.push_back({x, y});
                }
            }
        }

        std::ranges::sort(
            removed, [](const auto a, const auto b) { return a.m_y < b.m_y or (a.m_y == b.m_y and a.m_x < b.m_x); });

        if (source.m_next_id == 0u or
            removed.size() > std::numeric_limits<u64>::max() - source.m_next_id or
            removed.size() > sim::kMaxMatterParticles - source.m_records.size() - source.m_gas.size()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::value_too_large);
        }

        MatterState result = source;
        const double volume = source.m_cell_metres * source.m_cell_metres * source.m_thickness_metres;
        for (const auto position: removed) {
            const auto before = cell(position.m_x, position.m_y, false);
            const auto *const material = catalog.findMaterial(before.m_element);
            const double density = material->m_reference_density_kg_m3;
            const double mass = density * volume;
            if (not std::isfinite(mass) or
                mass <= 0.0 or
                not std::isfinite(before.m_temperature) or
                before.m_temperature < 0.0f) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
            }

            sim::MatterParticle record{
                result.m_next_id++, (position.m_x + 0.5) * source.m_cell_metres,
                (position.m_y + 0.5) * source.m_cell_metres, volume, mass, static_cast<double>(before.m_temperature),
                density, before.m_element, sim::EGenerationMatter::solid};
            matter::GpuParticle particle{};
            particle.m_position_x = static_cast<float>(position.m_x) + 0.5f + kMatterOrigin;
            particle.m_position_y = static_cast<float>(position.m_y) + 0.5f + kMatterOrigin;
            particle.m_mass = static_cast<float>(mass / (1000.0 * volume));
            matter::GpuMaterialProperties defaults{};
            defaults.m_family = catalogFamily(material->m_key, record.m_phase);

            // Reuse saved rows for dug particles, including migrated legacy families.
            const auto property = std::ranges::find(
                result.m_properties, record.m_material, &matter::GpuMaterialProperties::m_substance_id);
            if (property != result.m_properties.end())
                particle.m_solid_fraction = property->m_solid_fraction0;

            matter::initializeMaterialState(particle, property != result.m_properties.end() ? *property : defaults);

            if (not std::isfinite(particle.m_mass) or particle.m_mass <= 0.0f) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::result_out_of_range);
            }

            result.m_records.push_back(record);
            result.m_particles.push_back(particle);
        }

        for (const auto &record: result.m_records) {
            if (covered(record, true) and not covered(record, false)) {
                return std::optional<MatterState>{};
            }
        }

        for (const auto &record: result.m_gas) {
            if (covered(record, true) and not covered(record, false)) {
                return std::optional<MatterState>{};
            }
        }

        result.m_profile.m_particle_count = static_cast<u32>(result.m_particles.size());
        ++result.m_identity.m_publication;
        ++result.m_identity.m_terrain_revision;
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, completeMatterProperties(result, &catalog));
        return std::optional{std::move(result)};
    }

    // ------------------------------------------------------------------
    // shared contour occupancy and predicted rigid boundaries
    // ------------------------------------------------------------------

    Expected<Array<u32>> makeMatterTerrain(const sim::ChunkGrid &grid, const std::span<const u16> materials) {
        const hal::ProfileScope scope{"Game.Matter.Terrain"};
        std::bitset<65536u> solid_ids{};
        for (const u16 id: materials) {
            solid_ids.set(id);
        }

        const u32 world_edge = grid.edge();
        const u32 chunks_per_edge = grid.chunksPerEdge();
        Array<std::span<const sim::Cell>> chunks(static_cast<std::size_t>(chunks_per_edge) * chunks_per_edge);
        for (u32 y = 0u; y < chunks_per_edge; ++y) {
            for (u32 x = 0u; x < chunks_per_edge; ++x) {
                const auto cells = grid.residentCells({x, y});
                if (cells) {
                    chunks[static_cast<std::size_t>(y) * chunks_per_edge + x] = *cells;
                }
            }
        }

        const auto solid = [&](const i32 x, const i32 y) {
            if (x < 0 or y < 0 or x >= static_cast<i32>(world_edge) or y >= static_cast<i32>(world_edge)) {
                return true;
            }

            const auto ux = static_cast<u32>(x), uy = static_cast<u32>(y);
            const auto cells =
                chunks[static_cast<std::size_t>(uy / sim::kChunkEdge) * chunks_per_edge + ux / sim::kChunkEdge];
            return not cells.empty() and
                   solid_ids.test(cells[(uy % sim::kChunkEdge) * sim::kChunkEdge + ux % sim::kChunkEdge].m_element);
        };

        const u32 terrain_edge = world_edge + 1u;
        Array<u32> masks(static_cast<std::size_t>(terrain_edge) * terrain_edge, 0u);
        for (u32 y = 0u; y < terrain_edge; ++y) {
            for (u32 x = 0u; x < terrain_edge; ++x) {
                const i32 sx = static_cast<i32>(x) - 1, sy = static_cast<i32>(y) - 1;
                masks[static_cast<std::size_t>(y) * terrain_edge + x] = (solid(sx, sy) ? 1u : 0u) |
                                                                        (solid(sx + 1, sy) ? 2u : 0u) |
                                                                        (solid(sx + 1, sy + 1) ? 4u : 0u) |
                                                                        (solid(sx, sy + 1) ? 8u : 0u);
            }
        }
        return masks;
    }

    Expected<Array<matter::GpuBodyBoundary>> predictMatterBodies(
        const std::span<const physics::BodyBoundarySnapshot> bodies, const double h,
        const matter::GpuParameters &profile) {
        if (not std::isfinite(h) or
            h <= 0.0 or
            bodies.size() > std::numeric_limits<u32>::max() / matter::kGameSubsteps) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        Array<matter::GpuBodyBoundary> result;
        result.reserve(bodies.size() * matter::kGameSubsteps);
        for (u32 step = 0u; step < matter::kGameSubsteps; ++step) {
            const float t = static_cast<float>(step + 1u) * profile.m_delta_time;
            for (const auto &body: bodies) {
                const float angle = body.m_state.m_angle + body.m_state.m_angular_velocity * t;
                const float da = body.m_state.m_angular_velocity * t;
                const float rx = body.m_state.m_x - body.m_com_x, ry = body.m_state.m_y - body.m_com_y;
                const float cx = body.m_com_x + body.m_state.m_velocity_x * t;
                const float cy = body.m_com_y + body.m_state.m_velocity_y * t;
                matter::GpuBodyBoundary row{};
                row.m_shape.m_position_x = static_cast<float>(
                    static_cast<double>(cx + std::cos(da) * rx - std::sin(da) * ry) / h +
                    static_cast<double>(kMatterOrigin));
                row.m_shape.m_position_y = static_cast<float>(
                    static_cast<double>(cy + std::sin(da) * rx + std::cos(da) * ry) / h +
                    static_cast<double>(kMatterOrigin));
                row.m_shape.m_half_size_x = static_cast<float>(static_cast<double>(body.m_half_width) / h);
                row.m_shape.m_half_size_y = static_cast<float>(static_cast<double>(body.m_half_height) / h);
                row.m_shape.m_radius = row.m_shape.m_half_size_x;
                row.m_shape.m_rotation_degrees = -angle * (180.0f / std::numbers::pi_v<float>);
                row.m_shape.m_shape = body.m_geometry == physics::EGeometry::circle ? matter::EStaticShape::circle
                                                                                    : matter::EStaticShape::box;
                row.m_com_x = static_cast<float>(static_cast<double>(cx) / h + static_cast<double>(kMatterOrigin));
                row.m_com_y = static_cast<float>(static_cast<double>(cy) / h + static_cast<double>(kMatterOrigin));
                row.m_velocity_x = static_cast<float>(static_cast<double>(body.m_state.m_velocity_x) / h);
                row.m_velocity_y = static_cast<float>(static_cast<double>(body.m_state.m_velocity_y) / h);
                row.m_angular_velocity = body.m_state.m_angular_velocity;
                result.push_back(row);
            }
        }
        return result;
    }

    // ------------------------------------------------------------------
    // immutable renderer metadata admission
    // ------------------------------------------------------------------

    namespace {
        Expected<Array<GridParticleMetadataRow>> presentationMetadata(
            const MatterState &source, std::shared_ptr<MatterPhysicalLedger> *const physical = nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GameMatter, source.m_records.size() == source.m_profile.m_particle_count and
                                    source.m_records.size() + source.m_gas.size() <= sim::kMaxMatterParticles and
                                    (source.m_cell_metres == 1.0 or source.m_cell_metres == 1.0 / 3.0)
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));
            const double thickness = static_cast<double>(static_cast<float>(source.m_thickness_metres));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GameMatter, std::isfinite(thickness) and thickness > 0.0
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));

            const auto ledger = physical ? std::make_shared<MatterPhysicalLedger>() : nullptr;
            if (ledger) {
                ledger->m_rows.reserve(source.m_records.size() + source.m_gas.size());
            }

            Array<GridParticleMetadataRow> rows;
            rows.reserve(source.m_records.size() + source.m_gas.size());
            const auto append = [&](const sim::MatterParticle &record, const u32 source_row) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter, std::isfinite(record.m_volume_m3) and
                                        record.m_volume_m3 > 0.0 and
                                        std::isfinite(record.m_mass_kg) and
                                        record.m_mass_kg > 0.0 and
                                        std::isfinite(record.m_temperature_k) and
                                        record.m_temperature_k >= 0.0 and
                                        std::isfinite(record.m_x_m) and
                                        std::isfinite(record.m_y_m) and
                                        record.m_x_m >= 0.0 and
                                        record.m_y_m >= 0.0 and
                                        record.m_x_m <= source.m_world_edge * source.m_cell_metres and
                                        record.m_y_m <= source.m_world_edge * source.m_cell_metres
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::invalid_argument));

                if (ledger) {
                    ledger->m_rows.push_back({record.m_id, record.m_volume_m3, record.m_material});
                }

                const double physical_fill = record.m_volume_m3 /
                                             (source.m_cell_metres * source.m_cell_metres * source.m_thickness_metres);
                const float fill = static_cast<float>(physical_fill);
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter,
                    std::isfinite(physical_fill) and physical_fill > 0.0 and std::isnormal(fill) and fill > 0.0f
                        ? std::error_code{}
                        : std::make_error_code(std::errc::argument_out_of_domain));

                GridParticleMetadataRow row{};
                row.m_id_low = static_cast<u32>(record.m_id);
                row.m_id_high = static_cast<u32>(record.m_id >> 32u);
                row.m_source_row = source_row;
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter, record.m_material > 0u and record.m_material < 0xffffu
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::invalid_argument));
                const auto packed = lighting::packCell(static_cast<u16>(record.m_material), record.m_temperature_k);
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, packed ? std::error_code{} : packed.error());
                row.m_material_temperature = *packed;
                row.m_physical_fill_bits = std::bit_cast<u32>(fill);
                row.m_gas_x = static_cast<float>(record.m_x_m);
                row.m_gas_y = static_cast<float>(record.m_y_m);
                row.m_radius = static_cast<float>(std::sqrt(record.m_volume_m3 / thickness / std::numbers::pi));
                rows.push_back(row);
                return {};
            };

            for (u32 index = 0u; index < source.m_records.size(); ++index) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, append(source.m_records[index], index));
            }

            for (const auto &record: source.m_gas) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, append(record, 0xffff'ffffu));
            }

            std::ranges::sort(rows, [](const auto &left, const auto &right) {
                return left.m_id_high < right.m_id_high or
                       (left.m_id_high == right.m_id_high and left.m_id_low < right.m_id_low);
            });
            if (ledger) {
                std::ranges::sort(ledger->m_rows, {}, &MatterPhysicalLedgerRow::m_id);
                *physical = ledger;
            }

            return rows;
        }
    }

    // ------------------------------------------------------------------
    // device-lane handoffs
    // ------------------------------------------------------------------

    struct MatterRuntime::State {
        std::unique_ptr<matter::GpuSolver> m_active{};
        std::unique_ptr<matter::GpuSolver> m_prepared{};
        std::shared_ptr<const GridParticleMetadataResource> m_active_metadata{}, m_prepared_metadata{};
        std::shared_ptr<const MatterPhysicalLedger> m_active_ledger{}, m_prepared_ledger{};
        std::filesystem::path m_content_root{};
        rhi::IDevice *m_device{};
        rhi::ICommandQueue *m_queue{};
        IShaderService *m_shaders{};
        const ITimerClock *m_observation_clock{};
        u64 m_metadata_version{};
        MatterIdentity m_identity{};
        MatterIdentity m_prepared_identity{};
        std::error_code m_submit_error{};
        u32 m_iterations{matter::kDefaultIterations}, m_prepared_iterations{matter::kDefaultIterations};
        bool m_submitted{};
        bool m_ready{};
        bool m_append_prepared{}, m_append_ready{};
        bool m_snapshot{}, m_snapshot_copied{};
    };

    struct MatterRuntime::Pending {
        std::unique_ptr<RenderLaneOperation> m_operation{};
        std::shared_ptr<std::optional<MatterReadback>> m_readback{};
        std::optional<GridParticleGpuView> m_view{};
        std::shared_ptr<const MatterPhysicalLedger> m_physical_ledger{};
        std::optional<RenderLaneOperationTicket> m_ticket{};
        matter::GpuAccounting m_accounting{};
        matter::GpuFailure m_failure{};
        std::optional<matter::GpuFailure> m_stall{};
        u64 m_generation{};
        bool m_shutdown{};
        bool m_cancelled{};
    };

    MatterRuntime::MatterRuntime() : m_instance(std::make_shared<const u8>(u8{})) {
    }

    MatterRuntime::~MatterRuntime() {
        // Best-effort destructor fallback for partial initialization. Failure
        // cannot unwind through device dependencies with unknown completion.
        try {
            if (m_state) {
                if (m_lane) {
                    hal::fatalExit("matter-shutdown-unsettled-lane");
                }

                auto request = beginShutdown();
                if (not request) {
                    hal::fatalExit("matter-shutdown-admission-failure");
                }

                auto completion = advance(*request);
                if (not completion or
                    not *completion or
                    not (**completion).m_execution.m_ran or
                    (**completion).m_execution.m_operation_error or
                    (**completion).m_execution.m_transport_error) {
                    hal::fatalExit("matter-shutdown-failure");
                }
            }
        } catch (const std::bad_alloc &) {
            hal::fatalExit("allocation-failure");
        } catch (...) {
            hal::fatalExit("matter-shutdown-exception");
        }
    }

    Expected<MatterRequest> MatterRuntime::enqueue(
        RenderLaneOperation operation, std::shared_ptr<std::optional<MatterReadback>> readback, const bool shutdown,
        const bool discard, std::shared_ptr<std::optional<matter::GpuFailure>> stall) {
        if (m_pending) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::device_or_resource_busy);
        }

        if (m_generation == std::numeric_limits<u64>::max()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::value_too_large);
        }

        auto pending = std::make_unique<Pending>();
        auto *const result = pending.get();
        const auto state = m_state;
        pending->m_operation = std::make_unique<RenderLaneOperation>(
            [state, result, discard, stall, operation = std::move(operation)]() mutable -> std::error_code {
                const auto error = operation();
                // A lane operation cannot see its own Pending record, so a decoded
                // recoverable verdict travels through the caller's sink.
                if (stall) {
                    result->m_stall = std::move(*stall);
                }

                if (state and state->m_active) {
                    result->m_accounting = state->m_active->accounting();
                    result->m_failure = state->m_submit_error ? matter::GpuFailure{.m_cause = state->m_submit_error}
                                                              : state->m_active->failure();

                    // Discard acknowledges cleanup, even when the solver retains
                    // a numeric failure that deliberately forbids a committed view.
                    if (not error and not discard and state->m_active_metadata) {
                        auto view = state->m_active->committedView();
                        PPR_RETURN_ERROR_ON_FAIL(GameMatter, view);

                        GridParticleGpuView selected{};
                        selected.m_numerical = view->m_hot;
                        selected.m_readonly = view->m_cold;
                        selected.m_metadata = state->m_active_metadata;
                        selected.m_live_epoch = view->m_live_epoch;
                        selected.m_device_epoch = view->m_epoch;
                        selected.m_publication = state->m_identity.m_publication;
                        selected.m_terrain_revision = state->m_identity.m_terrain_revision;
                        selected.m_world.m_world_generation = state->m_identity.m_world;
                        result->m_view = std::move(selected);
                        result->m_physical_ledger = state->m_active_ledger;
                    }
                }

                if (state and state->m_prepared) {
                    const auto staged = state->m_prepared->accounting();
                    result->m_accounting.m_device_bytes += staged.m_device_bytes;
                    result->m_accounting.m_readback_capacity_bytes += staged.m_readback_capacity_bytes;
                }

                return error;
            });
        pending->m_readback = std::move(readback);
        pending->m_generation = ++m_generation;
        pending->m_shutdown = shutdown;
        m_pending = std::move(pending);

        MatterRequest request;
        request.m_instance = m_instance;
        request.m_generation = m_generation;
        return request;
    }

    Expected<std::optional<MatterCompletion>> MatterRuntime::advance(const MatterRequest &request) {
        if (request.m_instance.lock() != m_instance or
            not m_pending or
            request.m_generation != m_pending->m_generation) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        RenderLaneOperationCompletion execution{};
        if (m_lane) {
            if (not m_pending->m_ticket) {
                if (m_pending->m_cancelled) {
                    execution.m_cancel_requested = true;
                } else {
                    auto ticket = m_lane->tryBeginOperation(m_pending->m_operation);
                    if (not ticket and ticket.error() == std::errc::device_or_resource_busy) {
                        return std::optional<MatterCompletion>{};
                    }

                    PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, ticket);
                    m_pending->m_ticket = *ticket;
                }
            }

            if (m_pending->m_ticket) {
                auto completion = m_lane->takeOperation(*m_pending->m_ticket);
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, completion);
                if (not *completion) {
                    return std::optional<MatterCompletion>{};
                }

                execution = **completion;
            }
        } else if (not m_pending->m_cancelled) {
            execution.m_ran = true;
            execution.m_operation_error = (*m_pending->m_operation)();
        } else {
            execution.m_cancel_requested = true;
        }

        // Only the matching take grants access to callback-written storage.
        m_pending->m_operation.reset();
        MatterCompletion result{};
        result.m_execution = execution;
        if (execution.m_ran) {
            m_last_accounting = m_pending->m_accounting;
            m_last_failure = m_pending->m_failure;
            result.m_view = std::move(m_pending->m_view);
            result.m_physical_ledger = std::move(m_pending->m_physical_ledger);
            result.m_stall = std::move(m_pending->m_stall);
            if (m_pending->m_readback) {
                result.m_readback = std::move(*m_pending->m_readback);
            }
        }

        const bool shut_down = m_pending->m_shutdown and execution.m_ran and not execution.m_operation_error;
        m_pending.reset();
        if (shut_down) {
            m_state.reset();
            m_lane = nullptr;
        }

        if (result.m_stall) {
            reportStall_(*result.m_stall);
        }

        return std::optional{std::move(result)};
    }

    std::error_code MatterRuntime::cancel(const MatterRequest &request) {
        if (request.m_instance.lock() != m_instance or
            not m_pending or
            request.m_generation != m_pending->m_generation) {
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        if (m_pending->m_ticket) {
            const auto cancelled = m_lane->cancelOperation(*m_pending->m_ticket);
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, cancelled);
        } else {
            m_pending->m_operation.reset();
            m_pending->m_cancelled = true;
        }

        return {};
    }

    bool MatterRuntime::requestPending() const noexcept {
        return m_pending != nullptr;
    }

    bool MatterRuntime::serviceReady() const noexcept {
        if (not m_pending or not m_lane or m_pending->m_cancelled) {
            return true;
        }

        if (m_pending->m_ticket) {
            return m_lane->operationStatus(*m_pending->m_ticket) != ERenderLaneOperationStatus::pending;
        }

        const auto availability = m_lane->operationAvailability();
        return not availability.m_live or availability.m_available;
    }

    Expected<MatterRequest> MatterRuntime::beginInitialize(
        RenderLane *const lane, rhi::IDevice &device, rhi::ICommandQueue &queue, IShaderService &shaders,
        std::filesystem::path content_root, const ITimerClock *const observation_clock) {
        if (m_state) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::already_connected);
        }

        if (lane and not lane->operationAvailability().m_live) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::not_connected);
        }

        // The application supplies the renderer's actual graphics queue and
        // retains it until Matter shutdown; this owner never reacquires one.
        auto state = std::make_shared<State>();
        state->m_device = &device;
        state->m_queue = &queue;
        state->m_shaders = &shaders;
        state->m_content_root = std::move(content_root);
        state->m_observation_clock = observation_clock;
        m_lane = lane;
        m_state = state;

        return enqueue([]() -> std::error_code { return {}; });
    }

    Expected<MatterRequest> MatterRuntime::beginPrepareResidentWorld(const MatterState &source, Array<u32> terrain) {
        if (not m_state) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::not_connected);
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameMatter, matter::isSupportedIterations(source.m_iterations) and
                                sim::isSupportedWorldEdge(source.m_world_edge) and
                                source.m_profile.m_grid_width == source.m_world_edge + 7u and
                                source.m_profile.m_grid_height == source.m_world_edge + 7u
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));

        const auto state = m_state;
        return enqueue([state, source, terrain = std::move(terrain)]() -> std::error_code {
            if (state->m_prepared or state->m_submitted or state->m_snapshot or state->m_append_prepared) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::device_or_resource_busy);
            }

            auto candidate = std::make_unique<matter::GpuSolver>();
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter, candidate->initialize(
                                *state->m_device, *state->m_queue, *state->m_shaders, state->m_content_root,
                                source.m_profile, state->m_observation_clock));

            auto indices = matterPropertyIndices(source);
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, indices);
            const auto prepared = candidate->prepareReplacement(
                source.m_particles, {}, candidate->accounting().m_epoch,
                {terrain, source.m_world_edge + 1u, source.m_world_edge + 1u, 2.5f, 2.5f},
                matter::ETerrainUpdate::replace, {source.m_properties, *indices});
            if (prepared) {
                // Candidate never submitted; release on this same lane.
                PPR_LOG_WARNING_ON_FAIL(GameMatter, candidate->shutdown());
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, prepared);
            }

            candidate->publishReplacement();

            std::shared_ptr<MatterPhysicalLedger> ledger;
            auto rows = presentationMetadata(source, state->m_observation_clock ? &ledger : nullptr);
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, rows);
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter, state->m_metadata_version != std::numeric_limits<u64>::max()
                                ? std::error_code{}
                                : std::make_error_code(std::errc::value_too_large));
            const u64 metadata_version = ++state->m_metadata_version;
            auto prepared_metadata = prepareGridParticleMetadata(
                *state->m_device, *rows, source.m_profile.m_particle_count, source.m_cell_metres, metadata_version);
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, prepared_metadata);

            if (ledger) {
                ledger->m_metadata_owner = *prepared_metadata;
            }

            state->m_prepared = std::move(candidate);
            state->m_prepared_metadata = std::move(*prepared_metadata);
            state->m_prepared_ledger = std::move(ledger);
            state->m_prepared_identity = source.m_identity;
            state->m_prepared_iterations = source.m_iterations;
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginPublishWorld(const MatterIdentity identity) {
        const auto state = m_state;
        return enqueue([state, identity]() -> std::error_code {
            if (not state or not state->m_prepared or state->m_prepared_identity != identity) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
            }

            if (state->m_submitted) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::device_or_resource_busy);
            }

            if (state->m_active) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_active->shutdown());
            }

            state->m_active = std::move(state->m_prepared);
            state->m_active_metadata = std::move(state->m_prepared_metadata);
            state->m_active_ledger = std::move(state->m_prepared_ledger);
            state->m_identity = identity;
            state->m_iterations = state->m_prepared_iterations;
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginCancelWorld() {
        const auto state = m_state;
        return enqueue([state]() -> std::error_code {
            if (state and state->m_append_prepared) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_active->cancelPreparedReplacement());
                state->m_append_prepared = false;
                state->m_append_ready = false;
            }

            if (state and state->m_prepared) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_prepared->shutdown());
                state->m_prepared.reset();
            }

            if (state) {
                state->m_prepared_metadata.reset();
                state->m_prepared_ledger.reset();
            }
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginResumeWorld(const MatterIdentity identity) {
        const auto state = m_state;
        return enqueue([state, identity]() -> std::error_code {
            if (not state or
                not state->m_active or
                state->m_identity != identity or
                state->m_submitted or
                state->m_prepared) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::operation_not_permitted);
            }
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginSubmitResident(
        const MatterIdentity source, Array<matter::GpuBodyBoundary> bodies, const u32 body_count,
        const matter::GpuRadialForce force) {
        const auto state = m_state;
        return enqueue([state, source, bodies = std::move(bodies), body_count, force]() -> std::error_code {
            if (not state or not state->m_active or state->m_identity != source or state->m_submitted) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
            }

            if (state->m_prepared or state->m_snapshot or state->m_append_prepared) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::device_or_resource_busy);
            }

            // The device counter belongs only to this live solver instance.
            state->m_submitted = true;
            state->m_ready = false;
            state->m_submit_error = state->m_active->submitResidentTick(
                state->m_active->accounting().m_epoch, matter::kGameSubsteps, state->m_iterations, bodies, body_count,
                force);
            // An unsuccessful queue submit may still execute. Keep the
            // attempted batch frozen until explicit completion/discard.
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_submit_error);
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginPoll(const MatterIdentity source) {
        const auto state = m_state;
        const auto result = std::make_shared<std::optional<MatterReadback>>();
        const auto stall = std::make_shared<std::optional<matter::GpuFailure>>();
        return enqueue(
            [state, source, result, stall]() -> std::error_code {
                if (not state or not state->m_active or state->m_identity != source or not state->m_submitted) {
                    PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
                }

                PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_submit_error);

                const auto outcome = state->m_active->pollTick();
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, outcome);
                if (outcome->pending()) {
                    return {};
                }

                if (outcome->rejectedTerrainPosition()) {
                    // The rejected working batch is dropped by the same ordered
                    // cleanup beginDiscard performs (no snapshot is open, so the
                    // tick is not in flight), inlined because a lane operation
                    // cannot enqueue. The committed pair is untouched, the epoch
                    // and publication identity cannot move, and the caller sees
                    // the not-ready shape plus the decoded verdict instead of an
                    // error.
                    PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_active->discardTick());
                    state->m_submitted = false;
                    state->m_ready = false;
                    state->m_submit_error = {};
                    *stall = state->m_active->failure();
                    return {};
                }

                MatterReadback candidate{};
                candidate.m_accounting = state->m_active->accounting();
                candidate.m_source = source;

                *result = std::move(candidate);
                state->m_ready = true;
                return {};
            },
            result, false, false, stall);
    }

    Expected<MatterRequest> MatterRuntime::beginPublish(const MatterIdentity source) {
        const auto state = m_state;
        return enqueue([state, source]() -> std::error_code {
            if (not state or
                not state->m_active or
                state->m_identity != source or
                not state->m_submitted or
                not state->m_ready or
                source.m_publication == std::numeric_limits<u64>::max()) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
            }

            state->m_active->publishTick();
            ++state->m_identity.m_publication;
            state->m_submitted = false;
            state->m_ready = false;
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginDiscard() {
        const auto state = m_state;
        return enqueue(
            [state]() -> std::error_code {
                if (state and state->m_append_prepared) {
                    PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_active->cancelPreparedReplacement());
                    state->m_append_prepared = false;
                    state->m_append_ready = false;
                    state->m_prepared_metadata.reset();
                    state->m_prepared_ledger.reset();
                }

                if (state and state->m_active and state->m_submitted) {
                    PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_active->drain());
                    PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_active->discardTick());
                    state->m_submitted = false;
                    state->m_ready = false;
                    state->m_submit_error = {};
                }
                return {};
            },
            {}, false, true);
    }

    Expected<MatterRequest> MatterRuntime::beginInjectCollectionFailureForTest(const std::error_code cause) {
        const auto state = m_state;
        return enqueue([state, cause]() -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter,
                state and state->m_active ? std::error_code{} : std::make_error_code(std::errc::not_connected));

            state->m_active->injectCollectionFailureForTest(cause);
            return {};
        });
    }

    // ------------------------------------------------------------------
    // exact demand capture and resident inventory transactions
    // ------------------------------------------------------------------

    Expected<MatterRequest> MatterRuntime::beginSnapshot(const MatterIdentity source) {
        const auto state = m_state;
        return enqueue([state, source]() -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter, state and state->m_active and state->m_identity == source
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter, not state->m_submitted and
                                    not state->m_prepared and
                                    not state->m_append_prepared and
                                    not state->m_snapshot
                                ? std::error_code{}
                                : std::make_error_code(std::errc::device_or_resource_busy));

            state->m_snapshot = true;
            state->m_snapshot_copied = false;
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_active->beginSnapshot(state->m_active->accounting().m_epoch));
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginPollSnapshot(const MatterIdentity source) {
        const auto state = m_state;
        const auto result = std::make_shared<std::optional<MatterReadback>>();
        return enqueue(
            [state, source, result]() -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter, state and
                                        state->m_active and
                                        state->m_identity == source and
                                        state->m_snapshot and
                                        not state->m_snapshot_copied
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::operation_not_permitted));

                auto captured = state->m_active->pollSnapshot();
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, captured);
                if (not *captured) {
                    return {};
                }

                MatterReadback snapshot{};
                snapshot.m_particles = std::move((**captured).m_particles);
                snapshot.m_accounting = state->m_active->accounting();
                snapshot.m_source = source;
                *result = std::move(snapshot);
                state->m_snapshot_copied = true;
                return {};
            },
            result);
    }

    Expected<MatterRequest> MatterRuntime::beginEndSnapshot(const MatterIdentity source) {
        const auto state = m_state;
        return enqueue([state, source]() -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter, state and state->m_active and state->m_identity == source and state->m_snapshot
                                ? std::error_code{}
                                : std::make_error_code(std::errc::operation_not_permitted));
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_active->cancelSnapshot());

            state->m_snapshot = false;
            state->m_snapshot_copied = false;
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginPrepareEdit(
        const MatterIdentity source, const MatterState &candidate, const u32 retained_count, Array<u32> terrain,
        const EMatterEditKind kind) {
        const bool terrain_edit = kind == EMatterEditKind::terrain_append;
        const bool replace = kind == EMatterEditKind::particle_replace;
        const bool known_kind = terrain_edit or replace or kind == EMatterEditKind::particle_append;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameMatter, known_kind and
                                (terrain_edit or terrain.empty()) and
                                sim::isSupportedWorldEdge(candidate.m_world_edge) and
                                candidate.m_records.size() == candidate.m_particles.size() and
                                candidate.m_particles.size() + candidate.m_gas.size() <= sim::kMaxMatterParticles and
                                (replace or retained_count <= candidate.m_particles.size()) and
                                candidate.m_profile.m_particle_count == candidate.m_particles.size() and
                                source.m_publication != std::numeric_limits<u64>::max() and
                                (not terrain_edit or source.m_terrain_revision != std::numeric_limits<u64>::max()) and
                                candidate.m_identity ==
                                    MatterIdentity{
                                        source.m_world, source.m_publication + 1u,
                                        source.m_terrain_revision + (terrain_edit ? 1u : 0u)}
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));

        Array<matter::GpuParticle> uploaded(
            candidate.m_particles.begin() + (replace ? 0u : retained_count), candidate.m_particles.end());
        auto all_indices = matterPropertyIndices(candidate);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, all_indices);
        Array<u32> indices(all_indices->begin() + (replace ? 0u : retained_count), all_indices->end());
        Array<GridParticleMetadataRow> metadata;
        std::shared_ptr<MatterPhysicalLedger> ledger;
        auto prepared = presentationMetadata(candidate, m_state->m_observation_clock ? &ledger : nullptr);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, prepared);
        metadata = std::move(*prepared);

        const auto state = m_state;
        const auto target = candidate.m_identity;
        const double cell = candidate.m_cell_metres;
        const u32 terrain_edge = candidate.m_world_edge + 1u;
        const auto profile = candidate.m_profile;
        const u32 iterations = candidate.m_iterations;
        const u32 moving_count = static_cast<u32>(candidate.m_particles.size());
        return enqueue(
            [state, source, target, retained_count, cell, terrain_edge, profile, iterations, moving_count, replace,
                terrain_edit, uploaded = std::move(uploaded), metadata = std::move(metadata),
                ledger = std::move(ledger), terrain = std::move(terrain), properties = candidate.m_properties,
                indices = std::move(indices)]() -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter, state and
                                        state->m_active and
                                        state->m_identity == source and
                                        state->m_iterations == iterations and
                                        state->m_active->profile().m_grid_width == terrain_edge + 6u and
                                        state->m_active->profile().m_grid_height == terrain_edge + 6u and
                                        state->m_active->profile().m_particle_count == retained_count
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::invalid_argument));
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter, not state->m_prepared and
                                        not state->m_submitted and
                                        not state->m_snapshot and
                                        not state->m_append_prepared
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::device_or_resource_busy));

                auto expected_profile = state->m_active->profile();
                expected_profile.m_particle_count = moving_count;
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter, std::bit_cast<std::array<u32, 16u>>(profile) ==
                                        std::bit_cast<std::array<u32, 16u>>(expected_profile)
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::invalid_argument));

                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter, state->m_metadata_version != std::numeric_limits<u64>::max()
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::value_too_large));
                const u64 metadata_version = ++state->m_metadata_version;
                auto resource = prepareGridParticleMetadata(
                    *state->m_device, metadata, moving_count, cell, metadata_version);
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, resource);
                auto prepared_metadata = std::move(*resource);
                if (ledger) {
                    ledger->m_metadata_owner = prepared_metadata;
                }

                state->m_append_prepared = true;
                state->m_append_ready = false;
                state->m_prepared_identity = target;
                state->m_prepared_metadata = std::move(prepared_metadata);
                state->m_prepared_ledger = std::move(ledger);
                const matter::TerrainView view =
                    terrain_edit ? matter::TerrainView{terrain, terrain_edge, terrain_edge, 2.5f, 2.5f}
                                 : matter::TerrainView{};
                const auto policy = terrain_edit ? matter::ETerrainUpdate::replace : matter::ETerrainUpdate::preserve;
                const auto prepared = replace ? state->m_active->prepareReplacement(
                                                    uploaded, {}, state->m_active->accounting().m_epoch, view, policy,
                                                    {properties, indices})
                                              : state->m_active->prepareAppend(
                                                    uploaded, state->m_active->accounting().m_epoch, view, policy,
                                                    {properties, indices});
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, prepared);
                return {};
            });
    }

    Expected<MatterRequest> MatterRuntime::beginPollEdit(const MatterIdentity target) {
        const auto state = m_state;
        const auto result = std::make_shared<std::optional<MatterReadback>>();
        return enqueue(
            [state, target, result]() -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter,
                    state and state->m_active and state->m_append_prepared and state->m_prepared_identity == target
                        ? std::error_code{}
                        : std::make_error_code(std::errc::invalid_argument));

                auto ready = state->m_active->pollReplacement();
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, ready);
                if (not *ready) {
                    return {};
                }

                MatterReadback status{};
                status.m_source = target;
                status.m_accounting = state->m_active->accounting();
                *result = std::move(status);
                state->m_append_ready = true;
                return {};
            },
            result);
    }

    Expected<MatterRequest> MatterRuntime::beginPublishEdit(const MatterIdentity target) {
        const auto state = m_state;
        return enqueue([state, target]() -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                GameMatter, state and
                                    state->m_active and
                                    state->m_append_prepared and
                                    state->m_append_ready and
                                    state->m_prepared_identity == target
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));

            state->m_active->publishReplacement();
            state->m_identity = target;
            state->m_active_metadata = std::move(state->m_prepared_metadata);
            state->m_active_ledger = std::move(state->m_prepared_ledger);
            state->m_append_prepared = false;
            state->m_append_ready = false;
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginShutdown() {
        const auto state = m_state;
        return enqueue(
            [state]() -> std::error_code {
                if (not state) {
                    return {};
                }

                std::error_code first{};
                if (state->m_prepared) {
                    PPR_RETAIN_ERROR_ON_FAIL(GameMatter, first, state->m_prepared->shutdown());
                }

                if (state->m_active) {
                    PPR_RETAIN_ERROR_ON_FAIL(GameMatter, first, state->m_active->shutdown());
                }

                PPR_RETURN_ERROR_ON_FAIL(GameMatter, first);

                state->m_prepared.reset();
                state->m_active.reset();
                state->m_prepared_metadata.reset();
                state->m_prepared_ledger.reset();
                state->m_active_metadata.reset();
                state->m_active_ledger.reset();
                state->m_queue = nullptr;
                return {};
            },
            {}, true);
    }

    Expected<MatterRequest> MatterRuntime::beginDrain() {
        const auto state = m_state;
        return enqueue([state]() -> std::error_code {
            if (state and state->m_active) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_active->drain());
            }
            return {};
        });
    }

    void MatterRuntime::reportStall_(const matter::GpuFailure &verdict) {
        m_last_stall = verdict;
        ++m_stall_count;

        // One entry diagnostic per armed stall, never per frame or poll. The
        // sentence names the row and the frozen epoch; the fields carry all six
        // protocol words so the boundary against flag 8 stays auditable.
        const auto message = std::format(
            "terrain position certificate rejected row {}: final position not certified; "
            "matter frozen at epoch {} (consecutive stall {})",
            verdict.m_particle, m_last_accounting.m_epoch, m_stall_count);
        PPR_LOG_RAW(
            GameMatter, warning, message,
            {
                {"flags", verdict.m_flags},
                {"claimed", verdict.m_claimed},
                {"node", verdict.m_node},
                {"field", verdict.m_field},
                {"particle_row", verdict.m_particle},
                {"body", verdict.m_body},
                {"epoch", m_last_accounting.m_epoch},
                {"stall_count", m_stall_count},
            });
    }

    bool MatterRuntime::stalled() const noexcept {
        return m_last_stall.has_value();
    }

    u32 MatterRuntime::stallCount() const noexcept {
        return m_stall_count;
    }

    void MatterRuntime::clearStall() {
        if (not m_last_stall) {
            return;
        }

        const matter::GpuFailure released = *m_last_stall;
        const u32 stalls = std::exchange(m_stall_count, 0u);
        m_last_stall.reset();

        // One resume diagnostic for the frozen interval just released.
        const auto message = std::format(
            "terrain position certificate stall released after {} consecutive rejection(s); "
            "matter resumes from committed epoch {}",
            stalls, m_last_accounting.m_epoch);
        PPR_LOG_RAW(
            GameMatter, warning, message,
            {
                {"flags", released.m_flags},
                {"claimed", released.m_claimed},
                {"node", released.m_node},
                {"field", released.m_field},
                {"particle_row", released.m_particle},
                {"body", released.m_body},
                {"stalls", stalls},
                {"epoch", m_last_accounting.m_epoch},
            });
    }

    Expected<MatterRequest> MatterRuntime::beginInjectTickVerdictForTest(std::optional<std::array<u32, 6u>> words) {
        const auto state = m_state;
        return enqueue([state, words]() -> std::error_code {
            if (not state or not state->m_active or state->m_submitted) {
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::device_or_resource_busy);
            }

            state->m_active->injectTickVerdictForTest(words);
            return {};
        });
    }

    Expected<matter::GpuAccounting> MatterRuntime::accounting() {
        if (not m_state) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::not_connected);
        }

        return m_last_accounting;
    }

    Expected<matter::GpuFailure> MatterRuntime::failure() {
        if (not m_state) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::not_connected);
        }

        return m_last_failure;
    }
}
