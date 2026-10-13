module;
#include "pP/Macros.h"
module engine.sim;
import :worldgen.raster;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldRaster, info, none);
    namespace {
        [[nodiscard]] std::error_code canceled(const IContext *const context) noexcept {
            return context != nullptr ? context->error() : std::error_code{};
        }
        [[nodiscard]] bool depthContains(const double depth, const double low, const double high) noexcept {
            return depth >= low and (depth < high or high == 1.0);
        }
        [[nodiscard]] bool reserved(
            const GenerationRecipe &recipe, const GenerationInstance &instance, const u32 x, const u32 y) noexcept {
            const auto &bounds = recipe.m_features[instance.m_feature].m_bounds;
            const i64 local_x = static_cast<i64>(x) - instance.m_origin_x;
            const i64 local_y = static_cast<i64>(y) - instance.m_origin_y;
            return local_x >= bounds.m_min_x and
                   local_x < bounds.m_max_x and
                   local_y >= bounds.m_min_y and
                   local_y < bounds.m_max_y;
        }
        [[nodiscard]] bool capsule(
            const double x, const double y, const double x0, const double y0, const double x1, const double y1,
            const double radius) noexcept {
            const double dx = x1 - x0, dy = y1 - y0;
            const double length_squared = dx * dx + dy * dy;
            const double t = length_squared == 0.0
                                 ? 0.0
                                 : std::clamp(((x - x0) * dx + (y - y0) * dy) / length_squared, 0.0, 1.0);
            return std::hypot(x - x0 - t * dx, y - y0 - t * dy) <= radius;
        }
        [[nodiscard]] bool operationContains(
            const GenerationRecipe &recipe, const GenerationInstance &instance, const GenerationOperation &operation,
            const u32 x, const u32 y) noexcept {
            const auto &bounds = recipe.m_features[instance.m_feature].m_bounds;
            double local_x = static_cast<double>(x) + 0.5 - instance.m_origin_x;
            const double local_y = static_cast<double>(y) + 0.5 - instance.m_origin_y;
            if (instance.m_reflected) {
                local_x = static_cast<double>(bounds.m_min_x) + bounds.m_max_x - local_x;
            }
            if (operation.m_shape == EGenerationShape::capsule) {
                return capsule(
                    local_x, local_y, operation.m_x, operation.m_y, operation.m_end_x, operation.m_end_y,
                    operation.m_radius_m * (1.0 / recipe.m_cell_size_m));
            }
            if (local_x < operation.m_x or
                local_y < operation.m_y or
                local_x >= operation.m_x + static_cast<double>(operation.m_width) or
                local_y >= operation.m_y + static_cast<double>(operation.m_height)) {
                return false;
            }
            if (operation.m_shape == EGenerationShape::rectangle) {
                return true;
            }
            const double nx = 2.0 * (local_x - operation.m_x) / operation.m_width - 1.0;
            const double ny = 2.0 * (local_y - operation.m_y) / operation.m_height - 1.0;
            return nx * nx + ny * ny <= 1.0;
        }
        [[nodiscard]] bool traversalRoom(const GenerationFeature &feature) noexcept {
            const bool sealed = std::ranges::any_of(feature.m_markers, [](const auto &marker) {
                return marker.m_role == EGenerationMarker::sealed_pocket;
            });
            return not sealed and std::ranges::any_of(feature.m_markers, [](const auto &marker) {
                return marker.m_role == EGenerationMarker::traversal_room;
            });
        }
        [[nodiscard]] double fieldValue(
            const GenerationPlan &plan, const u32 field, const double x, const double y) noexcept {
            return std::clamp(static_cast<double>(plan.fields()[field].sample(x, y)), -1.0, 1.0);
        }
        [[nodiscard]] double cellDraw(
            const u64 seed, const u64 key, const u32 x, const u32 y, const u64 domain) noexcept {
            GenerationRandom random{generationStreamSeed(seed, key ^ domain, static_cast<u64>(y) * kWorldEdge + x)};
            return random.unit();
        }
        [[nodiscard]] u16 baseMaterial(
            const GenerationPlan &plan, const GenerationBiome &biome, const double depth, const double x,
            const double y, const u32 gx, const u32 gy, const bool sprinkle, const bool allow_vacuum) noexcept {
            u16 material = 0u;
            for (const auto &band: biome.m_bands) {
                if (not depthContains(depth, band.m_depth_min, band.m_depth_max)) {
                    continue;
                }
                double draw = 0.5 * (fieldValue(plan, band.m_field, x, y) + 1.0);
                if (band.m_bed_period_m > 0.0) {
                    // A periodic package of ordered beds, bent by the authored field;
                    // long-wave width modulation avoids mechanically repeated layers.
                    const double thickness = band.m_bed_period_m *
                                             (1.0 + 0.2 * fieldValue(plan, band.m_field, x * 0.37, 17.0));
                    GenerationRandom phase{generationStreamSeed(plan.seed(), band.m_key, 0x6265647068617365ull)};
                    const double coordinate = (generationSurfaceHeight(plan, x) -
                                                  y +
                                                  x * band.m_bed_dip +
                                                  band.m_bed_warp_m * fieldValue(plan, band.m_field, x, y)) /
                                                  thickness +
                                              phase.unit();
                    draw = coordinate - std::floor(coordinate);
                }
                bool salted = false;
                // Material salt samples only solids. Pore clusters have their own
                // coherent field and globally anchored seed stream.
                if (sprinkle and
                    biome.m_sprinkle_probability > 0.0 and
                    cellDraw(plan.seed(), biome.m_key, gx, gy, 0x73616c742e676174ull) < biome.m_sprinkle_probability) {
                    draw = cellDraw(plan.seed(), band.m_key, gx, gy, 0x73616c742e636466ull);
                    salted = true;
                }
                double total = band.m_total_weight;
                if (not allow_vacuum or salted) {
                    // Authored reservations retain solid substrate between stamp
                    // operations; only their explicit carve operations open it.
                    total = 0.0;
                    for (const auto &choice: band.m_choices) {
                        if (choice.m_material != 0u) {
                            total += choice.m_weight;
                        }
                    }
                }
                double cumulative = 0.0;
                for (const auto &choice: band.m_choices) {
                    if ((not allow_vacuum or salted) and choice.m_material == 0u) {
                        continue;
                    }
                    // Retain the final eligible choice when the field reaches one.
                    material = choice.m_material;
                    cumulative += choice.m_weight;
                    if (draw < cumulative / total) {
                        break;
                    }
                }
                break;
            }
            // Validated bands cover every depth; zero is an explicit vacuum choice.
            return material;
        }
        [[nodiscard]] double blobRadius(
            const GenerationPlan &plan, const GenerationDeposit &deposit, const GenerationGeometry::Blob &blob,
            const double x, const double y) noexcept {
            const double dx = x - blob.m_x_m, dy = y - blob.m_y_m;
            const double cosine = std::cos(blob.m_angle), sine = std::sin(blob.m_angle);
            const double local_x = (dx * cosine + dy * sine) / (blob.m_radius_m * blob.m_stretch);
            const double local_y = (-dx * sine + dy * cosine) / blob.m_radius_m;
            const double edge = 1.0 + deposit.m_distortion *
                                          fieldValue(plan, deposit.m_field, dx + blob.m_offset_x, dy + blob.m_offset_y);
            return std::hypot(local_x, local_y) / edge;
        }
        [[nodiscard]] u64 materialKey(const GenerationRecipe &recipe, const u16 id) noexcept {
            if (id == 0u) {
                return 0x76616375756dull;
            }
            const auto material = std::ranges::lower_bound(recipe.m_materials, id, {}, &GenerationMaterial::m_id);
            if (material != recipe.m_materials.end() and material->m_key != 0u) {
                return material->m_key;
            }
            for (const auto &biome: recipe.m_biomes) {
                for (const auto &band: biome.m_bands) {
                    for (const auto &choice: band.m_choices) {
                        if (choice.m_material == id) {
                            return choice.m_key;
                        }
                    }
                }
                for (const auto &deposit: biome.m_deposits) {
                    if (deposit.m_material == id) {
                        return deposit.m_key;
                    }
                }
            }
            return 0u;
        }
        [[nodiscard]] u16 neighborhoodMode(
            const GenerationPlan &plan, const std::array<u16, 8u> &neighbors, const u16 current, const u16 affinity,
            const u32 x, const u32 y, const u32 pass, const bool erosion) noexcept {
            std::array<u16, 8u> materials{};
            std::array<u8, 8u> votes{};
            u32 distinct = 0u;
            for (const u16 neighbor: neighbors) {
                if (not erosion and neighbor == 0u) {
                    continue;
                }
                u32 index = 0u;
                while (index < distinct and materials[index] != neighbor) {
                    ++index;
                }
                if (index == distinct) {
                    materials[distinct++] = neighbor;
                }
                ++votes[index];
            }
            u16 mode = current;
            double best = -1.0;
            u64 best_tie{};
            for (u32 i = 0u; i < distinct; ++i) {
                const u16 candidate = materials[i];
                double score = votes[i];
                if (not erosion) {
                    // Preserve the tweaked geological filter before erosion.
                    score += (candidate == affinity ? 1.25 : 0.0) + (candidate == current ? 0.75 : 0.0);
                    if (candidate == current and current != 0u) {
                        const auto host = std::ranges::lower_bound(
                            plan.recipe().m_materials, current, {}, &GenerationMaterial::m_id);
                        if (host != plan.recipe().m_materials.end()) {
                            score += std::clamp(host->m_reference_density_kg_m3 / 5000.0, 0.0, 1.0);
                        }
                    }
                }
                const u64 tie = generationStreamSeed(
                    plan.seed(), materialKey(plan.recipe(), candidate) ^ 0x746965726f636bull,
                    (static_cast<u64>(y) * kWorldEdge + x) * 24u + pass);
                if (score > best or (score == best and tie > best_tie)) {
                    mode = candidate;
                    best = score;
                    best_tie = tie;
                }
            }
            return mode;
        }
        [[nodiscard]] bool poreAffinity(
            const GenerationPlan &plan, const GenerationBiome &biome, const double x, const double y) noexcept {
            return fieldValue(plan, biome.m_pore_field, x, y) >= biome.m_pore_affinity_min;
        }
        [[nodiscard]] bool poreHost(const GenerationBiome &biome, const double depth, const u16 material) noexcept {
            for (const auto &band: biome.m_bands) {
                if (depthContains(depth, band.m_depth_min, band.m_depth_max)) {
                    return std::ranges::any_of(band.m_choices, [material](const auto &v) {
                        return v.m_material != 0u and v.m_material == material;
                    });
                }
            }
            return false;
        }
        [[nodiscard]] bool poreSeed(
            const GenerationPlan &plan, const GenerationBiome &biome, const u32 x, const u32 y, const u16 material,
            const double depth) noexcept {
            if (biome.m_pore_probability == 0.0 or not poreHost(biome, depth, material)) {
                return false;
            }
            const double scale = plan.recipe().m_cell_size_m;
            if (not poreAffinity(plan, biome, (x + 0.5) * scale, (y + 0.5) * scale)) {
                return false;
            }
            const i32 tile_x = static_cast<i32>(x / 16u), tile_y = static_cast<i32>(y / 16u);
            const i32 tile_radius = static_cast<i32>(std::ceil(biome.m_pore_radius_max_m / (16.0 * scale)));
            for (i32 dy = -tile_radius; dy <= tile_radius; ++dy) {
                for (i32 dx = -tile_radius; dx <= tile_radius; ++dx) {
                    const i32 tx = tile_x + dx, ty = tile_y + dy;
                    if (tx < 0 or ty < 0 or tx >= 256 or ty >= 256) {
                        continue;
                    }
                    GenerationRandom random{generationStreamSeed(
                        plan.seed(), biome.m_key ^ 0x706f726573656564ull,
                        static_cast<u64>(ty) * 256u + static_cast<u32>(tx))};
                    if (random.unit() >= biome.m_pore_probability) {
                        continue;
                    }
                    const double ax = (tx * 16.0 + random.unit() * 16.0) * scale,
                                 ay = (ty * 16.0 + random.unit() * 16.0) * scale;
                    const double radius = biome.m_pore_radius_min_m +
                                          (biome.m_pore_radius_max_m - biome.m_pore_radius_min_m) * random.unit();
                    if (poreAffinity(plan, biome, ax, ay) and
                        std::hypot((x + 0.5) * scale - ax, (y + 0.5) * scale - ay) <= radius) {
                        return true;
                    }
                }
            }
            return false;
        }
        [[nodiscard]] u16 deposits(
            const GenerationPlan &plan, const GenerationGeometry &geometry, const std::span<const u32> blobs,
            const u32 biome_index, const double depth, const double xm, const double ym, const u32 x, const u32 y,
            const EGenerationDepositStage stage, u16 material, const bool satellites = false) noexcept {
            const auto &biome = plan.recipe().m_biomes[biome_index];
            for (u32 d = 0u; d < biome.m_deposits.size(); ++d) {
                const auto &deposit = biome.m_deposits[d];
                if (material != deposit.m_host or
                    not depthContains(depth, deposit.m_depth_min, deposit.m_depth_max) or
                    (not satellites and deposit.m_stage != stage) or
                    (satellites and deposit.m_sprinkle_probability == 0.0)) {
                    continue;
                }
                if (deposit.m_shape == EGenerationDepositShape::field) {
                    if (satellites) {
                        continue;
                    }
                    const double value = fieldValue(plan, deposit.m_field, xm, ym);
                    if (value >= deposit.m_threshold_min and value <= deposit.m_threshold_max) {
                        material = deposit.m_material;
                    }
                    continue;
                }
                for (const u32 index: blobs) {
                    const auto &blob = geometry.m_blobs[index];
                    if (blob.m_biome != biome_index or blob.m_deposit != d) {
                        continue;
                    }
                    const double q = blobRadius(plan, deposit, blob, xm, ym);
                    double probability = 0.0;
                    if (satellites) {
                        if (q > 1.0 and q <= 1.2) {
                            probability = deposit.m_sprinkle_probability * (1.2 - q) / 0.2;
                        }
                    } else if (q <= 1.0) {
                        const double fringe = std::clamp((q - 0.6) / 0.4, 0.0, 1.0);
                        probability = deposit.m_core_coverage +
                                      (deposit.m_fringe_coverage - deposit.m_core_coverage) * fringe;
                    }
                    if (probability > 0.0 and
                        cellDraw(
                            plan.seed(), deposit.m_key, x, y,
                            generationStreamSeed(0u, 0x626c6f622e677261ull, blob.m_ordinal)) < probability) {
                        material = deposit.m_material;
                        break;
                    }
                }
            }
            return material;
        }
        [[nodiscard]] Cell sample(
            const GenerationPlan &plan, const GenerationGeometry &geometry, const std::span<const u32> segments,
            const std::span<const u32> instances, const std::span<const u32> blobs, const u32 x, const u32 y,
            const u32 biome_index, const bool lining, const bool sprinkle = false) noexcept {
            const auto &recipe = plan.recipe();
            const double xm = (x + 0.5) * recipe.m_cell_size_m, ym = (y + 0.5) * recipe.m_cell_size_m;
            const double depth = generationDepth(plan, xm, ym);
            const auto &biome = recipe.m_biomes[biome_index];
            const float temperature = static_cast<float>(
                biome.m_temperature_base_k +
                biome.m_temperature_variation_k * fieldValue(plan, biome.m_temperature_field, xm, ym));
            if (recipe.m_open_surface) {
                if (ym >= generationSurfaceHeight(plan, xm)) {
                    return {};
                }
                if (ym <= generationBedrockHeight(plan, xm)) {
                    return {recipe.m_bedrock_material, temperature};
                }
            }
            if (x < recipe.m_boundary_fine or
                y < recipe.m_boundary_fine or
                x >= kWorldEdge - recipe.m_boundary_fine or
                (not recipe.m_open_surface and y >= kWorldEdge - recipe.m_boundary_fine)) {
                return {recipe.m_boundary_material, temperature};
            }
            const GenerationInstance *reservation = nullptr;
            for (const u32 i: instances) {
                if (reserved(recipe, geometry.m_instances[i], x, y)) {
                    reservation = &geometry.m_instances[i];
                    break;
                }
            }
            u16 material = lining and reservation == nullptr
                               ? biome.m_lining_material
                               : baseMaterial(
                                     plan, biome, depth, xm, ym, x, y,
                                     sprinkle and reservation == nullptr and not lining, reservation == nullptr);
            // Field deposits retain their historical behavior inside reservations;
            // finite natural bodies stop at authored reservations and barriers.
            if (not lining or reservation != nullptr) {
                material = deposits(
                    plan, geometry, reservation == nullptr ? blobs : std::span<const u32>{}, biome_index, depth, xm, ym,
                    x, y, EGenerationDepositStage::bulk, material);
            }
            if (sprinkle and reservation == nullptr and not lining and poreSeed(plan, biome, x, y, material, depth)) {
                material = 0u;
            }
            if (recipe.m_open_surface and reservation == nullptr and not lining) {
                const double cover = generationSurfaceHeight(plan, xm) - ym;
                const double variation = 1.0 + 0.25 * fieldValue(plan, biome.m_temperature_field, xm, 0.0);
                if (cover < biome.m_soil_depth_m * variation) {
                    material = biome.m_soil_material;
                } else if (cover < (biome.m_soil_depth_m + biome.m_weathered_depth_m) * variation) {
                    material = biome.m_weathered_material;
                }
            }
            bool portal = false;
            for (u32 r = 0u; r < recipe.m_rules.size(); ++r) {
                const auto &rule = recipe.m_rules[r];
                if (const auto *const cave = std::get_if<GenerationCave>(&rule.m_parameters)) {
                    if (lining or
                        reservation != nullptr or
                        cave->m_biome != biome_index or
                        not depthContains(depth, cave->m_depth_min, cave->m_depth_max)) {
                        continue;
                    }
                    const double value = fieldValue(plan, cave->m_field, xm, ym);
                    if (value >= cave->m_threshold_min and value <= cave->m_threshold_max) {
                        material = 0u;
                    }
                    continue;
                }
                const auto *const worm = std::get_if<GenerationWorm>(&rule.m_parameters);
                const bool connection = std::holds_alternative<GenerationConnection>(rule.m_parameters);
                if (worm == nullptr and not connection) {
                    continue;
                }
                if (worm != nullptr and (reservation != nullptr or
                                            worm->m_biome != biome_index or
                                            not depthContains(depth, worm->m_depth_min, worm->m_depth_max) or
                                            (not worm->m_carve and material != worm->m_host))) {
                    continue;
                }
                if (connection and
                    reservation != nullptr and
                    (reservation->m_rule == kGenerationStartRule or
                        not traversalRoom(recipe.m_features[reservation->m_feature]))) {
                    continue;
                }
                for (const u32 s: segments) {
                    const auto &segment = geometry.m_segments[s];
                    if (segment.m_rule != r or not capsule(
                                                   xm, ym, segment.m_x0_m, segment.m_y0_m, segment.m_x1_m,
                                                   segment.m_y1_m, segment.m_radius_m)) {
                        continue;
                    }
                    material = worm != nullptr and not worm->m_carve ? worm->m_material : 0u;
                    portal = portal or connection;
                    break;
                }
            }
            if (reservation != nullptr) {
                for (const auto &operation: recipe.m_features[reservation->m_feature].m_operations) {
                    if (not operationContains(recipe, *reservation, operation, x, y)) {
                        continue;
                    }
                    // A traversal-room marker authorizes its planned connection
                    // portals. Sealed pockets and the start reservation never do.
                    if (portal and not operation.m_carve) {
                        continue;
                    }
                    material = operation.m_carve ? 0u : operation.m_material;
                }
            }
            return {material, material == 0u ? 0.0f : temperature};
        }
        [[nodiscard]] std::error_code indexBounds(
            Array<Array<u32>> &bins, u32 &references, const u32 index, const double min_x, const double min_y,
            const double max_x, const double max_y) {
            if (max_x < 0.0 or max_y < 0.0 or min_x >= kWorldEdge or min_y >= kWorldEdge) {
                return {};
            }
            const u32 left = static_cast<u32>(
                                 std::clamp(std::floor(min_x), 0.0, static_cast<double>(kWorldEdge - 1u))) /
                             kChunkEdge;
            const u32 bottom = static_cast<u32>(
                                   std::clamp(std::floor(min_y), 0.0, static_cast<double>(kWorldEdge - 1u))) /
                               kChunkEdge;
            const u32 right = static_cast<u32>(
                                  std::clamp(std::floor(max_x), 0.0, static_cast<double>(kWorldEdge - 1u))) /
                              kChunkEdge;
            const u32 top = static_cast<u32>(std::clamp(std::floor(max_y), 0.0, static_cast<double>(kWorldEdge - 1u))) /
                            kChunkEdge;
            const u32 count = (right - left + 1u) * (top - bottom + 1u);
            if (count > kMaxGenerationRasterReferences - references) {
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, make_error_code(EGenerationErrc::raster_reference_limit));
            }
            references += count;
            for (u32 y = bottom; y <= top; ++y) {
                for (u32 x = left; x <= right; ++x) {
                    bins[y * kChunksPerEdge + x].push_back(index);
                }
            }
            return {};
        }
    }
    GenerationRaster::GenerationRaster(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry,
        Array<Array<u32>> segments, Array<Array<u32>> instances, Array<Array<u32>> blobs) noexcept
        : m_plan(plan), m_layout(layout), m_geometry(geometry), m_segments(std::move(segments)),
          m_instances(std::move(instances)), m_blobs(std::move(blobs)) {
    }
    u64 GenerationRaster::indexPayloadBytes() const noexcept {
        u64 bytes = (m_segments.capacity() + m_instances.capacity() + m_blobs.capacity()) * sizeof(Array<u32>);
        for (const auto &bucket: m_segments) {
            bytes += bucket.capacity() * sizeof(u32);
        }
        for (const auto &bucket: m_instances) {
            bytes += bucket.capacity() * sizeof(u32);
        }
        for (const auto &bucket: m_blobs) {
            bytes += bucket.capacity() * sizeof(u32);
        }
        return bytes;
    }
    bool GenerationRaster::protectedCell(const u32 x, const u32 y) const noexcept {
        const auto &r = m_plan.recipe();
        if (x >= kWorldEdge or y >= kWorldEdge) {
            return true;
        }
        const double xm = (x + 0.5) * r.m_cell_size_m, ym = (y + 0.5) * r.m_cell_size_m;
        if (x < r.m_boundary_fine or
            y < r.m_boundary_fine or
            x >= kWorldEdge - r.m_boundary_fine or
            (not r.m_open_surface and y >= kWorldEdge - r.m_boundary_fine) or
            (r.m_open_surface and
                (ym <= generationBedrockHeight(m_plan, xm) or ym >= generationSurfaceHeight(m_plan, xm)))) {
            return true;
        }
        const u32 biome = generationBiomeAt(m_plan, m_layout, xm, ym);
        const i32 radius = static_cast<i32>(std::max(1u, r.m_biomes[biome].m_lining_thickness_fine));
        for (i32 dy = -radius; dy <= radius; ++dy) {
            for (i32 dx = -radius; dx <= radius; ++dx) {
                if (std::abs(dx) + std::abs(dy) > radius and (std::abs(dx) > 1 or std::abs(dy) > 1)) {
                    continue;
                }
                const double nx = (static_cast<double>(x) + dx + 0.5) * r.m_cell_size_m,
                             ny = (static_cast<double>(y) + dy + 0.5) * r.m_cell_size_m;
                if (generationBiomeAt(m_plan, m_layout, nx, ny) != biome) {
                    return true;
                }
            }
        }
        const u32 bucket = (y / kChunkEdge) * kChunksPerEdge + x / kChunkEdge;
        for (const u32 i: m_instances[bucket]) {
            if (reserved(r, m_geometry.m_instances[i], x, y)) {
                return true;
            }
        }
        for (const u32 i: m_segments[bucket]) {
            const auto &segment = m_geometry.m_segments[i];
            if (std::holds_alternative<GenerationConnection>(r.m_rules[segment.m_rule].m_parameters) and
                capsule(xm, ym, segment.m_x0_m, segment.m_y0_m, segment.m_x1_m, segment.m_y1_m, segment.m_radius_m)) {
                return true;
            }
        }
        return false;
    }
    u64 GenerationRaster::chunkWorkingPayloadBytes() const noexcept {
        u32 lining = 0u, pore = 0u, competition = 0u, finish = 0u, erosion = 0u;
        for (const auto &biome: m_plan.recipe().m_biomes) {
            lining = std::max(lining, biome.m_lining_thickness_fine);
            pore = std::max(pore, biome.m_pore_passes);
            competition = std::max(competition, biome.m_relaxation_passes);
            finish = std::max(finish, biome.m_finish_passes);
            erosion = std::max(erosion, biome.m_erosion_passes);
        }
        const u32 passes = pore + competition + finish + erosion;
        const u64 edge = kChunkEdge + 2u * (lining + passes + 1u);
        return kCellsPerChunk * sizeof(Cell) + edge *
                                                   edge *
                                                   (sizeof(u32) +
                                                       sizeof(u8) +
                                                       (passes == 0u ? 0u : sizeof(u16)) +
                                                       (pore == 0u ? 0u : sizeof(u8)) +
                                                       sizeof(Cell) * (passes == 0u ? 1u : 2u));
    }
    Expected<Array<Cell>> GenerationRaster::chunk(const ChunkPos position, const IContext *const context) const {
        if (position.m_x >= kChunksPerEdge or position.m_y >= kChunksPerEdge) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::result_out_of_range);
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));

        const auto scratch_scope = mem::ScratchPad::open();

        const auto &recipe = m_plan.recipe();
        u32 lining_radius = 0u, passes = 0u, pore_passes = 0u, competition_passes = 0u, finish_passes = 0u,
            erosion_passes = 0u;
        for (const auto &biome: recipe.m_biomes) {
            lining_radius = std::max(lining_radius, biome.m_lining_thickness_fine);
            pore_passes = std::max(pore_passes, biome.m_pore_passes);
            competition_passes = std::max(competition_passes, biome.m_relaxation_passes);
            finish_passes = std::max(finish_passes, biome.m_finish_passes);
            erosion_passes = std::max(erosion_passes, biome.m_erosion_passes);
            passes = std::max(
                passes,
                biome.m_relaxation_passes + biome.m_pore_passes + biome.m_finish_passes + biome.m_erosion_passes);
        }
        passes = pore_passes + competition_passes + finish_passes + erosion_passes;
        const u32 guard = lining_radius + 1u, padding = guard + passes;
        const u32 edge = kChunkEdge + 2u * padding;
        const auto global = [&](const u32 value, const u32 chunk) -> u32 {
            return static_cast<u32>(std::clamp(
                static_cast<i64>(chunk * kChunkEdge + value) - padding, i64{0}, static_cast<i64>(kWorldEdge - 1u)));
        };
        Array<u32, mem::ScratchPad> biomes(edge * edge);
        for (u32 y = 0u; y < edge; ++y) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            for (u32 x = 0u; x < edge; ++x) {
                biomes[y * edge + x] = generationBiomeAt(
                    m_plan, m_layout, (global(x, position.m_x) + 0.5) * recipe.m_cell_size_m,
                    (global(y, position.m_y) + 0.5) * recipe.m_cell_size_m);
            }
        }
        const auto liningAt = [&](const u32 at, const GenerationBiome &biome) noexcept {
            const i32 radius = static_cast<i32>(biome.m_lining_thickness_fine);
            for (i32 dy = -radius; dy <= radius; ++dy) {
                const i32 horizontal = radius - std::abs(dy);
                for (i32 dx = -horizontal; dx <= horizontal; ++dx) {
                    const auto neighbor = static_cast<std::size_t>(
                        static_cast<i64>(at) + dy * static_cast<i64>(edge) + dx);
                    if (biomes[neighbor] != biomes[at]) {
                        return true;
                    }
                }
            }
            return false;
        };
        u32 local_passes = 0u;
        bool detail = false, sprinkle = false;
        std::array<bool, kMaxGenerationBiomes> present{};
        for (const u32 index: biomes) {
            present[index] = true;
        }
        for (u32 index = 0u; index < recipe.m_biomes.size(); ++index) {
            if (not present[index]) {
                continue;
            }
            const auto &biome = recipe.m_biomes[index];
            local_passes = std::max(
                local_passes,
                biome.m_relaxation_passes + biome.m_pore_passes + biome.m_finish_passes + biome.m_erosion_passes);
            sprinkle = sprinkle or biome.m_sprinkle_probability > 0.0 or biome.m_pore_probability > 0.0;
            detail = detail or std::ranges::any_of(biome.m_deposits, [](const auto &deposit) {
                return deposit.m_stage == EGenerationDepositStage::detail or deposit.m_sprinkle_probability > 0.0;
            });
        }
        if (local_passes == 0u and not detail and not sprinkle) {
            // Unchanged provinces need only their original chunk payload. They
            // never allocate relaxation buffers or scan protection geometry.
            Array<Cell> cells(kCellsPerChunk);
            const u32 bucket = position.m_y * kChunksPerEdge + position.m_x;
            for (u32 y = 0u; y < kChunkEdge; ++y) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
                for (u32 x = 0u; x < kChunkEdge; ++x) {
                    const u32 at = (y + padding) * edge + x + padding;
                    cells[y * kChunkEdge + x] = sample(
                        m_plan, m_geometry, m_segments[bucket], m_instances[bucket], m_blobs[bucket],
                        position.m_x * kChunkEdge + x, position.m_y * kChunkEdge + y, biomes[at],
                        liningAt(at, recipe.m_biomes[biomes[at]]));
                }
            }
            return cells;
        }

        Array<Cell, mem::ScratchPad> current(edge * edge);
        Array<u16, mem::ScratchPad> affinity_materials(passes == 0u ? 0u : edge * edge);
        Array<u8, mem::ScratchPad> pore_eligible(pore_passes == 0u ? 0u : edge * edge);
        Array<u8, mem::ScratchPad> protected_cells(edge * edge, 0u);
        for (u32 y = guard; y < edge - guard; ++y) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            for (u32 x = guard; x < edge - guard; ++x) {
                const u32 at = y * edge + x, biome_index = biomes[at];
                const auto &biome = recipe.m_biomes[biome_index];
                const u32 gx = global(x, position.m_x), gy = global(y, position.m_y);
                const u32 bucket = (gy / kChunkEdge) * kChunksPerEdge + gx / kChunkEdge;
                const bool lining = liningAt(at, biome);
                bool protect = lining or
                               gx < recipe.m_boundary_fine or
                               gy < recipe.m_boundary_fine or
                               gx >= kWorldEdge - recipe.m_boundary_fine or
                               (not recipe.m_open_surface and gy >= kWorldEdge - recipe.m_boundary_fine) or
                               (recipe.m_open_surface and
                                   ((gy + 0.5) * recipe.m_cell_size_m >=
                                           generationSurfaceHeight(m_plan, (gx + 0.5) * recipe.m_cell_size_m) or
                                       (gy + 0.5) * recipe.m_cell_size_m <=
                                           generationBedrockHeight(m_plan, (gx + 0.5) * recipe.m_cell_size_m)));
                // Both sides of a biome boundary remain fixed, including biomes
                // without authored lining. A mode vote never transports material across it.
                for (i32 dy = -1; dy <= 1; ++dy) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const auto neighbor = static_cast<std::size_t>(
                            static_cast<i64>(at) + dy * static_cast<i64>(edge) + dx);
                        protect = protect or biomes[neighbor] != biome_index;
                    }
                }
                for (const u32 index: m_instances[bucket]) {
                    protect = protect or reserved(recipe, m_geometry.m_instances[index], gx, gy);
                }
                for (const u32 index: m_segments[bucket]) {
                    const auto &segment = m_geometry.m_segments[index];
                    if (std::holds_alternative<GenerationConnection>(recipe.m_rules[segment.m_rule].m_parameters)) {
                        protect = protect or capsule(
                                                 (gx + 0.5) * recipe.m_cell_size_m, (gy + 0.5) * recipe.m_cell_size_m,
                                                 segment.m_x0_m, segment.m_y0_m, segment.m_x1_m, segment.m_y1_m,
                                                 segment.m_radius_m);
                    }
                }
                protected_cells[at] = protect ? 1u : 0u;
                current[at] = sample(
                    m_plan, m_geometry, m_segments[bucket], m_instances[bucket], m_blobs[bucket], gx, gy, biome_index,
                    lining, not protect);
                if (passes != 0u and not protect) {
                    const double xm = (gx + 0.5) * recipe.m_cell_size_m, ym = (gy + 0.5) * recipe.m_cell_size_m;
                    const double depth = generationDepth(m_plan, xm, ym);
                    affinity_materials[at] = baseMaterial(m_plan, biome, depth, xm, ym, gx, gy, false, false);
                    if (pore_passes != 0u) {
                        pore_eligible[at] = (current[at].m_element == 0u or
                                                poreHost(biome, depth, current[at].m_element)) and
                                                    poreAffinity(m_plan, biome, xm, ym)
                                                ? u8{1u}
                                                : u8{0u};
                    }
                }
            }
        }

        Array<Cell, mem::ScratchPad> next(passes == 0u ? 0u : edge * edge);
        // Each pass consumes only the immutable previous buffer. Shrinking its
        // valid halo by one cell proves identical interior results in every chunk.
        for (u32 pass = 0u; pass < passes; ++pass) {
            if (pass == pore_passes + competition_passes) {
                // Detail is initialized over the entire remaining valid halo.
                // Finishing therefore sees identical geology across chunk seams.
                const u32 detail_margin = guard + pass;
                for (u32 y = detail_margin; y < edge - detail_margin; ++y) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
                    for (u32 x = detail_margin; x < edge - detail_margin; ++x) {
                        const u32 at = y * edge + x;
                        if (protected_cells[at] != 0u or current[at].m_element == 0u) {
                            continue;
                        }
                        const u32 gx = global(x, position.m_x), gy = global(y, position.m_y);
                        const u32 bucket = (gy / kChunkEdge) * kChunksPerEdge + gx / kChunkEdge;
                        const double xm = (gx + 0.5) * recipe.m_cell_size_m, ym = (gy + 0.5) * recipe.m_cell_size_m;
                        const double depth = generationDepth(m_plan, xm, ym);
                        current[at].m_element = deposits(
                            m_plan, m_geometry, m_blobs[bucket], biomes[at], depth, xm, ym, gx, gy,
                            EGenerationDepositStage::detail, current[at].m_element);
                        current[at].m_element = deposits(
                            m_plan, m_geometry, m_blobs[bucket], biomes[at], depth, xm, ym, gx, gy,
                            EGenerationDepositStage::detail, current[at].m_element, true);
                    }
                }
            }
            next = current;
            const u32 margin = guard + pass + 1u;
            for (u32 y = margin; y < edge - margin; ++y) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
                for (u32 x = margin; x < edge - margin; ++x) {
                    const u32 at = y * edge + x;
                    const auto &biome = recipe.m_biomes[biomes[at]];
                    const bool pore_phase = pass < pore_passes;
                    const bool erosion_phase = pass >= pore_passes + competition_passes + finish_passes;
                    const bool finish_phase = not erosion_phase and pass >= pore_passes + competition_passes;
                    const u32 phase_pass = pore_phase
                                               ? pass
                                               : (erosion_phase
                                                         ? pass - pore_passes - competition_passes - finish_passes
                                                         : (finish_phase ? pass - pore_passes - competition_passes
                                                                         : pass - pore_passes));
                    const u32 phase_count = pore_phase ? biome.m_pore_passes
                                                       : (erosion_phase ? biome.m_erosion_passes
                                                                        : (finish_phase ? biome.m_finish_passes
                                                                                        : biome.m_relaxation_passes));
                    if (protected_cells[at] != 0u or phase_pass >= phase_count) {
                        continue;
                    }
                    const u32 gx = global(x, position.m_x), gy = global(y, position.m_y);
                    if (cellDraw(m_plan.seed(), biome.m_key, gx, gy, 0x72656c6178000000ull + pass) >=
                        (pore_phase ? 1.0
                                    : (erosion_phase ? 0.5
                                                     : (finish_phase ? biome.m_finish_adoption
                                                                     : biome.m_relaxation_adoption)))) {
                        continue;
                    }
                    std::array<u16, 8u> neighbors{};
                    u32 count = 0u;
                    for (i32 dy = -1; dy <= 1; ++dy) {
                        for (i32 dx = -1; dx <= 1; ++dx) {
                            if (dx != 0 or dy != 0) {
                                neighbors[count++] =
                                    current[static_cast<std::size_t>(
                                                static_cast<i64>(at) + dy * static_cast<i64>(edge) + dx)]
                                        .m_element;
                            }
                        }
                    }
                    const double xm = (gx + 0.5) * recipe.m_cell_size_m, ym = (gy + 0.5) * recipe.m_cell_size_m;
                    const u16 affinity = affinity_materials[at];
                    const u16 neighbor_mode = neighborhoodMode(
                        m_plan, neighbors, current[at].m_element, affinity, gx, gy, pass, erosion_phase);
                    u16 mode = current[at].m_element;
                    if (pore_phase) {
                        const u32 empty = static_cast<u32>(std::ranges::count(neighbors, u16{0u}));
                        if (mode == 0u) {
                            mode = empty >= 2u ? u16{0u} : (neighbor_mode == 0u ? affinity : neighbor_mode);
                        } else if (empty >= 3u and pore_eligible[at] != 0u) {
                            mode = 0u;
                        }
                    } else if (erosion_phase or mode != 0u) {
                        // Only the appended prototype erosion can excavate or
                        // refill; the earlier geological stages remain intact.
                        mode = neighbor_mode;
                    }
                    if (mode == current[at].m_element) {
                        continue;
                    }
                    const float temperature = mode == 0u
                                                  ? 0.0f
                                                  : static_cast<float>(
                                                        biome.m_temperature_base_k +
                                                        biome.m_temperature_variation_k *
                                                            fieldValue(m_plan, biome.m_temperature_field, xm, ym));
                    next[at] = {mode, temperature};
                }
            }
            current.swap(next);
        }

        Array<Cell> cells(kCellsPerChunk);
        for (u32 y = 0u; y < kChunkEdge; ++y) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            for (u32 x = 0u; x < kChunkEdge; ++x) {
                const u32 at = (y + padding) * edge + x + padding;
                Cell cell = current[at];
                if (finish_passes == 0u and
                    erosion_passes == 0u and
                    protected_cells[at] == 0u and
                    cell.m_element != 0u) {
                    const u32 gx = position.m_x * kChunkEdge + x, gy = position.m_y * kChunkEdge + y;
                    const u32 bucket = position.m_y * kChunksPerEdge + position.m_x;
                    const double xm = (gx + 0.5) * recipe.m_cell_size_m, ym = (gy + 0.5) * recipe.m_cell_size_m;
                    const double depth = generationDepth(m_plan, xm, ym);
                    cell.m_element = deposits(
                        m_plan, m_geometry, m_blobs[bucket], biomes[at], depth, xm, ym, gx, gy,
                        EGenerationDepositStage::detail, cell.m_element);
                    cell.m_element = deposits(
                        m_plan, m_geometry, m_blobs[bucket], biomes[at], depth, xm, ym, gx, gy,
                        EGenerationDepositStage::detail, cell.m_element, true);
                }
                cells[y * kChunkEdge + x] = cell;
            }
        }
        return cells;
    }
    namespace {
        [[nodiscard]] std::error_code rasterChunks(
            const GenerationRaster &raster, ChunkGrid &grid, const GenerationExecution &execution,
            u64 *const known_scratch_peak) {
            std::atomic<u32> active{}, peak{};
            WaitGroup wg{};
            ParallelScheduler *const pool = execution.m_pool;
            const bool parallel = pool != nullptr and not pool->isWorkerThread();

            PPR_DEFER {
                std::ignore = wg.wait();

                if (known_scratch_peak != nullptr) {
                    *known_scratch_peak = std::max(
                        *known_scratch_peak,
                        raster.indexPayloadBytes() +
                            static_cast<u64>(peak.load(std::memory_order_relaxed)) * raster.chunkWorkingPayloadBytes());
                }
            };

            const auto fill = [&](const u32 c) -> std::error_code {
                // One requested chunk payload per live task. Include the short
                // handoff to grid ownership; this is a conservative task bound.
                if (known_scratch_peak != nullptr) {
                    const u32 count = active.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    auto previous = peak.load(std::memory_order_relaxed);
                    while (count > previous and
                           not peak.compare_exchange_weak(previous, count, std::memory_order_relaxed)) {
                    }
                }
                PPR_DEFER {
                    if (known_scratch_peak != nullptr) {
                        active.fetch_sub(1u, std::memory_order_relaxed);
                    }
                };
                const ChunkPos position{c % kChunksPerEdge, c / kChunksPerEdge};
                auto cells = raster.chunk(position, execution.m_context);
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, cells);
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, grid.setCells(position, std::move(*cells)));
                if (execution.m_completed_chunks != nullptr) {
                    execution.m_completed_chunks->fetch_add(1u, std::memory_order_relaxed);
                }
                return {};
            };

            for (u32 chunk_index = 0u; chunk_index < kChunkCount; ++chunk_index) {
                std::error_code err{};
                if (not parallel) {
                    err = fill(chunk_index);
                } else {
                    auto task = pool->schedule(
                        [&, chunk_index](const IContext &context) -> std::error_code {
                            PPR_RETURN_ERROR_ON_FAIL(WorldRaster, context.error());
                            return fill(chunk_index);
                        },
                        wg, ParallelScheduler::wait_if_full);
                    if (not task) {
                        err = task.error();
                    }
                }
                if (err) [[unlikely]] {
                    return err;
                }
            }

            return wg.wait();
        }
    }
    std::error_code validateGenerationBackbone(
        const GenerationPlan &plan, const GenerationGeometry &geometry, const ChunkGrid &grid,
        const IContext *const context) {
        const auto &r = plan.recipe();
        const auto invalid = make_error_code(EGenerationErrc::constraints_exhausted);
        std::map<std::pair<double, double>, u32> nodes;
        Array<u32> parents;
        const auto node = [&](const double x, const double y) {
            const auto [entry, inserted] = nodes.emplace(std::pair{x, y}, static_cast<u32>(parents.size()));
            if (inserted) {
                parents.push_back(entry->second);
            }
            return entry->second;
        };
        const auto root = [&](u32 id) {
            while (parents[id] != id) {
                parents[id] = parents[parents[id]];
                id = parents[id];
            }
            return id;
        };
        const auto empty = [&](const u32 x, const u32 y) {
            const auto cells = grid.residentCells({x / kChunkEdge, y / kChunkEdge});
            return not cells or (*cells)[(y % kChunkEdge) * kChunkEdge + x % kChunkEdge].m_element == 0u;
        };
        for (const auto &segment: geometry.m_segments) {
            if (not std::holds_alternative<GenerationConnection>(r.m_rules[segment.m_rule].m_parameters)) {
                continue;
            }
            const auto &connection = std::get<GenerationConnection>(r.m_rules[segment.m_rule].m_parameters);
            if (connection.m_natural_anchors == 0u and
                connection.m_wells == 0u and
                connection.m_branches == 0u and
                connection.m_roughness_m == 0.0) {
                continue;
            }
            const double hx = r.m_actor_half_width_m, hy = r.m_actor_half_height_m;
            const u32 a = node(segment.m_x0_m, segment.m_y0_m), b = node(segment.m_x1_m, segment.m_y1_m);
            parents[root(b)] = root(a);
            const u32 steps = std::max(
                1u, static_cast<u32>(std::ceil(
                        std::hypot(segment.m_x1_m - segment.m_x0_m, segment.m_y1_m - segment.m_y0_m) /
                        (r.m_cell_size_m * 0.5))));
            for (u32 step = 0u; step <= steps; ++step) {
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, canceled(context));
                const double t = static_cast<double>(step) / steps;
                const double x = segment.m_x0_m + t * (segment.m_x1_m - segment.m_x0_m),
                             y = segment.m_y0_m + t * (segment.m_y1_m - segment.m_y0_m);
                const i32 left = static_cast<i32>(std::floor((x - hx) / r.m_cell_size_m)),
                          right = static_cast<i32>(std::floor((x + hx) / r.m_cell_size_m));
                const i32 bottom = static_cast<i32>(std::floor((y - hy) / r.m_cell_size_m)),
                          top = static_cast<i32>(std::floor((y + hy) / r.m_cell_size_m));
                if (left < 0 or
                    bottom < 0 or
                    right >= static_cast<i32>(kWorldEdge) or
                    top >= static_cast<i32>(kWorldEdge)) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldRaster, invalid);
                }
                for (i32 gy = bottom; gy <= top; ++gy) {
                    for (i32 gx = left; gx <= right; ++gx) {
                        if (not empty(static_cast<u32>(gx), static_cast<u32>(gy))) {
                            PPR_RETURN_ERROR_ON_FAIL(WorldRaster, invalid);
                        }
                    }
                }
            }
        }

        if (not parents.empty()) {
            const u32 connected = root(0u);
            for (u32 i = 1u; i < parents.size(); ++i) {
                if (root(i) != connected) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldRaster, invalid);
                }
            }
        }
        return {};
    }
    namespace {
        std::error_code applyFormations(
            const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry, ChunkGrid &grid,
            const GenerationCellBounds candidates, const IContext *const context, u64 *const known_scratch_peak) {
            const auto &r = plan.recipe();
            const bool formations = std::ranges::any_of(
                r.m_biomes, [](const auto &biome) { return biome.m_formation_probability > 0.0; });
            if (not formations) {
                return {};
            }
            auto raster = compileGenerationRaster(plan, layout, geometry, context);
            PPR_RETURN_ERROR_ON_FAIL(WorldRaster, raster);
            struct Formation {
                u32 x{}, y{}, biome{}, length{}, half_width{};
                i32 direction{};
                Cell support{};
            };
            Array<Formation> records;
            records.reserve(4096u);
            // Discovery sees this frozen grid only. No writes occur until every
            // admitted root and its complete tapered footprint have been collected.
            const ChunkGrid &original = grid;
            const auto cell = [&](const u32 x, const u32 y) {
                const auto cells = original.residentCells({x / kChunkEdge, y / kChunkEdge});
                return cells ? (*cells)[(y % kChunkEdge) * kChunkEdge + x % kChunkEdge] : Cell{};
            };
            for (u32 ty = std::max(1u, candidates.m_bottom / 16u);
                ty < std::min(kWorldEdge / 16u - 1u, candidates.m_top / 16u); ++ty) {
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, canceled(context));
                for (u32 tx = std::max(1u, candidates.m_left / 16u);
                    tx < std::min(kWorldEdge / 16u - 1u, candidates.m_right / 16u); ++tx) {
                    const u64 tile = static_cast<u64>(ty) * 256u + tx;
                    GenerationRandom random{generationStreamSeed(plan.seed(), 0x666f726d6174696full, tile)};
                    const u32 x = tx * 16u + static_cast<u32>(random.bounded(16u));
                    const u32 initial_y = ty * 16u + static_cast<u32>(random.bounded(16u));
                    const u32 biome_index = generationBiomeAt(
                        plan, layout, (x + 0.5) * r.m_cell_size_m, (initial_y + 0.5) * r.m_cell_size_m);
                    const auto &biome = r.m_biomes[biome_index];
                    if (random.unit() >= biome.m_formation_probability or records.size() == 4096u) {
                        continue;
                    }
                    const i32 direction = random.bounded(2u) == 0u ? -1 : 1;
                    std::optional<u32> root_y;
                    for (u32 dy = 0u; dy < 16u; ++dy) {
                        const u32 y = ty * 16u + ((initial_y + dy) % 16u);
                        const u32 next_y = static_cast<u32>(static_cast<i32>(y) + direction);
                        if (cell(x, y).m_element != 0u and cell(x, next_y).m_element == 0u) {
                            root_y = y;
                            break;
                        }
                    }
                    if (not root_y) {
                        continue;
                    }
                    const Cell support = cell(x, *root_y);
                    const auto host = std::ranges::find(r.m_materials, support.m_element, &GenerationMaterial::m_id);
                    if (host == r.m_materials.end() or not host->m_diggable) {
                        continue;
                    }
                    const u32 length = std::max(
                        2u, static_cast<u32>(std::ceil(
                                biome.m_formation_length_m * (0.45 + 0.55 * random.unit()) / r.m_cell_size_m)));
                    const u32 width = 1u + static_cast<u32>(random.bounded(3u));
                    bool valid = true;
                    for (i32 dx = -static_cast<i32>(width); dx <= static_cast<i32>(width); ++dx) {
                        const u32 gx = static_cast<u32>(static_cast<i32>(x) + dx);
                        valid = valid and cell(gx, *root_y).m_element != 0u and not raster->protectedCell(gx, *root_y);
                    }
                    for (u32 step = 1u; valid and step <= length; ++step) {
                        const i32 gy = static_cast<i32>(*root_y) + direction * static_cast<i32>(step);
                        if (gy <= 0 or gy >= static_cast<i32>(kWorldEdge) - 1) {
                            valid = false;
                            break;
                        }
                        const i32 half = static_cast<i32>((width * (length - step)) / length);
                        for (i32 dx = -half - 1; dx <= half + 1; ++dx) {
                            const u32 gx = static_cast<u32>(static_cast<i32>(x) + dx), y = static_cast<u32>(gy);
                            if (cell(gx, y).m_element != 0u or
                                raster->protectedCell(gx, y) or
                                generationBiomeAt(
                                    plan, layout, (gx + 0.5) * r.m_cell_size_m, (y + 0.5) * r.m_cell_size_m) !=
                                    biome_index) {
                                valid = false;
                                break;
                            }
                        }
                    }
                    if (valid) {
                        records.push_back({x, *root_y, biome_index, length, width, direction, support});
                    }
                }
            }
            if (known_scratch_peak != nullptr) {
                *known_scratch_peak = std::max(
                    *known_scratch_peak, raster->indexPayloadBytes() + records.capacity() * sizeof(Formation));
            }

            for (const auto &record: records) {
                PPR_RETURN_ERROR_ON_FAIL(WorldRaster, canceled(context));
                const auto &biome = r.m_biomes[record.biome];
                bool clear = true;
                for (u32 step = 1u; clear and step <= record.length; ++step) {
                    const u32 y = static_cast<u32>(
                        static_cast<i32>(record.y) + record.direction * static_cast<i32>(step));
                    const i32 half = static_cast<i32>((record.half_width * (record.length - step)) / record.length);
                    for (i32 dx = -half; dx <= half; ++dx) {
                        if (cell(static_cast<u32>(static_cast<i32>(record.x) + dx), y).m_element != 0u) {
                            clear = false;
                            break;
                        }
                    }
                }
                if (not clear) {
                    continue;
                }
                for (u32 step = 1u; step <= record.length; ++step) {
                    const u32 y = static_cast<u32>(
                        static_cast<i32>(record.y) + record.direction * static_cast<i32>(step));
                    const i32 half = static_cast<i32>((record.half_width * (record.length - step)) / record.length);
                    for (i32 dx = -half; dx <= half; ++dx) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            WorldRaster, grid.setCell(
                                             {static_cast<u32>(static_cast<i32>(record.x) + dx), y},
                                             {biome.m_formation_material, record.support.m_temperature}));
                    }
                }
                if (biome.m_rind_material != 0u) {
                    for (i32 dx = -static_cast<i32>(record.half_width); dx <= static_cast<i32>(record.half_width);
                        ++dx) {
                        const u32 x = static_cast<u32>(static_cast<i32>(record.x) + dx);
                        if (cellDraw(plan.seed(), biome.m_key, x, record.y, 0x72696e64636f6e74ull) < 0.55) {
                            PPR_RETURN_ERROR_ON_FAIL(
                                WorldRaster,
                                grid.setCell({x, record.y}, {biome.m_rind_material, record.support.m_temperature}));
                        }
                    }
                }
            }
            return {};
        }
    }
    std::error_code finishGenerationTerrain(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry, ChunkGrid &grid,
        const IContext *const context, u64 *const known_scratch_peak) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRaster, applyFormations(
                             plan, layout, geometry, grid, {16u, 16u, kWorldEdge - 16u, kWorldEdge - 16u}, context,
                             known_scratch_peak));
        PPR_RETURN_ERROR_ON_FAIL(WorldRaster, validateGenerationBackbone(plan, geometry, grid, context));
        return {};
    }
    std::error_code finishGenerationTerrainRegion(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry, ChunkGrid &grid,
        const GenerationCellBounds candidates, const IContext *const context, u64 *const known_scratch_peak) {
        PPR_RETURN_ERROR_ON_FAIL(WorldRaster, canceled(context));
        if (candidates.m_left >= candidates.m_right or
            candidates.m_bottom >= candidates.m_top or
            candidates.m_right > kWorldEdge or
            candidates.m_top > kWorldEdge or
            candidates.m_left % 16u != 0u or
            candidates.m_bottom % 16u != 0u or
            candidates.m_right % 16u != 0u or
            candidates.m_top % 16u != 0u) {
            PPR_RETURN_ERROR_ON_FAIL(WorldRaster, std::errc::invalid_argument);
        }
        u32 reach = 2u;
        for (const auto &biome: plan.recipe().m_biomes) {
            reach = std::max(
                reach, static_cast<u32>(std::ceil(biome.m_formation_length_m / plan.recipe().m_cell_size_m)));
        }
        reach += 4u;
        const u32 left = candidates.m_left > reach ? candidates.m_left - reach : 0u;
        const u32 bottom = candidates.m_bottom > reach ? candidates.m_bottom - reach : 0u;
        const u32 right = std::min(kWorldEdge, candidates.m_right + reach);
        const u32 top = std::min(kWorldEdge, candidates.m_top + reach);
        for (u32 cy = bottom / kChunkEdge; cy <= (top - 1u) / kChunkEdge; ++cy) {
            for (u32 cx = left / kChunkEdge; cx <= (right - 1u) / kChunkEdge; ++cx) {
                if (not grid.residentCells({cx, cy})) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldRaster, std::errc::invalid_argument);
                }
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            WorldRaster, applyFormations(plan, layout, geometry, grid, candidates, context, known_scratch_peak));
        return {};
    }
    Expected<ChunkGrid> rasterizeGenerationTerrain(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry,
        const GenerationExecution &execution, u64 *const known_scratch_peak) {
        try {
            auto raster = compileGenerationRaster(plan, layout, geometry, execution.m_context);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, raster);
            ChunkGrid grid;
            if (execution.m_stage != nullptr) {
                execution.m_stage->store(EGenerationStage::terrain, std::memory_order_release);
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, rasterChunks(*raster, grid, execution, known_scratch_peak));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRaster,
                finishGenerationTerrain(plan, layout, geometry, grid, execution.m_context, known_scratch_peak));
            return grid;
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::system_error &error) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, error.code());
            // An exception carrying a success code still represents failure.
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::state_not_recoverable);
        } catch (...) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::state_not_recoverable);
        }
        std::unreachable();
    }
    Expected<GenerationRaster> compileGenerationRaster(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry,
        const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
        if (geometry.m_segments.size() > kMaxGenerationSegments) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, make_error_code(EGenerationErrc::segment_limit));
        }
        if (geometry.m_instances.size() > 1025u or geometry.m_blobs.size() > 4096u) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::value_too_large);
        }
        Array<Array<u32>> segments(kChunkCount), instances(kChunkCount), blobs(kChunkCount);
        u32 references = 0u;
        const auto &recipe = plan.recipe();
        const double scale = 1.0 / recipe.m_cell_size_m;
        const double extent = kWorldEdge / scale;
        for (u32 s = 0u; s < geometry.m_segments.size(); ++s) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            const auto &segment = geometry.m_segments[s];
            if (segment.m_rule >= recipe.m_rules.size() or
                not std::isfinite(segment.m_x0_m) or
                not std::isfinite(segment.m_y0_m) or
                not std::isfinite(segment.m_x1_m) or
                not std::isfinite(segment.m_y1_m) or
                not std::isfinite(segment.m_radius_m) or
                segment.m_radius_m <= 0.0 or
                segment.m_x0_m < 0.0 or
                segment.m_y0_m < 0.0 or
                segment.m_x1_m < 0.0 or
                segment.m_y1_m < 0.0 or
                segment.m_x0_m > extent or
                segment.m_y0_m > extent or
                segment.m_x1_m > extent or
                segment.m_y1_m > extent or
                segment.m_radius_m > extent) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::invalid_argument);
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRaster,
                indexBounds(
                    segments, references, s, (std::min(segment.m_x0_m, segment.m_x1_m) - segment.m_radius_m) * scale,
                    (std::min(segment.m_y0_m, segment.m_y1_m) - segment.m_radius_m) * scale,
                    (std::max(segment.m_x0_m, segment.m_x1_m) + segment.m_radius_m) * scale,
                    (std::max(segment.m_y0_m, segment.m_y1_m) + segment.m_radius_m) * scale));
        }

        for (u32 i = 0u; i < geometry.m_instances.size(); ++i) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            const auto &instance = geometry.m_instances[i];
            if (instance.m_feature >= recipe.m_features.size()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::invalid_argument);
            }
            const auto &bounds = recipe.m_features[instance.m_feature].m_bounds;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRaster, indexBounds(
                                 instances, references, i, instance.m_origin_x + static_cast<double>(bounds.m_min_x),
                                 instance.m_origin_y + static_cast<double>(bounds.m_min_y),
                                 instance.m_origin_x + static_cast<double>(bounds.m_max_x) - 1.0,
                                 instance.m_origin_y + static_cast<double>(bounds.m_max_y) - 1.0));
        }

        for (u32 i = 0u; i < geometry.m_blobs.size(); ++i) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, canceled(context));
            const auto &blob = geometry.m_blobs[i];
            for (u32 previous = 0u; previous < i; ++previous) {
                const auto &other = geometry.m_blobs[previous];
                if (std::tie(blob.m_biome, blob.m_deposit, blob.m_ordinal) ==
                    std::tie(other.m_biome, other.m_deposit, other.m_ordinal)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::invalid_argument);
                }
            }
            if (blob.m_biome >= recipe.m_biomes.size() or
                blob.m_deposit >= recipe.m_biomes[blob.m_biome].m_deposits.size()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::invalid_argument);
            }
            const auto &deposit = recipe.m_biomes[blob.m_biome].m_deposits[blob.m_deposit];
            if (deposit.m_shape != EGenerationDepositShape::blob or
                blob.m_ordinal >= deposit.m_blob_count or
                not std::isfinite(blob.m_x_m) or
                not std::isfinite(blob.m_y_m) or
                not std::isfinite(blob.m_radius_m) or
                not std::isfinite(blob.m_stretch) or
                not std::isfinite(blob.m_angle) or
                blob.m_angle < 2.0 * std::numbers::pi * (deposit.m_orientation_min_degrees / 360.0) or
                blob.m_angle > 2.0 * std::numbers::pi * (deposit.m_orientation_max_degrees / 360.0) or
                not std::isfinite(blob.m_offset_x) or
                not std::isfinite(blob.m_offset_y) or
                blob.m_x_m < 0.0 or
                blob.m_x_m > extent or
                blob.m_y_m < 0.0 or
                blob.m_y_m > extent or
                blob.m_radius_m < deposit.m_radius_min_m or
                blob.m_radius_m > deposit.m_radius_max_m or
                blob.m_stretch < deposit.m_stretch_min or
                blob.m_stretch > deposit.m_stretch_max or
                blob.m_offset_x < 0.0 or
                blob.m_offset_x > 1024.0 or
                blob.m_offset_y < 0.0 or
                blob.m_offset_y > 1024.0) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRaster, std::errc::invalid_argument);
            }
            const double radius = blob.m_radius_m * std::max(1.0, blob.m_stretch) * (1.0 + deposit.m_distortion) * 1.2;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRaster, indexBounds(
                                 blobs, references, i, (blob.m_x_m - radius) * scale, (blob.m_y_m - radius) * scale,
                                 (blob.m_x_m + radius) * scale, (blob.m_y_m + radius) * scale));
        }
        return GenerationRaster{plan, layout, geometry, std::move(segments), std::move(instances), std::move(blobs)};
    }
}
