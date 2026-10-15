module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module game.tests;

import engine.asset;
import engine.core;
import engine.sim;
import engine.image;
import game.colony.content;
import std;

namespace pP::game::tests::detail::descriptors {
    PPR_DEFINE_LOG_CATEGORY(DescriptorTests, debug, none);

    // ------------------------------------------------------------------
    // independent physical pack fixtures
    // ------------------------------------------------------------------

    struct Fixture {
        std::filesystem::path m_root, m_base, m_shared;

        Fixture() {
            static std::atomic<u32> ordinal{};
            const auto parent = hal::process::currentExecutablePath().parent_path();
            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                const auto candidate = parent / ("descriptor-pack-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(candidate)) {
                    m_root = candidate;
                    break;
                }
            }

            PPR_TEST_ASSERT(not m_root.empty());

            m_base = m_root / "base";
            m_shared = m_root / "shared";
            PPR_TEST_ASSERT(std::filesystem::create_directory(m_base));
            PPR_TEST_ASSERT(std::filesystem::create_directory(m_shared));

            write(m_base / "pack.toml", R"(schema_version = 1
pack_id = "ppr.base"
[requires.shared]
pack_id = "ppr.shared"
[assets.world]
kind = "sim.world"
descriptor = "world.toml"
[assets.rock]
kind = "sim.material"
descriptor = "rock.toml"
[assets.rock_copy]
kind = "sim.material"
descriptor = "rock_copy.toml"
[assets.skies]
kind = "colony.background"
descriptor = "skies.toml"
)");

            write(m_shared / "pack.toml", R"(schema_version = 1
pack_id = "ppr.shared"
[assets.solid]
kind = "sim.element"
descriptor = "solid.toml"
[assets.albedo]
kind = "image.texture"
descriptor = "albedo.toml"
[assets.noise]
kind = "sim.field"
descriptor = "noise.toml"
[assets.hearth]
kind = "sim.biome"
descriptor = "hearth.toml"
[assets.landing]
kind = "sim.feature"
descriptor = "landing.toml"
[assets.cave]
kind = "sim.rule"
descriptor = "cave.toml"
)");

            sim::asset::ElementDescriptor element;
            element.m_schema_version = 1u;
            element.m_density = 2000.0;
            element.m_phase = sim::EGenerationMatter::solid;
            write(m_shared / "solid.toml", element);

            image::TextureDescriptor texture;
            texture.m_schema_version = 1u;
            texture.m_source = "images/missing.png";
            write(m_shared / "albedo.toml", texture);

            colony::MaterialAssetDescriptor material;
            material.m_schema_version = 1u;
            material.m_element = "ppr.shared:solid";
            material.m_diggable = true;
            material.m_appearance.m_display_name = "Rock";
            material.m_appearance.m_color_rgba = "808080ff";
            material.m_appearance.m_albedo_texture = "ppr.shared:albedo";
            write(m_base / "rock.toml", material);

            material.m_appearance.m_display_name = "Other Rock";
            write(m_base / "rock_copy.toml", material);

            sim::asset::FieldDescriptor field;
            field.m_schema_version = 1u;
            field.m_definition.m_frequency_per_metre = 0.01;
            write(m_shared / "noise.toml", field);

            colony::BiomeAssetDescriptor biome;
            biome.m_schema_version = 1u;
            biome.m_appearance.m_display_name = "Hearth";
            biome.m_appearance.m_preview_color_rgba = "808080ff";
            biome.m_definition.m_temperature = {{"noise"}, 300.0, 1.0};
            sim::definition::CompositionBand band;
            band.m_field.m_key = "noise";
            band.m_materials.m_entries.push_back({"stone", {{"rock"}, 1.0}});
            biome.m_definition.m_composition.m_entries.push_back({"cover", std::move(band)});
            write(m_shared / "hearth.toml", biome);

            colony::FeatureAssetDescriptor feature;
            feature.m_schema_version = 1u;
            feature.m_appearance.m_display_name = "Landing";
            feature.m_definition.m_reservation = {0, 0, 16, 16};
            sim::definition::Operation operation;
            operation.m_action = sim::definition::Paint{{"rock"}, sim::definition::Rectangle{16u, 16u}};
            feature.m_definition.m_operations.m_entries.push_back({"fill", std::move(operation)});
            feature.m_definition.m_markers.m_entries.push_back({"spawn", {sim::EGenerationMarker::start, 8, 8}});
            write(m_shared / "landing.toml", feature);

            sim::asset::RuleDescriptor rule;
            rule.m_schema_version = 1u;
            sim::definition::Cave cave;
            cave.m_biome.m_key = "hearth";
            cave.m_field.m_key = "noise";
            rule.m_definition.m_payload = cave;
            write(m_shared / "cave.toml", rule);

            colony::BackgroundAssetDescriptor background;
            background.m_schema_version = 1u;
            for (u32 index = 0u; index < 4u; ++index) {
                colony::BackgroundAppearanceLayer layer;
                layer.m_order = index;
                layer.m_texture = "ppr.shared:albedo";
                layer.m_color_rgba = "ffffffff";
                layer.m_repeat_period_m = 1.0;
                background.m_definition.m_layers.m_entries.push_back(
                    {"layer_" + std::to_string(index), std::move(layer)});
            }

            write(m_base / "skies.toml", background);

            colony::WorldAssetDescriptor world;
            world.m_schema_version = 1u;
            world.m_semantic_key = "frontier";
            world.m_appearance.m_display_name = "Frontier";
            world.m_appearance.m_background_stack = "skies";
            world.m_palette.m_materials.m_entries = {{"rock", {"rock", 2u}}, {"rock_copy", {"rock_copy", 1u}}};
            world.m_palette.m_fields.m_entries.push_back({"noise", {"ppr.shared:noise", 0u}});
            world.m_palette.m_biomes.m_entries.push_back({"hearth", {"ppr.shared:hearth", 0u}});
            world.m_palette.m_features.m_entries.push_back({"landing", {"ppr.shared:landing", 0u}});
            world.m_palette.m_rules.m_entries.push_back({"cave", {"ppr.shared:cave"}});
            world.m_profile.m_layout = {1u, 1u, 16u, 1.0};
            world.m_profile.m_boundaries = {{"rock"}, 1u, 0.0, {"noise"}, {"noise"}};
            world.m_profile.m_start.m_biome.m_key = "hearth";
            world.m_profile.m_start.m_feature.m_key = "landing";
            world.m_profile.m_start.m_marker.m_key = "spawn";
            world.m_profile.m_start.m_x_fine = 128u;
            world.m_profile.m_start.m_y_fine = 128u;
            world.m_profile.m_start.m_clearance_m = 1.0;
            world.m_profile.m_start.m_route_width_m = 1.0;
            sim::definition::BiomeChoice choice;
            choice.m_biome.m_key = "hearth";
            choice.m_required_count = 1u;
            choice.m_distance_max_m = 1024.0;
            world.m_profile.m_biomes.m_entries.push_back({"hearth", std::move(choice)});
            write(m_base / "world.toml", world);
        }

        ~Fixture() {
            // Best-effort fixture teardown; all imported catalogs own their source strings.
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
        }

        static void write(const std::filesystem::path &path, const std::string_view bytes) {
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            PPR_TEST_ASSERT(stream.good());
        }

        template<typename T>
            requires requires { T::describe(); }
        static void write(const std::filesystem::path &path, const T &value) {
            auto document = config::documentFrom(value);
            PPR_TEST_ASSERT(document.has_value());

            auto encoded = config::encode(*document);
            PPR_TEST_ASSERT(encoded.has_value());

            write(path, std::string_view{*encoded});
        }

        Expected<std::shared_ptr<const asset::PackSet>> packs(config::Diagnostics &diagnostics) const {
            auto base = asset::readPackSource(m_base, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(DescriptorTests, base);

            auto shared = asset::readPackSource(m_shared, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(DescriptorTests, shared);

            Array<asset::PackSource> sources{std::move(*base), std::move(*shared)};
            auto indexed = asset::PackSet::index(std::move(sources), colony::kContentAssetKinds, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(DescriptorTests, indexed);

            return std::make_shared<const asset::PackSet>(std::move(*indexed));
        }

        Expected<colony::ContentDescriptorCatalog> catalog(config::Diagnostics &diagnostics) const {
            auto indexed = packs(diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(DescriptorTests, indexed);

            return colony::ContentDescriptorCatalog::importDefinitions(std::move(*indexed), diagnostics);
        }
    };

    // ------------------------------------------------------------------
    // actual cross-module import and ownership
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (two_pack_world_definitions_import_without_binary_sources) {
        static_assert(not std::same_as<asset::AssetKey, image::TextureId>);
        static_assert(not std::same_as<sim::asset::MaterialId, sim::asset::ElementId>);
        Fixture fixture;
        config::Diagnostics diagnostics{16u};
        auto catalog = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        PPR_TEST_ASSERT(catalog->packs().packs().size() == 2u);
        PPR_TEST_ASSERT(catalog->worlds().size() == 1u);
        PPR_TEST_ASSERT(catalog->materials().size() == 2u);
        PPR_TEST_ASSERT(catalog->elements().definitions().size() == 1u);
        PPR_TEST_ASSERT(catalog->textures().definitions().size() == 1u);

        const auto &world = catalog->worlds().front().m_descriptor.m_value;
        PPR_TEST_ASSERT(world.m_semantic_key == "frontier");
        PPR_TEST_ASSERT(world.m_palette.m_materials.m_entries.front().m_value.m_id == 2u);
        PPR_TEST_ASSERT(world.m_profile.m_start.m_biome.m_key == "hearth");
        PPR_TEST_ASSERT(world.m_palette.m_biomes.m_entries.front().m_value.m_asset == "ppr.shared:hearth");
        PPR_TEST_ASSERT(not std::filesystem::exists(fixture.m_shared / "images/missing.png"));

        const auto &first = catalog->materials().front();
        const auto &second = catalog->materials().back();
        PPR_TEST_ASSERT(*first.m_id != *second.m_id);
        PPR_TEST_ASSERT(first.m_descriptor.m_value.m_element == second.m_descriptor.m_value.m_element);
        PPR_TEST_ASSERT(first.m_descriptor.m_source.m_path.parent_path() == fixture.m_base);
        PPR_TEST_ASSERT(
            catalog->textures().definitions().front().m_descriptor.m_source.m_path.parent_path() == fixture.m_shared);
    };

    PPR_UNIT_TEST (typed_values_and_provenance_survive_pack_input_and_file_removal) {
        config::Diagnostics diagnostics{16u};
        std::optional<colony::ContentDescriptorCatalog> retained;
        {
            Fixture fixture;
            auto imported = fixture.catalog(diagnostics);
            PPR_TEST_ASSERT(imported.has_value());

            retained.emplace(std::move(*imported));
        }

        {
            const auto scratch = mem::ScratchPad::open();
            Array<std::string, mem::ScratchPad> reuse(128u, std::string(128u, 'x'));
        }

        PPR_TEST_ASSERT(retained->materials().front().m_descriptor.m_value.m_appearance.m_display_name == "Rock");
        PPR_TEST_ASSERT(not retained->worlds().front().m_descriptor.m_origins.empty());
        PPR_TEST_ASSERT(retained->worlds().front().m_descriptor.m_source.m_bytes.find("frontier") != std::string::npos);
        PPR_TEST_ASSERT(retained->packs().packs().size() == 2u);
    };

    PPR_UNIT_TEST (corrupt_binary_is_outside_definition_validation) {
        Fixture fixture;
        PPR_TEST_ASSERT(std::filesystem::create_directory(fixture.m_shared / "images"));
        Fixture::write(fixture.m_shared / "images/missing.png", "This is not an encoded image.");

        config::Diagnostics diagnostics{16u};
        auto catalog = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (physical_material_composition_keeps_shared_element_and_validates_roles) {
        sim::asset::Element element{2000.0, sim::EGenerationMatter::solid};
        sim::asset::MaterialDescriptor material;
        material.m_diggable = true;
        config::Diagnostics diagnostics{8u};
        const auto terrain = sim::asset::composeMaterial(material, element, diagnostics, "material.toml");
        PPR_TEST_ASSERT(terrain.has_value());
        PPR_TEST_ASSERT(terrain->m_density == element.m_density);
        PPR_TEST_ASSERT(terrain->m_phase == element.m_phase);
        PPR_TEST_ASSERT(terrain->m_diggable == true);

        material.m_representation = sim::definition::EMaterialRepresentation::particle;
        PPR_TEST_ASSERT(not sim::asset::composeMaterial(material, element, diagnostics, "material.toml"));

        material.m_diggable.reset();
        PPR_TEST_ASSERT(sim::asset::composeMaterial(material, element, diagnostics, "material.toml").has_value());

        element.m_density = std::numeric_limits<double>::quiet_NaN();
        PPR_TEST_ASSERT(not sim::asset::composeMaterial(material, element, diagnostics, "material.toml"));
    };

    // ------------------------------------------------------------------
    // descriptor errors and physical containment
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (wrong_kind_reference_reports_owned_descriptor_property) {
        Fixture fixture;
        Fixture::write(fixture.m_base / "rock.toml", R"(schema_version = 1
element = "ppr.shared:albedo"
representation = "terrain"
diggable = true
[appearance]
display_name = "Rock"
color_rgba = "808080ff"
)");

        config::Diagnostics diagnostics{16u};
        auto rejected = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(not rejected and rejected.error() == config::EConfigErrc::type_mismatch);
        PPR_TEST_ASSERT(diagnostics.entries().back().m_property == "element");
        PPR_TEST_ASSERT(diagnostics.entries().back().m_location == (fixture.m_base / "rock.toml").string());
    };

    PPR_UNIT_TEST (duplicate_and_missing_world_binding_indices_reject) {
        Fixture fixture;
        config::Diagnostics diagnostics{16u};
        auto original = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(original.has_value());

        auto world = original->worlds().front().m_descriptor.m_value;
        world.m_palette.m_materials.m_entries.back().m_value.m_id = 2u;
        Fixture::write(fixture.m_base / "world.toml", world);
        PPR_TEST_ASSERT(not fixture.catalog(diagnostics));

        world.m_palette.m_materials.m_entries.back().m_value.m_id = 3u;
        Fixture::write(fixture.m_base / "world.toml", world);
        PPR_TEST_ASSERT(not fixture.catalog(diagnostics));
    };

    PPR_UNIT_TEST (binary_source_symlink_and_descriptor_symlink_cannot_escape_pack) {
        Fixture fixture;
        config::Diagnostics diagnostics{16u};
        auto indexed = fixture.packs(diagnostics);
        PPR_TEST_ASSERT(indexed.has_value());

        std::error_code error;
        std::filesystem::create_directory_symlink(fixture.m_shared, fixture.m_base / "escaped", error);
        PPR_TEST_ASSERT(not error);
        PPR_TEST_ASSERT(not (*indexed)->containedPath("ppr.base", "escaped/albedo.toml", diagnostics));

        std::filesystem::remove(fixture.m_shared / "albedo.toml");
        std::filesystem::create_symlink(fixture.m_base / "rock.toml", fixture.m_shared / "albedo.toml", error);
        PPR_TEST_ASSERT(not error);
        PPR_TEST_ASSERT(not fixture.catalog(diagnostics));
    };

    PPR_UNIT_TEST (unsupported_image_settings_fail_before_source_io) {
        Fixture fixture;
        Fixture::write(fixture.m_shared / "albedo.toml", R"(schema_version = 1
source = "images/missing.png"
interpretation = "color"
mip_policy = "unsupported"
)");

        config::Diagnostics diagnostics{16u};
        const auto rejected = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(not rejected and rejected.error() == config::EConfigErrc::unknown_enum_value);

        image::TextureDescriptor normal;
        normal.m_schema_version = 1u;
        normal.m_source = "images/missing.png";
        normal.m_interpretation = image::ETextureInterpretation::normal;
        normal.m_preserve_coverage = true;
        Fixture::write(fixture.m_shared / "albedo.toml", normal);
        PPR_TEST_ASSERT(not fixture.catalog(diagnostics));
    };
}

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // descriptor seam suite
    // ------------------------------------------------------------------

    const UnitTest content_descriptors = UnitTest::Named("content_descriptors") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::descriptors::two_pack_world_definitions_import_without_binary_sources,
            detail::descriptors::typed_values_and_provenance_survive_pack_input_and_file_removal,
            detail::descriptors::corrupt_binary_is_outside_definition_validation,
            detail::descriptors::physical_material_composition_keeps_shared_element_and_validates_roles,
            detail::descriptors::wrong_kind_reference_reports_owned_descriptor_property,
            detail::descriptors::duplicate_and_missing_world_binding_indices_reject,
            detail::descriptors::binary_source_symlink_and_descriptor_symlink_cannot_escape_pack,
            detail::descriptors::unsupported_image_settings_fail_before_source_io,
        });
    };

    const UnitTest &gameContentDescriptorTests() noexcept {
        return content_descriptors;
    }
}
