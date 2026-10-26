module;
#include "pP/Macros.h"

module game.colony.matter;

import std;
import engine.math;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(GameMatter, info, none)

    // ------------------------------------------------------------------
    // physical inventory and source units
    // ------------------------------------------------------------------

    Expected<MatterState> makeMatterState(
        const sim::MatterParticleSnapshot &snapshot, const double h, const double thickness, const float gravity_y,
        const u64 world) {
        if (not std::isfinite(h) or
            h <= 0.0 or
            not std::isfinite(thickness) or
            thickness <= 0.0 or
            not std::isfinite(gravity_y) or
            world == 0u) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        MatterState result{};
        result.m_cell_metres = h;
        result.m_thickness_metres = thickness;
        result.m_next_id = snapshot.m_next_id;
        result.m_identity.m_world = world;
        result.m_profile.m_grid_width = kMatterGridEdge;
        result.m_profile.m_grid_height = kMatterGridEdge;
        result.m_profile.m_gravity_strength = static_cast<float>(std::abs(gravity_y) / (kMatterGridEdge * h));
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
            particle.m_position_x = static_cast<float>(record.m_x_m / h + kMatterOrigin);
            particle.m_position_y = static_cast<float>(record.m_y_m / h + kMatterOrigin);
            particle.m_mass = static_cast<float>(record.m_mass_kg / mass_unit);
            particle.m_volume = static_cast<float>(record.m_volume_m3 / volume_unit);
            particle.m_material = static_cast<float>(
                record.m_phase == sim::EGenerationMatter::liquid ? matter::ESourceMaterial::liquid
                                                                 : matter::ESourceMaterial::sand);
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
        return result;
    }

    Expected<sim::MatterParticleSnapshot> captureMovingMatter(
        const MatterState &state, const std::span<const matter::GpuParticle> particles) {
        if (particles.size() != state.m_records.size()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        const double edge = sim::kWorldEdge * state.m_cell_metres;
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
                std::min(static_cast<u32>(record.m_x_m / state.m_cell_metres), sim::kWorldEdge - 1u),
                std::min(static_cast<u32>(record.m_y_m / state.m_cell_metres), sim::kWorldEdge - 1u)};
            chunks[sim::chunkIndexOf(sim::chunkOf(cell))].push_back(record);
            return {};
        };

        for (std::size_t row = 0u; row < particles.size(); ++row) {
            auto record = state.m_records[row];
            record.m_x_m = (static_cast<double>(particles[row].m_position_x) - kMatterOrigin) * state.m_cell_metres;
            record.m_y_m = (static_cast<double>(particles[row].m_position_y) - kMatterOrigin) * state.m_cell_metres;
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
    // carried-state admission
    // ------------------------------------------------------------------

    std::error_code validateMatterProfile(
        const matter::GpuParameters &profile, const double h, const double thickness, const float gravity_y) {
        const double mass_unit = 1000.0 * h * h * thickness;
        if (not std::isfinite(h) or
            h <= 0.0 or
            not std::isfinite(thickness) or
            thickness <= 0.0 or
            not std::isfinite(gravity_y) or
            not std::isfinite(mass_unit) or
            mass_unit <= 0.0 or
            profile.m_particle_count > sim::kMaxMatterParticles) {
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        matter::GpuParameters expected{};
        expected.m_grid_width = kMatterGridEdge;
        expected.m_grid_height = kMatterGridEdge;
        expected.m_particle_count = profile.m_particle_count;
        expected.m_gravity_strength = static_cast<float>(std::abs(gravity_y) / (kMatterGridEdge * h));
        if (not std::isfinite(expected.m_gravity_strength) or
            std::bit_cast<std::array<u32, 16u>>(profile) != std::bit_cast<std::array<u32, 16u>>(expected)) {
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }
        return {};
    }

    std::error_code validateMatterState(
        const MatterState &state, const std::span<const sim::GenerationMaterial> materials, const double h,
        const double thickness, const float gravity_y) {
        if (state.m_cell_metres != h or
            state.m_thickness_metres != thickness or
            state.m_identity.m_world == 0u or
            state.m_identity.m_terrain_revision == 0u or
            state.m_particles.size() != state.m_profile.m_particle_count) {
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
        }

        PPR_RETURN_ERROR_ON_FAIL(GameMatter, validateMatterProfile(state.m_profile, h, thickness, gravity_y));

        auto snapshot = captureMovingMatter(state, state.m_particles);
        PPR_RETURN_ERROR_ON_FAIL(GameMatter, snapshot);
        PPR_RETURN_ERROR_ON_FAIL(
            GameMatter, sim::validateMovingMatterParticles(*snapshot, materials, h, {}, thickness));

        const double volume_unit = h * h * thickness;
        const double mass_unit = 1000.0 * volume_unit;

        for (std::size_t row = 0u; row < state.m_particles.size(); ++row) {
            const auto &particle = state.m_particles[row];
            const auto &record = state.m_records[row];
            for (const float field: std::bit_cast<std::array<float, 22u>>(particle)) {
                if (not std::isfinite(field)) {
                    PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
                }
            }

            const float material = static_cast<float>(
                record.m_phase == sim::EGenerationMatter::liquid ? matter::ESourceMaterial::liquid
                                                                 : matter::ESourceMaterial::sand);
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
                record.m_x_m > sim::kWorldEdge * h or
                record.m_y_m > sim::kWorldEdge * h or
                static_cast<float>(record.m_x_m / h + kMatterOrigin) != particle.m_position_x or
                static_cast<float>(record.m_y_m / h + kMatterOrigin) != particle.m_position_y or
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
                    record.m_x_m > sim::kWorldEdge * source.m_cell_metres or
                    record.m_y_m > sim::kWorldEdge * source.m_cell_metres) {
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
            if (not sim::isInsideChunkGrid(position)) {
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
                    sx >= static_cast<i32>(sim::kWorldEdge) or
                    sy >= static_cast<i32>(sim::kWorldEdge) or
                    solids.test(cell(static_cast<u32>(sx), static_cast<u32>(sy), edited).m_element))
                    mask |= 1u << corner;
            }

            const auto &shape = math::contourCases()[mask];
            const auto points = math::contourPoints();
            for (u32 triangle = 0u; triangle < shape.m_triangle_count; ++triangle) {
                const auto indices = shape.m_triangles[triangle];
                const auto cross = [&](const auto from, const auto to) {
                    return static_cast<double>(to.m_x - from.m_x) * (py - from.m_y) -
                           static_cast<double>(to.m_y - from.m_y) * (px - from.m_x);
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
                (position.m_y + 0.5) * source.m_cell_metres, volume, mass, before.m_temperature, density,
                before.m_element, sim::EGenerationMatter::solid};
            matter::GpuParticle particle{};
            particle.m_position_x = static_cast<float>(position.m_x) + 0.5f + kMatterOrigin;
            particle.m_position_y = static_cast<float>(position.m_y) + 0.5f + kMatterOrigin;
            particle.m_mass = static_cast<float>(mass / (1000.0 * volume));
            particle.m_material = static_cast<float>(matter::ESourceMaterial::sand);
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

        const auto solid = [&](const i32 x, const i32 y) {
            if (x < 0 or y < 0 or x >= static_cast<i32>(sim::kWorldEdge) or y >= static_cast<i32>(sim::kWorldEdge)) {
                return true;
            }

            const auto ux = static_cast<u32>(x), uy = static_cast<u32>(y);
            const auto cells = grid.residentCells({ux / sim::kChunkEdge, uy / sim::kChunkEdge});
            return cells and
                   solid_ids.test((*cells)[(uy % sim::kChunkEdge) * sim::kChunkEdge + ux % sim::kChunkEdge].m_element);
        };

        Array<u32> masks(static_cast<std::size_t>(kMatterTerrainEdge) * kMatterTerrainEdge, 0u);
        for (u32 y = 0u; y < kMatterTerrainEdge; ++y) {
            for (u32 x = 0u; x < kMatterTerrainEdge; ++x) {
                const i32 sx = static_cast<i32>(x) - 1, sy = static_cast<i32>(y) - 1;
                masks[static_cast<std::size_t>(y) * kMatterTerrainEdge + x] = (solid(sx, sy) ? 1u : 0u) |
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
                    (cx + std::cos(da) * rx - std::sin(da) * ry) / h + kMatterOrigin);
                row.m_shape.m_position_y = static_cast<float>(
                    (cy + std::sin(da) * rx + std::cos(da) * ry) / h + kMatterOrigin);
                row.m_shape.m_half_size_x = static_cast<float>(body.m_half_width / h);
                row.m_shape.m_half_size_y = static_cast<float>(body.m_half_height / h);
                row.m_shape.m_radius = row.m_shape.m_half_size_x;
                row.m_shape.m_rotation_degrees = -angle * (180.0f / std::numbers::pi_v<float>);
                row.m_shape.m_shape = body.m_geometry == physics::EGeometry::circle ? matter::EStaticShape::circle
                                                                                    : matter::EStaticShape::box;
                row.m_com_x = static_cast<float>(cx / h + kMatterOrigin);
                row.m_com_y = static_cast<float>(cy / h + kMatterOrigin);
                row.m_velocity_x = static_cast<float>(body.m_state.m_velocity_x / h);
                row.m_velocity_y = static_cast<float>(body.m_state.m_velocity_y / h);
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
        Expected<Array<GridParticleMetadataRow>> presentationMetadata(const MatterState &source) {
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
                                        record.m_x_m <= sim::kWorldEdge * source.m_cell_metres and
                                        record.m_y_m <= sim::kWorldEdge * source.m_cell_metres
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::invalid_argument));

                GridParticleMetadataRow row{};
                row.m_id_low = static_cast<u32>(record.m_id);
                row.m_id_high = static_cast<u32>(record.m_id >> 32u);
                row.m_source_row = source_row;
                row.m_material = record.m_material;
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
        std::filesystem::path m_content_root{};
        rhi::IDevice *m_device{};
        rhi::ComPtr<rhi::ICommandQueue> m_queue{};
        IShaderService *m_shaders{};
        const ITimerClock *m_observation_clock{};
        MatterIdentity m_identity{};
        MatterIdentity m_prepared_identity{};
        std::error_code m_submit_error{};
        bool m_submitted{};
        bool m_ready{};
        bool m_append_prepared{}, m_append_ready{};
        bool m_snapshot{}, m_snapshot_copied{};
    };

    struct MatterRuntime::Pending {
        std::unique_ptr<RenderLaneOperation> m_operation{};
        std::shared_ptr<std::optional<MatterReadback>> m_readback{};
        std::optional<GridParticleGpuView> m_view{};
        std::optional<RenderLaneOperationTicket> m_ticket{};
        matter::GpuAccounting m_accounting{};
        matter::GpuFailure m_failure{};
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
        const bool discard) {
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
            [state, result, discard, operation = std::move(operation)]() mutable -> std::error_code {
                const auto error = operation();
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
        RenderLane *const lane, rhi::IDevice &device, IShaderService &shaders, std::filesystem::path content_root,
        const ITimerClock *const observation_clock) {
        if (m_state) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::already_connected);
        }

        if (lane and not lane->operationAvailability().m_live) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::not_connected);
        }

        auto state = std::make_shared<State>();
        state->m_device = &device;
        state->m_shaders = &shaders;
        state->m_content_root = std::move(content_root);
        state->m_observation_clock = observation_clock;
        m_lane = lane;

        const auto owner = state;
        m_state = state;

        return enqueue([owner]() -> std::error_code {
            const auto error = make_error_code(
                owner->m_device->getQueue(rhi::QueueType::Graphics, owner->m_queue.writeRef()));
            if (error) {
                // Release any partially returned queue on its acquisition lane.
                owner->m_queue.setNull();
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, error);
            }
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginPrepareResidentWorld(const MatterState &source, Array<u32> terrain) {
        if (not m_state) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, std::errc::not_connected);
        }

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

            const auto prepared = candidate->prepareReplacement(
                source.m_particles, {}, candidate->accounting().m_epoch,
                {terrain, kMatterTerrainEdge, kMatterTerrainEdge, 2.5f, 2.5f});
            if (prepared) {
                // Candidate never submitted; release on this same lane.
                PPR_LOG_WARNING_ON_FAIL(GameMatter, candidate->shutdown());
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, prepared);
            }

            candidate->publishReplacement();

            auto rows = presentationMetadata(source);
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, rows);
            auto prepared_metadata = prepareGridParticleMetadata(
                *state->m_device, *rows, source.m_profile.m_particle_count, source.m_cell_metres,
                source.m_identity.m_terrain_revision);
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, prepared_metadata);

            state->m_prepared = std::move(candidate);
            state->m_prepared_metadata = std::move(*prepared_metadata);
            state->m_prepared_identity = source.m_identity;
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
            state->m_identity = identity;
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
        const MatterIdentity source, Array<matter::GpuBodyBoundary> bodies, const u32 body_count) {
        const auto state = m_state;
        return enqueue([state, source, bodies = std::move(bodies), body_count]() -> std::error_code {
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
                state->m_active->accounting().m_epoch, matter::kGameSubsteps, matter::kSourceIterations, bodies,
                body_count);
            // An unsuccessful queue submit may still execute. Keep the
            // attempted batch frozen until explicit completion/discard.
            PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_submit_error);
            return {};
        });
    }

    Expected<MatterRequest> MatterRuntime::beginPoll(const MatterIdentity source) {
        const auto state = m_state;
        const auto result = std::make_shared<std::optional<MatterReadback>>();
        return enqueue(
            [state, source, result]() -> std::error_code {
                if (not state or not state->m_active or state->m_identity != source or not state->m_submitted) {
                    PPR_RETURN_ERROR_ON_FAIL(GameMatter, std::errc::invalid_argument);
                }

                PPR_RETURN_ERROR_ON_FAIL(GameMatter, state->m_submit_error);

                const auto ready = state->m_active->pollTick();
                PPR_RETURN_ERROR_ON_FAIL(GameMatter, ready);
                if (not *ready) {
                    return {};
                }

                MatterReadback candidate{};
                candidate.m_accounting = state->m_active->accounting();
                candidate.m_source = source;

                *result = std::move(candidate);
                state->m_ready = true;
                return {};
            },
            result);
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
        const MatterIdentity source, const MatterState &candidate, const u32 retained_count, Array<u32> terrain) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameMatter,
            candidate.m_records.size() == candidate.m_particles.size() and
                    candidate.m_particles.size() <= sim::kMaxMatterParticles and
                    retained_count <= candidate.m_particles.size() and
                    source.m_publication != std::numeric_limits<u64>::max() and
                    source.m_terrain_revision != std::numeric_limits<u64>::max() and
                    candidate.m_identity ==
                        MatterIdentity{source.m_world, source.m_publication + 1u, source.m_terrain_revision + 1u}
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));

        Array<matter::GpuParticle> appended(
            candidate.m_particles.begin() + retained_count, candidate.m_particles.end());
        Array<GridParticleMetadataRow> metadata;
        if (not appended.empty()) {
            auto prepared = presentationMetadata(candidate);
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameMatter, prepared);
            metadata = std::move(*prepared);
        }

        const auto state = m_state;
        const auto target = candidate.m_identity;
        const double cell = candidate.m_cell_metres;
        return enqueue(
            [state, source, target, retained_count, cell, appended = std::move(appended),
                metadata = std::move(metadata), terrain = std::move(terrain)]() -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter, state and
                                        state->m_active and
                                        state->m_identity == source and
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

                auto prepared_metadata = state->m_active_metadata;
                if (not metadata.empty()) {
                    auto resource = prepareGridParticleMetadata(
                        *state->m_device, metadata, retained_count + static_cast<u32>(appended.size()), cell,
                        target.m_terrain_revision);
                    PPR_RETURN_ERROR_ON_FAIL(GameMatter, resource);
                    prepared_metadata = std::move(*resource);
                }

                state->m_append_prepared = true;
                state->m_append_ready = false;
                state->m_prepared_identity = target;
                state->m_prepared_metadata = std::move(prepared_metadata);
                PPR_RETURN_ERROR_ON_FAIL(
                    GameMatter, state->m_active->prepareAppend(
                                    appended, state->m_active->accounting().m_epoch,
                                    {terrain, kMatterTerrainEdge, kMatterTerrainEdge, 2.5f, 2.5f}));
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
                state->m_active_metadata.reset();
                state->m_queue.setNull();
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
