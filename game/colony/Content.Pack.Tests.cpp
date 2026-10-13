module;
#include "pP/UnitTest.h"
module game.tests;
import engine.core;
import engine.physics;
import game.colony.content;
import game.colony.content.authoring;
import game.colony.worldgen;
import game.colony.worldgen_preview;
import game.colony.digtool;
import game.colony.buildings;
import game.colony.agents;
import game.colony.pathfinding;
import engine.sim;
import engine.image;
import game.application;
import std;

namespace pP::game::tests::detail::pack {
    // ------------------------------------------------------------------
    // content pack
    // ------------------------------------------------------------------

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
            catalog->textures().size() == 135u and
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
        PPR_TEST_ASSERT(catalog->materials().size() == 63u and catalog->textures().size() == 134u);
        PPR_TEST_ASSERT(catalog->biomes().size() == 11u and catalog->features().size() == 26u);
        PPR_TEST_ASSERT(
            catalog->worlds().size() == 2u and catalog->fields().size() == 44u and catalog->backgrounds().size() == 2u);
        PPR_TEST_ASSERT(catalog->findWorld("frontier") != nullptr and catalog->findWorld("hollows") != nullptr);
        PPR_TEST_ASSERT(catalog->findWorld("unknown") == nullptr);
        PPR_TEST_ASSERT(catalog->contentFingerprint().has_value());
        std::println("Shipped content fingerprint: {:016x}", *catalog->contentFingerprint());
        PPR_TEST_ASSERT(catalog->contentFingerprint() == 0xf3033606eee22dd4ull);
        PPR_TEST_ASSERT(catalog->findMaterial(u16{1001})->m_representation == EMaterialRepresentation::particle);
        PPR_TEST_ASSERT(catalog->findMaterial("masonry")->m_texture_sampling == EMaterialTextureSampling::repeat);
        PPR_TEST_ASSERT(catalog->findMaterial("basalt")->m_texture_sampling == EMaterialTextureSampling::stochastic);
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
                PPR_TEST_ASSERT(plan->recipe().m_materials.size() == 63u);
                PPR_TEST_ASSERT(plan->fields().size() == 44u);
                PPR_TEST_ASSERT(plan->recipe().m_boundary_material == 257u);
                PPR_TEST_ASSERT(plan->recipe().m_cell_size_m == 1.0 / 3.0);
                PPR_TEST_ASSERT(plan->recipe().m_open_surface and plan->recipe().m_bedrock_material == 633u);
                PPR_TEST_ASSERT(catalog->findMaterial("bedrock")->m_diggable == false);
                const auto &start_feature = plan->recipe().m_features[plan->recipe().m_start_feature];
                const auto &bounds = start_feature.m_bounds;
                PPR_TEST_ASSERT(
                    sim::generationSubsurfaceBounds(
                        *plan, static_cast<i64>(plan->recipe().m_start_x_fine) + bounds.m_min_x,
                        static_cast<i64>(plan->recipe().m_start_y_fine) + bounds.m_min_y,
                        static_cast<i64>(plan->recipe().m_start_x_fine) + bounds.m_max_x,
                        static_cast<i64>(plan->recipe().m_start_y_fine) + bounds.m_max_y));
                for (std::size_t i = 0u; i < catalog->biomes().size(); ++i) {
                    const auto &source = catalog->biomes()[i];
                    const auto &target = plan->recipe().m_biomes[i];
                    PPR_TEST_ASSERT(target.m_relaxation_passes == source.m_relaxation_passes);
                    PPR_TEST_ASSERT(target.m_relaxation_adoption == source.m_relaxation_adoption);
                    PPR_TEST_ASSERT(target.m_erosion_passes == source.m_erosion_passes);
                    PPR_TEST_ASSERT(target.m_sprinkle_probability == source.m_sprinkle_probability);
                    PPR_TEST_ASSERT(
                        target.m_pore_probability == source.m_pore_probability and
                        target.m_pore_passes == source.m_pore_passes);
                    PPR_TEST_ASSERT(
                        target.m_finish_passes == source.m_finish_passes and
                        target.m_finish_adoption == source.m_finish_adoption);
                    PPR_TEST_ASSERT(
                        target.m_formation_material == source.m_formation_material_id and
                        target.m_formation_probability == source.m_formation_probability);
                    for (std::size_t j = 0u; j < source.m_composition.size(); ++j) {
                        PPR_TEST_ASSERT(target.m_bands[j].m_bed_period_m == source.m_composition[j].m_bed_period_m);
                        PPR_TEST_ASSERT(target.m_bands[j].m_bed_warp_m == source.m_composition[j].m_bed_warp_m);
                    }
                    PPR_TEST_ASSERT(target.m_deposits.size() == source.m_deposits.size());
                    for (std::size_t j = 0u; j < source.m_deposits.size(); ++j) {
                        const auto &from = source.m_deposits[j];
                        const auto &to = target.m_deposits[j];
                        PPR_TEST_ASSERT(to.m_shape == from.m_shape and to.m_stage == from.m_stage);
                        PPR_TEST_ASSERT(to.m_blob_count == from.m_blob_count);
                        PPR_TEST_ASSERT(
                            to.m_orientation_min_degrees == from.m_orientation_min_degrees and
                            to.m_orientation_max_degrees == from.m_orientation_max_degrees);
                        PPR_TEST_ASSERT(
                            to.m_radius_min_m == from.m_radius_min_m and to.m_radius_max_m == from.m_radius_max_m);
                        PPR_TEST_ASSERT(
                            to.m_stretch_min == from.m_stretch_min and to.m_stretch_max == from.m_stretch_max);
                        PPR_TEST_ASSERT(
                            to.m_distortion == from.m_distortion and to.m_core_coverage == from.m_core_coverage);
                        PPR_TEST_ASSERT(
                            to.m_fringe_coverage == from.m_fringe_coverage and
                            to.m_sprinkle_probability == from.m_sprinkle_probability);
                    }
                }
                for (std::size_t i = 0u; i < catalog->findWorld(profile)->m_rules.size(); ++i) {
                    const auto *const from = std::get_if<WorldWormRule>(
                        &catalog->findWorld(profile)->m_rules[i].m_parameters);
                    if (from == nullptr) {
                        continue;
                    }
                    const auto &to = std::get<sim::GenerationWorm>(plan->recipe().m_rules[i].m_parameters);
                    PPR_TEST_ASSERT(
                        to.m_turn_smoothing == from->m_turn_smoothing and
                        to.m_radius_variation == from->m_radius_variation);
                    PPR_TEST_ASSERT(
                        to.m_chamber_probability == from->m_chamber_probability and
                        to.m_chamber_scale == from->m_chamber_scale);
                }
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
        PPR_TEST_ASSERT(changed->recipe().m_materials.size() == 64u);
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
            {"materials.toml", "texture_sampling = \"repeat\"", "texture_sampling = \"stochastic\""},
            {"backgrounds.toml", "parallax_factor = 0.08", "parallax_factor = 0.09"},
            {"biomes.toml", "293.0", "294.0"},
            {"biomes.toml", "bed_period_m = 48", "bed_period_m = 49"},
            {"biomes.toml", "pore_probability = 0.055", "pore_probability = 0.056"},
            {"biomes.toml", "pore_passes = 3", "pore_passes = 2"},
            {"biomes.toml", "erosion_passes = 8", "erosion_passes = 7"},
            {"biomes.toml", "finish_adoption = 0.12", "finish_adoption = 0.13"},
            {"biomes.toml", "formation_probability = 0.045", "formation_probability = 0.046"},
            {"worlds.toml", "surface_amplitude_m = 55.0", "surface_amplitude_m = 56.0"},
            {"worlds.toml", "bedrock_variation_m = 12.5", "bedrock_variation_m = 12.4"},
            {"worlds.toml", "start_depth_m = 65.0", "start_depth_m = 66.0"},
            {"worlds.toml", "natural_anchors = 28", "natural_anchors = 29"},
            {"worlds.toml", "roughness_m = 4.5", "roughness_m = 4.4"},
            {"biomes.toml", "relaxation_passes = 8", "relaxation_passes = 9"},
            {"biomes.toml", "sprinkle_probability = 0.08", "sprinkle_probability = 0.09"},
            {"biomes.toml", "relaxation_adoption = 0.32", "relaxation_adoption = 0.33"},
            {"biomes.toml", "stage = \"bulk\"", "stage = \"detail\""},
            {"biomes.toml", "blob_count = 48", "blob_count = 47"},
            {"biomes.toml", "radius_min_m = 8.0", "radius_min_m = 7.0"},
            {"biomes.toml", "radius_max_m = 13.0", "radius_max_m = 14.0"},
            {"biomes.toml", "stretch_min = 1.1", "stretch_min = 1.2"},
            {"biomes.toml", "stretch_max = 2.5", "stretch_max = 2.6"},
            {"biomes.toml", "distortion = 0.28", "distortion = 0.27"},
            {"biomes.toml", "orientation_min_degrees = 0.0", "orientation_min_degrees = 1.0"},
            {"biomes.toml", "orientation_max_degrees = 360.0", "orientation_max_degrees = 359.0"},
            {"biomes.toml", "core_coverage = 0.95", "core_coverage = 0.94"},
            {"biomes.toml", "fringe_coverage = 0.22", "fringe_coverage = 0.21"},
            {"biomes.toml", "sprinkle_probability = 0.004", "sprinkle_probability = 0.005"},
            {"worlds.toml", "turn_smoothing = 0.82", "turn_smoothing = 0.83"},
            {"worlds.toml", "radius_variation = 0.35", "radius_variation = 0.34"},
            {"worlds.toml", "chamber_probability = 0.025", "chamber_probability = 0.026"},
            {"worlds.toml", "chamber_scale = 2.2", "chamber_scale = 2.3"},
            {"features.toml", "Hearth landing", "Hearth landing revised"},
            {"worlds.toml", "site_spacing_m = 110.0", "site_spacing_m = 111.0"},
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

    PPR_UNIT_TEST (authoring_all_definition_categories_round_trip_and_multidocument_save_retains_newer_edits) {
        Fixture fixture;
        useSmallIdentityImages(fixture);
        config::Diagnostics diagnostics{64u};
        auto opened = ContentAuthoringSession::open(fixture.m_root, diagnostics);
        PPR_TEST_ASSERT(opened.has_value());
        auto session = std::move(*opened);
        const auto original = session.draftSnapshot();
        constexpr std::array<std::string_view, kContentDocumentCount> keys{
            "basalt", "hearth", "composition", "landing", "frontier", "frontier"};
        for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
            const auto kind = static_cast<EContentDocument>(index);
            PPR_TEST_ASSERT(not session.cloneDefinition(
                kind, keys[index], "workshop_copy",
                kind == EContentDocument::materials ? std::optional<u16>{static_cast<u16>(60000u)} : std::nullopt,
                diagnostics));
            const auto cloned = session.draftSnapshot();
            PPR_TEST_ASSERT(
                cloned.m_source.m_documents[index].m_bytes.starts_with(original.m_source.m_documents[index].m_bytes));
            PPR_TEST_ASSERT(session.undo().value());
            PPR_TEST_ASSERT(
                session.draftSource().m_documents[index].m_bytes == original.m_source.m_documents[index].m_bytes);
            PPR_TEST_ASSERT(session.redo().value());
        }
        const auto all_cloned = session.draftSnapshot();
        PPR_TEST_ASSERT(validateContentDraft(all_cloned, diagnostics).has_value());
        for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
            const auto kind = static_cast<EContentDocument>(index);
            PPR_TEST_ASSERT(not session.deleteDefinition(kind, "workshop_copy", diagnostics));
            const auto removed = session.draftSnapshot();
            PPR_TEST_ASSERT(session.undo().value());
            PPR_TEST_ASSERT(
                session.draftSource().m_documents[index].m_bytes == all_cloned.m_source.m_documents[index].m_bytes);
            PPR_TEST_ASSERT(session.redo().value());
            PPR_TEST_ASSERT(
                session.draftSource().m_documents[index].m_bytes == removed.m_source.m_documents[index].m_bytes);
        }
        PPR_TEST_ASSERT(validateContentDraft(session.draftSnapshot(), diagnostics).has_value());
        session.revert();
        PPR_TEST_ASSERT(
            session.cloneDefinition(
                EContentDocument::materials, "basalt", "duplicate_id", static_cast<u16>(16u), diagnostics) ==
            config::EConfigErrc::duplicate_key);
        PPR_TEST_ASSERT(
            session.editDefinition(
                EContentDocument::materials, "basalt", "numeric_id", config::FileValue::integer(18), diagnostics) ==
            std::errc::invalid_argument);
        const auto referenced_field = session.validatedCatalog()->biomes().front().m_temperature_field;
        PPR_TEST_ASSERT(not session.deleteDefinition(EContentDocument::fields, referenced_field, diagnostics));
        config::Diagnostics references;
        PPR_TEST_ASSERT(not validateContentDraft(session.draftSnapshot(), references) and references.hasErrors());
        PPR_TEST_ASSERT(not saveContentDraft(session.draftSnapshot(), references));
        PPR_TEST_ASSERT(session.dirty() and fixture.read("fields.toml") == original.m_source.m_documents[2u].m_bytes);
        PPR_TEST_ASSERT(session.undo().value() and not session.dirty());

        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::materials, "basalt", "roughness", config::FileValue::floating(0.5), diagnostics));
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::fields, "composition", "frequency_per_metre", config::FileValue::floating(0.06),
            diagnostics));
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::worlds, "frontier", "display_name", config::FileValue::text("Edited profile"),
            diagnostics));
        const auto submitted = session.draftSnapshot();
        // Staging collisions are discovered before any document commits.
        fixture.write("worlds.toml.workshop.tmp", "another owner");
        PPR_TEST_ASSERT(not saveContentDraft(submitted, diagnostics));
        PPR_TEST_ASSERT(fixture.read("materials.toml") == original.m_source.m_documents[0u].m_bytes);
        PPR_TEST_ASSERT(fixture.read("fields.toml") == original.m_source.m_documents[2u].m_bytes);
        PPR_TEST_ASSERT(fixture.read("worlds.toml.workshop.tmp") == "another owner");
        std::filesystem::remove(fixture.m_root / "worlds.toml.workshop.tmp");
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::fields, "composition", "frequency_per_metre", config::FileValue::floating(0.07),
            diagnostics));
        const auto newer = session.draftSnapshot();
        auto saved = saveContentDraft(submitted, diagnostics);
        PPR_TEST_ASSERT(saved.has_value() and not session.acceptSave(std::move(*saved)));
        PPR_TEST_ASSERT(session.dirty());
        PPR_TEST_ASSERT(fixture.read("materials.toml") == submitted.m_source.m_documents[0u].m_bytes);
        PPR_TEST_ASSERT(fixture.read("fields.toml") == submitted.m_source.m_documents[2u].m_bytes);
        PPR_TEST_ASSERT(fixture.read("worlds.toml") == submitted.m_source.m_documents[4u].m_bytes);
        PPR_TEST_ASSERT(session.draftSource().m_documents[2u].m_bytes == newer.m_source.m_documents[2u].m_bytes);
        auto retry = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(retry.has_value() and session.acceptSave(std::move(*retry)) and not session.dirty());
    };

    PPR_UNIT_TEST (authoring_partial_commit_reconciles_baseline_preserves_newer_edits_and_undo_intent) {
#ifdef _WIN32
        Fixture fixture;
        useSmallIdentityImages(fixture);
        config::Diagnostics diagnostics{64u};
        auto opened = ContentAuthoringSession::open(fixture.m_root, diagnostics);
        PPR_TEST_ASSERT(opened.has_value());
        auto session = std::move(*opened);
        const auto catalog = session.validatedCatalog();
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::materials, "basalt", "roughness", config::FileValue::floating(0.5), diagnostics));
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::fields, "composition", "frequency_per_metre", config::FileValue::floating(0.06),
            diagnostics));
        const auto submitted = session.draftSnapshot();
        // MSVC file streams share reading/writing but not deletion: reading and
        // staging succeed while the second document's replacement is denied.
        std::ifstream locked{fixture.m_root / "fields.toml", std::ios::binary};
        PPR_TEST_ASSERT(locked.good());
        auto partial = saveContentDraft(submitted, diagnostics);
        PPR_TEST_ASSERT(partial.has_value() and partial->m_error != std::error_code{} and not partial->m_catalog);
        PPR_TEST_ASSERT(fixture.read("materials.toml") == submitted.m_source.m_documents[0u].m_bytes);
        PPR_TEST_ASSERT(fixture.read("fields.toml") == submitted.m_baseline.m_documents[2u].m_bytes);
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::materials, "basalt", "roughness", config::FileValue::floating(0.6), diagnostics));
        const auto newer = session.draftSnapshot();
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*partial)));
        PPR_TEST_ASSERT(session.dirty() and session.validatedCatalog() == catalog);
        PPR_TEST_ASSERT(session.baselineSource().m_documents[0u].m_bytes == submitted.m_source.m_documents[0u].m_bytes);
        PPR_TEST_ASSERT(session.draftSource().m_documents[0u].m_bytes == newer.m_source.m_documents[0u].m_bytes);
        locked.close();
        auto retry = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(retry.has_value() and not retry->m_error and session.acceptSave(std::move(*retry)));
        PPR_TEST_ASSERT(not session.dirty());

        const auto before_undo = session.draftSnapshot();
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::materials, "basalt", "roughness", config::FileValue::floating(0.7), diagnostics));
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::fields, "composition", "frequency_per_metre", config::FileValue::floating(0.08),
            diagnostics));
        locked.open(fixture.m_root / "fields.toml", std::ios::binary);
        PPR_TEST_ASSERT(locked.good());
        auto undo_partial = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(undo_partial.has_value() and undo_partial->m_error != std::error_code{});
        PPR_TEST_ASSERT(session.undo().value() and session.undo().value());
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*undo_partial)) and session.dirty());
        PPR_TEST_ASSERT(session.draftSource().m_documents[0u].m_bytes == before_undo.m_source.m_documents[0u].m_bytes);
        PPR_TEST_ASSERT(
            session.baselineSource().m_documents[0u].m_bytes != before_undo.m_source.m_documents[0u].m_bytes);
        locked.close();
        auto restore = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(restore.has_value() and not restore->m_error and session.acceptSave(std::move(*restore)));
        PPR_TEST_ASSERT(
            not session.dirty() and fixture.read("materials.toml") == before_undo.m_source.m_documents[0u].m_bytes);
#endif
    };

    PPR_UNIT_TEST (authoring_drafts_validate_independently_and_save_without_losing_newer_edits) {
        Fixture fixture;
        useSmallIdentityImages(fixture);
        const auto original = fixture.read("biomes.toml");
        config::Diagnostics diagnostics{64u};
        auto opened = ContentAuthoringSession::open(fixture.m_root, diagnostics);
        PPR_TEST_ASSERT(opened.has_value());
        auto session = std::move(*opened);
        const auto baseline_catalog = session.validatedCatalog();
        const auto baseline_revision = session.validatedRevision();
        PPR_TEST_ASSERT(not session.dirty() and not session.canUndo());
        constexpr auto materials = static_cast<std::size_t>(EContentDocument::materials);
        const auto original_materials = session.draftSource().m_documents[materials].m_bytes;
        PPR_TEST_ASSERT(
            not session.clearDefinitionProperty(EContentDocument::materials, "basalt", "normal_texture", diagnostics));
        const auto cleared_materials = session.draftSource().m_documents[materials].m_bytes;
        PPR_TEST_ASSERT(cleared_materials != original_materials and session.dirty() and session.canUndo());
        const auto cleared = config::decode(
            cleared_materials, "cleared.toml", diagnostics, config::EDecodeMode::records);
        PPR_TEST_ASSERT(cleared.has_value());
        PPR_TEST_ASSERT(
            std::ranges::find(cleared->m_entries, "materials.basalt.normal_texture", &config::Entry::m_key) ==
            cleared->m_entries.end());
        const auto clear_revision = session.revision();
        PPR_TEST_ASSERT(
            not session.clearDefinitionProperty(EContentDocument::materials, "basalt", "normal_texture", diagnostics));
        PPR_TEST_ASSERT(session.revision() == clear_revision);
        const auto required_error = session.clearDefinitionProperty(
            EContentDocument::materials, "basalt", "numeric_id", diagnostics);
        PPR_TEST_ASSERT(required_error != std::error_code{} and session.revision() == clear_revision);
        diagnostics.clear();
        PPR_TEST_ASSERT(session.undo().value() and not session.dirty());
        PPR_TEST_ASSERT(session.draftSource().m_documents[materials].m_bytes == original_materials);
        PPR_TEST_ASSERT(session.redo().value());
        PPR_TEST_ASSERT(session.draftSource().m_documents[materials].m_bytes == cleared_materials);
        PPR_TEST_ASSERT(session.undo().value() and not session.dirty());
        PPR_TEST_ASSERT(session.validatedCatalog() == baseline_catalog);
        PPR_TEST_ASSERT(
            not session.editBiome("hearth", "temperature_base_k", config::FileValue::floating(300.0), diagnostics));
        const auto valid_snapshot = session.draftSnapshot();
        PPR_TEST_ASSERT(session.dirty() and session.revision() != baseline_revision);
        PPR_TEST_ASSERT(
            session.validatedRevision() == baseline_revision and session.validatedCatalog() == baseline_catalog);
        PPR_TEST_ASSERT(fixture.read("biomes.toml") == original);

        PPR_TEST_ASSERT(not session.editBiome(
            "hearth", "temperature_field", config::FileValue::text("missing-field"), diagnostics));
        config::Diagnostics invalid_diagnostics{64u};
        const auto invalid = validateContentDraft(session.draftSnapshot(), invalid_diagnostics);
        PPR_TEST_ASSERT(not invalid and invalid_diagnostics.hasErrors());
        PPR_TEST_ASSERT(session.validatedCatalog() == baseline_catalog);
        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(
            session.draftSource().m_documents[static_cast<std::size_t>(EContentDocument::biomes)].m_bytes ==
            valid_snapshot.m_source.m_documents[static_cast<std::size_t>(EContentDocument::biomes)].m_bytes);
        PPR_TEST_ASSERT(not session.acceptValidation(valid_snapshot.m_revision, baseline_catalog));
        PPR_TEST_ASSERT(session.redo().value() and session.undo().value());
        auto validated = validateContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(validated.has_value());
        PPR_TEST_ASSERT((*validated)->contentFingerprint() != baseline_catalog->contentFingerprint());
        PPR_TEST_ASSERT(session.acceptValidation(session.revision(), *validated));

        auto save_snapshot = session.draftSnapshot();
        PPR_TEST_ASSERT(
            not session.editBiome("hearth", "temperature_base_k", config::FileValue::floating(301.0), diagnostics));
        auto saved = saveContentDraft(std::move(save_snapshot), diagnostics);
        PPR_TEST_ASSERT(saved.has_value());
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*saved)) and session.dirty());
        auto newer_save = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(newer_save.has_value() and session.acceptSave(std::move(*newer_save)));
        PPR_TEST_ASSERT(not session.dirty());
        auto reopened = ContentAuthoringSession::open(fixture.m_root, diagnostics);
        PPR_TEST_ASSERT(reopened.has_value());
        PPR_TEST_ASSERT(
            reopened->validatedCatalog()->contentFingerprint() == session.validatedCatalog()->contentFingerprint());
        PPR_TEST_ASSERT(session.undo().value() and session.dirty());
        session.revert();
        PPR_TEST_ASSERT(not session.dirty() and not session.canUndo() and not session.canRedo());

        PPR_TEST_ASSERT(
            not session.editBiome("hearth", "temperature_base_k", config::FileValue::floating(301.5), diagnostics));
        const auto before_external_fields = session.draftSnapshot();
        auto old_validation = validateContentDraft(before_external_fields, diagnostics);
        PPR_TEST_ASSERT(old_validation.has_value());
        PPR_TEST_ASSERT(session.acceptValidation(before_external_fields.m_revision, *old_validation));
        auto changed_fields = fixture.read("fields.toml");
        replace(changed_fields, "frequency_per_metre = 0.008", "frequency_per_metre = 0.01");
        fixture.write("fields.toml", changed_fields);
        auto save_with_external_fields = saveContentDraft(before_external_fields, diagnostics);
        PPR_TEST_ASSERT(save_with_external_fields.has_value());
        PPR_TEST_ASSERT(session.acceptSave(std::move(*save_with_external_fields)));
        PPR_TEST_ASSERT(not session.dirty() and session.revision() != before_external_fields.m_revision);
        PPR_TEST_ASSERT(session.validatedRevision() == session.revision());
        PPR_TEST_ASSERT(session.validatedCatalog()->contentFingerprint() != (*old_validation)->contentFingerprint());
        PPR_TEST_ASSERT(
            session.draftSource().m_documents[static_cast<std::size_t>(EContentDocument::fields)].m_bytes ==
            changed_fields);
        PPR_TEST_ASSERT(not session.acceptValidation(before_external_fields.m_revision, *old_validation));

        const auto before_external_texture = session.draftSnapshot();
        const auto before_texture_catalog = session.validatedCatalog();
        std::filesystem::copy_file(
            fixture.m_root / "textures/ice.png", fixture.m_root / "textures/basalt.png",
            std::filesystem::copy_options::overwrite_existing);
        auto save_with_external_texture = saveContentDraft(before_external_texture, diagnostics);
        PPR_TEST_ASSERT(save_with_external_texture.has_value());
        PPR_TEST_ASSERT(session.acceptSave(std::move(*save_with_external_texture)));
        PPR_TEST_ASSERT(not session.dirty() and session.revision() != before_external_texture.m_revision);
        PPR_TEST_ASSERT(session.validatedRevision() == session.revision());
        PPR_TEST_ASSERT(
            session.validatedCatalog()->contentFingerprint() != before_texture_catalog->contentFingerprint());
        PPR_TEST_ASSERT(
            session.draftSource().m_documents[static_cast<std::size_t>(EContentDocument::biomes)].m_bytes ==
            before_external_texture.m_source.m_documents[static_cast<std::size_t>(EContentDocument::biomes)].m_bytes);
        PPR_TEST_ASSERT(not session.acceptValidation(before_external_texture.m_revision, before_texture_catalog));

        const auto committed = fixture.read("biomes.toml");
        PPR_TEST_ASSERT(
            not session.editBiome("hearth", "temperature_base_k", config::FileValue::floating(302.0), diagnostics));
        const auto retained = session.draftSnapshot();
        fixture.write("biomes.toml", committed + "\n# external edit\n");
        const auto conflicting = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(not conflicting and conflicting.error() == std::errc::resource_unavailable_try_again);
        PPR_TEST_ASSERT(session.dirty() and session.revision() == retained.m_revision);
        PPR_TEST_ASSERT(fixture.read("biomes.toml") == committed + "\n# external edit\n");
        fixture.write("biomes.toml", committed);

        fixture.write("biomes.toml.workshop.tmp", "another owner's staging file");
        const auto collision = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(not collision and collision.error() == std::errc::io_error);
        PPR_TEST_ASSERT(fixture.read("biomes.toml.workshop.tmp") == "another owner's staging file");
        PPR_TEST_ASSERT(fixture.read("biomes.toml") == committed and session.dirty());
        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = saveContentDraft(session.draftSnapshot(), diagnostics, job_context.get());
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        PPR_TEST_ASSERT(fixture.read("biomes.toml") == committed and session.dirty());
    };

    PPR_UNIT_TEST (workshop_queued_requests_preserve_newer_drafts_and_require_current_discard_authority) {
        struct Clock final : ITimerClock {
            TimePoint m_now{};
            TimePoint now() const noexcept override {
                return m_now;
            }
        } clock;
        Fixture fixture{};
        useSmallIdentityImages(fixture);
        ParallelScheduler pool{"Workshop request contracts", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldgenWorkshop workshop{pool, clock};
        const auto settle = [&]() {
            while (workshop.busy()) {
                PPR_TEST_ASSERT(not workshop.update());
                std::this_thread::yield();
            }
        };
        // Queuing never starts I/O. A replacement supersedes an invalid root,
        // and cancellation removes both before the first admission update.
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root / "missing"));
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root));
        PPR_TEST_ASSERT(workshop.busy() and workshop.session() == nullptr);
        workshop.cancel();
        PPR_TEST_ASSERT(not workshop.busy());
        PPR_TEST_ASSERT(not workshop.update() and workshop.session() == nullptr);
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root / "missing"));
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root));
        settle();
        auto *const session = workshop.session();
        PPR_TEST_ASSERT(session != nullptr and not session->dirty());
        PPR_TEST_ASSERT(session->root() == std::filesystem::canonical(fixture.m_root));
        const auto catalog = session->validatedCatalog();
        const u64 validated_revision = session->validatedRevision();
        const auto original = session->draftSnapshot();
        constexpr auto biomes = static_cast<std::size_t>(EContentDocument::biomes);
        config::Diagnostics diagnostics;
        PPR_TEST_ASSERT(
            not session->editBiome("hearth", "temperature_base_k", config::FileValue::floating(301.0), diagnostics));
        const auto validation_input = session->draftSnapshot();
        PPR_TEST_ASSERT(not workshop.validate());
        // The immutable request has revisionR, but admission has not run yet.
        PPR_TEST_ASSERT(
            not session->editBiome("hearth", "temperature_base_k", config::FileValue::floating(302.0), diagnostics));
        const auto newer = session->draftSnapshot();
        PPR_TEST_ASSERT(newer.m_revision > validation_input.m_revision);
        settle();
        PPR_TEST_ASSERT(workshop.session() == session and session->revision() == newer.m_revision);
        PPR_TEST_ASSERT(session->validatedRevision() == validated_revision and session->validatedCatalog() == catalog);
        PPR_TEST_ASSERT(
            session->draftSource().m_documents[biomes].m_bytes == newer.m_source.m_documents[biomes].m_bytes);
        PPR_TEST_ASSERT(session->dirty() and session->canUndo() and not session->canRedo());

        PPR_TEST_ASSERT(workshop.open(fixture.m_root) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(not workshop.busy() and session->revision() == newer.m_revision);
        PPR_TEST_ASSERT(
            session->draftSource().m_documents[biomes].m_bytes == newer.m_source.m_documents[biomes].m_bytes);
        PPR_TEST_ASSERT(session->canUndo() and not session->canRedo());
        const auto undo = session->undo();
        PPR_TEST_ASSERT(undo.has_value() and *undo);
        PPR_TEST_ASSERT(
            session->draftSource().m_documents[biomes].m_bytes ==
            validation_input.m_source.m_documents[biomes].m_bytes);
        const auto redo = session->redo();
        PPR_TEST_ASSERT(redo.has_value() and *redo);
        PPR_TEST_ASSERT(
            session->draftSource().m_documents[biomes].m_bytes == newer.m_source.m_documents[biomes].m_bytes);

        const u64 discard_revision = session->revision();
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root, discard_revision));
        PPR_TEST_ASSERT(
            not session->editBiome("hearth", "temperature_base_k", config::FileValue::floating(303.0), diagnostics));
        const auto after_discard_authorization = session->draftSnapshot();
        PPR_TEST_ASSERT(after_discard_authorization.m_revision > discard_revision);
        settle();
        PPR_TEST_ASSERT(
            workshop.session() == session and session->revision() == after_discard_authorization.m_revision);
        PPR_TEST_ASSERT(
            session->draftSource().m_documents[biomes].m_bytes ==
            after_discard_authorization.m_source.m_documents[biomes].m_bytes);
        PPR_TEST_ASSERT(session->dirty() and session->canUndo() and not session->canRedo());
        PPR_TEST_ASSERT(session->validatedRevision() == validated_revision and session->validatedCatalog() == catalog);
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root, session->revision()));
        settle();
        const auto *const reopened = workshop.session();
        PPR_TEST_ASSERT(reopened != nullptr and not reopened->dirty());
        PPR_TEST_ASSERT(reopened->validatedRevision() == reopened->revision());
        PPR_TEST_ASSERT(not reopened->canUndo() and not reopened->canRedo());
        PPR_TEST_ASSERT(
            reopened->draftSource().m_documents[biomes].m_bytes == original.m_source.m_documents[biomes].m_bytes);
        PPR_TEST_ASSERT(reopened->validatedCatalog()->contentFingerprint() == catalog->contentFingerprint());
        WorldgenPreviewSpec spec{"frontier", "hearth", 42u, reopened->revision()};
        workshop.configureLivePreview(true, spec, pP::EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not workshop.busy());
        clock.m_now += std::chrono::milliseconds{349};
        workshop.configureLivePreview(true, spec, pP::EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not workshop.update() and not workshop.busy());
        clock.m_now += std::chrono::milliseconds{1};
        PPR_TEST_ASSERT(not workshop.update() and workshop.busy());
        workshop.cancel();
        settle();
        PPR_TEST_ASSERT(workshop.preview() == nullptr);
        clock.m_now += std::chrono::seconds{1};
        workshop.configureLivePreview(true, spec, pP::EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not workshop.update() and not workshop.busy());
        // Toggling re-arms the same identity. An explicit preview satisfies the
        // pending automatic request, so the elapsed deadline cannot duplicate it.
        workshop.configureLivePreview(false, spec, pP::EGridDiagnosticMode::material);
        workshop.configureLivePreview(true, spec, pP::EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not workshop.requestPreview(spec, pP::EGridDiagnosticMode::material));
        clock.m_now += std::chrono::seconds{1};
        PPR_TEST_ASSERT(not workshop.update() and workshop.busy());
        settle();
        PPR_TEST_ASSERT(workshop.preview() != nullptr and workshop.preview()->m_spec.m_seed == spec.m_seed);
        PPR_TEST_ASSERT(not workshop.update() and not workshop.busy());
        ++spec.m_seed;
        workshop.configureLivePreview(true, spec, pP::EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root));
        clock.m_now += std::chrono::seconds{1};
        settle();
        PPR_TEST_ASSERT(not workshop.update() and not workshop.busy());
        auto *const qa_session = workshop.session();
        PPR_TEST_ASSERT(qa_session != nullptr);
        const auto hearth = std::ranges::find(
            qa_session->validatedCatalog()->biomes(), "hearth", &BiomeDefinition::m_key);
        PPR_TEST_ASSERT(hearth != qa_session->validatedCatalog()->biomes().end() and not hearth->m_composition.empty());
        const std::string bed_dip = "composition." + hearth->m_composition.front().m_key + ".bed_dip";
        workshop.configureLivePreview(false, spec, pP::EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not qa_session->editBiome("hearth", bed_dip, config::FileValue::floating(1.0), diagnostics));
        PPR_TEST_ASSERT(not workshop.validate());
        settle();
        const auto property_diagnostics = workshop.propertyDiagnostics();
        PPR_TEST_ASSERT(not property_diagnostics.empty());
        PPR_TEST_ASSERT(std::ranges::any_of(property_diagnostics, [&](const auto &diagnostic) {
            return diagnostic.m_property == "biomes.hearth." + bed_dip and not diagnostic.m_message.empty();
        }));
        spec.m_revision = qa_session->revision();
        workshop.configureLivePreview(true, spec, pP::EGridDiagnosticMode::material);
        clock.m_now += std::chrono::seconds{1};
        PPR_TEST_ASSERT(not workshop.update() and not workshop.busy());
        PPR_TEST_ASSERT(qa_session->undo().value());
        PPR_TEST_ASSERT(workshop.propertyDiagnostics().empty());
        workshop.configureLivePreview(false, spec, pP::EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not workshop.validate());
        settle();
        PPR_TEST_ASSERT(workshop.propertyDiagnostics().empty());
        PPR_TEST_ASSERT(not workshop.shutdown());
    };

    PPR_UNIT_TEST (workshop_property_metadata_tracks_schemas_and_contextual_references) {
        std::array<config::Document, kContentDocumentCount> documents;
        const auto descriptor = [&](const EContentDocument category, const std::string_view property) {
            return contentPropertyDescriptor(
                category, property, documents[static_cast<std::size_t>(category)], "frontier");
        };
        const auto probability = descriptor(EContentDocument::biomes, "formation_probability");
        PPR_TEST_ASSERT(probability and probability->m_min == 0.0 and probability->m_max == 0.2);
        const auto phase = descriptor(EContentDocument::materials, "phase");
        PPR_TEST_ASSERT(phase and phase->m_choices.size() == 3u);
        PPR_TEST_ASSERT(not phase->m_optional);
        PPR_TEST_ASSERT(std::ranges::find(phase->m_choices, "liquid") != phase->m_choices.end());
        const auto octaves = descriptor(EContentDocument::fields, "octaves");
        PPR_TEST_ASSERT(octaves and octaves->m_min == 1.0 and octaves->m_max == 16.0);
        auto &fields = documents[static_cast<std::size_t>(EContentDocument::fields)];
        fields.m_entries = {
            {"fields.frontier.frequency_per_metre", config::FileValue::floating(1.0e20), "metadata.toml:1"}};
        const auto frequency = descriptor(EContentDocument::fields, "frequency_per_metre");
        PPR_TEST_ASSERT(frequency and frequency->m_exclusive_min and *frequency->m_max > 1.0e20);
        PPR_TEST_ASSERT(std::isfinite(frequency->m_slider_max) and frequency->m_slider_max < 1.0e20);
        PPR_TEST_ASSERT(frequency->m_slider_max == 0.1 and frequency->m_units == "1/m");
        PPR_TEST_ASSERT(fields.m_entries.front().m_value.m_floating == 1.0e20);
        const auto band = descriptor(EContentDocument::biomes, "composition.lower.bed_dip");
        PPR_TEST_ASSERT(band and band->m_min == -0.5 and band->m_max == 0.5);
        const auto weight = descriptor(EContentDocument::biomes, "composition.lower.materials.rock.weight");
        PPR_TEST_ASSERT(weight and weight->m_exclusive_min and weight->m_min == 0.0);
        PPR_TEST_ASSERT(std::isfinite(weight->m_slider_max) and weight->m_slider_max < *weight->m_max);
        const auto shape = descriptor(EContentDocument::biomes, "deposits.ore.shape");
        PPR_TEST_ASSERT(shape and std::ranges::find(shape->m_choices, "blob") != shape->m_choices.end());
        const auto operation = descriptor(EContentDocument::features, "operations.clear.shape");
        PPR_TEST_ASSERT(operation and operation->m_choices.size() == 3u);
        PPR_TEST_ASSERT(std::ranges::find(operation->m_choices, "capsule") != operation->m_choices.end());
        const auto layer = descriptor(EContentDocument::backgrounds, "layers.far.parallax_factor");
        PPR_TEST_ASSERT(layer and layer->m_min == 0.0 and layer->m_max == 1.0);
        const auto material = descriptor(EContentDocument::biomes, "soil_material");
        PPR_TEST_ASSERT(material and material->m_reference_category == EContentDocument::materials);
        PPR_TEST_ASSERT(material->m_optional);
        PPR_TEST_ASSERT(not descriptor(EContentDocument::biomes, "composition.lower.unknown"));
        auto &worlds = documents[static_cast<std::size_t>(EContentDocument::worlds)];
        worlds.m_entries = {
            {"worlds.frontier.start_feature", config::FileValue::text("start"), "metadata.toml:1"},
            {"worlds.frontier.biomes.home.biome", config::FileValue::text("hearth"), "metadata.toml:2"},
            {"worlds.frontier.rules.cave.kind", config::FileValue::text("threshold_cave"), "metadata.toml:3"},
            {"worlds.frontier.rules.pocket.kind", config::FileValue::text("feature"), "metadata.toml:4"},
            {"worlds.frontier.rules.pocket.feature", config::FileValue::text("sealed"), "metadata.toml:5"},
            {"worlds.frontier.rules.fill.kind", config::FileValue::text("matter_fill"), "metadata.toml:6"},
            {"worlds.frontier.rules.fill.feature_rule", config::FileValue::text("pocket"), "metadata.toml:7"}};
        auto &features = documents[static_cast<std::size_t>(EContentDocument::features)];
        features.m_entries = {
            {"features.start.markers.spawn.role", config::FileValue::text("start"), "metadata.toml:8"},
            {"features.sealed.markers.liquid.role", config::FileValue::text("sealed_pocket"), "metadata.toml:9"}};
        PPR_TEST_ASSERT(descriptor(EContentDocument::worlds, "rules.cave.threshold_min").has_value());
        const auto kind = descriptor(EContentDocument::worlds, "rules.cave.kind");
        PPR_TEST_ASSERT(kind and kind->m_read_only and kind->m_choices.size() == 6u);
        PPR_TEST_ASSERT(not kind->m_description.empty());
        PPR_TEST_ASSERT(std::ranges::find(kind->m_choices, "matter_fill") != kind->m_choices.end());
        PPR_TEST_ASSERT(not descriptor(EContentDocument::worlds, "rules.cave.radius_min_m"));
        worlds.m_entries[2u].m_value = config::FileValue::text("carve_worm");
        const auto radius = descriptor(EContentDocument::worlds, "rules.cave.radius_min_m");
        PPR_TEST_ASSERT(radius and radius->m_min == 0.0 and radius->m_max == 64.0 and radius->m_exclusive_min);
        PPR_TEST_ASSERT(radius->m_units == "m");
        const auto choices = [&](const std::string_view property) {
            return contentPropertyReferenceChoices(EContentDocument::worlds, property, documents, "frontier");
        };
        PPR_TEST_ASSERT(choices("rules.cave.biome") == Array<std::string>{"hearth"});
        PPR_TEST_ASSERT(choices("rules.fill.feature_rule") == Array<std::string>{"pocket"});
        PPR_TEST_ASSERT(choices("rules.fill.marker") == Array<std::string>{"liquid"});
        PPR_TEST_ASSERT(choices("start_marker") == Array<std::string>{"spawn"});
        worlds.m_entries[6u].m_value = config::FileValue::text("missing");
        PPR_TEST_ASSERT(choices("rules.fill.marker").empty());
        const auto lifetime = operation->m_choices;
        documents = {};
        PPR_TEST_ASSERT(lifetime.size() == 3u and lifetime[0u] == "rectangle");
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
                    detail::pack::required_texture_limits_missing_and_cancel_are_errors,
                    detail::pack::workshop_property_metadata_tracks_schemas_and_contextual_references,
                    detail::pack::authoring_drafts_validate_independently_and_save_without_losing_newer_edits,
                    detail::pack::authoring_partial_commit_reconciles_baseline_preserves_newer_edits_and_undo_intent,
                    detail::pack::
                        authoring_all_definition_categories_round_trip_and_multidocument_save_retains_newer_edits,
                    detail::pack::
                        workshop_queued_requests_preserve_newer_drafts_and_require_current_discard_authority});
        };
        return tests;
    }
}
