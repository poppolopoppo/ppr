module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import engine.sim;
import game.colony.content;
import std;

namespace pP::game::tests::detail::biomes {
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
                detail::biomes::deposit_interval_errors_identify_the_offending_field,
                detail::biomes::child_counts_are_bounded_before_compilation});
    };
    const UnitTest &gameBiomeContentTests() noexcept {
        return biome_content;
    }
}
