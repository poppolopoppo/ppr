module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import engine.sim;
import game.colony.content;
import std;

namespace pP::game::tests::detail::biomes {
    // ------------------------------------------------------------------
    // content biomes
    // ------------------------------------------------------------------

    using namespace colony;

    const std::string materials_text = R"(schema_version = 2
[materials.rock]
numeric_id = 16
display_name = "Rock"
phase = "solid"
representation = "terrain"
color_rgba = "AABBCCFF"
albedo_texture = "textures/rock.png"
texture_period_m = 2.0
diggable = true
reference_density_kg_m3 = 2200.0
[materials.ore]
numeric_id = 501
display_name = "Ore"
phase = "solid"
representation = "terrain"
color_rgba = "112233FF"
albedo_texture = "textures/ore.png"
texture_period_m = 1.0
diggable = true
reference_density_kg_m3 = 3000.0
[materials.sand]
numeric_id = 32
display_name = "Sand"
phase = "solid"
representation = "particle"
color_rgba = "EECCAAFF"
reference_density_kg_m3 = 1600.0
)";
    const std::string fields_text = R"(schema_version = 2
[fields.composition]
algorithm = "perlin"
frequency_per_metre = 0.05
[fields.temperature]
algorithm = "open_simplex2"
frequency_per_metre = 0.01
)";
    const std::string base = R"(
[biomes.temperate]
display_name = "Temperate"
preview_color_rgba = "709040FF"
temperature_base_k = 295.0
temperature_variation_k = 10.0
temperature_field = "temperature"
)";
    const std::string shallow = R"(
[biomes.temperate.composition.shallow]
order = 10
depth_min_normalized = 0.0
depth_max_normalized = 0.5
field = "composition"
[biomes.temperate.composition.shallow.materials.zrock]
material = "rock"
weight = 3.0
[biomes.temperate.composition.shallow.materials.aore]
material = "ore"
weight = 1.0
)";
    const std::string deep = R"(
[biomes.temperate.composition.deep]
order = 20
depth_min_normalized = 0.5
depth_max_normalized = 1.0
field = "composition"
[biomes.temperate.composition.deep.materials.host]
material = "rock"
weight = 1.0
)";
    const std::string deposit = R"(
[biomes.temperate.deposits.copper]
kind = "threshold"
order = 5
field = "composition"
material = "ore"
host_material = "rock"
depth_min_normalized = 0.2
depth_max_normalized = 0.8
threshold_min = 0.3
threshold_max = 1.0
)";
    std::string validDocument() {
        return "schema_version = 2\n" + base + shallow + deep + deposit;
    }

    void replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto position = text.find(before);
        PPR_TEST_ASSERT(position != std::string::npos);
        text.replace(position, before.size(), after);
    }

    PPR_UNIT_TEST (owned_sorted_composition_and_terrain_references) {
        config::Diagnostics diagnostics{};
        const auto materials = decodeMaterialCatalog(materials_text, "materials.toml", diagnostics);
        const auto fields = decodeFieldDefinitions(fields_text, "fields.toml", diagnostics);
        PPR_TEST_ASSERT(materials.has_value() and fields.has_value());
        std::string source = validDocument();
        auto recipes = decodeBiomeDefinitions(source, "biomes.toml", *materials, *fields, diagnostics);
        PPR_TEST_ASSERT(recipes.has_value() and recipes->size() == 1u);
        source.clear();
        const auto &biome = recipes->front();
        PPR_TEST_ASSERT(biome.m_key == "temperate" and biome.m_display_name == "Temperate");
        PPR_TEST_ASSERT(biome.m_preview_color_rgba == 0x709040ffu);
        PPR_TEST_ASSERT(biome.m_temperature_base_k == 295.0 and biome.m_temperature_variation_k == 10.0);
        PPR_TEST_ASSERT(biome.m_temperature_field == "temperature");
        PPR_TEST_ASSERT(biome.m_composition.size() == 2u and biome.m_composition[0u].m_key == "shallow");
        const auto &band = biome.m_composition[0u];
        PPR_TEST_ASSERT(band.m_total_weight == 4.0 and band.m_materials.size() == 2u);
        PPR_TEST_ASSERT(band.m_materials[0u].m_key == "aore" and band.m_materials[0u].m_material_id == 501u);
        PPR_TEST_ASSERT(band.m_materials[1u].m_key == "zrock" and band.m_materials[1u].m_material_id == 16u);
        PPR_TEST_ASSERT(biome.m_deposits.size() == 1u);
        PPR_TEST_ASSERT(biome.m_deposits[0u].m_material_id == 501u and biome.m_deposits[0u].m_host_material_id == 16u);

        const auto reordered = decodeBiomeDefinitions(
            "schema_version = 2\n" + deposit + deep + shallow + base, "other/location.toml", *materials, *fields,
            diagnostics);
        PPR_TEST_ASSERT(reordered.has_value());
        PPR_TEST_ASSERT(reordered->front().m_composition[0u].m_materials[0u].m_material_id == 501u);
        PPR_TEST_ASSERT(reordered->front().m_composition[0u].m_total_weight == band.m_total_weight);
        const auto no_deposits = decodeBiomeDefinitions(
            "schema_version = 2\n" + base + shallow + deep, "biomes.toml", *materials, *fields, diagnostics);
        PPR_TEST_ASSERT(no_deposits.has_value() and no_deposits->front().m_deposits.empty());
        auto zero_variation = validDocument();
        replace(zero_variation, "temperature_variation_k = 10.0", "temperature_variation_k = 0.0");
        replace(zero_variation, "threshold_min = 0.3", "threshold_min = -1.0");
        const auto endpoints = decodeBiomeDefinitions(zero_variation, "biomes.toml", *materials, *fields, diagnostics);
        PPR_TEST_ASSERT(endpoints.has_value() and endpoints->front().m_temperature_variation_k == 0.0);
        auto porous = validDocument();
        replace(porous, "material = \"ore\"", "material = \"vacuum\"");
        replace(
            porous, "temperature_field = \"temperature\"",
            "temperature_field = \"temperature\"\nsprinkle_probability = 0.4\nrelaxation_passes = 21");
        const auto holes = decodeBiomeDefinitions(porous, "biomes.toml", *materials, *fields, diagnostics);
        PPR_TEST_ASSERT(holes.has_value());
        PPR_TEST_ASSERT(holes->front().m_composition[0u].m_materials[0u].m_material_id == 0u);
        PPR_TEST_ASSERT(holes->front().m_sprinkle_probability == 0.4 and holes->front().m_relaxation_passes == 21u);
        for (const auto probability: {"-0.1", "1.1", "nan"}) {
            auto invalid = porous;
            replace(invalid, "sprinkle_probability = 0.4", std::string{"sprinkle_probability = "} + probability);
            PPR_TEST_ASSERT(not decodeBiomeDefinitions(invalid, "biomes.toml", *materials, *fields, diagnostics));
        }
    };

    PPR_UNIT_TEST (finite_blob_controls_validate_and_preserve_legacy_defaults) {
        config::Diagnostics setup{};
        const auto materials = decodeMaterialCatalog(materials_text, "materials.toml", setup);
        const auto fields = decodeFieldDefinitions(fields_text, "fields.toml", setup);
        PPR_TEST_ASSERT(materials.has_value() and fields.has_value());
        const auto legacy = decodeBiomeDefinitions(validDocument(), "biomes.toml", *materials, *fields, setup);
        PPR_TEST_ASSERT(legacy.has_value());
        const auto &original = legacy->front();
        PPR_TEST_ASSERT(original.m_relaxation_passes == 0u and original.m_relaxation_adoption == 0.5);
        PPR_TEST_ASSERT(original.m_erosion_passes == 0u);
        auto erosion = validDocument();
        replace(
            erosion, "temperature_field = \"temperature\"", "temperature_field = \"temperature\"\nerosion_passes = 24");
        const auto erosion_only = decodeBiomeDefinitions(erosion, "biomes.toml", *materials, *fields, setup);
        PPR_TEST_ASSERT(erosion_only and erosion_only->front().m_erosion_passes == 24u);
        for (const auto replacement: {"erosion_passes = 25", "erosion_passes = 24\nrelaxation_passes = 1"}) {
            auto invalid = erosion;
            replace(invalid, "erosion_passes = 24", replacement);
            config::Diagnostics rejected;
            PPR_TEST_ASSERT(not decodeBiomeDefinitions(invalid, "biomes.toml", *materials, *fields, rejected));
        }
        PPR_TEST_ASSERT(original.m_deposits.front().m_shape == sim::EGenerationDepositShape::field);
        PPR_TEST_ASSERT(original.m_deposits.front().m_stage == sim::EGenerationDepositStage::bulk);
        PPR_TEST_ASSERT(
            original.m_deposits.front().m_orientation_min_degrees == 0.0 and
            original.m_deposits.front().m_orientation_max_degrees == 360.0);
        auto blob = validDocument();
        replace(
            blob, "temperature_field = \"temperature\"",
            "temperature_field = \"temperature\"\nrelaxation_passes = 8\nrelaxation_adoption = 0.4");
        replace(blob, "threshold_min = 0.3\nthreshold_max = 1.0", R"(shape = "blob"
stage = "detail"
blob_count = 4096
radius_min_m = 2.0
radius_max_m = 5.0
stretch_min = 0.1
stretch_max = 16.0
distortion = 0.45
orientation_min_degrees = 80.0
orientation_max_degrees = 100.0
core_coverage = 0.9
fringe_coverage = 0.2
sprinkle_probability = 0.02)");
        config::Diagnostics diagnostics{};
        const auto accepted = decodeBiomeDefinitions(blob, "biomes.toml", *materials, *fields, diagnostics);
        PPR_TEST_ASSERT(accepted.has_value());
        const auto &biome = accepted->front();
        const auto &item = biome.m_deposits.front();
        PPR_TEST_ASSERT(biome.m_relaxation_passes == 8u and biome.m_relaxation_adoption == 0.4);
        PPR_TEST_ASSERT(
            item.m_shape == sim::EGenerationDepositShape::blob and
            item.m_stage == sim::EGenerationDepositStage::detail);
        PPR_TEST_ASSERT(item.m_blob_count == 4096u and item.m_radius_min_m == 2.0 and item.m_radius_max_m == 5.0);
        PPR_TEST_ASSERT(item.m_stretch_min == 0.1 and item.m_stretch_max == 16.0 and item.m_distortion == 0.45);
        PPR_TEST_ASSERT(item.m_orientation_min_degrees == 80.0 and item.m_orientation_max_degrees == 100.0);
        auto fixed_orientation = blob;
        replace(fixed_orientation, "orientation_max_degrees = 100.0", "orientation_max_degrees = 80.0");
        PPR_TEST_ASSERT(
            decodeBiomeDefinitions(fixed_orientation, "biomes.toml", *materials, *fields, diagnostics).has_value());
        PPR_TEST_ASSERT(
            item.m_core_coverage == 0.9 and item.m_fringe_coverage == 0.2 and item.m_sprinkle_probability == 0.02);
        const std::pair<std::string_view, std::string_view> failures[] = {
            {"relaxation_passes = 8", "relaxation_passes = 25"},
            {"relaxation_adoption = 0.4", "relaxation_adoption = nan"},
            {"relaxation_adoption = 0.4", "relaxation_adoption = 1.1"},
            {"shape = \"blob\"", "shape = \"field\""},
            {"stage = \"detail\"", "stage = \"bulk\""},
            {"blob_count = 4096", "blob_count = 0"},
            {"blob_count = 4096", "blob_count = 4097"},
            {"radius_min_m = 2.0", "radius_min_m = 0.0"},
            {"radius_max_m = 5.0", "radius_max_m = 1.0"},
            {"radius_max_m = 5.0", "radius_max_m = 257.0"},
            {"radius_max_m = 5.0", "radius_max_m = 100.0"},
            {"stretch_min = 0.1", "stretch_min = 0.09"},
            {"stretch_max = 16.0", "stretch_max = 17.0"},
            {"distortion = 0.45", "distortion = 0.46"},
            {"orientation_min_degrees = 80.0", "orientation_min_degrees = -0.1"},
            {"orientation_max_degrees = 100.0", "orientation_max_degrees = 360.1"},
            {"orientation_max_degrees = 100.0", "orientation_max_degrees = 79.0"},
            {"orientation_max_degrees = 100.0", "orientation_max_degrees = nan"},
            {"core_coverage = 0.9", "core_coverage = 0.1"},
            {"fringe_coverage = 0.2", "fringe_coverage = -0.1"},
            {"sprinkle_probability = 0.02", "sprinkle_probability = 0.021"},
        };
        for (const auto &[before, after]: failures) {
            auto invalid = blob;
            replace(invalid, before, after);
            config::Diagnostics rejected{};
            PPR_TEST_ASSERT(
                not decodeBiomeDefinitions(invalid, "biomes.toml", *materials, *fields, rejected).has_value());
            PPR_TEST_ASSERT(rejected.hasErrors());
        }
        auto second = deposit;
        replace(second, "deposits.copper", "deposits.second");
        replace(second, "order = 5", "order = 6");
        replace(
            second, "threshold_min = 0.3\nthreshold_max = 1.0",
            "shape = \"blob\"\nblob_count = 1\nradius_min_m = 1.0\nradius_max_m = 1.0");
        config::Diagnostics aggregate{};
        PPR_TEST_ASSERT(
            not decodeBiomeDefinitions(blob + second, "biomes.toml", *materials, *fields, aggregate).has_value());
        PPR_TEST_ASSERT(aggregate.entries().back().m_property == "biomes");
        auto mixed = validDocument();
        replace(mixed, "order = 5", "order = 5\ncore_coverage = 0.5");
        config::Diagnostics neutral{};
        PPR_TEST_ASSERT(not decodeBiomeDefinitions(mixed, "biomes.toml", *materials, *fields, neutral).has_value());
        mixed = validDocument();
        replace(mixed, "order = 5", "order = 5\norientation_max_degrees = 90.0");
        PPR_TEST_ASSERT(not decodeBiomeDefinitions(mixed, "biomes.toml", *materials, *fields, neutral).has_value());
    };

    PPR_UNIT_TEST (invalid_recipes_keep_qualified_diagnostics) {
        config::Diagnostics setup{};
        const auto materials = decodeMaterialCatalog(materials_text, "materials.toml", setup);
        const auto fields = decodeFieldDefinitions(fields_text, "fields.toml", setup);
        PPR_TEST_ASSERT(materials.has_value() and fields.has_value());
        const std::pair<std::string_view, std::string_view> failures[] = {
            {"temperature_base_k = 295.0", ""},
            {"temperature_variation_k = 10.0", "temperature_variation_k = 300.0"},
            {"temperature_variation_k = 10.0", "temperature_variation_k = nan"},
            {"temperature_variation_k = 10.0", "temperature_variation_k = inf"},
            {"preview_color_rgba = \"709040FF\"", "preview_color_rgba = \"709040XZ\""},
            {"temperature_field = \"temperature\"", "temperature_field = \"absent\""},
            {"field = \"composition\"", "field = \"absent\""},
            {"material = \"ore\"", "material = \"absent\""},
            {"material = \"ore\"", "material = \"sand\""},
            {"composition.deep.materials.host]\nmaterial = \"rock\"",
                "composition.deep.materials.host]\nmaterial = \"vacuum\""},
            {"host_material = \"rock\"", "host_material = \"sand\""},
            {"weight = 3.0", "weight = 0.0"},
            {"weight = 3.0", "weight = nan"},
            {"weight = 3.0", "weight = inf"},
            {"order = 20", "order = 10"},
            {"depth_min_normalized = 0.5", "depth_min_normalized = 0.4"},
            {"depth_min_normalized = 0.5", "depth_min_normalized = 0.6"},
            {"depth_min_normalized = 0.5", "depth_min_normalized = nan"},
            {"threshold_min = 0.3", "threshold_min = nan"},
            {"depth_max_normalized = 1.0", "depth_max_normalized = 0.9"},
            {"threshold_min = 0.3", "threshold_min = 1.0"},
            {"kind = \"threshold\"", "kind = \"unknown\""},
            {"order = 5", "order = 5\nunknown = true"},
            {"composition.shallow]", "composition.Upper]"},
            {"materials.zrock]", "materials.Upper]"},
        };
        for (const auto &[before, after]: failures) {
            auto source = validDocument();
            replace(source, before, after);
            config::Diagnostics diagnostics{};
            const auto failed = decodeBiomeDefinitions(source, "biomes.toml", *materials, *fields, diagnostics);
            PPR_TEST_ASSERT(not failed.has_value() and diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().back().m_property.starts_with("biomes.temperate."));
            PPR_TEST_ASSERT(diagnostics.entries().back().m_location.find("biomes.toml") != std::string::npos);
        }
        for (const auto &[first, second]: {std::pair{"1e308", "1e308"}, std::pair{"5e-324", "1e308"}}) {
            auto source = validDocument();
            replace(source, "weight = 1.0", std::string{"weight = "} + first);
            replace(source, "weight = 3.0", std::string{"weight = "} + second);
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(
                not decodeBiomeDefinitions(source, "biomes.toml", *materials, *fields, diagnostics).has_value());
        }
        auto duplicate = deposit;
        replace(duplicate, "deposits.copper", "deposits.second");
        config::Diagnostics duplicates{};
        PPR_TEST_ASSERT(
            not decodeBiomeDefinitions(validDocument() + duplicate, "biomes.toml", *materials, *fields, duplicates)
                .has_value());
    };

    PPR_UNIT_TEST (deposit_interval_errors_identify_the_offending_field) {
        config::Diagnostics setup{};
        const auto materials = decodeMaterialCatalog(materials_text, "materials.toml", setup);
        const auto fields = decodeFieldDefinitions(fields_text, "fields.toml", setup);
        PPR_TEST_ASSERT(materials.has_value() and fields.has_value());
        for (const bool invalid_depth: {true, false}) {
            auto invalid_deposit = deposit;
            replace(
                invalid_deposit, invalid_depth ? "depth_max_normalized = 0.8" : "threshold_max = 1.0",
                invalid_depth ? "depth_max_normalized = 0.1" : "threshold_max = 0.1");
            const auto source = "schema_version = 2\n" + base + shallow + deep + invalid_deposit;
            const std::string property = std::string{"biomes.temperate.deposits.copper."} +
                                         (invalid_depth ? "depth_max_normalized" : "threshold_max");
            config::Diagnostics codec_diagnostics{};
            const auto document = config::decode(
                source, "biomes.toml", codec_diagnostics, config::EDecodeMode::records);
            PPR_TEST_ASSERT(document.has_value());
            const auto entry = std::ranges::find(document->m_entries, property, &config::Entry::m_key);
            PPR_TEST_ASSERT(entry != document->m_entries.end());
            config::Diagnostics diagnostics{};
            const auto failed = decodeBiomeDefinitions(source, "biomes.toml", *materials, *fields, diagnostics);
            PPR_TEST_ASSERT(not failed.has_value() and diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().back().m_property == property);
            PPR_TEST_ASSERT(diagnostics.entries().back().m_location == entry->m_location);
        }
    };

    PPR_UNIT_TEST (child_counts_are_bounded_before_compilation) {
        config::Diagnostics setup{};
        const auto materials = decodeMaterialCatalog(materials_text, "materials.toml", setup);
        const auto fields = decodeFieldDefinitions(fields_text, "fields.toml", setup);
        PPR_TEST_ASSERT(materials.has_value() and fields.has_value());
        const std::pair<std::string_view, std::size_t> collections[] = {
            {"composition", kMaxBiomeCompositionBands}, {"deposits", kMaxBiomeDeposits},
            {"composition.shallow.materials", kMaxCompositionMaterials}};
        for (const auto &[collection, limit]: collections) {
            std::string source = "schema_version = 2\n" + base;
            for (std::size_t i = 0u; i <= limit; ++i) {
                source += "[biomes.temperate." + std::string{collection} + ".b" + std::to_string(i) + "]\norder = 1\n";
            }
            config::Diagnostics diagnostics{};
            const auto failed = decodeBiomeDefinitions(source, "biomes.toml", *materials, *fields, diagnostics);
            PPR_TEST_ASSERT(not failed.has_value());
            PPR_TEST_ASSERT(failed.error() == config::make_error_code(config::EConfigErrc::out_of_range));
        }
    };
}

namespace pP::game::tests {
    const UnitTest biome_content = UnitTest::Named("biome_content") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::biomes::owned_sorted_composition_and_terrain_references,
                detail::biomes::invalid_recipes_keep_qualified_diagnostics,
                detail::biomes::finite_blob_controls_validate_and_preserve_legacy_defaults,
                detail::biomes::deposit_interval_errors_identify_the_offending_field,
                detail::biomes::child_counts_are_bounded_before_compilation});
    };
    const UnitTest &gameBiomeContentTests() noexcept {
        return biome_content;
    }
}
