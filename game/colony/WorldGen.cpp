module;
#include "pP/Macros.h"
module game.colony.worldgen;

import engine.core;
import engine.sim;
import engine.math;

import std;

namespace pP::colony {
    // ------------------------------------------------------------------
    // private factory construction
    // ------------------------------------------------------------------

    class PreparedWorldConstruction {
    public:
        [[nodiscard]] static PreparedWorld make(
            std::shared_ptr<const ContentCatalog> catalog, WorldDescriptor descriptor, sim::GeneratedWorld world,
            const bool lighting_showcase = false) {
            return PreparedWorld{std::move(catalog), std::move(descriptor), std::move(world), lighting_showcase};
        }
    };

    // ------------------------------------------------------------------
    // prepared world
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(WorldPreparation, info, none)

    PreparedWorld::PreparedWorld(
        std::shared_ptr<const ContentCatalog> catalog, WorldDescriptor descriptor, sim::GeneratedWorld world,
        const bool lighting_showcase)
        : m_catalog(std::move(catalog)), m_descriptor(std::move(descriptor)), m_world(std::move(world)),
          m_lighting_showcase(lighting_showcase) {
    }

    PreparedWorld::PreparedWorld(PreparedWorld &&) noexcept = default;
    PreparedWorld &PreparedWorld::operator=(PreparedWorld &&) noexcept = default;

    PreparedWorld::~PreparedWorld() = default;

    const ContentCatalog &PreparedWorld::catalog() const noexcept {
        return *m_catalog;
    }

    const std::shared_ptr<const ContentCatalog> &PreparedWorld::catalogOwner() const noexcept {
        return m_catalog;
    }

    const std::shared_ptr<const ContentPresentation> &PreparedWorld::presentation() const noexcept {
        return m_presentation;
    }

    std::error_code PreparedWorld::attachPresentation(
        std::shared_ptr<const ContentPresentation> presentation, const asset::RequestGeneration generation) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation, not presentation or
                                      presentation->catalog() != m_catalog or
                                      presentation->generation() != generation or
                                      presentation->catalog()->worldId() != m_catalog->worldId() or
                                      presentation->catalog()->contentFingerprint() !=
                                          std::optional<u64>{m_descriptor.m_content_fingerprint}
                                  ? std::make_error_code(std::errc::invalid_argument)
                                  : std::error_code{});

        m_presentation = std::move(presentation);
        return {};
    }

    const WorldDescriptor &PreparedWorld::descriptor() const noexcept {
        return m_descriptor;
    }

    const sim::GeneratedWorld &PreparedWorld::generated() const noexcept {
        return m_world;
    }

    bool PreparedWorld::lightingShowcase() const noexcept {
        return m_lighting_showcase;
    }

    sim::ChunkGrid &PreparedWorld::grid() noexcept {
        return m_world.m_grid;
    }

    // ------------------------------------------------------------------
    // world preparation
    // ------------------------------------------------------------------

    std::error_code validateRuntimeWorldParameters(const sim::GenerationRecipe &recipe) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldPreparation,
            recipe.m_cell_size_m == kWorldCompileParameters.m_cell_size_m and
                    recipe.m_reference_thickness_m == kWorldCompileParameters.m_reference_thickness_m and
                    recipe.m_actor_half_width_m == kWorldCompileParameters.m_actor_half_width_m and
                    recipe.m_actor_half_height_m == kWorldCompileParameters.m_actor_half_height_m and
                    sim::isSupportedWorldEdge(recipe.m_world_edge)
                ? std::error_code{}
                : std::make_error_code(std::errc::not_supported));
        return {};
    }

    Expected<PreparedWorld> prepareWorld(
        const std::filesystem::path &root, const std::string_view profile, const u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution, const u32 world_edge) {
        const std::filesystem::path roots[] = {root};
        return prepareWorld(roots, profile, seed, clock, diagnostics, execution, world_edge);
    }

    Expected<PreparedWorld> prepareWorld(
        const std::span<const std::filesystem::path> roots, const std::string_view profile, const u64 seed,
        const ITimerClock &clock, config::Diagnostics &diagnostics, const sim::GenerationExecution &execution,
        const u32 world_edge) {
        auto catalog = loadContentCatalog(roots, diagnostics, execution.m_context.get());
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, catalog);

        auto owner = std::make_shared<const ContentDescriptorCatalog>(std::move(*catalog));
        auto world = resolveContentWorld(*owner, profile, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, world);

        auto selected = selectWorldCatalog(std::move(owner), *world, worldCompileParameters(world_edge), diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, selected);

        const auto qualified_profile = selected->worlds().front().m_key;
        return prepareWorld(std::move(*selected), qualified_profile, seed, clock, execution);
    }

    Expected<PreparedWorld> prepareWorld(
        ContentCatalog catalog, const std::string_view profile, const u64 seed, const ITimerClock &clock,
        const sim::GenerationExecution &execution) {
        return prepareWorld(
            std::make_shared<const ContentCatalog>(std::move(catalog)), profile, seed, clock, execution);
    }

    Expected<PreparedWorld> prepareWorld(
        std::shared_ptr<const ContentCatalog> catalog, const std::string_view profile, const u64 seed,
        const ITimerClock &clock, const sim::GenerationExecution &execution) {
        if (execution.m_context != nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, execution.m_context->error());
        }

        if (not catalog or not catalog->contentFingerprint()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, std::errc::invalid_argument);
        }

        auto plan = compileWorldPlan(*catalog, profile, seed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, plan);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, validateRuntimeWorldParameters(plan->recipe()));

        auto world = isLightingLabWorld(*catalog, profile) ? buildLightingLabWorld(std::move(*plan), *catalog)
                                                           : sim::generateWorld(std::move(*plan), clock, execution);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, world);

        const WorldDescriptor descriptor{
            .m_profile = std::string{profile},
            .m_seed = seed,
            .m_content_fingerprint = *catalog->contentFingerprint(),
            .m_world_edge = world->m_grid.edge()};
        return PreparedWorldConstruction::make(std::move(catalog), descriptor, std::move(*world));
    }

    // ------------------------------------------------------------------
    // shared bounded lighting scene recipe
    // ------------------------------------------------------------------

    Expected<std::vector<u16>> lightingSceneLabels(
        const std::span<const u16> ids, const bool door_open, const bool environment) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreparation,
            ids.size() == (environment ? 8u : 6u) and std::ranges::all_of(ids, [](const u16 id) { return id != 0u; })
                ? std::error_code{}
                : std::make_error_code(std::errc::invalid_argument));

        // Both recipes retain a full vacuum ring. The diagnostic divider is
        // one fine cell thick and only its eight door labels change.
        std::vector<u16> labels(kLightingSceneWidth * kLightingSceneHeight, 0u);
        if (environment) {
            // Thin banks expose sediment rather than filling the picture
            // with unlit cut faces. The central shaft opens between shelves.
            for (u32 y = 1u; y + 1u < kLightingSceneHeight; ++y) {
                for (u32 x = 1u; x + 1u < kLightingSceneWidth; ++x) {
                    const u32 floor = 4u + ((x / 7u) % 3u);
                    const u32 ceiling = 31u + ((x / 9u) % 2u);
                    const bool mass = y <= floor or
                                      y >= ceiling or
                                      x <= 2u or
                                      x >= 61u or
                                      (x >= 4u and x <= 10u and y <= 12u) or
                                      (x >= 7u and x <= 17u and y >= 12u and y <= 14u) or
                                      (x >= 27u and x <= 40u and y >= 18u and y <= 19u) or
                                      (x >= 53u and x <= 60u and y <= 9u);
                    if (not mass) {
                        continue;
                    }

                    u16 label = y == floor ? ids[1u] : ids[0u];
                    if (y == 3u or y == 4u or y == ceiling + 1u) {
                        label = ids[2u];
                    }

                    if ((x >= 5u and x <= 9u and y >= 8u and y <= 11u) or
                        (x >= 55u and x <= 59u and y >= 5u and y <= 8u)) {
                        label = ids[3u];
                    }

                    if (x >= 34u and x <= 39u and y >= 31u) {
                        label = ids[4u];
                    }

                    labels[y * kLightingSceneWidth + x] = label;
                }
            }

            // Actual opaque Grid labels form a platform, riser, pipe and
            // tank shell. They use captured ore rows and receive the RC field.
            for (u32 y = 7u; y <= 24u; ++y) {
                for (u32 x = 26u; x <= 52u; ++x) {
                    const bool platform = y == 14u and x <= 39u;
                    const bool support = x == 28u and y <= 13u;
                    const bool pipe = (x == 39u and y >= 15u and y <= 24u) or (y == 24u and x >= 39u and x <= 49u);
                    const bool tank = x >= 43u and y <= 15u and (x == 43u or x == 52u or y == 7u or y == 15u);
                    if (platform or support or pipe or tank) {
                        labels[y * kLightingSceneWidth + x] = pipe ? ids[4u] : ids[3u];
                    }
                }
            }

            // Each emitting object has a complete vacuum sample gap from
            // every other source/solid. No differing-S dual cell is admitted.
            for (u32 y = 1u; y + 1u < kLightingSceneHeight; ++y) {
                for (u32 x = 1u; x + 1u < kLightingSceneWidth; ++x) {
                    const bool amber = x >= 30u and x <= 31u and y >= 21u and y <= 22u;
                    const bool violet = (x == 53u and y >= 23u and y <= 26u) or
                                        (x == 55u and y >= 23u and y <= 28u) or
                                        (x == 57u and y >= 23u and y <= 25u);
                    const bool cyan = x >= 45u and x <= 50u and y >= 9u and y <= 12u;
                    if (amber or violet or cyan) {
                        labels[y * kLightingSceneWidth + x] = amber ? ids[5u] : violet ? ids[6u] : ids[7u];
                    }
                }
            }
        } else {
            for (u32 y = 1u; y + 1u < kLightingSceneHeight; ++y) {
                for (u32 x = 1u; x + 1u < kLightingSceneWidth; ++x) {
                    const u32 floor = 6u + ((x / 6u) % 3u == 0u ? 2u : 0u);
                    const u32 ceiling = 29u - ((x / 8u) % 3u == 1u ? 2u : 0u);
                    const bool mass = y <= floor or
                                      y >= ceiling or
                                      x <= 3u or
                                      x >= 60u or
                                      (x >= 8u and x <= 11u and y <= 12u) or
                                      (x >= 48u and x <= 52u and y <= 12u);
                    u16 label{};
                    if (mass) {
                        label = y < 4u ? ids[2u] : y < 7u ? ids[1u] : ids[0u];
                        if ((x >= 6u and x <= 14u and y >= 9u and y <= 12u) or
                            (x >= 46u and x <= 54u and y >= 5u and y <= 10u)) {
                            label = ids[3u];
                        }
                        if (x >= 42u and x <= 47u and y >= 26u) {
                            label = ids[4u];
                        }
                    }

                    if (x == 32u and y >= 7u and y <= 29u and (not door_open or y < 13u or y > 20u)) {
                        label = ids[3u];
                    }
                    if (x >= 17u and x <= 18u and y >= 21u and y <= 22u) {
                        label = ids[5u];
                    }

                    labels[y * kLightingSceneWidth + x] = label;
                }
            }
        }

        return labels;
    }

    // ------------------------------------------------------------------
    // interactive industrial service bay
    // ------------------------------------------------------------------

    Expected<std::vector<u16>> lightingShowcaseLabels(const std::span<const u16> ids) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreparation, ids.size() == 12u and std::ranges::all_of(ids, [](const u16 id) { return id != 0u; })
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));

        auto labels = lightingSceneLabels(ids.first(8u), false, true);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, labels);

        // Replace only the old industrial silhouettes. Preserve the sediment
        // ledge and cyan source; amber is remounted below the ledge later.
        // Headless recipes never enter this helper.
        for (u32 y = 7u; y <= 24u; ++y) {
            for (u32 x = 26u; x <= 52u; ++x) {
                const bool ledge = x >= 27u and x <= 40u and y >= 18u and y <= 19u;
                const bool source = x >= 45u and x <= 50u and y >= 9u and y <= 12u;
                if (not ledge and not source) {
                    (*labels)[y * kLightingSceneWidth + x] = 0u;
                }
            }
        }

        // Taper the bank down to the spawn shelf, exposing a bounded dark ore
        // pocket. No new mass protrudes into the full-width walking corridor.
        for (u32 x = 4u; x <= 17u; ++x) {
            const u32 top = 12u - (x - 4u) * 6u / 13u;
            for (u32 y = 5u; y <= 14u; ++y) {
                const bool ore = x >= 7u and x <= 11u and y + 2u >= top;
                (*labels)[y * kLightingSceneWidth + x] = y > top ? 0u : ore ? ids[3u] : ids[0u];
            }
        }

        // The level tread joins the existing canonical spawn support. Keep
        // seven native rows (2.33 m) clear above it for the 0.8 x 1.8 m body.
        for (u32 x = 18u; x <= 41u; ++x) {
            (*labels)[6u * kLightingSceneWidth + x] = ids[9u];
            for (u32 y = 7u; y <= 13u; ++y) {
                (*labels)[y * kLightingSceneWidth + x] = 0u;
            }
        }

        for (const u32 x: {26u, 38u}) {
            for (u32 y = 1u; y <= 5u; ++y) {
                (*labels)[y * kLightingSceneWidth + x] = ids[8u];
            }
        }

        // Initial OPEN service door: the seven actor rows stay vacuum. Its
        // fixed lintel joins the tread-to-ledge column above that clearance.
        for (u32 y = 14u; y <= 17u; ++y) {
            (*labels)[y * kLightingSceneWidth + 36u] = ids[8u];
        }

        // Mount the same four amber cells directly beneath the central ledge,
        // with a metal top plate and vacuum below/both sides. The ledge no
        // longer blocks the source from the receiving wall and walking route.
        for (u32 x = 29u; x <= 32u; ++x) {
            (*labels)[18u * kLightingSceneWidth + x] = ids[8u];
        }

        for (u32 y = 16u; y <= 17u; ++y) {
            for (u32 x = 30u; x <= 31u; ++x) {
                (*labels)[y * kLightingSceneWidth + x] = ids[5u];
            }
        }

        // Oriented pipe segments and their occupied elbow stay above the actor
        // clearance. The final union contacts the tank's left top cap.
        for (u32 x = 32u; x <= 39u; ++x) {
            (*labels)[23u * kLightingSceneWidth + x] = ids[10u];
        }

        for (u32 y = 16u; y <= 22u; ++y) {
            (*labels)[y * kLightingSceneWidth + 39u] = ids[11u];
        }

        for (u32 x = 39u; x <= 45u; ++x) {
            (*labels)[16u * kLightingSceneWidth + x] = ids[10u];
        }

        // Cyan is an opaque emissive proxy, not liquid behind glass. The top
        // opening and right-side vent are real vacuum, with no invisible trim.
        for (u32 y = 7u; y <= 15u; ++y) {
            for (u32 x = 43u; x <= 52u; ++x) {
                const bool base = y == 7u;
                const bool cap = y == 15u and (x <= 45u or x >= 50u);
                const bool side = x == 43u or (x == 52u and (y <= 9u or y >= 12u));
                if (base or cap or side) {
                    (*labels)[y * kLightingSceneWidth + x] = ids[8u];
                }
            }
        }

        for (const u32 x: {44u, 51u}) {
            for (u32 y = 1u; y <= 6u; ++y) {
                (*labels)[y * kLightingSceneWidth + x] = ids[8u];
            }
        }

        return labels;
    }

    // ------------------------------------------------------------------
    // authored showcase construction and canonical actor start
    // ------------------------------------------------------------------

    Expected<sim::GenerationStart> validateLightingShowcaseStart(
        const sim::ChunkGrid &grid, u64 *const known_scratch_peak) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreparation, sim::isSupportedWorldEdge(grid.edge())
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));

        // Fixed authored marker, in fine cells. Its entire actor footprint is
        // on the level sediment shelf and clear of the higher left overhang.
        constexpr double spawn_x = 19.25, marker_y = 10.0;
        const double half_width = kWorldCompileParameters.m_actor_half_width_m / kFineCellMetres;
        const double body_height = 2.0 * kWorldCompileParameters.m_actor_half_height_m / kFineCellMetres;
        const double left = spawn_x - half_width, right = spawn_x + half_width;
        const auto points = math::contourPoints();
        const auto cases = math::contourCases();
        using Point = std::array<double, 2>;
        std::vector<std::array<Point, 3>> triangles;
        std::vector<std::array<double, 3>> supports;
        PPR_DEFER {
            if (known_scratch_peak != nullptr) {
                *known_scratch_peak = std::max(
                    *known_scratch_peak, triangles.capacity() * sizeof(std::array<Point, 3>) +
                                             supports.capacity() * sizeof(std::array<double, 3>));
            }
        };

        for (u32 y = 0u; y + 1u < kLightingSceneHeight; ++y) {
            for (u32 x = 0u; x + 1u < kLightingSceneWidth; ++x) {
                constexpr std::array<std::array<u32, 2>, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
                u32 mask{};
                for (u32 corner = 0u; corner < corners.size(); ++corner) {
                    const auto cell = grid.getCell({x + corners[corner][0], y + corners[corner][1]});
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, cell);
                    if (cell->m_element != 0u) {
                        mask |= 1u << corner;
                    }
                }

                const auto point = [&](const u32 index) -> Point {
                    return {double(x) + 0.5 + double(points[index].m_x), double(y) + 0.5 + double(points[index].m_y)};
                };
                const auto &shape = cases[mask];
                for (u32 index = 0u; index < shape.m_segment_count; ++index) {
                    const auto a = point(shape.m_segments[index].m_begin);
                    const auto b = point(shape.m_segments[index].m_end);
                    if (a[1] == b[1] and a[0] > b[0] and a[1] <= marker_y) {
                        supports.push_back({a[1], b[0], a[0]});
                    }
                }

                for (u32 index = 0u; index < shape.m_triangle_count; ++index) {
                    const auto &triangle = shape.m_triangles[index];
                    triangles.push_back({point(triangle.m_a), point(triangle.m_b), point(triangle.m_c)});
                }
            }
        }

        std::ranges::sort(
            supports, [](const auto &a, const auto &b) { return a[0] != b[0] ? a[0] > b[0] : a[1] < b[1]; });

        std::optional<double> floor;
        for (std::size_t index = 0u; index < supports.size();) {
            const double level = supports[index][0];
            double begin = supports[index][1], end = supports[index][2];
            for (++index; index < supports.size() and supports[index][0] == level; ++index) {
                if (supports[index][1] > end) {
                    if (begin <= left and end >= right) {
                        floor = level;
                    }
                    begin = supports[index][1];
                }
                end = std::max(end, supports[index][2]);
            }

            if (floor or (begin <= left and end >= right)) {
                floor = level;
                break;
            }
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreparation, floor ? std::error_code{} : sim::make_error_code(sim::EGenerationErrc::start_invalid));

        const double bottom = *floor, top = bottom + body_height;
        const std::array<Point, 4> rectangle{{{left, bottom}, {right, bottom}, {right, top}, {left, top}}};
        for (const auto &triangle: triangles) {
            std::array<Point, 5> axes{{{1.0, 0.0}, {0.0, 1.0}}};
            for (u32 edge = 0u; edge < 3u; ++edge) {
                const auto &a = triangle[edge], &b = triangle[(edge + 1u) % 3u];
                axes[edge + 2u] = {a[1] - b[1], b[0] - a[0]};
            }

            bool separated{};
            for (const auto &axis: axes) {
                const auto projection = [&](const Point point) { return point[0] * axis[0] + point[1] * axis[1]; };
                double triangle_min = projection(triangle[0]), triangle_max = triangle_min;
                double box_min = projection(rectangle[0]), box_max = box_min;
                for (const auto &point: triangle) {
                    triangle_min = std::min(triangle_min, projection(point));
                    triangle_max = std::max(triangle_max, projection(point));
                }

                for (const auto &point: rectangle) {
                    box_min = std::min(box_min, projection(point));
                    box_max = std::max(box_max, projection(point));
                }

                // Contact with the proved supporting floor is allowed; any
                // positive occupied overlap with the actor is rejected.
                if (triangle_max <= box_min or box_max <= triangle_min) {
                    separated = true;
                    break;
                }
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldPreparation,
                separated ? std::error_code{} : sim::make_error_code(sim::EGenerationErrc::start_invalid));
        }

        return sim::GenerationStart{
            spawn_x * kFineCellMetres, bottom * kFineCellMetres + kWorldCompileParameters.m_actor_half_height_m,
            bottom * kFineCellMetres, {}};
    }

    Expected<PreparedWorld> prepareLightingShowcase(
        const std::span<const std::filesystem::path> roots, const u64 seed, const ITimerClock &clock,
        config::Diagnostics &diagnostics, const sim::GenerationExecution &execution, const u32 world_edge) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreparation, execution.m_context ? execution.m_context->error() : std::error_code{});
        auto loaded = loadContentCatalog(roots, diagnostics, execution.m_context.get());
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, loaded);
        auto descriptors = std::make_shared<const ContentDescriptorCatalog>(std::move(*loaded));
        const auto selected_world = resolveContentWorld(*descriptors, "lighting_showcase", diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, selected_world);
        auto selected = selectWorldCatalog(
            std::move(descriptors), *selected_world, worldCompileParameters(world_edge), diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, selected);
        auto catalog = std::make_shared<const ContentCatalog>(std::move(*selected));
        auto plan = compileWorldPlan(*catalog, catalog->worlds().front().m_key, seed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, plan);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, validateRuntimeWorldParameters(plan->recipe()));

        constexpr std::array<std::string_view, 12> keys{
            "sandstone", "sandstone", "clay", "iron_ore", "copper_ore", "showcase_amber_lamp",
            "showcase_violet_sculpture", "showcase_cyan_insert", "showcase_housing", "showcase_tread",
            "showcase_pipe_horizontal", "showcase_pipe_vertical"};
        std::array<u16, 12> ids{};
        for (u32 index = 0u; index < keys.size(); ++index) {
            const auto *const material = catalog->findMaterial(keys[index]);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldPreparation, material and material->m_representation == EMaterialRepresentation::terrain
                                      ? std::error_code{}
                                      : std::make_error_code(std::errc::invalid_argument));
            ids[index] = material->m_numeric_id;
        }

        const auto labels = lightingShowcaseLabels(ids);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, labels);
        const auto raster_started = clock.now();
        sim::ChunkGrid grid{world_edge};
        for (u32 y = 0u; y < kLightingSceneHeight; ++y) {
            for (u32 x = 0u; x < kLightingSceneWidth; ++x) {
                const u16 id = (*labels)[y * kLightingSceneWidth + x];
                if (id != 0u) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, grid.setCell({x, y}, {id, 293.15f}));
                }
            }
        }

        const auto raster_elapsed = clock.now() - raster_started;
        const auto validation_started = clock.now();
        u64 start_scratch{};
        auto start = validateLightingShowcaseStart(grid, &start_scratch);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, start);
        const std::array<sim::GenerationSite, 1> sites{
            {{start->m_spawn_x_m, start->m_spawn_y_m, plan->recipe().m_start_biome}}};
        const auto layout_started = clock.now();
        auto layout = sim::restoreBiomeLayout(*plan, sites);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, layout);
        const auto layout_elapsed = clock.now() - layout_started;

        // These are measurements of the actual authored world, not procedural
        // landing/resource guarantees or copied procedural generation reports.
        sim::GenerationReport report{};
        Array<u64> counts(65536u, 0ull);
        sim::definition::SemanticFingerprint terrain{catalog->definition()};
        terrain.string("ppr.authored-lighting-showcase.grid.v1");
        terrain.integer(world_edge);
        for (u32 cy = 0u; cy < grid.chunksPerEdge(); ++cy) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldPreparation, execution.m_context ? execution.m_context->error() : std::error_code{});
            for (u32 cx = 0u; cx < grid.chunksPerEdge(); ++cx) {
                const auto payload = grid.residentCells({cx, cy});
                for (u32 index = 0u; index < sim::kCellsPerChunk; ++index) {
                    const auto cell = payload ? (*payload)[index] : sim::Cell{};
                    ++counts[cell.m_element];
                    terrain.integer(cell.m_element);
                    terrain.integer(std::bit_cast<u32>(cell.m_temperature));
                }
            }
        }

        for (u32 id = 0u; id < counts.size(); ++id) {
            if (counts[id] != 0u) {
                report.m_material_counts.push_back({static_cast<u16>(id), counts[id]});
            }
        }
        report.m_terrain_checksum = terrain.hash();
        terrain.string("ppr.authored-lighting-showcase.output.v1");
        terrain.integer(seed);
        terrain.integer(std::bit_cast<u64>(start->m_spawn_x_m));
        terrain.integer(std::bit_cast<u64>(start->m_spawn_y_m));
        report.m_output_checksum = terrain.hash();
        report.m_grid_payload_bytes = u64(grid.residentChunkCount()) * sim::kCellsPerChunk * sizeof(sim::Cell);
        report.m_retained_known_payload_bytes = grid.retainedPayloadBytes() +
                                                plan->retainedPayloadBytes() +
                                                layout->retainedPayloadBytes() +
                                                report.m_material_counts.capacity() *
                                                    sizeof(sim::GenerationMaterialCount);
        report.m_known_phase_scratch_bytes[4] = labels->capacity() * sizeof(u16) +
                                                std::max(start_scratch, counts.capacity() * sizeof(u64));
        report.m_phase_times[0] = layout_elapsed;
        report.m_phase_times[2] = raster_elapsed;
        report.m_phase_times[4] = clock.now() - validation_started - layout_elapsed;

        sim::GeneratedWorld world{
            std::move(*plan), std::move(*layout), {}, std::move(grid), {}, std::move(*start), std::move(report)};
        WorldDescriptor descriptor{
            .m_profile = catalog->worlds().front().m_key,
            .m_seed = seed,
            .m_content_fingerprint = *catalog->contentFingerprint(),
            .m_world_edge = world_edge};
        return PreparedWorldConstruction::make(std::move(catalog), std::move(descriptor), std::move(world), true);
    }

    // ------------------------------------------------------------------
    // world restoration
    // ------------------------------------------------------------------

    Expected<PreparedWorld> restorePreparedWorld(
        ContentCatalog catalog, WorldDescriptor descriptor, sim::GeneratedWorld world) {
        auto restored = restorePreparedWorld(
            std::make_shared<const ContentCatalog>(std::move(catalog)), std::move(descriptor), std::move(world));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, restored);
        return restored;
    }

    Expected<PreparedWorld> restorePreparedWorld(
        std::shared_ptr<const ContentCatalog> catalog, WorldDescriptor descriptor, sim::GeneratedWorld world) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, validateWorldMetric(descriptor.m_metric));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldPreparation, validateRuntimeWorldParameters(world.m_plan.recipe()));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldPreparation, catalog and
                                      descriptor.m_pack_schema_version == asset::Manifest::schema_version and
                                      descriptor.m_generator_version == sim::kGeneratorAlgorithmVersion and
                                      catalog->contentFingerprint() and
                                      *catalog->contentFingerprint() == descriptor.m_content_fingerprint and
                                      catalog->findWorld(descriptor.m_profile) and
                                      world.m_plan.seed() == descriptor.m_seed and
                                      descriptor.m_world_edge == world.m_grid.edge() and
                                      descriptor.m_world_edge == world.m_plan.recipe().m_world_edge
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::protocol_not_supported));
        return PreparedWorldConstruction::make(std::move(catalog), std::move(descriptor), std::move(world));
    }
}
