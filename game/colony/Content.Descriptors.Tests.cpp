module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module game.tests;

import engine.asset;
import engine.core;
import engine.sim;
import engine.image;
import game.colony.content;
import game.colony.worldgen;
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
            material.m_appearance.m_texture_period_m = 1.0;
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
            // The assembly tests compile a 1.8 m tall actor; the start must contain it.
            world.m_profile.m_start.m_clearance_m = 2.0;
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
            const std::filesystem::path roots[] = {m_base, m_shared};
            auto source = colony::loadContentSourceSet(roots, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(DescriptorTests, source);

            return colony::loadContentCatalog(*source, diagnostics);
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

    PPR_UNIT_TEST (selected_runtime_view_preserves_explicit_palette_and_actual_compile_parameters) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto definitions = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(definitions.has_value());
        auto owner = std::make_shared<const colony::ContentDescriptorCatalog>(std::move(*definitions));

        PPR_TEST_ASSERT(not colony::resolveContentWorld(*owner, "world", diagnostics));
        const auto world = colony::resolveContentWorld(*owner, "ppr.base:world", diagnostics);
        PPR_TEST_ASSERT(world.has_value());
        const sim::definition::GenerationCompileParameters first_parameters{1.0 / 3.0, 1.0, 0.4, 0.9};
        const sim::definition::GenerationCompileParameters second_parameters{0.5, 2.0, 0.3, 0.7};
        auto first = colony::selectWorldCatalog(owner, *world, first_parameters, diagnostics);
        auto second = colony::selectWorldCatalog(owner, *world, second_parameters, diagnostics);
        PPR_TEST_ASSERT(first.has_value() and second.has_value());

        const auto *material_one = first->findMaterial(1u);
        const auto *material_two = first->findMaterial(2u);
        PPR_TEST_ASSERT(material_one != nullptr and material_two != nullptr);
        PPR_TEST_ASSERT(material_one->m_key == "rock_copy" and material_two->m_key == "rock");
        PPR_TEST_ASSERT(first->worlds().size() == 1u and first->worlds().front().m_key == "ppr.base:world");
        PPR_TEST_ASSERT(first->definition().m_worlds.m_entries.front().m_key == "frontier");
        PPR_TEST_ASSERT(first->descriptorOwner() == owner and first->worldId() == *world);
        PPR_TEST_ASSERT(first->textureDefinitions().size() == 1u);
        PPR_TEST_ASSERT(first->contentFingerprint() != second->contentFingerprint());

        const auto one = first->materialAssetName(1u, diagnostics);
        const auto two = first->materialAssetName(2u, diagnostics);
        PPR_TEST_ASSERT(one and one->string() == "ppr.base:rock_copy");
        PPR_TEST_ASSERT(two and two->string() == "ppr.base:rock");
        PPR_TEST_ASSERT(not first->materialAssetName(0u, diagnostics));
        PPR_TEST_ASSERT(not first->materialAssetName(65535u, diagnostics));
        const colony::ContentCatalog unselected{ZeroValue{}};
        PPR_TEST_ASSERT(not unselected.materialAssetName(1u, diagnostics));

        const auto first_plan = colony::compileWorldPlan(*first, "ppr.base:world", 41u);
        const auto second_plan = colony::compileWorldPlan(*second, "ppr.base:world", 41u);
        PPR_TEST_ASSERT(first_plan.has_value() and second_plan.has_value());
        PPR_TEST_ASSERT(first_plan->recipe().m_cell_size_m == first_parameters.m_cell_size_m);
        PPR_TEST_ASSERT(second_plan->recipe().m_cell_size_m == second_parameters.m_cell_size_m);
        PPR_TEST_ASSERT(second_plan->recipe().m_reference_thickness_m == second_parameters.m_reference_thickness_m);
        PPR_TEST_ASSERT(second_plan->recipe().m_actor_half_width_m == second_parameters.m_actor_half_width_m);
        PPR_TEST_ASSERT(second_plan->recipe().m_actor_half_height_m == second_parameters.m_actor_half_height_m);
        PPR_TEST_ASSERT(not colony::compileWorldPlan(*first, "ppr.shared:world", 41u));

        struct CountingClock final : ITimerClock {
            mutable u32 m_reads{};

            TimePoint now() const noexcept override {
                ++m_reads;
                return {};
            }
        } clock;

        for (u32 parameter = 0u; parameter != 4u; ++parameter) {
            auto custom = colony::kWorldCompileParameters;
            switch (parameter) {
                case 0u: custom.m_cell_size_m = second_parameters.m_cell_size_m; break;
                case 1u: custom.m_reference_thickness_m = second_parameters.m_reference_thickness_m; break;
                case 2u: custom.m_actor_half_width_m = second_parameters.m_actor_half_width_m; break;
                case 3u: custom.m_actor_half_height_m = second_parameters.m_actor_half_height_m; break;
            }

            auto selected = colony::selectWorldCatalog(owner, *world, custom, diagnostics);
            PPR_TEST_ASSERT(selected.has_value());

            const auto rejected = colony::prepareWorld(std::move(*selected), "ppr.base:world", 41u, clock);
            PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::not_supported);
            PPR_TEST_ASSERT(clock.m_reads == 0u);
        }

        Fixture::write(fixture.m_shared / "cross_material.toml", owner->materials().front().m_descriptor.m_value);
        Fixture::write(
            fixture.m_shared / "pack.toml",
            owner->packs().packs().back().m_manifest.m_source.m_bytes +
                "\n[assets.cross_material]\nkind = 'sim.material'\ndescriptor = 'cross_material.toml'\n");
        auto cross_world = owner->worlds().front().m_descriptor.m_value;
        const auto binding = std::ranges::find_if(
            cross_world.m_palette.m_materials.m_entries, [](const auto &entry) { return entry.m_key == "rock"; });
        PPR_TEST_ASSERT(binding != cross_world.m_palette.m_materials.m_entries.end());
        binding->m_value.m_asset = "ppr.shared:cross_material";
        Fixture::write(fixture.m_base / "world.toml", cross_world);

        auto cross_definitions = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(cross_definitions.has_value());

        auto cross_owner = std::make_shared<const colony::ContentDescriptorCatalog>(std::move(*cross_definitions));
        auto selected = colony::selectWorldCatalog(cross_owner, *world, first_parameters, diagnostics);
        PPR_TEST_ASSERT(selected.has_value());

        const auto cross_name = selected->materialAssetName(2u, diagnostics);
        PPR_TEST_ASSERT(cross_name and cross_name->string() == "ppr.shared:cross_material");
        PPR_TEST_ASSERT(selected->findMaterial(2u)->m_key == "rock");
        PPR_TEST_ASSERT(selected->contentFingerprint() == first->contentFingerprint());
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

    // ------------------------------------------------------------------
    // descriptor domains and selected world assembly
    // ------------------------------------------------------------------

    constexpr sim::definition::GenerationCompileParameters kParameters{1.0 / 3.0, 1.0, 0.4, 0.9};

    PPR_UNIT_TEST (physical_descriptor_domains_are_shared_by_import_and_composition) {
        using Element = sim::asset::Element;
        const auto element_density = std::get<1u>(Element::describe()).m_constraint;
        const auto material_density = std::get<3u>(sim::definition::Material::describe()).m_constraint;
        PPR_TEST_ASSERT(element_density.m_min == Element::density_domain.m_min);
        PPR_TEST_ASSERT(element_density.m_max == Element::density_domain.m_max);
        PPR_TEST_ASSERT(material_density.m_min == element_density.m_min);
        PPR_TEST_ASSERT(material_density.m_max == element_density.m_max);
        PPR_TEST_ASSERT(material_density.m_exclusive_min == element_density.m_exclusive_min);

        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto original = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(original.has_value());

        const auto material = original->materials().front().m_descriptor.m_value;
        auto element = original->elements().definitions().front().m_descriptor.m_value;

        for (const double density:
            {Element::density_domain.m_min, std::midpoint(Element::density_domain.m_min, Element::density_domain.m_max),
                Element::density_domain.m_max}) {
            element.m_density = density;
            const auto expected_error = element_density.validate(density);
            const auto composed = sim::asset::composeMaterial(material, element, diagnostics, "programmatic");
            PPR_TEST_ASSERT(composed.has_value() == not static_cast<bool>(expected_error));

            Fixture::write(fixture.m_shared / "solid.toml", element);
            const auto imported = fixture.catalog(diagnostics);
            PPR_TEST_ASSERT(imported.has_value() == composed.has_value());
        }

        element.m_density = std::midpoint(Element::density_domain.m_min, Element::density_domain.m_max);
        element.m_phase = static_cast<sim::EGenerationMatter>(255u);
        PPR_TEST_ASSERT(Element::phase_domain.validate(element.m_phase) == config::EConfigErrc::unknown_enum_value);
        PPR_TEST_ASSERT(not sim::asset::composeMaterial(material, element, diagnostics, "programmatic"));
    };

    PPR_UNIT_TEST (selected_world_uses_explicit_material_ids_and_owned_texture_closure) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto catalog = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());

        auto assembled = colony::assembleWorld(*catalog, catalog->worlds().front().m_id, kParameters, diagnostics);
        PPR_TEST_ASSERT(assembled.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        PPR_TEST_ASSERT(assembled->m_compiled.m_bindings.m_materials.front().m_key == "rock_copy");
        PPR_TEST_ASSERT(assembled->m_compiled.m_recipe.m_materials.front().m_id == 1u);
        PPR_TEST_ASSERT(assembled->m_compiled.m_recipe.m_materials.back().m_id == 2u);
        PPR_TEST_ASSERT(assembled->m_compiled.m_recipe.m_materials.front().m_reference_density_kg_m3 == 2000.0);
        PPR_TEST_ASSERT(
            assembled->m_compiled.m_recipe.m_materials.front().m_key !=
            assembled->m_compiled.m_recipe.m_materials.back().m_key);
        PPR_TEST_ASSERT(
            assembled->m_compiled.m_recipe.m_biomes.front().m_bands.front().m_choices.front().m_material == 2u);
        PPR_TEST_ASSERT(assembled->m_compiled.m_bindings.m_rules.front().m_profile == "frontier");
        PPR_TEST_ASSERT(assembled->m_textures.size() == 1u);
        PPR_TEST_ASSERT(assembled->m_textures.front().m_descriptor.m_value.m_source == "images/missing.png");
        PPR_TEST_ASSERT(assembled->m_packs->packs().size() == 2u);
    };

    PPR_UNIT_TEST (numeric_identity_separates_appearance_density_and_source_provenance) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto original = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(original.has_value());
        const auto world = original->worlds().front().m_id;

        const auto baseline = colony::assembleWorld(*original, world, kParameters, diagnostics);
        PPR_TEST_ASSERT(baseline.has_value());

        auto material = original->materials().front().m_descriptor.m_value;
        material.m_appearance.m_color_rgba = "804020ff";
        Fixture::write(fixture.m_base / "rock.toml", material);
        const auto tinted = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(tinted.has_value());

        const auto tinted_world = colony::assembleWorld(*tinted, world, kParameters, diagnostics);
        PPR_TEST_ASSERT(tinted_world.has_value());
        PPR_TEST_ASSERT(tinted_world->m_numeric_fingerprint == baseline->m_numeric_fingerprint);
        PPR_TEST_ASSERT(
            tinted_world->m_presentation_descriptor_fingerprint != baseline->m_presentation_descriptor_fingerprint);

        auto element = original->elements().definitions().front().m_descriptor.m_value;
        element.m_density += 1.0;
        Fixture::write(fixture.m_shared / "solid.toml", element);
        const auto denser = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(denser.has_value());

        const auto denser_world = colony::assembleWorld(*denser, world, kParameters, diagnostics);
        PPR_TEST_ASSERT(denser_world.has_value());
        PPR_TEST_ASSERT(denser_world->m_numeric_fingerprint != tinted_world->m_numeric_fingerprint);
        PPR_TEST_ASSERT(
            denser_world->m_presentation_descriptor_fingerprint == tinted_world->m_presentation_descriptor_fingerprint);

        Fixture::write(
            fixture.m_shared / "solid.toml",
            denser->elements().definitions().front().m_descriptor.m_source.m_bytes + "\n# provenance only\n");
        const auto commented = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(commented.has_value());

        const auto commented_world = colony::assembleWorld(*commented, world, kParameters, diagnostics);
        PPR_TEST_ASSERT(commented_world.has_value());
        PPR_TEST_ASSERT(commented_world->m_numeric_fingerprint == denser_world->m_numeric_fingerprint);
        PPR_TEST_ASSERT(
            commented_world->m_presentation_descriptor_fingerprint ==
            denser_world->m_presentation_descriptor_fingerprint);

        const auto &shared_manifest = commented->packs().packs().back().m_manifest.m_source.m_bytes;
        Fixture::write(
            fixture.m_shared / "pack.toml",
            shared_manifest +
                "\n[assets.identical_solid]\nkind = \"sim.element\"\ndescriptor = \"identical_solid.toml\"\n");
        Fixture::write(fixture.m_shared / "identical_solid.toml", element);
        for (const auto &record: commented->materials()) {
            auto relinked = record.m_descriptor.m_value;
            relinked.m_element = "ppr.shared:identical_solid";
            Fixture::write(record.m_descriptor.m_source.m_path, relinked);
        }

        const auto relinked = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(relinked.has_value());

        const auto relinked_world = colony::assembleWorld(*relinked, world, kParameters, diagnostics);
        PPR_TEST_ASSERT(relinked_world.has_value());
        PPR_TEST_ASSERT(relinked_world->m_numeric_fingerprint == commented_world->m_numeric_fingerprint);
        PPR_TEST_ASSERT(
            relinked_world->m_presentation_descriptor_fingerprint ==
            commented_world->m_presentation_descriptor_fingerprint);
    };

    PPR_UNIT_TEST (unsampled_declared_biomes_remain_in_numeric_palette) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto original = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(original.has_value());

        auto biome = original->biomes().front().m_descriptor.m_value;
        biome.m_definition.m_erosion.m_passes = 5u;
        Fixture::write(fixture.m_shared / "unsampled.toml", biome);
        const auto &manifest = original->packs().packs().back().m_manifest.m_source.m_bytes;
        Fixture::write(
            fixture.m_shared / "pack.toml",
            manifest + "\n[assets.unsampled]\nkind = \"sim.biome\"\ndescriptor = \"unsampled.toml\"\n");

        auto world = original->worlds().front().m_descriptor.m_value;
        world.m_palette.m_biomes.m_entries.push_back({"unsampled", {"ppr.shared:unsampled", 1u}});
        Fixture::write(fixture.m_base / "world.toml", world);
        const auto expanded = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(expanded.has_value());

        const auto assembled = colony::assembleWorld(
            *expanded, expanded->worlds().front().m_id, kParameters, diagnostics);
        PPR_TEST_ASSERT(assembled.has_value());
        PPR_TEST_ASSERT(assembled->m_compiled.m_recipe.m_constraints.size() == 1u);
        PPR_TEST_ASSERT(assembled->m_compiled.m_recipe.m_biomes.size() == 2u);
        PPR_TEST_ASSERT(assembled->m_compiled.m_recipe.m_biomes[1u].m_erosion_passes == 5u);
    };

    PPR_UNIT_TEST (unrelated_mounted_pack_cannot_change_selected_world_bindings_or_identity) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto original = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(original.has_value());
        const auto world = original->worlds().front().m_id;

        const auto baseline = colony::assembleWorld(*original, world, kParameters, diagnostics);
        PPR_TEST_ASSERT(baseline.has_value());

        const auto unrelated = fixture.m_root / "unrelated";
        PPR_TEST_ASSERT(std::filesystem::create_directory(unrelated));
        Fixture::write(
            unrelated / "pack.toml",
            "schema_version = 1\npack_id = \"aaa.unrelated\"\n[assets.noise]\nkind = \"sim.field\"\ndescriptor = \"noise.toml\"\n");
        Fixture::write(unrelated / "noise.toml", original->fields().front().m_descriptor.m_value);
        auto extra = asset::readPackSource(unrelated, diagnostics);
        PPR_TEST_ASSERT(extra.has_value());

        Array<asset::PackSource> sources;
        for (const auto &pack: original->packs().packs()) {
            sources.push_back({pack.m_root, pack.m_manifest.m_source});
        }

        sources.push_back(std::move(*extra));
        auto indexed = asset::PackSet::index(std::move(sources), colony::kContentAssetKinds, diagnostics);
        PPR_TEST_ASSERT(indexed.has_value());

        const auto expanded = colony::ContentDescriptorCatalog::importDefinitions(
            std::make_shared<const asset::PackSet>(std::move(*indexed)), diagnostics);
        PPR_TEST_ASSERT(expanded.has_value());

        const auto assembled = colony::assembleWorld(*expanded, world, kParameters, diagnostics);
        PPR_TEST_ASSERT(assembled.has_value());
        PPR_TEST_ASSERT(assembled->m_numeric_fingerprint == baseline->m_numeric_fingerprint);
        PPR_TEST_ASSERT(
            assembled->m_presentation_descriptor_fingerprint == baseline->m_presentation_descriptor_fingerprint);
        PPR_TEST_ASSERT(assembled->m_compiled.m_bindings.m_materials.front().m_id == 1u);
        PPR_TEST_ASSERT(assembled->m_compiled.m_bindings.m_fields.size() == 1u);
    };

    PPR_UNIT_TEST (duplicate_asset_targets_reject_for_each_world_palette_kind) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto original = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(original.has_value());
        const auto baseline = original->worlds().front().m_descriptor.m_value;
        const auto verify = [&](colony::WorldAssetDescriptor world) {
            Fixture::write(fixture.m_base / "world.toml", world);
            const auto catalog = fixture.catalog(diagnostics);
            PPR_TEST_ASSERT(catalog.has_value());

            const auto rejected = colony::assembleWorld(
                *catalog, catalog->worlds().front().m_id, kParameters, diagnostics);
            PPR_TEST_ASSERT(not rejected and rejected.error() == config::EConfigErrc::duplicate_key);
        };

        auto world = baseline;
        world.m_palette.m_materials.m_entries.back().m_value.m_asset = "rock";
        verify(world);

        world = baseline;
        world.m_palette.m_fields.m_entries.push_back({"other", {"ppr.shared:noise", 1u}});
        verify(world);

        world = baseline;
        world.m_palette.m_biomes.m_entries.push_back({"other", {"ppr.shared:hearth", 1u}});
        verify(world);

        world = baseline;
        world.m_palette.m_features.m_entries.push_back({"other", {"ppr.shared:landing", 1u}});
        verify(world);

        world = baseline;
        world.m_palette.m_rules.m_entries.push_back({"other", {"ppr.shared:cave"}});
        verify(world);
    };

    PPR_UNIT_TEST (world_alias_errors_keep_descriptor_origin_and_selected_world_context) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto original = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(original.has_value());
        auto biome = original->biomes().front().m_descriptor.m_value;
        biome.m_definition.m_temperature.m_field.m_key = "missing";
        Fixture::write(fixture.m_shared / "hearth.toml", biome);
        const auto catalog = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());

        const auto rejected = colony::assembleWorld(*catalog, catalog->worlds().front().m_id, kParameters, diagnostics);
        PPR_TEST_ASSERT(not rejected);
        PPR_TEST_ASSERT(std::ranges::any_of(diagnostics.entries(), [](const auto &entry) {
            return entry.m_property == "definition.temperature.field" and
                   entry.m_location.find("hearth.toml") != std::string::npos and
                   entry.m_message.find("World ppr.base:world") != std::string::npos;
        }));
    };

    PPR_UNIT_TEST (assembled_appearance_requires_valid_rgba_period_and_background_order) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto original = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(original.has_value());
        auto material = original->materials().front().m_descriptor.m_value;
        material.m_appearance.m_color_rgba = "nothex!!";
        Fixture::write(fixture.m_base / "rock.toml", material);
        const auto bad_color = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(bad_color.has_value());
        PPR_TEST_ASSERT(
            not colony::assembleWorld(*bad_color, bad_color->worlds().front().m_id, kParameters, diagnostics));

        material = original->materials().front().m_descriptor.m_value;
        material.m_appearance.m_texture_period_m.reset();
        Fixture::write(fixture.m_base / "rock.toml", material);
        const auto no_period = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(no_period.has_value());
        PPR_TEST_ASSERT(
            not colony::assembleWorld(*no_period, no_period->worlds().front().m_id, kParameters, diagnostics));

        Fixture::write(fixture.m_base / "rock.toml", original->materials().front().m_descriptor.m_value);
        auto background = original->backgrounds().front().m_descriptor.m_value;
        background.m_definition.m_layers.m_entries.back().m_value.m_order = 0u;
        Fixture::write(fixture.m_base / "skies.toml", background);
        const auto bad_order = fixture.catalog(diagnostics);
        PPR_TEST_ASSERT(bad_order.has_value());
        PPR_TEST_ASSERT(
            not colony::assembleWorld(*bad_order, bad_order->worlds().front().m_id, kParameters, diagnostics));
    };

    PPR_UNIT_TEST (shipped_definition_explicit_maps_preserve_current_numeric_recipe) {
        config::Diagnostics diagnostics{32u};
        const auto source = colony::loadContentCatalog(
            hal::process::currentExecutablePath().parent_path() / "worldgen/default", diagnostics);
        PPR_TEST_ASSERT(source.has_value());

        const auto world = colony::resolveContentWorld(*source, "frontier", diagnostics);
        PPR_TEST_ASSERT(world.has_value());

        const auto assembled = colony::assembleWorld(*source, *world, kParameters, diagnostics);
        PPR_TEST_ASSERT(assembled.has_value());

        const auto *const definition = &assembled->m_definition;
        PPR_TEST_ASSERT(definition->m_materials.m_entries.size() == 63u);
        PPR_TEST_ASSERT(definition->m_fields.m_entries.size() == 44u);
        PPR_TEST_ASSERT(definition->m_biomes.m_entries.size() == 11u);
        PPR_TEST_ASSERT(definition->m_features.m_entries.size() == 26u);

        const auto baseline = sim::definition::compileRecipe(*definition, {"frontier"}, kParameters, diagnostics);
        PPR_TEST_ASSERT(baseline.has_value());

        const auto same_binding = [](const auto &left, const auto &right) {
            if constexpr (requires { left.m_id; }) {
                return left.m_key == right.m_key and left.m_id == right.m_id;
            } else {
                return left.m_key == right.m_key and left.m_index == right.m_index;
            }
        };
        const auto &declared = assembled->m_compiled.m_bindings;
        PPR_TEST_ASSERT(std::ranges::equal(baseline->m_bindings.m_materials, declared.m_materials, same_binding));
        PPR_TEST_ASSERT(std::ranges::equal(baseline->m_bindings.m_fields, declared.m_fields, same_binding));
        PPR_TEST_ASSERT(std::ranges::equal(baseline->m_bindings.m_biomes, declared.m_biomes, same_binding));
        PPR_TEST_ASSERT(std::ranges::equal(baseline->m_bindings.m_features, declared.m_features, same_binding));

        auto bindings = baseline->m_bindings;
        bindings.m_rules.clear();
        for (auto &feature: bindings.m_features) {
            feature.m_markers.clear();
        }

        std::ranges::reverse(bindings.m_materials);
        std::ranges::reverse(bindings.m_fields);
        std::ranges::reverse(bindings.m_biomes);
        std::ranges::reverse(bindings.m_features);
        const auto explicit_result = sim::definition::compileRecipe(
            *definition, {"frontier"}, kParameters, bindings, diagnostics);
        PPR_TEST_ASSERT(explicit_result.has_value());

        const auto &first = baseline->m_recipe;
        const auto &second = explicit_result->m_recipe;
        PPR_TEST_ASSERT(first.m_materials.size() == second.m_materials.size());
        PPR_TEST_ASSERT(first.m_biomes.size() == second.m_biomes.size());
        PPR_TEST_ASSERT(first.m_fields.size() == second.m_fields.size());
        PPR_TEST_ASSERT(first.m_features.size() == second.m_features.size());
        PPR_TEST_ASSERT(first.m_rules.size() == second.m_rules.size());
        PPR_TEST_ASSERT(first.m_boundary_material == second.m_boundary_material);
        PPR_TEST_ASSERT(first.m_start_biome == second.m_start_biome);
        PPR_TEST_ASSERT(first.m_start_feature == second.m_start_feature);
        PPR_TEST_ASSERT(first.m_start_marker == second.m_start_marker);

        for (std::size_t index = 0u; index < first.m_materials.size(); ++index) {
            const auto &left = first.m_materials[index];
            const auto &right = second.m_materials[index];
            PPR_TEST_ASSERT(
                left.m_id == right.m_id and
                left.m_key == right.m_key and
                left.m_terrain == right.m_terrain and
                left.m_diggable == right.m_diggable and
                left.m_matter == right.m_matter and
                left.m_reference_density_kg_m3 == right.m_reference_density_kg_m3);
        }

        for (std::size_t index = 0u; index < first.m_fields.size(); ++index) {
            PPR_TEST_ASSERT(first.m_fields[index].m_key == second.m_fields[index].m_key);
        }

        for (std::size_t index = 0u; index < first.m_biomes.size(); ++index) {
            PPR_TEST_ASSERT(first.m_biomes[index].m_key == second.m_biomes[index].m_key);
            PPR_TEST_ASSERT(first.m_biomes[index].m_temperature_field == second.m_biomes[index].m_temperature_field);
            PPR_TEST_ASSERT(first.m_biomes[index].m_bands.size() == second.m_biomes[index].m_bands.size());
            PPR_TEST_ASSERT(first.m_biomes[index].m_erosion_passes == second.m_biomes[index].m_erosion_passes);
        }

        for (std::size_t index = 0u; index < first.m_rules.size(); ++index) {
            PPR_TEST_ASSERT(first.m_rules[index].m_key == second.m_rules[index].m_key);
            PPR_TEST_ASSERT(first.m_rules[index].m_order == second.m_rules[index].m_order);
            PPR_TEST_ASSERT(first.m_rules[index].m_phase == second.m_rules[index].m_phase);
        }

        PPR_TEST_ASSERT(bindings.m_materials.size() > 1u and bindings.m_fields.size() > 1u);
        std::swap(bindings.m_materials.front().m_id, bindings.m_materials.back().m_id);
        std::swap(bindings.m_fields.front().m_index, bindings.m_fields.back().m_index);
        const auto permuted = sim::definition::compileRecipe(
            *definition, {"frontier"}, kParameters, bindings, diagnostics);
        PPR_TEST_ASSERT(permuted.has_value());

        for (const auto &binding: permuted->m_bindings.m_materials) {
            const auto previous = std::ranges::find(
                baseline->m_bindings.m_materials, binding.m_key, &sim::definition::MaterialBinding::m_key);
            PPR_TEST_ASSERT(previous != baseline->m_bindings.m_materials.end());

            const auto &before = first.m_materials[previous->m_id - 1u];
            const auto &after = permuted->m_recipe.m_materials[binding.m_id - 1u];
            PPR_TEST_ASSERT(
                before.m_key == after.m_key and before.m_reference_density_kg_m3 == after.m_reference_density_kg_m3);
        }

        for (const auto &binding: permuted->m_bindings.m_fields) {
            const auto previous = std::ranges::find(
                baseline->m_bindings.m_fields, binding.m_key, &sim::definition::FieldBinding::m_key);
            PPR_TEST_ASSERT(previous != baseline->m_bindings.m_fields.end());

            PPR_TEST_ASSERT(
                first.m_fields[previous->m_index].m_key == permuted->m_recipe.m_fields[binding.m_index].m_key);
        }
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

    const UnitTest world_assembly = UnitTest::Named("world_assembly") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::descriptors::physical_descriptor_domains_are_shared_by_import_and_composition,
            detail::descriptors::selected_world_uses_explicit_material_ids_and_owned_texture_closure,
            detail::descriptors::selected_runtime_view_preserves_explicit_palette_and_actual_compile_parameters,
            detail::descriptors::numeric_identity_separates_appearance_density_and_source_provenance,
            detail::descriptors::unsampled_declared_biomes_remain_in_numeric_palette,
            detail::descriptors::unrelated_mounted_pack_cannot_change_selected_world_bindings_or_identity,
            detail::descriptors::duplicate_asset_targets_reject_for_each_world_palette_kind,
            detail::descriptors::world_alias_errors_keep_descriptor_origin_and_selected_world_context,
            detail::descriptors::assembled_appearance_requires_valid_rgba_period_and_background_order,
            detail::descriptors::shipped_definition_explicit_maps_preserve_current_numeric_recipe,
        });
    };

    const UnitTest &gameWorldAssemblyTests() noexcept {
        return world_assembly;
    }
}
