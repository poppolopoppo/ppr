module;
#include "pP/Macros.h"
module engine.sim;
import :worldgen.layout;
import :chunk_grid;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldLayout, info, none);
    namespace {
        class GenerationCategory final : public std::error_category {
            const char *name() const noexcept override {
                return "sim.generation";
            }
            std::string message(const int value) const override {
                switch (static_cast<EGenerationErrc>(value)) {
                    case EGenerationErrc::constraints_exhausted: return "Generation placement constraints exhausted";
                    case EGenerationErrc::segment_limit: return "Generation segment limit (1048576) exceeded";
                    case EGenerationErrc::room_anchor_limit: return "Generation room anchor limit (1024) exceeded";
                    case EGenerationErrc::raster_reference_limit:
                        return "Generation chunk-reference limit (16777216) exceeded";
                    case EGenerationErrc::matter_component_limit:
                        return "Matter component-cell limit (1048576) exceeded";
                    case EGenerationErrc::matter_search_limit: return "Matter searched-cell limit (4194304) exceeded";
                    case EGenerationErrc::matter_span_limit: return "Matter span limit (1048576) exceeded";
                    case EGenerationErrc::matter_region_limit: return "Matter region limit (4096) exceeded";
                    case EGenerationErrc::matter_anchor_solid: return "Matter anchor lies in solid terrain";
                    case EGenerationErrc::matter_not_sealed: return "Matter component escapes its reserved container";
                    case EGenerationErrc::start_contour_limit: return "Start contour-cell limit (262144) exceeded";
                    case EGenerationErrc::start_invalid: return "Start clearance, support or resource guarantee failed";
                }
                return "Unknown generation error";
            }
        };
        [[nodiscard]] std::error_code canceled(const IContext *const context) noexcept {
            return context != nullptr ? context->error() : std::error_code{};
        }
        [[nodiscard]] double distanceSquared(const GenerationSite &site, const double x, const double y) noexcept {
            const double dx = site.m_x_m - x, dy = site.m_y_m - y;
            return dx * dx + dy * dy;
        }
        [[nodiscard]] bool eligible(
            const GenerationBiomeConstraint &constraint, const GenerationSite &site, const GenerationSite &start,
            const double extent) noexcept {
            const double depth = 1.0 - site.m_y_m / extent;
            const double distance = std::sqrt(distanceSquared(site, start.m_x_m, start.m_y_m));
            return depth >= constraint.m_depth_min and
                   depth <= constraint.m_depth_max and
                   distance >= constraint.m_distance_min_m and
                   distance <= constraint.m_distance_max_m;
        }
        // Each required occurrence is one node. Iterative augmenting paths can
        // reassign earlier choices, so overlapping eligibility cannot make a
        // feasible assignment fail merely because of an early greedy choice.
        [[nodiscard]] std::error_code assignRequired(
            const GenerationRecipe &recipe, Array<GenerationSite> &sites, const double extent,
            const IContext *const context) {
            struct Demand {
                u32 m_constraint{}, m_eligible{};
                u64 m_key{};
            };
            Array<Demand> demands;
            for (u32 c = 0u; c < recipe.m_constraints.size(); ++c) {
                const auto &constraint = recipe.m_constraints[c];
                u32 count = constraint.m_required_count;
                if (constraint.m_biome == recipe.m_start_biome) {
                    if (not eligible(constraint, sites[0u], sites[0u], extent)) {
                        PPR_RETURN_ERROR_ON_FAIL(WorldLayout, make_error_code(EGenerationErrc::constraints_exhausted));
                    }
                    if (count > 0u) {
                        --count;
                    }
                }
                u32 candidates = 0u;
                for (std::size_t s = 1u; s < sites.size(); ++s) {
                    if (eligible(constraint, sites[s], sites[0u], extent)) {
                        ++candidates;
                    }
                }
                if (count > candidates) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldLayout, make_error_code(EGenerationErrc::constraints_exhausted));
                }
                for (u32 i = 0u; i < count; ++i) {
                    demands.push_back({c, candidates, constraint.m_key});
                }
            }
            std::stable_sort(demands.begin(), demands.end(), [](const Demand &a, const Demand &b) {
                return std::tie(a.m_eligible, a.m_key) < std::tie(b.m_eligible, b.m_key);
            });
            Array<i32> owners(sites.size(), -1), parent(sites.size(), -1);
            Array<u8> seen(demands.size(), 0u);
            Array<u32> queue;
            queue.reserve(demands.size());
            for (u32 d = 0u; d < demands.size(); ++d) {
                PPR_RETURN_ERROR_ON_FAIL(WorldLayout, canceled(context));
                std::ranges::fill(parent, -1);
                std::ranges::fill(seen, u8{0u});
                queue.clear();
                queue.push_back(d);
                seen[d] = 1u;
                i32 free_site = -1;
                for (std::size_t q = 0u; q < queue.size() and free_site < 0; ++q) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldLayout, canceled(context));
                    const u32 demand = queue[q];
                    const auto &constraint = recipe.m_constraints[demands[demand].m_constraint];
                    for (u32 s = 1u; s < sites.size(); ++s) {
                        if (parent[s] >= 0 or not eligible(constraint, sites[s], sites[0u], extent)) {
                            continue;
                        }
                        parent[s] = static_cast<i32>(demand);
                        if (owners[s] < 0) {
                            free_site = static_cast<i32>(s);
                            break;
                        }
                        const auto occupied = static_cast<u32>(owners[s]);
                        if (seen[occupied] == 0u) {
                            seen[occupied] = 1u;
                            queue.push_back(occupied);
                        }
                    }
                }
                if (free_site < 0) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldLayout, make_error_code(EGenerationErrc::constraints_exhausted));
                }
                i32 site = free_site;
                while (site >= 0) {
                    const i32 demand = parent[static_cast<u32>(site)];
                    i32 previous_site = -1;
                    for (u32 s = 1u; s < owners.size(); ++s) {
                        if (owners[s] == demand) {
                            previous_site = static_cast<i32>(s);
                            break;
                        }
                    }
                    owners[static_cast<u32>(site)] = demand;
                    site = previous_site;
                }
            }
            for (u32 s = 1u; s < sites.size(); ++s) {
                if (owners[s] >= 0) {
                    sites[s].m_biome = recipe.m_constraints[demands[static_cast<u32>(owners[s])].m_constraint].m_biome;
                }
            }
            return {};
        }
    }
    const std::error_category &generationErrorCategory() noexcept {
        static const GenerationCategory category;
        return category;
    }
    std::error_code make_error_code(const EGenerationErrc error) noexcept {
        return {static_cast<int>(error), generationErrorCategory()};
    }
    BiomeLayout::BiomeLayout(const double extent_m) : m_extent_m(extent_m) {
        m_heads.fill(-1);
        m_sites.reserve(kMaxGenerationSites);
        m_next.reserve(kMaxGenerationSites);
    }
    void BiomeLayout::append(const GenerationSite site) {
        const auto bin = [this](const double value) {
            const double width = m_extent_m / kBinsPerEdge;
            auto index = std::min(static_cast<u32>(value / width), kBinsPerEdge - 1u);
            if (index > 0u and value < index * width) {
                --index;
            } else if (index + 1u < kBinsPerEdge and value >= (index + 1u) * width) {
                ++index;
            }
            return index;
        };
        const u32 index = bin(site.m_y_m) * kBinsPerEdge + bin(site.m_x_m);
        m_next.push_back(m_heads[index]);
        m_heads[index] = static_cast<i32>(m_sites.size());
        m_sites.push_back(site);
    }
    std::span<const GenerationSite> BiomeLayout::sites() const noexcept {
        return m_sites;
    }
    u32 BiomeLayout::nearest(const double x_m, const double y_m) const noexcept {
        PPR_ASSERT(not m_sites.empty() and std::isfinite(x_m) and std::isfinite(y_m));
        PPR_ASSERT(
            std::abs(x_m) <= 2.0 * kNoiseCoordinateLimitMetres and std::abs(y_m) <= 2.0 * kNoiseCoordinateLimitMetres);
        const double width = m_extent_m / kBinsPerEdge;
        const auto bin = [width](const double value) {
            return static_cast<i32>(std::clamp(std::floor(value / width), 0.0, static_cast<double>(kBinsPerEdge - 1u)));
        };
        const i32 centre_x = bin(x_m), centre_y = bin(y_m);
        double best_distance = (std::numeric_limits<double>::max)();
        u32 best = (std::numeric_limits<u32>::max)();
        const auto visit = [&](const i32 x, const i32 y) {
            for (i32 s = m_heads[static_cast<u32>(y) * kBinsPerEdge + static_cast<u32>(x)]; s >= 0;
                s = m_next[static_cast<u32>(s)]) {
                const double distance = distanceSquared(m_sites[static_cast<u32>(s)], x_m, y_m);
                if (distance < best_distance or (distance == best_distance and static_cast<u32>(s) < best)) {
                    best = static_cast<u32>(s);
                    best_distance = distance;
                }
            }
        };
        for (i32 radius = 0; radius < static_cast<i32>(kBinsPerEdge); ++radius) {
            const i32 low_x = std::max(0, centre_x - radius), high_x = std::min(63, centre_x + radius);
            const i32 low_y = std::max(0, centre_y - radius), high_y = std::min(63, centre_y + radius);
            // Enumerate only the original Chebyshev ring, in row-major order.
            // Clipping a square's perimeter would revisit world-edge bins.
            if (radius == 0) {
                visit(centre_x, centre_y);
            } else {
                if (centre_y - radius >= 0) {
                    for (i32 x = low_x; x <= high_x; ++x) {
                        visit(x, centre_y - radius);
                    }
                }
                for (i32 y = std::max(0, centre_y - radius + 1); y <= std::min(63, centre_y + radius - 1); ++y) {
                    if (centre_x - radius >= 0) {
                        visit(centre_x - radius, y);
                    }
                    if (centre_x + radius <= 63) {
                        visit(centre_x + radius, y);
                    }
                }
                if (centre_y + radius <= 63) {
                    for (i32 x = low_x; x <= high_x; ++x) {
                        visit(x, centre_y + radius);
                    }
                }
            }
            double outside = (std::numeric_limits<double>::max)();
            if (low_x > 0) {
                outside = std::min(outside, std::max(0.0, x_m - low_x * width));
            }
            if (high_x < 63) {
                outside = std::min(outside, std::max(0.0, (high_x + 1) * width - x_m));
            }
            if (low_y > 0) {
                outside = std::min(outside, std::max(0.0, y_m - low_y * width));
            }
            if (high_y < 63) {
                outside = std::min(outside, std::max(0.0, (high_y + 1) * width - y_m));
            }
            // Strict comparison preserves the lowest-ID tie rule even for a
            // site exactly on the first unvisited bin boundary.
            if (best != (std::numeric_limits<u32>::max)() and
                (outside == (std::numeric_limits<double>::max)() or
                    best_distance < std::nextafter(std::pow(std::nextafter(outside, 0.0), 2.0), 0.0))) {
                return best;
            }
        }
        PPR_ASSERT(best < m_sites.size());
        return best;
    }

    u32 generationBiomeAt(
        const GenerationPlan &plan, const BiomeLayout &layout, const double x_m, const double y_m) noexcept {
        const auto &recipe = plan.recipe();
        if (recipe.m_biomes.size() == 1u) {
            return 0u;
        }
        const auto &bounds = recipe.m_features[recipe.m_start_feature].m_bounds;
        const double scale = 1.0 / recipe.m_cell_size_m;
        const double x = x_m * scale - recipe.m_start_x_fine, y = y_m * scale - recipe.m_start_y_fine;
        if (x >= bounds.m_min_x and x < bounds.m_max_x and y >= bounds.m_min_y and y < bounds.m_max_y) {
            return recipe.m_start_biome;
        }
        const auto fields = plan.fields();
        const double warp_x = std::clamp(
            static_cast<double>(fields[recipe.m_warp_x_field].sample(x_m, y_m)), -1.0, 1.0);
        const double warp_y = std::clamp(
            static_cast<double>(fields[recipe.m_warp_y_field].sample(x_m, y_m)), -1.0, 1.0);
        return layout
            .sites()[layout.nearest(
                x_m + recipe.m_boundary_warp_amplitude_m * warp_x, y_m + recipe.m_boundary_warp_amplitude_m * warp_y)]
            .m_biome;
    }

    Expected<BiomeLayout> compileBiomeLayout(const GenerationPlan &plan, const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLayout, canceled(context));
        const auto &recipe = plan.recipe();
        const double cells_per_metre = 1.0 / recipe.m_cell_size_m;
        const double extent = kWorldEdge / cells_per_metre;
        const double boundary = recipe.m_boundary_fine / cells_per_metre;
        const auto &start_marker = recipe.m_features[recipe.m_start_feature].m_markers[recipe.m_start_marker];
        BiomeLayout layout{extent};
        layout.append(
            {(recipe.m_start_x_fine + static_cast<double>(start_marker.m_x) + 0.5) / cells_per_metre,
                (recipe.m_start_y_fine + static_cast<double>(start_marker.m_y) + 0.5) / cells_per_metre,
                recipe.m_start_biome});
        Array<u32> active{0u};
        GenerationRandom random{generationStreamSeed(plan.seed(), 0x6d6163726f736974ull, 0u)};
        while (not active.empty() and layout.m_sites.size() < recipe.m_max_sites) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLayout, canceled(context));
            const auto active_index = static_cast<u32>(random.bounded(active.size()));
            const GenerationSite parent = layout.m_sites[active[active_index]];
            bool admitted = false;
            for (u32 attempt = 0u; attempt < recipe.m_site_attempts; ++attempt) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLayout, canceled(context));
                const double angle = random.unit() * (2.0 * std::numbers::pi);
                // Uniform area density in the annulus [r,2r].
                const double radius = recipe.m_site_spacing_m * std::sqrt(1.0 + 3.0 * random.unit());
                const double x = parent.m_x_m + radius * std::cos(angle), y = parent.m_y_m + radius * std::sin(angle);
                if (x < boundary or y < boundary or x >= extent - boundary or y >= extent - boundary) {
                    continue;
                }
                const auto &nearest = layout.m_sites[layout.nearest(x, y)];
                if (distanceSquared(nearest, x, y) < recipe.m_site_spacing_m * recipe.m_site_spacing_m) {
                    continue;
                }
                active.push_back(static_cast<u32>(layout.m_sites.size()));
                layout.append({x, y, (std::numeric_limits<u32>::max)()});
                admitted = true;
                break;
            }
            if (not admitted) {
                active[active_index] = active.back();
                active.pop_back();
            }
        }
        if (layout.m_sites.size() < recipe.m_min_sites) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLayout, make_error_code(EGenerationErrc::constraints_exhausted));
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLayout, assignRequired(recipe, layout.m_sites, extent, context));
        for (u32 s = 1u; s < layout.m_sites.size(); ++s) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLayout, canceled(context));
            auto &site = layout.m_sites[s];
            if (site.m_biome != (std::numeric_limits<u32>::max)()) {
                continue;
            }
            double total = 0.0;
            for (const auto &constraint: recipe.m_constraints) {
                if (eligible(constraint, site, layout.m_sites[0u], extent)) {
                    total += constraint.m_weight;
                }
            }
            if (total == 0.0) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldLayout, make_error_code(EGenerationErrc::constraints_exhausted));
            }
            GenerationRandom choice{generationStreamSeed(plan.seed(), 0x62696f6d65736974ull, s)};
            const double draw = choice.unit();
            double cumulative = 0.0;
            for (const auto &constraint: recipe.m_constraints) {
                if (not eligible(constraint, site, layout.m_sites[0u], extent)) {
                    continue;
                }
                cumulative += constraint.m_weight;
                if (draw < cumulative / total) {
                    site.m_biome = constraint.m_biome;
                    break;
                }
            }
            PPR_ASSERT(site.m_biome < recipe.m_biomes.size());
        }
        return layout;
    }
}
