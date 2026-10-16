module;
#include "pP/UnitTest.h"

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

namespace pP::game::tests::detail::biomes {
    // ------------------------------------------------------------------
    // programmatic schema fixture
    // ------------------------------------------------------------------

    // Programmatic schema fragments, independent of the named pack file format.
    struct ProgrammaticInputs {
        std::array<asset::DescriptorSource, 7u> m_documents;
    };

    Expected<colony::PackDefinition> resolveFixtureDefinition(
        const ProgrammaticInputs &input, config::Diagnostics &diagnostics) {
        return fixtures::resolveDefinition(input.m_documents, diagnostics);
    }

    // ------------------------------------------------------------------
    // content biomes
    // ------------------------------------------------------------------

    using namespace colony;

    const std::string materials_text = R"([materials.rock]
phase = "solid"
representation = "terrain"
diggable = true
density = 2200.0
[materials.ore]
phase = "solid"
representation = "terrain"
diggable = true
density = 3000.0
[materials.sand]
phase = "solid"
representation = "particle"
density = 1600.0
)";
    const std::string fields_text = R"([fields.composition]
algorithm = "perlin"
frequency_per_metre = 0.05
[fields.temperature]
algorithm = "open_simplex2"
frequency_per_metre = 0.01
)";
    const std::string base = R"(
[biomes.temperate]
temperature.base_k = 295.0
temperature.variation_k = 10.0
temperature.field = "temperature"
)";
    const std::string shallow = R"(
[biomes.temperate.composition.shallow]
order = 10
depth.min = 0.0
depth.max = 0.5
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
depth.min = 0.5
depth.max = 1.0
field = "composition"
[biomes.temperate.composition.deep.materials.host]
material = "rock"
weight = 1.0
)";
    const std::string deposit = R"(
[biomes.temperate.deposits.copper]
order = 5
field = "composition"
material = "ore"
host_material = "rock"
depth.min = 0.2
depth.max = 0.8
threshold.min = 0.3
threshold.max = 1.0
)";
    std::string validDocument() {
        return "" + base + shallow + deep + deposit;
    }

    void replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto position = text.find(before);
        PPR_TEST_ASSERT(position != std::string::npos);
        text.replace(position, before.size(), after);
    }

    Expected<Array<BiomeDefinition>> decodeBiomes(
        const std::string_view text, const std::string_view origin, config::Diagnostics &diagnostics) {
        ProgrammaticInputs source{};
        const char *names[] = {
            "materials.toml", "biomes.toml", "fields.toml", "features.toml", "worlds.toml", "rules.toml",
            "appearance.toml"};
        for (std::size_t i = 0u; i < 7u; ++i) {
            source.m_documents[i] = {names[i], ""};
        }
        source.m_documents[0u].m_bytes = materials_text;
        source.m_documents[1u] = {std::string{origin}, std::string{text}};
        source.m_documents[2u].m_bytes = fields_text;
        source.m_documents[6u].m_bytes = R"([appearance.materials.rock]
display_name = "Rock"
color_rgba = "AABBCCFF"
albedo_texture = "rock_texture"
texture_period_m = 2.0
[appearance.materials.ore]
display_name = "Ore"
color_rgba = "112233FF"
albedo_texture = "ore_texture"
texture_period_m = 1.0
[appearance.materials.sand]
display_name = "Sand"
color_rgba = "EECCAAFF"
[appearance.biomes.temperate]
display_name = "Temperate"
preview_color_rgba = "709040FF"
)";
        return resolveFixtureDefinition(source, diagnostics)
            .and_then(
                [&](PackDefinition definition) { return projectContentCatalog(std::move(definition), diagnostics); })
            .transform([](ContentCatalog catalog) {
                return Array<BiomeDefinition>{catalog.biomes().begin(), catalog.biomes().end()};
            });
    }

    u16 materialId(const std::string_view key) {
        config::Diagnostics diagnostics;
        const auto document = config::decode(
            materials_text.substr(materials_text.find("[materials.")) +
                fields_text.substr(fields_text.find("[fields.")) +
                validDocument().substr(validDocument().find("[biomes.")),
            "bindings.toml", diagnostics, config::EDecodeMode::tree);
        PPR_TEST_ASSERT(document.has_value());
        const auto layer = config::toTreeLayer(*document, "bindings.toml");
        const auto resolved = config::resolveRecursive<sim::definition::Definition>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        const auto compiled = sim::definition::compileDefinition(resolved->m_value, 1.0 / 3.0, diagnostics);
        PPR_TEST_ASSERT(compiled.has_value());
        const auto binding = std::ranges::find_if(
            compiled->m_bindings.m_materials, [&](const auto &item) { return item.m_key == key; });
        PPR_TEST_ASSERT(binding != compiled->m_bindings.m_materials.end());
        return binding->m_id;
    }

    PPR_UNIT_TEST (owned_sorted_composition_and_terrain_references) {
        config::Diagnostics diagnostics{};
        std::string source = validDocument();
        auto recipes = decodeBiomes(source, "biomes.toml", diagnostics);
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
        PPR_TEST_ASSERT(
            band.m_materials[0u].m_key == "aore" and band.m_materials[0u].m_material_id == materialId("ore"));
        PPR_TEST_ASSERT(
            band.m_materials[1u].m_key == "zrock" and band.m_materials[1u].m_material_id == materialId("rock"));
        PPR_TEST_ASSERT(biome.m_deposits.size() == 1u);
        PPR_TEST_ASSERT(
            biome.m_deposits[0u].m_material_id == materialId("ore") and
            biome.m_deposits[0u].m_host_material_id == materialId("rock"));

        const auto reordered = decodeBiomes("" + deposit + deep + shallow + base, "other/location.toml", diagnostics);
        PPR_TEST_ASSERT(reordered.has_value());
        PPR_TEST_ASSERT(reordered->front().m_composition[0u].m_materials[0u].m_material_id == materialId("ore"));
        PPR_TEST_ASSERT(reordered->front().m_composition[0u].m_total_weight == band.m_total_weight);
        const auto no_deposits = decodeBiomes("" + base + shallow + deep, "biomes.toml", diagnostics);
        PPR_TEST_ASSERT(no_deposits.has_value() and no_deposits->front().m_deposits.empty());
        auto zero_variation = validDocument();
        replace(zero_variation, "temperature.variation_k = 10.0", "temperature.variation_k = 0.0");
        replace(zero_variation, "threshold.min = 0.3", "threshold.min = -1.0");
        const auto endpoints = decodeBiomes(zero_variation, "biomes.toml", diagnostics);
        PPR_TEST_ASSERT(endpoints.has_value() and endpoints->front().m_temperature_variation_k == 0.0);
        auto porous = validDocument();
        replace(porous, "material = \"ore\"", "material = \"vacuum\"");
        replace(
            porous, "temperature.field = \"temperature\"",
            "temperature.field = \"temperature\"\nsprinkle.probability = 0.4\nrelaxation.passes = 21");
        const auto holes = decodeBiomes(porous, "biomes.toml", diagnostics);
        PPR_TEST_ASSERT(holes.has_value());
        PPR_TEST_ASSERT(holes->front().m_composition[0u].m_materials[0u].m_material_id == 0u);
        PPR_TEST_ASSERT(holes->front().m_sprinkle_probability == 0.4 and holes->front().m_relaxation_passes == 21u);
        for (const auto probability: {"-0.1", "1.1", "nan"}) {
            auto invalid = porous;
            replace(invalid, "sprinkle.probability = 0.4", std::string{"sprinkle.probability = "} + probability);
            PPR_TEST_ASSERT(not decodeBiomes(invalid, "biomes.toml", diagnostics));
        }
    };

    PPR_UNIT_TEST (finite_blob_controls_validate_and_preserve_canonical_defaults) {
        config::Diagnostics setup{};
        const auto defaults = decodeBiomes(validDocument(), "biomes.toml", setup);
        PPR_TEST_ASSERT(defaults.has_value());
        const auto &original = defaults->front();
        PPR_TEST_ASSERT(original.m_relaxation_passes == 0u and original.m_relaxation_adoption == 0.5);
        PPR_TEST_ASSERT(original.m_erosion_passes == 0u);
        auto erosion = validDocument();
        replace(
            erosion, "temperature.field = \"temperature\"", "temperature.field = \"temperature\"\nerosion.passes = 24");
        const auto erosion_only = decodeBiomes(erosion, "biomes.toml", setup);
        PPR_TEST_ASSERT(erosion_only and erosion_only->front().m_erosion_passes == 24u);
        for (const auto replacement: {"erosion.passes = 25", "erosion.passes = 24\nrelaxation.passes = 1"}) {
            auto invalid = erosion;
            replace(invalid, "erosion.passes = 24", replacement);
            config::Diagnostics rejected;
            PPR_TEST_ASSERT(not decodeBiomes(invalid, "biomes.toml", rejected));
        }
        PPR_TEST_ASSERT(original.m_deposits.front().m_shape == sim::EGenerationDepositShape::field);
        PPR_TEST_ASSERT(original.m_deposits.front().m_stage == sim::EGenerationDepositStage::bulk);
        PPR_TEST_ASSERT(
            original.m_deposits.front().m_orientation_min_degrees == 0.0 and
            original.m_deposits.front().m_orientation_max_degrees == 360.0);
        auto blob = validDocument();
        replace(
            blob, "temperature.field = \"temperature\"",
            "temperature.field = \"temperature\"\nrelaxation.passes = 8\nrelaxation.adoption = 0.4");
        replace(blob, "threshold.min = 0.3\nthreshold.max = 1.0", R"(threshold.min = 0.3
threshold.max = 1.0
stage = "detail"
blob.count = 4096
blob.radius.min_m = 2.0
blob.radius.max_m = 5.0
blob.stretch.min = 0.1
blob.stretch.max = 16.0
blob.distortion = 0.45
blob.orientation.min_degrees = 80.0
blob.orientation.max_degrees = 100.0
core_coverage = 0.9
fringe_coverage = 0.2
sprinkle_probability = 0.02)");
        config::Diagnostics diagnostics{};
        const auto accepted = decodeBiomes(blob, "biomes.toml", diagnostics);
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
        replace(fixed_orientation, "blob.orientation.max_degrees = 100.0", "blob.orientation.max_degrees = 80.0");
        PPR_TEST_ASSERT(decodeBiomes(fixed_orientation, "biomes.toml", diagnostics).has_value());
        PPR_TEST_ASSERT(
            item.m_core_coverage == 0.9 and item.m_fringe_coverage == 0.2 and item.m_sprinkle_probability == 0.02);
        const std::pair<std::string_view, std::string_view> failures[] = {
            {"relaxation.passes = 8", "relaxation.passes = 25"},
            {"relaxation.adoption = 0.4", "relaxation.adoption = nan"},
            {"relaxation.adoption = 0.4", "relaxation.adoption = 1.1"},
            {"stage = \"detail\"", "stage = \"bulk\""},
            {"blob.count = 4096", "blob.count = 0"},
            {"blob.count = 4096", "blob.count = 4097"},
            {"blob.radius.min_m = 2.0", "blob.radius.min_m = 0.0"},
            {"blob.radius.max_m = 5.0", "blob.radius.max_m = 1.0"},
            {"blob.radius.max_m = 5.0", "blob.radius.max_m = 257.0"},
            {"blob.radius.max_m = 5.0", "blob.radius.max_m = 100.0"},
            {"blob.stretch.min = 0.1", "blob.stretch.min = 0.09"},
            {"blob.stretch.max = 16.0", "blob.stretch.max = 17.0"},
            {"blob.distortion = 0.45", "blob.distortion = 0.46"},
            {"blob.orientation.min_degrees = 80.0", "blob.orientation.min_degrees = -0.1"},
            {"blob.orientation.max_degrees = 100.0", "blob.orientation.max_degrees = 360.1"},
            {"blob.orientation.max_degrees = 100.0", "blob.orientation.max_degrees = 79.0"},
            {"blob.orientation.max_degrees = 100.0", "blob.orientation.max_degrees = nan"},
            {"core_coverage = 0.9", "core_coverage = 0.1"},
            {"fringe_coverage = 0.2", "fringe_coverage = -0.1"},
            {"sprinkle_probability = 0.02", "sprinkle_probability = 0.021"},
        };
        for (const auto &[before, after]: failures) {
            auto invalid = blob;
            replace(invalid, before, after);
            config::Diagnostics rejected{};
            PPR_TEST_ASSERT(not decodeBiomes(invalid, "biomes.toml", rejected).has_value());
            PPR_TEST_ASSERT(rejected.hasErrors());
        }
        auto second = deposit;
        replace(second, "deposits.copper", "deposits.second");
        replace(second, "order = 5", "order = 6");
        replace(
            second, "threshold.min = 0.3\nthreshold.max = 1.0",
            "threshold.min = 0.3\nthreshold.max = 1.0\nblob.count = 1\nblob.radius.min_m = 1.0\nblob.radius.max_m = 1.0");
        config::Diagnostics aggregate{};
        PPR_TEST_ASSERT(not decodeBiomes(blob + second, "biomes.toml", aggregate).has_value());
        PPR_TEST_ASSERT(aggregate.entries().back().m_property == "biomes");
        auto mixed = validDocument();
        replace(mixed, "order = 5", "order = 5\ncore_coverage = 0.5");
        config::Diagnostics neutral{};
        PPR_TEST_ASSERT(not decodeBiomes(mixed, "biomes.toml", neutral).has_value());
        mixed = validDocument();
        replace(mixed, "order = 5", "order = 5\nblob.orientation.max_degrees = 90.0");
        PPR_TEST_ASSERT(not decodeBiomes(mixed, "biomes.toml", neutral).has_value());
    };

    PPR_UNIT_TEST (invalid_recipes_keep_qualified_diagnostics) {
        config::Diagnostics setup{};
        const std::pair<std::string_view, std::string_view> failures[] = {
            {"temperature.base_k = 295.0", ""},
            {"temperature.variation_k = 10.0", "temperature.variation_k = 300.0"},
            {"temperature.variation_k = 10.0", "temperature.variation_k = nan"},
            {"temperature.variation_k = 10.0", "temperature.variation_k = inf"},
            {"temperature.field = \"temperature\"", "temperature.field = \"absent\""},
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
            {"depth.min = 0.5", "depth.min = 0.4"},
            {"depth.min = 0.5", "depth.min = 0.6"},
            {"depth.min = 0.5", "depth.min = nan"},
            {"threshold.min = 0.3", "threshold.min = nan"},
            {"depth.max = 1.0", "depth.max = 0.9"},
            {"threshold.min = 0.3", "threshold.min = 1.0"},
            {"order = 5", "order = 5\nunknown = true"},
            {"composition.shallow]", "composition.Upper]"},
            {"materials.zrock]", "materials.Upper]"},
        };
        for (const auto &[before, after]: failures) {
            auto source = validDocument();
            replace(source, before, after);
            config::Diagnostics diagnostics{};
            const auto failed = decodeBiomes(source, "biomes.toml", diagnostics);
            PPR_TEST_ASSERT(not failed.has_value() and diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().back().m_property.starts_with("biomes.temperate."));
            PPR_TEST_ASSERT(diagnostics.entries().back().m_location.find("biomes.toml") != std::string::npos);
        }
        for (const auto &[first, second]: {std::pair{"1e308", "1e308"}, std::pair{"5e-324", "1e308"}}) {
            auto source = validDocument();
            replace(source, "weight = 1.0", std::string{"weight = "} + first);
            replace(source, "weight = 3.0", std::string{"weight = "} + second);
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not decodeBiomes(source, "biomes.toml", diagnostics).has_value());
        }
        auto duplicate = deposit;
        replace(duplicate, "deposits.copper", "deposits.second");
        config::Diagnostics duplicates{};
        PPR_TEST_ASSERT(not decodeBiomes(validDocument() + duplicate, "biomes.toml", duplicates).has_value());
    };

    PPR_UNIT_TEST (deposit_interval_errors_identify_the_offending_field) {
        config::Diagnostics setup{};
        for (const bool invalid_depth: {true, false}) {
            auto invalid_deposit = deposit;
            replace(
                invalid_deposit, invalid_depth ? "depth.max = 0.8" : "threshold.max = 1.0",
                invalid_depth ? "depth.max = 0.1" : "threshold.max = 0.1");
            const auto source = "" + base + shallow + deep + invalid_deposit;
            const std::string property = std::string{"biomes.temperate.deposits.copper."} +
                                         (invalid_depth ? "depth.max" : "threshold.max");
            config::Diagnostics codec_diagnostics{};
            const auto document = config::decode(source, "biomes.toml", codec_diagnostics, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            const auto entry = std::ranges::find(document->m_entries, property, &config::Entry::m_key);
            PPR_TEST_ASSERT(entry != document->m_entries.end());
            config::Diagnostics diagnostics{};
            const auto failed = decodeBiomes(source, "biomes.toml", diagnostics);
            PPR_TEST_ASSERT(not failed.has_value() and diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().back().m_property == property);
            PPR_TEST_ASSERT(diagnostics.entries().back().m_location == entry->m_location);
        }
    };

    PPR_UNIT_TEST (child_counts_are_bounded_before_compilation) {
        config::Diagnostics setup{};
        const std::pair<std::string_view, std::size_t> collections[] = {
            {"composition", kMaxBiomeCompositionBands}, {"deposits", kMaxBiomeDeposits},
            {"composition.shallow.materials", kMaxCompositionMaterials}};
        for (const auto &[collection, limit]: collections) {
            std::string source = "" + base;
            if (collection == "deposits") {
                source += shallow + deep;
            } else if (collection == "composition.shallow.materials") {
                source += deep + shallow.substr(0u, shallow.find("[biomes.temperate.composition.shallow.materials."));
            }
            for (std::size_t i = 0u; i <= limit; ++i) {
                if (collection == "composition") {
                    auto band = shallow;
                    replace(band, "composition.shallow", "composition.b" + std::to_string(i));
                    replace(band, "composition.shallow", "composition.b" + std::to_string(i));
                    replace(band, "composition.shallow", "composition.b" + std::to_string(i));
                    replace(band, "order = 10", "order = " + std::to_string(i));
                    replace(band, "depth.max = 0.5", "depth.max = 1.0");
                    source += band;
                } else if (collection == "deposits") {
                    auto item = deposit;
                    replace(item, "deposits.copper", "deposits.b" + std::to_string(i));
                    replace(item, "order = 5", "order = " + std::to_string(i));
                    source += item;
                } else {
                    source += "[biomes.temperate.composition.shallow.materials.b" +
                              std::to_string(i) +
                              "]\nmaterial = 'rock'\nweight = 1.0\n";
                }
            }
            config::Diagnostics diagnostics{};
            const auto failed = decodeBiomes(source, "biomes.toml", diagnostics);
            if (not failed and failed.error() != config::make_error_code(config::EConfigErrc::out_of_range)) {
                throw std::system_error(failed.error(), std::string{collection} + "\n" + diagnostics.format());
            }
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
                detail::biomes::finite_blob_controls_validate_and_preserve_canonical_defaults,
                detail::biomes::deposit_interval_errors_identify_the_offending_field,
                detail::biomes::child_counts_are_bounded_before_compilation});
    };
    const UnitTest &gameBiomeContentTests() noexcept {
        return biome_content;
    }
}
