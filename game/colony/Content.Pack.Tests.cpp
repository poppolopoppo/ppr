module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <cstdio>

module game.tests;

import engine.asset;
import engine.app;
import engine.core;
import engine.physics;
import engine.sim;
import engine.image;
import game.colony.content;
import game.colony.content.authoring;
import game.colony.worldgen;
import game.colony.worldgen_preview;
import game.colony.digtool;
import game.colony.buildings;
import game.colony.agents;
import game.colony.pathfinding;
import game.application;
import std;

namespace pP::game::tests::detail::pack {
    using namespace colony;

    PPR_DEFINE_LOG_CATEGORY(PackContentTests, debug, none);

    // ------------------------------------------------------------------
    // small owned descriptor pack
    // ------------------------------------------------------------------

    constexpr std::string_view kWorld = "ppr.test:frontier";

    struct Fixture {
        std::filesystem::path m_root;
        std::string m_manifest{"schema_version = 1\npack_id = \"ppr.test\"\n"};

        Fixture() {
            static std::atomic<u32> ordinal{};
            const auto parent = hal::process::currentExecutablePath().parent_path();

            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                const auto candidate = parent / ("worldgen-pack-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(candidate)) {
                    m_root = std::filesystem::canonical(candidate);
                    break;
                }
            }

            PPR_TEST_ASSERT(not m_root.empty());

            sim::asset::ElementDescriptor element;
            element.m_schema_version = 1u;
            element.m_density = 2000.0;
            element.m_phase = sim::EGenerationMatter::solid;
            add("solid", sim::asset::kElementKind, element);

            element.m_phase = sim::EGenerationMatter::liquid;
            element.m_density = 1000.0;
            add("liquid", sim::asset::kElementKind, element);

            image::TextureDescriptor texture;
            texture.m_schema_version = 1u;
            texture.m_source = "images/color.png";
            add("albedo", image::kTextureAssetKind, texture);

            texture.m_interpretation = image::ETextureInterpretation::normal;
            add("normal", image::kTextureAssetKind, texture);

            MaterialAssetDescriptor material;
            material.m_schema_version = 1u;
            material.m_element = "solid";
            material.m_diggable = true;
            material.m_appearance.m_display_name = "Basalt";
            material.m_appearance.m_color_rgba = "808080ff";
            material.m_appearance.m_albedo_texture = "albedo";
            material.m_appearance.m_normal_texture = "normal";
            material.m_appearance.m_texture_period_m = 1.0;
            add("basalt", sim::asset::kMaterialKind, material);

            material.m_diggable = false;
            material.m_appearance.m_display_name = "Masonry";
            add("masonry", sim::asset::kMaterialKind, material);

            material.m_diggable = true;
            material.m_appearance.m_display_name = "Sandstone";
            add("sandstone", sim::asset::kMaterialKind, material);
            add("spare", sim::asset::kMaterialKind, material, "Odd-Original.TOML");

            material.m_element = "liquid";
            material.m_diggable.reset();
            material.m_representation = sim::definition::EMaterialRepresentation::particle;
            material.m_appearance.m_display_name = "Water";
            add("water", sim::asset::kMaterialKind, material);

            sim::asset::FieldDescriptor field;
            field.m_schema_version = 1u;
            field.m_definition.m_frequency_per_metre = 0.01;
            add("composition", sim::asset::kFieldKind, field);

            BiomeAssetDescriptor biome;
            biome.m_schema_version = 1u;
            biome.m_appearance.m_display_name = "Hearth";
            biome.m_appearance.m_preview_color_rgba = "808080ff";
            biome.m_definition.m_temperature = {{"composition"}, 300.0, 1.0};
            sim::definition::CompositionBand band;
            band.m_field.m_key = "composition";
            band.m_materials.m_entries.push_back({"stone", {{"basalt"}, 1.0}});
            biome.m_definition.m_composition.m_entries.push_back({"cover", std::move(band)});
            add("hearth", sim::asset::kBiomeKind, biome);

            FeatureAssetDescriptor feature;
            feature.m_schema_version = 1u;
            feature.m_appearance.m_display_name = "Landing";
            feature.m_definition.m_reservation = {0, 0, 16, 16};
            sim::definition::Operation operation;
            operation.m_action = sim::definition::Carve{sim::definition::Rectangle{16u, 16u}};
            feature.m_definition.m_operations.m_entries.push_back({"op_01", std::move(operation)});
            feature.m_definition.m_markers.m_entries.push_back({"spawn", {sim::EGenerationMarker::start, 8, 8}});
            add("landing", sim::asset::kFeatureKind, feature);

            sim::asset::RuleDescriptor rule;
            rule.m_schema_version = 1u;
            sim::definition::Cave cave;
            cave.m_biome.m_key = "hearth";
            cave.m_field.m_key = "composition";
            cave.m_threshold = {0.95, 1.0};
            rule.m_definition.m_payload = cave;
            add("caves", sim::asset::kRuleKind, rule);

            BackgroundAssetDescriptor background;
            background.m_schema_version = 1u;

            for (u32 index = 0u; index < 4u; ++index) {
                BackgroundAppearanceLayer layer;
                layer.m_order = index;
                layer.m_texture = "albedo";
                layer.m_color_rgba = "ffffffff";
                layer.m_repeat_period_m = 1.0;
                background.m_definition.m_layers.m_entries.push_back(
                    {"layer_" + std::to_string(index), std::move(layer)});
            }

            add("skies", kBackgroundAssetKind, background);

            WorldAssetDescriptor world;
            world.m_schema_version = 1u;
            world.m_semantic_key = "frontier";
            world.m_appearance.m_display_name = "Frontier";
            world.m_appearance.m_background_stack = "skies";
            world.m_palette.m_materials.m_entries = {
                {"basalt", {"basalt", 1u}}, {"masonry", {"masonry", 2u}}, {"sandstone", {"sandstone", 3u}},
                {"water", {"water", 4u}}};
            world.m_palette.m_fields.m_entries.push_back({"composition", {"composition", 0u}});
            world.m_palette.m_biomes.m_entries.push_back({"hearth", {"hearth", 0u}});
            world.m_palette.m_features.m_entries.push_back({"landing", {"landing", 0u}});
            world.m_palette.m_rules.m_entries.push_back({"caves", {"caves"}});
            world.m_profile.m_layout = {1u, 1u, 16u, 1.0};
            world.m_profile.m_boundaries = {{"basalt"}, 1u, 0.0, {"composition"}, {"composition"}};
            world.m_profile.m_start.m_biome.m_key = "hearth";
            world.m_profile.m_start.m_feature.m_key = "landing";
            world.m_profile.m_start.m_marker.m_key = "spawn";
            world.m_profile.m_start.m_x_fine = 128u;
            world.m_profile.m_start.m_y_fine = 128u;
            world.m_profile.m_start.m_clearance_m = 2.0;
            world.m_profile.m_start.m_route_width_m = 1.0;
            sim::definition::BiomeChoice choice;
            choice.m_biome.m_key = "hearth";
            choice.m_required_count = 1u;
            choice.m_distance_max_m = 1024.0;
            world.m_profile.m_biomes.m_entries.push_back({"hearth", std::move(choice)});
            add("frontier", sim::asset::kWorldKind, world);

            world.m_semantic_key = "hollows";
            world.m_appearance.m_display_name = "Hollows";
            add("hollows", sim::asset::kWorldKind, world);
            write("pack.toml", m_manifest);
        }

        ~Fixture() {
            // Best-effort fixture cleanup; retained catalogs and snapshots own their bytes.
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
            PPR_LOG_WARNING_ON_FAIL(PackContentTests, error);
        }

        Fixture(const Fixture &) = delete;
        Fixture &operator=(const Fixture &) = delete;

        std::string read(const std::string_view name) const {
            std::ifstream file{m_root / name, std::ios::binary};
            PPR_TEST_ASSERT(file.good());
            return {std::istreambuf_iterator<char>{file}, {}};
        }

        void write(const std::string_view name, const std::string_view bytes) const {
            const auto path = m_root / name;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file{path, std::ios::binary | std::ios::trunc};
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            file.close();
            PPR_TEST_ASSERT(not file.fail());
        }

        template<typename T>
            requires requires { T::describe(); }
        void write(const std::string_view name, const T &value) const {
            const auto document = config::documentFrom(value);
            PPR_TEST_ASSERT(document.has_value());
            const auto encoded = config::encode(*document);
            PPR_TEST_ASSERT(encoded.has_value());

            write(name, std::string_view{*encoded});
        }

        template<typename T>
        void add(
            const std::string_view key, const std::string_view kind, const T &value,
            const std::string_view existing = {}) {
            const auto file = existing.empty() ? std::string{key} + ".toml" : std::string{existing};
            m_manifest += "\n[assets." +
                          std::string{key} +
                          "]\nkind = \"" +
                          std::string{kind} +
                          "\"\ndescriptor = \"" +
                          file +
                          "\"\n";

            write(file, value);
        }

        Expected<ContentCatalog> load(config::Diagnostics &diagnostics) const {
            auto definitions = loadContentCatalog(m_root, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(PackContentTests, definitions);

            auto owner = std::make_shared<const ContentDescriptorCatalog>(std::move(*definitions));
            const auto id = resolveContentWorld(*owner, kWorld, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(PackContentTests, id);

            return selectWorldCatalog(owner, *id, kWorldCompileParameters, diagnostics);
        }

        ContentAuthoringSession open(config::Diagnostics &diagnostics) const {
            auto opened = ContentAuthoringSession::open(m_root, diagnostics);
            PPR_TEST_ASSERT(opened.has_value());
            return std::move(*opened);
        }
    };

    const asset::DescriptorSource &descriptor(const ContentSourceSet &source, const std::filesystem::path &path) {
        const auto found = std::ranges::find(source.m_descriptors, path, &asset::DescriptorSource::m_path);
        PPR_TEST_ASSERT(found != source.m_descriptors.end());
        return *found;
    }

    const ContentDiskRecord &ledger(const ContentDiskBaseline &baseline, const std::filesystem::path &path) {
        const auto found = std::ranges::find_if(baseline.m_descriptors, [&](const auto &record) {
            return record.m_target and record.m_target->path() == path;
        });
        PPR_TEST_ASSERT(found != baseline.m_descriptors.end());
        return *found;
    }

    std::filesystem::path target(
        const ContentAuthoringSession &session, const std::string_view key, const std::string_view kind,
        config::Diagnostics &diagnostics) {
        const auto entry = session.sourceIndex().resolve("ppr.test", key, kind, diagnostics);
        PPR_TEST_ASSERT(entry.has_value());
        return (*entry)->m_descriptor;
    }

    void save(ContentAuthoringSession &session, config::Diagnostics &diagnostics) {
        auto result = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(result.has_value() and not result->m_error);
        PPR_TEST_ASSERT(session.acceptSave(std::move(*result)));
        PPR_TEST_ASSERT(not session.dirty());
    }

    std::string reverseTables(const std::string &text) {
        const auto first = text.find("\n[");
        PPR_TEST_ASSERT(first != std::string::npos);

        Array<std::string_view> tables;
        auto position = first;

        while (position != std::string::npos) {
            const auto next = text.find("\n[", position + 2u);
            tables.push_back(
                std::string_view{text}.substr(
                    position, next == std::string::npos ? std::string::npos : next - position));
            position = next;
        }

        std::string result = text.substr(0u, first);
        for (const auto table: tables | std::views::reverse) {
            result += table;
        }

        return result;
    }

    void useSmallIdentityImages(const Fixture &fixture, const bool alternate = false) {
        // Real 4x4 PNGs with different pixel values, both valid for mip generation.
        constexpr std::array<u8, 75u> first{
            0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x48u, 0x44u,
            0x52u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x04u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0xa9u,
            0xf1u, 0x9eu, 0x7eu, 0x00u, 0x00u, 0x00u, 0x12u, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0x9cu, 0x63u, 0x68u,
            0x68u, 0xf8u, 0xffu, 0x1fu, 0x19u, 0x33u, 0x90u, 0x2eu, 0x00u, 0x00u, 0x4cu, 0x7eu, 0x2fu, 0xe1u, 0x74u,
            0xdcu, 0xdau, 0x7cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x49u, 0x45u, 0x4eu, 0x44u, 0xaeu, 0x42u, 0x60u, 0x82u};
        constexpr std::array<u8, 75u> second{
            0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x48u, 0x44u,
            0x52u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x04u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0xa9u,
            0xf1u, 0x9eu, 0x7eu, 0x00u, 0x00u, 0x00u, 0x12u, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0x9cu, 0x63u, 0x38u,
            0xd0u, 0xf0u, 0xeeu, 0x3fu, 0x32u, 0x66u, 0x20u, 0x5du, 0x00u, 0x00u, 0xb6u, 0xe6u, 0x32u, 0xd1u, 0xedu,
            0xfbu, 0xd5u, 0x33u, 0x00u, 0x00u, 0x00u, 0x00u, 0x49u, 0x45u, 0x4eu, 0x44u, 0xaeu, 0x42u, 0x60u, 0x82u};
        const auto bytes = alternate ? std::span<const u8>{second} : std::span<const u8>{first};

        fixture.write("images/color.png", {reinterpret_cast<const char *>(bytes.data()), bytes.size()});
    }

    std::shared_ptr<const ContentPresentation> present(
        std::shared_ptr<const ContentCatalog> catalog, config::Diagnostics &diagnostics,
        const asset::RequestGeneration generation = asset::RequestGeneration{1u}, ContentSourceEpochToken epoch = {},
        const std::shared_ptr<const ContentPresentation> &previous = {}) {
        auto request = ContentPresentationRequest::start(
            std::move(catalog), generation, context::background(), diagnostics, std::move(epoch), previous);
        PPR_TEST_ASSERT(request.has_value());
        auto result = request->finish(diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        return *result;
    }

    // ------------------------------------------------------------------
    // catalog tools and small numeric plans
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (catalog_dig_respects_protected_material_and_preflights_the_complete_rectangle) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not grid.setCell({127u, 20u}, {catalog->findMaterial("basalt")->m_numeric_id, 301.0f}));
        PPR_TEST_ASSERT(not grid.setCell({128u, 20u}, {catalog->findMaterial("masonry")->m_numeric_id, 302.0f}));
        PPR_TEST_ASSERT(not grid.setCell({129u, 20u}, {catalog->findMaterial("water")->m_numeric_id, 303.0f}));
        grid.clearAllDirty();

        const DigRect rect{{127u, 20u}, 3u, 1u};
        const auto particle = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(not particle and particle.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_element == catalog->findMaterial("basalt")->m_numeric_id);

        PPR_TEST_ASSERT(not grid.setCell({129u, 20u}, {60000u, 303.0f}));
        const auto unknown = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(not unknown and unknown.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_element == catalog->findMaterial("basalt")->m_numeric_id);

        PPR_TEST_ASSERT(not grid.setCell({129u, 20u}, {catalog->findMaterial("sandstone")->m_numeric_id, 303.0f}));
        const auto result = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(result.has_value() and result->m_dug == 2u and result->m_touched.size() == 2u);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_element == 0u);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_temperature == 301.0f);
        PPR_TEST_ASSERT(grid.getCell({128u, 20u})->m_element == catalog->findMaterial("masonry")->m_numeric_id);
        PPR_TEST_ASSERT(grid.getCell({129u, 20u})->m_element == 0u);

        const auto repeated = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(repeated.has_value() and repeated->m_dug == 0u and repeated->m_touched.empty());
    };

    PPR_UNIT_TEST (construction_checks_complete_tiles_reservations_and_live_actor_overlap) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        sim::ChunkGrid grid;
        sim::Registry registry;
        PPR_TEST_ASSERT(not registerColonyComponents(registry));
        const Footprint print{{126u, 21u}, 3u, 3u, catalog->findMaterial("masonry")->m_numeric_id};
        PPR_TEST_ASSERT(not validateConstruction(grid, registry, print, *catalog));

        auto invalid = print;
        invalid.m_element = catalog->findMaterial("water")->m_numeric_id;
        PPR_TEST_ASSERT(validateConstruction(grid, registry, invalid, *catalog) == std::errc::invalid_argument);
        invalid = print;
        invalid.m_min.m_x = 127u;
        PPR_TEST_ASSERT(validateConstruction(grid, registry, invalid, *catalog) == std::errc::invalid_argument);
        invalid.m_min.m_x = 4095u;
        PPR_TEST_ASSERT(validateConstruction(grid, registry, invalid, *catalog) == std::errc::invalid_argument);

        PPR_TEST_ASSERT(not grid.setCell({128u, 23u}, {catalog->findMaterial("basalt")->m_numeric_id, 299.0f}));
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
        PPR_TEST_ASSERT(grid.getCell({128u, 23u})->m_element == catalog->findMaterial("masonry")->m_numeric_id);
    };

    PPR_UNIT_TEST (errand_preflight_rejects_invalid_actor_before_any_demolition_writes) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        sim::ChunkGrid grid;
        sim::Registry registry;
        PPR_TEST_ASSERT(not registerColonyComponents(registry));
        PPR_TEST_ASSERT(not grid.setCell({30u, 30u}, {catalog->findMaterial("basalt")->m_numeric_id, 293.0f}));
        const auto demolition = registry.create();
        registry.emplace<BuildErrand>(demolition, BuildErrand{Footprint{{30u, 30u}, 3u, 3u, 0u, false}, 0u, 9u});
        const auto build = registry.create();
        registry.emplace<BuildErrand>(
            build, BuildErrand{Footprint{{60u, 30u}, 3u, 3u, catalog->findMaterial("masonry")->m_numeric_id}, 0u, 9u});
        const auto actor = registry.create();
        registry.emplace<Agent>(actor, Agent{});
        registry.emplace<sim::BodyState>(actor, sim::BodyState{10.0f, 10.0f});

        Pathfinder finder;
        Array<sim::ChunkPos> touched;
        PPR_TEST_ASSERT(advanceErrands(grid, registry, finder, touched, 64u, &*catalog) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.getCell({30u, 30u})->m_element == catalog->findMaterial("basalt")->m_numeric_id);
        PPR_TEST_ASSERT(touched.empty());
        PPR_TEST_ASSERT(registry.get<BuildErrand>(demolition)->m_progress == 0u);
        PPR_TEST_ASSERT(registry.get<BuildErrand>(build)->m_progress == 0u);
    };

    PPR_UNIT_TEST (data_only_material_extension_reaches_raster_tools_and_colliders) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        PPR_TEST_ASSERT(not session.cloneDefinition(EContentDocument::materials, "basalt", "violet_ore", diagnostics));
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::materials, "violet_ore", "appearance.display_name", config::FileValue::text("Violet ore"),
            diagnostics));
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::materials, "violet_ore", "appearance.color_rgba", config::FileValue::text("c07cffff"),
            diagnostics));

        const auto palette = config::decode(
            "asset = 'violet_ore'\nid = 5\n", "palette", diagnostics, config::EDecodeMode::tree);
        PPR_TEST_ASSERT(palette.has_value());
        auto draft = session.draftSnapshot();

        for (const auto name: {"frontier.toml", "hollows.toml"}) {
            auto &worlds = draft.m_source.m_descriptors;
            const auto found = std::ranges::find(worlds, fixture.m_root / name, &asset::DescriptorSource::m_path);
            PPR_TEST_ASSERT(found != worlds.end());
            const auto insertion = config::patchTree(
                found->m_bytes, "palette.materials.violet_ore", config::ETreeSourceEdit::insert, *palette,
                found->m_path.string(), diagnostics);
            PPR_TEST_ASSERT(insertion.has_value());
            const auto bytes = config::applySourcePatch(found->m_bytes, *insertion);
            PPR_TEST_ASSERT(bytes.has_value());

            found->m_bytes = *bytes;
        }

        const auto cover = config::decode(
            "material = 'violet_ore'\nweight = 1.0\n", "cover", diagnostics, config::EDecodeMode::tree);
        PPR_TEST_ASSERT(cover.has_value());
        PPR_TEST_ASSERT(not session.replaceDefinitionSubtree(
            EContentDocument::biomes, "hearth", "definition.composition.cover.materials.stone", *cover, diagnostics));
        const auto biome_path = fixture.m_root / "hearth.toml";
        const auto biome = std::ranges::find(
            draft.m_source.m_descriptors, biome_path, &asset::DescriptorSource::m_path);
        PPR_TEST_ASSERT(biome != draft.m_source.m_descriptors.end());

        biome->m_bytes = descriptor(session.draftSource(), biome_path).m_bytes;

        const auto validated = validateContentDraft(draft, diagnostics, nullptr, kWorld);
        PPR_TEST_ASSERT(validated.has_value() and validated->m_selected);
        const auto &catalog = *validated->m_selected;

        const auto *material = catalog.findMaterial("violet_ore");
        PPR_TEST_ASSERT(material != nullptr and material->m_color_rgba == 0xc07cffffu);

        const auto plan = compileWorldPlan(catalog, kWorld, 42u);
        PPR_TEST_ASSERT(plan.has_value());

        const auto layout = sim::compileBiomeLayout(*plan);
        PPR_TEST_ASSERT(layout.has_value());

        const auto geometry = sim::compileGenerationGeometry(*plan, *layout);
        PPR_TEST_ASSERT(geometry.has_value());

        const auto raster = sim::compileGenerationRaster(*plan, *layout, *geometry);
        PPR_TEST_ASSERT(raster.has_value());
        const auto cells = raster->chunk({0u, 0u});
        PPR_TEST_ASSERT(cells.has_value());
        const auto generated = std::ranges::find(*cells, material->m_numeric_id, &sim::Cell::m_element);
        PPR_TEST_ASSERT(generated != cells->end());

        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not grid.setCell({1u, 1u}, *generated));
        physics::Scene scene;
        physics::ChunkColliders colliders{{1.0f / 3.0f}};
        PPR_TEST_ASSERT(not scene.initialize({}));
        std::array<u16, 16u> occupancy{};
        occupancy[5u] = material->m_numeric_id;
        const std::array chunks{physics::ColliderChunkView{{0, 0}, occupancy, true}};
        const std::array dirty{physics::ColliderChunkPos{0, 0}};
        u32 processed{};
        PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, dirty, 4u, 1u, processed));
        PPR_TEST_ASSERT(colliders.chainCount({0, 0}) == 1u);

        const auto dug = digCells(grid, DigRect{{1u, 1u}, 1u, 1u}, &catalog);
        PPR_TEST_ASSERT(dug.has_value() and dug->m_dug == 1u);
        occupancy[5u] = grid.getCell({1u, 1u})->m_element;
        PPR_TEST_ASSERT(not colliders.rebuildDirtyChunks(scene, chunks, dirty, 4u, 1u, processed));
        PPR_TEST_ASSERT(colliders.chainCount({0, 0}) == 0u);
        PPR_TEST_ASSERT(not scene.shutdown());
    };

    PPR_UNIT_TEST (prepared_world_rejects_invalid_inputs_and_cancellation_before_generation) {
        struct Clock final : ITimerClock {
            mutable u32 m_calls{};

            TimePoint now() const noexcept override {
                ++m_calls;
                return {};
            }
        } clock;

        Fixture fixture;
        config::Diagnostics diagnostics;
        auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        const auto unknown = prepareWorld(*catalog, "ppr.test:unknown", 42u, clock);
        PPR_TEST_ASSERT(not unknown and clock.m_calls == 0u);

        const auto incomplete = prepareWorld(ContentCatalog{zero_v}, kWorld, 42u, clock);
        PPR_TEST_ASSERT(not incomplete and incomplete.error() == std::errc::invalid_argument);

        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = prepareWorld(std::move(*catalog), kWorld, 42u, clock, {.m_context = job_context});
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        const auto cancelled_load = prepareWorld(
            fixture.m_root, kWorld, 42u, clock, diagnostics, {.m_context = job_context});
        PPR_TEST_ASSERT(not cancelled_load and cancelled_load.error() == std::errc::operation_canceled);
        PPR_TEST_ASSERT(clock.m_calls == 0u);
    };

    // ------------------------------------------------------------------
    // split identity and explicit presentation ownership
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (numeric_streams_ignore_manifest_order_comments_and_unused_assets) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        const auto baseline = fixture.load(diagnostics);
        PPR_TEST_ASSERT(baseline.has_value());
        const auto first = compileWorldPlan(*baseline, kWorld, 42u);
        PPR_TEST_ASSERT(first.has_value());

        auto source = loadContentSourceSet(std::array{fixture.m_root}, diagnostics);
        PPR_TEST_ASSERT(source.has_value());
        auto changed = source->source();
        std::ranges::reverse(changed.m_descriptors);
        changed.m_packs.front().m_manifest.m_bytes = reverseTables(changed.m_packs.front().m_manifest.m_bytes);
        changed.m_packs.front().m_manifest.m_bytes += "\n# provenance only\n";

        for (auto &entry: changed.m_descriptors) {
            entry.m_bytes += "\n# provenance only\n";
        }

        auto &manifest = changed.m_packs.front().m_manifest.m_bytes;
        manifest += "\n[assets.aaa_unused]\nkind = 'sim.material'\ndescriptor = 'materials/asset-aaa_unused.toml'\n";
        changed.m_descriptors.push_back(
            {fixture.m_root / "materials/asset-aaa_unused.toml",
                descriptor(changed, fixture.m_root / "basalt.toml").m_bytes});

        const auto moved_path = fixture.m_root / "renamed.toml";
        const auto renamed = std::ranges::find(
            changed.m_descriptors, fixture.m_root / "basalt.toml", &asset::DescriptorSource::m_path);
        PPR_TEST_ASSERT(renamed != changed.m_descriptors.end());
        std::filesystem::rename(renamed->m_path, moved_path);
        renamed->m_path = moved_path;
        const auto location = config::patchScalar(
            manifest, "assets.basalt.descriptor", config::FileValue::text("renamed.toml"), "manifest", diagnostics);
        PPR_TEST_ASSERT(location.has_value());
        const auto relocated = config::applySourcePatch(manifest, *location);
        PPR_TEST_ASSERT(relocated.has_value());
        manifest = *relocated;

        auto imported = loadContentCatalog(changed, diagnostics);
        PPR_TEST_ASSERT(imported.has_value());
        auto owner = std::make_shared<const ContentDescriptorCatalog>(std::move(*imported));
        const auto selected = selectWorldCatalog(owner, baseline->worldId(), kWorldCompileParameters, diagnostics);
        PPR_TEST_ASSERT(selected.has_value());
        PPR_TEST_ASSERT(selected->contentFingerprint() == baseline->contentFingerprint());
        PPR_TEST_ASSERT(selected->presentationDescriptorFingerprint() == baseline->presentationDescriptorFingerprint());
        const auto second = compileWorldPlan(*selected, kWorld, 42u);
        PPR_TEST_ASSERT(second.has_value());

        for (u32 sample = 0u; sample < 32u; ++sample) {
            PPR_TEST_ASSERT(
                first->fields().front().sample(sample * 13.25, 123.75) ==
                second->fields().front().sample(sample * 13.25, 123.75));
        }
    };

    PPR_UNIT_TEST (identity_separates_numeric_appearance_descriptor_and_binary_changes) {
        Fixture fixture;
        useSmallIdentityImages(fixture);
        config::Diagnostics diagnostics;
        auto baseline_value = fixture.load(diagnostics);
        PPR_TEST_ASSERT(baseline_value.has_value());
        auto baseline = std::make_shared<const ContentCatalog>(std::move(*baseline_value));
        const auto baseline_images = present(baseline, diagnostics);
        auto session = fixture.open(diagnostics);
        const auto original = session.draftSnapshot();

        struct Change {
            config::FileValue m_value;
            std::string_view m_key, m_property;
            EContentDocument m_category;
            bool m_numeric;

            Change(
                config::FileValue value, const EContentDocument category, const std::string_view key,
                const std::string_view property, const bool numeric)
                : m_value{std::move(value)}, m_key{key}, m_property{property}, m_category{category},
                  m_numeric{numeric} {
            }
        };

        const Change changes[] = {
            {config::FileValue::floating(2100.0), EContentDocument::elements, "solid", "density", true},
            {config::FileValue::floating(0.02), EContentDocument::fields, "composition",
                "definition.frequency_per_metre", true},
            {config::FileValue::floating(301.0), EContentDocument::biomes, "hearth", "definition.temperature.base_k",
                true},
            {config::FileValue::integer(15), EContentDocument::features, "landing",
                "definition.operations.op_01.action.carve.shape.rectangle.width_fine", true},
            {config::FileValue::floating(0.2), EContentDocument::rules, "caves",
                "definition.payload.cave.threshold.min", true},
            {config::FileValue::integer(129), EContentDocument::worlds, "frontier", "profile.start.x_fine", true},
            {config::FileValue::floating(0.1), EContentDocument::materials, "basalt", "appearance.metallic", false},
            {config::FileValue::floating(0.9), EContentDocument::materials, "basalt", "appearance.roughness", false},
            {config::FileValue::floating(0.1), EContentDocument::materials, "basalt", "appearance.emissive_r", false},
            {config::FileValue::floating(1.1), EContentDocument::materials, "basalt", "appearance.emissive_strength",
                false},
            {config::FileValue::floating(1.6), EContentDocument::materials, "basalt", "appearance.ior", false},
            {config::FileValue::floating(0.9), EContentDocument::materials, "basalt", "appearance.normal_scale", false},
            {config::FileValue::floating(0.1), EContentDocument::materials, "basalt", "appearance.refraction_depth_m",
                false},
            {config::FileValue::text("blend"), EContentDocument::materials, "basalt", "appearance.alpha_mode", false},
            {config::FileValue::text("repeat"), EContentDocument::materials, "basalt", "appearance.texture_sampling",
                false},
            {config::FileValue::floating(0.1), EContentDocument::backgrounds, "skies",
                "definition.layers.layer_0.parallax_factor", false},
            {config::FileValue::boolean(false), EContentDocument::textures, "albedo", "full_chain", false},
        };

        for (const auto &change: changes) {
            PPR_TEST_ASSERT(not session.editDefinition(
                change.m_category, change.m_key, change.m_property, change.m_value, diagnostics));
            const auto candidate = validateContentDraft(session.draftSnapshot(), diagnostics, nullptr, kWorld);
            PPR_TEST_ASSERT(candidate.has_value() and candidate->m_selected);
            const auto &catalog = *candidate->m_selected;
            PPR_TEST_ASSERT((catalog.contentFingerprint() != baseline->contentFingerprint()) == change.m_numeric);
            if (not change.m_numeric) {
                PPR_TEST_ASSERT(
                    catalog.presentationDescriptorFingerprint() != baseline->presentationDescriptorFingerprint());
            }

            PPR_TEST_ASSERT(session.undo().value());
        }

        PPR_TEST_ASSERT(not session.dirty());
        PPR_TEST_ASSERT(session.draftSource().m_descriptors.size() == original.m_source.m_descriptors.size());
        useSmallIdentityImages(fixture, true);
        auto unchanged_value = fixture.load(diagnostics);
        PPR_TEST_ASSERT(unchanged_value.has_value());
        auto unchanged = std::make_shared<const ContentCatalog>(std::move(*unchanged_value));
        PPR_TEST_ASSERT(unchanged->contentFingerprint() == baseline->contentFingerprint());
        PPR_TEST_ASSERT(
            unchanged->presentationDescriptorFingerprint() == baseline->presentationDescriptorFingerprint());
        const auto different_images = present(unchanged, diagnostics);
        PPR_TEST_ASSERT(different_images->fingerprint() != baseline_images->fingerprint());
    };

    PPR_UNIT_TEST (explicit_presentation_owns_distinct_usages_and_survives_source_removal) {
        config::Diagnostics diagnostics;
        std::shared_ptr<const ContentPresentation> retained;

        {
            Fixture fixture;
            useSmallIdentityImages(fixture);
            auto value = fixture.load(diagnostics);
            PPR_TEST_ASSERT(value.has_value());
            auto catalog = std::make_shared<const ContentCatalog>(std::move(*value));
            retained = present(catalog, diagnostics, asset::RequestGeneration{7u});
            PPR_TEST_ASSERT(retained->catalog() == catalog and retained->generation() == asset::RequestGeneration{7u});
            PPR_TEST_ASSERT(retained->textures().size() == 2u);

            for (const auto &texture: retained->textures()) {
                PPR_TEST_ASSERT(texture.m_image.m_width == 4u and texture.m_image.m_height == 4u);
                PPR_TEST_ASSERT(texture.m_image.m_mip_count == 3u and texture.m_image.m_storage.isOwned());
                PPR_TEST_ASSERT(texture.m_image.m_is_srgb == not texture.m_normal_map);
            }

            const auto before = retained->textures().front().m_image.m_storage.getBufferData().front();
            auto shared = retained->textures().front().m_image.m_storage;
            auto mutable_copy = shared.moveToUnique();
            const auto mutable_bytes = mutable_copy.getMutableData();
            PPR_TEST_ASSERT(mutable_bytes.has_value());
            mutable_bytes->front() ^= std::byte{1u};
            PPR_TEST_ASSERT(retained->textures().front().m_image.m_storage.getBufferData().front() == before);

            fixture.write("images/color.png", "corrupt");
            PPR_TEST_ASSERT(fixture.load(diagnostics).has_value());
            auto request = ContentPresentationRequest::start(
                catalog, asset::RequestGeneration{8u}, context::background(), diagnostics);
            PPR_TEST_ASSERT(request.has_value());
            PPR_TEST_ASSERT(not request->finish(diagnostics));
        }

        const auto scratch = mem::ScratchPad::open();
        Array<std::string, mem::ScratchPad> reuse(64u, std::string(128u, 'x'));
        PPR_TEST_ASSERT(retained->textures().front().m_image.m_storage.isOwned());
        PPR_TEST_ASSERT(not retained->textures().front().m_image.m_storage.getBufferData().empty());
    };

    // ------------------------------------------------------------------
    // source epochs and independently owned image reuse
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (definition_edits_recompose_current_presentation_with_unchanged_owned_images) {
        Fixture fixture;
        useSmallIdentityImages(fixture);
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        const auto epoch = session.sourceEpoch();
        PPR_TEST_ASSERT(epoch != nullptr);

        const auto original = validateContentDraft(session.draftSnapshot(), diagnostics, nullptr, kWorld);
        PPR_TEST_ASSERT(original.has_value() and original->m_selected);

        auto previous = present(original->m_selected, diagnostics, asset::RequestGeneration{1u}, epoch);
        const auto pixels = previous->textures().front().m_image.m_storage.getBufferData().data();

        auto comment = session.draftSnapshot();
        const auto comment_source = std::ranges::find(
            comment.m_source.m_descriptors, fixture.m_root / "solid.toml", &asset::DescriptorSource::m_path);
        PPR_TEST_ASSERT(comment_source != comment.m_source.m_descriptors.end());

        comment_source->m_bytes += "\n# author comment\n";

        const auto commented = validateContentDraft(comment, diagnostics, nullptr, kWorld);
        PPR_TEST_ASSERT(commented.has_value() and commented->m_selected);

        const auto unchanged = present(
            commented->m_selected, diagnostics, asset::RequestGeneration{2u}, comment.m_source_epoch, previous);
        PPR_TEST_ASSERT(unchanged->catalog() == commented->m_selected and unchanged->catalog() != previous->catalog());
        PPR_TEST_ASSERT(unchanged->generation() == asset::RequestGeneration{2u} and unchanged->sourceEpoch() == epoch);
        PPR_TEST_ASSERT(
            unchanged->work().m_read_count == 0u and
            unchanged->work().m_prepared_count == 0u and
            unchanged->work().m_reused_count == 2u);
        PPR_TEST_ASSERT(unchanged->fingerprint() == previous->fingerprint());

        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::elements, "solid", "density", config::FileValue::floating(2100.0), diagnostics));
        PPR_TEST_ASSERT(session.sourceEpoch() == epoch and session.draftSnapshot().m_source_epoch == epoch);

        const auto density = validateContentDraft(session.draftSnapshot(), diagnostics, nullptr, kWorld);
        PPR_TEST_ASSERT(density.has_value() and density->m_selected);

        const auto recomposed = present(
            density->m_selected, diagnostics, asset::RequestGeneration{3u}, epoch, unchanged);
        PPR_TEST_ASSERT(recomposed->catalog()->contentFingerprint() != previous->catalog()->contentFingerprint());
        PPR_TEST_ASSERT(recomposed->fingerprint() != previous->fingerprint());
        PPR_TEST_ASSERT(recomposed->work().m_read_count == 0u and recomposed->work().m_reused_count == 2u);

        const std::weak_ptr<const ContentPresentation> previous_owner = previous;
        previous.reset();
        PPR_TEST_ASSERT(previous_owner.expired());
        PPR_TEST_ASSERT(recomposed->textures().front().m_image.m_storage.getBufferData().data() == pixels);
        PPR_TEST_ASSERT(
            recomposed->textures().front().m_source_path ==
            std::filesystem::canonical(fixture.m_root / "images/color.png"));

        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(session.sourceEpoch() == epoch);
    };

    PPR_UNIT_TEST (texture_setting_edit_prepares_one_image_while_null_epoch_disables_reuse) {
        Fixture fixture;
        useSmallIdentityImages(fixture);
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        const auto original = validateContentDraft(session.draftSnapshot(), diagnostics, nullptr, kWorld);
        PPR_TEST_ASSERT(original.has_value() and original->m_selected);

        const auto previous = present(
            original->m_selected, diagnostics, asset::RequestGeneration{1u}, session.sourceEpoch());

        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::textures, "albedo", "high_quality", config::FileValue::boolean(true), diagnostics));

        const auto edited = validateContentDraft(session.draftSnapshot(), diagnostics, nullptr, kWorld);
        PPR_TEST_ASSERT(edited.has_value() and edited->m_selected);

        const auto changed = present(
            edited->m_selected, diagnostics, asset::RequestGeneration{2u}, session.sourceEpoch(), previous);
        PPR_TEST_ASSERT(
            changed->work().m_read_count == 1u and
            changed->work().m_prepared_count == 1u and
            changed->work().m_reused_count == 1u);
        PPR_TEST_ASSERT(changed->fingerprint() != previous->fingerprint());

        const auto fresh = present(edited->m_selected, diagnostics, asset::RequestGeneration{3u}, {}, changed);
        PPR_TEST_ASSERT(not fresh->sourceEpoch());
        PPR_TEST_ASSERT(fresh->work().m_read_count == 2u and fresh->work().m_reused_count == 0u);
    };

    PPR_UNIT_TEST (independent_open_epochs_force_binary_reread_even_with_equal_size_and_time) {
        Fixture fixture;
        useSmallIdentityImages(fixture);
        config::Diagnostics diagnostics;
        auto first = fixture.open(diagnostics);
        const auto original = validateContentDraft(first.draftSnapshot(), diagnostics, nullptr, kWorld);
        PPR_TEST_ASSERT(original.has_value() and original->m_selected);

        const auto previous = present(
            original->m_selected, diagnostics, asset::RequestGeneration{1u}, first.sourceEpoch());
        const auto path = fixture.m_root / "images/color.png";
        const auto size = std::filesystem::file_size(path);
        const auto time = std::filesystem::last_write_time(path);

        useSmallIdentityImages(fixture, true);
        std::filesystem::last_write_time(path, time);
        PPR_TEST_ASSERT(std::filesystem::file_size(path) == size and std::filesystem::last_write_time(path) == time);

        auto reloaded = fixture.open(diagnostics);
        PPR_TEST_ASSERT(reloaded.sourceEpoch() and reloaded.sourceEpoch() != first.sourceEpoch());

        const auto selected = validateContentDraft(reloaded.draftSnapshot(), diagnostics, nullptr, kWorld);
        PPR_TEST_ASSERT(selected.has_value() and selected->m_selected);

        const auto fresh = present(
            selected->m_selected, diagnostics, asset::RequestGeneration{2u}, reloaded.sourceEpoch(), previous);
        PPR_TEST_ASSERT(fresh->work().m_read_count == 2u and fresh->work().m_reused_count == 0u);
        PPR_TEST_ASSERT(fresh->fingerprint() != previous->fingerprint());
        PPR_TEST_ASSERT(
            fresh->textures().front().m_source_fingerprint != previous->textures().front().m_source_fingerprint);
    };

    PPR_UNIT_TEST (required_texture_limits_missing_and_cancel_are_errors) {
        Fixture fixture;
        useSmallIdentityImages(fixture);
        config::Diagnostics diagnostics;
        auto value = fixture.load(diagnostics);
        PPR_TEST_ASSERT(value.has_value());
        auto catalog = std::make_shared<const ContentCatalog>(std::move(*value));
        const auto original = fixture.read("images/color.png");

        std::filesystem::resize_file(fixture.m_root / "images/color.png", kMaxTextureInputBytes + 1u);
        const auto oversized = ContentPresentationRequest::start(
            catalog, asset::RequestGeneration{1u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(not oversized and oversized.error() == config::EConfigErrc::out_of_range);

        fixture.write("images/color.png", original);
        PPR_TEST_ASSERT(std::filesystem::remove(fixture.m_root / "images/color.png"));
        const auto before_absence = diagnostics.size();
        const auto absent = ContentPresentationRequest::start(
            catalog, asset::RequestGeneration{2u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(not absent and absent.error() == std::errc::no_such_file_or_directory);
        PPR_TEST_ASSERT(diagnostics.size() == before_absence + 1u);
        PPR_TEST_ASSERT(diagnostics.entries().back().m_property == "source");
        PPR_TEST_ASSERT(
            diagnostics.entries().back().m_location ==
            catalog->textureDefinitions().front().m_descriptor.m_source.m_path.string());

        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = loadContentCatalog(fixture.m_root, diagnostics, job_context.get());
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        const auto cancelled_images = ContentPresentationRequest::start(
            catalog, asset::RequestGeneration{3u}, job_context, diagnostics);
        PPR_TEST_ASSERT(not cancelled_images and cancelled_images.error() == std::errc::operation_canceled);

        fixture.write("images/color.png", original);
        auto admitted = ContentPresentationRequest::start(
            catalog, asset::RequestGeneration{4u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(admitted.has_value());
        admitted->cancel();
        const auto stopped = admitted->finish(diagnostics);
        PPR_TEST_ASSERT(not stopped and stopped.error() == std::errc::operation_canceled);
    };

    PPR_UNIT_TEST (earlier_mip_failure_precedes_later_texture_preparation_failure) {
        Fixture fixture;
        constexpr std::array<u8, 70u> pixel{
            0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x48u, 0x44u,
            0x52u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0x1fu,
            0x15u, 0xc4u, 0x89u, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0x9cu, 0x63u, 0x68u,
            0x68u, 0xf8u, 0xffu, 0x1fu, 0x00u, 0x06u, 0x82u, 0x02u, 0xffu, 0xb3u, 0xbeu, 0x51u, 0x33u, 0x00u, 0x00u,
            0x00u, 0x00u, 0x49u, 0x45u, 0x4eu, 0x44u, 0xaeu, 0x42u, 0x60u, 0x82u};
        fixture.write("images/color.png", {reinterpret_cast<const char *>(pixel.data()), pixel.size()});
        fixture.write("images/broken.png", "corrupt");
        config::Diagnostics diagnostics;
        const auto original = fixture.load(diagnostics);
        PPR_TEST_ASSERT(original.has_value() and original->textureDefinitions().size() == 2u);

        const auto first = original->textureDefinitions().front();
        const auto second = original->textureDefinitions().back();

        // Assign the failure roles in the selected closure's actual ID order;
        // lexical descriptor names do not determine preparation precedence.
        auto mip = first.m_descriptor.m_value;
        mip.m_source = "images/color.png";
        fixture.write(first.m_descriptor.m_source.m_path.filename().string(), mip);

        auto broken = second.m_descriptor.m_value;
        broken.m_source = "images/broken.png";
        fixture.write(second.m_descriptor.m_source.m_path.filename().string(), broken);

        auto value = fixture.load(diagnostics);
        PPR_TEST_ASSERT(value.has_value());
        auto catalog = std::make_shared<const ContentCatalog>(std::move(*value));
        PPR_TEST_ASSERT(catalog->textureDefinitions().front().m_id == first.m_id);

        auto request = ContentPresentationRequest::start(
            catalog, asset::RequestGeneration{1u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(request.has_value());

        const auto result = request->finish(diagnostics);
        PPR_TEST_ASSERT(not result and result.error() == image::make_error_code(image::errc::invalid_argument));
        PPR_TEST_ASSERT(diagnostics.entries().size() == 2u);
        PPR_TEST_ASSERT(diagnostics.entries().front().m_location == first.m_descriptor.m_source.m_path.string());
        PPR_TEST_ASSERT(diagnostics.entries().front().m_message.starts_with("Cannot generate texture mip chain"));
        PPR_TEST_ASSERT(diagnostics.entries().back().m_location == second.m_descriptor.m_source.m_path.string());
        PPR_TEST_ASSERT(
            diagnostics.entries().back().m_message.starts_with("Cannot decode texture within image limits"));
    };

    // ------------------------------------------------------------------
    // descriptor schema and complete candidate validation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (old_formats_ranges_references_and_versions_reject_without_binary_work) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        const auto original = fixture.read("hearth.toml");
        PPR_TEST_ASSERT(not session.editBiome(
            "hearth", "definition.composition.cover.bed_dip", config::FileValue::floating(1.0), diagnostics));
        const auto range = validateContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(not range and range.error() == config::EConfigErrc::out_of_range);
        PPR_TEST_ASSERT(not saveContentDraft(session.draftSnapshot(), diagnostics));
        PPR_TEST_ASSERT(fixture.read("hearth.toml") == original);
        PPR_TEST_ASSERT(session.undo().value());

        PPR_TEST_ASSERT(not session.editBiome(
            "hearth", "definition.temperature.field", config::FileValue::text("missing"), diagnostics));
        PPR_TEST_ASSERT(not validateContentDraft(session.draftSnapshot(), diagnostics));
        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::elements, "solid", "schema_version", config::FileValue::integer(99), diagnostics));
        const auto version = validateContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(not version and version.error() == config::EConfigErrc::out_of_range);
        PPR_TEST_ASSERT(session.undo().value());

        fixture.write("pack.toml", "schema_version = 1\npack_id = 'old'\n[files]\nmaterials = 'materials.toml'\n");
        PPR_TEST_ASSERT(not loadContentCatalog(fixture.m_root, diagnostics));
        PPR_TEST_ASSERT(not std::filesystem::exists(fixture.m_root / "images/color.png"));
    };

    PPR_UNIT_TEST (valid_selected_world_does_not_hide_broken_unselected_world) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::worlds, "hollows", "profile.start.marker", config::FileValue::text("missing"),
            diagnostics));
        const auto draft = session.draftSnapshot();
        auto definitions = loadContentCatalog(draft.m_source, diagnostics);
        PPR_TEST_ASSERT(definitions.has_value());
        auto owner = std::make_shared<const ContentDescriptorCatalog>(std::move(*definitions));
        const auto selected = resolveContentWorld(*owner, kWorld, diagnostics);
        PPR_TEST_ASSERT(selected.has_value());
        PPR_TEST_ASSERT(selectWorldCatalog(owner, *selected, draft.m_parameters, diagnostics).has_value());

        PPR_TEST_ASSERT(not validateContentDraft(draft, diagnostics, nullptr, kWorld));
        PPR_TEST_ASSERT(not saveContentDraft(draft, diagnostics));
        PPR_TEST_ASSERT(
            fixture.read("hollows.toml") != descriptor(draft.m_source, fixture.m_root / "hollows.toml").m_bytes);
    };

    PPR_UNIT_TEST (public_snapshots_reject_null_and_duplicate_ownership_records) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        auto missing = session.draftSnapshot();
        missing.m_baseline.m_descriptors.front().m_target.reset();
        const auto null_validation = validateContentDraft(missing, diagnostics);
        PPR_TEST_ASSERT(not null_validation and null_validation.error() == std::errc::invalid_argument);
        const auto null_save = saveContentDraft(std::move(missing), diagnostics);
        PPR_TEST_ASSERT(not null_save and null_save.error() == std::errc::invalid_argument);

        auto duplicate = session.draftSnapshot();
        duplicate.m_baseline.m_descriptors.push_back(duplicate.m_baseline.m_descriptors.front());
        const auto duplicate_validation = validateContentDraft(duplicate, diagnostics);
        PPR_TEST_ASSERT(not duplicate_validation and duplicate_validation.error() == std::errc::invalid_argument);
        const auto duplicate_save = saveContentDraft(std::move(duplicate), diagnostics);
        PPR_TEST_ASSERT(not duplicate_save and duplicate_save.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not session.dirty());
    };

    PPR_UNIT_TEST (multi_pack_authoring_requires_qualified_names_and_checks_untouched_packs) {
        Fixture fixture;
        fixture.write(
            "addon/pack.toml", "schema_version = 1\npack_id = 'ppr.addon'\n"
                               "[assets.noise]\nkind = 'sim.field'\ndescriptor = 'noise.toml'\n");
        sim::asset::FieldDescriptor field;
        field.m_schema_version = 1u;
        field.m_definition.m_frequency_per_metre = 0.01;
        fixture.write("addon/noise.toml", field);
        const std::filesystem::path roots[] = {fixture.m_root, fixture.m_root / "addon"};
        config::Diagnostics diagnostics;
        auto opened = ContentAuthoringSession::open(roots, diagnostics);
        PPR_TEST_ASSERT(opened.has_value());
        auto session = std::move(*opened);
        const auto revision = session.revision();

        PPR_TEST_ASSERT(
            session.editBiome(
                "hearth", "definition.temperature.base_k", config::FileValue::floating(301.0), diagnostics) ==
            config::EConfigErrc::cross_field_invalid);
        PPR_TEST_ASSERT(session.revision() == revision and not session.dirty());
        PPR_TEST_ASSERT(not session.editBiome(
            "ppr.test:hearth", "definition.temperature.base_k", config::FileValue::floating(301.0), diagnostics));
        PPR_TEST_ASSERT(
            session.cloneDefinition(EContentDocument::fields, "ppr.test:composition", "ppr.addon:copy", diagnostics) ==
            std::errc::file_exists);

        const auto external = fixture.read("addon/noise.toml");
        fixture.write("addon/noise.toml", external + "\n# external pack edit\n");
        const auto rejected = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(not rejected and rejected.error() == config::EConfigErrc::cross_field_invalid);
        PPR_TEST_ASSERT(
            fixture.read("hearth.toml") == *ledger(session.baselineSource(), fixture.m_root / "hearth.toml").m_bytes);

        fixture.write("addon/noise.toml", external);
        save(session, diagnostics);
    };

    PPR_UNIT_TEST (workshop_property_metadata_tracks_descriptor_schemas_and_reference_categories) {
        const config::Document document;
        const auto metadata = [&](const EContentDocument category, const std::string_view property) {
            return contentPropertyDescriptor(category, property, document, "selected");
        };

        const auto probability = metadata(EContentDocument::biomes, "definition.formation.probability");
        PPR_TEST_ASSERT(probability and probability->m_min == 0.0 and probability->m_max == 0.2);

        const auto phase = metadata(EContentDocument::elements, "phase");
        PPR_TEST_ASSERT(phase and phase->m_choices.size() == sim::asset::Element::phase_domain.m_entries.size());
        PPR_TEST_ASSERT(std::ranges::find(phase->m_choices, "liquid") != phase->m_choices.end());

        const auto octaves = metadata(EContentDocument::fields, "definition.octaves");
        PPR_TEST_ASSERT(octaves and octaves->m_min == 1.0 and octaves->m_max == 16.0);

        const auto frequency = metadata(EContentDocument::fields, "definition.frequency_per_metre");
        PPR_TEST_ASSERT(frequency and frequency->m_exclusive_min and std::isfinite(frequency->m_slider_max));

        const auto band = metadata(EContentDocument::biomes, "definition.composition.cover.bed_dip");
        PPR_TEST_ASSERT(band and band->m_min == -0.5 and band->m_max == 0.5);

        const auto weight = metadata(EContentDocument::biomes, "definition.composition.cover.materials.stone.weight");
        PPR_TEST_ASSERT(weight and weight->m_exclusive_min and weight->m_min == 0.0);

        const auto operation = metadata(
            EContentDocument::features, "definition.operations.op_01.action.carve.shape.tag");
        PPR_TEST_ASSERT(operation and operation->m_choices.size() == 3u);

        const auto element = metadata(EContentDocument::materials, "element");
        PPR_TEST_ASSERT(element and not element->m_optional);

        const auto material = metadata(EContentDocument::biomes, "definition.surface_layers.soil.material");
        PPR_TEST_ASSERT(material and material->m_reference_category == EContentDocument::materials);

        const auto kind = metadata(EContentDocument::rules, "definition.payload.tag");
        PPR_TEST_ASSERT(kind and not kind->m_read_only and kind->m_choices.size() == 6u);
        PPR_TEST_ASSERT(not metadata(EContentDocument::rules, "definition.payload.cave.unknown"));

        const auto lifetime = operation->m_choices;
        PPR_TEST_ASSERT(lifetime.front() == "rectangle");
    };

    // ------------------------------------------------------------------
    // authoring history and independent definition edits
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (authoring_all_descriptor_categories_clone_delete_and_round_trip) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        const auto original = session.draftSnapshot();
        constexpr std::array<std::string_view, kContentDocumentCount> keys{
            "basalt", "hearth", "composition", "landing", "frontier", "caves", "solid", "albedo", "skies"};

        for (std::size_t index = 0u; index < keys.size(); ++index) {
            const auto category = static_cast<EContentDocument>(index);
            const auto copy = "copy_" + std::to_string(index);
            PPR_TEST_ASSERT(not session.cloneDefinition(category, keys[index], copy, diagnostics));
            const auto copied_path = target(session, copy, contentAssetKind(category), diagnostics);
            const auto cloned = session.draftSnapshot();
            PPR_TEST_ASSERT(cloned.m_source.m_descriptors.size() == original.m_source.m_descriptors.size() + 1u);
            PPR_TEST_ASSERT(not std::filesystem::exists(copied_path));

            PPR_TEST_ASSERT(session.undo().value());
            PPR_TEST_ASSERT(not session.dirty());
            PPR_TEST_ASSERT(session.redo().value());
            PPR_TEST_ASSERT(
                descriptor(session.draftSource(), copied_path).m_bytes ==
                descriptor(cloned.m_source, copied_path).m_bytes);
            PPR_TEST_ASSERT(not session.deleteDefinition(category, copy, diagnostics));
            PPR_TEST_ASSERT(session.undo().value());
            PPR_TEST_ASSERT(
                descriptor(session.draftSource(), copied_path).m_bytes ==
                descriptor(cloned.m_source, copied_path).m_bytes);
            PPR_TEST_ASSERT(session.redo().value());
            PPR_TEST_ASSERT(not session.revert());
        }

        PPR_TEST_ASSERT(
            session.cloneDefinition(EContentDocument::materials, "basalt", "basalt", diagnostics) ==
            std::errc::file_exists);

        const auto revision = session.revision();
        const auto unrelated = fixture.read("basalt.toml");
        fixture.write("materials/asset-taken.toml", unrelated);
        PPR_TEST_ASSERT(
            session.cloneDefinition(EContentDocument::materials, "basalt", "taken", diagnostics) ==
            config::EConfigErrc::cross_field_invalid);
        PPR_TEST_ASSERT(session.revision() == revision and not session.dirty());
        PPR_TEST_ASSERT(fixture.read("materials/asset-taken.toml") == unrelated);

        PPR_TEST_ASSERT(not session.deleteDefinition(EContentDocument::fields, "composition", diagnostics));
        PPR_TEST_ASSERT(not validateContentDraft(session.draftSnapshot(), diagnostics));
        PPR_TEST_ASSERT(not saveContentDraft(session.draftSnapshot(), diagnostics));
        PPR_TEST_ASSERT(session.undo().value() and not session.dirty());
    };

    PPR_UNIT_TEST (authoring_optional_group_and_variant_transactions_are_atomic) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        const auto original = session.draftSnapshot();
        const auto material_path = fixture.m_root / "basalt.toml";
        const auto feature_path = fixture.m_root / "landing.toml";
        PPR_TEST_ASSERT(not session.clearDefinitionProperty(
            EContentDocument::materials, "basalt", "appearance.normal_texture", diagnostics));
        const auto cleared = descriptor(session.draftSource(), material_path).m_bytes;
        PPR_TEST_ASSERT(cleared != descriptor(original.m_source, material_path).m_bytes);
        const auto revision = session.revision();
        PPR_TEST_ASSERT(not session.clearDefinitionProperty(
            EContentDocument::materials, "basalt", "appearance.normal_texture", diagnostics));
        PPR_TEST_ASSERT(session.revision() == revision);
        PPR_TEST_ASSERT(
            session.clearDefinitionProperty(
                EContentDocument::materials, "basalt", "appearance.display_name", diagnostics) ==
            config::EConfigErrc::cross_field_invalid);
        PPR_TEST_ASSERT(session.undo().value() and not session.dirty());
        PPR_TEST_ASSERT(session.redo().value());
        PPR_TEST_ASSERT(descriptor(session.draftSource(), material_path).m_bytes == cleared);
        PPR_TEST_ASSERT(session.undo().value());

        const auto before_same_tag = session.revision();
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::features, "landing", "definition.operations.op_01.action.tag",
            config::FileValue::text("carve"), diagnostics));
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::features, "landing", "definition.operations.op_01.action.carve.shape.tag",
            config::FileValue::text("rectangle"), diagnostics));
        PPR_TEST_ASSERT(session.revision() == before_same_tag and not session.dirty());

        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::features, "landing", "definition.operations.op_01.action.carve.shape.tag",
            config::FileValue::text("capsule"), diagnostics));
        const auto invalid = session.draftSnapshot();
        PPR_TEST_ASSERT(not validateContentDraft(invalid, diagnostics));
        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(
            descriptor(session.draftSource(), feature_path).m_bytes ==
            descriptor(original.m_source, feature_path).m_bytes);
        PPR_TEST_ASSERT(session.redo().value());
        PPR_TEST_ASSERT(
            descriptor(session.draftSource(), feature_path).m_bytes ==
            descriptor(invalid.m_source, feature_path).m_bytes);
        PPR_TEST_ASSERT(session.undo().value());

        const auto ellipse = config::decode(
            "tag = 'ellipse'\n[ellipse]\nwidth_fine = 16\nheight_fine = 16\n", "replacement", diagnostics,
            config::EDecodeMode::tree);
        PPR_TEST_ASSERT(ellipse.has_value());
        PPR_TEST_ASSERT(not session.replaceDefinitionSubtree(
            EContentDocument::features, "landing", "definition.operations.op_01.action.carve.shape", *ellipse,
            diagnostics));
        const auto variant = session.draftSnapshot();
        PPR_TEST_ASSERT(validateContentDraft(variant, diagnostics).has_value());
        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(session.redo().value());
        PPR_TEST_ASSERT(
            descriptor(session.draftSource(), feature_path).m_bytes ==
            descriptor(variant.m_source, feature_path).m_bytes);

        const auto temperature = config::decode(
            "field = 'composition'\nbase_k = 301.0\nvariation_k = 2.0\n", "replacement", diagnostics,
            config::EDecodeMode::tree);
        PPR_TEST_ASSERT(temperature.has_value());
        PPR_TEST_ASSERT(not session.replaceDefinitionSubtree(
            EContentDocument::biomes, "hearth", "definition.temperature", *temperature, diagnostics));
        const auto grouped = session.draftSnapshot();
        PPR_TEST_ASSERT(validateContentDraft(grouped, diagnostics).has_value());
        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(session.redo().value());
        PPR_TEST_ASSERT(
            descriptor(session.draftSource(), fixture.m_root / "hearth.toml").m_bytes ==
            descriptor(grouped.m_source, fixture.m_root / "hearth.toml").m_bytes);
        PPR_TEST_ASSERT(
            descriptor(session.draftSource(), material_path).m_bytes ==
            descriptor(original.m_source, material_path).m_bytes);
    };

    PPR_UNIT_TEST (authoring_validation_and_save_preserve_newer_drafts_and_revision_history) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        const auto catalog = session.validatedCatalog();
        const auto baseline_revision = session.validatedRevision();
        PPR_TEST_ASSERT(not session.dirty() and not session.canUndo());
        PPR_TEST_ASSERT(not session.editBiome(
            "hearth", "definition.temperature.base_k", config::FileValue::floating(301.0), diagnostics));
        const auto submitted = session.draftSnapshot();
        auto validated = validateContentDraft(submitted, diagnostics, nullptr, kWorld);
        PPR_TEST_ASSERT(validated.has_value());
        PPR_TEST_ASSERT(session.acceptValidation(submitted.m_revision, validated->m_definitions));

        PPR_TEST_ASSERT(not session.editBiome(
            "hearth", "definition.temperature.base_k", config::FileValue::floating(302.0), diagnostics));
        const auto newer = session.draftSnapshot();
        PPR_TEST_ASSERT(not session.acceptValidation(submitted.m_revision, catalog));
        auto saved = saveContentDraft(submitted, diagnostics);
        PPR_TEST_ASSERT(saved.has_value() and not saved->m_error);
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*saved)) and session.dirty());
        PPR_TEST_ASSERT(
            descriptor(session.draftSource(), fixture.m_root / "hearth.toml").m_bytes ==
            descriptor(newer.m_source, fixture.m_root / "hearth.toml").m_bytes);
        PPR_TEST_ASSERT(session.validatedRevision() != baseline_revision);
        save(session, diagnostics);

        auto reopened = fixture.open(diagnostics);
        PPR_TEST_ASSERT(
            descriptor(reopened.draftSource(), fixture.m_root / "hearth.toml").m_bytes ==
            descriptor(session.draftSource(), fixture.m_root / "hearth.toml").m_bytes);
        PPR_TEST_ASSERT(session.undo().value() and session.dirty());
        PPR_TEST_ASSERT(not session.revert());
        PPR_TEST_ASSERT(not session.dirty() and not session.canUndo() and not session.canRedo());
    };

    // ------------------------------------------------------------------
    // partial saves, ownership conflicts and history restoration
    // ------------------------------------------------------------------

    class CancelAfterDiskCommit final : public IContext {
        std::string m_path, m_expected;
        SharedContext m_parent{context::background()};

    public:
        CancelAfterDiskCommit(const std::filesystem::path &path, std::string expected)
            : m_path{path.string()}, m_expected{std::move(expected)} {
        }

        std::error_code error() const noexcept override {
            // Nonthrowing C I/O observes a physical result, independent of query count.
            std::FILE *file{};
#ifdef _WIN32
            if (::fopen_s(&file, m_path.c_str(), "rb") != 0) {
                return {};
            }
#else
            file = std::fopen(m_path.c_str(), "rb");
#endif

            if (file == nullptr) {
                return {};
            }

            bool matches = true;
            for (const unsigned char byte: m_expected) {
                if (std::fgetc(file) != byte) {
                    matches = false;
                    break;
                }
            }

            const auto ending = std::fgetc(file);
            matches = matches and ending < 0 and std::feof(file) != 0 and std::ferror(file) == 0;
            std::fclose(file);
            return matches ? std::make_error_code(std::errc::operation_canceled) : std::error_code{};
        }

        std::optional<const opaque::Block::Value *> value(const string_literal key) const noexcept override {
            return m_parent->value(key);
        }

        TagPtr<ISignal> subscribeEvent(TagPtr<ISignal> signal) noexcept override {
            return m_parent->subscribeEvent(std::move(signal));
        }

        void unsubscribeEvent(TagPtr<ISignal> signal, TagPtr<ISignal> restore) noexcept override {
            m_parent->unsubscribeEvent(std::move(signal), std::move(restore));
        }

        bool pollEvent() noexcept override {
            return error() != std::error_code{};
        }

        void resetEvent() noexcept override {
            m_parent->resetEvent();
        }
    };

    PPR_UNIT_TEST (cancellation_after_physical_success_retains_every_observed_disk_outcome) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        const auto catalog = session.validatedCatalog();
        PPR_TEST_ASSERT(not session.editBiome(
            "hearth", "definition.temperature.base_k", config::FileValue::floating(301.0), diagnostics));
        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::fields, "composition", "definition.frequency_per_metre",
            config::FileValue::floating(0.02), diagnostics));
        const auto submitted = session.draftSnapshot();
        const auto field_path = fixture.m_root / "composition.toml";
        const auto biome_path = fixture.m_root / "hearth.toml";
        CancelAfterDiskCommit cancellation{field_path, descriptor(submitted.m_source, field_path).m_bytes};
        PPR_TEST_ASSERT(not cancellation.error());

        auto result = saveContentDraft(submitted, diagnostics, std::addressof(cancellation));
        PPR_TEST_ASSERT(
            result.has_value() and result->m_error == std::errc::operation_canceled and not result->m_catalog);
        PPR_TEST_ASSERT(fixture.read("composition.toml") == descriptor(submitted.m_source, field_path).m_bytes);
        PPR_TEST_ASSERT(fixture.read("hearth.toml") == descriptor(submitted.m_source, biome_path).m_bytes);
        PPR_TEST_ASSERT(ledger(result->m_snapshot.m_baseline, field_path).m_bytes == fixture.read("composition.toml"));
        PPR_TEST_ASSERT(ledger(result->m_snapshot.m_baseline, biome_path).m_bytes == fixture.read("hearth.toml"));

        PPR_TEST_ASSERT(not session.editBiome(
            "hearth", "definition.temperature.base_k", config::FileValue::floating(302.0), diagnostics));
        const auto newer = session.draftSnapshot();
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*result)) and session.validatedCatalog() == catalog);
        PPR_TEST_ASSERT(
            descriptor(session.draftSource(), biome_path).m_bytes == descriptor(newer.m_source, biome_path).m_bytes);
        PPR_TEST_ASSERT(session.dirty());
        save(session, diagnostics);
    };

    PPR_UNIT_TEST (save_checks_all_owned_sources_and_never_adopts_external_bytes) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        PPR_TEST_ASSERT(not session.editBiome(
            "hearth", "definition.temperature.base_k", config::FileValue::floating(301.0), diagnostics));
        const auto before = session.draftSnapshot();
        const auto field = fixture.read("composition.toml");
        fixture.write("composition.toml", field + "\n# external untouched descriptor\n");
        const auto conflict = saveContentDraft(before, diagnostics);
        PPR_TEST_ASSERT(not conflict and conflict.error() == config::EConfigErrc::cross_field_invalid);
        PPR_TEST_ASSERT(session.revision() == before.m_revision and session.dirty());
        PPR_TEST_ASSERT(
            fixture.read("hearth.toml") == *ledger(before.m_baseline, fixture.m_root / "hearth.toml").m_bytes);
        PPR_TEST_ASSERT(descriptor(session.draftSource(), fixture.m_root / "composition.toml").m_bytes == field);

        fixture.write("composition.toml", field);
        fixture.write("pack.toml", fixture.m_manifest + "\n# external manifest\n");
        const auto manifest_conflict = saveContentDraft(before, diagnostics);
        PPR_TEST_ASSERT(
            not manifest_conflict and manifest_conflict.error() == config::EConfigErrc::cross_field_invalid);

        fixture.write("pack.toml", fixture.m_manifest);
        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = saveContentDraft(before, diagnostics, job_context.get());
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        PPR_TEST_ASSERT(
            fixture.read("hearth.toml") == *ledger(before.m_baseline, fixture.m_root / "hearth.toml").m_bytes);
    };

    PPR_UNIT_TEST (descriptor_failure_blocks_manifest_and_obsolete_removal) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        PPR_TEST_ASSERT(not session.cloneDefinition(EContentDocument::fields, "composition", "x", diagnostics));
        PPR_TEST_ASSERT(not session.cloneDefinition(EContentDocument::fields, "composition", "y", diagnostics));
        PPR_TEST_ASSERT(not session.deleteDefinition(EContentDocument::materials, "spare", diagnostics));
        const auto x = target(session, "x", sim::asset::kFieldKind, diagnostics);
        const auto y = target(session, "y", sim::asset::kFieldKind, diagnostics);
        const auto y_temporary = y.lexically_relative(fixture.m_root).generic_string() + ".workshop.tmp";
        fixture.write(y_temporary, "another owner");
        const auto before = session.draftSnapshot();
        const auto catalog = session.validatedCatalog();
        auto result = saveContentDraft(before, diagnostics);
        PPR_TEST_ASSERT(result.has_value() and result->m_error == std::errc::io_error and not result->m_catalog);
        PPR_TEST_ASSERT(std::filesystem::exists(x) and not std::filesystem::exists(y));
        PPR_TEST_ASSERT(fixture.read("pack.toml") == fixture.m_manifest);
        PPR_TEST_ASSERT(std::filesystem::exists(fixture.m_root / "Odd-Original.TOML"));
        PPR_TEST_ASSERT(ledger(result->m_snapshot.m_baseline, x).m_bytes.has_value());
        PPR_TEST_ASSERT(not ledger(result->m_snapshot.m_baseline, y).m_bytes.has_value());
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*result)));
        PPR_TEST_ASSERT(session.validatedCatalog() == catalog and session.dirty());
        PPR_TEST_ASSERT(fixture.read(y_temporary) == "another owner");
        PPR_TEST_ASSERT(std::filesystem::remove(fixture.m_root / y_temporary));
        save(session, diagnostics);
        PPR_TEST_ASSERT(not std::filesystem::exists(fixture.m_root / "Odd-Original.TOML"));
    };

    PPR_UNIT_TEST (manifest_failure_retains_orphan_ledger_and_newer_x_and_y_intent) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        PPR_TEST_ASSERT(not session.cloneDefinition(EContentDocument::fields, "composition", "x", diagnostics));
        const auto x = target(session, "x", sim::asset::kFieldKind, diagnostics);
        const auto submitted = session.draftSnapshot();
        fixture.write("pack.toml.workshop.tmp", "another owner");
        auto partial = saveContentDraft(submitted, diagnostics);
        PPR_TEST_ASSERT(partial.has_value() and partial->m_error == std::errc::io_error);
        PPR_TEST_ASSERT(std::filesystem::exists(x));
        PPR_TEST_ASSERT(fixture.read("pack.toml") == fixture.m_manifest);

        PPR_TEST_ASSERT(not session.editDefinition(
            EContentDocument::fields, "x", "definition.frequency_per_metre", config::FileValue::floating(0.02),
            diagnostics));
        PPR_TEST_ASSERT(not session.cloneDefinition(EContentDocument::fields, "composition", "y", diagnostics));
        const auto y = target(session, "y", sim::asset::kFieldKind, diagnostics);
        const auto newer = session.draftSnapshot();
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*partial)));
        PPR_TEST_ASSERT(ledger(session.baselineSource(), x).m_bytes == descriptor(submitted.m_source, x).m_bytes);
        PPR_TEST_ASSERT(not ledger(session.baselineSource(), y).m_bytes);
        PPR_TEST_ASSERT(descriptor(session.draftSource(), x).m_bytes == descriptor(newer.m_source, x).m_bytes);
        PPR_TEST_ASSERT(descriptor(session.draftSource(), y).m_bytes == descriptor(newer.m_source, y).m_bytes);

        PPR_TEST_ASSERT(std::filesystem::remove(fixture.m_root / "pack.toml.workshop.tmp"));
        save(session, diagnostics);
        PPR_TEST_ASSERT(
            fixture.read(x.lexically_relative(fixture.m_root).generic_string()) ==
            descriptor(newer.m_source, x).m_bytes);
        PPR_TEST_ASSERT(std::filesystem::exists(y));
        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(descriptor(session.draftSource(), x).m_bytes == descriptor(submitted.m_source, x).m_bytes);
        PPR_TEST_ASSERT(session.dirty());
        save(session, diagnostics);
        PPR_TEST_ASSERT(not std::filesystem::exists(y));
    };

    PPR_UNIT_TEST (saved_delete_undo_restores_exact_arbitrary_path_and_reappearance_conflicts) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        const auto path = fixture.m_root / "Odd-Original.TOML";
        const auto original = fixture.read("Odd-Original.TOML");
        PPR_TEST_ASSERT(not session.deleteDefinition(EContentDocument::materials, "spare", diagnostics));
        save(session, diagnostics);
        PPR_TEST_ASSERT(not std::filesystem::exists(path));
        PPR_TEST_ASSERT(not ledger(session.baselineSource(), path).m_bytes);
        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(target(session, "spare", sim::asset::kMaterialKind, diagnostics) == path);
        save(session, diagnostics);
        PPR_TEST_ASSERT(fixture.read("Odd-Original.TOML") == original);

        PPR_TEST_ASSERT(session.redo().value());
        save(session, diagnostics);
        PPR_TEST_ASSERT(session.undo().value());
        fixture.write("Odd-Original.TOML", original);
        const auto reappeared = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(not reappeared and reappeared.error() == config::EConfigErrc::cross_field_invalid);
        PPR_TEST_ASSERT(session.dirty() and fixture.read("Odd-Original.TOML") == original);
    };

    PPR_UNIT_TEST (obsolete_orphan_cleanup_blocks_fresh_growth_but_desired_orphans_are_reusable) {
#ifdef _WIN32
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto session = fixture.open(diagnostics);
        PPR_TEST_ASSERT(not session.cloneDefinition(EContentDocument::fields, "composition", "x", diagnostics));
        const auto x = target(session, "x", sim::asset::kFieldKind, diagnostics);
        fixture.write("pack.toml.workshop.tmp", "another owner");
        auto orphaned = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(orphaned.has_value() and orphaned->m_error == std::errc::io_error);
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*orphaned)));
        PPR_TEST_ASSERT(std::filesystem::remove(fixture.m_root / "pack.toml.workshop.tmp"));

        // MSVC streams allow reading but deny deletion/replacement of this file.
        std::ifstream locked{x, std::ios::binary};
        PPR_TEST_ASSERT(locked.good());
        save(session, diagnostics);
        PPR_TEST_ASSERT(ledger(session.baselineSource(), x).m_bytes.has_value());
        locked.close();

        PPR_TEST_ASSERT(session.undo().value());
        fixture.write("pack.toml.workshop.tmp", "another owner");
        auto removed_manifest = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(removed_manifest.has_value() and removed_manifest->m_error == std::errc::io_error);
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*removed_manifest)));
        PPR_TEST_ASSERT(std::filesystem::remove(fixture.m_root / "pack.toml.workshop.tmp"));
        save(session, diagnostics);
        PPR_TEST_ASSERT(not std::filesystem::exists(x));

        PPR_TEST_ASSERT(session.redo().value());
        fixture.write("pack.toml.workshop.tmp", "another owner");
        auto unpublished = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(unpublished.has_value() and unpublished->m_error == std::errc::io_error);
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*unpublished)));
        PPR_TEST_ASSERT(std::filesystem::remove(fixture.m_root / "pack.toml.workshop.tmp"));
        PPR_TEST_ASSERT(session.undo().value());
        PPR_TEST_ASSERT(not session.cloneDefinition(EContentDocument::fields, "composition", "y", diagnostics));
        const auto y = target(session, "y", sim::asset::kFieldKind, diagnostics);
        locked.open(x, std::ios::binary);
        PPR_TEST_ASSERT(locked.good());
        auto blocked = saveContentDraft(session.draftSnapshot(), diagnostics);
        PPR_TEST_ASSERT(blocked.has_value() and blocked->m_error != std::error_code{});
        PPR_TEST_ASSERT(not std::filesystem::exists(y) and std::filesystem::exists(x));
        PPR_TEST_ASSERT(not session.acceptSave(std::move(*blocked)));

        locked.close();
        save(session, diagnostics);
        PPR_TEST_ASSERT(not std::filesystem::exists(x) and std::filesystem::exists(y));
#endif
    };

    // ------------------------------------------------------------------
    // queued workshop requests and external-owner teardown
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (workshop_queued_requests_preserve_newer_drafts_and_require_current_discard_authority) {
        struct Clock final : ITimerClock {
            TimePoint m_now{};

            TimePoint now() const noexcept override {
                return m_now;
            }
        } clock;

        Fixture fixture;
        useSmallIdentityImages(fixture);
        ParallelScheduler pool{"Workshop request contracts", 1u, static_cast<std::size_t>(hal::page_granularity)};
        WorldgenWorkshop workshop{pool, clock};
        const auto settle = [&]() {
            while (workshop.busy()) {
                PPR_TEST_ASSERT(not workshop.update());
                std::this_thread::yield();
            }
        };

        PPR_TEST_ASSERT(not workshop.open(fixture.m_root / "missing"));
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root));
        PPR_TEST_ASSERT(workshop.busy() and workshop.session() == nullptr);
        workshop.cancel();
        PPR_TEST_ASSERT(not workshop.busy());
        PPR_TEST_ASSERT(not workshop.update() and workshop.session() == nullptr);
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root));
        settle();
        auto *const session = workshop.session();
        PPR_TEST_ASSERT(session != nullptr and not session->dirty());
        const auto original = session->draftSnapshot();
        const auto catalog = session->validatedCatalog();
        const auto validated_revision = session->validatedRevision();
        config::Diagnostics diagnostics;
        PPR_TEST_ASSERT(not session->editBiome(
            "hearth", "definition.temperature.base_k", config::FileValue::floating(301.0), diagnostics));
        const auto submitted = session->draftSnapshot();
        PPR_TEST_ASSERT(not workshop.validate());
        PPR_TEST_ASSERT(not session->editBiome(
            "hearth", "definition.temperature.base_k", config::FileValue::floating(302.0), diagnostics));
        const auto newer = session->draftSnapshot();
        settle();
        PPR_TEST_ASSERT(workshop.session() == session and session->revision() == newer.m_revision);
        PPR_TEST_ASSERT(session->validatedRevision() == validated_revision and session->validatedCatalog() == catalog);
        PPR_TEST_ASSERT(
            descriptor(session->draftSource(), fixture.m_root / "hearth.toml").m_bytes ==
            descriptor(newer.m_source, fixture.m_root / "hearth.toml").m_bytes);

        PPR_TEST_ASSERT(workshop.open(fixture.m_root) == std::errc::operation_not_permitted);
        PPR_TEST_ASSERT(session->undo().value());
        PPR_TEST_ASSERT(
            descriptor(session->draftSource(), fixture.m_root / "hearth.toml").m_bytes ==
            descriptor(submitted.m_source, fixture.m_root / "hearth.toml").m_bytes);
        PPR_TEST_ASSERT(session->redo().value());

        const auto discard = session->revision();
        PPR_TEST_ASSERT(not workshop.open(fixture.m_root, discard));
        PPR_TEST_ASSERT(not session->editBiome(
            "hearth", "definition.temperature.base_k", config::FileValue::floating(303.0), diagnostics));
        const auto after_authorization = session->revision();
        settle();
        PPR_TEST_ASSERT(workshop.session() == session and session->revision() == after_authorization);
        PPR_TEST_ASSERT(session->dirty() and session->canUndo());

        PPR_TEST_ASSERT(not workshop.open(fixture.m_root, session->revision()));
        settle();
        auto *const reopened = workshop.session();
        PPR_TEST_ASSERT(reopened != nullptr and not reopened->dirty());
        PPR_TEST_ASSERT(not reopened->canUndo() and not reopened->canRedo());
        PPR_TEST_ASSERT(
            descriptor(reopened->draftSource(), fixture.m_root / "hearth.toml").m_bytes ==
            descriptor(original.m_source, fixture.m_root / "hearth.toml").m_bytes);

        WorldgenPreviewSpec spec{std::string{kWorld}, "hearth", 42u, reopened->revision()};
        workshop.configureLivePreview(true, spec, EGridDiagnosticMode::material);
        clock.m_now += std::chrono::milliseconds{349};
        PPR_TEST_ASSERT(not workshop.update() and not workshop.busy());
        clock.m_now += std::chrono::milliseconds{1};
        PPR_TEST_ASSERT(not workshop.update() and workshop.busy());
        workshop.cancel();
        settle();
        PPR_TEST_ASSERT(workshop.preview() == nullptr);
        clock.m_now += std::chrono::seconds{1};
        workshop.configureLivePreview(true, spec, EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not workshop.update() and not workshop.busy());

        workshop.configureLivePreview(false, spec, EGridDiagnosticMode::material);
        workshop.configureLivePreview(true, spec, EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not workshop.requestPreview(spec, EGridDiagnosticMode::material));
        clock.m_now += std::chrono::seconds{1};
        PPR_TEST_ASSERT(not workshop.update() and workshop.busy());
        settle();
        const auto preview = workshop.previewHandle();
        PPR_TEST_ASSERT(preview and preview->m_spec.m_seed == spec.m_seed and preview->m_presentation);
        PPR_TEST_ASSERT(preview->m_presentation_generation == preview->m_presentation->generation());
        PPR_TEST_ASSERT(*preview->m_presentation_generation != preview->m_spec.m_revision);

        const auto stage = workshop.takePresentation();
        PPR_TEST_ASSERT(stage and stage->m_ack_seed == *preview->m_presentation_generation);
        PPR_TEST_ASSERT(stage->m_content_fingerprint == preview->m_presentation->fingerprint());
        PPR_TEST_ASSERT(
            not exportWorldgenPreview(*preview, reopened->draftSource(), {}, fixture.m_root / "workshop-export"));

        PPR_TEST_ASSERT(not workshop.update() and not workshop.busy());
        PPR_TEST_ASSERT(workshop.previewHandle() == preview);

        const auto opened_epoch = reopened->sourceEpoch();
        PPR_TEST_ASSERT(preview->m_presentation->sourceEpoch() == opened_epoch);
        PPR_TEST_ASSERT(preview->m_presentation->work().m_read_count == 2u);
        PPR_TEST_ASSERT(not workshop.requestPreview(spec, EGridDiagnosticMode::material));
        PPR_TEST_ASSERT(not workshop.update());
        PPR_TEST_ASSERT(not workshop.requestPreview(spec, EGridDiagnosticMode::material));
        settle();
        const auto reused_preview = workshop.previewHandle();
        PPR_TEST_ASSERT(reused_preview and reused_preview != preview);
        PPR_TEST_ASSERT(reused_preview->m_presentation_generation != preview->m_presentation_generation);
        PPR_TEST_ASSERT(reused_preview->m_presentation->sourceEpoch() == opened_epoch);
        PPR_TEST_ASSERT(
            reused_preview->m_presentation->work().m_read_count == 0u and
            reused_preview->m_presentation->work().m_reused_count == 2u);
        PPR_TEST_ASSERT(reused_preview->m_presentation->catalog() == reused_preview->m_catalog);
        PPR_TEST_ASSERT(
            reused_preview->m_presentation->textures().front().m_image.m_storage.getBufferData().data() ==
            preview->m_presentation->textures().front().m_image.m_storage.getBufferData().data());

        workshop.configureLivePreview(false, spec, EGridDiagnosticMode::material);
        PPR_TEST_ASSERT(not reopened->editBiome(
            "hearth", "definition.composition.cover.bed_dip", config::FileValue::floating(1.0), diagnostics));
        PPR_TEST_ASSERT(not workshop.validate());
        settle();
        PPR_TEST_ASSERT(workshop.draftInvalid() and not workshop.propertyDiagnostics().empty());
        PPR_TEST_ASSERT(std::ranges::any_of(workshop.propertyDiagnostics(), [&](const auto &diagnostic) {
            return diagnostic.m_property == "definition.composition.cover.bed_dip" and
                   diagnostic.m_location.starts_with((fixture.m_root / "hearth.toml").string()) and
                   not diagnostic.m_message.empty();
        }));

        PPR_TEST_ASSERT(reopened->undo().value());
        PPR_TEST_ASSERT(workshop.propertyDiagnostics().empty());

        PPR_TEST_ASSERT(not workshop.open(fixture.m_root, reopened->revision()));
        settle();
        auto *const reloaded = workshop.session();
        PPR_TEST_ASSERT(reloaded and reloaded->sourceEpoch() != opened_epoch);
        spec.m_revision = reloaded->revision();
        PPR_TEST_ASSERT(not workshop.requestPreview(spec, EGridDiagnosticMode::material));
        settle();
        const auto fresh_preview = workshop.previewHandle();
        PPR_TEST_ASSERT(fresh_preview and fresh_preview->m_presentation->sourceEpoch() == reloaded->sourceEpoch());
        PPR_TEST_ASSERT(
            fresh_preview->m_presentation->work().m_read_count == 2u and
            fresh_preview->m_presentation->work().m_reused_count == 0u);

        PPR_TEST_ASSERT(not workshop.validate());
        // Shutdown drains an admitted request while the explicit scheduler still lives.
        PPR_TEST_ASSERT(not workshop.update());
        PPR_TEST_ASSERT(not workshop.shutdown());
        PPR_TEST_ASSERT(not workshop.busy());
        PPR_TEST_ASSERT(preview->m_presentation->textures().front().m_image.m_storage.isOwned());
    };
}

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // descriptor pack and authoring suite
    // ------------------------------------------------------------------

    const UnitTest pack_content = UnitTest::Named("pack_content") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::pack::catalog_dig_respects_protected_material_and_preflights_the_complete_rectangle,
            detail::pack::construction_checks_complete_tiles_reservations_and_live_actor_overlap,
            detail::pack::errand_preflight_rejects_invalid_actor_before_any_demolition_writes,
            detail::pack::data_only_material_extension_reaches_raster_tools_and_colliders,
            detail::pack::prepared_world_rejects_invalid_inputs_and_cancellation_before_generation,
            detail::pack::numeric_streams_ignore_manifest_order_comments_and_unused_assets,
            detail::pack::identity_separates_numeric_appearance_descriptor_and_binary_changes,
            detail::pack::explicit_presentation_owns_distinct_usages_and_survives_source_removal,
            detail::pack::definition_edits_recompose_current_presentation_with_unchanged_owned_images,
            detail::pack::texture_setting_edit_prepares_one_image_while_null_epoch_disables_reuse,
            detail::pack::independent_open_epochs_force_binary_reread_even_with_equal_size_and_time,
            detail::pack::required_texture_limits_missing_and_cancel_are_errors,
            detail::pack::earlier_mip_failure_precedes_later_texture_preparation_failure,
            detail::pack::old_formats_ranges_references_and_versions_reject_without_binary_work,
            detail::pack::valid_selected_world_does_not_hide_broken_unselected_world,
            detail::pack::public_snapshots_reject_null_and_duplicate_ownership_records,
            detail::pack::multi_pack_authoring_requires_qualified_names_and_checks_untouched_packs,
            detail::pack::workshop_property_metadata_tracks_descriptor_schemas_and_reference_categories,
            detail::pack::authoring_all_descriptor_categories_clone_delete_and_round_trip,
            detail::pack::authoring_optional_group_and_variant_transactions_are_atomic,
            detail::pack::authoring_validation_and_save_preserve_newer_drafts_and_revision_history,
            detail::pack::cancellation_after_physical_success_retains_every_observed_disk_outcome,
            detail::pack::save_checks_all_owned_sources_and_never_adopts_external_bytes,
            detail::pack::descriptor_failure_blocks_manifest_and_obsolete_removal,
            detail::pack::manifest_failure_retains_orphan_ledger_and_newer_x_and_y_intent,
            detail::pack::saved_delete_undo_restores_exact_arbitrary_path_and_reappearance_conflicts,
            detail::pack::obsolete_orphan_cleanup_blocks_fresh_growth_but_desired_orphans_are_reusable,
            detail::pack::workshop_queued_requests_preserve_newer_drafts_and_require_current_discard_authority,
        });
    };

    const UnitTest &gamePackContentTests() noexcept {
        return pack_content;
    }
}
