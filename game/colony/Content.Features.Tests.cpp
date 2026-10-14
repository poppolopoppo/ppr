module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import engine.sim;
import game.colony.content;
import std;

namespace pP::game::tests::detail::features {
    // ------------------------------------------------------------------
    // content features
    // ------------------------------------------------------------------

    using namespace colony;
    constexpr std::string_view materials_text = R"(schema_version = 3
[materials.rock]
phase = "solid"
representation = "terrain"
diggable = true
density = 2500.0
[materials.water]
phase = "liquid"
representation = "particle"
density = 1000.0
)";
    const std::string base = R"([features.room]
allow_horizontal_reflection = true
reservation.min_x_fine = -6
reservation.min_y_fine = -6
reservation.max_x_fine = 30
reservation.max_y_fine = 18
)";
    const std::string wall = R"([features.room.operations.wall]
order = 10
x_fine = -6
y_fine = -6
action.tag = "paint"
action.paint.material = "rock"
action.paint.shape.tag = "rectangle"
action.paint.shape.rectangle.width_fine = 36
action.paint.shape.rectangle.height_fine = 24
)";
    const std::string room = R"([features.room.operations.room]
order = 20
x_fine = 0
y_fine = 0
action.tag = "carve"
action.carve.shape.tag = "ellipse"
action.carve.shape.ellipse.width_fine = 24
action.carve.shape.ellipse.height_fine = 12
)";
    const std::string tunnel = R"([features.room.operations.tunnel]
order = 30
x_fine = 0
y_fine = 0
action.tag = "carve"
action.carve.shape.tag = "capsule"
action.carve.shape.capsule.end_x_fine = 27
action.carve.shape.capsule.end_y_fine = 0
action.carve.shape.capsule.radius_m = 1.0
)";
    const std::string marker = R"([features.room.markers.spawn]
role = "start"
x_fine = 0
y_fine = 0
)";
    std::string validDocument() {
        return "schema_version = 3\n" + base + tunnel + room + wall + marker;
    }
    void replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto position = text.find(before);
        PPR_TEST_ASSERT(position != std::string::npos);
        text.replace(position, before.size(), after);
    }

    ContentPackSource packSource(const std::string_view text, const std::string_view origin) {
        ContentPackSource source{};
        const char *names[] = {
            "materials.toml", "biomes.toml", "fields.toml", "features.toml", "worlds.toml", "rules.toml",
            "appearance.toml"};
        for (std::size_t i = 0u; i < kContentDocumentCount; ++i) {
            source.m_documents[i] = {names[i], "schema_version = 3\n"};
        }

        source.m_documents[0u].m_bytes = std::string{materials_text};
        source.m_documents[1u].m_bytes += R"([biomes.plain]
temperature.field = "noise"
temperature.base_k = 293.0
[biomes.plain.composition.base]
order = 0
field = "noise"
depth.min = 0.0
[biomes.plain.composition.base.materials.rock]
material = "rock"
weight = 1.0
)";
        source.m_documents[2u].m_bytes += "[fields.noise]\nalgorithm = 'perlin'\nfrequency_per_metre = 0.01\n";

        source.m_documents[3u] = {std::string{origin}, std::string{text}};
        source.m_documents[6u].m_bytes += R"([appearance.materials.rock]
display_name = "Rock"
color_rgba = "778899FF"
albedo_texture = "textures/rock.png"
texture_period_m = 1.0
[appearance.materials.water]
display_name = "Water"
color_rgba = "0000FFFF"
[appearance.biomes.plain]
display_name = "Plain"
preview_color_rgba = "709040FF"
[appearance.features.room]
display_name = "Room"
)";
        return source;
    }

    Expected<Array<FeatureDefinition>> decodeFeatures(
        const std::string_view text, const std::string_view origin, config::Diagnostics &diagnostics) {
        return decodePackDefinition(packSource(text, origin), diagnostics)
            .and_then(
                [&](PackDefinition definition) { return projectContentCatalog(std::move(definition), diagnostics); })
            .transform([](ContentCatalog catalog) {
                return Array<FeatureDefinition>{catalog.features().begin(), catalog.features().end()};
            });
    }

    u16 materialId(const std::string_view key) {
        config::Diagnostics diagnostics;
        const auto definition = decodePackDefinition(packSource(validDocument(), "features.toml"), diagnostics);
        PPR_TEST_ASSERT(definition.has_value());
        const auto compiled = sim::definition::compileDefinition(*definition, 1.0 / 3.0, diagnostics);
        PPR_TEST_ASSERT(compiled.has_value());
        const auto binding = std::ranges::find_if(
            compiled->m_bindings.m_materials, [&](const auto &item) { return item.m_key == key; });
        PPR_TEST_ASSERT(binding != compiled->m_bindings.m_materials.end());
        return binding->m_id;
    }

    PPR_UNIT_TEST (owned_shapes_sorted_orders_and_resolved_materials) {
        config::Diagnostics diagnostics{};
        auto source = validDocument();
        const auto decoded = decodeFeatures(source, "features.toml", diagnostics);
        PPR_TEST_ASSERT(decoded.has_value() and decoded->size() == 1u);
        source.clear();
        const auto &feature = decoded->front();
        PPR_TEST_ASSERT(feature.m_key == "room" and feature.m_display_name == "Room");
        PPR_TEST_ASSERT(feature.m_allow_horizontal_reflection and feature.m_reservation.m_min_x_fine == -6);
        PPR_TEST_ASSERT(feature.m_operations.size() == 3u and feature.m_markers.size() == 1u);
        PPR_TEST_ASSERT(feature.m_operations[0u].m_shape == EFeatureShape::rectangle);
        PPR_TEST_ASSERT(feature.m_operations[0u].m_material_id == materialId("rock"));
        PPR_TEST_ASSERT(feature.m_operations[1u].m_shape == EFeatureShape::ellipse);
        PPR_TEST_ASSERT(feature.m_operations[1u].m_material_id == 0u);
        PPR_TEST_ASSERT(feature.m_operations[2u].m_radius_m == 1.0);
        PPR_TEST_ASSERT(feature.m_markers[0u].m_role == EFeatureMarkerRole::start);

        const auto reordered = decodeFeatures(
            "schema_version = 3\n" + marker + wall + room + tunnel + base, "reordered.toml", diagnostics);
        PPR_TEST_ASSERT(reordered.has_value());
        for (std::size_t i = 0u; i < feature.m_operations.size(); ++i) {
            PPR_TEST_ASSERT(reordered->front().m_operations[i].m_key == feature.m_operations[i].m_key);
        }
        const auto no_markers = decodeFeatures("schema_version = 3\n" + base + wall, "features.toml", diagnostics);
        PPR_TEST_ASSERT(no_markers.has_value() and no_markers->front().m_markers.empty());
    };

    PPR_UNIT_TEST (malformed_templates_fail_with_qualified_source_diagnostics) {
        config::Diagnostics setup{};
        const std::pair<std::string_view, std::string_view> failures[] = {
            {"allow_horizontal_reflection = true", ""},
            {"reservation.max_y_fine = 18", "reservation.max_y_fine = -6"},
            {"reservation.min_x_fine = -6", "reservation.min_x_fine = -4096"},
            {"action.paint.shape.rectangle.width_fine = 36", "action.paint.shape.rectangle.width_fine = 37"},
            {"action.paint.shape.rectangle.width_fine = 36", "action.paint.shape.rectangle.width_fine = 0"},
            {"action.paint.shape.rectangle.width_fine = 36", "action.paint.shape.rectangle.width_fine = 1.5"},
            {"action.paint.shape.rectangle.width_fine = 36", ""},
            {"action.carve.shape.capsule.radius_m = 1.0", "action.carve.shape.capsule.radius_m = 1.1"},
            {"action.carve.shape.capsule.radius_m = 1.0", "action.carve.shape.capsule.radius_m = nan"},
            {"action.carve.shape.capsule.radius_m = 1.0", "action.carve.shape.capsule.radius_m = inf"},
            {"action.carve.shape.capsule.radius_m = 1.0", "action.carve.shape.capsule.radius_m = 0.0"},
            {"action.carve.shape.capsule.end_y_fine = 0", ""},
            {"action.carve.shape.capsule.radius_m = 1.0",
                "action.carve.shape.capsule.radius_m = 1.0\naction.carve.shape.capsule.width_fine = 2"},
            {"action.carve.shape.ellipse.width_fine = 24",
                "action.carve.shape.ellipse.width_fine = 24\nend_x_fine = 3"},
            {"order = 30", "order = 20"},
            {"action.tag = \"paint\"", "action.tag = \"carve\""},
            {"action.paint.material = \"rock\"", "action.paint.material = \"water\""},
            {"action.paint.material = \"rock\"", "action.paint.material = \"absent\""},
            {"action.paint.material = \"rock\"", ""},
            {"action.carve.shape.tag = \"ellipse\"", "action.carve.shape.tag = \"unknown\""},
            {"role = \"start\"", "role = \"entity\""},
            {"role = \"start\"", "role = \"start\"\nentity = \"dupe\""},
            {"[features.room.markers.spawn]", "[features.room.markers.spawn]\nx_fine = 0"},
        };
        for (const auto &[before, after]: failures) {
            auto source = validDocument();
            replace(source, before, after);
            config::Diagnostics diagnostics{};
            const auto failed = decodeFeatures(source, "features.toml", diagnostics);
            PPR_TEST_ASSERT(not failed.has_value() and diagnostics.hasErrors());
            // Codec duplicate-key errors precede record grouping and use their own location.
            if (failed.error() != config::make_error_code(config::EConfigErrc::codec_error)) {
                PPR_TEST_ASSERT(diagnostics.entries().back().m_property.starts_with("features.room."));
            }
            PPR_TEST_ASSERT(diagnostics.entries().back().m_location.find("features.toml") != std::string::npos);
        }
    };

    PPR_UNIT_TEST (invalid_y_diagnostics_identify_the_exact_source_field) {
        config::Diagnostics setup{};
        for (const bool reservation: {true, false}) {
            auto bad_base = base;
            auto bad_marker = marker;
            if (reservation) {
                replace(bad_base, "reservation.max_y_fine = 18", "reservation.max_y_fine = -6");
            } else {
                replace(bad_marker, "y_fine = 0", "y_fine = 18");
            }
            const std::string source = "schema_version = 3\n" + bad_base + wall + bad_marker;
            const std::string property = reservation ? "features.room.reservation.max_y_fine"
                                                     : "features.room.markers.spawn.y_fine";
            config::Diagnostics codec_diagnostics{};
            const auto document = config::decode(source, "features.toml", codec_diagnostics, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            const auto entry = std::ranges::find(document->m_entries, property, &config::Entry::m_key);
            PPR_TEST_ASSERT(entry != document->m_entries.end());
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not decodeFeatures(source, "features.toml", diagnostics).has_value());
            const auto diagnostic = std::ranges::find(diagnostics.entries(), property, &config::Diagnostic::m_property);
            if (diagnostic == diagnostics.entries().end()) {
                throw std::runtime_error("Missing expected diagnostic " + property + "\n" + diagnostics.format());
            }
            PPR_TEST_ASSERT(diagnostic->m_code == config::EConfigErrc::cross_field_invalid);
            PPR_TEST_ASSERT(diagnostic->m_location == entry->m_location);
        }
    };

    PPR_UNIT_TEST (marker_bounds_and_child_caps) {
        config::Diagnostics setup{};
        for (const auto coordinate: {"x_fine = -7", "x_fine = 30", "y_fine = 18"}) {
            auto bad_marker = marker;
            replace(bad_marker, coordinate[0] == 'x' ? "x_fine = 0" : "y_fine = 0", coordinate);
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(
                not decodeFeatures("schema_version = 3\n" + base + wall + bad_marker, "features.toml", diagnostics)
                    .has_value());
        }
        for (const auto &[collection, cap]:
            {std::pair{"operations", kMaxFeatureOperations}, std::pair{"markers", kMaxFeatureMarkers}}) {
            std::string source = "schema_version = 3\n" +
                                 base +
                                 (std::string_view{collection} == "markers" ? wall : std::string{});
            for (std::size_t i = 0u; i <= cap; ++i) {
                source += "[features.room." +
                          std::string{collection} +
                          ".item" +
                          std::to_string(i) +
                          "]\nx_fine = 0\ny_fine = 0\n";
                source +=
                    std::string_view{collection} == "markers"
                        ? "role = 'inert'\n"
                        : "order = " +
                              std::to_string(i) +
                              "\naction.tag = 'carve'\naction.carve.shape.tag = 'rectangle'\naction.carve.shape.rectangle.width_fine = 1\naction.carve.shape.rectangle.height_fine = 1\n";
            }
            config::Diagnostics diagnostics{};
            const auto failed = decodeFeatures(source, "features.toml", diagnostics);
            if (not failed and failed.error() != config::make_error_code(config::EConfigErrc::out_of_range)) {
                throw std::system_error(failed.error(), std::string{collection} + "\n" + diagnostics.format());
            }
            PPR_TEST_ASSERT(
                not failed.has_value() and
                failed.error() == config::make_error_code(config::EConfigErrc::out_of_range));
        }
        config::Diagnostics empty{};
        PPR_TEST_ASSERT(not decodeFeatures("schema_version = 3\n" + base, "features.toml", empty).has_value());
        std::string excessive = "schema_version = 3\n";
        for (std::size_t i = 0u; i <= kMaxContentFeatures; ++i) {
            auto feature = base + wall;
            replace(feature, "features.room", "features.item" + std::to_string(i));
            replace(feature, "features.room", "features.item" + std::to_string(i));
            excessive += feature;
        }
        config::Diagnostics count_diagnostics{};
        auto excessive_source = packSource(excessive, "features.toml");
        auto &appearance = excessive_source.m_documents[6u].m_bytes;
        appearance.erase(appearance.find("[appearance.features.room]"));
        const auto too_many = decodePackDefinition(excessive_source, count_diagnostics);
        if (not too_many and too_many.error() != config::make_error_code(config::EConfigErrc::out_of_range)) {
            throw std::system_error(too_many.error(), count_diagnostics.format());
        }
        PPR_TEST_ASSERT(
            not too_many.has_value() and
            too_many.error() == config::make_error_code(config::EConfigErrc::out_of_range));
    };
}

namespace pP::game::tests {
    const UnitTest feature_content = UnitTest::Named("feature_content") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::features::owned_shapes_sorted_orders_and_resolved_materials,
                detail::features::malformed_templates_fail_with_qualified_source_diagnostics,
                detail::features::marker_bounds_and_child_caps,
                detail::features::invalid_y_diagnostics_identify_the_exact_source_field});
    };
    const UnitTest &gameFeatureContentTests() noexcept {
        return feature_content;
    }
}
