module;
#include "pP/UnitTest.h"
module game.tests;
import engine.core;
import engine.physics;
import game.colony.content;
import game.colony.worldgen;
import game.colony.digtool;
import game.colony.buildings;
import game.colony.agents;
import game.colony.pathfinding;
import engine.sim;
import engine.image;
import std;

namespace pP::game::tests::detail::pack {
    using namespace colony;
    struct Fixture {
        std::filesystem::path m_root;
        Fixture() {
            const auto executable = hal::process::currentExecutablePath();
            static std::atomic<u32> ordinal{};
            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                m_root = executable.parent_path() / ("worldgen-pack-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(m_root)) {
                    break;
                }
                m_root.clear();
            }
            PPR_TEST_ASSERT(not m_root.empty());
            std::filesystem::copy(
                executable.parent_path() / "worldgen/default", m_root,
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        }
        ~Fixture() {
            // Best-effort test-fixture cleanup; a failed removal cannot affect catalog ownership.
            std::error_code error{};
            std::filesystem::remove_all(m_root, error);
        }
        std::string read(const std::string_view name) const {
            std::ifstream file{m_root / name, std::ios::binary};
            PPR_TEST_ASSERT(file.good());
            return {std::istreambuf_iterator<char>{file}, {}};
        }
        void write(const std::string_view name, const std::string_view bytes) const {
            std::ofstream file{m_root / name, std::ios::binary | std::ios::trunc};
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            PPR_TEST_ASSERT(file.good());
        }
        Expected<ContentCatalog> load(config::Diagnostics &diagnostics) const {
            return loadContentCatalog(m_root, diagnostics);
        }
    };
    void useSmallIdentityImages(const Fixture &fixture) {
        // Identity mutations exercise source-byte hashing and usage separation.
        // Full-size authored maps and mip chains have a dedicated catalog test.
        constexpr std::array<u8, 70u> first{
            0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x48u, 0x44u,
            0x52u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0x1fu,
            0x15u, 0xc4u, 0x89u, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0x9cu, 0x63u, 0x68u,
            0x68u, 0xf8u, 0xffu, 0x1fu, 0x00u, 0x06u, 0x82u, 0x02u, 0xffu, 0xb3u, 0xbeu, 0x51u, 0x33u, 0x00u, 0x00u,
            0x00u, 0x00u, 0x49u, 0x45u, 0x4eu, 0x44u, 0xaeu, 0x42u, 0x60u, 0x82u};
        constexpr std::array<u8, 70u> second{
            0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x48u, 0x44u,
            0x52u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0x1fu,
            0x15u, 0xc4u, 0x89u, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0x9cu, 0x63u, 0x38u,
            0xd0u, 0xf0u, 0xeeu, 0x3fu, 0x00u, 0x07u, 0x60u, 0x03u, 0x2eu, 0x16u, 0xe3u, 0x8cu, 0xafu, 0x00u, 0x00u,
            0x00u, 0x00u, 0x49u, 0x45u, 0x4eu, 0x44u, 0xaeu, 0x42u, 0x60u, 0x82u};
        for (const auto &entry: std::filesystem::directory_iterator{fixture.m_root / "textures"}) {
            if (entry.path().extension() != ".png") {
                continue;
            }
            const auto name = entry.path().filename().string();
            const bool alternate = name == "ice.png" or
                                   name == "ice_normal.png" or
                                   name == "background_hollows_distant.png";
            const auto bytes = alternate ? std::span<const u8>{second} : std::span<const u8>{first};
            fixture.write(
                "textures/" + name, std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()});
        }
    }

    void replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto at = text.find(before);
        PPR_TEST_ASSERT(at != std::string::npos);
        text.replace(at, before.size(), after);
    }
    std::string reorder(const std::string &text) {
        Array<std::string_view> tables{};
        const auto first = text.find('\n' + std::string{"["});
        PPR_TEST_ASSERT(first != std::string::npos);
        auto at = first;
        while (at != std::string::npos) {
            const auto next = text.find("\n[", at + 1u);
            tables.push_back(std::string_view{text}.substr(at, next == std::string::npos ? next : next - at));
            at = next;
        }
        std::string result = "# Reordered without changing resolved values\n" + text.substr(0u, first);
        for (const auto table: tables | std::views::reverse) {
            result.append(table);
            // Splitting at LF may leave a Windows CR at the end of a block.
            result.push_back('\n');
        }
        return result;
    }

    PPR_UNIT_TEST (catalog_dig_respects_protected_material_and_preflights_the_complete_rectangle) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not grid.setCell({127u, 20u}, {16u, 301.0f}));
        PPR_TEST_ASSERT(not grid.setCell({128u, 20u}, {257u, 302.0f}));
        PPR_TEST_ASSERT(not grid.setCell({129u, 20u}, {1001u, 303.0f}));
        grid.clearAllDirty();
        const DigRect rect{{127u, 20u}, 3u, 1u};
        const auto particle = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(not particle and particle.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_element == 16u);
        PPR_TEST_ASSERT(not grid.setCell({129u, 20u}, {60000u, 303.0f}));
        const auto unknown = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(not unknown and unknown.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_element == 16u);
        PPR_TEST_ASSERT(not grid.setCell({129u, 20u}, {32u, 303.0f}));
        const auto result = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(result.has_value() and result->m_dug == 2u and result->m_touched.size() == 2u);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_element == 0u);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_temperature == 301.0f);
        PPR_TEST_ASSERT(grid.getCell({128u, 20u})->m_element == 257u);
        PPR_TEST_ASSERT(grid.getCell({129u, 20u})->m_element == 0u);
        const auto repeated = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(repeated.has_value() and repeated->m_dug == 0u and repeated->m_touched.empty());
    };

    PPR_UNIT_TEST (construction_checks_complete_tiles_reservations_and_live_actor_overlap) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        sim::ChunkGrid grid;
        sim::Registry registry;
        PPR_TEST_ASSERT(not registerColonyComponents(registry));
        const Footprint print{{126u, 21u}, 3u, 3u, 257u};
        PPR_TEST_ASSERT(not validateConstruction(grid, registry, print, *catalog));
        auto invalid = print;
        invalid.m_element = 1001u;
        PPR_TEST_ASSERT(validateConstruction(grid, registry, invalid, *catalog) == std::errc::invalid_argument);
        invalid = print;
        invalid.m_min.m_x = 127u;
        PPR_TEST_ASSERT(validateConstruction(grid, registry, invalid, *catalog) == std::errc::invalid_argument);
        invalid.m_min.m_x = 4095u;
        PPR_TEST_ASSERT(validateConstruction(grid, registry, invalid, *catalog) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not grid.setCell({128u, 23u}, {16u, 299.0f}));
        PPR_TEST_ASSERT(validateConstruction(grid, registry, print, *catalog) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(not grid.setCell({128u, 23u}, {0u, 299.0f}));
        const auto actor = registry.create();
        registry.emplace<Agent>(actor, Agent{});
        registry.emplace<sim::BodyState>(actor, sim::BodyState{42.5f, 7.5f});
        registry.emplace<sim::BodyDefinition>(actor, sim::BodyDefinition{.m_half_width = 0.4f, .m_half_height = 0.9f});
        PPR_TEST_ASSERT(validateConstruction(grid, registry, print, *catalog) == std::errc::device_or_resource_busy);
        registry.get<sim::BodyState>(actor)->m_x = 10.0f;
        PPR_TEST_ASSERT(not validateConstruction(grid, registry, print, *catalog));
        const auto entity = registry.create();
        registry.emplace<BuildErrand>(entity, BuildErrand{print, 0u, 9u});
        PPR_TEST_ASSERT(validateConstruction(grid, registry, print, *catalog) == std::errc::device_or_resource_busy);
        Pathfinder finder;
        Array<sim::ChunkPos> touched;
        registry.get<sim::BodyState>(actor)->m_x = 42.5f;
        PPR_TEST_ASSERT(not advanceErrands(grid, registry, finder, touched, 64u, &*catalog));
        PPR_TEST_ASSERT(registry.get<BuildErrand>(entity)->m_progress == 0u and touched.empty());
        registry.get<sim::BodyState>(actor)->m_x = 10.0f;
        PPR_TEST_ASSERT(not advanceErrands(grid, registry, finder, touched, 4u, &*catalog));
        PPR_TEST_ASSERT(registry.get<BuildErrand>(entity)->m_progress == 4u and touched.size() == 2u);
        PPR_TEST_ASSERT(not advanceErrands(grid, registry, finder, touched, 64u, &*catalog));
        PPR_TEST_ASSERT(registry.get<BuildErrand>(entity)->m_progress == 9u);
        registry.get<BuildErrand>(entity)->m_print.m_solid = false;
        registry.get<BuildErrand>(entity)->m_print.m_element = 0u;
        registry.get<BuildErrand>(entity)->m_progress = 0u;
        PPR_TEST_ASSERT(not advanceErrands(grid, registry, finder, touched, 64u, &*catalog));
        PPR_TEST_ASSERT(grid.getCell({128u, 23u})->m_element == 257u);
    };

    PPR_UNIT_TEST (errand_preflight_rejects_invalid_actor_before_any_demolition_writes) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        sim::ChunkGrid grid;
        sim::Registry registry;
        PPR_TEST_ASSERT(not registerColonyComponents(registry));
        PPR_TEST_ASSERT(not grid.setCell({30u, 30u}, {16u, 293.0f}));
        const auto demolition = registry.create();
        registry.emplace<BuildErrand>(demolition, BuildErrand{Footprint{{30u, 30u}, 3u, 3u, 0u, false}, 0u, 9u});
        const auto build = registry.create();
        registry.emplace<BuildErrand>(build, BuildErrand{Footprint{{60u, 30u}, 3u, 3u, 257u}, 0u, 9u});
        const auto actor = registry.create();
        registry.emplace<Agent>(actor, Agent{});
        registry.emplace<sim::BodyState>(actor, sim::BodyState{10.0f, 10.0f});
        Pathfinder finder;
        Array<sim::ChunkPos> touched;
        PPR_TEST_ASSERT(advanceErrands(grid, registry, finder, touched, 64u, &*catalog) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.getCell({30u, 30u})->m_element == 16u and touched.empty());
        PPR_TEST_ASSERT(registry.get<BuildErrand>(demolition)->m_progress == 0u);
        PPR_TEST_ASSERT(registry.get<BuildErrand>(build)->m_progress == 0u);
    };

    PPR_UNIT_TEST (data_only_material_extension_reaches_raster_tools_and_colliders) {
        Fixture fixture{};
        config::Diagnostics baseline_diagnostics{};
        const auto baseline_catalog = fixture.load(baseline_diagnostics);
        PPR_TEST_ASSERT(baseline_catalog.has_value());
        fixture.write("materials.toml", fixture.read("materials.toml") + R"(
[materials.violet_ore]
numeric_id = 60000
display_name = "Violet ore"
phase = "solid"
representation = "terrain"
color_rgba = "C07CFFFF"
albedo_texture = "textures/violet_ore.png"
texture_period_m = 0.75
diggable = true
reference_density_kg_m3 = 3100.0
)");
        std::filesystem::copy_file(fixture.m_root / "textures/ice.png", fixture.m_root / "textures/violet_ore.png");
        auto biomes = fixture.read("biomes.toml");
        fixture.write("biomes.toml", biomes + R"(
[biomes.hearth.deposits.violet_lens]
kind = "threshold"
order = 65535
field = "ore_grain"
material = "violet_ore"
host_material = "sandstone"
depth_min_normalized = 0.0
depth_max_normalized = 1.0
threshold_min = -1.0
threshold_max = 1.0
)");
        config::Diagnostics diagnostics{};
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value() and not diagnostics.hasErrors());
        const auto *const material = catalog->findMaterial(u16{60000u});
        PPR_TEST_ASSERT(material and material->m_display_name == "Violet ore");
        PPR_TEST_ASSERT(material->m_diggable == true and material->m_color_rgba == 0xC07CFFFFu);
        PPR_TEST_ASSERT(
            material->m_texture_period_m == 0.75 and material->m_albedo_texture == "textures/violet_ore.png");
        const auto texture = std::ranges::find(
            catalog->textures(), *material->m_albedo_texture, &ContentTexture::m_path);
        PPR_TEST_ASSERT(texture != catalog->textures().end() and texture->m_image.m_storage.isOwned());
        PPR_TEST_ASSERT(
            catalog->textures().size() == 99u and
            catalog->contentFingerprint() != baseline_catalog->contentFingerprint());

        const auto plan = compileWorldPlan(*catalog, "frontier", 42u);
        PPR_TEST_ASSERT(plan.has_value());
        const auto layout = sim::compileBiomeLayout(*plan);
        PPR_TEST_ASSERT(layout.has_value());
        const auto geometry = sim::compileGenerationGeometry(*plan, *layout);
        PPR_TEST_ASSERT(geometry.has_value());
        const auto raster = sim::compileGenerationRaster(*plan, *layout, *geometry);
        PPR_TEST_ASSERT(raster.has_value());
        std::optional<sim::Cell> generated{};
        for (const auto &site: layout->sites()) {
            const sim::ChunkPos pos{
                static_cast<u32>(site.m_x_m * 3.0) / sim::kChunkEdge,
                static_cast<u32>(site.m_y_m * 3.0) / sim::kChunkEdge};
            const auto cells = raster->chunk(pos);
            PPR_TEST_ASSERT(cells.has_value());
            const auto found = std::ranges::find(*cells, u16{60000u}, &sim::Cell::m_element);
            if (found != cells->end()) {
                generated = *found;
                break;
            }
        }
        PPR_TEST_ASSERT(generated.has_value());
        sim::ChunkGrid grid{};
        PPR_TEST_ASSERT(not grid.setCell({1u, 1u}, *generated));
        physics::Scene scene{};
        physics::ChunkColliders colliders{{1.0f / 3.0f}};
        PPR_TEST_ASSERT(not scene.initialize({}));
        std::array<u16, 16u> occupancy{};
        occupancy[5u] = grid.getCell({1u, 1u})->m_element;
        const std::array chunks{physics::ColliderChunkView{{0, 0}, occupancy, true}};
        const std::array dirty{physics::ColliderChunkPos{0, 0}};
        u32 processed{};
        PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, dirty, 4u, 1u, processed));
        PPR_TEST_ASSERT(colliders.chainCount({0, 0}) == 1u);
        const auto dug = digCells(grid, DigRect{{1u, 1u}, 1u, 1u}, &*catalog);
        PPR_TEST_ASSERT(dug and dug->m_dug == 1u and grid.getCell({1u, 1u})->m_element == 0u);
        occupancy[5u] = grid.getCell({1u, 1u})->m_element;
        PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, dirty, 4u, 1u, processed));
        PPR_TEST_ASSERT(colliders.chainCount({0, 0}) == 0u);
        PPR_TEST_ASSERT(not scene.shutdown());
    };

    PPR_UNIT_TEST (complete_shipped_catalog_is_owned) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        auto catalog = fixture.load(diagnostics);
        if (not catalog) {
            std::println("{}", diagnostics.format());
        }
        PPR_TEST_ASSERT(catalog.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(catalog->packId() == "ppr_frontier");
        PPR_TEST_ASSERT(catalog->materials().size() == 45u and catalog->textures().size() == 98u);
        PPR_TEST_ASSERT(catalog->biomes().size() == 11u and catalog->features().size() == 26u);
        PPR_TEST_ASSERT(
            catalog->worlds().size() == 2u and catalog->fields().size() == 28u and catalog->backgrounds().size() == 2u);
        PPR_TEST_ASSERT(catalog->findWorld("frontier") != nullptr and catalog->findWorld("hollows") != nullptr);
        PPR_TEST_ASSERT(catalog->findWorld("unknown") == nullptr);
        PPR_TEST_ASSERT(catalog->contentFingerprint().has_value());
        std::println("Shipped content fingerprint: {:016x}", *catalog->contentFingerprint());
        PPR_TEST_ASSERT(catalog->contentFingerprint() == 0xc70c6df6a40ffaafull);
        PPR_TEST_ASSERT(catalog->findMaterial(u16{1001})->m_representation == EMaterialRepresentation::particle);
        for (const auto key: {"clay", "limestone", "salt", "sulfur", "crystal"}) {
            const auto *const material = catalog->findMaterial(key);
            PPR_TEST_ASSERT(material != nullptr);
            PPR_TEST_ASSERT(material->m_representation == EMaterialRepresentation::terrain);
            PPR_TEST_ASSERT(material->m_diggable == true);
            PPR_TEST_ASSERT(material->m_albedo_texture.has_value());
        }
        for (const auto &texture: catalog->textures()) {
            const bool background = texture.m_path.starts_with("textures/background_");
            const u32 edge = background ? 512u : 128u;
            const u32 levels = background ? 10u : 8u;
            PPR_TEST_ASSERT(texture.m_image.m_width == edge and texture.m_image.m_height == edge);
            PPR_TEST_ASSERT(texture.m_image.m_mip_count == levels);
            PPR_TEST_ASSERT(texture.m_image.m_subresources.size() == levels);
            PPR_TEST_ASSERT(texture.m_image.m_subresources.back().m_slice_pitch == 4u);
            PPR_TEST_ASSERT(texture.m_image.m_storage.isOwned());
            PPR_TEST_ASSERT(texture.m_image.m_is_srgb == not texture.m_normal_map);
        }
        const auto before = catalog->textures().front().m_image.m_storage.getBufferData().front();
        // SharedBuffer exposes read-only bytes. Extracting a mutable copy while
        // the catalog owns the image must clone rather than alter its storage.
        auto shared = catalog->textures().front().m_image.m_storage;
        auto mutable_copy = shared.moveToUnique();
        auto mutable_bytes = mutable_copy.getMutableData();
        PPR_TEST_ASSERT(mutable_bytes.has_value());
        mutable_bytes->front() ^= std::byte{1u};
        PPR_TEST_ASSERT(catalog->textures().front().m_image.m_storage.getBufferData().front() == before);
        fixture.write("textures/basalt.png", "corrupt");
        const auto failed = fixture.load(diagnostics);
        PPR_TEST_ASSERT(not failed);
        PPR_TEST_ASSERT(catalog->textures().front().m_image.m_storage.getBufferData().front() == before);
        PPR_TEST_ASSERT(diagnostics.entries().back().m_property == "materials.basalt.albedo_texture");
        PPR_TEST_ASSERT(diagnostics.entries().back().m_location.ends_with("materials.toml:10"));
        PPR_TEST_ASSERT(diagnostics.entries().back().m_message.find("basalt.png") != std::string::npos);
    };

    PPR_UNIT_TEST (shipped_profiles_compile_owned_numeric_plans) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        PPR_TEST_ASSERT(not compileWorldPlan(*catalog, "unknown", 42u));
        std::array<bool, 26u> seen_features{};
        for (const auto profile: {"frontier", "hollows"}) {
            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                auto plan = compileWorldPlan(*catalog, profile, seed);
                PPR_TEST_ASSERT(plan.has_value());
                PPR_TEST_ASSERT(plan->seed() == seed);
                PPR_TEST_ASSERT(plan->recipe().m_materials.size() == 45u);
                PPR_TEST_ASSERT(plan->fields().size() == 28u);
                PPR_TEST_ASSERT(plan->recipe().m_boundary_material == 257u);
                PPR_TEST_ASSERT(plan->recipe().m_cell_size_m == 1.0 / 3.0);
                auto layout = sim::compileBiomeLayout(*plan);
                PPR_TEST_ASSERT(layout.has_value());
                PPR_TEST_ASSERT(layout->sites().size() >= plan->recipe().m_min_sites);
                auto geometry = sim::compileGenerationGeometry(*plan, *layout);
                PPR_TEST_ASSERT(geometry.has_value());
                PPR_TEST_ASSERT(not geometry->m_instances.empty());
                PPR_TEST_ASSERT(geometry->m_instances.front().m_rule == sim::kGenerationStartRule);
                PPR_TEST_ASSERT(
                    std::ranges::count(
                        geometry->m_instances, sim::kGenerationStartRule, &sim::GenerationInstance::m_rule) == 1);
                for (const auto &instance: geometry->m_instances) {
                    PPR_TEST_ASSERT(instance.m_feature < seen_features.size());
                    seen_features[instance.m_feature] = true;
                }
                PPR_TEST_ASSERT(not geometry->m_segments.empty());
                std::array<u32, 11u> counts{};
                for (const auto &site: layout->sites()) {
                    ++counts[site.m_biome];
                }
                for (const auto &constraint: plan->recipe().m_constraints) {
                    PPR_TEST_ASSERT(counts[constraint.m_biome] >= constraint.m_required_count);
                }
            }
        }
        // Optional landmarks need not all fit one world, but the fixed seed set
        // must exercise every shipped template, including the new landmarks.
        PPR_TEST_ASSERT(std::ranges::all_of(seen_features, [](const bool seen) { return seen; }));
        auto owned = [&] {
            auto temporary = fixture.load(diagnostics);
            PPR_TEST_ASSERT(temporary.has_value());
            return compileWorldPlan(*temporary, "frontier", 42u);
        }();
        PPR_TEST_ASSERT(owned.has_value());
        PPR_TEST_ASSERT(std::isfinite(owned->fields().front().sample(100.0, 200.0)));
        PPR_TEST_ASSERT(not sim::validateGenerationRecipe(owned->recipe()));
    };

    PPR_UNIT_TEST (prepared_world_rejects_invalid_inputs_and_cancellation_before_generation) {
        struct Clock final : ITimerClock {
            mutable u32 m_calls{};
            TimePoint now() const noexcept override {
                ++m_calls;
                return {};
            }
        } clock;
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        const auto unknown = prepareWorld(*catalog, "unknown", 42u, clock);
        PPR_TEST_ASSERT(not unknown and clock.m_calls == 0u);

        auto materials = decodeMaterialCatalog(fixture.read("materials.toml"), "materials.toml", diagnostics);
        PPR_TEST_ASSERT(materials.has_value());
        const auto incomplete = prepareWorld(std::move(*materials), "frontier", 42u, clock);
        PPR_TEST_ASSERT(not incomplete and incomplete.error() == std::errc::invalid_argument);

        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = prepareWorld(
            std::move(*catalog), "frontier", 0xffffffffffffffffull, clock, {.m_context = job_context.get()});
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        const auto cancelled_load = prepareWorld(
            fixture.m_root, "hollows", 42u, clock, diagnostics, {.m_context = job_context.get()});
        PPR_TEST_ASSERT(not cancelled_load and cancelled_load.error() == std::errc::operation_canceled);
        PPR_TEST_ASSERT(clock.m_calls == 0u);
    };

    PPR_UNIT_TEST (numeric_streams_ignore_document_order_and_unused_materials) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        auto baseline = compileWorldPlan(*catalog, "frontier", 42u);
        PPR_TEST_ASSERT(baseline.has_value());
        for (const auto name:
            {"materials.toml", "fields.toml", "biomes.toml", "features.toml", "worlds.toml", "backgrounds.toml"}) {
            fixture.write(name, reorder(fixture.read(name)));
        }
        auto materials = fixture.read("materials.toml");
        materials += "\n[materials.aaa_unused]\nnumeric_id = 60000\ndisplay_name = \"Unused\"\nphase = \"solid\"\n"
                     "representation = \"particle\"\ncolor_rgba = \"ffffffff\"\nreference_density_kg_m3 = 1000.0\n";
        fixture.write("materials.toml", materials);
        catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        auto changed = compileWorldPlan(*catalog, "frontier", 42u);
        PPR_TEST_ASSERT(changed.has_value());
        PPR_TEST_ASSERT(changed->recipe().m_materials.size() == 46u);
        PPR_TEST_ASSERT(baseline->recipe().m_rules.size() == changed->recipe().m_rules.size());
        for (std::size_t i = 0u; i < baseline->recipe().m_rules.size(); ++i) {
            PPR_TEST_ASSERT(baseline->recipe().m_rules[i].m_key == changed->recipe().m_rules[i].m_key);
        }
        for (std::size_t i = 0u; i < baseline->fields().size(); ++i) {
            for (u32 sample = 0u; sample < 32u; ++sample) {
                PPR_TEST_ASSERT(
                    baseline->fields()[i].sample(sample * 13.25, 123.75) ==
                    changed->fields()[i].sample(sample * 13.25, 123.75));
            }
        }
    };

    PPR_UNIT_TEST (complete_identity_ignores_order_comments_and_source_paths) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto first = fixture.load(diagnostics);
        PPR_TEST_ASSERT(first.has_value());
        const auto identity = first->contentFingerprint();
        for (const auto name:
            {"materials.toml", "fields.toml", "biomes.toml", "features.toml", "worlds.toml", "backgrounds.toml"}) {
            fixture.write(name, reorder(fixture.read(name)));
        }
        auto manifest = fixture.read("pack.toml");
        replace(manifest, "materials.toml", "renamed.toml");
        std::filesystem::rename(fixture.m_root / "materials.toml", fixture.m_root / "renamed.toml");
        fixture.write("pack.toml", manifest);
        auto material = fixture.read("renamed.toml");
        replace(material, "textures/basalt.png", "textures/renamed.png");
        fixture.write("renamed.toml", material);
        std::filesystem::rename(fixture.m_root / "textures/basalt.png", fixture.m_root / "textures/renamed.png");
        replace(material, "textures/basalt_normal.png", "textures/renamed_normal.png");
        fixture.write("renamed.toml", material);
        std::filesystem::rename(
            fixture.m_root / "textures/basalt_normal.png", fixture.m_root / "textures/renamed_normal.png");
        auto backgrounds = fixture.read("backgrounds.toml");
        replace(backgrounds, "textures/background_frontier_distant.png", "textures/renamed_background.png");
        fixture.write("backgrounds.toml", backgrounds);
        std::filesystem::rename(
            fixture.m_root / "textures/background_frontier_distant.png",
            fixture.m_root / "textures/renamed_background.png");
        const auto reordered = fixture.load(diagnostics);
        if (not reordered) {
            std::println("{}", diagnostics.format());
        }
        PPR_TEST_ASSERT(reordered.has_value());
        PPR_TEST_ASSERT(reordered->contentFingerprint() == identity);
    };

    PPR_UNIT_TEST (identity_changes_for_every_definition_kind_and_texture_bytes) {
        Fixture fixture{};
        useSmallIdentityImages(fixture);
        config::Diagnostics diagnostics{};
        const auto baseline = fixture.load(diagnostics);
        PPR_TEST_ASSERT(baseline.has_value());
        struct Change {
            std::string_view file, before, after;
        };
        const Change changes[] = {
            {"materials.toml", "2900.0", "2901.0"},
            {"fields.toml", "0.055", "0.056"},
            {"fields.toml", "axis_scale_x = 1.0", "axis_scale_x = 1.1"},
            {"materials.toml", "metallic = 0.0", "metallic = 0.1"},
            {"materials.toml", "roughness = 0.87", "roughness = 0.88"},
            {"materials.toml", "emissive_r = 0.0", "emissive_r = 0.1"},
            {"materials.toml", "emissive_g = 0.0", "emissive_g = 0.1"},
            {"materials.toml", "emissive_b = 0.0", "emissive_b = 0.1"},
            {"materials.toml", "emissive_strength = 1.0", "emissive_strength = 1.1"},
            {"materials.toml", "ior = 1.5", "ior = 1.6"},
            {"materials.toml", "normal_scale = 1.0", "normal_scale = 0.9"},
            {"materials.toml", "refraction_depth_m = 0.0", "refraction_depth_m = 0.1"},
            {"materials.toml", "alpha_mode = \"opaque\"", "alpha_mode = \"blend\""},
            {"backgrounds.toml", "parallax_factor = 0.08", "parallax_factor = 0.09"},
            {"biomes.toml", "293.0", "294.0"},
            {"features.toml", "Hearth landing", "Hearth landing revised"},
            {"worlds.toml", "site_spacing_m = 160.0", "site_spacing_m = 161.0"},
            {"worlds.toml", "turn_probability = 0.12", "turn_probability = 0.13"},
            {"worlds.toml", "fill_height_m = 4.333333333333333", "fill_height_m = 4.666666666666667"},
        };
        for (const auto &change: changes) {
            const auto original = fixture.read(change.file);
            auto changed = original;
            replace(changed, change.before, change.after);
            fixture.write(change.file, changed);
            const auto candidate = fixture.load(diagnostics);
            PPR_TEST_ASSERT(candidate.has_value());
            PPR_TEST_ASSERT(candidate->contentFingerprint() != baseline->contentFingerprint());
            fixture.write(change.file, original);
        }
        for (const auto &paths:
            {std::pair{"textures/basalt.png", "textures/ice.png"},
                std::pair{"textures/basalt_normal.png", "textures/ice_normal.png"},
                std::pair{"textures/background_frontier_distant.png", "textures/background_hollows_distant.png"}}) {
            const auto original = fixture.read(paths.first);
            fixture.write(paths.first, fixture.read(paths.second));
            const auto texture_change = fixture.load(diagnostics);
            PPR_TEST_ASSERT(texture_change.has_value());
            PPR_TEST_ASSERT(texture_change->contentFingerprint() != baseline->contentFingerprint());
            fixture.write(paths.first, original);
        }
    };

    PPR_UNIT_TEST (texture_usage_is_distinct_and_background_diagnostics_preserve_source_keys) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto materials = fixture.read("materials.toml");
        replace(
            materials, "normal_texture = \"textures/basalt_normal.png\"", "normal_texture = \"textures/basalt.png\"");
        fixture.write("materials.toml", materials);
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        u32 usages = 0u;
        for (const auto &texture: catalog->textures()) {
            if (texture.m_path == "textures/basalt.png") {
                ++usages;
                PPR_TEST_ASSERT(texture.m_image.m_is_srgb == not texture.m_normal_map);
            }
        }
        PPR_TEST_ASSERT(usages == 2u);
        auto worlds = fixture.read("worlds.toml");
        replace(worlds, "background_stack = \"frontier\"", "background_stack = \"absent\"");
        fixture.write("worlds.toml", worlds);
        config::Diagnostics missing;
        PPR_TEST_ASSERT(not fixture.load(missing));
        PPR_TEST_ASSERT(missing.entries().back().m_property == "worlds.frontier.background_stack");
        replace(worlds, "background_stack = \"absent\"", "background_stack = \"frontier\"");
        fixture.write("worlds.toml", worlds);
        std::filesystem::remove(fixture.m_root / "textures/background_frontier_distant.png");
        config::Diagnostics background;
        PPR_TEST_ASSERT(not fixture.load(background));
        PPR_TEST_ASSERT(background.entries().back().m_property == "backgrounds.frontier.layers.distant.texture");
        PPR_TEST_ASSERT(background.entries().back().m_location.find("backgrounds.toml:") != std::string::npos);
    };
    PPR_UNIT_TEST (required_texture_limits_missing_and_cancel_are_errors) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto original = fixture.read("textures/basalt.png");
        std::filesystem::resize_file(fixture.m_root / "textures/basalt.png", kMaxTextureInputBytes + 1u);
        const auto large = fixture.load(diagnostics);
        PPR_TEST_ASSERT(not large and large.error() == std::errc::file_too_large);
        fixture.write("textures/basalt.png", original);
        std::filesystem::remove(fixture.m_root / "textures/basalt.png");
        const auto missing = fixture.load(diagnostics);
        PPR_TEST_ASSERT(not missing and missing.error() == std::errc::no_such_file_or_directory);
        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = loadContentCatalog(fixture.m_root, diagnostics, job_context.get());
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
    };
}
namespace pP::game::tests {
    const UnitTest &gamePackContentTests() noexcept {
        static const UnitTest tests = UnitTest::Named("pack_content") / [](UnitTest::IRun &_) -> void {
            _.recurse(
                {detail::pack::catalog_dig_respects_protected_material_and_preflights_the_complete_rectangle,
                    detail::pack::construction_checks_complete_tiles_reservations_and_live_actor_overlap,
                    detail::pack::errand_preflight_rejects_invalid_actor_before_any_demolition_writes,
                    detail::pack::complete_shipped_catalog_is_owned,
                    detail::pack::data_only_material_extension_reaches_raster_tools_and_colliders,
                    detail::pack::shipped_profiles_compile_owned_numeric_plans,
                    detail::pack::prepared_world_rejects_invalid_inputs_and_cancellation_before_generation,
                    detail::pack::numeric_streams_ignore_document_order_and_unused_materials,
                    detail::pack::complete_identity_ignores_order_comments_and_source_paths,
                    detail::pack::identity_changes_for_every_definition_kind_and_texture_bytes,
                    detail::pack::texture_usage_is_distinct_and_background_diagnostics_preserve_source_keys,
                    detail::pack::required_texture_limits_missing_and_cancel_are_errors});
        };
        return tests;
    }
}
