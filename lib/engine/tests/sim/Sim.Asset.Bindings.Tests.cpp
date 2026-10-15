module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.sim;
import std;

namespace pP::tests::detail::AssetBindings {
    // ------------------------------------------------------------------
    // numeric definition fixtures
    // ------------------------------------------------------------------

    sim::definition::Definition definition() {
        using namespace sim::definition;
        Definition result;
        Material first;
        first.m_phase = sim::EGenerationMatter::solid;
        first.m_diggable = true;
        first.m_density = 2000.0;
        result.m_materials.m_entries.push_back({"alpha", first});

        first.m_density = 2400.0;
        result.m_materials.m_entries.push_back({"beta", first});

        sim::NoiseFieldDesc noise;
        noise.m_frequency_per_metre = 0.01;
        result.m_fields.m_entries.push_back({"noise_a", noise});

        noise.m_frequency_per_metre = 0.02;
        result.m_fields.m_entries.push_back({"noise_b", noise});

        Biome biome;
        biome.m_temperature = {{"noise_a"}, 300.0, 1.0};
        CompositionBand band;
        band.m_field.m_key = "noise_b";
        band.m_materials.m_entries.push_back({"stone", {{"alpha"}, 1.0}});
        biome.m_composition.m_entries.push_back({"cover", std::move(band)});
        result.m_biomes.m_entries.push_back({"active", biome});

        biome.m_erosion.m_passes = 3u;
        result.m_biomes.m_entries.push_back({"unsampled", std::move(biome)});

        FeatureDefinition feature;
        feature.m_reservation = {0, 0, 16, 16};
        Operation operation;
        operation.m_action = Paint{{"alpha"}, Rectangle{16u, 16u}};
        feature.m_operations.m_entries.push_back({"paint", std::move(operation)});
        feature.m_markers.m_entries.push_back({"spawn", {sim::EGenerationMarker::start, 8, 8}});
        result.m_features.m_entries.push_back({"room", std::move(feature)});
        return result;
    }

    sim::definition::DefinitionBindings globalBindings(const sim::definition::CompiledDefinition &compiled) {
        auto result = compiled.m_bindings;
        result.m_rules.clear();
        for (auto &feature: result.m_features) {
            feature.m_markers.clear();
        }

        return result;
    }

    // ------------------------------------------------------------------
    // explicit ordering and semantic random keys
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (explicit_palette_controls_numeric_order_without_changing_rng_keys) {
        const auto source = definition();
        config::Diagnostics diagnostics{16u};
        const auto baseline = sim::definition::compileDefinition(source, 1.0 / 3.0, diagnostics);
        PPR_TEST_ASSERT(baseline.has_value());

        auto bindings = globalBindings(*baseline);
        std::swap(bindings.m_materials[0u].m_id, bindings.m_materials[1u].m_id);
        std::swap(bindings.m_fields[0u].m_index, bindings.m_fields[1u].m_index);
        std::swap(bindings.m_biomes[0u].m_index, bindings.m_biomes[1u].m_index);

        const auto explicit_result = sim::definition::compileDefinition(source, 1.0 / 3.0, bindings, diagnostics);
        PPR_TEST_ASSERT(explicit_result.has_value());
        PPR_TEST_ASSERT(explicit_result->m_bindings.m_materials[0u].m_key == "beta");
        PPR_TEST_ASSERT(explicit_result->m_bindings.m_materials[1u].m_key == "alpha");
        PPR_TEST_ASSERT(explicit_result->m_materials[1u].m_id == 2u);
        PPR_TEST_ASSERT(explicit_result->m_materials[1u].m_key == baseline->m_materials[0u].m_key);
        PPR_TEST_ASSERT(explicit_result->m_fields[1u].m_key == baseline->m_fields[0u].m_key);
        PPR_TEST_ASSERT(explicit_result->m_biomes[1u].m_key == baseline->m_biomes[0u].m_key);
        PPR_TEST_ASSERT(explicit_result->m_biomes[1u].m_temperature_field == 1u);
        PPR_TEST_ASSERT(explicit_result->m_biomes[1u].m_bands.front().m_field == 0u);
        PPR_TEST_ASSERT(explicit_result->m_biomes[1u].m_bands.front().m_choices.front().m_material == 2u);
        PPR_TEST_ASSERT(explicit_result->m_features.front().m_operations.front().m_material == 2u);
        PPR_TEST_ASSERT(explicit_result->m_bindings.m_features.front().m_markers.front().m_index == 0u);
        PPR_TEST_ASSERT(explicit_result->m_biomes.front().m_erosion_passes == 3u);
    };

    PPR_UNIT_TEST (binding_identity_ignores_storage_order_and_tracks_numeric_semantics) {
        auto source = definition();
        config::Diagnostics diagnostics{16u};
        const auto compiled = sim::definition::compileDefinition(source, 1.0 / 3.0, diagnostics);
        PPR_TEST_ASSERT(compiled.has_value());

        auto bindings = globalBindings(*compiled);
        const auto baseline = sim::definition::definitionFingerprint(source, bindings);

        std::ranges::reverse(bindings.m_materials);
        std::ranges::reverse(bindings.m_fields);
        PPR_TEST_ASSERT(sim::definition::definitionFingerprint(source, bindings) == baseline);

        std::swap(bindings.m_materials[0u].m_id, bindings.m_materials[1u].m_id);
        PPR_TEST_ASSERT(sim::definition::definitionFingerprint(source, bindings) != baseline);

        std::swap(bindings.m_materials[0u].m_id, bindings.m_materials[1u].m_id);
        source.m_materials.m_entries.front().m_value.m_density += 1.0;
        PPR_TEST_ASSERT(sim::definition::definitionFingerprint(source, bindings) != baseline);
    };

    // ------------------------------------------------------------------
    // malformed palette rejection before numeric emission
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (bindings_require_exact_unique_membership_and_contiguous_domains) {
        const auto source = definition();
        config::Diagnostics diagnostics{32u};
        const auto compiled = sim::definition::compileDefinition(source, 1.0 / 3.0, diagnostics);
        PPR_TEST_ASSERT(compiled.has_value());

        const auto baseline = globalBindings(*compiled);

        auto invalid = baseline;
        invalid.m_materials.pop_back();
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, invalid, diagnostics));

        invalid = baseline;
        invalid.m_materials.back().m_key = "missing";
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, invalid, diagnostics));

        invalid = baseline;
        invalid.m_materials.back().m_key = invalid.m_materials.front().m_key;
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, invalid, diagnostics));

        invalid = baseline;
        invalid.m_materials.back().m_id = invalid.m_materials.front().m_id;
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, invalid, diagnostics));

        invalid = baseline;
        invalid.m_materials.front().m_id = 0u;
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, invalid, diagnostics));

        invalid = baseline;
        invalid.m_materials.front().m_id = 65535u;
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, invalid, diagnostics));

        invalid = baseline;
        invalid.m_fields.front().m_index = 2u;
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, invalid, diagnostics));

        invalid = baseline;
        invalid.m_biomes.clear();
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, invalid, diagnostics));
    };

    PPR_UNIT_TEST (rule_and_marker_bindings_remain_compiler_owned) {
        const auto source = definition();
        config::Diagnostics diagnostics{16u};
        const auto compiled = sim::definition::compileDefinition(source, 1.0 / 3.0, diagnostics);
        PPR_TEST_ASSERT(compiled.has_value());

        auto bindings = globalBindings(*compiled);
        bindings.m_rules.push_back({"cave", 0u, "frontier"});
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, bindings, diagnostics));

        bindings = globalBindings(*compiled);
        bindings.m_features.front().m_markers.push_back({"spawn", 0u, "room"});
        PPR_TEST_ASSERT(not sim::definition::compileDefinition(source, 1.0 / 3.0, bindings, diagnostics));
        PPR_TEST_ASSERT(diagnostics.entries().back().m_property.ends_with(".markers"));
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // explicit binding suite registration
    // ------------------------------------------------------------------

    const UnitTest asset_bindings = UnitTest::Named("asset_bindings") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::AssetBindings::explicit_palette_controls_numeric_order_without_changing_rng_keys,
            detail::AssetBindings::binding_identity_ignores_storage_order_and_tracks_numeric_semantics,
            detail::AssetBindings::bindings_require_exact_unique_membership_and_contiguous_domains,
            detail::AssetBindings::rule_and_marker_bindings_remain_compiler_owned,
        });
    };

    const UnitTest &assetBindingTests() noexcept {
        return asset_bindings;
    }
}
