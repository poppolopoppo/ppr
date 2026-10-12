module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import game.colony.content;
import std;

namespace pP::game::tests::detail::features {
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
[materials.water]
numeric_id = 901
display_name = "Water"
phase = "liquid"
representation = "particle"
color_rgba = "0000FFFF"
reference_density_kg_m3 = 1000.0
)";
    const std::string base = R"([features.room]
display_name = "Room"
allow_horizontal_reflection = true
reservation_min_x_fine = -6
reservation_min_y_fine = -6
reservation_max_x_fine = 30
reservation_max_y_fine = 18
)";
    const std::string wall = R"([features.room.operations.wall]
order = 10
action = "paint"
shape = "rectangle"
x_fine = -6
y_fine = -6
width_fine = 36
height_fine = 24
material = "rock"
)";
    const std::string room = R"([features.room.operations.room]
order = 20
action = "carve"
shape = "ellipse"
x_fine = 0
y_fine = 0
width_fine = 24
height_fine = 12
)";
    const std::string tunnel = R"([features.room.operations.tunnel]
order = 30
action = "carve"
shape = "capsule"
x_fine = 0
y_fine = 0
end_x_fine = 27
end_y_fine = 0
radius_m = 1.0
)";
    const std::string marker = R"([features.room.markers.spawn]
role = "start"
x_fine = 0
y_fine = 0
)";
    std::string validDocument() {
        return "schema_version = 2\n" + base + tunnel + room + wall + marker;
    }
    void replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto position = text.find(before);
        PPR_TEST_ASSERT(position != std::string::npos);
        text.replace(position, before.size(), after);
    }

    PPR_UNIT_TEST (owned_shapes_sorted_orders_and_resolved_materials) {
        config::Diagnostics diagnostics{};
        auto materials = decodeMaterialCatalog(materials_text, "materials.toml", diagnostics);
        PPR_TEST_ASSERT(materials.has_value());
        auto source = validDocument();
        const auto decoded = decodeFeatureDefinitions(source, "features.toml", *materials, diagnostics);
        PPR_TEST_ASSERT(decoded.has_value() and decoded->size() == 1u);
        source.clear();
        const auto &feature = decoded->front();
        PPR_TEST_ASSERT(feature.m_key == "room" and feature.m_display_name == "Room");
        PPR_TEST_ASSERT(feature.m_allow_horizontal_reflection and feature.m_reservation.m_min_x_fine == -6);
        PPR_TEST_ASSERT(feature.m_operations.size() == 3u and feature.m_markers.size() == 1u);
        PPR_TEST_ASSERT(feature.m_operations[0u].m_shape == EFeatureShape::rectangle);
        PPR_TEST_ASSERT(feature.m_operations[0u].m_material_id == 501u);
        PPR_TEST_ASSERT(feature.m_operations[1u].m_shape == EFeatureShape::ellipse);
        PPR_TEST_ASSERT(feature.m_operations[1u].m_material_id == 0u);
        PPR_TEST_ASSERT(feature.m_operations[2u].m_radius_m == 1.0);
        PPR_TEST_ASSERT(feature.m_markers[0u].m_role == EFeatureMarkerRole::start);

        const auto reordered = decodeFeatureDefinitions(
            "schema_version = 2\n" + marker + wall + room + tunnel + base, "reordered.toml", *materials, diagnostics);
        PPR_TEST_ASSERT(reordered.has_value());
        for (std::size_t i = 0u; i < feature.m_operations.size(); ++i) {
            PPR_TEST_ASSERT(reordered->front().m_operations[i].m_key == feature.m_operations[i].m_key);
        }
        const auto no_markers = decodeFeatureDefinitions(
            "schema_version = 2\n" + base + wall, "features.toml", *materials, diagnostics);
        PPR_TEST_ASSERT(no_markers.has_value() and no_markers->front().m_markers.empty());
    };

    PPR_UNIT_TEST (malformed_templates_fail_with_qualified_source_diagnostics) {
        config::Diagnostics setup{};
        const auto materials = decodeMaterialCatalog(materials_text, "materials.toml", setup);
        PPR_TEST_ASSERT(materials.has_value());
        const std::pair<std::string_view, std::string_view> failures[] = {
            {"display_name = \"Room\"", ""},
            {"allow_horizontal_reflection = true", ""},
            {"reservation_max_y_fine = 18", "reservation_max_y_fine = -6"},
            {"reservation_min_x_fine = -6", "reservation_min_x_fine = -4096"},
            {"width_fine = 36", "width_fine = 37"},
            {"width_fine = 36", "width_fine = 0"},
            {"width_fine = 36", "width_fine = 1.5"},
            {"width_fine = 36", ""},
            {"radius_m = 1.0", "radius_m = 1.1"},
            {"radius_m = 1.0", "radius_m = nan"},
            {"radius_m = 1.0", "radius_m = inf"},
            {"radius_m = 1.0", "radius_m = 0.0"},
            {"end_y_fine = 0", ""},
            {"radius_m = 1.0", "radius_m = 1.0\nwidth_fine = 2"},
            {"width_fine = 24", "width_fine = 24\nend_x_fine = 3"},
            {"order = 30", "order = 20"},
            {"action = \"paint\"", "action = \"carve\""},
            {"material = \"rock\"", "material = \"water\""},
            {"material = \"rock\"", "material = \"absent\""},
            {"material = \"rock\"", ""},
            {"shape = \"ellipse\"", "shape = \"unknown\""},
            {"role = \"start\"", "role = \"entity\""},
            {"role = \"start\"", "role = \"start\"\nentity = \"dupe\""},
            {"[features.room.markers.spawn]", "[features.room.markers.spawn]\nx_fine = 0"},
        };
        for (const auto &[before, after]: failures) {
            auto source = validDocument();
            replace(source, before, after);
            config::Diagnostics diagnostics{};
            const auto failed = decodeFeatureDefinitions(source, "features.toml", *materials, diagnostics);
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
        const auto materials = decodeMaterialCatalog(materials_text, "materials.toml", setup);
        PPR_TEST_ASSERT(materials.has_value());
        for (const bool reservation: {true, false}) {
            auto bad_base = base;
            auto bad_marker = marker;
            if (reservation) {
                replace(bad_base, "reservation_max_y_fine = 18", "reservation_max_y_fine = -6");
            } else {
                replace(bad_marker, "y_fine = 0", "y_fine = 18");
            }
            const std::string source = "schema_version = 2\n" + bad_base + wall + bad_marker;
            const std::string property = reservation ? "features.room.reservation_max_y_fine"
                                                     : "features.room.markers.spawn.y_fine";
            config::Diagnostics codec_diagnostics{};
            const auto document = config::decode(
                source, "features.toml", codec_diagnostics, config::EDecodeMode::records);
            PPR_TEST_ASSERT(document.has_value());
            const auto entry = std::ranges::find(document->m_entries, property, &config::Entry::m_key);
            PPR_TEST_ASSERT(entry != document->m_entries.end());
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not decodeFeatureDefinitions(source, "features.toml", *materials, diagnostics).has_value());
            PPR_TEST_ASSERT(diagnostics.entries().back().m_property == property);
            PPR_TEST_ASSERT(diagnostics.entries().back().m_location == entry->m_location);
        }
    };

    PPR_UNIT_TEST (marker_bounds_and_child_caps) {
        config::Diagnostics setup{};
        const auto materials = decodeMaterialCatalog(materials_text, "materials.toml", setup);
        PPR_TEST_ASSERT(materials.has_value());
        for (const auto coordinate: {"x_fine = -7", "x_fine = 30", "y_fine = 18"}) {
            auto bad_marker = marker;
            replace(bad_marker, coordinate[0] == 'x' ? "x_fine = 0" : "y_fine = 0", coordinate);
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not decodeFeatureDefinitions(
                "schema_version = 2\n" + base + wall + bad_marker, "features.toml", *materials, diagnostics)
                    .has_value());
        }
        for (const auto &[collection, cap]:
            {std::pair{"operations", kMaxFeatureOperations}, std::pair{"markers", kMaxFeatureMarkers}}) {
            std::string source = "schema_version = 2\n" + base;
            for (std::size_t i = 0u; i <= cap; ++i) {
                source += "[features.room." + std::string{collection} + ".item" + std::to_string(i) + "]\nx_fine = 0\n";
            }
            config::Diagnostics diagnostics{};
            const auto failed = decodeFeatureDefinitions(source, "features.toml", *materials, diagnostics);
            PPR_TEST_ASSERT(
                not failed.has_value() and
                failed.error() == config::make_error_code(config::EConfigErrc::out_of_range));
        }
        config::Diagnostics empty{};
        PPR_TEST_ASSERT(not decodeFeatureDefinitions("schema_version = 2\n" + base, "features.toml", *materials, empty)
                .has_value());
        std::string excessive = "schema_version = 2\n";
        for (std::size_t i = 0u; i <= kMaxContentFeatures; ++i) {
            excessive += "[features.item" + std::to_string(i) + "]\ndisplay_name = \"Item\"\n";
        }
        config::Diagnostics count_diagnostics{};
        const auto too_many = decodeFeatureDefinitions(excessive, "features.toml", *materials, count_diagnostics);
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
