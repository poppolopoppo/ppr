module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import engine.sim;
import game.colony.content;
import std;

namespace pP::game::tests::detail::worlds {
    // ------------------------------------------------------------------
    // content worlds
    // ------------------------------------------------------------------

    using namespace colony;
    constexpr std::string_view materials_text = R"(schema_version = 3
[materials.rock]
phase = "solid"
representation = "terrain"
diggable = true
density = 2500.0
[materials.wall]
phase = "solid"
representation = "terrain"
diggable = false
density = 2500.0
[materials.water]
phase = "liquid"
representation = "particle"
density = 1000.0
[materials.air]
phase = "gas"
representation = "particle"
density = 1.2
)";
    ContentPackSource inputs() {
        ContentPackSource source{};
        const char *names[] = {
            "materials.toml", "biomes.toml", "fields.toml", "features.toml", "worlds.toml", "rules.toml",
            "appearance.toml"};
        for (std::size_t i = 0u; i < kContentDocumentCount; ++i) {
            source.m_documents[i] = {names[i], "schema_version = 3\n"};
        }

        source.m_documents[0u].m_bytes = std::string{materials_text};
        source.m_documents[1u].m_bytes += R"([biomes.temperate]
temperature.field = "noise"
temperature.base_k = 295.0
[biomes.temperate.composition.base]
order = 0
field = "noise"
depth.min = 0.0
[biomes.temperate.composition.base.materials.rock]
material = "rock"
weight = 1.0
)";
        source.m_documents[2u].m_bytes += "[fields.noise]\nalgorithm = 'perlin'\nfrequency_per_metre = 0.01\n";
        source.m_documents[3u].m_bytes += R"([features.landing]
allow_horizontal_reflection = false
reservation.min_x_fine = 0
reservation.min_y_fine = 0
reservation.max_x_fine = 90
reservation.max_y_fine = 30
[features.landing.operations.clear]
order = 0
x_fine = 0
y_fine = 0
action.tag = "carve"
action.carve.shape.tag = "rectangle"
action.carve.shape.rectangle.width_fine = 90
action.carve.shape.rectangle.height_fine = 30
[features.landing.markers.spawn]
role = "start"
x_fine = 45
y_fine = 5
[features.landing.markers.left]
role = "resource_anchor"
x_fine = 10
y_fine = 5
[features.landing.markers.right]
role = "resource_anchor"
x_fine = 80
y_fine = 5
[features.pocket]
allow_horizontal_reflection = true
reservation.min_x_fine = 0
reservation.min_y_fine = 0
reservation.max_x_fine = 30
reservation.max_y_fine = 30
[features.pocket.operations.clear]
order = 0
x_fine = 0
y_fine = 0
action.tag = "carve"
action.carve.shape.tag = "rectangle"
action.carve.shape.rectangle.width_fine = 30
action.carve.shape.rectangle.height_fine = 30
[features.pocket.markers.fill]
role = "sealed_pocket"
x_fine = 15
y_fine = 15
)";
        source.m_documents[6u].m_bytes += R"([appearance.materials.rock]
display_name = "Rock"
color_rgba = "778899FF"
albedo_texture = "textures/rock.png"
texture_period_m = 1.0
[appearance.materials.wall]
display_name = "Wall"
color_rgba = "778899FF"
albedo_texture = "textures/wall.png"
texture_period_m = 1.0
[appearance.materials.water]
display_name = "Water"
color_rgba = "0000FFFF"
[appearance.materials.air]
display_name = "Air"
color_rgba = "AACCCCFF"
[appearance.biomes.temperate]
display_name = "Temperate"
preview_color_rgba = "709040FF"
[appearance.features.landing]
display_name = "Landing"
[appearance.features.pocket]
display_name = "Pocket"
[appearance.worlds.default]
display_name = "Default"
background_stack = "default"
)";
        for (u32 i = 0u; i < 4u; ++i) {
            source.m_documents[6u].m_bytes +=
                "[appearance.backgrounds.default.layers.layer_" +
                std::to_string(i) +
                "]\norder = " +
                std::to_string(i) +
                "\ntexture = 'textures/background.png'\ncolor_rgba = 'FFFFFFFF'\nrepeat_period_m = 256.0\nparallax_factor = 0.22\n";
        }

        return source;
    }
    const std::string base = R"([worlds.default]
boundaries.material = "wall"
boundaries.thickness_fine = 3
layout.min_sites = 4
layout.max_sites = 32
layout.site_attempts = 30
layout.site_spacing_m = 100.0
boundaries.warp_amplitude_m = 10.0
boundaries.warp_x_field = "noise"
boundaries.warp_y_field = "noise"
start.biome = "temperate"
start.feature = "landing"
start.marker = "spawn"
start.x_fine = 500
start.y_fine = 300
start.clearance_m = 3.0
start.route_width_m = 1.0
)";
    const std::string biome = R"([worlds.default.biomes.temperate]
biome = "temperate"
weight = 1.0
required_count = 1
depth.min = 0.0
depth.max = 1.0
distance_min_m = 0.0
distance_max_m = 2048.0
)";
    const std::string resources = R"([worlds.default.resources.left]
marker = "left"
material = "rock"
minimum_fine_cells = 9
max_distance_m = 20.0
[worlds.default.resources.right]
marker = "right"
material = "rock"
minimum_fine_cells = 9
max_distance_m = 20.0
)";
    const std::string caves = R"([rules.default.caves]
order = 20
payload.tag = "cave"
payload.cave.biome = "temperate"
payload.cave.field = "noise"
payload.cave.depth.min = 0.0
payload.cave.depth.max = 1.0
payload.cave.threshold.min = 0.4
payload.cave.threshold.max = 1.0
)";
    const std::string carve = R"([rules.default.carve]
order = 30
payload.tag = "carve_worm"
payload.carve_worm.geometry.biome = "temperate"
payload.carve_worm.geometry.count = 10
payload.carve_worm.geometry.depth.min = 0.0
payload.carve_worm.geometry.depth.max = 1.0
payload.carve_worm.geometry.length_min_m = 20.0
payload.carve_worm.geometry.length_max_m = 60.0
payload.carve_worm.geometry.radius_min_m = 1.0
payload.carve_worm.geometry.radius_max_m = 3.0
payload.carve_worm.geometry.turn_probability = 0.2
)";
    const std::string vein = R"([rules.default.vein]
order = 10
payload.tag = "vein_worm"
payload.vein_worm.geometry.biome = "temperate"
payload.vein_worm.geometry.count = 3
payload.vein_worm.geometry.depth.min = 0.0
payload.vein_worm.geometry.depth.max = 1.0
payload.vein_worm.geometry.length_min_m = 10.0
payload.vein_worm.geometry.length_max_m = 30.0
payload.vein_worm.geometry.radius_min_m = 1.0
payload.vein_worm.geometry.radius_max_m = 2.0
payload.vein_worm.geometry.turn_probability = 0.1
payload.vein_worm.material = "rock"
payload.vein_worm.host_material = "wall"
)";
    const std::string connection = R"([rules.default.rooms]
order = 40
payload.tag = "connection"
payload.connection.radius_m = 1.0
payload.connection.extra_links = 2
)";
    const std::string feature = R"([rules.default.pockets]
order = 10
payload.tag = "feature"
payload.feature.feature = "pocket"
payload.feature.biome = "temperate"
payload.feature.depth.min = 0.0
payload.feature.depth.max = 1.0
payload.feature.spacing_m = 30.0
payload.feature.min_count = 1
payload.feature.max_count = 4
payload.feature.attempts = 30
payload.feature.priority = 100
payload.feature.reflect = true
)";
    const std::string fill = R"([rules.default.water]
order = 10
payload.tag = "matter_fill"
payload.matter_fill.placement = "pockets"
payload.matter_fill.marker = "fill"
payload.matter_fill.material = "water"
payload.matter_fill.temperature_k = 290.0
payload.matter_fill.bulk_density_kg_m3 = 1000.0
payload.matter_fill.fill_height_m = 4.0
)";
    std::string validDocument() {
        return "schema_version = 3\n" + fill + feature + connection + vein + carve + caves + resources + biome + base;
    }
    void replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto position = text.find(before);
        PPR_TEST_ASSERT(position != std::string::npos);
        text.replace(position, before.size(), after);
    }
    ContentPackSource bindWorldSource(const std::string_view text, const ContentPackSource &input) {
        auto source = input;
        auto &worlds = source.m_documents[4u].m_bytes;
        auto &rules = source.m_documents[5u].m_bytes;
        worlds.assign(text.size(), ' ');
        rules.assign(text.size(), ' ');
        for (std::size_t i = 0u; i < text.size(); ++i) {
            if (text[i] == '\n') {
                worlds[i] = '\n';
                rules[i] = '\n';
            }
        }

        // Preserve the fixture's authored line numbers in both source documents.
        const auto first = text.find('[');
        const auto header = text.substr(0u, first);
        worlds.replace(0u, header.size(), header);
        rules.replace(0u, header.size(), header);
        for (auto at = first; at != std::string_view::npos;) {
            const auto next = text.find("\n[", at + 1u);
            const auto count = next == std::string_view::npos ? text.size() - at : next + 1u - at;
            auto &target = text.substr(at).starts_with("[rules.") ? rules : worlds;
            target.replace(at, count, text.substr(at, count));
            at = next == std::string_view::npos ? next : next + 1u;
        }
        return source;
    }

    Expected<Array<WorldProfileDefinition>> decode(
        const std::string_view text, const ContentPackSource &input, config::Diagnostics &diagnostics) {
        auto source = bindWorldSource(text, input);
        return decodePackDefinition(source, diagnostics)
            .and_then([&](PackDefinition definition) {
                std::array<config::Document, kContentDocumentCount> documents;
                for (std::size_t i = 0u; i < documents.size(); ++i) {
                    const auto document = config::decode(
                        source.m_documents[i].m_bytes, source.m_documents[i].m_path.string(), diagnostics,
                        config::EDecodeMode::tree);
                    PPR_TEST_ASSERT(document.has_value());
                    documents[i] = *document;
                }
                const auto merged = mergeContentDocuments(documents, diagnostics);
                PPR_TEST_ASSERT(merged.has_value());
                return projectContentCatalog(std::move(definition), *merged, diagnostics);
            })
            .transform([](ContentCatalog catalog) {
                return Array<WorldProfileDefinition>{catalog.worlds().begin(), catalog.worlds().end()};
            });
    }

    Expected<PackDefinition> decodeDefinition(
        const std::string_view text, const ContentPackSource &input, config::Diagnostics &diagnostics) {
        auto source = bindWorldSource(text, input);
        return decodePackDefinition(source, diagnostics);
    }

    const sim::definition::RuleDefinition &ruleValue(const PackDefinition &definition, const std::string_view key) {
        const auto &rules = definition.m_worlds.m_entries.front().m_value.m_rules.m_entries;
        const auto rule = std::ranges::find_if(rules, [&](const auto &entry) { return entry.m_key == key; });
        PPR_TEST_ASSERT(rule != rules.end());
        return rule->m_value;
    }

    u16 materialId(const std::string_view key) {
        auto source = bindWorldSource(validDocument(), inputs());
        config::Diagnostics diagnostics;
        const auto definition = decodePackDefinition(source, diagnostics);
        PPR_TEST_ASSERT(definition.has_value());
        const auto compiled = sim::definition::compileDefinition(*definition, 1.0 / 3.0, diagnostics);
        PPR_TEST_ASSERT(compiled.has_value());
        const auto binding = std::ranges::find_if(
            compiled->m_bindings.m_materials, [&](const auto &item) { return item.m_key == key; });
        PPR_TEST_ASSERT(binding != compiled->m_bindings.m_materials.end());
        return binding->m_id;
    }

    PPR_UNIT_TEST (owned_profiles_resolve_all_rule_kinds_and_canonical_order) {
        const auto input = inputs();
        config::Diagnostics diagnostics{};
        auto source = validDocument();
        const auto profiles = decode(source, input, diagnostics);
        PPR_TEST_ASSERT(profiles.has_value() and profiles->size() == 1u);
        source.clear();
        const auto &profile = profiles->front();
        PPR_TEST_ASSERT(profile.m_key == "default" and profile.m_default_wall_material_id == materialId("wall"));
        PPR_TEST_ASSERT(profile.m_start_resources.size() == 2u and profile.m_biomes.size() == 1u);
        PPR_TEST_ASSERT(profile.m_start_resources[0u].m_material_id == materialId("rock"));
        PPR_TEST_ASSERT(profile.m_rule_keys.size() == 6u);
        const auto canonical = decodeDefinition(validDocument(), input, diagnostics);
        PPR_TEST_ASSERT(canonical.has_value());
        PPR_TEST_ASSERT(std::holds_alternative<sim::definition::VeinWorm>(ruleValue(*canonical, "vein").m_payload));
        PPR_TEST_ASSERT(std::holds_alternative<sim::definition::Cave>(ruleValue(*canonical, "caves").m_payload));
        PPR_TEST_ASSERT(std::holds_alternative<sim::definition::CarveWorm>(ruleValue(*canonical, "carve").m_payload));
        PPR_TEST_ASSERT(std::holds_alternative<sim::definition::Connection>(ruleValue(*canonical, "rooms").m_payload));
        PPR_TEST_ASSERT(std::holds_alternative<sim::definition::Feature>(ruleValue(*canonical, "pockets").m_payload));
        PPR_TEST_ASSERT(std::holds_alternative<sim::definition::MatterFill>(ruleValue(*canonical, "water").m_payload));
        PPR_TEST_ASSERT(
            std::get<sim::definition::VeinWorm>(ruleValue(*canonical, "vein").m_payload).m_host_material.m_key ==
            "wall");
        PPR_TEST_ASSERT(
            std::get<sim::definition::Feature>(ruleValue(*canonical, "pockets").m_payload).m_min_count == 1u);
        PPR_TEST_ASSERT(
            std::get<sim::definition::MatterFill>(ruleValue(*canonical, "water").m_payload).m_material.m_key ==
            "water");
        const auto reordered = decode(
            "schema_version = 3\n" + base + biome + resources + caves + carve + vein + connection + feature + fill,
            input, diagnostics);
        PPR_TEST_ASSERT(reordered.has_value());
        for (std::size_t i = 0u; i < profile.m_rule_keys.size(); ++i) {
            PPR_TEST_ASSERT(reordered->front().m_rule_keys[i] == profile.m_rule_keys[i]);
        }
        auto gas = fill;
        replace(gas, "payload.matter_fill.material = \"water\"", "payload.matter_fill.material = \"air\"");
        replace(gas, "payload.matter_fill.fill_height_m = 4.0", "");
        const auto gas_profile = decodeDefinition(
            "schema_version = 3\n" + base + biome + resources + feature + gas, input, diagnostics);
        PPR_TEST_ASSERT(gas_profile.has_value());
        PPR_TEST_ASSERT(
            not std::get<sim::definition::MatterFill>(ruleValue(*gas_profile, "water").m_payload).m_fill_height_m);
    };

    PPR_UNIT_TEST (smooth_worm_controls_are_opt_in_and_bounded) {
        const auto input = inputs();
        config::Diagnostics diagnostics{};
        const auto defaults = decodeDefinition(validDocument(), input, diagnostics);
        PPR_TEST_ASSERT(defaults.has_value());
        const auto &old_worm = std::get<sim::definition::CarveWorm>(ruleValue(*defaults, "carve").m_payload).m_geometry;
        PPR_TEST_ASSERT(old_worm.m_turn_smoothing == 0.0 and old_worm.m_radius_variation == 0.0);
        PPR_TEST_ASSERT(old_worm.m_chamber_probability == 0.0 and old_worm.m_chamber_scale == 1.0);
        auto smooth = validDocument();
        replace(
            smooth, "payload.carve_worm.geometry.turn_probability = 0.2",
            R"(payload.carve_worm.geometry.turn_probability = 0.2
payload.carve_worm.geometry.turn_smoothing = 0.8
payload.carve_worm.geometry.radius_variation = 0.4
payload.carve_worm.geometry.chamber_probability = 0.1
payload.carve_worm.geometry.chamber_scale = 4.0)");
        const auto accepted = decodeDefinition(smooth, input, diagnostics);
        PPR_TEST_ASSERT(accepted.has_value());
        const auto &worm = std::get<sim::definition::CarveWorm>(ruleValue(*accepted, "carve").m_payload).m_geometry;
        PPR_TEST_ASSERT(worm.m_turn_smoothing == 0.8 and worm.m_radius_variation == 0.4);
        PPR_TEST_ASSERT(worm.m_chamber_probability == 0.1 and worm.m_chamber_scale == 4.0);
        const std::pair<std::string_view, std::string_view> failures[] = {
            {"payload.carve_worm.geometry.turn_smoothing = 0.8", "payload.carve_worm.geometry.turn_smoothing = 0.0"},
            {"payload.carve_worm.geometry.turn_smoothing = 0.8", "payload.carve_worm.geometry.turn_smoothing = 1.1"},
            {"payload.carve_worm.geometry.turn_smoothing = 0.8", "payload.carve_worm.geometry.turn_smoothing = nan"},
            {"payload.carve_worm.geometry.radius_variation = 0.4",
                "payload.carve_worm.geometry.radius_variation = -0.1"},
            {"payload.carve_worm.geometry.radius_variation = 0.4",
                "payload.carve_worm.geometry.radius_variation = 1.1"},
            {"payload.carve_worm.geometry.chamber_probability = 0.1",
                "payload.carve_worm.geometry.chamber_probability = 0.11"},
            {"payload.carve_worm.geometry.chamber_scale = 4.0", "payload.carve_worm.geometry.chamber_scale = 4.1"},
            {"payload.carve_worm.geometry.chamber_scale = 4.0", "payload.carve_worm.geometry.chamber_scale = 0.9"},
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
            {"layout.max_sites = 32", "layout.max_sites = 3"},
            {"layout.site_attempts = 30", "layout.site_attempts = 4097"},
            {"layout.site_spacing_m = 100.0", "layout.site_spacing_m = nan"},
            {"boundaries.warp_amplitude_m = 10.0", "boundaries.warp_amplitude_m = inf"},
            {"boundaries.warp_x_field = \"noise\"", "boundaries.warp_x_field = \"absent\""},
            {"boundaries.material = \"wall\"", "boundaries.material = \"water\""},
            {"start.biome = \"temperate\"", "start.biome = \"absent\""},
            {"start.feature = \"landing\"", "start.feature = \"absent\""},
            {"start.marker = \"spawn\"", "start.marker = \"left\""},
            {"start.y_fine = 300", "start.y_fine = 4090"},
            {"start.clearance_m = 3.0", "start.clearance_m = 1.79"},
            {"start.clearance_m = 3.0", "start.clearance_m = 1366.0"},
            {"start.route_width_m = 1.0", "start.route_width_m = 0.79"},
            {"required_count = 1", "required_count = 33"},
            {"weight = 1.0", "weight = nan"},
            {"weight = 1.0", "weight = 0.0"},
            {"distance_max_m = 2048.0", "distance_max_m = -1.0"},
            {"marker = \"left\"", "marker = \"right\""},
            {"marker = \"left\"\nmaterial = \"rock\"", "marker = \"left\"\nmaterial = \"wall\""},
            {"minimum_fine_cells = 9", "minimum_fine_cells = 0"},
            {"max_distance_m = 20.0", "max_distance_m = 0.0"},
            {"payload.tag = \"cave\"", "payload.tag = \"unknown\""},
            {"payload.tag = \"cave\"", ""},
            {"order = 20", ""},
            {"payload.feature.biome = \"temperate\"", "payload.feature.biome = \"absent\""},
            {"payload.cave.field = \"noise\"", "payload.cave.field = \"absent\""},
            {"payload.cave.threshold.max = 1.0", "payload.cave.threshold.max = 0.3"},
            {"order = 30", "order = 20"},
            {"payload.carve_worm.geometry.count = 10", "payload.carve_worm.geometry.count = 0"},
            {"payload.carve_worm.geometry.length_min_m = 20.0", "payload.carve_worm.geometry.length_min_m = 61.0"},
            {"payload.carve_worm.geometry.radius_min_m = 1.0", "payload.carve_worm.geometry.radius_min_m = 4.0"},
            {"payload.carve_worm.geometry.turn_probability = 0.2",
                "payload.carve_worm.geometry.turn_probability = nan"},
            {"payload.vein_worm.host_material = \"wall\"", "payload.vein_worm.host_material = \"water\""},
            {"payload.tag = \"carve_worm\"", "payload.tag = \"carve_worm\"\npayload.carve_worm.material = \"rock\""},
            {"payload.connection.extra_links = 2", "payload.connection.extra_links = 1025"},
            {"payload.feature.feature = \"pocket\"", "payload.feature.feature = \"absent\""},
            {"payload.feature.attempts = 30", "payload.feature.attempts = 4097"},
            {"payload.feature.max_count = 4", "payload.feature.max_count = 0"},
            {"payload.feature.min_count = 1", "payload.feature.min_count = 5"},
            {"payload.matter_fill.placement = \"pockets\"", "payload.matter_fill.placement = \"carve\""},
            {"payload.matter_fill.marker = \"fill\"", "payload.matter_fill.marker = \"absent\""},
            {"payload.matter_fill.material = \"water\"", "payload.matter_fill.material = \"rock\""},
            {"payload.matter_fill.material = \"water\"", "payload.matter_fill.material = \"air\""},
            {"payload.matter_fill.temperature_k = 290.0", "payload.matter_fill.temperature_k = inf"},
            {"payload.matter_fill.bulk_density_kg_m3 = 1000.0", "payload.matter_fill.bulk_density_kg_m3 = 0.0"},
            {"payload.matter_fill.fill_height_m = 4.0", ""},
            {"payload.matter_fill.fill_height_m = 4.0", "payload.matter_fill.fill_height_m = 11.0"},
            {"payload.matter_fill.fill_height_m = 4.0", "payload.matter_fill.fill_height_m = 0.0"},
            {"payload.matter_fill.fill_height_m = 4.0", "payload.matter_fill.fill_height_m = nan"},
            {"payload.matter_fill.fill_height_m = 4.0", "payload.matter_fill.fill_height_m = 4.0\nunknown = true"},
        };
        for (const auto &[before, after]: failures) {
            auto source = validDocument();
            replace(source, before, after);
            config::Diagnostics diagnostics{};
            const auto failed = decode(source, input, diagnostics);
            if (failed) {
                throw std::runtime_error(
                    "Invalid world mutation was accepted: " + std::string{before} + " -> " + std::string{after});
            }
            PPR_TEST_ASSERT(not failed.has_value() and diagnostics.hasErrors());
            const bool numeric_boundary = after == "start.y_fine = 4090" or
                                          after == "start.clearance_m = 1.79" or
                                          after == "start.clearance_m = 1366.0" or
                                          after == "start.route_width_m = 0.79";
            if (numeric_boundary) {
                PPR_TEST_ASSERT(failed.error() == config::make_error_code(config::EConfigErrc::cross_field_invalid));
                PPR_TEST_ASSERT(diagnostics.entries().back().m_property == "worlds.default");
            } else {
                PPR_TEST_ASSERT(diagnostics.entries().back().m_property.starts_with("worlds.default."));
            }
            const auto &location = diagnostics.entries().back().m_location;
            PPR_TEST_ASSERT(
                location.find("worlds.toml") != std::string::npos or location.find("rules.toml") != std::string::npos);
        }
        for (const auto &[before, after]:
            {std::pair{"layout.site_attempts = 30", "layout.site_attempts = 65"},
                std::pair{"start.clearance_m = 3.0", "start.clearance_m = 2.9"},
                std::pair{"start.clearance_m = 3.0", "start.clearance_m = 11.0"},
                std::pair{"start.route_width_m = 1.0", "start.route_width_m = 31.0"},
                std::pair{"max_distance_m = 20.0", "max_distance_m = 1.0"}}) {
            auto source = validDocument();
            replace(source, before, after);
            config::Diagnostics diagnostics;
            PPR_TEST_ASSERT(decode(source, input, diagnostics).has_value() and not diagnostics.hasErrors());
        }
        auto reflection_input = inputs();
        replace(
            reflection_input.m_documents[3u].m_bytes, "allow_horizontal_reflection = true",
            "allow_horizontal_reflection = false");
        config::Diagnostics reflection{};
        PPR_TEST_ASSERT(not decode(validDocument(), reflection_input, reflection).has_value());
    };

    PPR_UNIT_TEST (world_diagnostics_preserve_exact_fields_and_locations) {
        const auto input = inputs();
        const std::tuple<std::string_view, std::string_view, std::string_view, config::EConfigErrc> failures[] = {
            {"start.y_fine = 300", "start.y_fine = 4090", "", config::EConfigErrc::cross_field_invalid},
            {"start.y_fine = 300", "start.y_fine = 4096", "start.y_fine", config::EConfigErrc::out_of_range},
            {"payload.carve_worm.geometry.length_min_m = 20.0", "payload.carve_worm.geometry.length_min_m = 61.0",
                "rules.carve.payload.carve_worm.geometry.length_max_m", config::EConfigErrc::cross_field_invalid},
            {"payload.matter_fill.marker = \"fill\"", "payload.matter_fill.marker = \"absent\"",
                "rules.water.payload.matter_fill.marker", config::EConfigErrc::cross_field_invalid},
            {"payload.matter_fill.fill_height_m = 4.0", "payload.matter_fill.fill_height_m = 11.0",
                "rules.water.payload.matter_fill.fill_height_m", config::EConfigErrc::cross_field_invalid},
        };
        for (const auto &[before, after, suffix, code]: failures) {
            auto source = validDocument();
            replace(source, before, after);
            const std::string property = suffix.empty() ? "worlds.default" : "worlds.default." + std::string{suffix};
            config::Diagnostics codec_diagnostics{};
            const auto bound = bindWorldSource(source, input);
            const bool rule = std::string_view{suffix}.starts_with("rules.");
            const auto &authored = bound.m_documents[rule ? 5u : 4u];
            const auto document = config::decode(
                authored.m_bytes, authored.m_path.string(), codec_diagnostics, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            const std::string source_property = rule ? "rules.default." + std::string{suffix.substr(6u)} : property;
            const auto entry = std::ranges::find(document->m_entries, source_property, &config::Entry::m_key);
            const auto group = std::ranges::find(document->m_groups, source_property, &config::GroupPresence::m_path);
            PPR_TEST_ASSERT(entry != document->m_entries.end() or group != document->m_groups.end());
            config::Diagnostics diagnostics{};
            const auto failed = decode(source, input, diagnostics);
            PPR_TEST_ASSERT(not failed.has_value());
            const auto diagnostic = std::ranges::find(diagnostics.entries(), property, &config::Diagnostic::m_property);
            if (diagnostic == diagnostics.entries().end()) {
                throw std::runtime_error("Missing exact world diagnostic " + property + "\n" + diagnostics.format());
            }
            PPR_TEST_ASSERT(failed.error() == config::make_error_code(code));
            PPR_TEST_ASSERT(diagnostic->m_code == code);
            PPR_TEST_ASSERT(
                diagnostic->m_location == (entry != document->m_entries.end() ? entry->m_location : group->m_location));
        }
    };

    PPR_UNIT_TEST (fill_heights_require_exact_signed_fine_cell_boundaries) {
        auto input = inputs();
        const auto pocket = input.m_documents[3u].m_bytes.find("[features.pocket]");
        const auto at = input.m_documents[3u].m_bytes.find("reservation.min_y_fine = 0", pocket);
        PPR_TEST_ASSERT(at != std::string::npos);
        input.m_documents[3u].m_bytes.replace(
            at, std::string_view{"reservation.min_y_fine = 0"}.size(), "reservation.min_y_fine = -30");
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
            replace(
                source, "payload.matter_fill.fill_height_m = 4.0",
                "payload.matter_fill.fill_height_m = " + authored_number(height));
            config::Diagnostics diagnostics{};
            const auto valid = decodeDefinition(source, input, diagnostics);
            PPR_TEST_ASSERT(valid.has_value());
            PPR_TEST_ASSERT(
                *std::get<sim::definition::MatterFill>(ruleValue(*valid, "water").m_payload).m_fill_height_m == height);
            // Subnormal decimal parsing can fail in the TOML codec before a
            // property exists. Normal neighbours exercise typed lattice errors.
            if (height == 0.0) {
                continue;
            }
            for (const double direction:
                {-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()}) {
                auto adjacent = validDocument();
                replace(
                    adjacent, "payload.matter_fill.fill_height_m = 4.0",
                    "payload.matter_fill.fill_height_m = " + authored_number(std::nextafter(height, direction)));
                config::Diagnostics invalid{};
                PPR_TEST_ASSERT(not decode(adjacent, input, invalid).has_value());
                PPR_TEST_ASSERT(
                    invalid.entries().back().m_property ==
                    "worlds.default.rules.water.payload.matter_fill.fill_height_m");
            }
        }
        auto off_lattice = validDocument();
        replace(off_lattice, "payload.matter_fill.fill_height_m = 4.0", "payload.matter_fill.fill_height_m = 0.1");
        config::Diagnostics diagnostics{};
        PPR_TEST_ASSERT(not decode(off_lattice, input, diagnostics).has_value());
    };

    PPR_UNIT_TEST (world_counts_and_weight_intervals_are_bounded) {
        const auto input = inputs();
        const std::pair<std::string_view, std::size_t> collections[] = {
            {"rules", kMaxWorldRules}, {"biomes", kMaxContentBiomes}, {"resources", kMaxStartResources}};
        for (const auto &[collection, cap]: collections) {
            std::string source = "schema_version = 3\n" + base + (collection == "biomes" ? std::string{} : biome);
            for (std::size_t i = 0u; i <= cap; ++i) {
                if (collection == "rules") {
                    auto rule = caves;
                    replace(rule, "rules.default.caves", "rules.default.item" + std::to_string(i));
                    replace(rule, "order = 20", "order = " + std::to_string(i));
                    source += rule;
                } else if (collection == "biomes") {
                    auto choice = biome;
                    replace(choice, "biomes.temperate", "biomes.item" + std::to_string(i));
                    replace(choice, "required_count = 1", "required_count = 0");
                    source += choice;
                } else {
                    auto resource = resources.substr(0u, resources.find("[worlds.default.resources.right]"));
                    replace(resource, "resources.left", "resources.item" + std::to_string(i));
                    source += resource;
                }
            }
            config::Diagnostics diagnostics{};
            const auto failed = decode(source, input, diagnostics);
            if (not failed and failed.error() != config::make_error_code(config::EConfigErrc::out_of_range)) {
                throw std::system_error(failed.error(), std::string{collection} + "\n" + diagnostics.format());
            }
            PPR_TEST_ASSERT(
                not failed.has_value() and
                failed.error() == config::make_error_code(config::EConfigErrc::out_of_range));
        }
        for (const auto &[first, second]: {std::pair{"1e308", "1e308"}, std::pair{"5e-324", "1e308"}}) {
            auto weight_input = input;
            auto secondary = weight_input.m_documents[1u].m_bytes.substr(
                weight_input.m_documents[1u].m_bytes.find("[biomes.temperate]"));
            for (auto at = secondary.find("biomes.temperate"); at != std::string::npos;
                at = secondary.find("biomes.temperate")) {
                secondary.replace(at, std::string_view{"biomes.temperate"}.size(), "biomes.secondary");
            }
            weight_input.m_documents[1u].m_bytes += secondary;
            weight_input.m_documents[6u].m_bytes +=
                "\n[appearance.biomes.secondary]\ndisplay_name = 'Secondary'\npreview_color_rgba = '709040FF'\n";
            auto a = biome, z = biome;
            replace(a, "biomes.temperate", "biomes.a");
            replace(z, "biomes.temperate", "biomes.z");
            replace(z, "biome = \"temperate\"", "biome = \"secondary\"");
            config::Diagnostics valid_diagnostics;
            PPR_TEST_ASSERT(
                decode("schema_version = 3\n" + base + resources + caves + a + z, weight_input, valid_diagnostics)
                    .has_value());
            replace(a, "weight = 1.0", std::string{"weight = "} + first);
            replace(z, "weight = 1.0", std::string{"weight = "} + second);
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(
                not decode("schema_version = 3\n" + base + resources + caves + a + z, weight_input, diagnostics)
                    .has_value());
        }
        auto duplicate_connection = connection;
        replace(duplicate_connection, "rules.default.rooms", "rules.default.second");
        replace(duplicate_connection, "order = 40", "order = 41");
        config::Diagnostics connections{};
        PPR_TEST_ASSERT(not decode(validDocument() + duplicate_connection, input, connections).has_value());
        auto too_many_features = feature;
        replace(too_many_features, "rules.default.pockets", "rules.default.more");
        replace(too_many_features, "order = 10", "order = 11");
        replace(too_many_features, "payload.feature.max_count = 4", "payload.feature.max_count = 1024");
        config::Diagnostics features{};
        PPR_TEST_ASSERT(not decode(validDocument() + too_many_features, input, features).has_value());
        std::string extra_worms = validDocument();
        for (u32 i = 0u; i < 4u; ++i) {
            auto worm = carve;
            replace(worm, "rules.default.carve", "rules.default.more" + std::to_string(i));
            replace(worm, "order = 30", "order = " + std::to_string(50u + i));
            replace(worm, "payload.carve_worm.geometry.count = 10", "payload.carve_worm.geometry.count = 1024");
            extra_worms += worm;
        }
        config::Diagnostics worms{};
        const auto excessive_worms = decode(extra_worms, input, worms);
        PPR_TEST_ASSERT(
            not excessive_worms.has_value() and
            excessive_worms.error() == config::make_error_code(config::EConfigErrc::out_of_range));
        std::string excessive_profiles = "schema_version = 3\n";
        for (std::size_t i = 0u; i <= 128u; ++i) {
            auto profile = base + biome + resources;
            for (auto at = profile.find("worlds.default"); at != std::string::npos;
                at = profile.find("worlds.default")) {
                profile.replace(at, std::string_view{"worlds.default"}.size(), "worlds.item" + std::to_string(i));
            }
            excessive_profiles += profile;
        }
        config::Diagnostics profiles{};
        auto profile_input = input;
        replace(
            profile_input.m_documents[6u].m_bytes,
            "[appearance.worlds.default]\ndisplay_name = \"Default\"\nbackground_stack = \"default\"\n", "");
        const auto too_many = decode(excessive_profiles, profile_input, profiles);
        if (not too_many and too_many.error() != config::make_error_code(config::EConfigErrc::out_of_range)) {
            throw std::system_error(too_many.error(), profiles.format());
        }
        PPR_TEST_ASSERT(
            not too_many.has_value() and
            too_many.error() == config::make_error_code(config::EConfigErrc::out_of_range));
    };

    PPR_UNIT_TEST (rule_documents_bind_explicit_profiles_and_reject_orphans_and_cross_document_duplicates) {
        auto source = inputs();
        source.m_documents[4u].m_bytes = "schema_version = 3\n" + base + biome + resources;
        auto rules = fill + feature + connection + vein + carve + caves;
        source.m_documents[5u].m_bytes += rules;
        config::Diagnostics diagnostics;
        const auto first = decodePackDefinition(source, diagnostics);
        PPR_TEST_ASSERT(first.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(first->m_worlds.m_entries.front().m_value.m_rules.m_entries.size() == 6u);

        auto orphan = source;
        replace(orphan.m_documents[5u].m_bytes, "rules.default.water", "rules.missing.water");
        config::Diagnostics orphan_errors;
        PPR_TEST_ASSERT(not decodePackDefinition(orphan, orphan_errors));
        PPR_TEST_ASSERT(orphan_errors.hasErrors());
        PPR_TEST_ASSERT(orphan_errors.entries().front().m_location.starts_with("rules.toml"));

        auto duplicate = source;
        auto forbidden_rule = caves;
        replace(forbidden_rule, "[rules.default.caves]", "[worlds.default.rules.caves]");
        duplicate.m_documents[4u].m_bytes += forbidden_rule;
        config::Diagnostics duplicate_errors;
        const auto rejected = decodePackDefinition(duplicate, duplicate_errors);
        PPR_TEST_ASSERT(
            not rejected and rejected.error() == config::make_error_code(config::EConfigErrc::unknown_property));

        std::array<config::Document, kContentDocumentCount> raw;
        for (std::size_t i = 0u; i < raw.size(); ++i) {
            const auto document = config::decode(
                source.m_documents[i].m_bytes, source.m_documents[i].m_path.string(), diagnostics,
                config::EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            raw[i] = *document;
        }
        const auto scalar = std::ranges::find(raw[5u].m_entries, "rules.default.water.order", &config::Entry::m_key);
        PPR_TEST_ASSERT(scalar != raw[5u].m_entries.end());
        const auto duplicate_scalar = *scalar;
        raw[5u].m_entries.push_back(duplicate_scalar);
        config::Diagnostics scalar_errors;
        const auto duplicated = mergeContentDocuments(raw, scalar_errors);
        PPR_TEST_ASSERT(
            not duplicated and duplicated.error() == config::make_error_code(config::EConfigErrc::duplicate_key));

        auto nested_owner = source;
        nested_owner.m_documents[4u].m_bytes = "schema_version = 3\n" + biome + resources + R"(
[worlds.default.boundaries]
material = "wall"
thickness_fine = 3
warp_amplitude_m = 10.0
warp_x_field = "noise"
warp_y_field = "noise"
[worlds.default.layout]
min_sites = 4
max_sites = 32
site_attempts = 30
site_spacing_m = 100.0
[worlds.default.start]
biome = "temperate"
feature = "landing"
marker = "spawn"
x_fine = 500
y_fine = 300
clearance_m = 3.0
route_width_m = 1.0
)";
        config::Diagnostics declaration_errors;
        PPR_TEST_ASSERT(decodePackDefinition(nested_owner, declaration_errors).has_value());
        PPR_TEST_ASSERT(not declaration_errors.hasErrors());
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
                detail::worlds::fill_heights_require_exact_signed_fine_cell_boundaries,
                detail::worlds::
                    rule_documents_bind_explicit_profiles_and_reject_orphans_and_cross_document_duplicates});
    };
    const UnitTest &gameWorldContentTests() noexcept {
        return world_content;
    }
}
