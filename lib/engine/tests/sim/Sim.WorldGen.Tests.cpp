module;
#include "pP/UnitTest.h"

module engine.tests.sim;

import engine.core;
import engine.sim;

import std;

namespace pP::tests::detail {
    // subgroup: world gen detail
    // ------------------------------------------------------------------
    // world gen
    // ------------------------------------------------------------------

    namespace WorldGen {
        using namespace pP::sim;

        [[nodiscard]] GenerationRecipe numericRecipe() {
            GenerationRecipe recipe;
            recipe.m_materials.push_back({17u, true, true, EGenerationMatter::solid, 2500.0});
            recipe.m_fields.push_back({101u, {.m_frequency_per_metre = 0.025}});
            GenerationBand band{.m_key = 201u, .m_depth_max = 1.0};
            band.m_choices.push_back({202u, 17u, 1.0});
            GenerationBiome biome{.m_key = 203u, .m_temperature_base_k = 293.0, .m_temperature_variation_k = 8.0};
            biome.m_bands.push_back(std::move(band));
            recipe.m_biomes.push_back(std::move(biome));
            GenerationFeature feature{.m_key = 301u, .m_bounds = {0, 0, 30, 30}};
            feature.m_operations.push_back({.m_key = 302u, .m_carve = true, .m_width = 30u, .m_height = 30u});
            feature.m_markers.push_back({303u, EGenerationMarker::start, 15, 10});
            recipe.m_features.push_back(std::move(feature));
            recipe.m_constraints.push_back({401u, 0u, 1u, 1.0, 0.0, 1.0, 0.0, 2048.0});
            recipe.m_cell_size_m = 1.0 / 3.0;
            recipe.m_reference_thickness_m = 1.0;
            recipe.m_boundary_material = 17u;
            recipe.m_boundary_fine = 3u;
            recipe.m_min_sites = 4u;
            recipe.m_max_sites = 16u;
            recipe.m_site_attempts = 30u;
            recipe.m_site_spacing_m = 100.0;
            recipe.m_start_x_fine = 2000u;
            recipe.m_start_y_fine = 3000u;
            recipe.m_start_clearance_m = 3.0;
            recipe.m_start_route_width_m = 1.0;
            recipe.m_actor_half_width_m = 0.4;
            recipe.m_actor_half_height_m = 0.9;
            return recipe;
        }

        [[nodiscard]] definition::Definition minimalDefinition() {
            definition::Definition result;
            result.m_materials.m_entries.push_back(
                {"rock", {EGenerationMatter::solid, definition::EMaterialRepresentation::terrain, true, 2500.0}});
            result.m_fields.m_entries.push_back({"climate", {.m_frequency_per_metre = 0.025}});
            definition::CompositionBand band;
            band.m_field.m_key = "climate";
            band.m_materials.m_entries.push_back({"stone", {{"rock"}, 1.0}});
            definition::Biome biome;
            biome.m_temperature = {{"climate"}, 293.0, 0.0};
            biome.m_composition.m_entries.push_back({"base", std::move(band)});
            result.m_biomes.m_entries.push_back({"plain", std::move(biome)});
            return result;
        }

        [[nodiscard]] definition::Definition completeDefinition() {
            auto result = minimalDefinition();
            auto &biome = result.m_biomes.m_entries.front().m_value;
            biome.m_temperature.m_variation_k = 8.0;
            definition::Deposit deposit;
            deposit.m_order = 7u;
            deposit.m_field.m_key = "climate";
            deposit.m_material.m_key = "rock";
            deposit.m_host_material.m_key = "rock";
            deposit.m_depth.m_min = 0.125;
            deposit.m_depth.m_max = 0.875;
            deposit.m_threshold = {-0.5, 0.75};
            biome.m_deposits.m_entries.push_back({"ore", std::move(deposit)});
            biome.m_pore = definition::Pore{{"climate"}, 0.1, {1.0, 2.0}, 0.25, 2u};
            return result;
        }

        [[nodiscard]] auto numericBiomeValues(const GenerationBiome &biome) {
            const auto choice_values = [](const GenerationChoice &choice) {
                return std::tuple{choice.m_key, choice.m_material, choice.m_weight};
            };
            using ChoiceValues = decltype(choice_values(GenerationChoice{}));
            const auto band_values = [&](const GenerationBand &band) {
                Array<ChoiceValues> choices;
                for (const auto &choice: band.m_choices) {
                    choices.push_back(choice_values(choice));
                }
                return std::tuple{
                    band.m_key, band.m_order, band.m_field, band.m_depth_min, band.m_depth_max, band.m_total_weight,
                    band.m_bed_period_m, band.m_bed_warp_m, band.m_bed_dip, std::move(choices)};
            };
            Array<decltype(band_values(GenerationBand{}))> bands;
            for (const auto &band: biome.m_bands) {
                bands.push_back(band_values(band));
            }
            const auto deposit_values = [](const GenerationDeposit &deposit) {
                return std::tuple{
                    deposit.m_key, deposit.m_order, deposit.m_field, deposit.m_material, deposit.m_host,
                    deposit.m_depth_min, deposit.m_depth_max, deposit.m_threshold_min, deposit.m_threshold_max,
                    deposit.m_shape, deposit.m_stage, deposit.m_blob_count, deposit.m_radius_min_m,
                    deposit.m_radius_max_m, deposit.m_stretch_min, deposit.m_stretch_max, deposit.m_distortion,
                    deposit.m_core_coverage, deposit.m_fringe_coverage, deposit.m_sprinkle_probability,
                    deposit.m_orientation_min_degrees, deposit.m_orientation_max_degrees};
            };
            Array<decltype(deposit_values(GenerationDeposit{}))> deposits;
            for (const auto &deposit: biome.m_deposits) {
                deposits.push_back(deposit_values(deposit));
            }
            return std::tuple{
                biome.m_key, biome.m_temperature_field, biome.m_temperature_base_k, biome.m_temperature_variation_k,
                std::move(bands), std::move(deposits), biome.m_lining_material, biome.m_lining_thickness_fine,
                biome.m_relaxation_passes, biome.m_relaxation_adoption, biome.m_sprinkle_probability,
                biome.m_pore_field, biome.m_pore_probability, biome.m_pore_radius_min_m, biome.m_pore_radius_max_m,
                biome.m_pore_affinity_min, biome.m_pore_passes, biome.m_finish_passes, biome.m_finish_adoption,
                biome.m_soil_material, biome.m_weathered_material, biome.m_soil_depth_m, biome.m_weathered_depth_m,
                biome.m_formation_material, biome.m_rind_material, biome.m_formation_probability,
                biome.m_formation_length_m, biome.m_erosion_passes};
        }

        template<typename T>
        void flipFingerprintMember(T &value) {
            // Fingerprinting is not validation: mutations need only be distinct resolved values.
            if constexpr (requires { value.m_entries; }) {
                if (value.m_entries.empty()) {
                    value.m_entries.emplace_back();
                } else {
                    value.m_entries.clear();
                }
            } else if constexpr (requires {
                                     value.has_value();
                                     value.reset();
                                 }) {
                if (value) {
                    value.reset();
                } else {
                    value.emplace();
                }
            } else if constexpr (requires { T::describe(); }) {
                bool flipped = false;
                config::visit(value, [&](const auto &, auto &child) {
                    if (not flipped) {
                        flipFingerprintMember(child);
                        flipped = true;
                    }
                });
            } else if constexpr (requires { std::get<0>(value); }) {
                std::visit([&](auto &active) { flipFingerprintMember(active); }, value);
            } else if constexpr (std::is_same_v<T, std::string>) {
                value += "_changed";
            } else if constexpr (std::is_same_v<T, bool>) {
                value = not value;
            } else if constexpr (std::is_enum_v<T>) {
                value = static_cast<T>(static_cast<std::underlying_type_t<T>>(value) + 1u);
            } else if constexpr (std::is_floating_point_v<T>) {
                value += 0.125;
            } else if constexpr (std::is_integral_v<T>) {
                ++value;
            } else {
                static_assert(sizeof(T) == 0u, "Fingerprint mutation needs a described value or supported scalar");
            }
        }

        template<typename T, typename Visitor>
        void walkFingerprintMembers(T &object, Visitor &visitor, std::string_view prefix = {});

        template<typename T, typename Visitor>
        void walkFingerprintChildren(T &value, Visitor &visitor, const std::string &path) {
            if constexpr (requires { value.m_entries; }) {
                for (auto &entry: value.m_entries) {
                    const auto entry_path = path + "." + entry.m_key;
                    visitor(entry_path, entry.m_value);
                    walkFingerprintMembers(entry.m_value, visitor, entry_path);
                }
            } else if constexpr (requires { value.has_value(); }) {
                if (value) {
                    if constexpr (requires { std::remove_cvref_t<decltype(*value)>::describe(); }) {
                        walkFingerprintMembers(*value, visitor, path);
                    }
                }
            } else if constexpr (requires { std::remove_cvref_t<T>::describe(); }) {
                walkFingerprintMembers(value, visitor, path);
            }
        }

        template<typename T, typename Visitor>
        void walkFingerprintMembers(T &object, Visitor &visitor, const std::string_view prefix) {
            config::visit(object, [&](const auto &member, auto &value) {
                const auto name = member.m_metadata.m_name;
                const auto path = prefix.empty() ? std::string{name} : std::string{prefix} + "." + std::string{name};
                visitor(path, value);

                // Reference wrappers are scalar schema members, not nested groups.
                using Constraint = std::remove_cvref_t<decltype(member.m_constraint)>;
                if constexpr (std::is_same_v<Constraint, config::details::GroupDomain> or
                              std::is_same_v<Constraint, config::CollectionDomain>) {
                    walkFingerprintChildren(value, visitor, path);
                } else if constexpr (config::details::is_variant_domain_v<Constraint>) {
                    const auto payload_path = path + "." + std::string{config::variantTag(member, value)};
                    const auto descend = [&](auto &variant) {
                        std::visit(
                            [&](auto &payload) { walkFingerprintMembers(payload, visitor, payload_path); }, variant);
                    };
                    if constexpr (config::details::is_optional_v<std::remove_cvref_t<decltype(value)>>) {
                        if (value) {
                            descend(*value);
                        }
                    } else {
                        descend(value);
                    }
                }
            });
        }

        [[nodiscard]] std::string featureText(
            const std::string_view action, const std::string_view shape, const bool allow_reflection = false) {
            std::string text = R"toml(
[materials.rock]
phase='solid'
representation='terrain'
diggable=true
density=2500.0
[fields.climate]
algorithm='open_simplex2'
frequency_per_metre=0.025
[biomes.plain.temperature]
field='climate'
base_k=293.0
[biomes.plain.composition.base]
order=0
field='climate'
[biomes.plain.composition.base.depth]
min=0.0
[biomes.plain.composition.base.materials.stone]
material='rock'
weight=1.0
[features.room]
)toml";
            text += allow_reflection ? "allow_horizontal_reflection=true\n" : "allow_horizontal_reflection=false\n";
            text += R"toml([features.room.reservation]
min_x_fine=-3
min_y_fine=-3
max_x_fine=12
max_y_fine=12
[features.room.operations.cut]
order=7
x_fine=0
y_fine=0
[features.room.operations.cut.action]
)toml";
            text += "tag='" +
                    std::string{action} +
                    "'\n[features.room.operations.cut.action." +
                    std::string{action} +
                    "]\n";
            if (action == "paint") {
                text += "material='rock'\n";
            }
            text += "[features.room.operations.cut.action." +
                    std::string{action} +
                    ".shape]\ntag='" +
                    std::string{shape} +
                    "'\n";
            text +=
                "[features.room.operations.cut.action." + std::string{action} + ".shape." + std::string{shape} + "]\n";
            text += shape == "capsule" ? "end_x_fine=6\nend_y_fine=3\nradius_m=1.0\n" : "width_fine=6\nheight_fine=9\n";
            return text;
        }

        [[nodiscard]] Expected<config::Resolved<definition::Definition>> resolveFeatureText(
            const std::string_view text, config::Diagnostics &diagnostics) {
            const auto document = config::decode(text, "feature.toml", diagnostics, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            const auto layer = config::toTreeLayer(*document, "feature.toml");
            return config::resolveRecursive<definition::Definition>(std::span{&layer, 1u}, diagnostics);
        }

        [[nodiscard]] definition::GenerationCompileParameters recipeParameters() {
            return {1.0 / 3.0, 1.0, 0.4, 0.9};
        }

        [[nodiscard]] definition::Definition recipeDefinition() {
            using namespace definition;
            auto result = minimalDefinition();
            result.m_materials.m_entries.push_back(
                {"water", {EGenerationMatter::liquid, EMaterialRepresentation::particle, {}, 1000.0}});
            result.m_materials.m_entries.push_back(
                {"air", {EGenerationMatter::gas, EMaterialRepresentation::particle, {}, 1.0}});
            FeatureDefinition feature;
            feature.m_reservation = {0, 0, 30, 30};
            feature.m_allow_horizontal_reflection = true;
            feature.m_operations.m_entries.push_back({"cut", {0u, 0, 0, Carve{Rectangle{30u, 30u}}}});
            feature.m_markers.m_entries = {
                {"start", {EGenerationMarker::start, 15, 10}}, {"pocket", {EGenerationMarker::sealed_pocket, 15, 15}},
                {"resource", {EGenerationMarker::resource_anchor, 20, 20}},
                {"room", {EGenerationMarker::traversal_room, 10, 10}}, {"inert", {EGenerationMarker::inert, 5, 5}}};
            result.m_features.m_entries.push_back({"room", feature});
            feature.m_markers.m_entries.front().m_value.m_x_fine = 8;
            result.m_features.m_entries.push_back({"other", std::move(feature)});
            ProfileDefinition profile;
            profile.m_layout = {4u, 16u, 30u, 100.0};
            profile.m_boundaries = {{"rock"}, 3u, 0.0, {"climate"}, {"climate"}};
            profile.m_start = {{"plain"}, {"room"}, {"start"}, 2000u, 3000u, 3.0, 1.0, {}};
            profile.m_biomes.m_entries.push_back({"plain", {{"plain"}, 1u, 1.0, {0.0, 1.0}, 0.0, 2048.0}});
            profile.m_resources.m_entries.push_back({"stone", {{"resource"}, {"rock"}, 1u, 100.0}});
            WormGeometry worm{{"plain"}, 2u, {0.0, 1.0}, 10.0, 20.0, 1.0, 2.0, 0.1};
            profile.m_rules.m_entries = {
                {"cave", {0u, true, Cave{{"plain"}, {"climate"}, {0.0, 1.0}, {-0.5, 0.5}}}},
                {"carve", {1u, true, CarveWorm{worm}}}, {"vein", {0u, true, VeinWorm{worm, {"rock"}, {"rock"}}}},
                {"connect", {2u, true, Connection{1.0, 1u, 2u, 1u, 1u, 0.5}}},
                {"place", {0u, true, Feature{{"room"}, {"plain"}, {0.0, 1.0}, 10.0, 1u, 2u, 30u, 3u, true}}},
                {"liquid", {0u, true, MatterFill{{"place"}, {"pocket"}, {"water"}, 293.0, 1000.0, 5.0}}},
                {"gas", {1u, true, MatterFill{{"place"}, {"pocket"}, {"air"}, 293.0, 1.0, {}}}}};
            result.m_worlds.m_entries.push_back({"frontier", std::move(profile)});
            return result;
        }

        void checkRecipeFailure(
            const definition::Definition &source, const config::EConfigErrc code, const std::string_view path) {
            config::Diagnostics diagnostics;
            const auto compiled = definition::compileRecipe(source, {"frontier"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(not compiled and compiled.error() == config::make_error_code(code));
            PPR_TEST_ASSERT(std::ranges::any_of(diagnostics.entries(), [&](const auto &entry) {
                return entry.m_property == path and entry.m_code == compiled.error();
            }));
        }

        template<typename T>
        [[nodiscard]] u64 semanticHash(
            const definition::Definition &context, const T &value,
            const definition::SemanticFingerprintScope scope = {}) {
            definition::SemanticFingerprint fingerprint{context};
            fingerprint.value(value, scope);
            return fingerprint.hash();
        }

        struct SemanticShape {
            std::variant<definition::Rectangle, definition::Ellipse> m_shape{definition::Rectangle{2u, 3u}};
            static constexpr auto describe() {
                return std::tuple{config::taggedVariant(
                    &SemanticShape::m_shape, config::Metadata{"shape"}, config::EPresence::required,
                    config::alternative<definition::Rectangle>("rectangle"),
                    config::alternative<definition::Ellipse>("ellipse"))};
            }
        };

        struct SemanticReorderedShape {
            std::variant<definition::Ellipse, definition::Rectangle> m_shape{definition::Rectangle{2u, 3u}};
            static constexpr auto describe() {
                return std::tuple{config::taggedVariant(
                    &SemanticReorderedShape::m_shape, config::Metadata{"shape"}, config::EPresence::required,
                    config::alternative<definition::Ellipse>("ellipse"),
                    config::alternative<definition::Rectangle>("rectangle"))};
            }
        };

        template<bool Scoped = false, bool Choice = false, bool DifferentCollection = false>
        struct SemanticReferenceProjection {
            definition::MaterialReference m_reference{"rock"};
            static constexpr auto describe() {
                return std::tuple{config::field(
                    &SemanticReferenceProjection::m_reference, config::Metadata{"reference"},
                    config::ReferenceDomain<definition::MaterialReference>{
                        {DifferentCollection ? "features" : "materials", Choice ? "terrain_or_vacuum" : "terrain",
                            Scoped ? config::EReferenceScope::owner : config::EReferenceScope::root}},
                    config::EPresence::required)};
            }
        };

        PPR_UNIT_TEST (semantic_fingerprint_reference_domains_are_semantic) {
            const definition::Definition context;
            const auto root = semanticHash(context, SemanticReferenceProjection<>{});
            PPR_TEST_ASSERT(root != semanticHash(context, SemanticReferenceProjection<true>{}));
            PPR_TEST_ASSERT(root != semanticHash(context, SemanticReferenceProjection<false, true>{}));
            PPR_TEST_ASSERT(root != semanticHash(context, SemanticReferenceProjection<false, false, true>{}));
        };

        PPR_UNIT_TEST (semantic_fingerprint_tags_ignore_variant_storage_indices) {
            const definition::Definition context;
            const SemanticShape first;
            const SemanticReorderedShape second;
            PPR_TEST_ASSERT(first.m_shape.index() != second.m_shape.index());
            PPR_TEST_ASSERT(semanticHash(context, first) == semanticHash(context, second));

            auto ellipse = first;
            ellipse.m_shape = definition::Ellipse{2u, 3u};
            PPR_TEST_ASSERT(semanticHash(context, first) != semanticHash(context, ellipse));
        };

        PPR_UNIT_TEST (semantic_fingerprint_canonical_keys_and_every_described_member) {
            using namespace definition;
            auto source = recipeDefinition();
            auto &biome = source.m_biomes.m_entries.front().m_value;
            biome = completeDefinition().m_biomes.m_entries.front().m_value;
            biome.m_deposits.m_entries.front().m_value.m_blob = DepositBlob{
                1u, {1.0, 2.0}, {1.0, 2.0}, {0.0, 90.0}, 0.1};
            biome.m_lining = Lining{{"rock"}, 1u};
            biome.m_surface_layers.m_soil = Soil{{"rock"}, 1.0};
            biome.m_surface_layers.m_weathered = Weathered{{"rock"}, 2.0};
            biome.m_formation = Formation{{"rock"}, 0.1, 3.0};
            biome.m_rind = Rind{{"rock"}};
            auto &profile = source.m_worlds.m_entries.front().m_value;
            profile.m_surface_bedrock = SurfaceBedrock{{"climate"}, {"climate"}, {"rock"}, 100.0, 4.0, 10.0, 2.0};
            profile.m_start.m_depth_m = 2.0;
            source.m_features.m_entries.front().m_value.m_operations.m_entries = {
                {"paint_rectangle", {0u, 0, 0, Paint{{"rock"}, Rectangle{2u, 3u}}}},
                {"paint_ellipse", {1u, 0, 0, Paint{{"rock"}, Ellipse{2u, 3u}}}},
                {"paint_capsule", {2u, 0, 0, Paint{{"rock"}, Capsule{2, 3, 0.5}}}},
                {"cut", {3u, 0, 0, Carve{Rectangle{2u, 3u}}}}, {"carve_ellipse", {4u, 0, 0, Carve{Ellipse{2u, 3u}}}},
                {"carve_capsule", {5u, 0, 0, Carve{Capsule{2, 3, 0.5}}}}};
            const auto expected = semanticHash(source, source);
            auto reordered = source;
            config::visitRecursive(reordered, [&](const auto &, const auto &, auto &value) {
                if constexpr (requires { value.m_entries; }) {
                    std::ranges::reverse(value.m_entries);
                }
            });
            PPR_TEST_ASSERT(semanticHash(reordered, reordered) == expected);

            Array<std::string> paths;
            const auto collect = [&](const std::string &path, const auto &) { paths.push_back(path); };
            walkFingerprintMembers(source, collect);
            for (const std::string_view path: std::array{
                     "worlds.frontier.rules.liquid.payload.matter_fill.marker",
                     "worlds.frontier.surface_bedrock.bedrock_variation_m",
                     "features.room.operations.paint_rectangle.action.paint.shape.rectangle.width_fine",
                     "features.room.operations.paint_ellipse.action.paint.shape.ellipse.width_fine",
                     "features.room.operations.paint_capsule.action.paint.shape.capsule.radius_m",
                     "features.room.operations.cut.action.carve.shape.rectangle.width_fine",
                     "features.room.operations.carve_ellipse.action.carve.shape.ellipse.width_fine",
                     "features.room.operations.carve_capsule.action.carve.shape.capsule.radius_m"}) {
                PPR_TEST_ASSERT(std::ranges::find(paths, path) != paths.end());
            }
            for (const auto &path: paths) {
                auto changed = source;
                bool found = false;
                const auto mutate = [&](const std::string &candidate, auto &value) {
                    if (candidate == path) {
                        flipFingerprintMember(value);
                        found = true;
                    }
                };
                walkFingerprintMembers(changed, mutate);
                PPR_TEST_ASSERT(found and semanticHash(changed, changed) != expected);
            }
        };

        PPR_UNIT_TEST (semantic_fingerprint_optional_presence_zero_and_primitive_framing) {
            auto source = recipeDefinition();
            const auto &profile = source.m_worlds.m_entries.front().m_value;
            auto start = profile.m_start;
            const auto absent = semanticHash(source, start, {"frontier"});
            start.m_depth_m = 0.0;
            const auto present = semanticHash(source, start, {"frontier"});
            PPR_TEST_ASSERT(absent != present);
            start.m_depth_m = -0.0;
            PPR_TEST_ASSERT(semanticHash(source, start, {"frontier"}) == present);

            definition::SemanticFingerprint first{source}, second{source};
            first.string("ab");
            first.string("c");
            second.string("a");
            second.string("bc");
            PPR_TEST_ASSERT(first.hash() != second.hash());
            definition::SemanticFingerprint number{source}, bytes{source};
            number.integer(u32{0x04030201u});
            for (const u8 byte: std::array<u8, 4>{1u, 2u, 3u, 4u}) {
                bytes.integer(byte);
            }
            PPR_TEST_ASSERT(number.hash() == bytes.hash());
        };

        PPR_UNIT_TEST (semantic_fingerprint_scoped_reference_owners_follow_authored_context) {
            auto source = recipeDefinition();
            auto &profile = source.m_worlds.m_entries.front().m_value;
            const auto start = profile.m_start;
            const auto resource = profile.m_resources.m_entries.front().m_value;
            const auto fill = std::get<definition::MatterFill>(profile.m_rules.m_entries.back().m_value.m_payload);
            const auto start_hash = semanticHash(source, start, {"frontier"});
            const auto resource_hash = semanticHash(source, resource, {"frontier"});
            const auto fill_hash = semanticHash(source, fill, {"frontier", "gas"});

            profile.m_start.m_feature.m_key = "missing_feature";
            PPR_TEST_ASSERT(semanticHash(source, start, {"frontier"}) != start_hash);
            PPR_TEST_ASSERT(semanticHash(source, resource, {"frontier"}) != resource_hash);
            profile.m_start.m_feature.m_key = "another_missing_feature";
            const auto missing = semanticHash(source, start, {"frontier"});
            profile.m_start.m_feature.m_key = "missing_feature";
            PPR_TEST_ASSERT(semanticHash(source, start, {"frontier"}) != missing);

            auto &placement = std::get<definition::Feature>(profile.m_rules.m_entries[4].m_value.m_payload);
            placement.m_feature.m_key = "missing_feature";
            const auto missing_fill = semanticHash(source, fill, {"frontier", "gas"});
            PPR_TEST_ASSERT(missing_fill != fill_hash);
            placement.m_feature.m_key.clear();
            const auto empty_owner = semanticHash(source, fill, {"frontier", "gas"});
            profile.m_rules.m_entries[4].m_value.m_payload = definition::Connection{};
            const auto absent_owner = semanticHash(source, fill, {"frontier", "gas"});
            PPR_TEST_ASSERT(absent_owner != empty_owner and absent_owner != missing_fill);

            auto second = recipeDefinition().m_worlds.m_entries.front();
            second.m_key = "hollows";
            source.m_worlds.m_entries.push_back(second);
            PPR_TEST_ASSERT(semanticHash(source, fill, {"hollows", "gas"}) != fill_hash);
            PPR_TEST_ASSERT(semanticHash(source, start, {"hollows"}) == start_hash);
        };

        PPR_UNIT_TEST (definition_rules_fingerprint_projects_all_profiles_canonically) {
            auto source = recipeDefinition();
            auto empty = source.m_worlds.m_entries.front();
            empty.m_key = "empty";
            empty.m_value.m_rules.m_entries.clear();
            const auto without_empty = definition::definitionRulesFingerprint(source);
            source.m_worlds.m_entries.push_back(empty);
            const auto expected = definition::definitionRulesFingerprint(source);
            PPR_TEST_ASSERT(expected != without_empty);

            auto reordered = source;
            std::ranges::reverse(reordered.m_worlds.m_entries);
            for (auto &profile: reordered.m_worlds.m_entries) {
                std::ranges::reverse(profile.m_value.m_rules.m_entries);
            }
            PPR_TEST_ASSERT(definition::definitionRulesFingerprint(reordered) == expected);
            reordered.m_worlds.m_entries.front().m_key = "renamed_empty";
            PPR_TEST_ASSERT(definition::definitionRulesFingerprint(reordered) != expected);

            auto changed = source;
            changed.m_worlds.m_entries.front().m_value.m_layout.m_min_sites += 1u;
            changed.m_materials.m_entries.front().m_value.m_density += 1.0;
            PPR_TEST_ASSERT(definition::definitionRulesFingerprint(changed) == expected);
            changed.m_worlds.m_entries.front().m_value.m_rules.m_entries.front().m_value.m_enabled = false;
            PPR_TEST_ASSERT(definition::definitionRulesFingerprint(changed) != expected);
        };

        PPR_UNIT_TEST (semantic_fingerprint_stream_boundaries_and_non_destructive_reads) {
            const definition::Definition context;
            definition::SemanticFingerprint words{context}, bytes{context};
            for (u32 i = 0u; i < 2049u; ++i) {
                const u32 word = i * 0x01020305u;
                words.integer(word);
                for (u32 shift = 0u; shift < 32u; shift += 8u) {
                    bytes.integer(static_cast<u8>(word >> shift));
                }

                PPR_TEST_ASSERT(words.hash() == bytes.hash());
                PPR_TEST_ASSERT(words.hash() == words.hash());
            }

            const std::string long_value(4097u, 'x');
            words.string(long_value);
            bytes.string(long_value);
            PPR_TEST_ASSERT(words.hash() == bytes.hash());

            definition::SemanticFingerprint uninterrupted{context};
            for (u32 i = 0u; i < 2049u; ++i) {
                uninterrupted.integer(i * 0x01020305u);
            }

            uninterrupted.string(long_value);
            PPR_TEST_ASSERT(words.hash() == uninterrupted.hash());
            PPR_TEST_ASSERT(semanticHash(context, u32{1u}) != semanticHash(context, u64{1u}));
            PPR_TEST_ASSERT(semanticHash(context, i32{1}) != semanticHash(context, u32{1u}));
        };

        PPR_UNIT_TEST (semantic_fingerprint_explicit_scope_presence_and_subtree_isolation) {
            auto context = recipeDefinition();
            const auto separate = context;
            const auto &profile = separate.m_worlds.m_entries.front().m_value;
            const auto &fill = std::get<definition::MatterFill>(profile.m_rules.m_entries.back().m_value.m_payload);
            const auto original = semanticHash(context, separate);
            context.m_worlds.m_entries.front().m_value.m_start.m_feature.m_key = "context_only";
            PPR_TEST_ASSERT(semanticHash(context, separate) != original);
            PPR_TEST_ASSERT(
                semanticHash(context, profile, {"frontier"}) != semanticHash(separate, profile, {"frontier"}));

            const auto absent = semanticHash(context, fill);
            const auto empty = semanticHash(context, fill, {std::string_view{}, "gas"});
            const auto missing = semanticHash(context, fill, {"missing", "gas"});
            PPR_TEST_ASSERT(absent != empty and absent != missing and empty != missing);

            definition::SemanticFingerprint first{context}, second{context};
            first.value(fill, {"frontier", "gas"});
            first.value(profile.m_start, {"missing"});
            first.value(fill);
            second.value(fill, {"frontier", "gas"});
            second.value(profile.m_start, {"missing"});
            second.value(fill, {});
            PPR_TEST_ASSERT(first.hash() == second.hash());

            definition::SemanticFingerprint unscoped{context}, after_scope{context};
            unscoped.value(u32{1u});
            unscoped.value(fill);
            after_scope.value(u32{1u}, {"frontier", "gas"});
            after_scope.value(fill);
            PPR_TEST_ASSERT(unscoped.hash() == after_scope.hash());
        };

        PPR_UNIT_TEST (semantic_fingerprint_nested_scratch_and_duplicate_key_order) {
            auto source = recipeDefinition();
            const auto prototype = source.m_worlds.m_entries.front();
            source.m_worlds.m_entries.clear();
            for (u32 i = 0u; i < 40u; ++i) {
                auto profile = prototype;
                profile.m_key = std::string(128u, 'w') + std::to_string(i);
                for (u32 j = 0u; j < 80u; ++j) {
                    auto rule = prototype.m_value.m_rules.m_entries.front();
                    rule.m_key = std::string(129u, 'r') + std::to_string(j);
                    profile.m_value.m_rules.m_entries.push_back(std::move(rule));
                }

                source.m_worlds.m_entries.push_back(std::move(profile));
            }

            const auto expected = semanticHash(source, source);
            auto reordered = source;
            std::ranges::reverse(reordered.m_worlds.m_entries);
            for (auto &profile: reordered.m_worlds.m_entries) {
                std::ranges::reverse(profile.m_value.m_rules.m_entries);
            }

            PPR_TEST_ASSERT(semanticHash(reordered, reordered) == expected);
            PPR_TEST_ASSERT(
                definition::definitionRulesFingerprint(reordered) == definition::definitionRulesFingerprint(source));

            auto duplicate = minimalDefinition();
            auto material = duplicate.m_materials.m_entries.front();
            material.m_value.m_density += 1.0;
            duplicate.m_materials.m_entries.push_back(material);
            const auto first = semanticHash(duplicate, duplicate);
            std::ranges::reverse(duplicate.m_materials.m_entries);
            PPR_TEST_ASSERT(semanticHash(duplicate, duplicate) != first);
        };

        PPR_UNIT_TEST (numeric_plan_compiles_sparse_material_keys_and_density_once) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({65000u, true, true, EGenerationMatter::solid, 7500.0});
            recipe.m_materials.push_back({40000u, true, true, EGenerationMatter::solid, 1000.0, 900u});
            recipe.m_materials.push_back({33000u, true, true, EGenerationMatter::solid, 1250.0});
            recipe.m_materials.push_back({32000u, true, true, EGenerationMatter::solid, 1250.0});
            auto &biome = recipe.m_biomes.front();
            biome.m_bands.front().m_choices.push_back({0u, 65000u, 1.0});
            biome.m_bands.front().m_choices.push_back({204u, 40000u, 1.0});
            biome.m_deposits.push_back(
                {.m_key = 205u,
                    .m_material = 65000u,
                    .m_host = 17u,
                    .m_depth_max = 1.0,
                    .m_threshold_min = -1.0,
                    .m_threshold_max = 1.0});
            biome.m_deposits.push_back(
                {.m_key = 206u,
                    .m_order = 1u,
                    .m_material = 33000u,
                    .m_host = 17u,
                    .m_depth_max = 1.0,
                    .m_threshold_min = -1.0,
                    .m_threshold_max = 1.0});
            auto later = biome;
            later.m_key = 207u;
            later.m_bands.front().m_choices.front().m_key = 208u;
            later.m_bands.front().m_choices.push_back({209u, 33000u, 1.0});
            recipe.m_biomes.push_back(std::move(later));

            auto plan = compileGenerationPlan(std::move(recipe), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto lookup = plan->materialLookup();
            PPR_TEST_ASSERT(lookup.size() == 65001u);
            PPR_TEST_ASSERT(lookup[0u].m_key == 0x76616375756dull and lookup[0u].m_density_score == 0.0);
            PPR_TEST_ASSERT(lookup[17u].m_key == 202u and lookup[17u].m_density_score == 0.5);
            PPR_TEST_ASSERT(lookup[65000u].m_key == 0u and lookup[65000u].m_density_score == 1.0);
            PPR_TEST_ASSERT(lookup[40000u].m_key == 900u and lookup[40000u].m_density_score == 0.2);
            PPR_TEST_ASSERT(lookup[33000u].m_key == 206u and lookup[33000u].m_density_score == 0.25);
            PPR_TEST_ASSERT(lookup[32000u].m_key == 0u and lookup[32000u].m_density_score == 0.25);

            PPR_TEST_ASSERT(
                sizeof(GenerationMaterialLookup) == 16u and alignof(GenerationMaterialLookup) == alignof(u64));
            const GenerationMaterialLookup layout{};
            const auto *layout_bytes = reinterpret_cast<const char *>(&layout);
            PPR_TEST_ASSERT(reinterpret_cast<const char *>(&layout.m_key) == layout_bytes);
            PPR_TEST_ASSERT(reinterpret_cast<const char *>(&layout.m_density_score) == layout_bytes + sizeof(u64));

            const u64 retained = plan->retainedPayloadBytes();
            auto moved = std::move(*plan);
            PPR_TEST_ASSERT(moved.materialLookup()[65000u].m_density_score == 1.0);
            PPR_TEST_ASSERT(moved.retainedPayloadBytes() == retained and retained >= lookup.size_bytes());
        };

        PPR_UNIT_TEST (definition_recipe_all_payloads_and_derived_phases) {
            using namespace definition;
            static_assert(phaseOf(Cave{}) == EGenerationPhase::carve);
            static_assert(phaseOf(CarveWorm{}) == EGenerationPhase::carve);
            static_assert(phaseOf(VeinWorm{}) == EGenerationPhase::deposit);
            static_assert(phaseOf(Connection{}) == EGenerationPhase::carve);
            static_assert(phaseOf(Feature{}) == EGenerationPhase::stamp);
            static_assert(phaseOf(MatterFill{}) == EGenerationPhase::fill);
            static_assert(phaseOf(RuleVariant{Connection{}}) == EGenerationPhase::carve);
            config::Diagnostics diagnostics;
            const auto compiled = compileRecipe(recipeDefinition(), {"frontier"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(compiled.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(not validateGenerationRecipe(compiled->m_recipe));
            const auto &rules = compiled->m_recipe.m_rules;
            PPR_TEST_ASSERT(rules.size() == 7u);
            PPR_TEST_ASSERT(
                rules[0].m_phase == EGenerationPhase::deposit and
                not std::get<GenerationWorm>(rules[0].m_parameters).m_carve);
            PPR_TEST_ASSERT(
                rules[1].m_phase == EGenerationPhase::carve and
                std::holds_alternative<GenerationCave>(rules[1].m_parameters));
            PPR_TEST_ASSERT(std::get<GenerationWorm>(rules[2].m_parameters).m_carve);
            PPR_TEST_ASSERT(std::holds_alternative<GenerationConnection>(rules[3].m_parameters));
            PPR_TEST_ASSERT(
                rules[4].m_phase == EGenerationPhase::stamp and
                std::get<GenerationPlacement>(rules[4].m_parameters).m_reflect);
            const auto &liquid = std::get<GenerationFill>(rules[5].m_parameters);
            const auto &gas = std::get<GenerationFill>(rules[6].m_parameters);
            PPR_TEST_ASSERT(
                rules[5].m_phase == EGenerationPhase::fill and not liquid.m_gas and liquid.m_fill_height_m == 5.0);
            PPR_TEST_ASSERT(gas.m_gas and not gas.m_fill_height_m and gas.m_placement_rule == 4u);
            PPR_TEST_ASSERT(compiled->m_recipe.m_resources.size() == 1u);
            for (const auto &binding: compiled->m_bindings.m_rules) {
                PPR_TEST_ASSERT(binding.m_profile == "frontier");
            }
        };

        PPR_UNIT_TEST (definition_recipe_particle_fill_accepts_every_phase) {
            using namespace definition;
            for (const auto phase: {EGenerationMatter::solid, EGenerationMatter::liquid, EGenerationMatter::gas}) {
                auto source = recipeDefinition();
                source.m_materials.m_entries.push_back(
                    {"fill", {phase, EMaterialRepresentation::particle, {}, 1000.0}});
                auto &fill = std::get<MatterFill>(
                    source.m_worlds.m_entries.front().m_value.m_rules.m_entries[5].m_value.m_payload);
                fill.m_material.m_key = "fill";
                const bool gas = phase == EGenerationMatter::gas;
                if (gas) {
                    fill.m_fill_height_m.reset();
                }

                config::Diagnostics diagnostics;
                const auto compiled = compileRecipe(source, {"frontier"}, recipeParameters(), diagnostics);
                PPR_TEST_ASSERT(compiled.has_value() and not diagnostics.hasErrors());
                PPR_TEST_ASSERT(not validateGenerationRecipe(compiled->m_recipe));
                const auto &numeric = std::get<GenerationFill>(compiled->m_recipe.m_rules[5].m_parameters);
                PPR_TEST_ASSERT(numeric.m_gas == gas and numeric.m_fill_height_m == fill.m_fill_height_m);

                if (phase == EGenerationMatter::solid) {
                    fill.m_fill_height_m.reset();
                    checkRecipeFailure(
                        source, config::EConfigErrc::missing_value,
                        "worlds.frontier.rules.liquid.payload.matter_fill.fill_height_m");
                    fill.m_fill_height_m = 5.0;
                    source.m_materials.m_entries.back().m_value.m_representation = EMaterialRepresentation::terrain;
                    source.m_materials.m_entries.back().m_value.m_diggable = true;
                    checkRecipeFailure(
                        source, config::EConfigErrc::cross_field_invalid,
                        "worlds.frontier.rules.liquid.payload.matter_fill.material");
                }
            }
        };

        PPR_UNIT_TEST (definition_recipe_fill_phase_height_and_suitability) {
            using namespace definition;
            auto source = recipeDefinition();
            auto &rules = source.m_worlds.m_entries.front().m_value.m_rules;
            auto &gas = std::get<MatterFill>(rules.m_entries.back().m_value.m_payload);
            gas.m_fill_height_m = 5.0;
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid,
                "worlds.frontier.rules.gas.payload.matter_fill.fill_height_m");
            gas.m_fill_height_m.reset();
            auto &liquid = std::get<MatterFill>(rules.m_entries[5].m_value.m_payload);
            liquid.m_fill_height_m.reset();
            checkRecipeFailure(
                source, config::EConfigErrc::missing_value,
                "worlds.frontier.rules.liquid.payload.matter_fill.fill_height_m");
            liquid.m_fill_height_m = 5.0;
            liquid.m_material.m_key = "rock";
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid,
                "worlds.frontier.rules.liquid.payload.matter_fill.material");
            liquid.m_material.m_key = "water";
            liquid.m_fill_height_m = 5.1;
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid,
                "worlds.frontier.rules.liquid.payload.matter_fill.fill_height_m");
        };

        PPR_UNIT_TEST (definition_recipe_scoped_markers_and_owner_first) {
            using namespace definition;
            auto source = recipeDefinition();
            auto &profile = source.m_worlds.m_entries.front().m_value;
            for (const std::string_view wrong: {"resource", "room", "inert", "pocket"}) {
                profile.m_start.m_marker.m_key = wrong;
                checkRecipeFailure(source, config::EConfigErrc::cross_field_invalid, "worlds.frontier.start.marker");
            }
            profile.m_start.m_marker.m_key = "start";
            auto &fill = std::get<MatterFill>(profile.m_rules.m_entries.back().m_value.m_payload);
            for (const std::string_view wrong: {"start", "resource", "room", "inert"}) {
                fill.m_marker.m_key = wrong;
                checkRecipeFailure(
                    source, config::EConfigErrc::cross_field_invalid,
                    "worlds.frontier.rules.gas.payload.matter_fill.marker");
            }
            fill.m_marker.m_key = "foreign";
            source.m_features.m_entries[1].m_value.m_markers.m_entries.push_back(
                {"foreign", {EGenerationMarker::sealed_pocket, 5, 5}});
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid,
                "worlds.frontier.rules.gas.payload.matter_fill.marker");
            fill.m_marker.m_key = "pocket";
            profile.m_start.m_feature.m_key = "missing";
            config::Diagnostics diagnostics;
            const auto missing_owner = compileRecipe(source, {"frontier"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(not missing_owner);
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "worlds.frontier.start.feature");
            profile.m_start.m_feature.m_key = "other";
            diagnostics.clear();
            const auto scoped = compileRecipe(source, {"frontier"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(scoped.has_value());
            const auto &start_feature = scoped->m_recipe.m_features[scoped->m_recipe.m_start_feature];
            PPR_TEST_ASSERT(start_feature.m_markers[scoped->m_recipe.m_start_marker].m_x == 8);
            for (const auto &feature: scoped->m_bindings.m_features) {
                for (const auto &marker: feature.m_markers) {
                    PPR_TEST_ASSERT(marker.m_feature == feature.m_key);
                }
            }
            auto &resource = profile.m_resources.m_entries.front().m_value;
            resource.m_marker.m_key = "pocket";
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid, "worlds.frontier.resources.stone.marker");
        };

        PPR_UNIT_TEST (definition_recipe_scoped_rules_disabled_placement_and_active_biomes) {
            using namespace definition;
            auto source = recipeDefinition();
            auto &profile = source.m_worlds.m_entries.front().m_value;
            profile.m_rules.m_entries[4].m_value.m_enabled = false;
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid,
                "worlds.frontier.rules.gas.payload.matter_fill.placement");
            profile.m_rules.m_entries[4].m_value.m_enabled = true;
            auto &gas = std::get<MatterFill>(profile.m_rules.m_entries.back().m_value.m_payload);
            gas.m_placement.m_key = "cave";
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid,
                "worlds.frontier.rules.gas.payload.matter_fill.placement");
            gas.m_placement.m_key = "foreign";
            auto other = profile;
            other.m_rules.m_entries[4].m_key = "foreign";
            std::get<MatterFill>(other.m_rules.m_entries[5].m_value.m_payload).m_placement.m_key = "foreign";
            source.m_worlds.m_entries.push_back({"other", std::move(other)});
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid,
                "worlds.frontier.rules.gas.payload.matter_fill.placement");

            source = recipeDefinition();
            source.m_biomes.m_entries.push_back({"unused", source.m_biomes.m_entries.front().m_value});
            std::get<Cave>(source.m_worlds.m_entries.front().m_value.m_rules.m_entries.front().m_value.m_payload)
                .m_biome.m_key = "unused";
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid, "worlds.frontier.rules.cave.payload.cave.biome");
            std::get<Cave>(source.m_worlds.m_entries.front().m_value.m_rules.m_entries.front().m_value.m_payload)
                .m_biome.m_key = "plain";
            std::get<Feature>(source.m_worlds.m_entries.front().m_value.m_rules.m_entries[4].m_value.m_payload)
                .m_biome.m_key = "unused";
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid, "worlds.frontier.rules.place.payload.feature.biome");
        };

        PPR_UNIT_TEST (definition_recipe_profile_owned_rule_bindings_and_reordering) {
            using namespace definition;
            auto source = recipeDefinition();
            source.m_worlds.m_entries.push_back({"second", source.m_worlds.m_entries.front().m_value});
            std::get<Feature>(source.m_worlds.m_entries.back().m_value.m_rules.m_entries[4].m_value.m_payload)
                .m_feature.m_key = "other";
            config::Diagnostics diagnostics;
            const auto first = compileRecipe(source, {"frontier"}, recipeParameters(), diagnostics);
            const auto second = compileRecipe(source, {"second"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(first.has_value() and second.has_value());
            PPR_TEST_ASSERT(first->m_bindings.m_rules.front().m_profile == "frontier");
            PPR_TEST_ASSERT(second->m_bindings.m_rules.front().m_profile == "second");
            PPR_TEST_ASSERT(first->m_recipe.m_rules.front().m_key != second->m_recipe.m_rules.front().m_key);
            std::ranges::reverse(source.m_worlds.m_entries);
            std::ranges::reverse(source.m_features.m_entries);
            std::ranges::reverse(source.m_materials.m_entries);
            for (auto &profile: source.m_worlds.m_entries) {
                std::ranges::reverse(profile.m_value.m_rules.m_entries);
                std::ranges::reverse(profile.m_value.m_biomes.m_entries);
                std::ranges::reverse(profile.m_value.m_resources.m_entries);
            }
            for (auto &feature: source.m_features.m_entries) {
                std::ranges::reverse(feature.m_value.m_markers.m_entries);
            }
            const auto reordered = compileRecipe(source, {"frontier"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(reordered.has_value() and not diagnostics.hasErrors());
            for (std::size_t i = 0u; i < first->m_recipe.m_rules.size(); ++i) {
                PPR_TEST_ASSERT(first->m_recipe.m_rules[i].m_key == reordered->m_recipe.m_rules[i].m_key);
                PPR_TEST_ASSERT(first->m_recipe.m_rules[i].m_phase == reordered->m_recipe.m_rules[i].m_phase);
                PPR_TEST_ASSERT(first->m_recipe.m_rules[i].m_order == reordered->m_recipe.m_rules[i].m_order);
                PPR_TEST_ASSERT(first->m_bindings.m_rules[i].m_key == reordered->m_bindings.m_rules[i].m_key);
                PPR_TEST_ASSERT(first->m_bindings.m_rules[i].m_index == reordered->m_bindings.m_rules[i].m_index);
            }
            const auto &fill = std::get<GenerationFill>(reordered->m_recipe.m_rules.back().m_parameters);
            PPR_TEST_ASSERT(
                fill.m_placement_rule ==
                std::get<GenerationFill>(first->m_recipe.m_rules.back().m_parameters).m_placement_rule);
            PPR_TEST_ASSERT(
                fill.m_marker == std::get<GenerationFill>(first->m_recipe.m_rules.back().m_parameters).m_marker);
        };

        PPR_UNIT_TEST (definition_recipe_numeric_boundary_and_missing_dependencies) {
            using namespace definition;
            auto source = recipeDefinition();
            auto &profile = source.m_worlds.m_entries.front().m_value;
            profile.m_layout.m_site_spacing_m = 2000.0;
            checkRecipeFailure(source, config::EConfigErrc::cross_field_invalid, "worlds.frontier");
            profile.m_layout.m_site_spacing_m = 100.0;
            profile.m_rules.m_entries[1].m_value.m_order = 0u;
            checkRecipeFailure(source, config::EConfigErrc::duplicate_key, "worlds.frontier.rules.cave.order");
            profile.m_rules.m_entries[1].m_value.m_order = 1u;
            profile.m_boundaries.m_warp_x_field.m_key = "missing";
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid, "worlds.frontier.boundaries.warp_x_field");
            profile.m_boundaries.m_warp_x_field.m_key = "climate";
            source.m_features.m_entries.front().m_value.m_allow_horizontal_reflection = false;
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid,
                "worlds.frontier.rules.place.payload.feature.reflect");
            source.m_features.m_entries.front().m_value.m_allow_horizontal_reflection = true;
            auto parameters = recipeParameters();
            parameters.m_reference_thickness_m = 0.0;
            profile.m_start.m_feature.m_key = "missing";
            config::Diagnostics diagnostics;
            const auto invalid_parameters = compileRecipe(source, {"absent"}, parameters, diagnostics);
            PPR_TEST_ASSERT(
                not invalid_parameters and
                invalid_parameters.error() == config::make_error_code(config::EConfigErrc::out_of_range));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "reference_thickness_m");
            diagnostics.clear();
            const auto missing_profile = compileRecipe(source, {"absent"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(not missing_profile and diagnostics.entries().front().m_property == "worlds.absent");
        };

        PPR_UNIT_TEST (definition_recipe_optional_surface_bedrock_and_owned_values) {
            using namespace definition;
            auto source = recipeDefinition();
            source.m_materials.m_entries.push_back(
                {"bedrock", {EGenerationMatter::solid, EMaterialRepresentation::terrain, false, 3000.0}});
            auto &profile = source.m_worlds.m_entries.front().m_value;
            profile.m_surface_bedrock = SurfaceBedrock{{"climate"}, {"climate"}, {"bedrock"}, 1000.0, 10.0, 20.0, 2.0};
            profile.m_start.m_depth_m = 50.0;
            config::Diagnostics diagnostics;
            const auto compiled = compileRecipe(source, {"frontier"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(compiled.has_value() and not diagnostics.hasErrors());
            source = {};
            PPR_TEST_ASSERT(compiled->m_recipe.m_open_surface and compiled->m_recipe.m_start_depth_m == 50.0);
            PPR_TEST_ASSERT(compiled->m_recipe.m_bedrock_thickness_m == 20.0);
            PPR_TEST_ASSERT(compiled->m_bindings.m_rules.front().m_profile == "frontier");
            source = recipeDefinition();
            source.m_worlds.m_entries.front().m_value.m_surface_bedrock = SurfaceBedrock{
                {"missing"}, {"climate"}, {"rock"}, 1000.0, 10.0, 20.0, 2.0};
            checkRecipeFailure(
                source, config::EConfigErrc::cross_field_invalid, "worlds.frontier.surface_bedrock.surface_field");
        };

        [[nodiscard]] std::string profileText() {
            return featureText("carve", "rectangle") + R"toml(
[features.room.markers.start]
role='start'
x_fine=1
y_fine=1
[worlds.frontier.layout]
min_sites=4
max_sites=16
site_attempts=30
site_spacing_m=100.0
[worlds.frontier.boundaries]
material='rock'
thickness_fine=3
warp_x_field='climate'
warp_y_field='climate'
[worlds.frontier.start]
biome='plain'
feature='room'
marker='start'
x_fine=2000
y_fine=3000
clearance_m=3.0
route_width_m=1.0
[worlds.frontier.biomes.plain]
biome='plain'
required_count=1
weight=1.0
distance_min_m=0.0
distance_max_m=2048.0
[worlds.frontier.biomes.plain.depth]
min=0.0
max=1.0
[worlds.frontier.rules.edit]
order=0
[worlds.frontier.rules.edit.payload]
tag='cave'
[worlds.frontier.rules.edit.payload.cave]
biome='plain'
field='missing'
[worlds.frontier.rules.edit.payload.cave.depth]
min=0.0
max=1.0
[worlds.frontier.rules.edit.payload.cave.threshold]
min=-0.5
max=0.5
)toml";
        }

        PPR_UNIT_TEST (definition_recipe_generic_resolution_variant_refs_and_atomic_replacement) {
            using namespace definition;
            config::Diagnostics diagnostics;
            const auto base_document = config::decode(
                profileText(), "base.toml", diagnostics, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(base_document.has_value());
            Array<config::TreeLayer> layers{config::toTreeLayer(*base_document, "base.toml")};
            const auto resolved = config::resolveRecursive<Definition>(layers, diagnostics);
            PPR_TEST_ASSERT(resolved.has_value() and not diagnostics.hasErrors());
            const auto &rule = resolved->m_value.m_worlds.m_entries.front().m_value.m_rules.m_entries.front().m_value;
            PPR_TEST_ASSERT(rule.m_enabled and std::holds_alternative<Cave>(rule.m_payload));
            const auto invalid_ref = compileRecipe(resolved->m_value, {"frontier"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(
                not invalid_ref and
                invalid_ref.error() == config::make_error_code(config::EConfigErrc::cross_field_invalid));
            PPR_TEST_ASSERT(
                diagnostics.entries().front().m_property == "worlds.frontier.rules.edit.payload.cave.field");
            diagnostics.clear();
            const auto replacement_document = config::decode(
                R"toml(
[worlds.frontier.rules.edit.payload]
tag='connection'
[worlds.frontier.rules.edit.payload.connection]
radius_m=1.0
)toml",
                "replace.toml", diagnostics, config::EDecodeMode::tree);
            PPR_TEST_ASSERT(replacement_document.has_value());
            layers.push_back(config::toTreeLayer(*replacement_document, "replace.toml"));
            const auto replaced = config::resolveRecursive<Definition>(layers, diagnostics);
            PPR_TEST_ASSERT(replaced.has_value() and not diagnostics.hasErrors());
            const auto &replacement =
                replaced->m_value.m_worlds.m_entries.front().m_value.m_rules.m_entries.front().m_value;
            PPR_TEST_ASSERT(std::holds_alternative<Connection>(replacement.m_payload));
            PPR_TEST_ASSERT(replaced->find("worlds.frontier.rules.edit.payload.cave.field") == nullptr);
            const auto compiled = compileRecipe(replaced->m_value, {"frontier"}, recipeParameters(), diagnostics);
            PPR_TEST_ASSERT(compiled.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(
                std::holds_alternative<GenerationConnection>(compiled->m_recipe.m_rules.front().m_parameters));
            auto missing_text = profileText();
            const auto offset = missing_text.find("field='missing'\n");
            missing_text.erase(offset, std::string_view{"field='missing'\n"}.size());
            diagnostics.clear();
            const auto missing = resolveFeatureText(missing_text, diagnostics);
            PPR_TEST_ASSERT(
                not missing and missing.error() == config::make_error_code(config::EConfigErrc::missing_value));
            PPR_TEST_ASSERT(std::ranges::any_of(diagnostics.entries(), [](const auto &entry) {
                return entry.m_property == "worlds.frontier.rules.edit.payload.cave.field";
            }));
        };

        void checkFeatureCombination(const std::string_view action, const std::string_view shape) {
            config::Diagnostics diagnostics;
            const auto resolved = resolveFeatureText(featureText(action, shape), diagnostics);
            PPR_TEST_ASSERT(resolved.has_value() and not diagnostics.hasErrors());
            const auto compiled = definition::compileDefinition(resolved->m_value, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(compiled.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(compiled->m_features.size() == 1u);
            const auto &feature = compiled->m_features.front();
            PPR_TEST_ASSERT(feature.m_bounds.m_min_x == -3 and feature.m_bounds.m_min_y == -3);
            PPR_TEST_ASSERT(feature.m_bounds.m_max_x == 12 and feature.m_bounds.m_max_y == 12);
            const auto &operation = feature.m_operations.front();
            PPR_TEST_ASSERT(operation.m_order == 7u and operation.m_x == 0 and operation.m_y == 0);
            PPR_TEST_ASSERT(operation.m_carve == (action == "carve"));
            PPR_TEST_ASSERT(operation.m_material == (action == "carve" ? 0u : 1u));
            if (shape == "capsule") {
                PPR_TEST_ASSERT(operation.m_shape == EGenerationShape::capsule);
                PPR_TEST_ASSERT(operation.m_end_x == 6 and operation.m_end_y == 3 and operation.m_radius_m == 1.0);
                PPR_TEST_ASSERT(operation.m_width == 0u and operation.m_height == 0u);
            } else {
                PPR_TEST_ASSERT(
                    operation.m_shape ==
                    (shape == "rectangle" ? EGenerationShape::rectangle : EGenerationShape::ellipse));
                PPR_TEST_ASSERT(operation.m_width == 6u and operation.m_height == 9u);
                PPR_TEST_ASSERT(operation.m_end_x == 0 and operation.m_end_y == 0 and operation.m_radius_m == 0.0);
            }
        }

        PPR_UNIT_TEST (definition_feature_paint_rectangle) {
            checkFeatureCombination("paint", "rectangle");
        };
        PPR_UNIT_TEST (definition_feature_paint_ellipse) {
            checkFeatureCombination("paint", "ellipse");
        };
        PPR_UNIT_TEST (definition_feature_paint_capsule) {
            checkFeatureCombination("paint", "capsule");
        };
        PPR_UNIT_TEST (definition_feature_carve_rectangle) {
            checkFeatureCombination("carve", "rectangle");
        };
        PPR_UNIT_TEST (definition_feature_carve_ellipse) {
            checkFeatureCombination("carve", "ellipse");
        };
        PPR_UNIT_TEST (definition_feature_carve_capsule) {
            checkFeatureCombination("carve", "capsule");
        };

        PPR_UNIT_TEST (definition_feature_reflection_required_and_numeric) {
            for (const bool allow: {false, true}) {
                config::Diagnostics diagnostics;
                const auto resolved = resolveFeatureText(featureText("carve", "rectangle", allow), diagnostics);
                PPR_TEST_ASSERT(resolved.has_value());
                const auto compiled = definition::compileDefinition(resolved->m_value, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(compiled.has_value() and compiled->m_features.front().m_allow_reflection == allow);
                auto changed = resolved->m_value;
                changed.m_features.m_entries.front().m_value.m_allow_horizontal_reflection = not allow;
                PPR_TEST_ASSERT(
                    definition::definitionFingerprint(changed) != definition::definitionFingerprint(resolved->m_value));
            }
            auto text = featureText("carve", "rectangle");
            const auto offset = text.find("allow_horizontal_reflection=false\n");
            text.erase(offset, std::string_view{"allow_horizontal_reflection=false\n"}.size());
            config::Diagnostics diagnostics;
            const auto missing = resolveFeatureText(text, diagnostics);
            PPR_TEST_ASSERT(
                not missing and missing.error() == config::make_error_code(config::EConfigErrc::missing_value));
            PPR_TEST_ASSERT(std::ranges::any_of(diagnostics.entries(), [](const auto &entry) {
                return entry.m_property == "features.room.allow_horizontal_reflection";
            }));
        };

        PPR_UNIT_TEST (definition_feature_paint_requires_terrain_carve_has_no_material) {
            using namespace definition;
            config::Diagnostics diagnostics;
            auto text = featureText("paint", "rectangle");
            const auto offset = text.rfind("material='rock'\n");
            text.erase(offset, std::string_view{"material='rock'\n"}.size());
            const auto missing = resolveFeatureText(text, diagnostics);
            PPR_TEST_ASSERT(
                not missing and missing.error() == config::make_error_code(config::EConfigErrc::missing_value));
            PPR_TEST_ASSERT(std::ranges::any_of(diagnostics.entries(), [](const auto &entry) {
                return entry.m_property == "features.room.operations.cut.action.paint.material";
            }));

            diagnostics.clear();
            auto resolved = resolveFeatureText(featureText("paint", "rectangle"), diagnostics);
            PPR_TEST_ASSERT(resolved.has_value());
            auto &source = resolved->m_value;
            source.m_materials.m_entries.push_back(
                {"dust", {EGenerationMatter::solid, EMaterialRepresentation::particle, {}, 1000.0}});
            auto &operation = source.m_features.m_entries.front().m_value.m_operations.m_entries.front().m_value;
            for (const std::string_view key: {"dust", "vacuum", "missing"}) {
                std::get<Paint>(operation.m_action).m_material.m_key = key;
                diagnostics.clear();
                const auto invalid = compileDefinition(source, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(
                    not invalid and
                    invalid.error() == config::make_error_code(config::EConfigErrc::cross_field_invalid));
                PPR_TEST_ASSERT(
                    diagnostics.entries().front().m_property == "features.room.operations.cut.action.paint.material");
            }
            operation.m_action = Carve{Rectangle{6u, 9u}};
            diagnostics.clear();
            const auto carved = compileDefinition(source, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(carved.has_value() and carved->m_features.front().m_operations.front().m_material == 0u);

            // Carve has no material leaf; stray material is rejected rather than resolved or retained.
            text = featureText("carve", "rectangle");
            const std::string_view table = "[features.room.operations.cut.action.carve]\n";
            text.insert(text.find(table) + table.size(), "material='missing'\n");
            diagnostics.clear();
            const auto stray = resolveFeatureText(text, diagnostics);
            PPR_TEST_ASSERT(
                not stray and stray.error() == config::make_error_code(config::EConfigErrc::unknown_property));
            PPR_TEST_ASSERT(std::ranges::any_of(diagnostics.entries(), [](const auto &entry) {
                return entry.m_property == "features.room.operations.cut.action.carve.material";
            }));
        };

        PPR_UNIT_TEST (definition_feature_unknown_action_and_shape_tags) {
            for (const bool action: {true, false}) {
                auto text = featureText("carve", "rectangle");
                const auto tag = action ? "tag='carve'" : "tag='rectangle'";
                text.replace(text.find(tag), std::string_view{tag}.size(), "tag='unknown'");
                config::Diagnostics diagnostics;
                const auto rejected = resolveFeatureText(text, diagnostics);
                PPR_TEST_ASSERT(
                    not rejected and
                    rejected.error() == config::make_error_code(config::EConfigErrc::unknown_enum_value));
                const auto path = action ? "features.room.operations.cut.action.tag"
                                         : "features.room.operations.cut.action.carve.shape.tag";
                PPR_TEST_ASSERT(std::ranges::any_of(diagnostics.entries(), [&](const auto &entry) {
                    return entry.m_property == path and entry.m_code == rejected.error();
                }));
            }
        };

        PPR_UNIT_TEST (definition_feature_markers_roles_and_extent_paths) {
            using namespace definition;
            config::Diagnostics diagnostics;
            auto text = featureText("carve", "rectangle");
            const std::array roles{"start", "traversal_room", "sealed_pocket", "resource_anchor", "inert"};
            for (u32 index = 0u; index < roles.size(); ++index) {
                text += std::format(
                    "[features.room.markers.marker_{}]\nrole='{}'\nx_fine={}\ny_fine=0\n", index, roles[index], index);
            }
            const auto resolved = resolveFeatureText(text, diagnostics);
            PPR_TEST_ASSERT(resolved.has_value());
            const auto compiled = compileDefinition(resolved->m_value, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(compiled.has_value() and compiled->m_features.front().m_markers.size() == roles.size());
            for (u32 index = 0u; index < roles.size(); ++index) {
                const auto &marker = compiled->m_features.front().m_markers[index];
                PPR_TEST_ASSERT(
                    marker.m_role == static_cast<EGenerationMarker>(index) and
                    marker.m_x == static_cast<i32>(index) and
                    marker.m_y == 0);
                const auto &binding = compiled->m_bindings.m_features.front().m_markers[index];
                PPR_TEST_ASSERT(binding.m_key == std::format("marker_{}", index) and binding.m_index == index);
            }
            for (const bool x: {true, false}) {
                auto invalid = resolved->m_value;
                auto &marker = invalid.m_features.m_entries.front().m_value.m_markers.m_entries.front().m_value;
                (x ? marker.m_x_fine : marker.m_y_fine) = 12;
                diagnostics.clear();
                PPR_TEST_ASSERT(
                    validateDefinition(invalid, diagnostics) ==
                    config::make_error_code(config::EConfigErrc::cross_field_invalid));
                PPR_TEST_ASSERT(
                    diagnostics.entries().front().m_property ==
                    (x ? "features.room.markers.marker_0.x_fine" : "features.room.markers.marker_0.y_fine"));
            }
        };

        PPR_UNIT_TEST (definition_feature_reservation_and_shape_extent_paths) {
            using namespace definition;
            for (const std::string_view shape: {"rectangle", "ellipse", "capsule"}) {
                config::Diagnostics diagnostics;
                const auto resolved = resolveFeatureText(featureText("carve", shape), diagnostics);
                PPR_TEST_ASSERT(resolved.has_value());
                auto invalid = resolved->m_value;
                auto &feature = invalid.m_features.m_entries.front().m_value;
                feature.m_operations.m_entries.front().m_value.m_x_fine = 10;
                PPR_TEST_ASSERT(
                    validateDefinition(invalid, diagnostics) ==
                    config::make_error_code(config::EConfigErrc::cross_field_invalid));
                PPR_TEST_ASSERT(
                    diagnostics.entries().front().m_property ==
                    "features.room.operations.cut.action.carve.shape." + std::string{shape});

                invalid = resolved->m_value;
                invalid.m_features.m_entries.front().m_value.m_reservation.m_max_x_fine = -3;
                diagnostics.clear();
                PPR_TEST_ASSERT(
                    validateDefinition(invalid, diagnostics) ==
                    config::make_error_code(config::EConfigErrc::cross_field_invalid));
                PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "features.room.reservation.max_x_fine");
                if (shape == "capsule") {
                    diagnostics.clear();
                    const auto too_small_cells = compileDefinition(resolved->m_value, 0.1, diagnostics);
                    PPR_TEST_ASSERT(
                        not too_small_cells and
                        too_small_cells.error() == config::make_error_code(config::EConfigErrc::cross_field_invalid));
                    PPR_TEST_ASSERT(
                        diagnostics.entries().front().m_property ==
                        "features.room.operations.cut.action.carve.shape.capsule");
                }
            }
        };

        [[nodiscard]] auto numericFeatureValues(const GenerationFeature &feature) {
            const auto operation_values = [](const GenerationOperation &value) {
                return std::tuple{
                    value.m_key, value.m_order, value.m_shape, value.m_carve, value.m_x, value.m_y, value.m_end_x,
                    value.m_end_y, value.m_width, value.m_height, value.m_radius_m, value.m_material};
            };
            Array<decltype(operation_values(GenerationOperation{}))> operations;
            for (const auto &operation: feature.m_operations) {
                operations.push_back(operation_values(operation));
            }
            Array<std::tuple<u64, EGenerationMarker, i32, i32>> markers;
            for (const auto &marker: feature.m_markers) {
                markers.push_back({marker.m_key, marker.m_role, marker.m_x, marker.m_y});
            }
            return std::tuple{
                feature.m_key, feature.m_bounds.m_min_x, feature.m_bounds.m_min_y, feature.m_bounds.m_max_x,
                feature.m_bounds.m_max_y, feature.m_allow_reflection, operations, markers};
        }

        PPR_UNIT_TEST (definition_feature_reordering_preserves_bindings_streams_and_geometry) {
            using namespace definition;
            config::Diagnostics diagnostics;
            const auto resolved = resolveFeatureText(featureText("paint", "capsule"), diagnostics);
            PPR_TEST_ASSERT(resolved.has_value());
            auto source = resolved->m_value;
            auto &feature = source.m_features.m_entries.front().m_value;
            auto later = feature.m_operations.m_entries.front();
            later.m_key = "a_later";
            later.m_value.m_order = 11u;
            later.m_value.m_action = Carve{Ellipse{6u, 9u}};
            feature.m_operations.m_entries.push_back(later);
            feature.m_markers.m_entries.push_back({"z_last", {EGenerationMarker::inert, 1, 2}});
            feature.m_markers.m_entries.push_back({"a_first", {EGenerationMarker::resource_anchor, -3, -3}});
            auto other = source.m_features.m_entries.front();
            other.m_key = "alpha";
            other.m_value.m_allow_horizontal_reflection = true;
            source.m_features.m_entries.push_back(other);
            const auto first = compileDefinition(source, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(first.has_value());
            auto reordered = source;
            std::ranges::reverse(reordered.m_features.m_entries);
            for (auto &entry: reordered.m_features.m_entries) {
                std::ranges::reverse(entry.m_value.m_operations.m_entries);
                std::ranges::reverse(entry.m_value.m_markers.m_entries);
            }
            const auto second = compileDefinition(reordered, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(second.has_value() and definitionFingerprint(source) == definitionFingerprint(reordered));
            std::set<u64> keys;
            for (u32 index = 0u; index < first->m_features.size(); ++index) {
                const auto &a = first->m_features[index];
                const auto &b = second->m_features[index];
                PPR_TEST_ASSERT(numericFeatureValues(a) == numericFeatureValues(b));
                const auto &binding = first->m_bindings.m_features[index];
                const auto &other_binding = second->m_bindings.m_features[index];
                PPR_TEST_ASSERT(binding.m_key == (index == 0u ? "alpha" : "room") and binding.m_index == index);
                PPR_TEST_ASSERT(binding.m_key == other_binding.m_key and binding.m_index == other_binding.m_index);
                PPR_TEST_ASSERT(a.m_operations.front().m_order == 7u and a.m_operations.back().m_order == 11u);
                PPR_TEST_ASSERT(keys.insert(a.m_key).second);
                for (const auto &operation: a.m_operations) {
                    PPR_TEST_ASSERT(keys.insert(operation.m_key).second);
                }
                for (u32 marker = 0u; marker < a.m_markers.size(); ++marker) {
                    PPR_TEST_ASSERT(keys.insert(a.m_markers[marker].m_key).second);
                    PPR_TEST_ASSERT(binding.m_markers[marker].m_key == other_binding.m_markers[marker].m_key);
                    PPR_TEST_ASSERT(
                        binding.m_markers[marker].m_index == marker and
                        other_binding.m_markers[marker].m_index == marker);
                }
            }
            auto duplicate_order = source;
            duplicate_order.m_features.m_entries.front().m_value.m_operations.m_entries.back().m_value.m_order = 7u;
            diagnostics.clear();
            PPR_TEST_ASSERT(
                validateDefinition(duplicate_order, diagnostics) ==
                config::make_error_code(config::EConfigErrc::duplicate_key));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "features.room.operations.cut.order");
        };

        PPR_UNIT_TEST (definition_recursive_paths_include_deposits_and_pore) {
            using namespace definition;
            auto source = completeDefinition();
            config::Diagnostics diagnostics;
            PPR_TEST_ASSERT(not validateDefinition(source, diagnostics) and not diagnostics.hasErrors());
            const auto fragment = compileDefinition(source, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(fragment.has_value() and fragment->m_biomes.front().m_deposits.size() == 1u);
            PPR_TEST_ASSERT(fragment->m_biomes.front().m_deposits.front().m_depth_min == 0.125);
            PPR_TEST_ASSERT(fragment->m_biomes.front().m_deposits.front().m_depth_max == 0.875);

            const Array<std::string> expected{
                "biomes", "biomes.plain", "biomes.plain.composition", "biomes.plain.composition.base",
                "biomes.plain.deposits", "biomes.plain.deposits.ore", "biomes.plain.deposits.ore.order",
                "biomes.plain.deposits.ore.field", "biomes.plain.deposits.ore.material",
                "biomes.plain.deposits.ore.host_material", "biomes.plain.deposits.ore.depth",
                "biomes.plain.deposits.ore.depth.min", "biomes.plain.deposits.ore.depth.max",
                "biomes.plain.deposits.ore.threshold", "biomes.plain.deposits.ore.threshold.min",
                "biomes.plain.deposits.ore.threshold.max", "biomes.plain.pore", "biomes.plain.pore.field",
                "biomes.plain.pore.probability", "biomes.plain.pore.radius", "biomes.plain.pore.radius.min_m",
                "biomes.plain.pore.radius.max_m", "biomes.plain.pore.affinity_min", "biomes.plain.pore.passes"};
            Array<std::string> schema_paths;
            config::describeRecursive<Definition>(
                [&](const std::string &path, const auto &, auto) { schema_paths.push_back(path); });
            for (auto path: expected) {
                for (const auto key: {".plain", ".base", ".ore"}) {
                    if (const auto offset = path.find(key); offset != std::string::npos) {
                        path.replace(offset, std::string_view{key}.size(), ".{key}");
                    }
                }
                PPR_TEST_ASSERT(std::ranges::find(schema_paths, path) != schema_paths.end());
            }

            const auto collect = [&](const Definition &object) {
                Array<std::string> paths;
                config::visitRecursive(object, [&](const std::string &path, const auto &, const auto &value) {
                    // Keep the selected deposit/pore contract independent of other biome schema additions.
                    if (std::ranges::find(expected, path) != expected.end()) {
                        paths.push_back(path);
                    }
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, double>) {
                        if (path == "biomes.plain.deposits.ore.depth.min") {
                            PPR_TEST_ASSERT(value == 0.125);
                        } else if (path == "biomes.plain.deposits.ore.depth.max") {
                            PPR_TEST_ASSERT(value == 0.875);
                        } else if (path == "biomes.plain.deposits.ore.threshold.min") {
                            PPR_TEST_ASSERT(value == -0.5);
                        } else if (path == "biomes.plain.deposits.ore.threshold.max") {
                            PPR_TEST_ASSERT(value == 0.75);
                        } else if (path == "biomes.plain.pore.radius.min_m") {
                            PPR_TEST_ASSERT(value == 1.0);
                        } else if (path == "biomes.plain.pore.radius.max_m") {
                            PPR_TEST_ASSERT(value == 2.0);
                        }
                    }
                });
                return paths;
            };
            PPR_TEST_ASSERT(collect(source) == expected);

            auto without_pore = source;
            without_pore.m_biomes.m_entries.front().m_value.m_pore.reset();
            auto expected_without_pore = expected;
            expected_without_pore.resize(17u); // The optional group remains; its seven descendants disappear.
            PPR_TEST_ASSERT(collect(without_pore) == expected_without_pore);

            auto invalid = source;
            invalid.m_biomes.m_entries.front().m_value.m_deposits.m_entries.front().m_value.m_depth.m_max = 1.125;
            diagnostics.clear();
            PPR_TEST_ASSERT(
                validateDefinition(invalid, diagnostics) == config::make_error_code(config::EConfigErrc::out_of_range));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "biomes.plain.deposits.ore.depth.max");
        };

        PPR_UNIT_TEST (definition_fingerprint_v3_pinned_and_sensitive_to_every_member) {
            using namespace definition;
            const auto source = completeDefinition();
            // V3 uses canonical fixed-block rapidhash chaining and structured borrowed owners.
            // Definition schema version remains 2; fingerprint format evolves independently.
            constexpr u64 expected = 0x770ac140d1b05e74ull;

            Array<std::string> paths;
            const auto collect = [&](const std::string &path, const auto &) { paths.push_back(path); };
            walkFingerprintMembers(source, collect);
            const auto actual = definitionFingerprint(source);
            PPR_TEST_ASSERT(kDefinitionSchemaVersion == 2u and actual == expected);

            std::size_t checked = 0u;
            for (const auto &path: paths) {
                auto changed = source;
                bool found = false;
                const auto mutate = [&](const std::string &candidate, auto &value) {
                    if (candidate == path) {
                        flipFingerprintMember(value);
                        found = true;
                    }
                };
                walkFingerprintMembers(changed, mutate);
                PPR_TEST_ASSERT(found and definitionFingerprint(changed) != expected);
                ++checked;
            }
            // Empty features/worlds collections add two descriptor members to the original 80.
            PPR_TEST_ASSERT(checked == 82u);
        };

        PPR_UNIT_TEST (definition_rejects_normalized_weight_collapse) {
            auto source = minimalDefinition();
            auto &choices =
                source.m_biomes.m_entries.front().m_value.m_composition.m_entries.front().m_value.m_materials;
            choices.m_entries = {{"a_small", {{"rock"}, 1.0e-300}}, {"z_large", {{"rock"}, 1.0e300}}};
            // Storage order must not change the key-sorted numeric validation or diagnostic path.
            for (u32 order = 0u; order < 2u; ++order) {
                config::Diagnostics diagnostics;
                const auto error = config::make_error_code(config::EConfigErrc::cross_field_invalid);
                PPR_TEST_ASSERT(definition::validateDefinition(source, diagnostics) == error);
                PPR_TEST_ASSERT(diagnostics.entries().size() == 1u);
                PPR_TEST_ASSERT(
                    diagnostics.entries().front().m_property ==
                    "biomes.plain.composition.base.materials.a_small.weight");
                PPR_TEST_ASSERT(diagnostics.entries().front().m_code == error);
                diagnostics.clear();
                const auto compiled = definition::compileDefinition(source, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(not compiled and compiled.error() == error);
                std::ranges::reverse(choices.m_entries);
            }

            choices.m_entries.front().m_value.m_weight = 1.0;
            choices.m_entries.back().m_value.m_weight = 3.0;
            config::Diagnostics diagnostics;
            PPR_TEST_ASSERT(not definition::validateDefinition(source, diagnostics) and not diagnostics.hasErrors());
        };

        PPR_UNIT_TEST (definition_validates_biome_chain) {
            using namespace definition;
            using config::EConfigErrc;
            const auto valid = minimalDefinition();
            config::Diagnostics diagnostics;
            PPR_TEST_ASSERT(not validateDefinition(valid, diagnostics) and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(valid.m_biomes.m_entries.front().m_value.m_deposits.m_entries.empty());
            PPR_TEST_ASSERT(not valid.m_biomes.m_entries.front().m_value.m_pore);

            const auto rejects = [](const Definition &candidate, const std::string_view path, const EConfigErrc code,
                                     const std::string_view related = {}) {
                config::Diagnostics errors;
                PPR_TEST_ASSERT(validateDefinition(candidate, errors) == config::make_error_code(code));
                const auto found = std::ranges::find(errors.entries(), path, &config::Diagnostic::m_property);
                PPR_TEST_ASSERT(found != errors.entries().end() and found->m_code == config::make_error_code(code));
                if (not related.empty()) {
                    PPR_TEST_ASSERT(
                        std::ranges::find(found->m_related_properties, related) != found->m_related_properties.end());
                }
            };
            auto candidate = valid;
            candidate.m_biomes.m_entries.front().m_value.m_temperature.m_field.m_key = "missing";
            rejects(candidate, "biomes.plain.temperature.field", EConfigErrc::cross_field_invalid, "fields.missing");

            candidate = valid;
            auto &material = candidate.m_materials.m_entries.front().m_value;
            material.m_representation = EMaterialRepresentation::particle;
            material.m_diggable.reset();
            rejects(
                candidate, "biomes.plain.composition.base.materials.stone.material", EConfigErrc::cross_field_invalid,
                "materials.rock");

            candidate = valid;
            candidate.m_biomes.m_entries.front().m_value.m_composition.m_entries.front().m_value.m_depth.m_max = 0.0;
            rejects(
                candidate, "biomes.plain.composition.base.depth.max", EConfigErrc::cross_field_invalid,
                "biomes.plain.composition.base.depth.min");

            candidate = valid;
            candidate.m_biomes.m_entries.front().m_value.m_composition.m_entries.clear();
            rejects(candidate, "biomes.plain.composition", EConfigErrc::out_of_range);

            candidate = valid;
            auto &bands = candidate.m_biomes.m_entries.front().m_value.m_composition.m_entries;
            bands.front().m_value.m_depth.m_max = 0.5;
            auto second = bands.front();
            second.m_key = "next";
            second.m_value.m_depth = {0.5, 1.0};
            bands.push_back(std::move(second));
            rejects(
                candidate, "biomes.plain.composition.next.order", EConfigErrc::duplicate_key,
                "biomes.plain.composition.base.order");

            candidate = valid;
            candidate.m_biomes.m_entries.front().m_value.m_pore = Pore{{"climate"}, 0.0, {2.0, 1.0}};
            rejects(
                candidate, "biomes.plain.pore.radius.max_m", EConfigErrc::cross_field_invalid,
                "biomes.plain.pore.radius.min_m");
        };

        PPR_UNIT_TEST (definition_ids_and_fingerprint_deterministic) {
            using namespace definition;
            constexpr std::string_view omitted = R"toml(
[materials.rock]
phase = "solid"
representation = "terrain"
diggable = true
density = 2500.0
[materials.ash]
phase = "solid"
representation = "terrain"
diggable = false
density = 1000.0
[fields.climate]
algorithm = "open_simplex2"
frequency_per_metre = 0.025
[fields.alpha]
algorithm = "perlin"
frequency_per_metre = 0.01
[biomes.plain.temperature]
field = "climate"
base_k = 293.0
[biomes.plain.composition.base]
order = 0
field = "climate"
depth.min = 0.0
[biomes.plain.composition.base.materials.stone]
material = "rock"
weight = 1.0
[biomes.plain.composition.base.materials.air]
material = "vacuum"
weight = 1.0
)toml";
            constexpr std::string_view explicit_defaults = R"toml(
# Same resolved values, with a different ordering and spelling of numbers.
[fields.alpha]
frequency_per_metre=1e-2
algorithm='perlin'
[materials.ash]
density=1e3
diggable=false
representation='terrain'
phase='solid'
[materials.rock]
density=2.5e3
diggable=true
representation='terrain'
phase='solid'
[fields.climate]
frequency_per_metre=0.025
algorithm='open_simplex2'
fractal='none'
octaves=4
lacunarity=2.0
gain=0.5
axis_scale_x=1.0
axis_scale_y=1.0
rotation_degrees=0.0
warp_amplitude_m=0.0
warp_frequency_per_metre=0.006
[biomes.plain.deposits]
[biomes.plain.temperature]
base_k=293.0
field='climate'
variation_k=0.0
[biomes.plain.composition.base]
field='climate'
order=0
depth.max=1.0
depth.min=0.0
[biomes.plain.composition.base.materials.air]
weight=1.0
material='vacuum'
[biomes.plain.composition.base.materials.stone]
weight=1.0
material='rock'
)toml";
            const auto resolve_source = [](const std::string_view source) {
                config::Diagnostics diagnostics;
                const auto document = config::decode(source, "definition.toml", diagnostics, config::EDecodeMode::tree);
                PPR_TEST_ASSERT(document.has_value());
                const auto layer = config::toTreeLayer(*document, "definition.toml");
                const auto resolved = config::resolveRecursive<Definition>(std::span{&layer, 1u}, diagnostics);
                PPR_TEST_ASSERT(resolved.has_value() and not diagnostics.hasErrors());
                return resolved->m_value;
            };
            const auto first = resolve_source(omitted);
            const auto second = resolve_source(explicit_defaults);
            PPR_TEST_ASSERT(definitionFingerprint(first) == definitionFingerprint(second));

            config::Diagnostics diagnostics;
            const auto a = compileDefinition(first, 1.0 / 3.0, diagnostics);
            const auto b = compileDefinition(second, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(a.has_value() and b.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(a->m_bindings.m_materials.size() == 2u and a->m_bindings.m_fields.size() == 2u);
            PPR_TEST_ASSERT(b->m_bindings.m_materials.size() == 2u and b->m_bindings.m_fields.size() == 2u);
            for (std::size_t index = 0u; index < a->m_bindings.m_materials.size(); ++index) {
                const auto &left = a->m_bindings.m_materials[index];
                const auto &right = b->m_bindings.m_materials[index];
                PPR_TEST_ASSERT(left.m_key == right.m_key and left.m_id == right.m_id);
                PPR_TEST_ASSERT(left.m_id == index + 1u and left.m_id != 0u and left.m_id != 65535u);
                PPR_TEST_ASSERT(a->m_materials[index].m_key == b->m_materials[index].m_key);
            }
            PPR_TEST_ASSERT(a->m_bindings.m_materials.front().m_key == "ash");
            for (std::size_t index = 0u; index < a->m_bindings.m_fields.size(); ++index) {
                PPR_TEST_ASSERT(a->m_bindings.m_fields[index].m_key == b->m_bindings.m_fields[index].m_key);
                PPR_TEST_ASSERT(a->m_bindings.m_fields[index].m_index == index);
                PPR_TEST_ASSERT(b->m_bindings.m_fields[index].m_index == index);
                PPR_TEST_ASSERT(a->m_fields[index].m_key == b->m_fields[index].m_key);
            }
            PPR_TEST_ASSERT(a->m_bindings.m_fields.front().m_key == "alpha");
            PPR_TEST_ASSERT(a->m_biomes.front().m_key == b->m_biomes.front().m_key);
            PPR_TEST_ASSERT(
                a->m_biomes.front().m_temperature_field == 1u and b->m_biomes.front().m_temperature_field == 1u);
            PPR_TEST_ASSERT(a->m_biomes.front().m_bands.front().m_key == b->m_biomes.front().m_bands.front().m_key);
            PPR_TEST_ASSERT(a->m_biomes.front().m_bands.front().m_choices.front().m_material == 0u);
            PPR_TEST_ASSERT(b->m_biomes.front().m_bands.front().m_choices.front().m_material == 0u);

            auto reordered = first;
            std::ranges::reverse(reordered.m_materials.m_entries);
            std::ranges::reverse(reordered.m_fields.m_entries);
            std::ranges::reverse(reordered.m_biomes.m_entries.front()
                    .m_value.m_composition.m_entries.front()
                    .m_value.m_materials.m_entries);
            const auto c = compileDefinition(reordered, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(c.has_value() and definitionFingerprint(reordered) == definitionFingerprint(first));
            PPR_TEST_ASSERT(
                c->m_bindings.m_materials.front().m_key == "ash" and c->m_bindings.m_materials.back().m_id == 2u);
            PPR_TEST_ASSERT(c->m_biomes.front().m_key == a->m_biomes.front().m_key);
            PPR_TEST_ASSERT(
                c->m_biomes.front().m_bands.front().m_choices.front().m_key ==
                a->m_biomes.front().m_bands.front().m_choices.front().m_key);

            auto present_pore = first;
            present_pore.m_biomes.m_entries.front().m_value.m_pore = Pore{{"climate"}, 0.0};
            PPR_TEST_ASSERT(not validateDefinition(present_pore, diagnostics));
            PPR_TEST_ASSERT(definitionFingerprint(present_pore) != definitionFingerprint(first));
        };

        PPR_UNIT_TEST (definition_compiles_every_biome_and_enforces_cardinality) {
            using namespace definition;
            auto source = completeDefinition();
            const auto prototype = source.m_biomes.m_entries.front().m_value;
            for (const u32 count: {1u, kMaxGenerationBiomes}) {
                source.m_biomes.m_entries.clear();
                for (u32 index = 0u; index < count; ++index) {
                    auto biome = prototype;
                    biome.m_temperature.m_base_k = 273.0 + index;
                    if (index % 2u == 0u) {
                        biome.m_pore.reset();
                    }
                    biome.m_deposits.m_entries.front().m_value.m_order = index;
                    source.m_biomes.m_entries.push_back({std::format("biome_{:02}", index), std::move(biome)});
                }
                std::ranges::reverse(source.m_biomes.m_entries);
                config::Diagnostics diagnostics;
                const auto compiled = compileDefinition(source, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(compiled.has_value() and not diagnostics.hasErrors());
                PPR_TEST_ASSERT(compiled->m_biomes.size() == count and compiled->m_bindings.m_biomes.size() == count);
                std::set<u64> streams;
                for (u32 index = 0u; index < count; ++index) {
                    const auto &binding = compiled->m_bindings.m_biomes[index];
                    const auto &biome = compiled->m_biomes[index];
                    PPR_TEST_ASSERT(binding.m_key == std::format("biome_{:02}", index) and binding.m_index == index);
                    PPR_TEST_ASSERT(biome.m_temperature_base_k == 273.0 + index and biome.m_temperature_field == 0u);
                    PPR_TEST_ASSERT(biome.m_bands.size() == 1u and biome.m_deposits.size() == 1u);
                    PPR_TEST_ASSERT(biome.m_bands.front().m_choices.front().m_material == 1u);
                    PPR_TEST_ASSERT(biome.m_deposits.front().m_order == index);
                    PPR_TEST_ASSERT(biome.m_deposits.front().m_depth_min == 0.125);
                    PPR_TEST_ASSERT(biome.m_pore_passes == (index % 2u == 0u ? 0u : 2u));
                    PPR_TEST_ASSERT(biome.m_pore_probability == (index % 2u == 0u ? 0.0 : 0.1));
                    PPR_TEST_ASSERT(streams.insert(biome.m_key).second);
                    PPR_TEST_ASSERT(streams.insert(biome.m_bands.front().m_key).second);
                    PPR_TEST_ASSERT(streams.insert(biome.m_bands.front().m_choices.front().m_key).second);
                    PPR_TEST_ASSERT(streams.insert(biome.m_deposits.front().m_key).second);
                }
            }

            // Invalid scalar in the last canonical biome must not be hidden by first-only lowering.
            source.m_biomes.m_entries.front().m_value.m_temperature.m_variation_k = 1000.0;
            config::Diagnostics diagnostics;
            const auto invalid = compileDefinition(source, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(
                not invalid and invalid.error() == config::make_error_code(config::EConfigErrc::cross_field_invalid));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "biomes.biome_63.temperature.variation_k");

            for (const u32 count: {0u, kMaxGenerationBiomes + 1u}) {
                source.m_biomes.m_entries.clear();
                for (u32 index = 0u; index < count; ++index) {
                    source.m_biomes.m_entries.push_back({std::format("biome_{:02}", index), prototype});
                }
                diagnostics.clear();
                const auto rejected = compileDefinition(source, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(
                    not rejected and rejected.error() == config::make_error_code(config::EConfigErrc::out_of_range));
                PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "biomes");
            }
        };

        PPR_UNIT_TEST (definition_reordering_preserves_all_bindings_streams_and_output) {
            using namespace definition;
            auto source = completeDefinition();
            source.m_materials.m_entries.push_back(
                {"ash", {EGenerationMatter::solid, EMaterialRepresentation::terrain, false, 1000.0}});
            source.m_fields.m_entries.push_back(
                {"alpha", {.m_algorithm = ENoiseAlgorithm::perlin, .m_frequency_per_metre = 0.01}});
            auto &biome = source.m_biomes.m_entries.front().m_value;
            biome.m_lining = Lining{{"ash"}, 2u};
            biome.m_relaxation = {3u, 0.25};
            biome.m_sprinkle.m_probability = 0.125;
            biome.m_finish = {1u, 0.2};
            biome.m_erosion.m_passes = 2u;
            biome.m_surface_layers.m_soil = Soil{{"ash"}, 4.0};
            biome.m_surface_layers.m_weathered = Weathered{{"rock"}, 5.0};
            biome.m_formation = Formation{{"rock"}, 0.1, 4.0};
            biome.m_rind = Rind{{"ash"}};
            auto &bands = biome.m_composition.m_entries;
            bands.front().m_key = "z_first";
            bands.front().m_value.m_depth.m_max = 0.5;
            bands.front().m_value.m_bed_period_m = 8.0;
            bands.front().m_value.m_bed_warp_m = 1.0;
            bands.front().m_value.m_bed_dip = 0.125;
            bands.front().m_value.m_materials.m_entries.push_back({"air", {{"vacuum"}, 2.0}});
            auto second_band = bands.front();
            second_band.m_key = "a_second";
            second_band.m_value.m_order = 9u;
            second_band.m_value.m_depth = {0.5, 1.0};
            second_band.m_value.m_field.m_key = "alpha";
            bands.push_back(std::move(second_band));
            auto second_deposit = biome.m_deposits.m_entries.front();
            second_deposit.m_key = "a_later";
            second_deposit.m_value.m_order = 11u;
            second_deposit.m_value.m_field.m_key = "alpha";
            second_deposit.m_value.m_material.m_key = "ash";
            second_deposit.m_value.m_stage = EGenerationDepositStage::detail;
            second_deposit.m_value.m_core_coverage = 0.75;
            second_deposit.m_value.m_fringe_coverage = 0.5;
            second_deposit.m_value.m_sprinkle_probability = 0.01;
            second_deposit.m_value.m_blob = DepositBlob{3u, {2.0, 4.0}, {1.25, 2.0}, {30.0, 90.0}, 0.2};
            biome.m_deposits.m_entries.push_back(std::move(second_deposit));
            auto second_biome = source.m_biomes.m_entries.front();
            second_biome.m_key = "alpine";
            second_biome.m_value.m_temperature = {{"alpha"}, 280.0, 4.0};
            source.m_biomes.m_entries.push_back(std::move(second_biome));

            auto reordered = source;
            std::ranges::reverse(reordered.m_materials.m_entries);
            std::ranges::reverse(reordered.m_fields.m_entries);
            std::ranges::reverse(reordered.m_biomes.m_entries);
            for (auto &entry: reordered.m_biomes.m_entries) {
                std::ranges::reverse(entry.m_value.m_composition.m_entries);
                std::ranges::reverse(entry.m_value.m_deposits.m_entries);
                for (auto &band: entry.m_value.m_composition.m_entries) {
                    std::ranges::reverse(band.m_value.m_materials.m_entries);
                }
            }
            PPR_TEST_ASSERT(definitionFingerprint(source) == definitionFingerprint(reordered));
            config::Diagnostics diagnostics;
            const auto first = compileDefinition(source, 1.0 / 3.0, diagnostics);
            const auto second = compileDefinition(reordered, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(first.has_value() and second.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(first->m_materials.size() == 2u and second->m_materials.size() == 2u);
            PPR_TEST_ASSERT(first->m_fields.size() == 2u and second->m_fields.size() == 2u);
            PPR_TEST_ASSERT(first->m_biomes.size() == 2u and second->m_biomes.size() == 2u);
            const auto material_values = [](const GenerationMaterial &material) {
                return std::tuple{
                    material.m_id, material.m_terrain, material.m_diggable, material.m_matter,
                    material.m_reference_density_kg_m3, material.m_key};
            };
            const auto noise_values = [](const NoiseFieldDesc &noise) {
                return std::tuple{
                    noise.m_algorithm, noise.m_fractal, noise.m_frequency_per_metre, noise.m_octaves,
                    noise.m_lacunarity, noise.m_gain, noise.m_axis_scale_x, noise.m_axis_scale_y,
                    noise.m_rotation_degrees, noise.m_warp_amplitude_m, noise.m_warp_frequency_per_metre};
            };
            for (u32 index = 0u; index < 2u; ++index) {
                const auto &a = first->m_bindings;
                const auto &b = second->m_bindings;
                const auto expected_material = index == 0u ? "ash" : "rock";
                const auto expected_field = index == 0u ? "alpha" : "climate";
                const auto expected_biome = index == 0u ? "alpine" : "plain";
                PPR_TEST_ASSERT(
                    a.m_materials[index].m_key == expected_material and a.m_materials[index].m_id == index + 1u);
                PPR_TEST_ASSERT(
                    a.m_materials[index].m_key == b.m_materials[index].m_key and
                    a.m_materials[index].m_id == b.m_materials[index].m_id);
                PPR_TEST_ASSERT(a.m_fields[index].m_key == expected_field and a.m_fields[index].m_index == index);
                PPR_TEST_ASSERT(
                    a.m_fields[index].m_key == b.m_fields[index].m_key and b.m_fields[index].m_index == index);
                PPR_TEST_ASSERT(a.m_biomes[index].m_key == expected_biome and a.m_biomes[index].m_index == index);
                PPR_TEST_ASSERT(
                    a.m_biomes[index].m_key == b.m_biomes[index].m_key and b.m_biomes[index].m_index == index);
                PPR_TEST_ASSERT(
                    material_values(first->m_materials[index]) == material_values(second->m_materials[index]));
                PPR_TEST_ASSERT(first->m_fields[index].m_key == second->m_fields[index].m_key);
                PPR_TEST_ASSERT(
                    noise_values(first->m_fields[index].m_noise) == noise_values(second->m_fields[index].m_noise));
                PPR_TEST_ASSERT(
                    numericBiomeValues(first->m_biomes[index]) == numericBiomeValues(second->m_biomes[index]));
                const auto &compiled = first->m_biomes[index];
                PPR_TEST_ASSERT(compiled.m_temperature_field == index);
                PPR_TEST_ASSERT(compiled.m_bands.front().m_order == 0u and compiled.m_bands.back().m_order == 9u);
                PPR_TEST_ASSERT(
                    compiled.m_deposits.front().m_order == 7u and compiled.m_deposits.back().m_order == 11u);
                PPR_TEST_ASSERT(compiled.m_deposits.front().m_shape == EGenerationDepositShape::field);
                PPR_TEST_ASSERT(compiled.m_deposits.back().m_shape == EGenerationDepositShape::blob);
                PPR_TEST_ASSERT(compiled.m_lining_material == 1u and compiled.m_lining_thickness_fine == 2u);
                PPR_TEST_ASSERT(compiled.m_soil_material == 1u and compiled.m_soil_depth_m == 4.0);
                PPR_TEST_ASSERT(compiled.m_weathered_material == 2u and compiled.m_weathered_depth_m == 5.0);
                PPR_TEST_ASSERT(compiled.m_formation_material == 2u and compiled.m_formation_probability == 0.1);
                PPR_TEST_ASSERT(compiled.m_formation_length_m == 4.0 and compiled.m_rind_material == 1u);
            }

            for (const bool deposits: {false, true}) {
                auto duplicate = source;
                auto &authored = duplicate.m_biomes.m_entries.back().m_value;
                if (deposits) {
                    authored.m_deposits.m_entries.back().m_value.m_order =
                        authored.m_deposits.m_entries.front().m_value.m_order;
                } else {
                    authored.m_composition.m_entries.back().m_value.m_order =
                        authored.m_composition.m_entries.front().m_value.m_order;
                }
                diagnostics.clear();
                const auto rejected = compileDefinition(duplicate, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(
                    not rejected and rejected.error() == config::make_error_code(config::EConfigErrc::duplicate_key));
            }
        };

        PPR_UNIT_TEST (definition_halo_includes_unused_biomes_and_reorder_stable_ties) {
            using namespace definition;
            auto source = minimalDefinition();
            const auto prototype = source.m_biomes.m_entries.front().m_value;
            source.m_biomes.m_entries.clear();
            for (const auto key: {"a", "b", "c", "d", "unused"}) {
                source.m_biomes.m_entries.push_back({key, prototype});
            }
            source.m_biomes.m_entries[0u].m_value.m_pore = Pore{{"climate"}, 0.0, {}, 0.0, 6u};
            source.m_biomes.m_entries[1u].m_value.m_relaxation.m_passes = 10u;
            source.m_biomes.m_entries[2u].m_value.m_finish.m_passes = 4u;
            source.m_biomes.m_entries[3u].m_value.m_erosion.m_passes = 4u;
            config::Diagnostics diagnostics;
            const auto accepted = compileDefinition(source, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(accepted.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(accepted->m_biomes.size() == 5u and accepted->m_biomes.back().m_pore_passes == 0u);

            // No layout selects this biome: authored participants still contribute and every tie is reported.
            auto &unused = source.m_biomes.m_entries.back().m_value;
            unused.m_pore = source.m_biomes.m_entries.front().m_value.m_pore;
            unused.m_relaxation.m_passes = 10u;
            unused.m_finish.m_passes = 4u;
            unused.m_erosion.m_passes = 5u;
            const Array<std::string> contributors{
                "biomes.a.pore.passes", "biomes.b.relaxation.passes", "biomes.c.finish.passes",
                "biomes.unused.erosion.passes", "biomes.unused.finish.passes", "biomes.unused.pore.passes",
                "biomes.unused.relaxation.passes"};
            for (u32 order = 0u; order < 2u; ++order) {
                diagnostics.clear();
                const auto rejected = compileDefinition(source, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(
                    not rejected and
                    rejected.error() == config::make_error_code(config::EConfigErrc::cross_field_invalid));
                PPR_TEST_ASSERT(diagnostics.entries().size() == 1u);
                PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "biomes");
                PPR_TEST_ASSERT(diagnostics.entries().front().m_related_properties == contributors);
                std::ranges::reverse(source.m_biomes.m_entries);
            }

            for (u32 stage = 0u; stage < 4u; ++stage) {
                auto candidate = minimalDefinition();
                auto &biome = candidate.m_biomes.m_entries.front().m_value;
                const std::array<std::string_view, 4u> paths{
                    "biomes.plain.pore.passes", "biomes.plain.relaxation.passes", "biomes.plain.finish.passes",
                    "biomes.plain.erosion.passes"};
                if (stage == 0u) {
                    biome.m_pore = Pore{{"climate"}, 0.0, {}, 0.0, 7u};
                } else if (stage == 1u) {
                    biome.m_relaxation.m_passes = 25u;
                } else if (stage == 2u) {
                    biome.m_finish.m_passes = 5u;
                } else {
                    biome.m_erosion.m_passes = 25u;
                }
                diagnostics.clear();
                const auto rejected = compileDefinition(candidate, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(
                    not rejected and rejected.error() == config::make_error_code(config::EConfigErrc::out_of_range));
                PPR_TEST_ASSERT(diagnostics.entries().front().m_property == paths[stage]);
            }
        };

        PPR_UNIT_TEST (definition_authored_field_and_terrain_references_resolve_and_diagnose) {
            using namespace definition;
            using config::EConfigErrc;
            constexpr std::string_view authored = R"toml(
[materials.rock]
phase = "solid"
representation = "terrain"
diggable = true
density = 2500.0
[materials.dust]
phase = "solid"
representation = "particle"
density = 1000.0
[fields.alpha]
algorithm = "perlin"
frequency_per_metre = 0.01
[fields.climate]
algorithm = "open_simplex2"
frequency_per_metre = 0.025
[biomes.plain.temperature]
field = "climate"
base_k = 293.0
[biomes.plain.composition.base]
order = 0
field = "climate"
depth.min = 0.0
[biomes.plain.composition.base.materials.stone]
material = "rock"
weight = 1.0
[biomes.plain.deposits.ore]
order = 0
field = "alpha"
material = "rock"
host_material = "rock"
depth.min = 0.1
depth.max = 0.9
threshold.min = -0.5
threshold.max = 0.5
[biomes.plain.pore]
field = "alpha"
[biomes.plain.lining]
material = "rock"
thickness_fine = 1
)toml";
            const auto resolve_source = [](const std::string_view source, config::Diagnostics &diagnostics) {
                const auto document = config::decode(source, "references.toml", diagnostics, config::EDecodeMode::tree);
                PPR_TEST_ASSERT(document.has_value());
                const auto layer = config::toTreeLayer(*document, "references.toml");
                return config::resolveRecursive<Definition>(std::span{&layer, 1u}, diagnostics);
            };
            config::Diagnostics diagnostics;
            const auto resolved = resolve_source(authored, diagnostics);
            PPR_TEST_ASSERT(resolved.has_value() and not diagnostics.hasErrors());
            const auto compiled = compileDefinition(resolved->m_value, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(compiled.has_value() and not diagnostics.hasErrors());
            const auto &biome = compiled->m_biomes.front();
            PPR_TEST_ASSERT(biome.m_temperature_field == 1u and biome.m_bands.front().m_field == 1u);
            PPR_TEST_ASSERT(biome.m_deposits.front().m_field == 0u and biome.m_pore_field == 0u);
            PPR_TEST_ASSERT(biome.m_bands.front().m_choices.front().m_material == 2u);
            PPR_TEST_ASSERT(biome.m_deposits.front().m_material == 2u and biome.m_deposits.front().m_host == 2u);
            PPR_TEST_ASSERT(biome.m_lining_material == 2u);

            const auto replace_leaf = [&](const std::string_view group, const std::string_view leaf,
                                          const std::string_view replacement) {
                std::string source{authored};
                const auto start = source.find(group);
                PPR_TEST_ASSERT(start != std::string::npos);
                const auto offset = source.find(leaf, start);
                PPR_TEST_ASSERT(offset != std::string::npos);
                source.replace(offset, leaf.size(), replacement);
                return source;
            };
            for (const auto &[group, leaf, path]: std::array{
                     std::tuple{"[biomes.plain.temperature]", "field = \"climate\"", "biomes.plain.temperature.field"},
                     std::tuple{
                         "[biomes.plain.deposits.ore]", "material = \"rock\"", "biomes.plain.deposits.ore.material"}}) {
                diagnostics.clear();
                const auto missing = resolve_source(replace_leaf(group, leaf, ""), diagnostics);
                PPR_TEST_ASSERT(not missing and missing.error() == config::make_error_code(EConfigErrc::missing_value));
                PPR_TEST_ASSERT(diagnostics.entries().front().m_property == path);

                const auto scalar = std::string_view{leaf}.starts_with("field") ? "field" : "material";
                diagnostics.clear();
                const auto invalid = resolve_source(
                    replace_leaf(group, leaf, std::string{scalar} + " = \"bad key\""), diagnostics);
                PPR_TEST_ASSERT(not invalid and invalid.error() == config::make_error_code(EConfigErrc::out_of_range));
                PPR_TEST_ASSERT(diagnostics.entries().front().m_property == path);

                diagnostics.clear();
                const auto unknown = resolve_source(
                    replace_leaf(group, leaf, std::string{scalar} + " = \"missing\""), diagnostics);
                PPR_TEST_ASSERT(unknown.has_value());
                const auto rejected = compileDefinition(unknown->m_value, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(
                    not rejected and rejected.error() == config::make_error_code(EConfigErrc::cross_field_invalid));
                PPR_TEST_ASSERT(diagnostics.entries().front().m_property == path);
                const std::string collection = scalar == std::string_view{"field"} ? "fields" : "materials";
                const Array<std::string> related{collection + ".missing", collection};
                PPR_TEST_ASSERT(diagnostics.entries().front().m_related_properties == related);
            }

            for (const auto &[leaf, path]: std::array{
                     std::pair{"material = \"rock\"", "biomes.plain.deposits.ore.material"},
                     std::pair{"host_material = \"rock\"", "biomes.plain.deposits.ore.host_material"}}) {
                diagnostics.clear();
                const auto name = std::string_view{leaf}.substr(0u, std::string_view{leaf}.find(' '));
                const auto particle = resolve_source(
                    replace_leaf("[biomes.plain.deposits.ore]", leaf, std::string{name} + " = \"dust\""), diagnostics);
                PPR_TEST_ASSERT(particle.has_value());
                const auto rejected = compileDefinition(particle->m_value, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(
                    not rejected and rejected.error() == config::make_error_code(EConfigErrc::cross_field_invalid));
                PPR_TEST_ASSERT(diagnostics.entries().front().m_property == path);
                PPR_TEST_ASSERT(
                    diagnostics.entries().front().m_related_properties ==
                    Array<std::string>{"materials.dust", "materials"});
            }
        };

        PPR_UNIT_TEST (definition_blob_presence_and_controls_across_all_biomes) {
            using namespace definition;
            using config::EConfigErrc;
            auto source = completeDefinition();
            source.m_biomes.m_entries.front().m_value.m_pore.reset();
            auto second = source.m_biomes.m_entries.front();
            second.m_key = "unused";
            second.m_value.m_deposits.m_entries.front().m_value.m_blob = DepositBlob{
                2048u, {1.0, 2.0}, {1.25, 2.0}, {30.0, 90.0}, 0.2};
            source.m_biomes.m_entries.push_back(std::move(second));
            config::Diagnostics diagnostics;
            const auto compiled = compileDefinition(source, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(compiled.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(compiled->m_biomes.front().m_deposits.front().m_shape == EGenerationDepositShape::field);
            const auto &blob = compiled->m_biomes.back().m_deposits.front();
            PPR_TEST_ASSERT(blob.m_shape == EGenerationDepositShape::blob and blob.m_blob_count == 2048u);
            PPR_TEST_ASSERT(blob.m_radius_min_m == 1.0 and blob.m_radius_max_m == 2.0);
            PPR_TEST_ASSERT(blob.m_stretch_min == 1.25 and blob.m_stretch_max == 2.0);
            PPR_TEST_ASSERT(blob.m_orientation_min_degrees == 30.0 and blob.m_orientation_max_degrees == 90.0);
            PPR_TEST_ASSERT(blob.m_distortion == 0.2 and blob.m_threshold_min == -0.5 and blob.m_threshold_max == 0.75);

            const auto rejects = [&](const auto &mutate, const std::string_view path, const EConfigErrc code,
                                     const double cell_size_m = 1.0 / 3.0) {
                auto candidate = source;
                mutate(candidate.m_biomes.m_entries.back().m_value.m_deposits.m_entries.front().m_value);
                config::Diagnostics errors;
                const auto rejected = compileDefinition(candidate, cell_size_m, errors);
                PPR_TEST_ASSERT(not rejected and rejected.error() == config::make_error_code(code));
                const auto diagnostic = std::ranges::find(errors.entries(), path, &config::Diagnostic::m_property);
                PPR_TEST_ASSERT(
                    diagnostic != errors.entries().end() and diagnostic->m_code == config::make_error_code(code));
            };
            rejects(
                [](auto &deposit) { deposit.m_blob->m_count = 0u; }, "biomes.unused.deposits.ore.blob.count",
                EConfigErrc::out_of_range);
            rejects(
                [](auto &deposit) { deposit.m_blob->m_count = 4097u; }, "biomes.unused.deposits.ore.blob.count",
                EConfigErrc::out_of_range);
            rejects(
                [](auto &deposit) { deposit.m_blob->m_radius.m_min_m = 0.0; },
                "biomes.unused.deposits.ore.blob.radius.min_m", EConfigErrc::cross_field_invalid);
            rejects(
                [](auto &deposit) { deposit.m_blob->m_radius.m_min_m = 3.0; },
                "biomes.unused.deposits.ore.blob.radius.min_m", EConfigErrc::cross_field_invalid);
            rejects(
                [](auto &deposit) { deposit.m_blob->m_stretch.m_max = 1.0; },
                "biomes.unused.deposits.ore.blob.stretch.max", EConfigErrc::cross_field_invalid);
            rejects(
                [](auto &deposit) { deposit.m_blob->m_stretch.m_min = 0.0; },
                "biomes.unused.deposits.ore.blob.stretch.min", EConfigErrc::out_of_range);
            rejects(
                [](auto &deposit) { deposit.m_blob->m_orientation.m_max_degrees = 20.0; },
                "biomes.unused.deposits.ore.blob.orientation.max_degrees", EConfigErrc::cross_field_invalid);
            rejects(
                [](auto &deposit) { deposit.m_blob->m_distortion = 0.5; }, "biomes.unused.deposits.ore.blob.distortion",
                EConfigErrc::out_of_range);
            rejects(
                [](auto &deposit) { deposit.m_core_coverage = 0.5; }, "biomes.unused.deposits.ore.fringe_coverage",
                EConfigErrc::cross_field_invalid);
            rejects(
                [](auto &deposit) { deposit.m_sprinkle_probability = 0.01; },
                "biomes.unused.deposits.ore.sprinkle_probability", EConfigErrc::cross_field_invalid);
            rejects(
                [](auto &deposit) {
                    deposit.m_blob.reset();
                    deposit.m_core_coverage = 0.5;
                    deposit.m_fringe_coverage = 0.5;
                },
                "biomes.unused.deposits.ore.blob", EConfigErrc::cross_field_invalid);
            rejects(
                [](auto &deposit) {
                    deposit.m_blob.reset();
                    deposit.m_stage = EGenerationDepositStage::detail;
                    deposit.m_sprinkle_probability = 0.01;
                },
                "biomes.unused.deposits.ore.blob", EConfigErrc::cross_field_invalid);
            rejects(
                [](auto &) {}, "biomes.unused.deposits.ore.blob.radius.max_m", EConfigErrc::cross_field_invalid,
                1.0 / kWorldEdge);

            // The aggregate cap covers the complete fragment, not one selected biome.
            source.m_biomes.m_entries.front().m_value.m_deposits.m_entries.front().m_value.m_blob =
                source.m_biomes.m_entries.back().m_value.m_deposits.m_entries.front().m_value.m_blob;
            diagnostics.clear();
            PPR_TEST_ASSERT(
                compileDefinition(source, 1.0 / 3.0, diagnostics).has_value() and not diagnostics.hasErrors());
            ++source.m_biomes.m_entries.back().m_value.m_deposits.m_entries.front().m_value.m_blob->m_count;
            const Array<std::string> counts{
                "biomes.plain.deposits.ore.blob.count", "biomes.unused.deposits.ore.blob.count"};
            for (u32 order = 0u; order < 2u; ++order) {
                diagnostics.clear();
                const auto rejected = compileDefinition(source, 1.0 / 3.0, diagnostics);
                PPR_TEST_ASSERT(
                    not rejected and rejected.error() == config::make_error_code(EConfigErrc::out_of_range));
                PPR_TEST_ASSERT(
                    diagnostics.entries().size() == 1u and diagnostics.entries().front().m_property == "biomes");
                PPR_TEST_ASSERT(diagnostics.entries().front().m_related_properties == counts);
                std::ranges::reverse(source.m_biomes.m_entries);
            }
        };

        PPR_UNIT_TEST (halo_maxima_across_biomes) {
            std::array<GenerationBiome, 4u> biomes{};
            const std::array<std::string_view, 4u> paths{"biomes.a", "biomes.b", "biomes.c", "biomes.d"};
            biomes[0u].m_pore_passes = 6u;
            biomes[1u].m_relaxation_passes = 10u;
            biomes[2u].m_finish_passes = 4u;
            biomes[3u].m_erosion_passes = 4u;
            config::Diagnostics diagnostics;
            PPR_TEST_ASSERT(not validateGenerationHalo(biomes, paths, &diagnostics));
            PPR_TEST_ASSERT(not diagnostics.hasErrors());

            biomes[3u].m_erosion_passes = 5u;
            PPR_TEST_ASSERT(
                validateGenerationHalo(biomes, paths, &diagnostics) ==
                config::make_error_code(config::EConfigErrc::cross_field_invalid));
            PPR_TEST_ASSERT(diagnostics.entries().size() == 1u);
            const auto &error = diagnostics.entries().front();
            PPR_TEST_ASSERT(error.m_property == "biomes");
            const Array<std::string> contributors{
                "biomes.a.pore.passes", "biomes.b.relaxation_passes", "biomes.c.finish_passes",
                "biomes.d.erosion_passes"};
            PPR_TEST_ASSERT(error.m_related_properties == contributors);
            std::ranges::reverse(biomes);
            auto reversed_paths = paths;
            std::ranges::reverse(reversed_paths);
            diagnostics.clear();
            PPR_TEST_ASSERT(
                validateGenerationHalo(biomes, reversed_paths, &diagnostics) ==
                config::make_error_code(config::EConfigErrc::cross_field_invalid));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_related_properties == contributors);

            for (auto &biome: biomes) {
                biome.m_pore_passes = 0u;
                biome.m_relaxation_passes = 0u;
                biome.m_finish_passes = 0u;
                biome.m_erosion_passes = 0u;
            }
            diagnostics.clear();
            PPR_TEST_ASSERT(not validateGenerationHalo(biomes, paths, &diagnostics) and not diagnostics.hasErrors());
        };

        PPR_UNIT_TEST (fragment_to_recipe_compiles) {
            using namespace definition;
            const auto source = minimalDefinition();
            config::Diagnostics diagnostics;
            const auto fragment = compileDefinition(source, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(fragment.has_value());
            auto recipe = numericRecipe();
            recipe.m_materials = fragment->m_materials;
            recipe.m_fields = fragment->m_fields;
            recipe.m_biomes = fragment->m_biomes;
            recipe.m_boundary_material = fragment->m_bindings.m_materials.front().m_id;
            // World scale, layout constraints, start reservation and actor geometry stay caller-owned.
            const auto plan = compileGenerationPlan(std::move(recipe), 42u);
            PPR_TEST_ASSERT(plan.has_value());

            auto shifted = source;
            shifted.m_materials.m_entries.push_back({"ash", source.m_materials.m_entries.front().m_value});
            shifted.m_fields.m_entries.push_back({"alpha", source.m_fields.m_entries.front().m_value});
            const auto shifted_fragment = compileDefinition(shifted, 1.0 / 3.0, diagnostics);
            PPR_TEST_ASSERT(shifted_fragment.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(fragment->m_bindings.m_materials.front().m_id == 1u);
            PPR_TEST_ASSERT(
                shifted_fragment->m_bindings.m_materials.back().m_key == "rock" and
                shifted_fragment->m_bindings.m_materials.back().m_id == 2u);
            PPR_TEST_ASSERT(
                shifted_fragment->m_bindings.m_fields.back().m_key == "climate" and
                shifted_fragment->m_bindings.m_fields.back().m_index == 1u);
            PPR_TEST_ASSERT(fragment->m_materials.front().m_key == shifted_fragment->m_materials.back().m_key);
            PPR_TEST_ASSERT(fragment->m_fields.front().m_key == shifted_fragment->m_fields.back().m_key);
            PPR_TEST_ASSERT(fragment->m_biomes.front().m_key == shifted_fragment->m_biomes.front().m_key);
            PPR_TEST_ASSERT(
                fragment->m_biomes.front().m_bands.front().m_key ==
                shifted_fragment->m_biomes.front().m_bands.front().m_key);
            PPR_TEST_ASSERT(
                fragment->m_biomes.front().m_bands.front().m_choices.front().m_key ==
                shifted_fragment->m_biomes.front().m_bands.front().m_choices.front().m_key);
        };

        PPR_UNIT_TEST (noise_descriptor_preserves_authored_editing_metadata) {
            using namespace config;
            const std::array<std::string_view, 11u> descriptions{
                "Base noise algorithm", "Fractal octave combination", "Base spatial frequency",
                "Number of fractal noise octaves", "Frequency multiplier between octaves",
                "Amplitude multiplier between octaves", "Horizontal sampling scale", "Vertical sampling scale",
                "Sampling coordinate rotation", "Domain warp displacement", "Domain warp spatial frequency"};
            const std::array<std::string_view, 11u> units{"", "", "1/m", "", "", "", "", "", "degrees", "m", "1/m"};
            const std::array<std::optional<double>, 11u> slider_min{
                std::nullopt, std::nullopt, 0.0, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                std::nullopt, 0.0, 0.0};
            const std::array<std::optional<double>, 11u> slider_max{
                std::nullopt, std::nullopt, 0.1, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                std::nullopt, 100.0, 0.1};
            const auto members = NoiseFieldDesc::describe();
            std::size_t index = 0u;
            const auto inspect = [&](const auto &member) {
                PPR_TEST_ASSERT(
                    member.m_metadata.m_description == descriptions[index] and
                    member.m_metadata.m_units == units[index]);
                PPR_TEST_ASSERT(
                    member.m_metadata.m_slider_min == slider_min[index] and
                    member.m_metadata.m_slider_max == slider_max[index]);
                const auto projected = describeProperty(member);
                PPR_TEST_ASSERT(projected.m_description == descriptions[index] and projected.m_units == units[index]);
                ++index;
            };
            std::apply([&](const auto &...member) { (inspect(member), ...); }, members);
            PPR_TEST_ASSERT(index == descriptions.size());

            const auto frequency = describeProperty(std::get<2>(members));
            const auto warp_frequency = describeProperty(std::get<10>(members));
            for (const auto &projected: {frequency, warp_frequency}) {
                PPR_TEST_ASSERT(projected.m_min == 0.0 and projected.m_max == (std::numeric_limits<double>::max)());
                // The authored zero slider endpoint is nudged only to respect the exclusive legal minimum.
                PPR_TEST_ASSERT(projected.m_exclusive_min and projected.m_slider_min == std::nextafter(0.0, 0.1));
                PPR_TEST_ASSERT(projected.m_slider_max == 0.1 and projected.m_units == "1/m");
            }
            PPR_TEST_ASSERT(std::get<2>(members).m_metadata.m_slider_min == 0.0);
            PPR_TEST_ASSERT(std::get<10>(members).m_metadata.m_slider_min == 0.0);

            const auto amplitude = describeProperty(std::get<9>(members));
            PPR_TEST_ASSERT(amplitude.m_min == 0.0 and amplitude.m_max == 4096.0);
            PPR_TEST_ASSERT(
                amplitude.m_slider_min == 0.0 and amplitude.m_slider_max == 100.0 and amplitude.m_units == "m");
            const auto rotation = describeProperty(std::get<8>(members));
            PPR_TEST_ASSERT(rotation.m_min == -180.0 and rotation.m_max == 180.0);
            PPR_TEST_ASSERT(
                rotation.m_slider_min == -180.0 and rotation.m_slider_max == 180.0 and rotation.m_units == "degrees");
        };

        PPR_UNIT_TEST (noise_schema_rejects_unsafe) {
            using namespace config;
            const auto base_layer = [] {
                TreeLayer layer;
                layer.m_scalars.m_source = ESource::file;
                layer.m_scalars.m_assignments = {
                    {"algorithm", {}, SourceValue::text("open_simplex2"), "noise.toml:1", 0u},
                    {"frequency_per_metre", {}, SourceValue::floating(1.0e-8), "noise.toml:2", 0u},
                    {"warp_frequency_per_metre", {}, SourceValue::floating(1.0e-8), "noise.toml:3", 0u}};
                return layer;
            };
            const auto rejects = [&](const std::string_view name, SourceValue value) {
                auto layer = base_layer();
                layer.m_scalars.m_assignments.push_back({std::string{name}, {}, std::move(value), "noise.toml:4", 0u});
                Diagnostics diagnostics;
                const auto result = resolveRecursive<NoiseFieldDesc>(std::span{&layer, 1u}, diagnostics);
                PPR_TEST_ASSERT(not result and result.error() == make_error_code(EConfigErrc::out_of_range));
                PPR_TEST_ASSERT(diagnostics.entries().front().m_property == name);
            };
            rejects("octaves", SourceValue::integer(0));
            rejects("octaves", SourceValue::integer(17));
            for (const auto &[name, low, high]: std::array{
                     std::tuple{"lacunarity", 0.99, 4.01}, std::tuple{"gain", -0.01, 1.01},
                     std::tuple{"axis_scale_x", 0.124, 8.01}, std::tuple{"axis_scale_y", 0.124, 8.01},
                     std::tuple{"rotation_degrees", -180.01, 180.01}}) {
                rejects(name, SourceValue::floating(low));
                rejects(name, SourceValue::floating(high));
            }

            auto edge = base_layer();
            edge.m_scalars.m_assignments.push_back({"octaves", {}, SourceValue::integer(16), "edge", 0u});
            edge.m_scalars.m_assignments.push_back({"lacunarity", {}, SourceValue::floating(4.0), "edge", 0u});
            edge.m_scalars.m_assignments.push_back({"gain", {}, SourceValue::floating(1.0), "edge", 0u});
            edge.m_scalars.m_assignments.push_back({"axis_scale_x", {}, SourceValue::floating(0.125), "edge", 0u});
            edge.m_scalars.m_assignments.push_back({"axis_scale_y", {}, SourceValue::floating(8.0), "edge", 0u});
            edge.m_scalars.m_assignments.push_back({"rotation_degrees", {}, SourceValue::floating(180.0), "edge", 0u});
            Diagnostics diagnostics;
            const auto valid = resolveRecursive<NoiseFieldDesc>(std::span{&edge, 1u}, diagnostics);
            PPR_TEST_ASSERT(valid.has_value() and not validateNoiseFieldDescription(valid->m_value));

            auto unsafe = minimalDefinition();
            unsafe.m_fields.m_entries.front().m_value = valid->m_value;
            unsafe.m_fields.m_entries.front().m_value.m_frequency_per_metre = 1.0e20;
            PPR_TEST_ASSERT(
                definition::validateDefinition(unsafe, diagnostics) == make_error_code(EConfigErrc::out_of_range));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "fields.climate");
        };

        PPR_UNIT_TEST (generation_random_has_fixed_full_width_streams) {
            GenerationRandom random{0u};
            PPR_TEST_ASSERT(random.next() == 0xe220a8397b1dcdafull);
            PPR_TEST_ASSERT(random.next() == 0x6e789e6aa1b965f4ull);
            PPR_TEST_ASSERT(random.next() == 0x06c45d188009454full);

            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                GenerationRandom first{generationStreamSeed(seed, 101u, 7u)};
                GenerationRandom replay{generationStreamSeed(seed, 101u, 7u)};
                GenerationRandom other{generationStreamSeed(seed, 101u, 8u)};
                const u64 first_draw = first.next();
                PPR_TEST_ASSERT(first_draw == replay.next());
                PPR_TEST_ASSERT(first_draw != other.next());
                for (const u64 bound: {1ull, 3ull, 17ull, 0x8000000000000001ull, 0xffffffffffffffffull}) {
                    for (u32 i = 0u; i < 100u; ++i) {
                        PPR_TEST_ASSERT(first.bounded(bound) < bound);
                    }
                }

                for (u32 i = 0u; i < 100u; ++i) {
                    const double value = first.unit();
                    PPR_TEST_ASSERT(value >= 0.0 and value < 1.0);
                }
            }
        };

        PPR_UNIT_TEST (numeric_plan_owns_recipe_and_isolates_field_streams) {
            auto input = numericRecipe();
            auto first = compileGenerationPlan(input, 42u);
            PPR_TEST_ASSERT(first.has_value());
            input.m_fields.insert(input.m_fields.begin(), {999u, {.m_frequency_per_metre = 0.1}});
            input.m_biomes[0u].m_temperature_field = 1u;
            input.m_biomes[0u].m_bands[0u].m_field = 1u;
            auto second = compileGenerationPlan(std::move(input), 42u);
            PPR_TEST_ASSERT(second.has_value());
            PPR_TEST_ASSERT(first->recipe().m_fields.size() == 1u and first->seed() == 42u);

            for (u32 i = 0u; i < 100u; ++i) {
                PPR_TEST_ASSERT(
                    first->fields()[0u].sample(i * 2.75, 17.0) == second->fields()[1u].sample(i * 2.75, 17.0));
            }
        };

        PPR_UNIT_TEST (numeric_plan_rejects_invalid_references_bounds_and_collapsed_weights) {
            const auto rejects = [](const auto &mutate) {
                auto recipe = numericRecipe();
                mutate(recipe);
                PPR_TEST_ASSERT(not compileGenerationPlan(std::move(recipe), 42u));
            };

            rejects([](auto &recipe) { recipe.m_biomes[0u].m_temperature_field = 1u; });
            rejects([](auto &recipe) { recipe.m_biomes[0u].m_erosion_passes = 25u; });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_relaxation_passes = 17u;
                recipe.m_biomes[0u].m_erosion_passes = 8u;
            });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_relaxation_passes = (std::numeric_limits<u32>::max)();
                recipe.m_biomes[0u].m_pore_passes = 1u;
            });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_relaxation_passes = 20u;
                recipe.m_biomes[0u].m_pore_passes = 3u;
                recipe.m_biomes[0u].m_finish_passes = 2u;
            });
            rejects([](auto &recipe) { recipe.m_fields.push_back(recipe.m_fields[0u]); });
            rejects([](auto &recipe) { recipe.m_biomes[0u].m_lining_material = 17u; });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_lining_material = 17u;
                recipe.m_biomes[0u].m_lining_thickness_fine = 4u;
            });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_lining_material = 99u;
                recipe.m_biomes[0u].m_lining_thickness_fine = 1u;
            });
            rejects([](auto &recipe) { recipe.m_materials[0u].m_id = 0u; });
            rejects([](auto &recipe) { recipe.m_biomes[0u].m_bands[0u].m_choices[0u].m_material = 0u; });
            rejects([](auto &recipe) { recipe.m_max_sites = kMaxGenerationSites + 1u; });
            rejects([](auto &recipe) {
                recipe.m_features[0u].m_bounds = {
                    (std::numeric_limits<i32>::min)(), 0, (std::numeric_limits<i32>::min)() + 30, 30};
                recipe.m_start_x_fine = 2147485648u;
            });
            rejects([](auto &recipe) { recipe.m_site_spacing_m = 1.0e-200; });
            rejects([](auto &recipe) { recipe.m_start_x_fine = 4090u; });
            rejects([](auto &recipe) { recipe.m_actor_half_width_m = std::numeric_limits<double>::quiet_NaN(); });
            rejects([](auto &recipe) { recipe.m_features[0u].m_operations[0u].m_width = 31u; });
            rejects([](auto &recipe) {
                recipe.m_biomes[0u].m_bands[0u].m_choices.push_back(
                    {999u, 17u, std::numeric_limits<double>::denorm_min()});
            });
            rejects([](auto &recipe) {
                recipe.m_constraints[0u].m_weight = std::numeric_limits<double>::denorm_min();
                recipe.m_biomes.push_back(recipe.m_biomes[0u]);
                recipe.m_biomes.back().m_key = 999u;
                recipe.m_constraints.push_back(
                    {999u, 1u, 0u, (std::numeric_limits<double>::max)(), 0.0, 1.0, 0.0, 2048.0});
            });
        };

        PPR_UNIT_TEST (numeric_plan_accepts_granular_fills_and_exact_signed_thirds) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({1001u, false, false, EGenerationMatter::solid, 1800.0});
            recipe.m_features[0u].m_bounds.m_min_y = -30;
            recipe.m_features[0u].m_markers.push_back({999u, EGenerationMarker::sealed_pocket, 15, 15});
            recipe.m_rules.push_back(
                {.m_key = 501u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{.m_max_count = 1u, .m_attempts = 1u}});
            recipe.m_rules.push_back(
                {.m_key = 502u,
                    .m_phase = EGenerationPhase::fill,
                    .m_parameters = GenerationFill{
                        .m_marker = 1u,
                        .m_material = 1001u,
                        .m_temperature_k = 290.0,
                        .m_bulk_density_kg_m3 = 1800.0,
                        .m_fill_height_m = 5.0 / 3.0}});

            for (const double height: {5.0 / 3.0, -5.0 / 3.0}) {
                auto &fill = std::get<GenerationFill>(recipe.m_rules[1u].m_parameters);
                fill.m_fill_height_m = height;
                PPR_TEST_ASSERT(not validateGenerationRecipe(recipe));
                fill.m_fill_height_m = std::nextafter(height, 0.0);
                PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::invalid_argument);
            }

            recipe.m_rules.clear();
            recipe.m_rules.push_back(
                {.m_key = 503u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationWorm{
                        .m_count = 1u,
                        .m_carve = true,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 0.1,
                        .m_length_max_m = 0.1,
                        .m_radius_min_m = 0.1,
                        .m_radius_max_m = 0.1}});
            recipe.m_materials[0u].m_reference_density_kg_m3 = 1.0e100;
            recipe.m_biomes[0u].m_temperature_base_k = 1.0e30;
            PPR_TEST_ASSERT(not validateGenerationRecipe(recipe));
        };

        PPR_UNIT_TEST (numeric_plan_preserves_authored_geometry_boundaries) {
            auto recipe = numericRecipe();
            recipe.m_features[0u].m_operations[0u] = {
                .m_key = 302u,
                .m_shape = EGenerationShape::capsule,
                .m_carve = true,
                .m_x = 7,
                .m_y = 7,
                .m_end_x = 20,
                .m_end_y = 20,
                .m_radius_m = 7.0 / 3.0};
            PPR_TEST_ASSERT(not validateGenerationRecipe(recipe));

            recipe.m_features[0u].m_bounds = {0, 0, 2000, 2000};
            auto &operation = recipe.m_features[0u].m_operations[0u];
            operation.m_x = 900;
            operation.m_y = 900;
            operation.m_end_x = 1100;
            operation.m_end_y = 1100;
            operation.m_radius_m = 300.0;
            recipe.m_start_x_fine = 1000u;
            recipe.m_start_y_fine = 1000u;
            recipe.m_start_clearance_m = 300.0;
            recipe.m_start_route_width_m = 300.0;
            recipe.m_features[0u].m_markers.push_back({999u, EGenerationMarker::resource_anchor, 20, 10});
            recipe.m_resources.push_back({999u, 1u, 1u, 17u, 2000.0});
            PPR_TEST_ASSERT(not validateGenerationRecipe(recipe));
        };

        PPR_UNIT_TEST (biome_lining_stops_threshold_caves_and_replays_across_chunks) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_biomes.push_back(recipe.m_biomes.front());
            recipe.m_biomes.back().m_key = 204u;
            recipe.m_constraints.push_back({402u, 1u, 1u, 1.0, 0.0, 1.0, 0.0, 2048.0});
            recipe.m_rules.push_back({501u, 1u, EGenerationPhase::carve, GenerationCave{0u, 0u, 0.0, 1.0, -1.0, 1.0}});
            recipe.m_rules.push_back({502u, 2u, EGenerationPhase::carve, GenerationCave{1u, 0u, 0.0, 1.0, -1.0, 1.0}});
            // Four surrounding sites make a single-cell different-biome island.
            // Diagonal distance two/three cannot be detected by cardinal rays.
            const std::array sites{
                GenerationSite{125.5, 127.5, 0u}, GenerationSite{129.5, 127.5, 0u}, GenerationSite{127.5, 125.5, 0u},
                GenerationSite{127.5, 129.5, 0u}, GenerationSite{127.5, 127.5, 1u}};

            for (const u32 width: {1u, 2u, 3u}) {
                recipe.m_biomes.front().m_lining_material = 17u;
                recipe.m_biomes.front().m_lining_thickness_fine = width;
                recipe.m_biomes.front().m_relaxation_passes = 3u;
                recipe.m_biomes.front().m_relaxation_adoption = 1.0;
                const auto plan = compileGenerationPlan(recipe, 42u);
                PPR_TEST_ASSERT(plan.has_value());
                const auto layout = restoreBiomeLayout(*plan, sites);
                PPR_TEST_ASSERT(layout.has_value());
                const auto geometry = compileGenerationGeometry(*plan, *layout);
                PPR_TEST_ASSERT(geometry.has_value());
                const auto raster = compileGenerationRaster(*plan, *layout, *geometry);
                PPR_TEST_ASSERT(raster.has_value());
                const auto left = raster->chunk({0u, 0u});
                const auto right = raster->chunk({1u, 0u});
                const auto replay = raster->chunk({1u, 0u});
                PPR_TEST_ASSERT(left.has_value() and right.has_value() and replay.has_value());
                PPR_TEST_ASSERT((*left)[126u * kChunkEdge + 126u].m_element == (width >= 2u ? 17u : 0u));
                PPR_TEST_ASSERT((*left)[126u * kChunkEdge + 125u].m_element == (width >= 3u ? 17u : 0u));
                PPR_TEST_ASSERT((*left)[126u * kChunkEdge + 127u].m_element == 17u);
                PPR_TEST_ASSERT((*right)[126u * kChunkEdge].m_element == (width >= 2u ? 17u : 0u));
                PPR_TEST_ASSERT(*right == *replay);

                const std::error_code err = parallel::forEach(
                    std::array{ChunkPos{0u, 31u}, ChunkPos{31u, 0u}, ChunkPos{31u, 31u}}, [&](const ChunkPos &corner) {
                        const auto cells = raster->chunk(corner);
                        PPR_TEST_ASSERT(cells.has_value());
                    });
                PPR_TEST_ASSERT(not err);
            }
        };

        PPR_UNIT_TEST (biome_sample_forces_selected_context_without_start_or_foreign_geometry) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0});
            recipe.m_biomes.push_back(recipe.m_biomes.front());
            recipe.m_biomes.back().m_key = 204u;
            recipe.m_biomes.back().m_bands[0u].m_choices[0u].m_material = 33u;
            recipe.m_biomes.back().m_erosion_passes = 8u;
            recipe.m_rules.push_back(
                {501u, 1u, EGenerationPhase::carve,
                    GenerationWorm{
                        .m_biome = 0u,
                        .m_count = 1u,
                        .m_carve = true,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 1.0,
                        .m_length_max_m = 1.0,
                        .m_radius_min_m = 0.2,
                        .m_radius_max_m = 0.2}});
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto layout = compileBiomeSampleLayout(*plan, 1u);
            PPR_TEST_ASSERT(layout.has_value() and layout->sampleBiome() == 1u);
            PPR_TEST_ASSERT(generationBiomeAt(*plan, *layout, 670.0, 1005.0) == 1u);
            const auto geometry = compileBiomeSampleGeometry(*plan, *layout);
            PPR_TEST_ASSERT(geometry.has_value() and geometry->m_instances.empty() and geometry->m_segments.empty());
            const auto raster = compileGenerationRaster(*plan, *layout, *geometry);
            PPR_TEST_ASSERT(raster.has_value());
            const auto first = raster->chunk({15u, 23u});
            const auto replay = raster->chunk({15u, 23u});
            PPR_TEST_ASSERT(first.has_value() and replay.has_value() and *first == *replay);
            PPR_TEST_ASSERT(std::ranges::all_of(*first, [](const Cell cell) { return cell.m_element == 33u; }));

            const auto invalid = compileBiomeSampleLayout(*plan, 2u);
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);

            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            const auto cancelled = compileBiomeSampleGeometry(*plan, *layout, context);
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        };

        PPR_UNIT_TEST (particle_seeding_replays_large_regions_with_coherent_positive_mass) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({99u, false, false, EGenerationMatter::liquid, 1000.0});
            ChunkGrid grid;
            InitialMatter matter;
            InitialMatterRegion region{
                .m_identity = 1u, .m_material = 99u, .m_temperature_k = 293.0, .m_bulk_density_kg_m3 = 1000.0,
                .m_spans = {}};
            for (u32 y = 10u; y < 110u; ++y) {
                region.m_spans.push_back({y, 10u, 1010u});
                for (u32 x = 10u; x < 1010u; ++x) {
                    region.m_volume_m3 += recipe.m_cell_size_m * recipe.m_cell_size_m;
                }
            }
            region.m_mass_kg = region.m_volume_m3 * region.m_bulk_density_kg_m3;
            matter.m_regions.push_back(region);
            matter.m_span_count = 100u;
            const auto seeded = seedMatterParticles(matter, grid, recipe);
            const auto replay = seedMatterParticles(matter, grid, recipe);
            PPR_TEST_ASSERT(seeded.has_value() and replay.has_value());

            double volume{}, mass{};
            u32 count{};
            for (u32 i = 0u; i < seeded->m_chunks.size(); ++i) {
                PPR_TEST_ASSERT(seeded->m_chunks[i].records().size() == replay->m_chunks[i].records().size());
                for (u32 j = 0u; j < seeded->m_chunks[i].records().size(); ++j) {
                    const auto &particle = seeded->m_chunks[i].records()[j];
                    PPR_TEST_ASSERT(particle == replay->m_chunks[i].records()[j]);
                    PPR_TEST_ASSERT(particle.m_volume_m3 > 0.0 and particle.m_mass_kg == particle.m_volume_m3 * 1000.0);
                    volume += particle.m_volume_m3;
                    mass += particle.m_mass_kg;
                    ++count;
                }
            }
            PPR_TEST_ASSERT(count == 100000u);
            PPR_TEST_ASSERT(std::abs(volume - region.m_volume_m3) < 1.0e-8 * region.m_volume_m3);
            PPR_TEST_ASSERT(std::abs(mass - region.m_mass_kg) < 1.0e-8 * region.m_mass_kg);
            matter.m_regions.front().m_volume_m3 *= 2.0;
            PPR_TEST_ASSERT(not seedMatterParticles(matter, grid, recipe));
        };

        PPR_UNIT_TEST (particle_edits_are_local_atomic_and_pure_digs_preserve_frozen_records) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({99u, false, false, EGenerationMatter::gas, 1.0});
            ChunkGrid grid;
            PPR_TEST_ASSERT(not grid.setCell({120u, 40u}, {17u, 293.0f}));
            const double volume = 2.0 * recipe.m_cell_size_m * recipe.m_cell_size_m;
            InitialMatter matter;
            matter.m_regions.push_back(
                {.m_identity = 1u,
                    .m_material = 99u,
                    .m_temperature_k = 293.0,
                    .m_bulk_density_kg_m3 = 1.0,
                    .m_volume_m3 = volume,
                    .m_mass_kg = volume,
                    .m_spans = {{40u, 127u, 129u}}});
            const auto seeded = seedMatterParticles(matter, grid, recipe);
            PPR_TEST_ASSERT(seeded.has_value());
            MatterParticleState state;
            PPR_TEST_ASSERT(not state.restore(*seeded, recipe.m_materials, recipe.m_cell_size_m));
            const auto frozen = state.capture();
            auto original = grid.captureChunk({0u, 0u});
            PPR_TEST_ASSERT(original.has_value());
            PPR_TEST_ASSERT(not grid.setCell({120u, 40u}, {}));
            grid.exchangeChunk(*original);
            std::array dig{std::move(*original)};
            const auto dug = state.prepareTerrainEdit(grid, dig, recipe.m_cell_size_m);
            PPR_TEST_ASSERT(dug.has_value() and dug->empty() and state.count() == 2u);
            auto old = grid.captureChunk({0u, 0u});
            PPR_TEST_ASSERT(old.has_value());
            PPR_TEST_ASSERT(not grid.setCell({127u, 40u}, {17u, 293.0f}));
            grid.exchangeChunk(*old);
            std::array build{std::move(*old)};
            const auto candidate = state.prepareTerrainEdit(grid, build, recipe.m_cell_size_m);
            PPR_TEST_ASSERT(candidate.has_value() and candidate->size() == 1u and state.count() == 2u);
            state.commitTerrainEdit(*candidate);
            PPR_TEST_ASSERT(state.count() == 1u and frozen.m_chunks[0].records().size() == 1u);
            PPR_TEST_ASSERT(state.chunk({1u, 0u})[0].m_id == 2u);
            auto [context, cancel] = context::withCancelClause(context::background());
            cancel(std::make_error_code(std::errc::operation_canceled));
            PPR_TEST_ASSERT(
                state.restore({}, recipe.m_materials, recipe.m_cell_size_m, context) == std::errc::operation_canceled);
            PPR_TEST_ASSERT(state.count() == 1u);
            auto invalid = state.capture();
            invalid.m_next_id = 2u;
            PPR_TEST_ASSERT(
                state.restore(invalid, recipe.m_materials, recipe.m_cell_size_m) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(state.count() == 1u);
        };

        PPR_UNIT_TEST (macro_layout_replays_spacing_and_spatial_nearest_matches_oracle) {
            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                auto plan = compileGenerationPlan(numericRecipe(), seed);
                PPR_TEST_ASSERT(plan.has_value());
                auto first = compileBiomeLayout(*plan), replay = compileBiomeLayout(*plan);
                PPR_TEST_ASSERT(first and replay);
                PPR_TEST_ASSERT(std::ranges::equal(first->sites(), replay->sites()));
                PPR_TEST_ASSERT(first->sites().size() == 16u);
                for (std::size_t a = 0u; a < first->sites().size(); ++a) {
                    PPR_TEST_ASSERT(first->sites()[a].m_biome == 0u);
                    for (std::size_t b = 0u; b < a; ++b) {
                        const double dx = first->sites()[a].m_x_m - first->sites()[b].m_x_m;
                        const double dy = first->sites()[a].m_y_m - first->sites()[b].m_y_m;
                        PPR_TEST_ASSERT(dx * dx + dy * dy >= 10000.0);
                    }
                }
                const auto check = [&](const double x, const double y) {
                    u32 expected = 0u;
                    double best = (std::numeric_limits<double>::max)();
                    for (u32 s = 0u; s < first->sites().size(); ++s) {
                        const double dx = first->sites()[s].m_x_m - x, dy = first->sites()[s].m_y_m - y;
                        const double distance = dx * dx + dy * dy;
                        if (distance < best) {
                            best = distance;
                            expected = s;
                        }
                    }
                    PPR_TEST_ASSERT(first->nearest(x, y) == expected);
                };
                for (i32 y = -200; y < 1600; y += 37) {
                    for (i32 x = -200; x < 1600; x += 43) {
                        check(x, y);
                    }
                }
                const double bin_width = (4096.0 / 3.0) / 64.0;
                for (u32 i = 0u; i < 64u; ++i) {
                    for (const double x:
                        {i * bin_width, std::nextafter(i * bin_width, 0.0), std::nextafter(i * bin_width, 2000.0)}) {
                        check(x, 437.5);
                    }
                }
            }
        };

        PPR_UNIT_TEST (macro_layout_reports_infeasible_constraints_and_cancellation) {
            auto recipe = numericRecipe();
            recipe.m_site_spacing_m = 4096.0 / 3.0;
            auto plan = compileGenerationPlan(std::move(recipe), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto exhausted = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(
                not exhausted and exhausted.error() == make_error_code(EGenerationErrc::constraints_exhausted));
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            auto cancelled = compileBiomeLayout(*plan, context);
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
            recipe = numericRecipe();
            recipe.m_constraints[0u].m_distance_min_m = 1.0;
            plan = compileGenerationPlan(std::move(recipe), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            exhausted = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(
                not exhausted and exhausted.error() == make_error_code(EGenerationErrc::constraints_exhausted));
        };

        PPR_UNIT_TEST (macro_layout_satisfies_required_biomes_before_weighted_assignment) {
            auto recipe = numericRecipe();
            recipe.m_max_sites = 64u;
            for (u32 i = 1u; i < 4u; ++i) {
                auto biome = recipe.m_biomes[0u];
                biome.m_key += i;
                recipe.m_biomes.push_back(std::move(biome));
                recipe.m_constraints.push_back({401u + i, i, 4u, 1.0, i == 3u ? 0.5 : 0.0, 1.0, 0.0, 2048.0});
            }
            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                auto plan = compileGenerationPlan(recipe, seed);
                PPR_TEST_ASSERT(plan.has_value());
                auto layout = compileBiomeLayout(*plan);
                PPR_TEST_ASSERT(layout.has_value());
                std::array<u32, 4u> counts{};
                for (const auto &site: layout->sites()) {
                    ++counts[site.m_biome];
                    if (site.m_biome == 3u) {
                        PPR_TEST_ASSERT(1.0 - site.m_y_m / (4096.0 / 3.0) >= 0.5);
                    }
                }
                for (const auto &constraint: recipe.m_constraints) {
                    PPR_TEST_ASSERT(counts[constraint.m_biome] >= constraint.m_required_count);
                }
            }
        };

        PPR_UNIT_TEST (nonlocal_geometry_replays_worms_and_links_only_traversal_rooms) {
            auto recipe = numericRecipe();
            auto room = recipe.m_features[0u];
            room.m_key = 701u;
            room.m_markers = {{702u, EGenerationMarker::traversal_room, 15, 15}};
            recipe.m_features.push_back(room);
            room.m_key = 703u;
            room.m_markers = {{704u, EGenerationMarker::sealed_pocket, 15, 15}};
            room.m_markers.push_back({705u, EGenerationMarker::traversal_room, 15, 15});
            recipe.m_features.push_back(room);
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationWorm{
                        .m_count = 2u,
                        .m_carve = true,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 3.25,
                        .m_length_max_m = 3.25,
                        .m_radius_min_m = 0.5,
                        .m_radius_max_m = 0.5,
                        .m_turn_probability = 0.5}});
            recipe.m_rules.push_back(
                {.m_key = 602u,
                    .m_order = 1u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationConnection{1.0, 1u}});
            recipe.m_rules.push_back(
                {.m_key = 603u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{
                        .m_feature = 1u,
                        .m_min_count = 3u,
                        .m_max_count = 3u,
                        .m_attempts = 64u,
                        .m_depth_max = 1.0,
                        .m_spacing_m = 40.0}});
            recipe.m_rules.push_back(
                {.m_key = 604u,
                    .m_order = 1u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{
                        .m_feature = 2u,
                        .m_min_count = 1u,
                        .m_max_count = 1u,
                        .m_attempts = 64u,
                        .m_depth_max = 1.0,
                        .m_spacing_m = 40.0}});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            auto first = compileGenerationGeometry(*plan, *layout), replay = compileGenerationGeometry(*plan, *layout);
            PPR_TEST_ASSERT(first and replay);
            PPR_TEST_ASSERT(std::ranges::equal(first->m_instances, replay->m_instances));
            PPR_TEST_ASSERT(std::ranges::equal(first->m_segments, replay->m_segments));
            PPR_TEST_ASSERT(first->m_instances.size() == 5u and first->m_segments.size() == 11u);
            PPR_TEST_ASSERT(first->m_omitted_instances.empty());
            PPR_TEST_ASSERT(first->m_worst_placement_attempts > 0u and first->m_worst_placement_attempts <= 64u);
            u32 links = 0u;
            for (const auto &segment: first->m_segments) {
                if (segment.m_rule != 1u) {
                    continue;
                }
                ++links;
                for (const auto point:
                    {std::pair{segment.m_x0_m, segment.m_y0_m}, std::pair{segment.m_x1_m, segment.m_y1_m}}) {
                    PPR_TEST_ASSERT(std::ranges::any_of(first->m_instances, [&](const auto &instance) {
                        return instance.m_feature == 1u and
                               point.first == (instance.m_origin_x + 15.5) / 3.0 and
                               point.second == (instance.m_origin_y + 15.5) / 3.0;
                    }));
                }
            }
            PPR_TEST_ASSERT(links == 3u);
        };

        PPR_UNIT_TEST (nonlocal_geometry_distinguishes_required_exhaustion_optional_omission_and_limits) {
            auto recipe = numericRecipe();
            auto huge = recipe.m_features[0u];
            huge.m_key = 701u;
            huge.m_bounds = {0, 0, 4096, 4096};
            recipe.m_features.push_back(huge);
            recipe.m_rules.push_back(
                {.m_key = 603u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{
                        .m_feature = 1u, .m_min_count = 1u, .m_max_count = 1u, .m_attempts = 64u, .m_depth_max = 1.0}});
            const auto run = [&] {
                auto plan = compileGenerationPlan(recipe, 42u);
                PPR_TEST_ASSERT(plan.has_value());
                auto layout = compileBiomeLayout(*plan);
                PPR_TEST_ASSERT(layout.has_value());
                return compileGenerationGeometry(*plan, *layout);
            };
            auto result = run();
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EGenerationErrc::constraints_exhausted));
            std::get<GenerationPlacement>(recipe.m_rules[0u].m_parameters).m_min_count = 0u;
            result = run();
            PPR_TEST_ASSERT(result and result->m_instances.size() == 1u);
            PPR_TEST_ASSERT(result->m_omitted_instances == Array<u32>{0u});
            recipe.m_rules = {
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationWorm{
                        .m_count = 1024u,
                        .m_carve = true,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 4096.0,
                        .m_length_max_m = 4096.0,
                        .m_radius_min_m = 0.5,
                        .m_radius_max_m = 0.5}}};
            result = run();
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EGenerationErrc::segment_limit));
            std::get<GenerationWorm>(recipe.m_rules.front().m_parameters).m_count = 1025u;
            PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::value_too_large);
        };

        PPR_UNIT_TEST (finite_blobs_and_smooth_worms_are_bounded_and_replay) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            GenerationDeposit deposit{
                .m_key = 501u,
                .m_material = 33u,
                .m_host = 17u,
                .m_depth_max = 1.0,
                .m_threshold_min = -1.0,
                .m_threshold_max = 1.0,
                .m_shape = EGenerationDepositShape::blob,
                .m_blob_count = 2u,
                .m_radius_min_m = 4.0,
                .m_radius_max_m = 8.0,
                .m_stretch_min = 0.5,
                .m_stretch_max = 3.0,
                .m_distortion = 0.3};
            recipe.m_biomes[0u].m_deposits.push_back(deposit);
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationWorm{
                        .m_count = 1u,
                        .m_carve = true,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 80.25,
                        .m_length_max_m = 80.25,
                        .m_radius_min_m = 1.0,
                        .m_radius_max_m = 3.0,
                        .m_turn_probability = 0.5,
                        .m_turn_smoothing = 0.85,
                        .m_radius_variation = 1.0,
                        .m_chamber_probability = 0.1,
                        .m_chamber_scale = 3.0}});
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            const auto first = compileGenerationGeometry(*plan, *layout);
            const auto replay = compileGenerationGeometry(*plan, *layout);
            PPR_TEST_ASSERT(first and replay);
            PPR_TEST_ASSERT(first->m_blobs == replay->m_blobs and first->m_blobs.size() == 2u);
            PPR_TEST_ASSERT(first->m_segments == replay->m_segments and first->m_segments.size() == 81u);
            bool varied_radius = false;
            for (u32 i = 0u; i < first->m_segments.size(); ++i) {
                const auto &segment = first->m_segments[i];
                PPR_TEST_ASSERT(segment.m_radius_m >= 1.0 and segment.m_radius_m <= 9.0);
                if (i == 0u) {
                    continue;
                }
                const auto &previous = first->m_segments[i - 1u];
                PPR_TEST_ASSERT(segment.m_x0_m == previous.m_x1_m and segment.m_y0_m == previous.m_y1_m);
                const double ax = previous.m_x1_m - previous.m_x0_m, ay = previous.m_y1_m - previous.m_y0_m;
                const double bx = segment.m_x1_m - segment.m_x0_m, by = segment.m_y1_m - segment.m_y0_m;
                PPR_TEST_ASSERT(std::abs(std::atan2(ax * by - ay * bx, ax * bx + ay * by)) <= 0.351);
                varied_radius = varied_radius or segment.m_radius_m != previous.m_radius_m;
            }
            PPR_TEST_ASSERT(varied_radius);
            deposit.m_key = 502u;
            deposit.m_order = 1u;
            recipe.m_biomes[0u].m_deposits.push_back(deposit);
            const auto extended_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(extended_plan.has_value());
            const auto extended_layout = compileBiomeLayout(*extended_plan);
            PPR_TEST_ASSERT(extended_layout.has_value());
            const auto extended = compileGenerationGeometry(*extended_plan, *extended_layout);
            PPR_TEST_ASSERT(extended.has_value() and extended->m_blobs.size() == 4u);
            PPR_TEST_ASSERT(
                first->m_blobs[0u] == extended->m_blobs[0u] and first->m_blobs[1u] == extended->m_blobs[1u]);
            for (const auto bounds: {std::pair{80.0, 100.0}, std::pair{90.0, 90.0}}) {
                auto oriented = recipe;
                auto &lens = oriented.m_biomes[0u].m_deposits[0u];
                lens.m_orientation_min_degrees = bounds.first;
                lens.m_orientation_max_degrees = bounds.second;
                const auto oriented_plan = compileGenerationPlan(std::move(oriented), 42u);
                PPR_TEST_ASSERT(oriented_plan.has_value());
                const auto oriented_geometry = compileGenerationGeometry(*oriented_plan, *layout);
                PPR_TEST_ASSERT(oriented_geometry.has_value());
                for (u32 i = 0u; i < 2u; ++i) {
                    const auto &blob = oriented_geometry->m_blobs[i];
                    PPR_TEST_ASSERT(
                        blob.m_angle >= 2.0 * std::numbers::pi * (bounds.first / 360.0) and
                        blob.m_angle <= 2.0 * std::numbers::pi * (bounds.second / 360.0));
                    PPR_TEST_ASSERT(blob.m_x_m == first->m_blobs[i].m_x_m and blob.m_y_m == first->m_blobs[i].m_y_m);
                    PPR_TEST_ASSERT(
                        blob.m_radius_m == first->m_blobs[i].m_radius_m and
                        blob.m_stretch == first->m_blobs[i].m_stretch);
                    PPR_TEST_ASSERT(
                        blob.m_offset_x == first->m_blobs[i].m_offset_x and
                        blob.m_offset_y == first->m_blobs[i].m_offset_y);
                }
                PPR_TEST_ASSERT(compileGenerationRaster(*oriented_plan, *layout, *oriented_geometry).has_value());
                auto malformed = *oriented_geometry;
                malformed.m_blobs[0u].m_angle = 2.0 * std::numbers::pi * (bounds.second / 360.0) + 0.01;
                PPR_TEST_ASSERT(not compileGenerationRaster(*oriented_plan, *layout, malformed).has_value());
            }
            for (const auto bounds:
                {std::pair{100.0, 80.0}, std::pair{-1.0, 90.0}, std::pair{0.0, 361.0},
                    std::pair{0.0, (std::numeric_limits<double>::quiet_NaN)()}}) {
                auto invalid_orientation = recipe;
                auto &lens = invalid_orientation.m_biomes[0u].m_deposits[0u];
                lens.m_orientation_min_degrees = bounds.first;
                lens.m_orientation_max_degrees = bounds.second;
                PPR_TEST_ASSERT(validateGenerationRecipe(invalid_orientation) == std::errc::invalid_argument);
            }
            recipe.m_biomes[0u].m_deposits[0u].m_radius_min_m = 0.0;
            PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::invalid_argument);
            recipe.m_biomes[0u].m_deposits[0u].m_radius_min_m = 4.0;
            recipe.m_biomes[0u].m_relaxation_passes = 25u;
            PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::invalid_argument);
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            const auto cancelled = compileGenerationGeometry(*plan, *layout, context);
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);

            // The target occupies a four-metre square in a 4096-metre world.
            // Global rejection sampling cannot reliably admit these origins.
            auto tiny = numericRecipe();
            tiny.m_cell_size_m = 1.0;
            tiny.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            tiny.m_biomes.push_back(tiny.m_biomes.front());
            tiny.m_biomes.back().m_key = 204u;
            tiny.m_constraints.push_back({402u, 1u, 1u, 1.0, 0.0, 1.0, 0.0, 2048.0});
            tiny.m_biomes.back().m_deposits.push_back(
                {.m_key = 701u,
                    .m_material = 33u,
                    .m_host = 17u,
                    .m_depth_min = 1.0 - 1000.25 / kWorldEdge,
                    .m_depth_max = 1.0 - 999.75 / kWorldEdge,
                    .m_shape = EGenerationDepositShape::blob,
                    .m_blob_count = 32u,
                    .m_radius_min_m = 0.5,
                    .m_radius_max_m = 1.0});
            constexpr std::array tiny_sites{
                GenerationSite{1000.0, 1000.0, 1u}, GenerationSite{996.0, 1000.0, 0u},
                GenerationSite{1004.0, 1000.0, 0u}, GenerationSite{1000.0, 996.0, 0u},
                GenerationSite{1000.0, 1004.0, 0u}};
            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                const auto tiny_plan = compileGenerationPlan(tiny, seed);
                PPR_TEST_ASSERT(tiny_plan.has_value());
                const auto tiny_layout = restoreBiomeLayout(*tiny_plan, tiny_sites);
                PPR_TEST_ASSERT(tiny_layout.has_value());
                u64 scratch = 0u;
                const auto admitted = compileGenerationGeometry(*tiny_plan, *tiny_layout, nullptr, &scratch);
                PPR_TEST_ASSERT(admitted.has_value() and admitted->m_blobs.size() == 32u and scratch > 0u);
                for (const auto &blob: admitted->m_blobs) {
                    PPR_TEST_ASSERT(blob.m_x_m >= 998.0 and blob.m_x_m <= 1002.0);
                    PPR_TEST_ASSERT(blob.m_y_m >= 999.75 and blob.m_y_m <= 1000.25);
                    PPR_TEST_ASSERT(generationBiomeAt(*tiny_plan, *tiny_layout, blob.m_x_m, blob.m_y_m) == 1u);
                }
                PPR_TEST_ASSERT(std::ranges::any_of(admitted->m_blobs, [&](const auto &blob) {
                    return blob.m_x_m != admitted->m_blobs.front().m_x_m and
                           blob.m_y_m != admitted->m_blobs.front().m_y_m;
                }));
            }
            tiny.m_biomes.back().m_deposits.front().m_depth_min = 0.1;
            tiny.m_biomes.back().m_deposits.front().m_depth_max = 0.2;
            const auto absent_plan = compileGenerationPlan(tiny, 42u);
            PPR_TEST_ASSERT(absent_plan.has_value());
            const auto absent_layout = restoreBiomeLayout(*absent_plan, tiny_sites);
            PPR_TEST_ASSERT(absent_layout.has_value());
            const auto absent = compileGenerationGeometry(*absent_plan, *absent_layout);
            PPR_TEST_ASSERT(not absent and absent.error() == make_error_code(EGenerationErrc::constraints_exhausted));
        };

        PPR_UNIT_TEST (coherent_vacuum_palette_is_salted_before_bounded_relaxation) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0});
            recipe.m_fields[0u].m_noise.m_algorithm = ENoiseAlgorithm::cellular_value;
            recipe.m_fields[0u].m_noise.m_frequency_per_metre = 0.12;
            auto &biome = recipe.m_biomes[0u];
            biome.m_bands[0u].m_choices[0u].m_weight = 3.0;
            biome.m_bands[0u].m_choices.push_back({204u, 0u, 1.0});
            biome.m_bands[0u].m_choices.push_back({205u, 33u, 1.0});
            biome.m_sprinkle_probability = 0.4;
            const auto salted = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(salted.has_value());
            const auto layout = compileBiomeLayout(*salted);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 70, 80, false});
            const auto raw_raster = compileGenerationRaster(*salted, *layout, geometry);
            PPR_TEST_ASSERT(raw_raster.has_value());
            const auto raw = raw_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(raw.has_value());
            recipe.m_biomes[0u].m_sprinkle_probability = 0.0;
            const auto coherent = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(coherent.has_value());
            const auto coherent_raster = compileGenerationRaster(*coherent, *layout, geometry);
            PPR_TEST_ASSERT(coherent_raster.has_value());
            const auto unsalted = coherent_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(unsalted.has_value() and *raw != *unsalted);
            recipe.m_biomes[0u].m_sprinkle_probability = 0.4;
            recipe.m_biomes[0u].m_relaxation_passes = 1u;
            recipe.m_biomes[0u].m_relaxation_adoption = 0.32;
            recipe.m_biomes[0u].m_finish_passes = 2u;
            recipe.m_biomes[0u].m_finish_adoption = 0.12;
            const auto tweaked_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(tweaked_plan.has_value());
            const auto tweaked_raster = compileGenerationRaster(*tweaked_plan, *layout, geometry);
            PPR_TEST_ASSERT(tweaked_raster.has_value());
            const auto tweaked = tweaked_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(tweaked.has_value());
            recipe.m_biomes[0u].m_erosion_passes = 1u;
            const auto modal_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(modal_plan.has_value());
            const auto modal_raster = compileGenerationRaster(*modal_plan, *layout, geometry);
            PPR_TEST_ASSERT(modal_raster.has_value());
            const auto modal = modal_raster->chunk({0u, 0u});
            const auto modal_replay = modal_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(modal and modal_replay and *modal == *modal_replay);
            constexpr std::array<u16, 3u> labels{0u, 17u, 33u};
            u32 excavated = 0u, filled = 0u, relabeled = 0u, retained = 0u;
            // The reference reads the completed tweaked/finish output, proving
            // erosion is appended. Unique pluralities need no tie assumption.
            for (u32 y = 4u; y < 76u; ++y) {
                for (u32 x = 4u; x < kChunkEdge - 1u; ++x) {
                    std::array<u32, 3u> histogram{};
                    for (i32 dy = -1; dy <= 1; ++dy) {
                        for (i32 dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 and dy == 0) {
                                continue;
                            }
                            const auto neighbor =
                                (*tweaked)[static_cast<std::size_t>((static_cast<i64>(y) + dy) * kChunkEdge + x + dx)]
                                    .m_element;
                            const auto label = std::ranges::find(labels, neighbor);
                            PPR_TEST_ASSERT(label != labels.end());
                            ++histogram[static_cast<std::size_t>(label - labels.begin())];
                        }
                    }
                    const auto maximum = std::ranges::max_element(histogram);
                    if (std::ranges::count(histogram, *maximum) != 1) {
                        continue;
                    }
                    const u16 expected = labels[static_cast<std::size_t>(maximum - histogram.begin())];
                    const auto before = (*tweaked)[y * kChunkEdge + x].m_element;
                    const auto &after = (*modal)[y * kChunkEdge + x];
                    PPR_TEST_ASSERT(after.m_element == expected or after.m_element == before);
                    PPR_TEST_ASSERT(after.m_element == 0u ? after.m_temperature == 0.0f : after.m_temperature > 0.0f);
                    if (before == after.m_element) {
                        retained += before != expected ? 1u : 0u;
                        continue;
                    }
                    excavated += before != 0u and expected == 0u ? 1u : 0u;
                    filled += before == 0u and expected != 0u ? 1u : 0u;
                    relabeled += before != 0u and expected != 0u and before != expected ? 1u : 0u;
                }
            }
            PPR_TEST_ASSERT(excavated > 0u and filled > 0u and relabeled > 0u and retained > 0u);
            recipe.m_biomes[0u].m_relaxation_passes = 21u;
            recipe.m_biomes[0u].m_relaxation_adoption = 0.5;
            recipe.m_biomes[0u].m_finish_passes = 0u;
            recipe.m_biomes[0u].m_erosion_passes = 0u;
            const auto relaxed = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(relaxed.has_value());
            const auto smooth_raster = compileGenerationRaster(*relaxed, *layout, geometry);
            PPR_TEST_ASSERT(smooth_raster.has_value());
            const auto smooth = smooth_raster->chunk({0u, 0u});
            const auto replay = smooth_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(smooth and replay and *smooth == *replay);
            PPR_TEST_ASSERT(std::ranges::any_of(*smooth, [](const Cell &cell) { return cell.m_element == 0u; }));
            const auto edges = [](const Array<Cell> &cells) {
                u32 changes = 0u;
                for (u32 y = 1u; y < kChunkEdge; ++y) {
                    for (u32 x = 1u; x < kChunkEdge; ++x) {
                        const u32 at = y * kChunkEdge + x;
                        changes += cells[at].m_element != cells[at - 1u].m_element ? 1u : 0u;
                        changes += cells[at].m_element != cells[at - kChunkEdge].m_element ? 1u : 0u;
                    }
                }
                return changes;
            };
            PPR_TEST_ASSERT(edges(*smooth) < edges(*raw));
            for (u32 y = 0u; y < kChunkEdge; ++y) {
                PPR_TEST_ASSERT((*smooth)[y * kChunkEdge].m_element == recipe.m_boundary_material);
            }
            for (u32 y = 80u; y < 110u; ++y) {
                for (u32 x = 70u; x < 100u; ++x) {
                    PPR_TEST_ASSERT((*smooth)[y * kChunkEdge + x].m_element == 0u);
                }
            }
            u32 natural_empty = 0u;
            for (u32 y = 3u; y < 80u; ++y) {
                for (u32 x = 3u; x < kChunkEdge; ++x) {
                    natural_empty += (*smooth)[y * kChunkEdge + x].m_element == 0u ? 1u : 0u;
                }
            }
            PPR_TEST_ASSERT(natural_empty > 0u);
            for (const auto &cell: *smooth) {
                PPR_TEST_ASSERT(cell.m_element == 0u ? cell.m_temperature == 0.0f : cell.m_temperature > 0.0f);
            }
            for (const double probability: {-0.1, 1.1, (std::numeric_limits<double>::quiet_NaN)()}) {
                recipe.m_biomes[0u].m_sprinkle_probability = probability;
                PPR_TEST_ASSERT(validateGenerationRecipe(recipe) == std::errc::invalid_argument);
            }
        };

        PPR_UNIT_TEST (relaxation_crosses_chunk_seams_preserves_reservations_and_precedes_detail) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            recipe.m_materials.push_back({49u, true, true, EGenerationMatter::solid, 2500.0});
            auto &biome = recipe.m_biomes[0u];
            biome.m_relaxation_passes = 1u;
            biome.m_relaxation_adoption = 1.0;
            biome.m_deposits.push_back(
                {.m_key = 501u,
                    .m_material = 33u,
                    .m_host = 17u,
                    .m_depth_max = 1.0,
                    .m_threshold_min = -1.0,
                    .m_threshold_max = 1.0,
                    .m_shape = EGenerationDepositShape::blob,
                    .m_blob_count = 1u,
                    .m_radius_min_m = 1.5,
                    .m_radius_max_m = 1.5});
            biome.m_deposits.push_back(
                {.m_key = 502u,
                    .m_order = 1u,
                    .m_material = 49u,
                    .m_host = 17u,
                    .m_depth_max = 1.0,
                    .m_threshold_min = -1.0,
                    .m_threshold_max = 1.0,
                    .m_shape = EGenerationDepositShape::blob,
                    .m_stage = EGenerationDepositStage::detail,
                    .m_blob_count = 1u,
                    .m_radius_min_m = 0.2,
                    .m_radius_max_m = 0.2});
            recipe.m_rules.push_back(
                {.m_key = 601u, .m_phase = EGenerationPhase::carve, .m_parameters = GenerationConnection{0.2, 0u}});
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 127, 80, false});
            geometry.m_segments.push_back({0u, 120.5, 70.5, 140.5, 70.5, 0.2});
            geometry.m_blobs.push_back({0u, 0u, 0u, 127.5, 64.5, 1.5, 1.0, 0.0, 0.0, 0.0});
            geometry.m_blobs.push_back({0u, 1u, 0u, 128.5, 63.5, 0.2, 1.0, 0.0, 0.0, 0.0});
            const auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            const auto right = raster->chunk({1u, 0u}), left = raster->chunk({0u, 0u});
            const auto replay = raster->chunk({1u, 0u});
            PPR_TEST_ASSERT(left and right and replay and *right == *replay);
            PPR_TEST_ASSERT((*left)[64u * kChunkEdge + 127u].m_element == 33u);
            PPR_TEST_ASSERT((*left)[63u * kChunkEdge + 126u].m_element == 17u);
            PPR_TEST_ASSERT((*right)[64u * kChunkEdge].m_element == 33u);
            PPR_TEST_ASSERT((*right)[63u * kChunkEdge].m_element == 49u);
            PPR_TEST_ASSERT((*left)[70u * kChunkEdge + 127u].m_element == 0u);
            PPR_TEST_ASSERT((*right)[70u * kChunkEdge].m_element == 0u);
            PPR_TEST_ASSERT((*left)[80u * kChunkEdge + 127u].m_element == 0u);
            PPR_TEST_ASSERT((*right)[80u * kChunkEdge].m_element == 0u);
            for (const auto &cell: *right) {
                PPR_TEST_ASSERT(cell.m_element == 0u ? cell.m_temperature == 0.0f : cell.m_temperature > 0.0f);
            }
            geometry.m_blobs[0u].m_stretch = std::numeric_limits<double>::infinity();
            const auto invalid = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (absent_biome_pass_maxima_preserve_sampled_chunk_output) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0});
            recipe.m_materials.push_back({49u, true, true, EGenerationMatter::solid, 3000.0});
            recipe.m_fields[0u].m_noise.m_algorithm = ENoiseAlgorithm::cellular_value;
            recipe.m_fields[0u].m_noise.m_frequency_per_metre = 0.12;

            auto &active = recipe.m_biomes.front();
            active.m_bands[0u].m_choices[0u].m_weight = 3.0;
            active.m_bands[0u].m_choices.push_back({204u, 0u, 1.0});
            active.m_bands[0u].m_choices.push_back({205u, 33u, 1.0});
            active.m_sprinkle_probability = 0.4;
            active.m_pore_probability = 0.15;
            active.m_pore_affinity_min = -1.0;
            active.m_pore_passes = 1u;
            active.m_relaxation_passes = 1u;
            active.m_relaxation_adoption = 0.7;
            active.m_finish_passes = 0u;
            active.m_erosion_passes = 1u;
            active.m_deposits.push_back(
                {.m_key = 501u,
                    .m_material = 49u,
                    .m_host = 17u,
                    .m_depth_max = 1.0,
                    .m_threshold_min = -1.0,
                    .m_threshold_max = 1.0,
                    .m_stage = EGenerationDepositStage::detail});

            // The absent biome still sets global stage offsets: detail at pass 5
            // and erosion at pass 7. Removing it changes the random draw identities.
            recipe.m_biomes.push_back(recipe.m_biomes.front());
            auto &absent = recipe.m_biomes.back();
            absent.m_key = 206u;
            absent.m_pore_passes = 2u;
            absent.m_relaxation_passes = 3u;
            absent.m_finish_passes = 2u;
            absent.m_erosion_passes = 2u;

            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());

            const auto layout = compileBiomeSampleLayout(*plan, 0u);
            PPR_TEST_ASSERT(layout.has_value() and layout->sampleBiome() == 0u);

            GenerationGeometry geometry;
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 120, 80, false});
            const auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());

            const auto left = raster->chunk({0u, 0u});
            const auto right = raster->chunk({1u, 0u});
            PPR_TEST_ASSERT(left.has_value() and right.has_value());

            const auto right_replay = raster->chunk({1u, 0u});
            const auto left_replay = raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(right_replay.has_value() and left_replay.has_value());
            PPR_TEST_ASSERT(*left == *left_replay and *right == *right_replay);

            for (const auto *const cells: {std::addressof(*left), std::addressof(*right)}) {
                PPR_TEST_ASSERT(std::ranges::any_of(*cells, [](const Cell cell) { return cell.m_element == 33u; }));
                PPR_TEST_ASSERT(std::ranges::any_of(*cells, [](const Cell cell) { return cell.m_element == 49u; }));

                for (const auto &cell: *cells) {
                    PPR_TEST_ASSERT(
                        cell.m_element == 0u or
                        cell.m_element == 17u or
                        cell.m_element == 33u or
                        cell.m_element == 49u);
                    PPR_TEST_ASSERT(cell.m_element == 0u ? cell.m_temperature == 0.0f : cell.m_temperature > 0.0f);
                }

                for (u32 x = 0u; x < kChunkEdge; ++x) {
                    PPR_TEST_ASSERT((*cells)[x].m_element == recipe.m_boundary_material);
                }
            }

            // The same protected carve crosses the independently rasterized seam.
            for (u32 y = 80u; y < 110u; ++y) {
                for (u32 x = 120u; x < 150u; ++x) {
                    const auto &cells = x < kChunkEdge ? *left : *right;
                    PPR_TEST_ASSERT(cells[y * kChunkEdge + x % kChunkEdge] == Cell{});
                }
            }

            const auto checksum = [](const Array<Cell> &cells) {
                hash_t result{hash::default_seed_v};

                for (const Cell &cell: cells) {
                    result = hash::combine(result, cell.m_element, std::bit_cast<u32>(cell.m_temperature));
                }

                return result.m_value;
            };

            // Pin the full global-pass output using semantic fields, never Cell padding.
            PPR_TEST_ASSERT(checksum(*left) == 0x40b7d1729d078c3bull);
            PPR_TEST_ASSERT(checksum(*right) == 0x5cac8b4b33fa15b9ull);
        };

        PPR_UNIT_TEST (chunk_raster_preserves_boundaries_host_replacement_and_global_seams) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            recipe.m_materials.push_back({49u, true, true, EGenerationMatter::solid, 3000.0});
            recipe.m_materials.push_back({1001u, false, false, EGenerationMatter::liquid, 1000.0});
            recipe.m_biomes[0u].m_deposits.push_back({501u, 0u, 0u, 33u, 17u, 0.0, 1.0, -1.0, 1.0});
            recipe.m_biomes[0u].m_deposits.push_back({502u, 1u, 0u, 49u, 17u, 0.0, 1.0, -1.0, 1.0});
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::deposit,
                    .m_parameters = GenerationWorm{
                        .m_count = 1u,
                        .m_depth_max = 1.0,
                        .m_length_min_m = 1.0,
                        .m_length_max_m = 1.0,
                        .m_radius_min_m = 1.0 / 3.0,
                        .m_radius_max_m = 1.0 / 3.0,
                        .m_material = 49u,
                        .m_host = 33u}});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 2000, 3000, false});
            geometry.m_segments.push_back({0u, 42.0, 64.5 / 3.0, 44.0, 64.5 / 3.0, 1.0 / 3.0});
            auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            const auto first = raster->chunk({0u, 0u}), second = raster->chunk({1u, 0u});
            PPR_TEST_ASSERT(first and second);
            PPR_TEST_ASSERT((*first)[0u].m_element == 17u);
            PPR_TEST_ASSERT((*first)[64u * kChunkEdge + 20u].m_element == 33u);
            PPR_TEST_ASSERT((*first)[64u * kChunkEdge + 127u].m_element == 49u);
            PPR_TEST_ASSERT((*second)[64u * kChunkEdge].m_element == 49u);
            for (const auto &cell: *first) {
                PPR_TEST_ASSERT(cell.m_element == 17u or cell.m_element == 33u or cell.m_element == 49u);
                PPR_TEST_ASSERT(cell.m_temperature >= 285.0f and cell.m_temperature <= 301.0f);
            }
            const auto second_replay = raster->chunk({1u, 0u}), first_replay = raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(first_replay and second_replay);
            PPR_TEST_ASSERT(*first == *first_replay and *second == *second_replay);
            const auto invalid = raster->chunk({32u, 0u});
            PPR_TEST_ASSERT(not invalid and invalid.error() == std::errc::result_out_of_range);
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            const auto cancelled = raster->chunk({0u, 0u}, context);
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        };

        PPR_UNIT_TEST (chunk_raster_opens_room_portals_but_preserves_mixed_role_sealed_pockets) {
            auto recipe = numericRecipe();
            auto room = recipe.m_features[0u];
            room.m_key = 701u;
            room.m_operations[0u].m_carve = false;
            room.m_operations[0u].m_material = 17u;
            room.m_markers = {{702u, EGenerationMarker::traversal_room, 15, 15}};
            recipe.m_features.push_back(room);
            room.m_key = 703u;
            room.m_markers.push_back({704u, EGenerationMarker::sealed_pocket, 15, 15});
            recipe.m_features.push_back(room);
            recipe.m_rules.push_back(
                {.m_key = 601u, .m_phase = EGenerationPhase::carve, .m_parameters = GenerationConnection{1.0, 0u}});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances = {
                {0u, kGenerationStartRule, 0u, 2000, 3000, false}, {1u, 1u, 0u, 30, 30, false},
                {2u, 2u, 0u, 90, 30, false}};
            geometry.m_segments.push_back({0u, 1.0, 45.5 / 3.0, 42.0, 45.5 / 3.0, 1.0});
            auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            auto chunk = raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(chunk.has_value());
            PPR_TEST_ASSERT((*chunk)[45u * kChunkEdge + 30u] == Cell{});
            PPR_TEST_ASSERT((*chunk)[45u * kChunkEdge + 59u] == Cell{});
            PPR_TEST_ASSERT((*chunk)[45u * kChunkEdge + 90u].m_element == 17u);
            PPR_TEST_ASSERT((*chunk)[45u * kChunkEdge + 119u].m_element == 17u);
            PPR_TEST_ASSERT((*chunk)[30u * kChunkEdge + 30u].m_element == 17u);
        };

        PPR_UNIT_TEST (chunk_raster_protects_template_reservations_from_cave_fields) {
            auto recipe = numericRecipe();
            recipe.m_features[0u].m_operations[0u].m_carve = false;
            recipe.m_features[0u].m_operations[0u].m_material = 17u;
            recipe.m_features[0u].m_operations.push_back(
                {.m_key = 999u, .m_order = 1u, .m_carve = true, .m_x = 4, .m_y = 4, .m_width = 22u, .m_height = 22u});
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::carve,
                    .m_parameters = GenerationCave{
                        .m_depth_max = 1.0, .m_threshold_min = -1.0, .m_threshold_max = 1.0}});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            auto geometry = compileGenerationGeometry(*plan, *layout);
            PPR_TEST_ASSERT(geometry.has_value());
            auto raster = compileGenerationRaster(*plan, *layout, *geometry);
            PPR_TEST_ASSERT(raster.has_value());
            auto chunk = raster->chunk({15u, 23u});
            PPR_TEST_ASSERT(chunk.has_value());
            const auto cell = [&](const u32 x, const u32 y) -> const Cell & {
                return (*chunk)[(y - 2944u) * kChunkEdge + x - 1920u];
            };
            PPR_TEST_ASSERT(cell(1999u, 3000u).m_element == 0u);
            PPR_TEST_ASSERT(cell(2000u, 3000u).m_element == 17u);
            PPR_TEST_ASSERT(cell(2015u, 3010u) == Cell{});
        };

        PPR_UNIT_TEST (matter_area_clips_shared_contours_and_horizontal_height) {
            ChunkGrid grid;
            constexpr double h = 1.0 / 3.0;
            const GlobalCellPos cell{64u, 64u};
            auto full = generationEmptyArea(grid, cell, h);
            PPR_TEST_ASSERT(full and *full == h * h);
            auto half = generationEmptyArea(grid, cell, h, 64.5 / 3.0);
            PPR_TEST_ASSERT(half and *half == 0.5 * h * h);
            for (u32 y = 63u; y <= 65u; ++y) {
                for (u32 x = 63u; x <= 65u; ++x) {
                    if (x != 64u or y != 64u) {
                        PPR_TEST_ASSERT(not grid.setCell({x, y}, {17u, 293.0f}));
                    }
                }
            }
            auto isolated = generationEmptyArea(grid, cell, h);
            PPR_TEST_ASSERT(isolated and *isolated == 0.5 * h * h);
            auto clipped = generationEmptyArea(grid, cell, h, 64.5 / 3.0);
            PPR_TEST_ASSERT(clipped and *clipped == 0.25 * h * h);
            PPR_TEST_ASSERT(not grid.setCell(cell, {17u, 293.0f}));
            auto solid = generationEmptyArea(grid, cell, h);
            PPR_TEST_ASSERT(solid and *solid == 0.0);
            PPR_TEST_ASSERT(not generationEmptyArea(grid, {4096u, 0u}, h));
            PPR_TEST_ASSERT(not generationEmptyArea(grid, cell, 0.0));
            PPR_TEST_ASSERT(not generationEmptyArea(grid, cell, (std::numeric_limits<double>::max)()));
            for (u32 mask = 0u; mask < 512u; ++mask) {
                for (u32 bit = 0u; bit < 9u; ++bit) {
                    PPR_TEST_ASSERT(not grid.setCell(
                        {63u + bit % 3u, 63u + bit / 3u}, {(mask & (1u << bit)) != 0u ? u16{17u} : u16{0u}, 293.0f}));
                }
                const auto whole = generationEmptyArea(grid, cell, h);
                const auto lower = generationEmptyArea(grid, cell, h, 64.5 / 3.0);
                PPR_TEST_ASSERT(whole and lower);
                PPR_TEST_ASSERT(*whole >= 0.0 and *whole <= 1.0 / 9.0);
                PPR_TEST_ASSERT(*lower >= 0.0 and *lower <= *whole);
            }
        };

        [[nodiscard]] GenerationRecipe matterRecipe() {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({1001u, false, false, EGenerationMatter::liquid, 1000.0});
            recipe.m_materials.push_back({1009u, false, false, EGenerationMatter::gas, 1.2});
            auto pocket = recipe.m_features[0u];
            pocket.m_key = 701u;
            pocket.m_bounds = {0, 0, 9, 9};
            pocket.m_operations[0u].m_width = 9u;
            pocket.m_operations[0u].m_height = 9u;
            pocket.m_markers = {{702u, EGenerationMarker::sealed_pocket, 4, 4}};
            recipe.m_features.push_back(pocket);
            recipe.m_rules.push_back(
                {.m_key = 601u,
                    .m_phase = EGenerationPhase::stamp,
                    .m_parameters = GenerationPlacement{
                        .m_feature = 1u, .m_min_count = 1u, .m_max_count = 1u, .m_attempts = 64u, .m_depth_max = 1.0}});
            recipe.m_rules.push_back(
                {.m_key = 602u,
                    .m_phase = EGenerationPhase::fill,
                    .m_parameters = GenerationFill{0u, 0u, 1001u, false, 290.0, 1000.0, 4.0 / 3.0}});
            recipe.m_rules.push_back(
                {.m_key = 603u,
                    .m_order = 1u,
                    .m_phase = EGenerationPhase::fill,
                    .m_parameters = GenerationFill{0u, 0u, 1009u, true, 290.0, 1.2, std::nullopt}});
            return recipe;
        }

        PPR_UNIT_TEST (vacuum_palette_preserves_unstamped_sealed_reservation_substrate) {
            auto recipe = matterRecipe();
            recipe.m_fields[0u].m_noise.m_algorithm = ENoiseAlgorithm::cellular_value;
            recipe.m_fields[0u].m_noise.m_frequency_per_metre = 0.12;
            auto &biome = recipe.m_biomes[0u];
            biome.m_bands[0u].m_choices.push_back({204u, 0u, 999.0});
            biome.m_sprinkle_probability = 0.4;
            biome.m_relaxation_passes = 21u;
            // The inner carve crosses the outer ellipse. Its corners require
            // unpainted substrate from the protected reservation to stay sealed.
            auto &pocket = recipe.m_features[1u];
            pocket.m_operations = {
                {.m_key = 703u,
                    .m_shape = EGenerationShape::ellipse,
                    .m_x = 1,
                    .m_y = 1,
                    .m_width = 7u,
                    .m_height = 7u,
                    .m_material = 17u},
                {.m_key = 704u, .m_order = 1u, .m_carve = true, .m_x = 1, .m_y = 1, .m_width = 7u, .m_height = 7u}};
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({1u, 0u, 0u, 60, 60, false});
            const auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            auto cells = raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(cells.has_value());
            ChunkGrid grid;
            PPR_TEST_ASSERT(not grid.setCells({0u, 0u}, std::move(*cells)));
            const auto matter = resolveInitialMatter(*plan, geometry, grid);
            PPR_TEST_ASSERT(matter and matter->m_regions.size() == 2u and matter->m_largest_component == 49u);
            PPR_TEST_ASSERT(grid.getCell({60u, 60u})->m_element == 17u);
            PPR_TEST_ASSERT(grid.getCell({61u, 61u})->m_element == 0u);
        };

        PPR_UNIT_TEST (open_surface_and_continuous_bedrock_override_every_terrain_phase) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_materials.push_back({633u, true, false, EGenerationMatter::solid, 3100.0});
            recipe.m_open_surface = true;
            recipe.m_surface_elevation_m = 220.0;
            recipe.m_bedrock_thickness_m = 25.0;
            recipe.m_bedrock_material = 633u;
            recipe.m_start_depth_m = 45.0;
            recipe.m_biomes[0u].m_pore_probability = 0.2;
            recipe.m_biomes[0u].m_pore_affinity_min = -1.0;
            recipe.m_biomes[0u].m_pore_passes = 3u;
            recipe.m_biomes[0u].m_relaxation_passes = 8u;
            recipe.m_biomes[0u].m_finish_passes = 3u;
            recipe.m_rules.push_back({501u, 1u, EGenerationPhase::carve, GenerationCave{0u, 0u, 0.0, 1.0, -1.0, 1.0}});
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan and plan->recipe().m_start_y_fine == 175u);
            PPR_TEST_ASSERT(generationDepth(*plan, 70.0, 220.0) == 0.0);
            PPR_TEST_ASSERT(generationDepth(*plan, 70.0, 25.0) == 1.0);
            const auto layout = restoreBiomeLayout(*plan, std::array{GenerationSite{100.0, 150.0, 0u}});
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            // Even a supplied carve template crossing the protected bedrock
            // cannot overwrite it; normal geometry rejects this placement.
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 10, 10, false});
            const auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            const auto bottom = raster->chunk({0u, 0u}), surface = raster->chunk({0u, 1u}),
                       top = raster->chunk({0u, 31u});
            PPR_TEST_ASSERT(bottom and surface and top);
            for (u32 x = 0u; x < kChunkEdge; ++x) {
                for (u32 y = 0u; y < 25u; ++y) {
                    PPR_TEST_ASSERT((*bottom)[y * kChunkEdge + x].m_element == 633u);
                }
                for (u32 y = 92u; y < kChunkEdge; ++y) {
                    PPR_TEST_ASSERT((*surface)[y * kChunkEdge + x] == Cell{});
                }
            }
            PPR_TEST_ASSERT(std::ranges::all_of(*top, [](const Cell &cell) { return cell == Cell{}; }));
            // The canonical metric multiplication differs by one ULP from
            // division through a reciprocal at this exact allowed surface.
            auto edge_recipe = numericRecipe();
            edge_recipe.m_materials.push_back({633u, true, false, EGenerationMatter::solid, 3100.0});
            edge_recipe.m_open_surface = true;
            edge_recipe.m_surface_elevation_m = 152.5 / 3.0;
            edge_recipe.m_bedrock_thickness_m = 25.0;
            edge_recipe.m_bedrock_material = 633u;
            edge_recipe.m_start_depth_m = 20.0;
            const auto edge_plan = compileGenerationPlan(edge_recipe, 42u);
            PPR_TEST_ASSERT(edge_plan.has_value());
            const auto edge_layout = restoreBiomeLayout(*edge_plan, std::array{GenerationSite{20.0, 35.0, 0u}});
            PPR_TEST_ASSERT(edge_layout.has_value());
            const GenerationGeometry edge_geometry;
            const auto edge_raster = compileGenerationRaster(*edge_plan, *edge_layout, edge_geometry);
            PPR_TEST_ASSERT(edge_raster.has_value());
            const auto edge_cells = edge_raster->chunk({0u, 1u});
            PPR_TEST_ASSERT(edge_cells.has_value());
            PPR_TEST_ASSERT((*edge_cells)[24u * kChunkEdge].m_element == 17u);
            PPR_TEST_ASSERT((*edge_cells)[25u * kChunkEdge] == Cell{});
            recipe.m_start_depth_m = 10.0;
            PPR_TEST_ASSERT(not compileGenerationPlan(recipe, 42u));
            recipe.m_start_depth_m = 45.0;
            recipe.m_materials.back().m_diggable = true;
            PPR_TEST_ASSERT(not compileGenerationPlan(recipe, 42u));
        };

        PPR_UNIT_TEST (warped_beds_and_clustered_pores_replay_through_split_filters) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0, 3301u});
            recipe.m_materials[0u].m_key = 1701u;
            auto &biome = recipe.m_biomes[0u];
            auto &band = biome.m_bands[0u];
            band.m_choices.push_back({204u, 33u, 1.0});
            band.m_bed_period_m = 22.0;
            band.m_bed_warp_m = 3.0;
            band.m_bed_dip = 0.05;
            biome.m_pore_probability = 0.2;
            biome.m_pore_affinity_min = -1.0;
            biome.m_sprinkle_probability = 0.2;
            const auto raw_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(raw_plan.has_value());
            const auto layout = restoreBiomeLayout(*raw_plan, std::array{GenerationSite{100.0, 100.0, 0u}});
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_instances.push_back({0u, kGenerationStartRule, 0u, 70, 80, false});
            const auto raw_raster = compileGenerationRaster(*raw_plan, *layout, geometry);
            PPR_TEST_ASSERT(raw_raster.has_value());
            const auto raw = raw_raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(raw.has_value());
            biome.m_pore_passes = 3u;
            biome.m_relaxation_passes = 8u;
            biome.m_finish_passes = 3u;
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto raster = compileGenerationRaster(*plan, *layout, geometry);
            PPR_TEST_ASSERT(raster.has_value());
            const auto left = raster->chunk({0u, 0u}), right = raster->chunk({1u, 0u}),
                       replay = raster->chunk({0u, 0u});
            PPR_TEST_ASSERT(left and right and replay and *left == *replay and *left != *raw);
            u32 pores = 0u, pore_changes = 0u, first_material = 0u, second_material = 0u;
            for (u32 y = 3u; y < 70u; ++y) {
                for (u32 x = 3u; x < kChunkEdge; ++x) {
                    const auto &cell = (*left)[y * kChunkEdge + x];
                    pores += cell.m_element == 0u ? 1u : 0u;
                    pore_changes += (cell.m_element == 0u) != ((*raw)[y * kChunkEdge + x].m_element == 0u) ? 1u : 0u;
                    first_material += cell.m_element == 17u ? 1u : 0u;
                    second_material += cell.m_element == 33u ? 1u : 0u;
                    PPR_TEST_ASSERT(cell.m_element == 0u ? cell.m_temperature == 0.0f : cell.m_temperature > 0.0f);
                }
            }
            PPR_TEST_ASSERT(pores > 0u and pore_changes > 0u and first_material > 0u and second_material > 0u);
            for (u32 y = 80u; y < 110u; ++y) {
                for (u32 x = 70u; x < 100u; ++x) {
                    PPR_TEST_ASSERT((*left)[y * kChunkEdge + x].m_element == 0u);
                }
            }
        };

        PPR_UNIT_TEST (supported_formations_use_frozen_contacts_and_preserve_backbone_clearance) {
            auto recipe = numericRecipe();
            recipe.m_cell_size_m = 1.0;
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0});
            auto &biome = recipe.m_biomes[0u];
            biome.m_formation_probability = 0.2;
            biome.m_formation_length_m = 5.0;
            biome.m_formation_material = 33u;
            biome.m_rind_material = 33u;
            recipe.m_rules.push_back({501u, 1u, EGenerationPhase::carve, GenerationConnection{2.2, 0u, 1u}});
            const auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            const auto layout = restoreBiomeLayout(*plan, std::array{GenerationSite{100.0, 100.0, 0u}});
            PPR_TEST_ASSERT(layout.has_value());
            GenerationGeometry geometry;
            geometry.m_segments = {{0u, 40.5, 60.5, 180.5, 60.5, 2.2}, {0u, 180.5, 60.5, 180.5, 90.5, 2.2}};
            ChunkGrid grid;
            for (u32 x = 16u; x < 400u; ++x) {
                PPR_TEST_ASSERT(not grid.setCell({x, 32u}, {17u, 293.0f}));
                PPR_TEST_ASSERT(not grid.setCell({x, 96u}, {17u, 293.0f}));
            }
            ChunkGrid region = grid;
            const GenerationCellBounds candidates{16u, 16u, 416u, 112u};
            ChunkGrid absent;
            PPR_TEST_ASSERT(
                finishGenerationTerrainRegion(*plan, *layout, geometry, absent, candidates) ==
                std::errc::invalid_argument);
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            PPR_TEST_ASSERT(
                finishGenerationTerrainRegion(*plan, *layout, geometry, region, candidates, context) ==
                std::errc::operation_canceled);
            u64 scratch{};
            PPR_TEST_ASSERT(
                not finishGenerationTerrainRegion(*plan, *layout, geometry, region, candidates, nullptr, &scratch));
            PPR_TEST_ASSERT(region.residentChunkCount() == 4u);
            PPR_TEST_ASSERT(not finishGenerationTerrain(*plan, *layout, geometry, grid, nullptr, &scratch));
            for (u32 x = 0u; x < 4u; ++x) {
                PPR_TEST_ASSERT(std::ranges::equal(*region.residentCells({x, 0u}), *grid.residentCells({x, 0u})));
            }
            PPR_TEST_ASSERT(scratch > 0u);
            u32 growth{};
            for (u32 y = 33u; y < 96u; ++y) {
                for (u32 x = 16u; x < 400u; ++x) {
                    growth += grid.getCell({x, y})->m_element == 33u ? 1u : 0u;
                }
            }
            PPR_TEST_ASSERT(growth > 0u);
            PPR_TEST_ASSERT(not validateGenerationBackbone(*plan, geometry, grid));
            PPR_TEST_ASSERT(not grid.setCell({100u, 60u}, {17u, 293.0f}));
            PPR_TEST_ASSERT(
                validateGenerationBackbone(*plan, geometry, grid) ==
                make_error_code(EGenerationErrc::constraints_exhausted));
            PPR_TEST_ASSERT(not grid.setCell({100u, 60u}, {}));
            geometry.m_segments.back().m_x0_m += 0.01;
            PPR_TEST_ASSERT(
                validateGenerationBackbone(*plan, geometry, grid) ==
                make_error_code(EGenerationErrc::constraints_exhausted));
        };

        PPR_UNIT_TEST (initial_matter_is_disjoint_canonical_and_mass_uses_empty_geometry) {
            auto plan = compileGenerationPlan(matterRecipe(), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            ChunkGrid grid;
            for (u32 y = 60u; y < 69u; ++y) {
                for (u32 x = 60u; x < 69u; ++x) {
                    if (x == 60u or x == 68u or y == 60u or y == 68u) {
                        PPR_TEST_ASSERT(not grid.setCell({x, y}, {17u, 293.0f}));
                    }
                }
            }
            GenerationGeometry geometry;
            geometry.m_instances.push_back({1u, 0u, 0u, 60, 60, false});
            auto first = resolveInitialMatter(*plan, geometry, grid),
                 replay = resolveInitialMatter(*plan, geometry, grid);
            PPR_TEST_ASSERT(first and replay);
            PPR_TEST_ASSERT(first->m_regions.size() == 2u and first->m_span_count == 7u);
            PPR_TEST_ASSERT(first->m_searched_cells == 98u and first->m_largest_component == 49u);
            std::array<bool, 81u> assigned{};
            double volume = 0.0;
            for (u32 r = 0u; r < first->m_regions.size(); ++r) {
                const auto &region = first->m_regions[r], &other = replay->m_regions[r];
                PPR_TEST_ASSERT(region.m_identity == other.m_identity and region.m_spans == other.m_spans);
                PPR_TEST_ASSERT(region.m_volume_m3 == other.m_volume_m3 and region.m_mass_kg == other.m_mass_kg);
                PPR_TEST_ASSERT(region.m_mass_kg == region.m_volume_m3 * region.m_bulk_density_kg_m3);
                PPR_TEST_ASSERT(region.m_material == (r == 0u ? 1001u : 1009u));
                for (const auto &span: region.m_spans) {
                    PPR_TEST_ASSERT(span.m_begin_x == 61u and span.m_end_x == 68u);
                    PPR_TEST_ASSERT(r == 0u ? span.m_y < 64u : span.m_y >= 64u);
                    for (u32 x = span.m_begin_x; x < span.m_end_x; ++x) {
                        const auto index = (span.m_y - 60u) * 9u + x - 60u;
                        PPR_TEST_ASSERT(not assigned[index]);
                        assigned[index] = true;
                        PPR_TEST_ASSERT(grid.getCell({x, span.m_y})->m_element == 0u);
                    }
                }
                volume += region.m_volume_m3;
            }
            // Four corner void cells lose1/8 cell each to their diagonal solid
            // neighbour. Whole-cell counting would incorrectly give49/9 m3.
            PPR_TEST_ASSERT(std::abs(volume - 48.5 / 9.0) < 1.0e-12);
            PPR_TEST_ASSERT(first->m_regions[0u].m_fill_height_m == std::optional<double>{64.0 / 3.0});
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            auto cancelled = resolveInitialMatter(*plan, geometry, grid, context);
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(not grid.setCell({64u, 64u}, {17u, 293.0f}));
            auto blocked = resolveInitialMatter(*plan, geometry, grid);
            PPR_TEST_ASSERT(not blocked and blocked.error() == make_error_code(EGenerationErrc::matter_anchor_solid));
        };

        PPR_UNIT_TEST (initial_matter_preserves_integer_boundaries_and_gas_is_remaining_volume) {
            for (const bool signed_height: {false, true}) {
                auto recipe = matterRecipe();
                if (signed_height) {
                    recipe.m_features[1u].m_bounds = {0, -9, 9, 0};
                    recipe.m_features[1u].m_operations[0u].m_y = -9;
                    recipe.m_features[1u].m_markers[0u].m_y = -5;
                }
                std::get<GenerationFill>(recipe.m_rules[1u].m_parameters).m_fill_height_m = signed_height ? -8.0 / 3.0
                                                                                                          : 1.0 / 3.0;
                // Gas deliberately appears before liquid in authored order.
                std::swap(recipe.m_rules[1u], recipe.m_rules[2u]);
                recipe.m_rules[1u].m_order = 0u;
                recipe.m_rules[2u].m_order = 1u;
                auto plan = compileGenerationPlan(std::move(recipe), 42u);
                PPR_TEST_ASSERT(plan.has_value());
                ChunkGrid grid;
                // Sample row25 is inside; the solid shell immediately below
                // sits outside the reservation. Its contour ends at boundary25.
                for (u32 y = 24u; y <= 33u; ++y) {
                    for (u32 x = 60u; x <= 68u; ++x) {
                        if (x == 60u or x == 68u or y == 24u or y == 33u) {
                            PPR_TEST_ASSERT(not grid.setCell({x, y}, {17u, 293.0f}));
                        }
                    }
                }
                GenerationGeometry geometry;
                geometry.m_instances.push_back({1u, 0u, 0u, 60, signed_height ? 34 : 25, false});
                auto result = resolveInitialMatter(*plan, geometry, grid);
                PPR_TEST_ASSERT(result and result->m_regions.size() == 2u);
                const auto &liquid = result->m_regions[0u], &gas = result->m_regions[1u];
                PPR_TEST_ASSERT(liquid.m_material == 1001u and gas.m_material == 1009u);
                PPR_TEST_ASSERT(liquid.m_spans.size() == 1u and liquid.m_spans[0u].m_y == 25u);
                PPR_TEST_ASSERT(liquid.m_fill_height_m == std::optional<double>{26.0 / 3.0});
                PPR_TEST_ASSERT(gas.m_spans.size() == 7u and gas.m_spans[0u].m_y == 26u);
            }
        };

        PPR_UNIT_TEST (initial_matter_rejects_diagonal_saddle_leaks) {
            auto plan = compileGenerationPlan(matterRecipe(), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            ChunkGrid grid;
            // Isolate the marker's void sample with solid orthogonal neighbours,
            // but leave a diagonal path to the exterior. Four-neighbour flood
            // would falsely call this sealed; shared saddle geometry is open.
            for (const auto position:
                {GlobalCellPos{63u, 64u}, GlobalCellPos{65u, 64u}, GlobalCellPos{64u, 63u}, GlobalCellPos{64u, 65u}}) {
                PPR_TEST_ASSERT(not grid.setCell(position, {17u, 293.0f}));
            }
            GenerationGeometry geometry;
            geometry.m_instances.push_back({1u, 0u, 0u, 60, 60, false});
            auto result = resolveInitialMatter(*plan, geometry, grid);
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EGenerationErrc::matter_not_sealed));
        };

        PPR_UNIT_TEST (start_validation_proves_supported_body_route_and_resource_components) {
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2500.0});
            recipe.m_features[0u].m_markers.push_back({304u, EGenerationMarker::resource_anchor, 6, 5});
            recipe.m_features[0u].m_markers.push_back({305u, EGenerationMarker::resource_anchor, 23, 5});
            recipe.m_resources = {{601u, 1u, 9u, 17u, 20.0}, {602u, 2u, 9u, 33u, 20.0}};
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            ChunkGrid grid;
            for (u32 y = 3000u; y < 3030u; ++y) {
                for (u32 x = 2000u; x < 2030u; ++x) {
                    if (x < 2003u or x >= 2027u or y < 3006u or y >= 3027u) {
                        PPR_TEST_ASSERT(
                            not grid.setCell({x, y}, {x >= 2022u and x <= 2024u ? u16{33u} : u16{17u}, 293.0f}));
                    }
                }
            }
            auto start = validateGenerationStart(*plan, grid);
            PPR_TEST_ASSERT(start.has_value());
            PPR_TEST_ASSERT(start->m_spawn_x_m == 2015.5 / 3.0);
            PPR_TEST_ASSERT(start->m_floor_y_m == 1002.0);
            PPR_TEST_ASSERT(std::abs(start->m_spawn_y_m - 1002.9) < 1.0e-12);
            PPR_TEST_ASSERT(start->m_resource_fine_cells.size() == 2u);
            PPR_TEST_ASSERT(start->m_resource_fine_cells[0u] >= 9u and start->m_resource_fine_cells[1u] == 18u);
            // Point-cell clearance would miss the need for continuous support.
            for (u32 y = 3000u; y < 3006u; ++y) {
                PPR_TEST_ASSERT(not grid.setCell({2010u, y}, {}));
            }
            auto gap = validateGenerationStart(*plan, grid);
            PPR_TEST_ASSERT(not gap and gap.error() == make_error_code(EGenerationErrc::start_invalid));
            for (u32 y = 3000u; y < 3006u; ++y) {
                PPR_TEST_ASSERT(not grid.setCell({2010u, y}, {17u, 293.0f}));
            }
            PPR_TEST_ASSERT(not grid.setCell({2015u, 3013u}, {17u, 293.0f}));
            auto ceiling = validateGenerationStart(*plan, grid);
            PPR_TEST_ASSERT(not ceiling and ceiling.error() == make_error_code(EGenerationErrc::start_invalid));
            PPR_TEST_ASSERT(not grid.setCell({2015u, 3013u}, {}));
            recipe.m_resources[1u].m_minimum_fine_cells = 19u;
            auto scarce_plan = compileGenerationPlan(std::move(recipe), 42u);
            PPR_TEST_ASSERT(scarce_plan.has_value());
            auto scarce = validateGenerationStart(*scarce_plan, grid);
            PPR_TEST_ASSERT(not scarce and scarce.error() == make_error_code(EGenerationErrc::start_invalid));
            auto buried_recipe = plan->recipe();
            buried_recipe.m_features[0u].m_markers[1u].m_x = 15;
            buried_recipe.m_features[0u].m_markers[1u].m_y = 4;
            buried_recipe.m_features[0u].m_markers[2u].m_x = 24;
            buried_recipe.m_resources = {{601u, 1u, 1u, 33u, 1.5}, {602u, 2u, 9u, 33u, 20.0}};
            for (u32 x = 2015u; x <= 2024u; ++x) {
                PPR_TEST_ASSERT(not grid.setCell({x, 3004u}, {33u, 293.0f}));
            }
            auto reachable_recipe = buried_recipe;
            reachable_recipe.m_resources[0u].m_max_distance_m = 20.0;
            auto reachable_plan = compileGenerationPlan(std::move(reachable_recipe), 42u);
            PPR_TEST_ASSERT(reachable_plan.has_value());
            PPR_TEST_ASSERT(validateGenerationStart(*reachable_plan, grid).has_value());
            auto buried_plan = compileGenerationPlan(std::move(buried_recipe), 42u);
            PPR_TEST_ASSERT(buried_plan.has_value());
            auto buried = validateGenerationStart(*buried_plan, grid);
            PPR_TEST_ASSERT(not buried and buried.error() == make_error_code(EGenerationErrc::start_invalid));
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            auto cancelled = validateGenerationStart(*plan, grid, context);
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        };

        PPR_UNIT_TEST (compiled_noise_algorithms_replay_and_support_concurrent_sampling) {
            const NoiseFieldDesc base_desc{.m_frequency_per_metre = 0.025, .m_octaves = 1u};
            auto billow_desc = base_desc;
            billow_desc.m_fractal = ENoiseFractal::billow;
            auto base = compileNoiseField(base_desc, 42u);
            auto billow = compileNoiseField(billow_desc, 42u);
            PPR_TEST_ASSERT(base and billow);
            for (u32 index = 0u; index < 32u; ++index) {
                const double x = index * 17.25 - 120.0;
                PPR_TEST_ASSERT(
                    std::abs(billow->sample(x, 57.75) - (2.0f * std::abs(base->sample(x, 57.75)) - 1.0f)) <= 1.0e-7f);
            }
            std::array<float, 32u> previous{};
            bool have_previous = false;
            for (const auto algorithm:
                {ENoiseAlgorithm::open_simplex2, ENoiseAlgorithm::perlin, ENoiseAlgorithm::cellular_distance,
                    ENoiseAlgorithm::cellular_value}) {
                for (const auto fractal:
                    {ENoiseFractal::none, ENoiseFractal::fbm, ENoiseFractal::ridged, ENoiseFractal::billow}) {
                    const NoiseFieldDesc desc{
                        .m_algorithm = algorithm,
                        .m_fractal = fractal,
                        .m_frequency_per_metre = 0.025,
                        .m_warp_amplitude_m = 12.0};
                    auto field = compileNoiseField(desc, 0x8000000012345678ull);
                    auto replay = compileNoiseField(desc, 0x8000000012345678ull);
                    auto other = compileNoiseField(desc, 0x12345678ull);
                    PPR_TEST_ASSERT(field and replay and other);
                    const auto samples = [](const NoiseField &noise) {
                        std::array<float, 32u> values{};
                        for (std::size_t index = 0u; index < values.size(); ++index) {
                            values[index] = noise.sample(static_cast<double>(index) * 17.25 - 120.0, 57.75);
                            PPR_TEST_ASSERT(std::isfinite(values[index]));
                        }
                        return values;
                    };
                    const auto expected = samples(*field);
                    if (fractal == ENoiseFractal::billow) {
                        PPR_TEST_ASSERT(std::ranges::all_of(expected, [](const float value) {
                            return value >= -1.0f and value <= 1.0f;
                        }));
                    }
                    auto concurrent = std::async(std::launch::async, [&] { return samples(*field); });
                    PPR_TEST_ASSERT(samples(*field) == expected);
                    PPR_TEST_ASSERT(samples(*replay) == expected);
                    PPR_TEST_ASSERT(concurrent.get() == expected);
                    PPR_TEST_ASSERT(samples(*other) != expected);
                    if (have_previous) {
                        PPR_TEST_ASSERT(previous != expected);
                    }
                    previous = expected;
                    have_previous = true;
                }
            }
        };

        PPR_UNIT_TEST (compiled_noise_samples_coordinate_limits_and_full_seed_domain) {
            for (const auto algorithm:
                {ENoiseAlgorithm::open_simplex2, ENoiseAlgorithm::perlin, ENoiseAlgorithm::cellular_distance,
                    ENoiseAlgorithm::cellular_value}) {
                const NoiseFieldDesc desc{
                    .m_algorithm = algorithm,
                    .m_fractal = ENoiseFractal::fbm,
                    .m_frequency_per_metre = 0.125,
                    .m_octaves = 16u,
                    .m_lacunarity = 1.25,
                    .m_warp_amplitude_m = kNoiseCoordinateLimitMetres};
                for (const u64 seed: {0ull, 0x7fffffffull, 0x80000000ull, 0xffffffffffffffffull}) {
                    auto field = compileNoiseField(desc, seed);
                    PPR_TEST_ASSERT(field.has_value());
                    for (const double x: {-kNoiseCoordinateLimitMetres, 0.0, kNoiseCoordinateLimitMetres}) {
                        for (const double y: {-kNoiseCoordinateLimitMetres, 0.0, kNoiseCoordinateLimitMetres}) {
                            PPR_TEST_ASSERT(std::isfinite(field->sample(x, y)));
                        }
                    }
                }
            }
        };

        PPR_UNIT_TEST (noise_description_rejects_invalid_work_and_numeric_domains) {
            const NoiseFieldDesc valid{.m_frequency_per_metre = 0.025};
            PPR_TEST_ASSERT(compileNoiseField(valid, 42u).has_value());
            auto transformed = valid;
            transformed.m_axis_scale_x = 2.0;
            transformed.m_axis_scale_y = 0.5;
            transformed.m_rotation_degrees = 90.0;
            auto scaled_field = compileNoiseField(transformed, 42u);
            auto base_field = compileNoiseField(valid, 42u);
            PPR_TEST_ASSERT(scaled_field and base_field);
            PPR_TEST_ASSERT(std::abs(scaled_field->sample(12.0, 40.0) - base_field->sample(-20.0, 24.0)) < 1.0e-6f);
            for (const double scale: {0.124, 8.001, std::numeric_limits<double>::quiet_NaN()}) {
                transformed.m_axis_scale_x = scale;
                PPR_TEST_ASSERT(validateNoiseFieldDescription(transformed) == std::errc::invalid_argument);
            }
            for (const double frequency:
                {0.0, -1.0, 1.0e-300, 1.0e300, std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN()}) {
                auto invalid = valid;
                invalid.m_frequency_per_metre = frequency;
                const auto rejected = compileNoiseField(invalid, 42u);
                PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::invalid_argument);
            }
            for (const u32 octaves: {0u, 17u}) {
                auto invalid = valid;
                invalid.m_octaves = octaves;
                PPR_TEST_ASSERT(not compileNoiseField(invalid, 42u));
            }
            auto invalid = valid;
            invalid.m_algorithm = static_cast<ENoiseAlgorithm>(255u);
            PPR_TEST_ASSERT(not compileNoiseField(invalid, 42u));
            invalid = valid;
            invalid.m_warp_amplitude_m = -1.0;
            PPR_TEST_ASSERT(not compileNoiseField(invalid, 42u));
            invalid = valid;
            invalid.m_gain = std::numeric_limits<double>::quiet_NaN();
            PPR_TEST_ASSERT(not compileNoiseField(invalid, 42u));
        };

        // Element ids below are numeric literals matching the colony-owned
        // registry (`game/colony/Colony.Elements.h`): the sim test target must
        // not depend on game code, so the mapping is documented, not included.
        // 0 vacuum, 1 rock, 2 ice, 3 caustic, 4 oil, 5 ruins, 6 abyssal, 7 vent.

        class WorkerCancellation final : public IContext {
            ParallelScheduler &m_pool;
            SharedContext m_context;
            context::CancelFunc m_cancel;
            const u32 m_threshold;
            mutable std::atomic<u32> m_worker_checks{};

        public:
            WorkerCancellation(ParallelScheduler &pool, const u32 threshold) : m_pool(pool), m_threshold(threshold) {
                std::tie(m_context, m_cancel) = context::withCancel(context::background());
            }
            std::error_code error() const noexcept override {
                if (m_pool.isWorkerThread() and m_worker_checks.fetch_add(1u) + 1u >= m_threshold) {
                    m_cancel();
                }
                return m_context->error();
            }
            u32 workerChecks() const noexcept {
                return m_worker_checks.load();
            }
            std::optional<const opaque::Block::Value *> value(const string_literal key) const noexcept override {
                return m_context->value(key);
            }
            TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
                return m_context->subscribeEvent(signal);
            }
            void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
                m_context->unsubscribeEvent(signal, restore);
            }
            bool pollEvent() noexcept override {
                return m_context->pollEvent();
            }
            void resetEvent() noexcept override {
                m_context->resetEvent();
            }
        };

        PPR_UNIT_TEST (owned_terrain_cancels_admitted_work_and_same_pool_worker_without_deadlock) {
            auto plan = compileGenerationPlan(numericRecipe(), 42u);
            PPR_TEST_ASSERT(plan.has_value());
            auto layout = compileBiomeLayout(*plan);
            PPR_TEST_ASSERT(layout.has_value());
            auto geometry = compileGenerationGeometry(*plan, *layout);
            PPR_TEST_ASSERT(geometry.has_value());
            ParallelScheduler pool{"Terrain cancellation test", 1u, 4096u};
            auto context = std::make_shared<WorkerCancellation>(pool, 10u);
            auto result = rasterizeGenerationTerrain(
                *plan, *layout, *geometry, {.m_scheduler = &pool, .m_context = context});
            PPR_TEST_ASSERT(not result and result.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(context->workerChecks() >= 10u);

            auto nested_context = std::make_shared<WorkerCancellation>(pool, 10u);
            auto nested = pool.schedule([&](const IContext &) {
                return rasterizeGenerationTerrain(
                    *plan, *layout, *geometry, {.m_scheduler = &pool, .m_context = nested_context});
            });
            PPR_TEST_ASSERT(nested.has_value());
            auto nested_result = nested->get();
            PPR_TEST_ASSERT(not nested_result and nested_result.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(nested_context->workerChecks() >= 10u);
            PPR_TEST_ASSERT(not pool.join());
            PPR_TEST_ASSERT(pool.getNumOutstandingTasks() == 0u);
        };

        [[nodiscard]] u64 gridHash(const ChunkGrid &grid) {
            u64 hash = 14695981039346656037ULL;
            for (u32 chunk_y = 0u; chunk_y < kChunksPerEdge; ++chunk_y) {
                for (u32 chunk_x = 0u; chunk_x < kChunksPerEdge; ++chunk_x) {
                    const auto cells = grid.residentCells(ChunkPos{chunk_x, chunk_y});
                    PPR_TEST_ASSERT(cells.has_value());
                    for (const Cell &cell: *cells) {
                        // Single FNV-1a round over the packed (element,
                        // temperature-bits) payload: the same
                        // equality/inequality discrimination at half the
                        // multiplies per cell.
                        const u64 packed = (static_cast<u64>(cell.m_element) << 32u) |
                                           std::bit_cast<u32>(cell.m_temperature);
                        hash ^= packed;
                        hash *= 1099511628211ULL;
                    }
                }
            }

            return hash;
        }

        PPR_UNIT_TEST (owned_terrain_replays_across_serial_and_parallel_execution) {
            struct Clock final : ITimerClock {
                mutable TimePoint m_now{};
                TimePoint now() const noexcept override {
                    m_now += std::chrono::milliseconds{1};
                    return m_now;
                }
            } clock;
            auto recipe = numericRecipe();
            recipe.m_materials.push_back({633u, true, false, EGenerationMatter::solid, 3200.0, 63301u});
            recipe.m_open_surface = true;
            recipe.m_surface_elevation_m = 1120.0;
            recipe.m_bedrock_thickness_m = 25.0;
            recipe.m_bedrock_material = 633u;
            recipe.m_start_depth_m = 120.0;
            recipe.m_features[0u].m_operations[0u].m_x = 3;
            recipe.m_features[0u].m_operations[0u].m_y = 6;
            recipe.m_features[0u].m_operations[0u].m_width = 24u;
            recipe.m_features[0u].m_operations[0u].m_height = 21u;
            recipe.m_materials.push_back({33u, true, true, EGenerationMatter::solid, 2700.0, 3301u});
            recipe.m_biomes[0u].m_pore_probability = 0.05;
            recipe.m_biomes[0u].m_pore_affinity_min = -1.0;
            recipe.m_biomes[0u].m_pore_passes = 1u;
            recipe.m_biomes[0u].m_relaxation_passes = 1u;
            recipe.m_biomes[0u].m_finish_passes = 2u;
            recipe.m_biomes[0u].m_erosion_passes = 2u;
            recipe.m_biomes[0u].m_deposits.push_back(
                {.m_key = 601u,
                    .m_material = 33u,
                    .m_host = 17u,
                    .m_depth_max = 1.0,
                    .m_threshold_min = -0.15,
                    .m_threshold_max = 0.15,
                    .m_stage = EGenerationDepositStage::detail});
            auto plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(plan.has_value());
            std::atomic<u32> completed{};
            std::atomic<EGenerationStage> stage{EGenerationStage::layout};
            auto serial = generateWorld(std::move(*plan), clock, {.m_completed_chunks = &completed, .m_stage = &stage});
            PPR_TEST_ASSERT(serial.has_value());
            PPR_TEST_ASSERT(serial->m_grid.getCell({0u, 0u})->m_element == 633u);
            PPR_TEST_ASSERT(serial->m_grid.getCell({0u, kWorldEdge - 1u})->m_element == 0u);
            PPR_TEST_ASSERT(serial->m_grid.getCell({0u, 1000u})->m_element == 17u);
            PPR_TEST_ASSERT(completed.load() == kChunkCount and stage.load() == EGenerationStage::complete);
            PPR_TEST_ASSERT(
                serial->m_grid.residentChunkCount() == kChunkCount and serial->m_grid.dirtyChunkCount() == kChunkCount);
            PPR_TEST_ASSERT(
                serial->m_report.m_grid_payload_bytes == static_cast<u64>(kWorldEdge) * kWorldEdge * sizeof(Cell));
            PPR_TEST_ASSERT(serial->m_report.m_retained_known_payload_bytes > serial->m_report.m_grid_payload_bytes);
            PPR_TEST_ASSERT(serial->m_plan.retainedPayloadBytes() > 0u);
            PPR_TEST_ASSERT(serial->m_layout.retainedPayloadBytes() > 0u);
            PPR_TEST_ASSERT(
                serial->m_report.m_known_phase_scratch_bytes[2u] >= static_cast<u64>(kCellsPerChunk) * sizeof(Cell));
            PPR_TEST_ASSERT(
                serial->m_report.m_known_phase_scratch_bytes[3u] >= static_cast<u64>(kWorldEdge) * kWorldEdge / 8u);
            PPR_TEST_ASSERT(serial->m_report.m_known_phase_scratch_bytes[4u] >= 65536u * (sizeof(u8) + sizeof(u64)));
            for (const auto duration: serial->m_report.m_phase_times) {
                PPR_TEST_ASSERT(duration == std::chrono::milliseconds{1});
            }
            u64 cell_count = 0u;
            for (const auto &count: serial->m_report.m_material_counts) {
                cell_count += count.m_cells;
            }
            PPR_TEST_ASSERT(cell_count == static_cast<u64>(kWorldEdge) * kWorldEdge);
            PPR_TEST_ASSERT(serial->m_start.m_floor_y_m == 1002.0);
            completed.store(0u);
            ParallelScheduler &pool = parallel::backgroundPool();
            auto parallel_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(parallel_plan.has_value());
            auto parallel = generateWorld(
                std::move(*parallel_plan), clock, {.m_scheduler = &pool, .m_completed_chunks = &completed});
            PPR_TEST_ASSERT(parallel.has_value());
            PPR_TEST_ASSERT(completed.load() == kChunkCount);
            PPR_TEST_ASSERT(gridHash(serial->m_grid) == gridHash(parallel->m_grid));
            PPR_TEST_ASSERT(serial->m_report.m_terrain_checksum == parallel->m_report.m_terrain_checksum);
            PPR_TEST_ASSERT(serial->m_report.m_output_checksum == parallel->m_report.m_output_checksum);
            PPR_TEST_ASSERT(
                serial->m_report.m_retained_known_payload_bytes == parallel->m_report.m_retained_known_payload_bytes);
            // Three workers plus coordinator fallback can own at most four
            // chunk buffers; the serial observation includes one already.
            PPR_TEST_ASSERT(
                parallel->m_report.m_known_phase_scratch_bytes[2u] >= serial->m_report.m_known_phase_scratch_bytes[2u]);
            const auto working_raster = compileGenerationRaster(
                parallel->m_plan, parallel->m_layout, parallel->m_geometry);
            PPR_TEST_ASSERT(working_raster.has_value());

            {
                const auto scratch_scope = mem::ScratchPad::open();
                const auto &biome = recipe.m_biomes.front();
                const u32 padded_edge = kChunkEdge + 2u * (biome.m_lining_thickness_fine +
                                                              biome.m_pore_passes +
                                                              biome.m_relaxation_passes +
                                                              biome.m_finish_passes +
                                                              biome.m_erosion_passes +
                                                              1u);
                Array<std::array<double, 2u>, mem::ScratchPad> columns(padded_edge);
                PPR_TEST_ASSERT(columns.capacity() == padded_edge);

                // This fixture needs two cell buffers, biome IDs, affinity IDs,
                // pore flags and protection flags, in addition to the column pair.
                const u64 square_payload = static_cast<u64>(padded_edge) *
                                           padded_edge *
                                           (2u * sizeof(Cell) + sizeof(u32) + sizeof(u16) + 2u * sizeof(u8));
                const u64 expected_payload = kCellsPerChunk * sizeof(Cell) +
                                             square_payload +
                                             columns.capacity() * sizeof(columns.front());
                PPR_TEST_ASSERT(working_raster->chunkWorkingPayloadBytes() == expected_payload);
            }

            PPR_TEST_ASSERT(
                parallel->m_report.m_known_phase_scratch_bytes[2u] <=
                serial->m_report.m_known_phase_scratch_bytes[2u] + 3u * working_raster->chunkWorkingPayloadBytes());
            PPR_TEST_ASSERT(serial->m_layout.sites().size() == parallel->m_layout.sites().size());
            const auto [context, cancel] = context::withCancel(context::background());
            cancel();
            completed.store(0u);
            auto cancelled_plan = compileGenerationPlan(recipe, 42u);
            PPR_TEST_ASSERT(cancelled_plan.has_value());
            auto cancelled = generateWorld(
                std::move(*cancelled_plan), clock,
                {.m_scheduler = &pool, .m_context = context, .m_completed_chunks = &completed});
            PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
            PPR_TEST_ASSERT(completed.load() == 0u);
        };

    } // namespace WorldGen
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest world_gen = UnitTest::Named("world_gen") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::WorldGen::definition_feature_paint_rectangle,
            detail::WorldGen::definition_recipe_all_payloads_and_derived_phases,
            detail::WorldGen::definition_recipe_fill_phase_height_and_suitability,
            detail::WorldGen::definition_recipe_particle_fill_accepts_every_phase,
            detail::WorldGen::definition_recipe_scoped_markers_and_owner_first,
            detail::WorldGen::definition_recipe_scoped_rules_disabled_placement_and_active_biomes,
            detail::WorldGen::definition_recipe_profile_owned_rule_bindings_and_reordering,
            detail::WorldGen::definition_recipe_numeric_boundary_and_missing_dependencies,
            detail::WorldGen::definition_recipe_optional_surface_bedrock_and_owned_values,
            detail::WorldGen::definition_recipe_generic_resolution_variant_refs_and_atomic_replacement,
            detail::WorldGen::definition_feature_paint_ellipse,
            detail::WorldGen::definition_feature_paint_capsule,
            detail::WorldGen::definition_feature_carve_rectangle,
            detail::WorldGen::definition_feature_carve_ellipse,
            detail::WorldGen::definition_feature_carve_capsule,
            detail::WorldGen::definition_feature_reflection_required_and_numeric,
            detail::WorldGen::definition_feature_paint_requires_terrain_carve_has_no_material,
            detail::WorldGen::definition_feature_unknown_action_and_shape_tags,
            detail::WorldGen::definition_feature_markers_roles_and_extent_paths,
            detail::WorldGen::definition_feature_reservation_and_shape_extent_paths,
            detail::WorldGen::definition_feature_reordering_preserves_bindings_streams_and_geometry,
            detail::WorldGen::definition_recursive_paths_include_deposits_and_pore,
            detail::WorldGen::definition_fingerprint_v3_pinned_and_sensitive_to_every_member,
            detail::WorldGen::semantic_fingerprint_tags_ignore_variant_storage_indices,
            detail::WorldGen::semantic_fingerprint_reference_domains_are_semantic,
            detail::WorldGen::semantic_fingerprint_canonical_keys_and_every_described_member,
            detail::WorldGen::semantic_fingerprint_optional_presence_zero_and_primitive_framing,
            detail::WorldGen::semantic_fingerprint_scoped_reference_owners_follow_authored_context,
            detail::WorldGen::semantic_fingerprint_stream_boundaries_and_non_destructive_reads,
            detail::WorldGen::semantic_fingerprint_explicit_scope_presence_and_subtree_isolation,
            detail::WorldGen::semantic_fingerprint_nested_scratch_and_duplicate_key_order,
            detail::WorldGen::definition_rules_fingerprint_projects_all_profiles_canonically,
            detail::WorldGen::definition_rejects_normalized_weight_collapse,
            detail::WorldGen::definition_validates_biome_chain,
            detail::WorldGen::definition_ids_and_fingerprint_deterministic,
            detail::WorldGen::definition_compiles_every_biome_and_enforces_cardinality,
            detail::WorldGen::definition_reordering_preserves_all_bindings_streams_and_output,
            detail::WorldGen::definition_halo_includes_unused_biomes_and_reorder_stable_ties,
            detail::WorldGen::definition_authored_field_and_terrain_references_resolve_and_diagnose,
            detail::WorldGen::definition_blob_presence_and_controls_across_all_biomes,
            detail::WorldGen::halo_maxima_across_biomes,
            detail::WorldGen::fragment_to_recipe_compiles,
            detail::WorldGen::noise_descriptor_preserves_authored_editing_metadata,
            detail::WorldGen::noise_schema_rejects_unsafe,
            detail::WorldGen::generation_random_has_fixed_full_width_streams,
            detail::WorldGen::numeric_plan_owns_recipe_and_isolates_field_streams,
            detail::WorldGen::numeric_plan_compiles_sparse_material_keys_and_density_once,
            detail::WorldGen::numeric_plan_rejects_invalid_references_bounds_and_collapsed_weights,
            detail::WorldGen::numeric_plan_accepts_granular_fills_and_exact_signed_thirds,
            detail::WorldGen::numeric_plan_preserves_authored_geometry_boundaries,
            detail::WorldGen::biome_lining_stops_threshold_caves_and_replays_across_chunks,
            detail::WorldGen::biome_sample_forces_selected_context_without_start_or_foreign_geometry,
            detail::WorldGen::particle_seeding_replays_large_regions_with_coherent_positive_mass,
            detail::WorldGen::particle_edits_are_local_atomic_and_pure_digs_preserve_frozen_records,
            detail::WorldGen::macro_layout_replays_spacing_and_spatial_nearest_matches_oracle,
            detail::WorldGen::macro_layout_reports_infeasible_constraints_and_cancellation,
            detail::WorldGen::macro_layout_satisfies_required_biomes_before_weighted_assignment,
            detail::WorldGen::nonlocal_geometry_replays_worms_and_links_only_traversal_rooms,
            detail::WorldGen::nonlocal_geometry_distinguishes_required_exhaustion_optional_omission_and_limits,
            detail::WorldGen::finite_blobs_and_smooth_worms_are_bounded_and_replay,
            detail::WorldGen::coherent_vacuum_palette_is_salted_before_bounded_relaxation,
            detail::WorldGen::relaxation_crosses_chunk_seams_preserves_reservations_and_precedes_detail,
            detail::WorldGen::absent_biome_pass_maxima_preserve_sampled_chunk_output,
            detail::WorldGen::chunk_raster_preserves_boundaries_host_replacement_and_global_seams,
            detail::WorldGen::chunk_raster_opens_room_portals_but_preserves_mixed_role_sealed_pockets,
            detail::WorldGen::chunk_raster_protects_template_reservations_from_cave_fields,
            detail::WorldGen::compiled_noise_algorithms_replay_and_support_concurrent_sampling,
            detail::WorldGen::matter_area_clips_shared_contours_and_horizontal_height,
            detail::WorldGen::vacuum_palette_preserves_unstamped_sealed_reservation_substrate,
            detail::WorldGen::open_surface_and_continuous_bedrock_override_every_terrain_phase,
            detail::WorldGen::warped_beds_and_clustered_pores_replay_through_split_filters,
            detail::WorldGen::supported_formations_use_frozen_contacts_and_preserve_backbone_clearance,
            detail::WorldGen::initial_matter_is_disjoint_canonical_and_mass_uses_empty_geometry,
            detail::WorldGen::initial_matter_rejects_diagonal_saddle_leaks,
            detail::WorldGen::initial_matter_preserves_integer_boundaries_and_gas_is_remaining_volume,
            detail::WorldGen::start_validation_proves_supported_body_route_and_resource_components,
            detail::WorldGen::compiled_noise_samples_coordinate_limits_and_full_seed_domain,
            detail::WorldGen::noise_description_rejects_invalid_work_and_numeric_domains,
            detail::WorldGen::owned_terrain_replays_across_serial_and_parallel_execution,
            detail::WorldGen::owned_terrain_cancels_admitted_work_and_same_pool_worker_without_deadlock,
        });
    };

    const UnitTest &worldGenTests() noexcept {
        return world_gen;
    }
} // namespace pP::tests
