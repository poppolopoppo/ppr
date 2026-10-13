module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import game.colony.content;
import std;

namespace pP::game::tests::detail::worlds {
    using namespace colony;
    constexpr std::string_view materials_text = R"(schema_version = 2
[materials.rock]
numeric_id = 501
display_name = "Rock"
phase = "solid"
representation = "terrain"
color_rgba = "778899FF"
albedo_texture = "textures/rock.png"
texture_period_m = 1.0
diggable = true
reference_density_kg_m3 = 2500.0
[materials.wall]
numeric_id = 777
display_name = "Wall"
phase = "solid"
representation = "terrain"
color_rgba = "778899FF"
albedo_texture = "textures/wall.png"
texture_period_m = 1.0
diggable = false
reference_density_kg_m3 = 2500.0
[materials.water]
numeric_id = 901
display_name = "Water"
phase = "liquid"
representation = "particle"
color_rgba = "0000FFFF"
reference_density_kg_m3 = 1000.0
[materials.air]
numeric_id = 902
display_name = "Air"
phase = "gas"
representation = "particle"
color_rgba = "AACCCCFF"
reference_density_kg_m3 = 1.2
)";
    struct Inputs {
        ContentCatalog m_materials;
        Array<FieldDefinition> m_fields{};
        Array<BiomeDefinition> m_biomes{};
        Array<FeatureDefinition> m_features{};
    };
    Inputs inputs() {
        config::Diagnostics diagnostics{};
        auto materials = decodeMaterialCatalog(materials_text, "materials.toml", diagnostics);
        PPR_TEST_ASSERT(materials.has_value());
        // World decoding consumes previously validated, owned definitions. This
        // fixture keeps their reference/attachment data independent of TOML tests.
        Inputs result{std::move(*materials)};
        result.m_fields.push_back({"noise", {}});
        result.m_fields.back().m_noise.m_frequency_per_metre = 0.01;
        BiomeDefinition biome{};
        biome.m_key = "temperate";
        result.m_biomes.push_back(std::move(biome));
        FeatureDefinition start{};
        start.m_key = "landing";
        start.m_reservation = {0, 0, 90, 30};
        start.m_markers.push_back({"spawn", EFeatureMarkerRole::start, 45, 5});
        start.m_markers.push_back({"left", EFeatureMarkerRole::resource_anchor, 10, 5});
        start.m_markers.push_back({"right", EFeatureMarkerRole::resource_anchor, 80, 5});
        result.m_features.push_back(std::move(start));
        FeatureDefinition pocket{};
        pocket.m_key = "pocket";
        pocket.m_allow_horizontal_reflection = true;
        pocket.m_reservation = {0, 0, 30, 30};
        pocket.m_markers.push_back({"fill", EFeatureMarkerRole::sealed_pocket, 15, 15});
        result.m_features.push_back(std::move(pocket));
        return result;
    }
    const std::string base = R"([worlds.default]
display_name = "Default"
background_stack = "default"
default_wall_material = "wall"
boundary_fine = 3
min_sites = 4
max_sites = 32
site_attempts = 30
site_spacing_m = 100.0
boundary_warp_amplitude_m = 10.0
boundary_warp_x_field = "noise"
boundary_warp_y_field = "noise"
start_biome = "temperate"
start_feature = "landing"
start_marker = "spawn"
start_x_fine = 500
start_y_fine = 300
start_clearance_m = 3.0
start_route_width_m = 1.0
)";
    const std::string biome = R"([worlds.default.biomes.temperate]
biome = "temperate"
weight = 1.0
required_count = 1
depth_min_normalized = 0.0
depth_max_normalized = 1.0
distance_min_m = 0.0
distance_max_m = 2048.0
)";
    const std::string resources = R"([worlds.default.start_resources.left]
marker = "left"
material = "rock"
minimum_fine_cells = 9
max_distance_m = 20.0
[worlds.default.start_resources.right]
marker = "right"
material = "rock"
minimum_fine_cells = 9
max_distance_m = 20.0
)";
    const std::string caves = R"([worlds.default.rules.caves]
kind = "threshold_cave"
phase = "carve"
order = 20
biome = "temperate"
field = "noise"
depth_min_normalized = 0.0
depth_max_normalized = 1.0
threshold_min = 0.4
threshold_max = 1.0
)";
    const std::string carve = R"([worlds.default.rules.carve]
kind = "carve_worm"
phase = "carve"
order = 30
biome = "temperate"
count = 10
depth_min_normalized = 0.0
depth_max_normalized = 1.0
length_min_m = 20.0
length_max_m = 60.0
radius_min_m = 1.0
radius_max_m = 3.0
turn_probability = 0.2
)";
    const std::string vein = R"([worlds.default.rules.vein]
kind = "vein_worm"
phase = "deposit"
order = 10
biome = "temperate"
count = 3
depth_min_normalized = 0.0
depth_max_normalized = 1.0
length_min_m = 10.0
length_max_m = 30.0
radius_min_m = 1.0
radius_max_m = 2.0
turn_probability = 0.1
material = "rock"
host_material = "wall"
)";
    const std::string connection = R"([worlds.default.rules.rooms]
kind = "connect_rooms"
phase = "carve"
order = 40
radius_m = 1.0
extra_links = 2
)";
    const std::string feature = R"([worlds.default.rules.pockets]
kind = "feature"
phase = "stamp"
order = 10
feature = "pocket"
biome = "temperate"
depth_min_normalized = 0.0
depth_max_normalized = 1.0
spacing_m = 30.0
min_count = 1
max_count = 4
attempts = 30
priority = 100
reflect = true
)";
    const std::string fill = R"([worlds.default.rules.water]
kind = "matter_fill"
phase = "fill"
order = 10
feature_rule = "pockets"
marker = "fill"
material = "water"
temperature_k = 290.0
bulk_density_kg_m3 = 1000.0
fill_height_m = 4.0
)";
    std::string validDocument() {
        return "schema_version = 2\n" + fill + feature + connection + vein + carve + caves + resources + biome + base;
    }
    void replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto position = text.find(before);
        PPR_TEST_ASSERT(position != std::string::npos);
        text.replace(position, before.size(), after);
    }
    Expected<Array<WorldProfileDefinition>> decode(
        const std::string_view source, const Inputs &input, config::Diagnostics &diagnostics) {
        return decodeWorldProfiles(
            source, "worlds.toml", input.m_materials, input.m_fields, input.m_biomes, input.m_features, diagnostics);
    }

    PPR_UNIT_TEST (owned_profiles_resolve_all_rule_kinds_and_canonical_order) {
        const auto input = inputs();
        config::Diagnostics diagnostics{};
        auto source = validDocument();
        const auto profiles = decode(source, input, diagnostics);
        PPR_TEST_ASSERT(profiles.has_value() and profiles->size() == 1u);
        source.clear();
        const auto &profile = profiles->front();
        PPR_TEST_ASSERT(profile.m_key == "default" and profile.m_default_wall_material_id == 777u);
        PPR_TEST_ASSERT(profile.m_start_resources.size() == 2u and profile.m_biomes.size() == 1u);
        PPR_TEST_ASSERT(profile.m_start_resources[0u].m_material_id == 501u);
        PPR_TEST_ASSERT(profile.m_rules.size() == 6u);
        PPR_TEST_ASSERT(profile.m_rules[0u].m_kind == EWorldRuleKind::vein_worm);
        PPR_TEST_ASSERT(std::get<WorldWormRule>(profile.m_rules[0u].m_parameters).m_host_material_id == 777u);
        PPR_TEST_ASSERT(profile.m_rules[1u].m_kind == EWorldRuleKind::threshold_cave);
        PPR_TEST_ASSERT(profile.m_rules[2u].m_kind == EWorldRuleKind::carve_worm);
        PPR_TEST_ASSERT(std::get<WorldWormRule>(profile.m_rules[2u].m_parameters).m_material_id == 0u);
        PPR_TEST_ASSERT(profile.m_rules[3u].m_kind == EWorldRuleKind::connect_rooms);
        PPR_TEST_ASSERT(profile.m_rules[4u].m_kind == EWorldRuleKind::feature);
        PPR_TEST_ASSERT(std::get<WorldFeatureRule>(profile.m_rules[4u].m_parameters).m_min_count == 1u);
        PPR_TEST_ASSERT(profile.m_rules[5u].m_kind == EWorldRuleKind::matter_fill);
        PPR_TEST_ASSERT(std::get<WorldMatterRule>(profile.m_rules[5u].m_parameters).m_material_id == 901u);
        const auto reordered = decode(
            "schema_version = 2\n" + base + biome + resources + caves + carve + vein + connection + feature + fill,
            input, diagnostics);
        PPR_TEST_ASSERT(reordered.has_value());
        for (std::size_t i = 0u; i < profile.m_rules.size(); ++i) {
            PPR_TEST_ASSERT(reordered->front().m_rules[i].m_key == profile.m_rules[i].m_key);
        }
        auto gas = fill;
        replace(gas, "material = \"water\"", "material = \"air\"");
        replace(gas, "fill_height_m = 4.0", "");
        const auto gas_profile = decode(
            "schema_version = 2\n" + base + biome + resources + feature + gas, input, diagnostics);
        PPR_TEST_ASSERT(gas_profile.has_value());
        PPR_TEST_ASSERT(not std::get<WorldMatterRule>(gas_profile->front().m_rules.back().m_parameters)
                .m_fill_height_m.has_value());
    };

    PPR_UNIT_TEST (smooth_worm_controls_are_opt_in_and_bounded) {
        const auto input = inputs();
        config::Diagnostics diagnostics{};
        const auto legacy = decode(validDocument(), input, diagnostics);
        PPR_TEST_ASSERT(legacy.has_value());
        const auto &old_worm = std::get<WorldWormRule>(legacy->front().m_rules[2u].m_parameters);
        PPR_TEST_ASSERT(old_worm.m_turn_smoothing == 0.0 and old_worm.m_radius_variation == 0.0);
        PPR_TEST_ASSERT(old_worm.m_chamber_probability == 0.0 and old_worm.m_chamber_scale == 1.0);
        auto smooth = validDocument();
        replace(smooth, "turn_probability = 0.2", R"(turn_probability = 0.2
turn_smoothing = 0.8
radius_variation = 0.4
chamber_probability = 0.1
chamber_scale = 4.0)");
        const auto accepted = decode(smooth, input, diagnostics);
        PPR_TEST_ASSERT(accepted.has_value());
        const auto &worm = std::get<WorldWormRule>(accepted->front().m_rules[2u].m_parameters);
        PPR_TEST_ASSERT(worm.m_turn_smoothing == 0.8 and worm.m_radius_variation == 0.4);
        PPR_TEST_ASSERT(worm.m_chamber_probability == 0.1 and worm.m_chamber_scale == 4.0);
        const std::pair<std::string_view, std::string_view> failures[] = {
            {"turn_smoothing = 0.8", "turn_smoothing = 0.0"},
            {"turn_smoothing = 0.8", "turn_smoothing = 1.1"},
            {"turn_smoothing = 0.8", "turn_smoothing = nan"},
            {"radius_variation = 0.4", "radius_variation = -0.1"},
            {"radius_variation = 0.4", "radius_variation = 1.1"},
            {"chamber_probability = 0.1", "chamber_probability = 0.11"},
            {"chamber_scale = 4.0", "chamber_scale = 4.1"},
            {"chamber_scale = 4.0", "chamber_scale = 0.9"},
        };
        for (const auto &[before, after]: failures) {
            auto invalid = smooth;
            replace(invalid, before, after);
            config::Diagnostics rejected{};
            PPR_TEST_ASSERT(not decode(invalid, input, rejected).has_value() and rejected.hasErrors());
        }
    };

    PPR_UNIT_TEST (invalid_world_values_references_and_phases_are_rejected) {
        const auto input = inputs();
        const std::pair<std::string_view, std::string_view> failures[] = {
            {"display_name = \"Default\"", ""},
            {"background_stack = \"default\"", ""},
            {"max_sites = 32", "max_sites = 3"},
            {"site_attempts = 30", "site_attempts = 65"},
            {"site_spacing_m = 100.0", "site_spacing_m = nan"},
            {"boundary_warp_amplitude_m = 10.0", "boundary_warp_amplitude_m = inf"},
            {"boundary_warp_x_field = \"noise\"", "boundary_warp_x_field = \"absent\""},
            {"default_wall_material = \"wall\"", "default_wall_material = \"water\""},
            {"start_biome = \"temperate\"", "start_biome = \"absent\""},
            {"start_feature = \"landing\"", "start_feature = \"absent\""},
            {"start_marker = \"spawn\"", "start_marker = \"left\""},
            {"start_y_fine = 300", "start_y_fine = 4090"},
            {"start_clearance_m = 3.0", "start_clearance_m = 2.9"},
            {"start_clearance_m = 3.0", "start_clearance_m = 11.0"},
            {"start_route_width_m = 1.0", "start_route_width_m = 31.0"},
            {"required_count = 1", "required_count = 33"},
            {"weight = 1.0", "weight = nan"},
            {"weight = 1.0", "weight = 0.0"},
            {"distance_max_m = 2048.0", "distance_max_m = -1.0"},
            {"marker = \"left\"", "marker = \"right\""},
            {"marker = \"left\"\nmaterial = \"rock\"", "marker = \"left\"\nmaterial = \"wall\""},
            {"minimum_fine_cells = 9", "minimum_fine_cells = 0"},
            {"max_distance_m = 20.0", "max_distance_m = 1.0"},
            {"kind = \"threshold_cave\"", "kind = \"unknown\""},
            {"kind = \"threshold_cave\"", ""},
            {"phase = \"deposit\"", ""},
            {"order = 20", ""},
            {"biome = \"temperate\"", "biome = \"absent\""},
            {"phase = \"deposit\"", "phase = \"carve\""},
            {"phase = \"stamp\"", "phase = \"fill\""},
            {"phase = \"fill\"", "phase = \"stamp\""},
            {"field = \"noise\"", "field = \"absent\""},
            {"threshold_max = 1.0", "threshold_max = 0.3"},
            {"order = 30", "order = 20"},
            {"count = 10", "count = 0"},
            {"length_min_m = 20.0", "length_min_m = 61.0"},
            {"radius_min_m = 1.0", "radius_min_m = 4.0"},
            {"turn_probability = 0.2", "turn_probability = nan"},
            {"host_material = \"wall\"", "host_material = \"water\""},
            {"kind = \"carve_worm\"", "kind = \"carve_worm\"\nmaterial = \"rock\""},
            {"extra_links = 2", "extra_links = 65"},
            {"feature = \"pocket\"", "feature = \"absent\""},
            {"attempts = 30", "attempts = 4097"},
            {"max_count = 4", "max_count = 0"},
            {"min_count = 1", "min_count = 5"},
            {"reflect = true", ""},
            {"feature_rule = \"pockets\"", "feature_rule = \"carve\""},
            {"marker = \"fill\"", "marker = \"absent\""},
            {"material = \"water\"", "material = \"rock\""},
            {"material = \"water\"", "material = \"air\""},
            {"temperature_k = 290.0", "temperature_k = inf"},
            {"bulk_density_kg_m3 = 1000.0", "bulk_density_kg_m3 = 0.0"},
            {"fill_height_m = 4.0", ""},
            {"fill_height_m = 4.0", "fill_height_m = 11.0"},
            {"fill_height_m = 4.0", "fill_height_m = 0.0"},
            {"fill_height_m = 4.0", "fill_height_m = nan"},
            {"fill_height_m = 4.0", "fill_height_m = 4.0\nunknown = true"},
        };
        for (const auto &[before, after]: failures) {
            auto source = validDocument();
            replace(source, before, after);
            config::Diagnostics diagnostics{};
            const auto failed = decode(source, input, diagnostics);
            PPR_TEST_ASSERT(not failed.has_value() and diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().back().m_property.starts_with("worlds.default."));
            PPR_TEST_ASSERT(diagnostics.entries().back().m_location.find("worlds.toml") != std::string::npos);
        }
        auto reflection_input = inputs();
        reflection_input.m_features[1u].m_allow_horizontal_reflection = false;
        config::Diagnostics reflection{};
        PPR_TEST_ASSERT(not decode(validDocument(), reflection_input, reflection).has_value());
    };

    PPR_UNIT_TEST (world_diagnostics_preserve_exact_fields_and_locations) {
        const auto input = inputs();
        const std::tuple<std::string_view, std::string_view, std::string_view> failures[] = {
            {"start_y_fine = 300", "start_y_fine = 4090", "start_y_fine"},
            {"length_min_m = 20.0", "length_min_m = 61.0", "rules.carve.length_max_m"},
            {"phase = \"deposit\"", "phase = \"carve\"", "rules.vein.phase"},
            {"marker = \"fill\"", "marker = \"absent\"", "rules.water.marker"},
            {"fill_height_m = 4.0", "fill_height_m = 11.0", "rules.water.fill_height_m"},
        };
        for (const auto &[before, after, suffix]: failures) {
            auto source = validDocument();
            replace(source, before, after);
            const std::string property = "worlds.default." + std::string{suffix};
            config::Diagnostics codec_diagnostics{};
            const auto document = config::decode(
                source, "worlds.toml", codec_diagnostics, config::EDecodeMode::records);
            PPR_TEST_ASSERT(document.has_value());
            const auto entry = std::ranges::find(document->m_entries, property, &config::Entry::m_key);
            PPR_TEST_ASSERT(entry != document->m_entries.end());
            config::Diagnostics diagnostics{};
            const auto failed = decode(source, input, diagnostics);
            PPR_TEST_ASSERT(not failed.has_value());
            PPR_TEST_ASSERT(diagnostics.entries().back().m_property == property);
            PPR_TEST_ASSERT(diagnostics.entries().back().m_location == entry->m_location);
        }
    };

    PPR_UNIT_TEST (fill_heights_require_exact_signed_fine_cell_boundaries) {
        auto input = inputs();
        input.m_features[1u].m_reservation.m_min_y_fine = -30;
        const auto authored_number = [](const double value) {
            char buffer[64]{};
            const auto [end, error] = std::to_chars(
                buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                std::numeric_limits<double>::max_digits10);
            PPR_TEST_ASSERT(error == std::errc{});
            return std::string{buffer, end};
        };
        for (const double height: {-29.0 / 3.0, -1.0 / 3.0, 0.0, 1.0 / 3.0, 4.0, 10.0}) {
            auto source = validDocument();
            replace(source, "fill_height_m = 4.0", "fill_height_m = " + authored_number(height));
            config::Diagnostics diagnostics{};
            const auto valid = decode(source, input, diagnostics);
            PPR_TEST_ASSERT(valid.has_value());
            PPR_TEST_ASSERT(
                *std::get<WorldMatterRule>(valid->front().m_rules.back().m_parameters).m_fill_height_m == height);
            // Subnormal decimal parsing can fail in the TOML codec before a
            // property exists. Normal neighbours exercise typed lattice errors.
            if (height == 0.0) {
                continue;
            }
            for (const double direction:
                {-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()}) {
                auto adjacent = validDocument();
                replace(
                    adjacent, "fill_height_m = 4.0",
                    "fill_height_m = " + authored_number(std::nextafter(height, direction)));
                config::Diagnostics invalid{};
                PPR_TEST_ASSERT(not decode(adjacent, input, invalid).has_value());
                PPR_TEST_ASSERT(invalid.entries().back().m_property == "worlds.default.rules.water.fill_height_m");
            }
        }
        auto off_lattice = validDocument();
        replace(off_lattice, "fill_height_m = 4.0", "fill_height_m = 0.1");
        config::Diagnostics diagnostics{};
        PPR_TEST_ASSERT(not decode(off_lattice, input, diagnostics).has_value());
    };

    PPR_UNIT_TEST (world_counts_and_weight_intervals_are_bounded) {
        const auto input = inputs();
        const std::pair<std::string_view, std::size_t> collections[] = {
            {"rules", kMaxWorldRules}, {"biomes", kMaxContentBiomes}, {"start_resources", kMaxStartResources}};
        for (const auto &[collection, cap]: collections) {
            std::string source = "schema_version = 2\n";
            for (std::size_t i = 0u; i <= cap; ++i) {
                source += "[worlds.default." + std::string{collection} + ".item" + std::to_string(i) + "]\norder = 1\n";
            }
            config::Diagnostics diagnostics{};
            const auto failed = decode(source, input, diagnostics);
            PPR_TEST_ASSERT(
                not failed.has_value() and
                failed.error() == config::make_error_code(config::EConfigErrc::out_of_range));
        }
        for (const auto &[first, second]: {std::pair{"1e308", "1e308"}, std::pair{"5e-324", "1e308"}}) {
            auto a = biome, z = biome;
            replace(a, "biomes.temperate", "biomes.a");
            replace(z, "biomes.temperate", "biomes.z");
            replace(a, "weight = 1.0", std::string{"weight = "} + first);
            replace(z, "weight = 1.0", std::string{"weight = "} + second);
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(
                not decode("schema_version = 2\n" + base + resources + caves + a + z, input, diagnostics).has_value());
        }
        auto duplicate_connection = connection;
        replace(duplicate_connection, "rules.rooms", "rules.second");
        replace(duplicate_connection, "order = 40", "order = 41");
        config::Diagnostics connections{};
        PPR_TEST_ASSERT(not decode(validDocument() + duplicate_connection, input, connections).has_value());
        auto too_many_features = feature;
        replace(too_many_features, "rules.pockets", "rules.more");
        replace(too_many_features, "order = 10", "order = 11");
        replace(too_many_features, "max_count = 4", "max_count = 1024");
        config::Diagnostics features{};
        PPR_TEST_ASSERT(not decode(validDocument() + too_many_features, input, features).has_value());
        std::string extra_worms = validDocument();
        for (u32 i = 0u; i < 4u; ++i) {
            auto worm = carve;
            replace(worm, "rules.carve", "rules.more" + std::to_string(i));
            replace(worm, "order = 30", "order = " + std::to_string(50u + i));
            replace(worm, "count = 10", "count = 1024");
            extra_worms += worm;
        }
        config::Diagnostics worms{};
        const auto excessive_worms = decode(extra_worms, input, worms);
        PPR_TEST_ASSERT(
            not excessive_worms.has_value() and
            excessive_worms.error() == config::make_error_code(config::EConfigErrc::out_of_range));
        std::string excessive_profiles = "schema_version = 2\n";
        for (std::size_t i = 0u; i <= kMaxContentWorlds; ++i) {
            excessive_profiles += "[worlds.item" + std::to_string(i) + "]\ndisplay_name = \"Item\"\n";
        }
        config::Diagnostics profiles{};
        const auto too_many = decode(excessive_profiles, input, profiles);
        PPR_TEST_ASSERT(
            not too_many.has_value() and
            too_many.error() == config::make_error_code(config::EConfigErrc::out_of_range));
    };
}

namespace pP::game::tests {
    const UnitTest world_content = UnitTest::Named("world_content") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::worlds::owned_profiles_resolve_all_rule_kinds_and_canonical_order,
                detail::worlds::invalid_world_values_references_and_phases_are_rejected,
                detail::worlds::smooth_worm_controls_are_opt_in_and_bounded,
                detail::worlds::world_diagnostics_preserve_exact_fields_and_locations,
                detail::worlds::world_counts_and_weight_intervals_are_bounded,
                detail::worlds::fill_heights_require_exact_signed_fine_cell_boundaries});
    };
    const UnitTest &gameWorldContentTests() noexcept {
        return world_content;
    }
}
