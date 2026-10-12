module;
#include "pP/Macros.h"
module engine.sim;
import :particles;
import engine.math;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(MatterParticles, info, none);
    namespace {
        [[nodiscard]] std::error_code canceled(const IContext *const context) noexcept {
            return context ? context->error() : std::error_code{};
        }
        [[nodiscard]] const GenerationMaterial *material(
            const std::span<const GenerationMaterial> materials, const u16 id) noexcept {
            const auto found = std::ranges::find(materials, id, &GenerationMaterial::m_id);
            return found == materials.end() ? nullptr : std::addressof(*found);
        }
        [[nodiscard]] bool positive(const double value) noexcept {
            return std::isfinite(value) and value > 0.0;
        }
        template<typename Read>
        [[nodiscard]] bool covered(const MatterParticle &particle, const double cell_size, Read &&read) noexcept {
            const double x = particle.m_x_m / cell_size - 0.5;
            const double y = particle.m_y_m / cell_size - 0.5;
            const i32 ix = static_cast<i32>(std::floor(x)), iy = static_cast<i32>(std::floor(y));
            const double px = x - ix, py = y - iy;
            constexpr std::array<std::pair<i32, i32>, 4u> offsets{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
            u32 mask{};
            for (u32 corner = 0u; corner < 4u; ++corner) {
                if (read(ix + offsets[corner].first, iy + offsets[corner].second)) {
                    mask |= 1u << corner;
                }
            }
            const auto &shape = math::contourCases()[mask];
            const auto points = math::contourPoints();
            for (u32 triangle = 0u; triangle < shape.m_triangle_count; ++triangle) {
                const auto indices = shape.m_triangles[triangle];
                const auto a = points[indices.m_a], b = points[indices.m_b], c = points[indices.m_c];
                const auto cross = [&](const auto from, const auto to) {
                    return (to.m_x - from.m_x) * (py - from.m_y) - (to.m_y - from.m_y) * (px - from.m_x);
                };
                const double ab = cross(a, b), bc = cross(b, c), ca = cross(c, a);
                if ((ab >= 0.0 and bc >= 0.0 and ca >= 0.0) or (ab <= 0.0 and bc <= 0.0 and ca <= 0.0)) {
                    return true;
                }
            }
            return false;
        }
    }
    std::span<const MatterParticle> MatterParticleBatch::records() const noexcept {
        return m_particles ? std::span<const MatterParticle>{m_particles->data(), m_particles->size()}
                           : std::span<const MatterParticle>{};
    }
    Expected<MatterParticleSnapshot> seedMatterParticles(
        const InitialMatter &matter, const ChunkGrid &grid, const GenerationRecipe &recipe,
        const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(MatterParticles, canceled(context));
        u64 count{};
        std::array<u32, kChunkCount> counts{};
        for (const auto &region: matter.m_regions) {
            for (const auto span: region.m_spans) {
                if (span.m_y >= kWorldEdge or span.m_begin_x >= span.m_end_x or span.m_end_x > kWorldEdge) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(MatterParticles, std::errc::invalid_argument);
                }
                for (u32 x = span.m_begin_x; x < span.m_end_x;) {
                    const u32 end = std::min(span.m_end_x, (x / kChunkEdge + 1u) * kChunkEdge);
                    counts[chunkIndexOf(chunkOf({x, span.m_y}))] += end - x;
                    x = end;
                }
                count += span.m_end_x - span.m_begin_x;
                if (count > kMaxMatterParticles) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(MatterParticles, std::errc::value_too_large);
                }
            }
        }
        std::array<Array<MatterParticle>, kChunkCount> chunks{};
        for (u32 i = 0u; i < kChunkCount; ++i) {
            chunks[i].reserve(counts[i]);
        }
        MatterParticleSnapshot result;
        for (const auto &region: matter.m_regions) {
            const auto *const definition = material(recipe.m_materials, region.m_material);
            if (definition == nullptr or
                definition->m_terrain or
                not positive(region.m_bulk_density_kg_m3) or
                (not std::isfinite(region.m_temperature_k) or region.m_temperature_k < 0.0)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(MatterParticles, std::errc::invalid_argument);
            }
            double volume{};
            MatterParticle *last = nullptr;
            for (const auto span: region.m_spans) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(MatterParticles, canceled(context));
                for (u32 x = span.m_begin_x; x < span.m_end_x; ++x) {
                    auto area = generationEmptyArea(grid, {x, span.m_y}, recipe.m_cell_size_m);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(MatterParticles, area);
                    const double particle_volume = *area * recipe.m_reference_thickness_m;
                    if (not positive(particle_volume)) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(MatterParticles, std::errc::invalid_argument);
                    }
                    auto &chunk = chunks[chunkIndexOf(chunkOf({x, span.m_y}))];
                    chunk.push_back(
                        {result.m_next_id++, (x + 0.5) * recipe.m_cell_size_m, (span.m_y + 0.5) * recipe.m_cell_size_m,
                            particle_volume, particle_volume * region.m_bulk_density_kg_m3, region.m_temperature_k,
                            region.m_bulk_density_kg_m3, region.m_material, definition->m_matter});
                    last = &chunk.back();
                    volume += particle_volume;
                }
            }
            if (last != nullptr) {
                // Region totals were accumulated in generation order. Accept only
                // floating-point accumulation error, never redistribute bad metadata.
                const double tolerance = 1.0e-8 * std::max(1.0, volume);
                if (not positive(region.m_volume_m3) or
                    not positive(region.m_mass_kg) or
                    std::abs(region.m_volume_m3 - volume) > tolerance or
                    std::abs(region.m_mass_kg - region.m_volume_m3 * region.m_bulk_density_kg_m3) >
                        1.0e-8 * std::max(1.0, region.m_mass_kg)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(MatterParticles, std::errc::invalid_argument);
                }
                last->m_volume_m3 += region.m_volume_m3 - volume;
                last->m_mass_kg = last->m_volume_m3 * last->m_bulk_density_kg_m3;
            }
        }
        for (u32 index = 0u; index < kChunkCount; ++index) {
            if (not chunks[index].empty()) {
                result.m_chunks.push_back(
                    {{index % kChunksPerEdge, index / kChunksPerEdge},
                        std::make_shared<const Array<MatterParticle>>(std::move(chunks[index]))});
            }
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            MatterParticles,
            validateMatterParticles(
                result, recipe.m_materials, recipe.m_cell_size_m, context, recipe.m_reference_thickness_m));
        return result;
    }
    std::error_code validateMatterParticles(
        const MatterParticleSnapshot &snapshot, const std::span<const GenerationMaterial> materials,
        const double cell_size, const IContext *const context, const double reference_thickness) {
        PPR_RETURN_ERROR_ON_FAIL(MatterParticles, canceled(context));
        if (not positive(cell_size) or
            not positive(reference_thickness) or
            not positive(cell_size * cell_size * reference_thickness) or
            snapshot.m_next_id == 0u or
            snapshot.m_chunks.size() > kChunkCount) {
            PPR_RETURN_ERROR_ON_FAIL(MatterParticles, std::errc::invalid_argument);
        }
        u64 total{};
        std::bitset<kChunkCount> seen_chunks{};
        Array<u64> ids;
        Array<u32> cells;
        for (const auto &batch: snapshot.m_chunks) {
            PPR_RETURN_ERROR_ON_FAIL(MatterParticles, canceled(context));
            if (batch.m_position.m_x >= kChunksPerEdge or
                batch.m_position.m_y >= kChunksPerEdge or
                not batch.m_particles) {
                PPR_RETURN_ERROR_ON_FAIL(MatterParticles, std::errc::invalid_argument);
            }
            const u32 index = chunkIndexOf(batch.m_position);
            total += batch.records().size();
            if (seen_chunks.test(index) or total > kMaxMatterParticles) {
                PPR_RETURN_ERROR_ON_FAIL(MatterParticles, std::errc::invalid_argument);
            }
            seen_chunks.set(index);
            for (const auto &particle: batch.records()) {
                const auto *const definition = material(materials, particle.m_material);
                if (particle.m_id == 0u or
                    particle.m_id >= snapshot.m_next_id or
                    definition == nullptr or
                    definition->m_terrain or
                    definition->m_matter != particle.m_phase or
                    not std::isfinite(particle.m_x_m) or
                    not std::isfinite(particle.m_y_m) or
                    particle.m_x_m < 0.0 or
                    particle.m_y_m < 0.0 or
                    particle.m_x_m >= kWorldEdge * cell_size or
                    particle.m_y_m >= kWorldEdge * cell_size or
                    not positive(particle.m_volume_m3) or
                    particle.m_volume_m3 > cell_size * cell_size * reference_thickness * 1.001 or
                    not positive(particle.m_mass_kg) or
                    not std::isfinite(particle.m_temperature_k) or
                    particle.m_temperature_k < 0.0 or
                    not positive(particle.m_bulk_density_kg_m3) or
                    std::ranges::any_of(particle.m_reserved, [](const u8 value) { return value != 0u; })) {
                    PPR_RETURN_ERROR_ON_FAIL(MatterParticles, std::errc::invalid_argument);
                }
                const GlobalCellPos cell{
                    static_cast<u32>(particle.m_x_m / cell_size), static_cast<u32>(particle.m_y_m / cell_size)};
                if (chunkOf(cell) != batch.m_position or
                    std::abs(particle.m_mass_kg - particle.m_volume_m3 * particle.m_bulk_density_kg_m3) >
                        1.0e-9 * (std::max) (1.0, particle.m_mass_kg)) {
                    PPR_RETURN_ERROR_ON_FAIL(MatterParticles, std::errc::invalid_argument);
                }
                ids.push_back(particle.m_id);
                cells.push_back(cell.m_y * kWorldEdge + cell.m_x);
            }
        }
        std::ranges::sort(ids);
        std::ranges::sort(cells);
        if (std::ranges::adjacent_find(ids) != ids.end() or std::ranges::adjacent_find(cells) != cells.end()) {
            PPR_RETURN_ERROR_ON_FAIL(MatterParticles, std::errc::invalid_argument);
        }
        return {};
    }
    std::error_code MatterParticleState::restore(
        const MatterParticleSnapshot &snapshot, const std::span<const GenerationMaterial> materials,
        const double cell_size, const IContext *const context, const double reference_thickness) {
        PPR_RETURN_ERROR_ON_FAIL(
            MatterParticles, validateMatterParticles(snapshot, materials, cell_size, context, reference_thickness));
        std::array<std::shared_ptr<const Array<MatterParticle>>, kChunkCount> chunks{};
        u32 count{};
        for (const auto &batch: snapshot.m_chunks) {
            chunks[chunkIndexOf(batch.m_position)] = batch.m_particles;
            count += static_cast<u32>(batch.records().size());
        }
        PPR_RETURN_ERROR_ON_FAIL(MatterParticles, canceled(context));
        m_chunks.swap(chunks);
        m_count = count;
        m_next_id = snapshot.m_next_id;
        return {};
    }
    MatterParticleSnapshot MatterParticleState::capture() const {
        MatterParticleSnapshot result{.m_next_id = m_next_id};
        for (u32 index = 0u; index < kChunkCount; ++index) {
            if (m_chunks[index] and not m_chunks[index]->empty()) {
                result.m_chunks.push_back({{index % kChunksPerEdge, index / kChunksPerEdge}, m_chunks[index]});
            }
        }
        return result;
    }
    std::span<const MatterParticle> MatterParticleState::chunk(const ChunkPos position) const noexcept {
        const auto &batch = m_chunks[chunkIndexOf(position)];
        return batch ? std::span<const MatterParticle>{batch->data(), batch->size()}
                     : std::span<const MatterParticle>{};
    }
    u32 MatterParticleState::count() const noexcept {
        return m_count;
    }
    Expected<Array<MatterParticleBatch>> MatterParticleState::prepareTerrainEdit(
        const ChunkGrid &original, const std::span<const ChunkGrid::CapturedChunk> candidate,
        const double cell_size) const {
        if (not positive(cell_size)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(MatterParticles, std::errc::invalid_argument);
        }
        std::array<const ChunkGrid::CapturedChunk *, kChunkCount> overlays{};
        std::bitset<kChunkCount> affected{};
        for (const auto &chunk: candidate) {
            const auto position = chunk.position();
            overlays[chunkIndexOf(position)] = &chunk;
            const auto before = original.residentCells(position);
            const auto after = chunk.residentCells();
            bool added_solid = false;
            if (after) {
                for (u32 cell = 0u; cell < kCellsPerChunk; ++cell) {
                    if ((*after)[cell].m_element != 0u and (not before or (*before)[cell].m_element == 0u)) {
                        added_solid = true;
                        break;
                    }
                }
            }
            if (not added_solid) {
                continue;
            }
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const i32 x = static_cast<i32>(position.m_x) + dx, y = static_cast<i32>(position.m_y) + dy;
                    if (x >= 0 and y >= 0 and x < kChunksPerEdge and y < kChunksPerEdge) {
                        affected.set(static_cast<u32>(y) * kChunksPerEdge + static_cast<u32>(x));
                    }
                }
            }
        }
        const auto read = [&](const i32 x, const i32 y, const bool edited) {
            if (x < 0 or y < 0 or x >= kWorldEdge or y >= kWorldEdge) {
                return true;
            }
            const GlobalCellPos cell{static_cast<u32>(x), static_cast<u32>(y)};
            const auto position = chunkOf(cell);
            const auto *const overlay = overlays[chunkIndexOf(position)];
            const auto cells = edited and overlay ? overlay->residentCells() : original.residentCells(position);
            return cells and (*cells)[(cell.m_y % kChunkEdge) * kChunkEdge + cell.m_x % kChunkEdge].m_element != 0u;
        };
        Array<MatterParticleBatch> result;
        for (u32 index = 0u; index < kChunkCount; ++index) {
            if (not affected.test(index) or not m_chunks[index]) {
                continue;
            }
            Array<MatterParticle> retained;
            bool removed = false;
            for (const auto &particle: *m_chunks[index]) {
                const bool before = covered(
                    particle, cell_size, [&](const i32 x, const i32 y) { return read(x, y, false); });
                const bool after = covered(
                    particle, cell_size, [&](const i32 x, const i32 y) { return read(x, y, true); });
                if (after and not before) {
                    removed = true;
                } else {
                    retained.push_back(particle);
                }
            }
            if (removed) {
                result.push_back(
                    {{index % kChunksPerEdge, index / kChunksPerEdge},
                        std::make_shared<const Array<MatterParticle>>(std::move(retained))});
            }
        }
        return result;
    }
    void MatterParticleState::commitTerrainEdit(const std::span<const MatterParticleBatch> batches) noexcept {
        for (const auto &batch: batches) {
            auto &owner = m_chunks[chunkIndexOf(batch.m_position)];
            m_count -= owner ? static_cast<u32>(owner->size()) : 0u;
            owner = batch.m_particles;
            m_count += owner ? static_cast<u32>(owner->size()) : 0u;
        }
    }
}
