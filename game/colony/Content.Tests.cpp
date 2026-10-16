module;

#include <cstdlib>
#include "pP/UnitTest.h"
#include "../Game.TestAllocationFailure.h"

module game.tests;

import engine.core;
import engine.asset;
import engine.sim;
import game.colony.content;
import std;

namespace pP::game::tests::fixtures {
    // ------------------------------------------------------------------
    // programmatic fixture declarations
    // ------------------------------------------------------------------

    Expected<config::Document> definitionDocument(
        std::span<const asset::DescriptorSource> fragments, config::Diagnostics &diagnostics);
    Expected<colony::PackDefinition> resolveDefinition(
        const config::Document &document, config::Diagnostics &diagnostics);
    Expected<colony::PackDefinition> resolveDefinition(
        std::span<const asset::DescriptorSource> fragments, config::Diagnostics &diagnostics);
}

namespace pP::game::tests::detail::content {
    // ------------------------------------------------------------------
    // programmatic schema fixture
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(ContentSchemaTests, debug, none)

    // Programmatic schema fragments, independent of the named pack file format.
    struct ProgrammaticInputs {
        std::array<asset::DescriptorSource, 7u> m_documents;
    };

    Expected<colony::PackDefinition> resolveFixtureDefinition(
        const ProgrammaticInputs &input, config::Diagnostics &diagnostics) {
        return fixtures::resolveDefinition(input.m_documents, diagnostics);
    }

    // ------------------------------------------------------------------
    // content
    // ------------------------------------------------------------------

    using namespace colony;

    const std::string rock = R"([materials.sandstone]
phase = "solid"
representation = "terrain"
diggable = true
density = 2200.0
)";
    const std::string snow = R"([materials.snow]
phase = "solid"
representation = "particle"
density = 300.0
)";
    const std::string rock_appearance = R"([appearance.materials.sandstone]
display_name = "Sandstone"
color_rgba = "B59C72FF"
albedo_texture = "sandstone_texture"
texture_period_m = 2.0
)";
    const std::string snow_appearance = R"([appearance.materials.snow]
display_name = "Snow"
color_rgba = "FFFFFFFF"
)";
    const std::string manifest = R"(schema_version = 1
pack_id = "default"
[assets.rock]
kind = "sim.element"
descriptor = "rock.toml"
)";
    const std::string element = "schema_version = 1\nphase = 'solid'\ndensity = 2200.0\n";

    [[nodiscard]] static std::string replace(
        std::string source, const std::string_view from, const std::string_view to) {
        const auto position = source.find(from);
        PPR_TEST_ASSERT(position != std::string::npos);
        source.replace(position, from.size(), to);
        return source;
    }

    ProgrammaticInputs fixtureSource() {
        ProgrammaticInputs source{};
        const char *names[] = {
            "materials.toml", "biomes.toml", "fields.toml", "features.toml", "worlds.toml", "rules.toml",
            "appearance.toml"};
        for (std::size_t i = 0u; i < 7u; ++i) {
            source.m_documents[i] = {names[i], ""};
        }
        source.m_documents[0u].m_bytes += rock + snow;
        source.m_documents[1u].m_bytes += R"([biomes.plain]
temperature.field = "climate"
temperature.base_k = 293.0
[biomes.plain.composition.base]
order = 0
field = "climate"
depth.min = 0.0
[biomes.plain.composition.base.materials.stone]
material = "sandstone"
weight = 1.0
)";
        source.m_documents[2u].m_bytes += "[fields.climate]\nalgorithm = 'perlin'\nfrequency_per_metre = 0.025\n";
        source.m_documents[6u].m_bytes += rock_appearance + snow_appearance + R"([appearance.biomes.plain]
display_name = "Plain"
preview_color_rgba = "709040FF"
)";
        return source;
    }

    Expected<ContentCatalog> decodeCatalog(const ProgrammaticInputs &source, config::Diagnostics &diagnostics) {
        auto document = fixtures::definitionDocument(source.m_documents, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSchemaTests, document);
        auto definition = fixtures::resolveDefinition(*document, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSchemaTests, definition);

        return projectContentCatalog(std::move(*definition), *document, diagnostics);
    }

    Expected<Array<FieldDefinition>> decodeFields(
        const std::string_view text, const std::string_view origin, config::Diagnostics &diagnostics) {
        auto source = fixtureSource();
        source.m_documents[2u] = {
            std::string{origin},
            std::string{text} + "\n[fields.climate]\nalgorithm = 'perlin'\nfrequency_per_metre = 0.025\n"};
        return decodeCatalog(source, diagnostics).transform([](ContentCatalog catalog) {
            Array<FieldDefinition> result;
            for (const auto &field: catalog.fields()) {
                if (field.m_key != "climate") {
                    result.push_back(field);
                }
            }
            return result;
        });
    }

    // Test-local fragment; no world/layout/start defaults are supplied by the decoder.
    const std::string definition_text = R"(
definition_schema_version = 2
[materials.rock]
phase = "solid"
representation = "terrain"
diggable = true
density = 2500.0
[fields.climate]
algorithm = "perlin"
frequency_per_metre = 0.025
[biomes.plain.temperature]
field = "climate"
base_k = 293.0
variation_k = 0.0
[biomes.plain.composition.base]
order = 0
field = "climate"
depth.min = 0.0
depth.max = 1.0
[biomes.plain.composition.base.materials.stone]
material = "rock"
weight = 1.0
)";

    PPR_UNIT_TEST (definition_envelope_requires_version_two_and_locates_logical_errors) {
        config::Diagnostics diagnostics{};
        const auto valid = decodeWorldGenDefinition(definition_text, "definition.toml", diagnostics);
        PPR_TEST_ASSERT(valid.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(valid->m_materials.m_entries.front().m_key == "rock");
        for (const auto version:
            {"", "definition_schema_version = 0", "definition_schema_version = 1", "definition_schema_version = '1'"}) {
            config::Diagnostics errors{};
            const auto invalid = decodeWorldGenDefinition(
                replace(definition_text, "definition_schema_version = 2", version), "version.toml", errors);
            PPR_TEST_ASSERT(not invalid.has_value() and errors.hasErrors());
            PPR_TEST_ASSERT(errors.entries().front().m_property == "definition_schema_version");
            PPR_TEST_ASSERT(errors.entries().front().m_location.starts_with("version.toml"));
            if (std::string_view{version}.empty()) {
                PPR_TEST_ASSERT(invalid.error() == config::make_error_code(config::EConfigErrc::missing_value));
            }
        }

        for (const auto &source:
            {replace(definition_text, "material = \"rock\"", "material = \"missing\""),
                replace(definition_text, "depth.max = 1.0", "depth.max = 0.0"),
                replace(definition_text, "variation_k = 0.0", "variation_k = -1.0")}) {
            config::Diagnostics errors{};
            PPR_TEST_ASSERT(not decodeWorldGenDefinition(source, "logical.toml", errors));
            PPR_TEST_ASSERT(errors.hasErrors());
            const auto document = config::decode(source, "logical.toml", diagnostics, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            for (const auto &error: errors.entries()) {
                const auto entry = std::ranges::find(document->m_entries, error.m_property, &config::Entry::m_key);
                if (entry != document->m_entries.end()) {
                    PPR_TEST_ASSERT(error.m_location == entry->m_location);
                } else {
                    PPR_TEST_ASSERT(error.m_property == "biomes.plain.composition");
                    const auto group = std::ranges::find(
                        document->m_groups, error.m_property, &config::GroupPresence::m_path);
                    PPR_TEST_ASSERT(group != document->m_groups.end() and error.m_location == group->m_location);
                }
            }
        }
    };

    PPR_UNIT_TEST (pack_fields_delegate_to_canonical_noise_schema) {
        const std::string base = "algorithm = 'perlin'\nfrequency_per_metre = 0.001\n";
        const std::pair<std::string, bool> cases[] = {
            {base, true},
            {base + "octaves = 1\n", true},
            {base + "octaves = 16\n", true},
            {base + "octaves = 0\n", false},
            {base + "octaves = 17\n", false},
            {base + "octaves = 16\nlacunarity = 4.0\n", false},
            {replace(base, "0.001", "1000000.0"), false},
            {replace(base, "0.001", "0.0"), false},
            {replace(base, "0.001", "1.0e-40"), false},
            {replace(base, "algorithm = 'perlin'\n", ""), false},
            {replace(base, "frequency_per_metre = 0.001\n", ""), false},
            {base + "warp_amplitude_m = 4096.0\n", true},
            {base + "warp_amplitude_m = 4096.01\n", false},
            {base + "warp_amplitude_m = -0.01\n", false},
            {base + "warp_amplitude_m = 1.0\nwarp_frequency_per_metre = 1000000.0\n", false},
            {base + "warp_frequency_per_metre = 0.0\n", false},
            {base + "fractal = 'billow'\naxis_scale_x = 8.0\nrotation_degrees = 180.0\n", true},
            {base + "axis_scale_y = 8.01\n", false},
        };
        for (const auto &[source, accepted]: cases) {
            config::Diagnostics canonical_errors{};
            const auto document = config::decode(source, "noise.toml", canonical_errors, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            const auto layer = config::toTreeLayer(*document, "noise.toml");
            const auto canonical = config::resolveRecursive<sim::NoiseFieldDesc>(
                std::span{&layer, 1u}, canonical_errors);
            const bool canonical_valid = canonical.has_value() and
                                         not sim::validateNoiseFieldDescription(canonical->m_value);

            config::Diagnostics pack_errors{};
            const auto pack = decodeFields("[fields.probe]\n" + source, "fields.toml", pack_errors);
            PPR_TEST_ASSERT(canonical_valid == accepted and pack.has_value() == canonical_valid);
            if (pack) {
                PPR_TEST_ASSERT(pack->size() == 1u and not pack_errors.hasErrors());
                const auto values = [](const sim::NoiseFieldDesc &noise) {
                    return std::tuple{
                        noise.m_algorithm, noise.m_fractal, noise.m_frequency_per_metre, noise.m_octaves,
                        noise.m_lacunarity, noise.m_gain, noise.m_warp_amplitude_m, noise.m_warp_frequency_per_metre,
                        noise.m_axis_scale_x, noise.m_axis_scale_y, noise.m_rotation_degrees};
                };
                PPR_TEST_ASSERT(values(pack->front().m_noise) == values(canonical->m_value));
            } else {
                PPR_TEST_ASSERT(pack_errors.hasErrors());
            }
        }
    };

    PPR_UNIT_TEST (pack_field_presence_and_domain_errors_match_canonical_schema) {
        using config::EConfigErrc;
        const std::tuple<std::string_view, std::string_view, EConfigErrc> cases[] = {
            {"frequency_per_metre = 0.001\n", "algorithm", EConfigErrc::missing_value},
            {"algorithm = 'perlin'\n", "frequency_per_metre", EConfigErrc::missing_value},
            {"algorithm = 'unsupported'\nfrequency_per_metre = 0.001\n", "algorithm", EConfigErrc::unknown_enum_value},
            {"algorithm = 'perlin'\nfrequency_per_metre = 0.0\n", "frequency_per_metre", EConfigErrc::out_of_range}};
        for (const auto &[source, property, code]: cases) {
            config::Diagnostics canonical_errors{};
            const auto document = config::decode(source, "noise.toml", canonical_errors, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            const auto layer = config::toTreeLayer(*document, "noise.toml");
            const auto canonical = config::resolveRecursive<sim::NoiseFieldDesc>(
                std::span{&layer, 1u}, canonical_errors);
            PPR_TEST_ASSERT(not canonical.has_value() and canonical_errors.hasErrors());
            PPR_TEST_ASSERT(canonical.error() == config::make_error_code(code));
            PPR_TEST_ASSERT(canonical_errors.entries().front().m_property == property);

            const std::string pack_source = "[fields.probe]\n" + std::string{source};
            config::Diagnostics pack_errors{};
            const auto pack = decodeFields(pack_source, "fields.toml", pack_errors);
            PPR_TEST_ASSERT(not pack.has_value() and pack_errors.hasErrors());
            PPR_TEST_ASSERT(pack.error() == canonical.error());
            const auto &error = pack_errors.entries().front();
            PPR_TEST_ASSERT(error.m_property == "fields.probe." + std::string{property});
            PPR_TEST_ASSERT(error.m_code == code and error.m_location.starts_with("fields.toml"));
            if (code != EConfigErrc::missing_value) {
                config::Diagnostics source_errors{};
                const auto located = config::decode(
                    pack_source, "fields.toml", source_errors, config::EDecodeMode::tree);
                PPR_TEST_ASSERT(located.has_value());
                const auto entry = std::ranges::find(located->m_entries, error.m_property, &config::Entry::m_key);
                PPR_TEST_ASSERT(entry != located->m_entries.end() and error.m_location == entry->m_location);
            }
        }
    };

    PPR_UNIT_TEST (decoded_fragment_requires_explicit_world_completion_to_compile) {
        config::Diagnostics diagnostics{};
        const auto definition = decodeWorldGenDefinition(definition_text, "fragment.toml", diagnostics);
        PPR_TEST_ASSERT(definition.has_value());
        const auto fragment = sim::definition::compileDefinition(*definition, 1.0 / 3.0, diagnostics);
        PPR_TEST_ASSERT(fragment.has_value() and not diagnostics.hasErrors());

        sim::GenerationRecipe recipe{};
        recipe.m_materials = fragment->m_materials;
        recipe.m_fields = fragment->m_fields;
        recipe.m_biomes = {fragment->m_biomes.front()};
        PPR_TEST_ASSERT(not sim::compileGenerationPlan(recipe, 42u));

        // Explicit minimal test world, layout and start; not production defaults.
        recipe.m_cell_size_m = 1.0 / 3.0;
        recipe.m_reference_thickness_m = 1.0;
        recipe.m_boundary_material = fragment->m_bindings.m_materials.front().m_id;
        recipe.m_boundary_fine = 3u;
        recipe.m_min_sites = 1u;
        recipe.m_max_sites = 1u;
        recipe.m_site_attempts = 1u;
        recipe.m_site_spacing_m = 100.0;
        recipe.m_constraints.push_back({401u, 0u, 1u, 1.0, 0.0, 1.0, 0.0, 2048.0});
        sim::GenerationFeature start{.m_key = 301u, .m_bounds = {0, 0, 30, 30}};
        start.m_operations.push_back({.m_key = 302u, .m_carve = true, .m_width = 30u, .m_height = 30u});
        start.m_markers.push_back({303u, sim::EGenerationMarker::start, 15, 10});
        recipe.m_features.push_back(std::move(start));
        recipe.m_start_x_fine = 2000u;
        recipe.m_start_y_fine = 3000u;
        recipe.m_start_clearance_m = 3.0;
        recipe.m_start_route_width_m = 1.0;
        recipe.m_actor_half_width_m = 0.4;
        recipe.m_actor_half_height_m = 0.9;

        const auto plan = sim::compileGenerationPlan(std::move(recipe), 42u);
        PPR_TEST_ASSERT(plan.has_value());
        PPR_TEST_ASSERT(plan->recipe().m_materials.front().m_id == fragment->m_bindings.m_materials.front().m_id);
        PPR_TEST_ASSERT(plan->recipe().m_biomes.front().m_key == fragment->m_biomes.front().m_key);
        PPR_TEST_ASSERT(plan->fields().size() == fragment->m_fields.size());
    };

    PPR_UNIT_TEST (named_manifest_schema_and_background_validation) {
        config::Diagnostics diagnostics;
        const auto decoded = asset::importDescriptor<asset::Manifest>({"pack.toml", manifest}, diagnostics);
        PPR_TEST_ASSERT(decoded and decoded->m_value.m_pack_id == "default");
        PPR_TEST_ASSERT(decoded->m_value.m_assets.m_entries.front().m_key == "rock");

        for (const auto &invalid:
            {replace(manifest, "schema_version = 1", "schema_version = 3"),
                replace(manifest, "schema_version = 1", "schema_version = '1'"),
                replace(manifest, "schema_version = 1", ""), replace(manifest, "descriptor = \"rock.toml\"", ""),
                manifest + "\n[files]\nmaterials = 'materials.toml'\n"}) {
            config::Diagnostics errors;
            PPR_TEST_ASSERT(not asset::importDescriptor<asset::Manifest>({"pack.toml", invalid}, errors));
            PPR_TEST_ASSERT(errors.hasErrors());
        }

        auto source = fixtureSource();
        for (u32 index = 0u; index < 4u; ++index) {
            source.m_documents[6u].m_bytes +=
                "[appearance.backgrounds.frontier.layers.layer_" +
                std::to_string(index) +
                "]\norder = " +
                std::to_string(index) +
                "\ntexture = 'background_texture'\ncolor_rgba = 'FFFFFFFF'\nrepeat_period_m = 256.0\nparallax_factor = 0.22\n";
        }
        const auto valid = decodeCatalog(source, diagnostics);
        PPR_TEST_ASSERT(valid and valid->backgrounds().front().m_layers.size() == 4u);
        for (const auto &[before, after]:
            {std::pair{"order = 1", "order = 0"}, std::pair{"256.0", "0.0"}, std::pair{"0.22", "1.01"},
                std::pair{"background_texture", "../layer.png"},
                std::pair{"color_rgba = 'FFFFFFFF'", "color_rgba = 'FFFFFF80'"}}) {
            auto invalid = source;
            invalid.m_documents[6u].m_bytes = replace(invalid.m_documents[6u].m_bytes, before, after);
            config::Diagnostics errors;
            PPR_TEST_ASSERT(not decodeCatalog(invalid, errors) and errors.hasErrors());
        }
    };

    PPR_UNIT_TEST (owned_materials_use_derived_ids_and_preserve_appearance) {
        auto source = fixtureSource();
        source.m_documents[6u].m_bytes =
            "" +
            rock_appearance +
            "metallic = 0.7\nroughness = 0.2\nemissive_r = 0.4\nemissive_strength = 3.0\nalpha_mode = 'blend'\nior = 1.33\nnormal_texture = 'normal_texture'\nnormal_scale = 2.0\nrefraction_depth_m = 0.3\n" +
            snow_appearance +
            "albedo_texture = 'snow_texture'\nnormal_texture = 'normal_texture'\ntexture_period_m = 1.0\n" +
            "[appearance.biomes.plain]\ndisplay_name = 'Plain'\npreview_color_rgba = '709040FF'\n";
        config::Diagnostics diagnostics;
        auto catalog = decodeCatalog(source, diagnostics);
        PPR_TEST_ASSERT(catalog and not diagnostics.hasErrors());
        const auto *const optical = catalog->findMaterial("sandstone");
        PPR_TEST_ASSERT(optical != nullptr and catalog->findMaterial(optical->m_numeric_id) == optical);
        PPR_TEST_ASSERT(optical->m_metallic == 0.7 and optical->m_roughness == 0.2);
        PPR_TEST_ASSERT(optical->m_emissive_rgb[0u] == 0.4 and optical->m_emissive_strength == 3.0);
        PPR_TEST_ASSERT(optical->m_alpha_mode == EMaterialAlphaMode::blend and optical->m_ior == 1.33);
        PPR_TEST_ASSERT(optical->m_normal_scale == 2.0 and optical->m_refraction_depth_m == 0.3);
        PPR_TEST_ASSERT(optical->m_color_rgba == 0xB59C72FFu and optical->m_texture_period_m == 2.0);
        PPR_TEST_ASSERT(optical->m_diggable == true and optical->m_roundness == 1.0);
        PPR_TEST_ASSERT(optical->m_texture_sampling == EMaterialTextureSampling::stochastic);
        PPR_TEST_ASSERT(catalog->findMaterial("snow")->m_albedo_texture == "snow_texture");
        PPR_TEST_ASSERT(catalog->findMaterial("snow")->m_representation == EMaterialRepresentation::particle);
        const auto compiled = sim::definition::compileDefinition(catalog->definition(), 1.0 / 3.0, diagnostics);
        PPR_TEST_ASSERT(compiled.has_value());
        for (const auto &binding: compiled->m_bindings.m_materials) {
            PPR_TEST_ASSERT(catalog->findMaterial(binding.m_key)->m_numeric_id == binding.m_id);
        }
        source = {};
        PPR_TEST_ASSERT(catalog->findMaterial("sandstone")->m_display_name == "Sandstone");
        PPR_TEST_ASSERT(catalog->findMaterial(u16{0}) == nullptr and catalog->findMaterial(u16{65535}) == nullptr);
        PPR_TEST_ASSERT(catalog->findMaterial("unknown") == nullptr);
    };

    PPR_UNIT_TEST (canonical_material_identity) {
        auto source = fixtureSource();
        config::Diagnostics diagnostics;
        const auto first = decodeCatalog(source, diagnostics);
        PPR_TEST_ASSERT(first.has_value());
        source.m_documents[0u].m_bytes = "# reordered\n" + snow + rock;
        const auto second = decodeCatalog(source, diagnostics);
        PPR_TEST_ASSERT(second and first->materialFingerprint() == second->materialFingerprint());
        for (const auto &suffix: {"texture_sampling = 'repeat'\n", "roundness = 0.25\n", "roughness = 0.2\n"}) {
            auto changed = fixtureSource();
            changed.m_documents[6u].m_bytes = replace(
                changed.m_documents[6u].m_bytes, "texture_period_m = 2.0\n",
                std::string{"texture_period_m = 2.0\n"} + suffix);
            const auto candidate = decodeCatalog(changed, diagnostics);
            PPR_TEST_ASSERT(candidate and candidate->materialFingerprint() == first->materialFingerprint());
        }
        auto explicit_default = fixtureSource();
        explicit_default.m_documents[6u].m_bytes = replace(
            explicit_default.m_documents[6u].m_bytes, "texture_period_m = 2.0\n",
            "texture_period_m = 2.0\ntexture_sampling = 'stochastic'\n");
        PPR_TEST_ASSERT(
            decodeCatalog(explicit_default, diagnostics)->materialFingerprint() == first->materialFingerprint());
        source.m_documents[0u].m_bytes = replace(
            source.m_documents[0u].m_bytes, "density = 2200.0", "density = 2201.0");
        PPR_TEST_ASSERT(decodeCatalog(source, diagnostics)->materialFingerprint() != first->materialFingerprint());
    };

    PPR_UNIT_TEST (required_values_are_not_cpp_defaults) {
        for (const auto &[category, field]:
            {std::pair{0u, "phase"}, std::pair{0u, "representation"}, std::pair{0u, "density"},
                std::pair{0u, "diggable"}, std::pair{6u, "display_name"}, std::pair{6u, "color_rgba"},
                std::pair{6u, "albedo_texture"}, std::pair{6u, "texture_period_m"}}) {
            auto source = fixtureSource();
            auto &bytes = source.m_documents[category].m_bytes;
            const auto begin = bytes.find(std::string{field} + " =");
            PPR_TEST_ASSERT(begin != std::string::npos);
            bytes.erase(begin, bytes.find('\n', begin) - begin + 1u);
            config::Diagnostics diagnostics;
            const auto catalog = decodeCatalog(source, diagnostics);
            PPR_TEST_ASSERT(not catalog and diagnostics.hasErrors());
            PPR_TEST_ASSERT(catalog.error() == config::make_error_code(config::EConfigErrc::missing_value));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property.starts_with(
                category == 0u ? "materials.sandstone" : "appearance.materials.sandstone"));
        }
    };

    PPR_UNIT_TEST (invalid_materials_fail_with_authored_key) {
        const std::tuple<std::size_t, std::string_view, std::string_view> cases[] = {
            {0u, "phase = \"solid\"", "phase = \"gas\""}, {0u, "phase = \"solid\"", "phase = \"plasma\""},
            {0u, "representation = \"terrain\"", "representation = \"fluid\""},
            {0u, "density = 2200.0", "density = inf"}, {0u, "density = 2200.0", "density = -2.0"},
            {0u, "diggable = true", "diggable = \"yes\""}, {0u, "diggable = true", "diggable = true\nnumeric_id = 16"},
            {6u, "display_name = \"Sandstone\"", "display_name = \"\""},
            {6u, "color_rgba = \"B59C72FF\"", "color_rgba = \"B59C72\""},
            {6u, "color_rgba = \"B59C72FF\"", "color_rgba = \"ZZ9C72FF\""}};
        for (const auto &[category, before, after]: cases) {
            auto source = fixtureSource();
            source.m_documents[category].m_bytes = replace(source.m_documents[category].m_bytes, before, after);
            config::Diagnostics diagnostics;
            PPR_TEST_ASSERT(not decodeCatalog(source, diagnostics) and diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property.starts_with(
                category == 0u ? "materials.sandstone." : "appearance.materials.sandstone."));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_location.starts_with(
                category == 0u ? "materials.toml" : "appearance.toml"));
        }
        for (const auto invalid:
            {"texture_period_m = 0.0", "texture_period_m = nan", "texture_period_m = 1e300",
                "texture_period_m = 1e-300", "texture_period_m = 1e-38", "roundness = -0.01", "roundness = 1.01",
                "roundness = nan", "metallic = -0.1", "roughness = 1.1", "emissive_r = nan",
                "emissive_strength = 1.0e100", "alpha_mode = 'mask'", "ior = 0.99", "ior = 3.01", "normal_scale = 4.01",
                "refraction_depth_m = 1.01", "texture_sampling = 'unknown'"}) {
            auto source = fixtureSource();
            source.m_documents[6u].m_bytes = replace(
                source.m_documents[6u].m_bytes, "texture_period_m = 2.0",
                std::string_view{invalid}.starts_with("texture_period_m")
                    ? std::string{invalid}
                    : std::string{"texture_period_m = 2.0\n"} + invalid);
            config::Diagnostics errors;
            PPR_TEST_ASSERT(not decodeCatalog(source, errors) and errors.hasErrors());
        }
    };

    PPR_UNIT_TEST (texture_paths_cannot_escape_pack) {
        for (const auto path:
            {"../rock.png", "/rock.png", "C:/rock.png", "textures/../../rock.png", "textures//rock.png",
                "textures/./rock.png", "textures/", "textures\\\\rock.png"}) {
            for (const bool normal: {false, true}) {
                auto source = fixtureSource();
                source.m_documents[6u].m_bytes =
                    normal ? replace(
                                 source.m_documents[6u].m_bytes, "texture_period_m = 2.0",
                                 "texture_period_m = 2.0\nnormal_texture = \"" + std::string{path} + "\"")
                           : replace(source.m_documents[6u].m_bytes, "sandstone_texture", path);
                config::Diagnostics diagnostics;
                PPR_TEST_ASSERT(not decodeCatalog(source, diagnostics));
                PPR_TEST_ASSERT(
                    diagnostics.entries().front().m_property ==
                    std::string{"appearance.materials.sandstone."} + (normal ? "normal_texture" : "albedo_texture"));
            }
        }
    };

    PPR_UNIT_TEST (particle_terrain_fields_and_authored_ids_are_rejected) {
        for (const auto &[category, before, after]:
            {std::tuple{0u, "density = 300.0", "density = 300.0\ndiggable = false"},
                std::tuple{0u, "density = 300.0", "density = 300.0\nnumeric_id = 1024"},
                std::tuple{6u, "display_name = \"Snow\"", "display_name = \"Snow\"\nroundness = 0.5"}}) {
            auto source = fixtureSource();
            source.m_documents[category].m_bytes = replace(source.m_documents[category].m_bytes, before, after);
            config::Diagnostics diagnostics;
            PPR_TEST_ASSERT(not decodeCatalog(source, diagnostics) and diagnostics.hasErrors());
        }
    };

    PPR_UNIT_TEST (version_names_types_and_budgets) {
        for (const auto &bytes:
            {"schema_version = 2\n" + rock, "schema_version = '3'\n" + rock, std::string{""},
                "" + replace(rock, "materials.sandstone", "materials.Sandstone"),
                "" + replace(rock, "materials.sandstone", "materials.\"sand.stone\""),
                "" + rock + "values = [1,2]\n"}) {
            auto source = fixtureSource();
            source.m_documents[0u].m_bytes = bytes;
            config::Diagnostics diagnostics;
            PPR_TEST_ASSERT(not decodeCatalog(source, diagnostics) and diagnostics.hasErrors());
        }
        auto source = fixtureSource();
        source.m_documents[0u].m_bytes.assign(kMaxMaterialDocumentBytes + 1u, ' ');
        config::Diagnostics large;
        PPR_TEST_ASSERT(not resolveFixtureDefinition(source, large));
        source.m_documents[0u].m_bytes = "";
        for (std::size_t i = 0u; i <= kMaxContentMaterials; ++i) {
            source.m_documents[0u].m_bytes += replace(
                rock, "materials.sandstone", "materials.rock_" + std::to_string(i));
        }
        config::Diagnostics count;
        source.m_documents[6u].m_bytes =
            "[appearance.biomes.plain]\ndisplay_name = 'Plain'\npreview_color_rgba = '709040FF'\n";
        const auto result = resolveFixtureDefinition(source, count);
        PPR_TEST_ASSERT(not result and result.error() == config::make_error_code(config::EConfigErrc::out_of_range));
    };

    PPR_UNIT_TEST (failed_candidate_preserves_previous_catalog) {
        auto source = fixtureSource();
        config::Diagnostics diagnostics;
        auto catalog = decodeCatalog(source, diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        const auto before = catalog->materialFingerprint();
        source.m_documents[0u].m_bytes = replace(source.m_documents[0u].m_bytes, "density = 2200.0", "density = -1.0");
        auto candidate = decodeCatalog(source, diagnostics);
        if (candidate) {
            catalog = std::move(candidate);
        }
        PPR_TEST_ASSERT(catalog->materialFingerprint() == before);
        PPR_TEST_ASSERT(catalog->findMaterial("sandstone") != nullptr);
    };

    PPR_UNIT_TEST (empty_and_ambiguous_records_are_not_erased) {
        for (const auto suffix: {"\n[materials.empty]\n", "\nunknown_field = {}\n", "\n[unknown]\n"}) {
            auto source = fixtureSource();
            source.m_documents[0u].m_bytes += suffix;
            config::Diagnostics diagnostics;
            PPR_TEST_ASSERT(not decodeCatalog(source, diagnostics) and diagnostics.hasErrors());
        }
        auto source = fixtureSource();
        source.m_documents[0u].m_bytes = replace(
            source.m_documents[0u].m_bytes, "materials.sandstone", "\"materials.sandstone\"");
        config::Diagnostics diagnostics;
        PPR_TEST_ASSERT(not decodeCatalog(source, diagnostics));
    };

    PPR_UNIT_TEST (excess_diagnostics_remain_bounded_and_reject_content) {
        auto source = fixtureSource();
        std::string extras;
        for (u32 i = 0u; i < 200u; ++i) {
            extras += "unknown_" + std::to_string(i) + " = 1\n";
        }
        source.m_documents[0u].m_bytes = replace(
            source.m_documents[0u].m_bytes, "density = 2200.0\n", "density = 2200.0\n" + extras);
        config::Diagnostics capped{2u};
        PPR_TEST_ASSERT(not resolveFixtureDefinition(source, capped) and capped.hasErrors());
        PPR_TEST_ASSERT(capped.entries().size() == 2u and capped.size() == 200u and capped.omittedCount() == 198u);
        PPR_TEST_ASSERT(capped.entries().front().m_property.starts_with("materials.sandstone."));
        PPR_TEST_ASSERT(capped.format().find("further diagnostics omitted") != std::string::npos);
        PPR_TEST_ASSERT(not resolveFixtureDefinition(source, capped));
        PPR_TEST_ASSERT(capped.entries().size() == 2u and capped.size() == 400u and capped.omittedCount() == 398u);
    };

    PPR_UNIT_TEST (allocation_failure_reaches_fatal_boundary) {
        constexpr const char *arm_name = "PPR_GAME_CONTENT_ALLOCATION_ARMED";
        std::string previous_arm;
#ifdef _WIN32
        char *stored{};
        PPR_TEST_ASSERT(::_dupenv_s(&stored, nullptr, arm_name) == 0);
        if (stored != nullptr) {
            PPR_DEFER {
                std::free(stored);
            };
            previous_arm = stored;
        }
#else
        if (const char *const stored = std::getenv(arm_name)) {
            previous_arm = stored;
        }
#endif
        if (previous_arm != "1") {
            const auto executable = hal::process::currentExecutablePath();
            const auto set_arm = [&](const char *const value) {
#ifdef _WIN32
                PPR_TEST_ASSERT(::_putenv_s(arm_name, value) == 0);
#else
                PPR_TEST_ASSERT(*value == '\0' ? ::unsetenv(arm_name) == 0 : ::setenv(arm_name, value, 1) == 0);
#endif
            };
            set_arm("1");
            PPR_DEFER {
                set_arm(previous_arm.c_str());
            };
            const std::vector<std::string> arguments{
                "--child-run", "--loop", "1", "--run-test", "game/content/allocation_failure_reaches_fatal_boundary"};
            const int exit_code = hal::process::spawnAndWait(executable, arguments);
            PPR_TEST_ASSERT(exit_code == 73 or exit_code == 74);
            return;
        }

        const auto source = fixtureSource();
        config::Diagnostics diagnostics;
        // Debug STL proxy construction can be noexcept. Accept only a confirmed
        // bad_alloc, whether caught here (73) or reaching terminate (74).
        std::set_terminate([]() noexcept {
            const auto exception = std::current_exception();
            if (not exception) {
                std::_Exit(75);
            }
            try {
                std::rethrow_exception(exception);
            } catch (const std::bad_alloc &) {
                std::_Exit(74);
            } catch (...) {
                std::_Exit(76);
            }
        });
        try {
            AllocationFailure failure{};
            (void) resolveFixtureDefinition(source, diagnostics);
        } catch (const std::bad_alloc &) {
            std::_Exit(73);
        } catch (...) {
            std::_Exit(77);
        }
        std::_Exit(78);
    };
    PPR_UNIT_TEST (pack_source_owns_bytes_and_bounds_io) {
        const auto root = std::filesystem::temp_directory_path() /
                          ("ppr-worldgen-pack-" + std::to_string(std::random_device{}()));
        PPR_TEST_ASSERT(std::filesystem::create_directory(root));
        PPR_DEFER {
            std::error_code cleanup;
            std::filesystem::remove_all(root, cleanup);
            PPR_TEST_ASSERT(not cleanup);
        };

        const auto write = [&](const char *name, const std::string_view bytes) {
            std::ofstream file{root / name, std::ios::binary | std::ios::trunc};
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            PPR_TEST_ASSERT(file.good());
        };

        write("pack.toml", manifest);
        write("rock.toml", element);

        const std::filesystem::path roots[] = {root};
        config::Diagnostics diagnostics;
        auto loaded = loadContentSourceSet(roots, diagnostics);
        PPR_TEST_ASSERT(loaded.has_value());
        PPR_TEST_ASSERT(loaded->source().m_packs.front().m_manifest.m_bytes == manifest);
        PPR_TEST_ASSERT(
            loaded->source().m_descriptors.front().m_path == std::filesystem::canonical(root / "rock.toml"));

        write("rock.toml", "changed\n");
        PPR_TEST_ASSERT(loaded->source().m_descriptors.front().m_bytes == element);
        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = loadContentSourceSet(roots, diagnostics, job_context.get());
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);

        std::filesystem::resize_file(root / "rock.toml", asset::kMaxDescriptorBytes + 1u);
        const auto oversized = loadContentSourceSet(roots, diagnostics);
        PPR_TEST_ASSERT(not oversized and oversized.error() == config::EConfigErrc::out_of_range);

        std::filesystem::remove(root / "rock.toml");
        const auto missing = loadContentSourceSet(roots, diagnostics);
        PPR_TEST_ASSERT(not missing and missing.error() == std::errc::no_such_file_or_directory);

        write("rock.toml", element);
        std::filesystem::create_hard_link(root / "rock.toml", root / "alias.toml");
        write("pack.toml", manifest + "\n[assets.rock_alias]\nkind = 'sim.element'\ndescriptor = 'alias.toml'\n");
        const auto duplicate = loadContentSourceSet(roots, diagnostics);
        PPR_TEST_ASSERT(not duplicate and duplicate.error() == config::EConfigErrc::duplicate_key);

        std::filesystem::remove(root / "alias.toml");
        std::filesystem::create_hard_link(root / "pack.toml", root / "alias.toml");
        const auto manifest_alias = loadContentSourceSet(roots, diagnostics);
        PPR_TEST_ASSERT(not manifest_alias and manifest_alias.error() == config::EConfigErrc::duplicate_key);
    };

    PPR_UNIT_TEST (field_optional_controls_accept_finite_inclusive_endpoints) {
        const std::string header = "[fields.boundary]\nalgorithm = \"perlin\"\nfrequency_per_metre = 0.001\n";
        for (const auto controls:
            {"lacunarity = 1.0\ngain = 0.0\nwarp_amplitude_m = 0.0\n",
                "lacunarity = 4.0\ngain = 1.0\nwarp_amplitude_m = 4096.0\n"}) {
            config::Diagnostics diagnostics{};
            const auto fields = decodeFields(header + controls, "fields.toml", diagnostics);
            PPR_TEST_ASSERT(fields.has_value() and fields->size() == 1u);
            PPR_TEST_ASSERT(not diagnostics.hasErrors());
        }
        for (const auto property: {"lacunarity", "gain", "warp_amplitude_m"}) {
            for (const auto value: {"nan", "inf"}) {
                config::Diagnostics diagnostics{};
                const auto fields = decodeFields(header + property + " = " + value + "\n", "fields.toml", diagnostics);
                PPR_TEST_ASSERT(not fields.has_value());
                PPR_TEST_ASSERT(diagnostics.entries().back().m_property == std::string{"fields.boundary."} + property);
            }
        }
    };
    PPR_UNIT_TEST (typed_fields_are_owned_sorted_and_compile_to_noise) {
        const std::string caves = R"(
[fields.caves]
algorithm = "open_simplex2"
frequency_per_metre = 0.025
fractal = "fbm"
octaves = 5
lacunarity = 2.0
gain = 0.45
warp_amplitude_m = 12.0
warp_frequency_per_metre = 0.006
)";
        const std::string deposits = R"(
[fields.deposits]
algorithm = "cellular_distance"
frequency_per_metre = 0.125
)";
        config::Diagnostics diagnostics{};
        auto fields = decodeFields("" + deposits + caves, "fields.toml", diagnostics);
        auto reordered = decodeFields("" + caves + deposits, "other.toml", diagnostics);
        PPR_TEST_ASSERT(fields and reordered and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(fields->size() == 2u and fields->front().m_key == "caves");
        PPR_TEST_ASSERT(fields->back().m_noise.m_fractal == sim::ENoiseFractal::none);
        PPR_TEST_ASSERT(fields->back().m_noise.m_octaves == 4u);
        const auto categorical = decodeFields(
            "[fields.palette]\nalgorithm = \"cellular_value\"\nfrequency_per_metre = 0.125\n", "fields.toml",
            diagnostics);
        PPR_TEST_ASSERT(categorical.has_value());
        PPR_TEST_ASSERT(categorical->front().m_noise.m_algorithm == sim::ENoiseAlgorithm::cellular_value);
        auto compiled = sim::compileNoiseField(fields->front().m_noise, 42u);
        auto replay = sim::compileNoiseField(reordered->front().m_noise, 42u);
        PPR_TEST_ASSERT(compiled and replay);
        PPR_TEST_ASSERT(compiled->sample(12.25, 13.75) == replay->sample(12.25, 13.75));
    };

    PPR_UNIT_TEST (field_required_values_and_numeric_limits_are_enforced) {
        const std::string valid = "[fields.caves]\nalgorithm = \"perlin\"\nfrequency_per_metre = 0.025\n";
        using config::EConfigErrc;
        const std::tuple<std::string_view, std::string_view, std::string_view, EConfigErrc> cases[] = {
            {"algorithm = \"perlin\"\n", "", "algorithm", EConfigErrc::missing_value},
            {"frequency_per_metre = 0.025\n", "", "frequency_per_metre", EConfigErrc::missing_value},
            {"\"perlin\"", "\"unsupported\"", "algorithm", EConfigErrc::unknown_enum_value},
            {"0.025", "0.0", "frequency_per_metre", EConfigErrc::out_of_range},
            {"0.025", "1e-300", "", EConfigErrc::out_of_range}, {"0.025", "1e300", "", EConfigErrc::out_of_range},
            {"0.025", "nan", "frequency_per_metre", EConfigErrc::non_finite_not_allowed},
            {"0.025", "inf", "frequency_per_metre", EConfigErrc::non_finite_not_allowed},
            {"0.025", "0.025\noctaves = 17", "octaves", EConfigErrc::out_of_range},
            {"0.025", "0.025\nwarp_amplitude_m = -1.0", "warp_amplitude_m", EConfigErrc::out_of_range},
            {"0.025", "0.025\nwarp_frequency_per_metre = 1e300", "", EConfigErrc::out_of_range},
            {"0.025", "0.025\nlacunarity = 5.0", "lacunarity", EConfigErrc::out_of_range},
            {"0.025", "0.025\ngain = 1.01", "gain", EConfigErrc::out_of_range},
            {"0.025", "0.025\nunknown = true", "unknown", EConfigErrc::unknown_property}};
        for (const auto &[from, to, suffix, code]: cases) {
            const auto source = replace(valid, from, to);
            config::Diagnostics diagnostics{};
            const auto fields = decodeFields(source, "invalid-fields.toml", diagnostics);
            PPR_TEST_ASSERT(not fields and diagnostics.hasErrors());
            // Sampling safety combines octave frequency and coordinate reach;
            // the canonical validator identifies that field group as a whole.
            const std::string property = suffix.empty() ? "fields.caves" : "fields.caves." + std::string{suffix};
            if (fields.error() != config::make_error_code(code) or
                diagnostics.entries().front().m_property != property) {
                throw std::system_error(fields.error(), property + "\n" + diagnostics.format());
            }
            PPR_TEST_ASSERT(fields.error() == config::make_error_code(code));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == property);

            config::Diagnostics codec_diagnostics;
            const auto document = config::decode(
                source, "invalid-fields.toml", codec_diagnostics, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            const auto entry = std::ranges::find(document->m_entries, property, &config::Entry::m_key);
            if (entry != document->m_entries.end()) {
                PPR_TEST_ASSERT(diagnostics.entries().front().m_location == entry->m_location);
            } else {
                const auto group = std::ranges::find(
                    document->m_groups, "fields.caves", &config::GroupPresence::m_path);
                PPR_TEST_ASSERT(group != document->m_groups.end());
                PPR_TEST_ASSERT(diagnostics.entries().front().m_location == group->m_location);
            }
        }
        std::string excessive = "";
        for (std::size_t index = 0u; index <= kMaxContentFields; ++index) {
            excessive += "[fields.field_" +
                         std::to_string(index) +
                         "]\nalgorithm = \"perlin\"\nfrequency_per_metre = 0.025\n";
        }
        config::Diagnostics diagnostics{};
        const auto rejected = decodeFields(excessive, "count.toml", diagnostics);
        PPR_TEST_ASSERT(
            not rejected and rejected.error() == config::make_error_code(config::EConfigErrc::out_of_range));
    };


}

namespace pP::game::tests {
    const UnitTest content = UnitTest::Named("content") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::content::definition_envelope_requires_version_two_and_locates_logical_errors,
                detail::content::pack_fields_delegate_to_canonical_noise_schema,
                detail::content::pack_field_presence_and_domain_errors_match_canonical_schema,
                detail::content::decoded_fragment_requires_explicit_world_completion_to_compile,
                detail::content::named_manifest_schema_and_background_validation,
                detail::content::owned_materials_use_derived_ids_and_preserve_appearance,
                detail::content::canonical_material_identity, detail::content::required_values_are_not_cpp_defaults,
                detail::content::invalid_materials_fail_with_authored_key,
                detail::content::texture_paths_cannot_escape_pack,
                detail::content::particle_terrain_fields_and_authored_ids_are_rejected,
                detail::content::version_names_types_and_budgets,
                detail::content::failed_candidate_preserves_previous_catalog,
                detail::content::empty_and_ambiguous_records_are_not_erased,
                detail::content::excess_diagnostics_remain_bounded_and_reject_content,
                detail::content::allocation_failure_reaches_fatal_boundary,
                detail::content::pack_source_owns_bytes_and_bounds_io,
                detail::content::field_optional_controls_accept_finite_inclusive_endpoints,
                detail::content::typed_fields_are_owned_sorted_and_compile_to_noise,
                detail::content::field_required_values_and_numeric_limits_are_enforced});
    };
    const UnitTest &gameContentTests() noexcept {
        return content;
    }
}
