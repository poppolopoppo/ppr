module;
#include "pP/Macros.h"
module engine.sim;
import :worldgen.definition;
import :worldgen.noise;
import :worldgen.plan;
import :chunk_grid;
import engine.core;
import std;

namespace pP::sim::definition {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldDefinition, info, none);
    // ------------------------------------------------------------------
    // validation helpers
    // ------------------------------------------------------------------

    // subgroup: validation
    namespace {
        using config::EConfigErrc;
        struct Validation {
            Diagnostics &m_diagnostics;
            std::error_code m_first{};

            void reject(
                const std::string &path, const EConfigErrc code, const std::string_view message,
                Array<std::string> related = {}) {
                const auto error = config::make_error_code(code);
                if (not m_first) {
                    m_first = error;
                }
                m_diagnostics.add("definition", path, std::string{message}, error, {}, std::move(related));
            }
        };
        template<typename T>
        [[nodiscard]] auto sortedEntries(const KeyedCollection<T> &collection) {
            Array<const config::KeyedEntry<T> *> result;
            result.reserve(collection.m_entries.size());
            for (const auto &entry: collection.m_entries) {
                result.push_back(std::addressof(entry));
            }
            std::ranges::sort(result, [](const auto *left, const auto *right) { return left->m_key < right->m_key; });
            return result;
        }

        template<typename T>
        [[nodiscard]] const T *find(const KeyedCollection<T> &collection, const std::string_view key) {
            for (const auto &entry: collection.m_entries) {
                if (entry.m_key == key) {
                    return std::addressof(entry.m_value);
                }
            }
            return nullptr;
        }

        template<typename T>
        [[nodiscard]] auto orderedEntries(
            const KeyedCollection<T> &collection, const std::string &path, Validation &validation) {
            auto result = sortedEntries(collection);
            std::ranges::stable_sort(result, {}, [](const auto *entry) { return entry->m_value.m_order; });
            for (std::size_t i = 1u; i < result.size(); ++i) {
                if (result[i - 1u]->m_value.m_order == result[i]->m_value.m_order) {
                    validation.reject(
                        path + "." + result[i]->m_key + ".order", EConfigErrc::duplicate_key,
                        "Authored order must be unique", {path + "." + result[i - 1u]->m_key + ".order"});
                }
            }
            return result;
        }

        void strictInterval(
            const double minimum, const double maximum, const std::string &path, Validation &validation) {
            if (not (minimum < maximum)) {
                validation.reject(
                    path + ".max", EConfigErrc::cross_field_invalid, "Interval must have strictly increasing endpoints",
                    {path + ".min"});
            }
        }

        // ------------------------------------------------------------------
        // references
        // ------------------------------------------------------------------

        // subgroup: references
        // Scoped names are resolved exclusively through the explicit referring owner's path.
        struct References {
            const Definition &m_definition;
            Validation &m_validation;

            void root(const FieldReference &value, const std::string &path, const std::string_view) const {
                if (find(m_definition.m_fields, value.m_key) == nullptr) {
                    m_validation.reject(
                        path, EConfigErrc::cross_field_invalid, "Unknown noise field reference",
                        {"fields." + value.m_key, "fields"});
                }
            }
            void root(
                const MaterialReference &value, const std::string &path, const std::string_view suitability) const {
                if (suitability == "terrain_or_vacuum" and value.m_key == "vacuum") {
                    return;
                }
                const auto *material = find(m_definition.m_materials, value.m_key);
                const bool particle = suitability == "particle";
                if (material == nullptr or
                    (particle ? material->m_representation != EMaterialRepresentation::particle
                              : material->m_representation != EMaterialRepresentation::terrain)) {
                    m_validation.reject(
                        path, EConfigErrc::cross_field_invalid, "Material reference has incompatible suitability",
                        {"materials." + value.m_key, "materials"});
                }
            }
            void root(const BiomeReference &value, const std::string &path, const std::string_view) const {
                if (find(m_definition.m_biomes, value.m_key) == nullptr) {
                    m_validation.reject(
                        path, EConfigErrc::cross_field_invalid, "Unknown biome reference",
                        {"biomes." + value.m_key, "biomes"});
                }
            }
            void root(const FeatureReference &value, const std::string &path, const std::string_view) const {
                if (find(m_definition.m_features, value.m_key) == nullptr) {
                    m_validation.reject(
                        path, EConfigErrc::cross_field_invalid, "Unknown feature reference",
                        {"features." + value.m_key, "features"});
                }
            }
            [[nodiscard]] const Feature *placement(
                const ProfileDefinition &profile, const RuleReference &value, const std::string &owner,
                const std::string &path) const {
                const auto *rule = find(profile.m_rules, value.m_key);
                const auto *payload = rule == nullptr ? nullptr : std::get_if<Feature>(&rule->m_payload);
                if (payload == nullptr or not rule->m_enabled) {
                    m_validation.reject(
                        path, EConfigErrc::cross_field_invalid,
                        "Reference requires an enabled placement in this profile",
                        {owner + ".rules." + value.m_key, owner});
                    return nullptr;
                }
                return payload;
            }
            void marker(
                const FeatureReference &feature_owner, const MarkerReference &value, const EGenerationMarker role,
                const std::string &owner_path, const std::string &path) const {
                const auto *feature = find(m_definition.m_features, feature_owner.m_key);
                // Missing owners are diagnosed on the referring owner scalar before any child lookup.
                if (feature == nullptr) {
                    root(feature_owner, owner_path, {});
                    return;
                }
                const std::string owner = "features." + feature_owner.m_key;
                const auto *target = find(feature->m_markers, value.m_key);
                if (target == nullptr or target->m_role != role) {
                    m_validation.reject(
                        path, EConfigErrc::cross_field_invalid, "Marker is absent or has the wrong role in its feature",
                        {owner + ".markers." + value.m_key, owner});
                }
            }
            [[nodiscard]] std::pair<const ProfileDefinition *, std::string> profile(const std::string &path) const {
                if (not path.starts_with("worlds.")) {
                    return {};
                }
                const auto end = path.find('.', 7u);
                const auto key = path.substr(7u, end - 7u);
                return {find(m_definition.m_worlds, key), "worlds." + key};
            }
            void root(const RuleReference &value, const std::string &path, const std::string_view) const {
                const auto [source, owner] = profile(path);
                if (source != nullptr) {
                    (void) placement(*source, value, owner, path);
                }
            }
            void root(const MarkerReference &value, const std::string &path, const std::string_view suitability) const {
                const auto [source, owner] = profile(path);
                if (source == nullptr) {
                    return;
                }
                if (suitability != "sealed_pocket") {
                    marker(
                        source->m_start.m_feature, value,
                        suitability == "start" ? EGenerationMarker::start : EGenerationMarker::resource_anchor,
                        owner + ".start.feature", path);
                    return;
                }
                const auto prefix = owner + ".rules.";
                const auto end = path.find('.', prefix.size());
                const auto *rule = find(source->m_rules, path.substr(prefix.size(), end - prefix.size()));
                const auto *fill = rule == nullptr ? nullptr : std::get_if<MatterFill>(&rule->m_payload);
                if (fill == nullptr) {
                    return;
                }
                const auto rule_path = path.substr(0u, path.rfind('.'));
                const auto *target = placement(*source, fill->m_placement, owner, rule_path + ".placement");
                if (target != nullptr) {
                    marker(
                        target->m_feature, value, EGenerationMarker::sealed_pocket,
                        owner + ".rules." + fill->m_placement.m_key + ".payload.feature.feature", path);
                }
            }
        };

        [[nodiscard]] auto orderedRules(
            const ProfileDefinition &profile, const std::string &path, Validation &validation) {
            auto rules = sortedEntries(profile.m_rules);
            std::erase_if(rules, [](const auto *entry) { return not entry->m_value.m_enabled; });
            std::ranges::stable_sort(rules, {}, [](const auto *entry) {
                return std::pair{phaseOf(entry->m_value.m_payload), entry->m_value.m_order};
            });
            for (std::size_t i = 1u; i < rules.size(); ++i) {
                if (phaseOf(rules[i - 1u]->m_value.m_payload) == phaseOf(rules[i]->m_value.m_payload) and
                    rules[i - 1u]->m_value.m_order == rules[i]->m_value.m_order) {
                    validation.reject(
                        path + ".rules." + rules[i]->m_key + ".order", EConfigErrc::duplicate_key,
                        "Enabled rule order must be unique within its derived phase",
                        {path + ".rules." + rules[i - 1u]->m_key + ".order"});
                }
            }
            return rules;
        }

        void validateProfile(
            const ProfileDefinition &profile, const Definition &definition, const std::string &path,
            Validation &validation) {
            const auto ordered = [&](const double minimum, const double maximum, const std::string &min_path,
                                     const std::string &max_path) {
                if (minimum > maximum) {
                    validation.reject(
                        max_path, EConfigErrc::cross_field_invalid, "Endpoints must be ordered", {min_path});
                }
            };
            ordered(
                profile.m_layout.m_min_sites, profile.m_layout.m_max_sites, path + ".layout.min_sites",
                path + ".layout.max_sites");
            std::set<std::string_view> active;
            u64 required = 0u;
            for (const auto *entry: sortedEntries(profile.m_biomes)) {
                const auto choice_path = path + ".biomes." + entry->m_key;
                const auto &choice = entry->m_value;
                if (not active.insert(choice.m_biome.m_key).second) {
                    validation.reject(
                        choice_path + ".biome", EConfigErrc::duplicate_key,
                        "Biome choices must target distinct biomes");
                }
                required += choice.m_required_count;
                ordered(
                    choice.m_depth.m_min, choice.m_depth.m_max, choice_path + ".depth.min", choice_path + ".depth.max");
                ordered(
                    choice.m_distance_min_m, choice.m_distance_max_m, choice_path + ".distance_min_m",
                    choice_path + ".distance_max_m");
            }
            if (required > profile.m_layout.m_max_sites) {
                validation.reject(
                    path + ".biomes", EConfigErrc::out_of_range, "Required biome counts exceed maximum sites",
                    {path + ".layout.max_sites"});
            }
            const auto active_biome = [&](const BiomeReference &biome, const std::string &ref_path) {
                if (not active.contains(biome.m_key)) {
                    validation.reject(
                        ref_path, EConfigErrc::cross_field_invalid, "Target biome is inactive in this profile",
                        {"biomes." + biome.m_key, path + ".biomes", path});
                }
            };
            active_biome(profile.m_start.m_biome, path + ".start.biome");
            if (profile.m_start.m_depth_m and not profile.m_surface_bedrock) {
                validation.reject(
                    path + ".start.depth_m", EConfigErrc::cross_field_invalid,
                    "Surface-relative start requires the surface/bedrock group", {path + ".surface_bedrock"});
            }
            std::set<std::string_view> resource_markers;
            for (const auto *entry: sortedEntries(profile.m_resources)) {
                const auto resource_path = path + ".resources." + entry->m_key;
                if (not resource_markers.insert(entry->m_value.m_marker.m_key).second) {
                    validation.reject(
                        resource_path + ".marker", EConfigErrc::duplicate_key, "Resource markers must be distinct");
                }
                const auto *material = find(definition.m_materials, entry->m_value.m_material.m_key);
                if (material != nullptr and not material->m_diggable.value_or(false)) {
                    validation.reject(
                        resource_path + ".material", EConfigErrc::cross_field_invalid,
                        "Start resources require diggable terrain",
                        {"materials." + entry->m_value.m_material.m_key + ".diggable", "materials"});
                }
            }

            u64 worm_count = 0u, feature_count = 0u;
            u32 connections = 0u;
            for (const auto *entry: orderedRules(profile, path, validation)) {
                const auto rule_path = path + ".rules." + entry->m_key + ".payload.";
                std::visit(
                    [&](const auto &payload) {
                        using T = std::remove_cvref_t<decltype(payload)>;
                        if constexpr (std::is_same_v<T, Cave>) {
                            active_biome(payload.m_biome, rule_path + "cave.biome");
                            ordered(
                                payload.m_depth.m_min, payload.m_depth.m_max, rule_path + "cave.depth.min",
                                rule_path + "cave.depth.max");
                            ordered(
                                payload.m_threshold.m_min, payload.m_threshold.m_max, rule_path + "cave.threshold.min",
                                rule_path + "cave.threshold.max");
                        } else if constexpr (std::is_same_v<T, CarveWorm> or std::is_same_v<T, VeinWorm>) {
                            const auto geometry_path = rule_path +
                                                       (std::is_same_v<T, CarveWorm> ? "carve_worm" : "vein_worm") +
                                                       ".geometry";
                            const auto &geometry = payload.m_geometry;
                            worm_count += geometry.m_count;
                            active_biome(geometry.m_biome, geometry_path + ".biome");
                            ordered(
                                geometry.m_depth.m_min, geometry.m_depth.m_max, geometry_path + ".depth.min",
                                geometry_path + ".depth.max");
                            ordered(
                                geometry.m_length_min_m, geometry.m_length_max_m, geometry_path + ".length_min_m",
                                geometry_path + ".length_max_m");
                            ordered(
                                geometry.m_radius_min_m, geometry.m_radius_max_m, geometry_path + ".radius_min_m",
                                geometry_path + ".radius_max_m");
                            if (geometry.m_turn_smoothing == 0.0 and (geometry.m_radius_variation != 0.0 or
                                                                         geometry.m_chamber_probability != 0.0 or
                                                                         geometry.m_chamber_scale != 1.0)) {
                                validation.reject(
                                    geometry_path + ".turn_smoothing", EConfigErrc::cross_field_invalid,
                                    "Advanced worm controls require positive turn smoothing",
                                    {geometry_path + ".radius_variation", geometry_path + ".chamber_probability",
                                        geometry_path + ".chamber_scale"});
                            }
                        } else if constexpr (std::is_same_v<T, Connection>) {
                            ++connections;
                        } else if constexpr (std::is_same_v<T, Feature>) {
                            feature_count += payload.m_max_count;
                            active_biome(payload.m_biome, rule_path + "feature.biome");
                            ordered(
                                payload.m_depth.m_min, payload.m_depth.m_max, rule_path + "feature.depth.min",
                                rule_path + "feature.depth.max");
                            ordered(
                                payload.m_min_count, payload.m_max_count, rule_path + "feature.min_count",
                                rule_path + "feature.max_count");
                            const auto *feature = find(definition.m_features, payload.m_feature.m_key);
                            if (payload.m_reflect and
                                feature != nullptr and
                                not feature->m_allow_horizontal_reflection) {
                                validation.reject(
                                    rule_path + "feature.reflect", EConfigErrc::cross_field_invalid,
                                    "Placement reflection is forbidden by its feature template",
                                    {"features." + payload.m_feature.m_key + ".allow_horizontal_reflection",
                                        "features." + payload.m_feature.m_key});
                            }
                        } else if constexpr (std::is_same_v<T, MatterFill>) {
                            const auto *material = find(definition.m_materials, payload.m_material.m_key);
                            if (material != nullptr) {
                                const bool gas = material->m_phase == EGenerationMatter::gas;
                                if (gas == payload.m_fill_height_m.has_value()) {
                                    validation.reject(
                                        rule_path + "matter_fill.fill_height_m",
                                        gas ? EConfigErrc::cross_field_invalid : EConfigErrc::missing_value,
                                        "Gas requires absent fill height; non-gas requires present fill height",
                                        {rule_path + "matter_fill.material",
                                            "materials." + payload.m_material.m_key + ".phase"});
                                }
                            }
                        }
                    },
                    entry->m_value.m_payload);
            }
            if (worm_count > 4096u or feature_count > 1024u or connections > 1u) {
                validation.reject(
                    path + ".rules", EConfigErrc::out_of_range, "Aggregate rule work exceeds numeric limits");
            }
        }

        [[nodiscard]] GenerationOperation numericOperation(const Operation &source) {
            GenerationOperation result{};
            result.m_order = source.m_order;
            result.m_x = source.m_x_fine;
            result.m_y = source.m_y_fine;

            std::visit(
                [&](const auto &action) {
                    result.m_carve = std::is_same_v<std::remove_cvref_t<decltype(action)>, Carve>;
                    std::visit(
                        [&](const auto &shape) {
                            using T = std::remove_cvref_t<decltype(shape)>;
                            if constexpr (std::is_same_v<T, Capsule>) {
                                result.m_shape = EGenerationShape::capsule;
                                result.m_end_x = shape.m_end_x_fine;
                                result.m_end_y = shape.m_end_y_fine;
                                result.m_radius_m = shape.m_radius_m;
                            } else {
                                result.m_shape = std::is_same_v<T, Rectangle> ? EGenerationShape::rectangle
                                                                              : EGenerationShape::ellipse;
                                result.m_width = shape.m_width_fine;
                                result.m_height = shape.m_height_fine;
                            }
                        },
                        action.m_shape);
                },
                source.m_action);
            return result;
        }

        [[nodiscard]] std::string shapePath(const Operation &source, const std::string &path) {
            const auto operation = numericOperation(source);
            const auto action = operation.m_carve ? ".action.carve.shape." : ".action.paint.shape.";
            const auto shape = operation.m_shape == EGenerationShape::rectangle ? "rectangle"
                               : operation.m_shape == EGenerationShape::ellipse ? "ellipse"
                                                                                : "capsule";
            return path + action + shape;
        }

        void validateOperationFit(
            const Operation &source, const ReservationBounds &bounds, const double cells_per_metre,
            const std::string &path, Validation &validation) {
            const auto operation = numericOperation(source);
            double min_x = operation.m_x, min_y = operation.m_y;
            double max_x = min_x + operation.m_width, max_y = min_y + operation.m_height;
            if (operation.m_shape == EGenerationShape::capsule) {
                const double radius = operation.m_radius_m * cells_per_metre;
                min_x = std::min(operation.m_x, operation.m_end_x) - radius;
                min_y = std::min(operation.m_y, operation.m_end_y) - radius;
                max_x = std::max(operation.m_x, operation.m_end_x) + radius;
                max_y = std::max(operation.m_y, operation.m_end_y) + radius;
            }

            if (min_x < bounds.m_min_x_fine or
                min_y < bounds.m_min_y_fine or
                max_x > bounds.m_max_x_fine or
                max_y > bounds.m_max_y_fine) {
                validation.reject(
                    shapePath(source, path), EConfigErrc::cross_field_invalid,
                    "Operation geometry exceeds its reservation");
            }
        }

        // ------------------------------------------------------------------
        // stream keys
        // ------------------------------------------------------------------

        // Length-prefixed semantic components, identical to the legacy compiler convention.
        // No numeric IDs, positions, file names or seeds enter these stream keys.
        class StreamKeys {
            std::set<u64> m_keys{};
            Validation &m_validation;

        public:
            explicit StreamKeys(Validation &validation) : m_validation(validation) {
            }
            [[nodiscard]] u64 key(const std::initializer_list<std::string_view> parts) {
                u64 hash = 14695981039346656037ull;
                const auto byte = [&hash](const u8 value) { hash = (hash ^ value) * 1099511628211ull; };
                std::string path;
                for (const auto part: parts) {
                    const u64 size = part.size();
                    for (u32 shift = 0u; shift < 64u; shift += 8u) {
                        byte(static_cast<u8>(size >> shift));
                    }
                    for (const char value: part) {
                        byte(static_cast<u8>(value));
                    }
                    if (not path.empty()) {
                        path += '.';
                    }
                    path += part;
                }
                if (not m_keys.insert(hash).second) {
                    m_validation.reject(path, EConfigErrc::cross_field_invalid, "Semantic stream key collision");
                }
                return hash;
            }
        };

        // ------------------------------------------------------------------
        // semantic fingerprint
        // ------------------------------------------------------------------

    }

    SemanticFingerprint::SemanticFingerprint(const Definition &definition) noexcept : m_definition(definition) {
    }

    void SemanticFingerprint::byte(const u8 value) noexcept {
        m_hash = (m_hash ^ value) * 1099511628211ull;
    }

    void SemanticFingerprint::string(const std::string_view item) noexcept {
        integer(static_cast<u64>(item.size()));
        for (const char ch: item) {
            byte(static_cast<u8>(ch));
        }
    }

    std::string SemanticFingerprint::childPath(const std::string_view prefix, const std::string_view name) {
        return prefix.empty() ? std::string{name} : std::string{prefix} + "." + std::string{name};
    }

    std::optional<std::string> SemanticFingerprint::referenceOwner(
        const config::ReferenceTarget &target, const std::string_view path) const {
        if (target.m_scope == config::EReferenceScope::root) {
            return std::string{};
        }
        if (not path.starts_with("worlds.")) {
            return std::nullopt;
        }

        const auto profile_end = path.find('.', 7u);
        const auto profile_key = path.substr(7u, profile_end - 7u);
        const std::string profile_path = "worlds." + std::string{profile_key};
        if (target.m_collection == "rules") {
            return profile_path;
        }
        const auto *profile = find(m_definition.m_worlds, profile_key);
        if (profile == nullptr or target.m_collection != "markers") {
            return std::nullopt;
        }
        if (target.m_suitability == "start" or target.m_suitability == "resource_anchor") {
            // The authored owner remains meaningful even when its feature is absent.
            return "features." + profile->m_start.m_feature.m_key;
        }

        const std::string rules_prefix = profile_path + ".rules.";
        if (target.m_suitability != "sealed_pocket" or not path.starts_with(rules_prefix)) {
            return std::nullopt;
        }
        const auto rule_end = path.find('.', rules_prefix.size());
        const auto rule_key = path.substr(rules_prefix.size(), rule_end - rules_prefix.size());
        const auto *rule = find(profile->m_rules, rule_key);
        const auto *fill = rule == nullptr ? nullptr : std::get_if<MatterFill>(&rule->m_payload);
        if (fill == nullptr) {
            return std::nullopt;
        }
        const auto *placement = find(profile->m_rules, fill->m_placement.m_key);
        const auto *feature = placement == nullptr ? nullptr : std::get_if<Feature>(&placement->m_payload);
        if (feature == nullptr) {
            return std::nullopt;
        }
        return "features." + feature->m_feature.m_key;
    }

    void SemanticFingerprint::reference(
        const config::ReferenceTarget &target, const std::string_view key, const std::string_view path) {
        byte(18u);
        string(target.m_collection);
        integer(static_cast<std::underlying_type_t<config::EReferenceScope>>(target.m_scope));
        string(target.m_suitability);
        string(key);

        const auto owner = referenceOwner(target, path);
        byte(owner.has_value() ? 1u : 0u);
        if (owner) {
            string(*owner);
        }
        byte(19u);
    }

    u64 SemanticFingerprint::hash() const noexcept {
        return m_hash;
    }

    // ------------------------------------------------------------------
    // definition validation
    // ------------------------------------------------------------------

    std::error_code validateDefinition(const Definition &definition, Diagnostics &diagnostics) {
        PPR_RETURN_ERROR_ON_FAIL(WorldDefinition, config::validateSchema<Definition>(diagnostics));
        Validation validation{diagnostics};
        References references{definition, validation};

        const auto inspect = [&]<typename T0>(const std::string &path, const auto &member, const T0 &value) {
            using T = std::remove_cvref_t<T0>;
            if constexpr (requires { value.m_entries; }) {
                if (value.m_entries.size() < member.m_constraint.m_min or
                    value.m_entries.size() > member.m_constraint.m_max) {
                    validation.reject(path, EConfigErrc::out_of_range, "Collection cardinality is outside its bounds");
                }
                std::set<std::string_view> keys;
                for (const auto &entry: value.m_entries) {
                    if (not config::isValidKey(entry.m_key)) {
                        validation.reject(path + "." + entry.m_key, EConfigErrc::out_of_range, "Invalid semantic key");
                    }
                    if (not keys.insert(entry.m_key).second) {
                        validation.reject(
                            path + "." + entry.m_key, EConfigErrc::duplicate_key, "Duplicate semantic key");
                    }
                }
            } else if constexpr (config::details::is_optional_v<T>) {
                if constexpr (requires { member.m_constraint.validate(*value); }) {
                    if (value and member.m_constraint.validate(*value)) {
                        validation.reject(path, EConfigErrc::out_of_range, "Value is outside its scalar domain");
                    }
                    if constexpr (requires { member.m_constraint.m_target; }) {
                        if (value) {
                            references.root(*value, path, member.m_constraint.m_target.m_suitability);
                        }
                    }
                }
            } else if constexpr (requires { member.m_constraint.validate(value); }) {
                if (member.m_constraint.validate(value)) {
                    validation.reject(path, EConfigErrc::out_of_range, "Value is outside its scalar domain");
                }
                if constexpr (requires { member.m_constraint.m_target; }) {
                    references.root(value, path, member.m_constraint.m_target.m_suitability);
                }
            }
        };
        config::visitRecursive(definition, inspect);

        for (const auto *entry: sortedEntries(definition.m_worlds)) {
            validateProfile(entry->m_value, definition, "worlds." + entry->m_key, validation);
        }

        for (const auto &entry: definition.m_materials.m_entries) {
            const auto path = "materials." + entry.m_key;
            const auto &material = entry.m_value;
            if (entry.m_key == "vacuum") {
                validation.reject(path, EConfigErrc::cross_field_invalid, "Vacuum is reserved and cannot be authored");
            }
            if (material.m_representation == EMaterialRepresentation::terrain) {
                if (material.m_phase != EGenerationMatter::solid) {
                    validation.reject(
                        path + ".phase", EConfigErrc::cross_field_invalid, "Terrain materials must be solid",
                        {path + ".representation"});
                }
                if (not material.m_diggable.has_value()) {
                    validation.reject(
                        path + ".diggable", EConfigErrc::missing_value,
                        "Terrain materials require an authored diggable flag");
                }
            } else if (material.m_diggable.has_value()) {
                validation.reject(
                    path + ".diggable", EConfigErrc::cross_field_invalid, "Particle materials cannot author diggable",
                    {path + ".representation"});
            }
        }
        for (const auto &entry: definition.m_fields.m_entries) {
            if (validateNoiseFieldDescription(entry.m_value)) {
                validation.reject(
                    "fields." + entry.m_key, EConfigErrc::out_of_range,
                    "Noise settings exceed the numeric sampling safety boundary");
            }
        }
        for (const auto *entry: sortedEntries(definition.m_features)) {
            const auto path = "features." + entry->m_key;
            const auto &feature = entry->m_value;
            const auto &bounds = feature.m_reservation;
            for (const auto &[minimum, maximum, axis]: std::array{
                     std::tuple{bounds.m_min_x_fine, bounds.m_max_x_fine, "x"},
                     std::tuple{bounds.m_min_y_fine, bounds.m_max_y_fine, "y"}}) {
                if (minimum >= maximum or static_cast<i64>(maximum) - minimum > kWorldEdge) {
                    validation.reject(
                        path + ".reservation.max_" + axis + "_fine", EConfigErrc::cross_field_invalid,
                        "Reservation must have positive extents no larger than the world",
                        {path + ".reservation.min_" + axis + "_fine"});
                }
            }
            for (const auto *operation: orderedEntries(feature.m_operations, path + ".operations", validation)) {
                // Legacy authoring geometry uses three fine cells per metre.
                validateOperationFit(
                    operation->m_value, bounds, 3.0, path + ".operations." + operation->m_key, validation);
            }
            for (const auto *marker: sortedEntries(feature.m_markers)) {
                const auto &value = marker->m_value;
                const bool invalid_x = value.m_x_fine < bounds.m_min_x_fine or value.m_x_fine >= bounds.m_max_x_fine;
                const bool invalid_y = value.m_y_fine < bounds.m_min_y_fine or value.m_y_fine >= bounds.m_max_y_fine;
                if (invalid_x or invalid_y) {
                    validation.reject(
                        path + ".markers." + marker->m_key + (invalid_x ? ".x_fine" : ".y_fine"),
                        EConfigErrc::cross_field_invalid, "Marker cell must lie inside its reservation");
                }
            }
        }

        u64 blob_count = 0u;
        Array<std::string> blob_count_paths;
        for (const auto &entry: definition.m_biomes.m_entries) {
            const auto path = "biomes." + entry.m_key;
            const auto &biome = entry.m_value;
            if (not (biome.m_temperature.m_variation_k < biome.m_temperature.m_base_k)) {
                validation.reject(
                    path + ".temperature.variation_k", EConfigErrc::cross_field_invalid,
                    "Temperature variation must be less than base temperature", {path + ".temperature.base_k"});
            }
            const auto bands = orderedEntries(biome.m_composition, path + ".composition", validation);
            double previous = 0.0;
            std::string previous_path;
            for (const auto *band_entry: bands) {
                const auto band_path = path + ".composition." + band_entry->m_key;
                const auto &band = band_entry->m_value;
                strictInterval(band.m_depth.m_min, band.m_depth.m_max, band_path + ".depth", validation);
                if (band.m_bed_period_m == 0.0 and (band.m_bed_warp_m != 0.0 or band.m_bed_dip != 0.0)) {
                    validation.reject(
                        band_path + ".bed_period_m", EConfigErrc::cross_field_invalid,
                        "Bed warp and dip require a positive bed period",
                        {band_path + ".bed_warp_m", band_path + ".bed_dip"});
                }
                if (band.m_depth.m_min != previous) {
                    Array<std::string> related;
                    if (not previous_path.empty()) {
                        related.push_back(previous_path);
                    }
                    validation.reject(
                        band_path + ".depth.min", EConfigErrc::cross_field_invalid,
                        "Composition must cover [0,1] contiguously in authored order", std::move(related));
                }
                previous = band.m_depth.m_max;
                previous_path = band_path + ".depth.max";

                double total = 0.0;
                bool non_vacuum = false;
                const auto choices = sortedEntries(band.m_materials);
                for (const auto *choice: choices) {
                    const double next = total + choice->m_value.m_weight;
                    if (not std::isfinite(next) or not (next > total)) {
                        validation.reject(
                            band_path + ".materials." + choice->m_key + ".weight", EConfigErrc::cross_field_invalid,
                            "Cumulative weights must increase representably");
                    }
                    total = next;
                    non_vacuum = non_vacuum or choice->m_value.m_material.m_key != "vacuum";
                }

                double cumulative = 0.0, previous_fraction = 0.0;
                for (const auto *choice: choices) {
                    cumulative += choice->m_value.m_weight;
                    const double fraction = cumulative / total;
                    if (fraction <= previous_fraction) {
                        validation.reject(
                            band_path + ".materials." + choice->m_key + ".weight", EConfigErrc::cross_field_invalid,
                            "Normalized cumulative weights must increase representably");
                    }
                    previous_fraction = fraction;
                }
                if (not non_vacuum) {
                    validation.reject(
                        band_path + ".materials", EConfigErrc::cross_field_invalid,
                        "Each composition band requires a non-vacuum material");
                }
            }
            if (previous != 1.0) {
                validation.reject(
                    path + ".composition", EConfigErrc::cross_field_invalid,
                    "Composition must finish at normalized depth 1", {previous_path});
            }
            for (const auto *deposit: orderedEntries(biome.m_deposits, path + ".deposits", validation)) {
                const auto deposit_path = path + ".deposits." + deposit->m_key;
                const auto &value = deposit->m_value;
                strictInterval(
                    deposit->m_value.m_depth.m_min, deposit->m_value.m_depth.m_max, deposit_path + ".depth",
                    validation);
                strictInterval(
                    deposit->m_value.m_threshold.m_min, deposit->m_value.m_threshold.m_max, deposit_path + ".threshold",
                    validation);
                if (value.m_core_coverage < value.m_fringe_coverage) {
                    validation.reject(
                        deposit_path + ".fringe_coverage", EConfigErrc::cross_field_invalid,
                        "Fringe coverage must not exceed core coverage", {deposit_path + ".core_coverage"});
                }
                if (value.m_stage == EGenerationDepositStage::bulk and value.m_sprinkle_probability != 0.0) {
                    validation.reject(
                        deposit_path + ".sprinkle_probability", EConfigErrc::cross_field_invalid,
                        "Bulk deposits cannot sprinkle", {deposit_path + ".stage"});
                }
                if (value.m_blob) {
                    const auto &blob = *value.m_blob;
                    blob_count += blob.m_count;
                    blob_count_paths.push_back(deposit_path + ".blob.count");
                    if (blob.m_radius.m_min_m <= 0.0 or blob.m_radius.m_min_m > blob.m_radius.m_max_m) {
                        validation.reject(
                            deposit_path + ".blob.radius.min_m", EConfigErrc::cross_field_invalid,
                            "Blob radii must be positive and ordered", {deposit_path + ".blob.radius.max_m"});
                    }
                    if (blob.m_stretch.m_min > blob.m_stretch.m_max) {
                        validation.reject(
                            deposit_path + ".blob.stretch.max", EConfigErrc::cross_field_invalid,
                            "Blob stretch endpoints must be ordered", {deposit_path + ".blob.stretch.min"});
                    }
                    if (blob.m_orientation.m_min_degrees > blob.m_orientation.m_max_degrees) {
                        validation.reject(
                            deposit_path + ".blob.orientation.max_degrees", EConfigErrc::cross_field_invalid,
                            "Blob orientation endpoints must be ordered",
                            {deposit_path + ".blob.orientation.min_degrees"});
                    }
                } else if (value.m_core_coverage != 1.0 or
                           value.m_fringe_coverage != 1.0 or
                           value.m_sprinkle_probability != 0.0) {
                    validation.reject(
                        deposit_path + ".blob", EConfigErrc::cross_field_invalid,
                        "Coverage and sprinkle controls require a blob deposit",
                        {deposit_path + ".core_coverage", deposit_path + ".fringe_coverage",
                            deposit_path + ".sprinkle_probability"});
                }
            }
            if (biome.m_pore and biome.m_pore->m_radius.m_min_m > biome.m_pore->m_radius.m_max_m) {
                validation.reject(
                    path + ".pore.radius.max_m", EConfigErrc::cross_field_invalid,
                    "Pore radius endpoints must be ordered", {path + ".pore.radius.min_m"});
            }
        }
        if (blob_count > 4096u) {
            std::ranges::sort(blob_count_paths);
            validation.reject(
                "biomes", EConfigErrc::out_of_range, "Blob deposits must total at most 4096 blobs",
                std::move(blob_count_paths));
        }
        // The numeric validator owns the all-biomes maxima, stage caps and 24-cell
        // sum. Translate only its legacy flat names into this concrete hierarchy.
        Array<GenerationBiome> halo_biomes;
        Array<std::string> halo_names;
        Array<std::string_view> halo_paths;
        for (const auto *entry: sortedEntries(definition.m_biomes)) {
            GenerationBiome biome{};
            biome.m_pore_passes = entry->m_value.m_pore ? entry->m_value.m_pore->m_passes : 0u;
            biome.m_relaxation_passes = entry->m_value.m_relaxation.m_passes;
            biome.m_finish_passes = entry->m_value.m_finish.m_passes;
            biome.m_erosion_passes = entry->m_value.m_erosion.m_passes;
            halo_biomes.push_back(std::move(biome));
            halo_names.push_back("biomes." + entry->m_key);
        }
        for (const auto &name: halo_names) {
            halo_paths.push_back(name);
        }
        Diagnostics halo_diagnostics;
        if (const auto error = validateGenerationHalo(halo_biomes, halo_paths, &halo_diagnostics)) {
            const auto canonical_path = [](std::string property) {
                for (const auto &[flat, nested]: std::array{
                         std::pair{std::string_view{".relaxation_passes"}, std::string_view{".relaxation.passes"}},
                         std::pair{std::string_view{".finish_passes"}, std::string_view{".finish.passes"}},
                         std::pair{std::string_view{".erosion_passes"}, std::string_view{".erosion.passes"}}}) {
                    if (property.ends_with(flat)) {
                        property.replace(property.size() - flat.size(), flat.size(), nested);
                        break;
                    }
                }
                return property;
            };
            for (const auto &diagnostic: halo_diagnostics.entries()) {
                Array<std::string> related;
                for (const auto &property: diagnostic.m_related_properties) {
                    related.push_back(canonical_path(property));
                }
                std::ranges::sort(related);
                related.erase(std::unique(related.begin(), related.end()), related.end());
                validation.reject(
                    canonical_path(diagnostic.m_property), static_cast<EConfigErrc>(diagnostic.m_code.value()),
                    "Neighbor stages exceed the numeric generation halo boundary", std::move(related));
            }
        }
        PPR_RETURN_ERROR_ON_FAIL(WorldDefinition, validation.m_first);
        return {};
    }

    // ------------------------------------------------------------------
    // definition compilation
    // ------------------------------------------------------------------

    Expected<CompiledDefinition> compileDefinition(
        Definition definition, const double cell_size_m, Diagnostics &diagnostics) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, validateDefinition(definition, diagnostics));
        Validation validation{diagnostics};
        if (not std::isfinite(cell_size_m) or
            cell_size_m <= 0.0 or
            cell_size_m > 1.0 or
            not std::isfinite(1.0 / cell_size_m)) {
            validation.reject("cell_size_m", EConfigErrc::out_of_range, "Invalid numeric world cell size");
        }
        for (const auto &entry: definition.m_biomes.m_entries) {
            if (entry.m_value.m_pore and
                entry.m_value.m_pore->m_probability > 0.0 and
                entry.m_value.m_pore->m_radius.m_max_m > 32.0 * cell_size_m) {
                validation.reject(
                    "biomes." + entry.m_key + ".pore.radius.max_m", EConfigErrc::cross_field_invalid,
                    "Active pore radius exceeds 32 fine cells", {"cell_size_m"});
            }
            for (const auto &deposit: entry.m_value.m_deposits.m_entries) {
                if (deposit.m_value.m_blob) {
                    const auto &blob = *deposit.m_value.m_blob;
                    if (blob.m_radius.m_max_m * std::max(1.0, blob.m_stretch.m_max) * (1.0 + blob.m_distortion) * 1.2 >
                        kWorldEdge * cell_size_m) {
                        validation.reject(
                            "biomes." + entry.m_key + ".deposits." + deposit.m_key + ".blob.radius.max_m",
                            EConfigErrc::cross_field_invalid, "Blob extent exceeds the numeric world boundary",
                            {"cell_size_m"});
                    }
                }
            }
        }
        if (not validation.m_first) {
            for (const auto *entry: sortedEntries(definition.m_features)) {
                for (const auto *operation: sortedEntries(entry->m_value.m_operations)) {
                    const auto path = "features." + entry->m_key + ".operations." + operation->m_key;
                    validateOperationFit(
                        operation->m_value, entry->m_value.m_reservation, 1.0 / cell_size_m, path, validation);
                    const auto numeric = numericOperation(operation->m_value);
                    if (numeric.m_shape == EGenerationShape::capsule and
                        numeric.m_radius_m > kWorldEdge * cell_size_m) {
                        validation.reject(
                            shapePath(operation->m_value, path) + ".radius_m", EConfigErrc::cross_field_invalid,
                            "Capsule radius exceeds the numeric world boundary", {"cell_size_m"});
                    }
                }
            }
        }

        // Guard before narrowing, independently of the authored 256-material schema cap.
        if (definition.m_materials.m_entries.size() > 65534u) {
            validation.reject("materials", EConfigErrc::out_of_range, "Material IDs cannot use reserved ID 65535");
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, validation.m_first);

        CompiledDefinition result{};
        StreamKeys keys{validation};
        u32 next_id = 1u;
        for (const auto *entry: sortedEntries(definition.m_materials)) {
            const auto &source = entry->m_value;
            const auto id = static_cast<u16>(next_id++);
            result.m_bindings.m_materials.push_back({entry->m_key, id});
            result.m_materials.push_back(
                {id, source.m_representation == EMaterialRepresentation::terrain, source.m_diggable.value_or(false),
                    source.m_phase, source.m_density, keys.key({"materials", entry->m_key})});
        }
        for (const auto *entry: sortedEntries(definition.m_fields)) {
            const auto index = static_cast<u32>(result.m_fields.size());
            result.m_bindings.m_fields.push_back({entry->m_key, index});
            result.m_fields.push_back({keys.key({"fields", entry->m_key}), entry->m_value});
        }
        const auto field_index = [&](const FieldReference &reference) {
            return std::ranges::find(result.m_bindings.m_fields, reference.m_key, &FieldBinding::m_key)->m_index;
        };
        const auto material_id = [&](const MaterialReference &reference) -> u16 {
            if (reference.m_key == "vacuum") {
                return 0u;
            }
            return std::ranges::find(result.m_bindings.m_materials, reference.m_key, &MaterialBinding::m_key)->m_id;
        };

        for (const auto *biome_entry: sortedEntries(definition.m_biomes)) {
            const auto &entry = *biome_entry;
            const auto &source = entry.m_value;
            result.m_bindings.m_biomes.push_back({entry.m_key, static_cast<u32>(result.m_biomes.size())});
            GenerationBiome biome{};
            // GenerationBiome{} retains deferred numeric defaults; only this slice's members are compiled.
            biome.m_key = keys.key({"biomes", entry.m_key});
            biome.m_temperature_field = field_index(source.m_temperature.m_field);
            biome.m_temperature_base_k = source.m_temperature.m_base_k;
            biome.m_temperature_variation_k = source.m_temperature.m_variation_k;

            for (const auto *band_entry:
                orderedEntries(source.m_composition, "biomes." + entry.m_key + ".composition", validation)) {
                const auto &band_source = band_entry->m_value;
                GenerationBand band{};
                band.m_key = keys.key({"biomes", entry.m_key, "composition", band_entry->m_key});
                band.m_order = band_source.m_order;
                band.m_field = field_index(band_source.m_field);
                band.m_depth_min = band_source.m_depth.m_min;
                band.m_depth_max = band_source.m_depth.m_max;
                band.m_bed_period_m = band_source.m_bed_period_m;
                band.m_bed_warp_m = band_source.m_bed_warp_m;
                band.m_bed_dip = band_source.m_bed_dip;
                for (const auto *choice: sortedEntries(band_source.m_materials)) {
                    band.m_choices.push_back(
                        {keys.key(
                             {"biomes", entry.m_key, "composition", band_entry->m_key, "materials", choice->m_key}),
                            material_id(choice->m_value.m_material), choice->m_value.m_weight});
                }
                biome.m_bands.push_back(std::move(band));
            }

            for (const auto *deposit_entry:
                orderedEntries(source.m_deposits, "biomes." + entry.m_key + ".deposits", validation)) {
                const auto &deposit_source = deposit_entry->m_value;
                GenerationDeposit deposit{};
                deposit.m_key = keys.key({"biomes", entry.m_key, "deposits", deposit_entry->m_key});
                deposit.m_order = deposit_source.m_order;
                deposit.m_field = field_index(deposit_source.m_field);
                deposit.m_material = material_id(deposit_source.m_material);
                deposit.m_host = material_id(deposit_source.m_host_material);
                deposit.m_depth_min = deposit_source.m_depth.m_min;
                deposit.m_depth_max = deposit_source.m_depth.m_max;
                deposit.m_threshold_min = deposit_source.m_threshold.m_min;
                deposit.m_threshold_max = deposit_source.m_threshold.m_max;
                deposit.m_stage = deposit_source.m_stage;
                deposit.m_core_coverage = deposit_source.m_core_coverage;
                deposit.m_fringe_coverage = deposit_source.m_fringe_coverage;
                deposit.m_sprinkle_probability = deposit_source.m_sprinkle_probability;
                deposit.m_shape = EGenerationDepositShape::field;
                if (deposit_source.m_blob) {
                    const auto &blob = *deposit_source.m_blob;
                    deposit.m_shape = EGenerationDepositShape::blob;
                    deposit.m_blob_count = blob.m_count;
                    deposit.m_radius_min_m = blob.m_radius.m_min_m;
                    deposit.m_radius_max_m = blob.m_radius.m_max_m;
                    deposit.m_stretch_min = blob.m_stretch.m_min;
                    deposit.m_stretch_max = blob.m_stretch.m_max;
                    deposit.m_orientation_min_degrees = blob.m_orientation.m_min_degrees;
                    deposit.m_orientation_max_degrees = blob.m_orientation.m_max_degrees;
                    deposit.m_distortion = blob.m_distortion;
                }
                biome.m_deposits.push_back(std::move(deposit));
            }

            // Absence is explicit: zero work and valid numeric field sentinel zero.
            biome.m_pore_field = 0u;
            biome.m_pore_probability = 0.0;
            biome.m_pore_passes = 0u;
            if (source.m_pore) {
                biome.m_pore_field = field_index(source.m_pore->m_field);
                biome.m_pore_probability = source.m_pore->m_probability;
                biome.m_pore_radius_min_m = source.m_pore->m_radius.m_min_m;
                biome.m_pore_radius_max_m = source.m_pore->m_radius.m_max_m;
                biome.m_pore_affinity_min = source.m_pore->m_affinity_min;
                biome.m_pore_passes = source.m_pore->m_passes;
            }

            if (source.m_lining) {
                biome.m_lining_material = material_id(source.m_lining->m_material);
                biome.m_lining_thickness_fine = source.m_lining->m_thickness_fine;
            }
            biome.m_relaxation_passes = source.m_relaxation.m_passes;
            biome.m_relaxation_adoption = source.m_relaxation.m_adoption;
            biome.m_sprinkle_probability = source.m_sprinkle.m_probability;
            biome.m_finish_passes = source.m_finish.m_passes;
            biome.m_finish_adoption = source.m_finish.m_adoption;
            biome.m_erosion_passes = source.m_erosion.m_passes;

            if (source.m_surface_layers.m_soil) {
                biome.m_soil_material = material_id(source.m_surface_layers.m_soil->m_material);
                biome.m_soil_depth_m = source.m_surface_layers.m_soil->m_depth_m;
            }
            if (source.m_surface_layers.m_weathered) {
                biome.m_weathered_material = material_id(source.m_surface_layers.m_weathered->m_material);
                biome.m_weathered_depth_m = source.m_surface_layers.m_weathered->m_depth_m;
            }
            if (source.m_formation) {
                biome.m_formation_material = material_id(source.m_formation->m_material);
                biome.m_formation_probability = source.m_formation->m_probability;
                biome.m_formation_length_m = source.m_formation->m_length_m;
            }
            if (source.m_rind) {
                biome.m_rind_material = material_id(source.m_rind->m_material);
            }

            result.m_biomes.push_back(std::move(biome));
        }
        for (const auto *entry: sortedEntries(definition.m_features)) {
            const auto &source = entry->m_value;
            const auto &bounds = source.m_reservation;
            FeatureBinding binding{entry->m_key, static_cast<u32>(result.m_features.size()), {}};
            GenerationFeature feature{};
            feature.m_key = keys.key({"features", entry->m_key});
            feature.m_bounds = {bounds.m_min_x_fine, bounds.m_min_y_fine, bounds.m_max_x_fine, bounds.m_max_y_fine};
            feature.m_allow_reflection = source.m_allow_horizontal_reflection;

            for (const auto *operation_entry:
                orderedEntries(source.m_operations, "features." + entry->m_key + ".operations", validation)) {
                const auto &operation_source = operation_entry->m_value;
                auto operation = numericOperation(operation_source);
                operation.m_key = keys.key({"features", entry->m_key, "operations", operation_entry->m_key});
                if (const auto *paint = std::get_if<Paint>(&operation_source.m_action)) {
                    operation.m_material = material_id(paint->m_material);
                }
                feature.m_operations.push_back(operation);
            }
            for (const auto *marker: sortedEntries(source.m_markers)) {
                binding.m_markers.push_back({marker->m_key, static_cast<u32>(feature.m_markers.size()), entry->m_key});
                feature.m_markers.push_back(
                    {keys.key({"features", entry->m_key, "markers", marker->m_key}), marker->m_value.m_role,
                        marker->m_value.m_x_fine, marker->m_value.m_y_fine});
            }

            result.m_bindings.m_features.push_back(std::move(binding));
            result.m_features.push_back(std::move(feature));
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, validation.m_first);
        return result;
    }

    Expected<CompiledRecipe> compileRecipe(
        Definition definition, const ProfileReference &profile_reference, const GenerationCompileParameters parameters,
        Diagnostics &diagnostics) {
        Validation validation{diagnostics};
        const auto positive = [&](const double value, const double maximum, const std::string &path) {
            if (not std::isfinite(value) or value <= 0.0 or value > maximum) {
                validation.reject(path, EConfigErrc::out_of_range, "Compile parameter is outside its numeric domain");
            }
        };
        positive(parameters.m_cell_size_m, 1.0, "cell_size_m");
        if (parameters.m_cell_size_m > 0.0 and not std::isfinite(1.0 / parameters.m_cell_size_m)) {
            validation.reject("cell_size_m", EConfigErrc::out_of_range, "Cell scale is not representable");
        }
        positive(parameters.m_reference_thickness_m, 4096.0, "reference_thickness_m");
        positive(parameters.m_actor_half_width_m, kWorldEdge * parameters.m_cell_size_m, "actor_half_width_m");
        positive(parameters.m_actor_half_height_m, kWorldEdge * parameters.m_cell_size_m, "actor_half_height_m");
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, validation.m_first);

        // Compile the owned global catalogs before resolving the selected profile.
        auto globals = definition;
        globals.m_worlds.m_entries.clear();
        auto fragment = compileDefinition(std::move(globals), parameters.m_cell_size_m, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, fragment);

        const auto *profile = find(definition.m_worlds, profile_reference.m_key);
        const std::string path = "worlds." + profile_reference.m_key;
        if (profile == nullptr) {
            validation.reject(path, EConfigErrc::cross_field_invalid, "Unknown profile reference", {path, "worlds"});
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, validation.m_first);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, validateDefinition(definition, diagnostics));

        CompiledRecipe result;
        result.m_bindings = std::move(fragment->m_bindings);
        auto &recipe = result.m_recipe;
        recipe.m_materials = std::move(fragment->m_materials);
        recipe.m_fields = std::move(fragment->m_fields);
        recipe.m_biomes = std::move(fragment->m_biomes);
        recipe.m_features = std::move(fragment->m_features);
        const auto material_id = [&](const MaterialReference &reference) {
            return std::ranges::find(result.m_bindings.m_materials, reference.m_key, &MaterialBinding::m_key)->m_id;
        };
        const auto field_index = [&](const FieldReference &reference) {
            return std::ranges::find(result.m_bindings.m_fields, reference.m_key, &FieldBinding::m_key)->m_index;
        };
        const auto biome_index = [&](const BiomeReference &reference) {
            return std::ranges::find(result.m_bindings.m_biomes, reference.m_key, &BiomeBinding::m_key)->m_index;
        };
        const auto feature_index = [&](const FeatureReference &reference) {
            return std::ranges::find(result.m_bindings.m_features, reference.m_key, &FeatureBinding::m_key)->m_index;
        };
        const auto marker_index = [&](const FeatureReference &owner, const MarkerReference &reference) {
            const auto &markers =
                std::ranges::find(result.m_bindings.m_features, owner.m_key, &FeatureBinding::m_key)->m_markers;
            return std::ranges::find(markers, reference.m_key, &MarkerBinding::m_key)->m_index;
        };

        recipe.m_cell_size_m = parameters.m_cell_size_m;
        recipe.m_reference_thickness_m = parameters.m_reference_thickness_m;
        recipe.m_actor_half_width_m = parameters.m_actor_half_width_m;
        recipe.m_actor_half_height_m = parameters.m_actor_half_height_m;
        recipe.m_boundary_material = material_id(profile->m_boundaries.m_material);
        recipe.m_boundary_fine = profile->m_boundaries.m_thickness_fine;
        recipe.m_boundary_warp_amplitude_m = profile->m_boundaries.m_warp_amplitude_m;
        recipe.m_warp_x_field = field_index(profile->m_boundaries.m_warp_x_field);
        recipe.m_warp_y_field = field_index(profile->m_boundaries.m_warp_y_field);
        recipe.m_min_sites = profile->m_layout.m_min_sites;
        recipe.m_max_sites = profile->m_layout.m_max_sites;
        recipe.m_site_attempts = profile->m_layout.m_site_attempts;
        recipe.m_site_spacing_m = profile->m_layout.m_site_spacing_m;
        recipe.m_start_biome = biome_index(profile->m_start.m_biome);
        recipe.m_start_feature = feature_index(profile->m_start.m_feature);
        recipe.m_start_marker = marker_index(profile->m_start.m_feature, profile->m_start.m_marker);
        recipe.m_start_x_fine = profile->m_start.m_x_fine;
        recipe.m_start_y_fine = profile->m_start.m_y_fine;
        recipe.m_start_clearance_m = profile->m_start.m_clearance_m;
        recipe.m_start_route_width_m = profile->m_start.m_route_width_m;
        recipe.m_start_depth_m = profile->m_start.m_depth_m;
        if (profile->m_surface_bedrock) {
            const auto &surface = *profile->m_surface_bedrock;
            recipe.m_open_surface = true;
            recipe.m_surface_field = field_index(surface.m_surface_field);
            recipe.m_bedrock_field = field_index(surface.m_bedrock_field);
            recipe.m_surface_elevation_m = surface.m_surface_elevation_m;
            recipe.m_surface_amplitude_m = surface.m_surface_amplitude_m;
            recipe.m_bedrock_thickness_m = surface.m_bedrock_thickness_m;
            recipe.m_bedrock_variation_m = surface.m_bedrock_variation_m;
            recipe.m_bedrock_material = material_id(surface.m_bedrock_material);
        }

        StreamKeys keys{validation};
        for (const auto *entry: sortedEntries(profile->m_biomes)) {
            const auto &choice = entry->m_value;
            recipe.m_constraints.push_back(
                {keys.key({"worlds", profile_reference.m_key, "biomes", entry->m_key}), biome_index(choice.m_biome),
                    choice.m_required_count, choice.m_weight, choice.m_depth.m_min, choice.m_depth.m_max,
                    choice.m_distance_min_m, choice.m_distance_max_m});
        }
        for (const auto *entry: sortedEntries(profile->m_resources)) {
            const auto &resource = entry->m_value;
            recipe.m_resources.push_back(
                {keys.key({"worlds", profile_reference.m_key, "resources", entry->m_key}),
                    marker_index(profile->m_start.m_feature, resource.m_marker), resource.m_minimum_fine_cells,
                    material_id(resource.m_material), resource.m_max_distance_m});
        }

        // Pass one fixes every enabled rule's final index before any rule-to-rule lookup.
        const auto rules = orderedRules(*profile, path, validation);
        for (const auto *entry: rules) {
            const auto index = static_cast<u32>(recipe.m_rules.size());
            result.m_bindings.m_rules.push_back({entry->m_key, index, profile_reference.m_key});
            GenerationRule rule;
            rule.m_key = keys.key({"worlds", profile_reference.m_key, "rules", entry->m_key});
            rule.m_order = entry->m_value.m_order;
            rule.m_phase = phaseOf(entry->m_value.m_payload);
            recipe.m_rules.push_back(std::move(rule));
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, validation.m_first);

        // Pass two compiles only the active alternative; carve/gas flags are derived.
        for (std::size_t i = 0u; i < rules.size(); ++i) {
            std::visit(
                [&](const auto &payload) {
                    using T = std::remove_cvref_t<decltype(payload)>;
                    auto &numeric = recipe.m_rules[i].m_parameters;
                    if constexpr (std::is_same_v<T, Cave>) {
                        numeric = GenerationCave{
                            biome_index(payload.m_biome), field_index(payload.m_field), payload.m_depth.m_min,
                            payload.m_depth.m_max, payload.m_threshold.m_min, payload.m_threshold.m_max};
                    } else if constexpr (std::is_same_v<T, CarveWorm> or std::is_same_v<T, VeinWorm>) {
                        const auto &geometry = payload.m_geometry;
                        GenerationWorm worm;
                        worm.m_biome = biome_index(geometry.m_biome);
                        worm.m_count = geometry.m_count;
                        worm.m_carve = std::is_same_v<T, CarveWorm>;
                        worm.m_depth_min = geometry.m_depth.m_min;
                        worm.m_depth_max = geometry.m_depth.m_max;
                        worm.m_length_min_m = geometry.m_length_min_m;
                        worm.m_length_max_m = geometry.m_length_max_m;
                        worm.m_radius_min_m = geometry.m_radius_min_m;
                        worm.m_radius_max_m = geometry.m_radius_max_m;
                        worm.m_turn_probability = geometry.m_turn_probability;
                        worm.m_turn_smoothing = geometry.m_turn_smoothing;
                        worm.m_radius_variation = geometry.m_radius_variation;
                        worm.m_chamber_probability = geometry.m_chamber_probability;
                        worm.m_chamber_scale = geometry.m_chamber_scale;
                        if constexpr (std::is_same_v<T, VeinWorm>) {
                            worm.m_material = material_id(payload.m_material);
                            worm.m_host = material_id(payload.m_host_material);
                        }
                        numeric = worm;
                    } else if constexpr (std::is_same_v<T, Connection>) {
                        numeric = GenerationConnection{
                            payload.m_radius_m, payload.m_extra_links, payload.m_natural_anchors, payload.m_wells,
                            payload.m_branches, payload.m_roughness_m};
                    } else if constexpr (std::is_same_v<T, Feature>) {
                        numeric = GenerationPlacement{
                            feature_index(payload.m_feature), biome_index(payload.m_biome), payload.m_min_count,
                            payload.m_max_count, payload.m_attempts, payload.m_priority, payload.m_depth.m_min,
                            payload.m_depth.m_max, payload.m_spacing_m, payload.m_reflect};
                    } else if constexpr (std::is_same_v<T, MatterFill>) {
                        const auto binding = std::ranges::find(
                            result.m_bindings.m_rules, payload.m_placement.m_key, &RuleBinding::m_key);
                        const auto &placement = std::get<Feature>(
                            find(profile->m_rules, payload.m_placement.m_key)->m_payload);
                        const auto *material = find(definition.m_materials, payload.m_material.m_key);
                        const bool gas = material->m_phase == EGenerationMatter::gas;
                        numeric = GenerationFill{
                            binding->m_index, marker_index(placement.m_feature, payload.m_marker),
                            material_id(payload.m_material), gas, payload.m_temperature_k, payload.m_bulk_density_kg_m3,
                            payload.m_fill_height_m};
                        if (payload.m_fill_height_m) {
                            const auto &bounds = recipe.m_features[feature_index(placement.m_feature)].m_bounds;
                            const double cells_per_metre = 1.0 / parameters.m_cell_size_m;
                            const double cells = std::round(*payload.m_fill_height_m * cells_per_metre);
                            if (not std::isfinite(cells) or
                                cells <= bounds.m_min_y or
                                cells > bounds.m_max_y or
                                *payload.m_fill_height_m != cells / cells_per_metre) {
                                validation.reject(
                                    path + ".rules." + rules[i]->m_key + ".payload.matter_fill.fill_height_m",
                                    EConfigErrc::cross_field_invalid,
                                    "Fill height must align to a fine-cell boundary inside its feature",
                                    {"features." + placement.m_feature.m_key + ".reservation", "cell_size_m"});
                            }
                        }
                    }
                },
                rules[i]->m_value.m_payload);
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, validation.m_first);
        if (const auto error = validateGenerationRecipe(recipe)) {
            validation.reject(
                path,
                error == std::errc::value_too_large ? EConfigErrc::out_of_range : EConfigErrc::cross_field_invalid,
                "Compiled profile fails the full numeric recipe safety boundary");
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldDefinition, validation.m_first);
        return result;
    }

    // ------------------------------------------------------------------
    // fingerprints
    // ------------------------------------------------------------------

    u64 definitionFingerprint(const Definition &definition) {
        SemanticFingerprint fingerprint{definition};
        fingerprint.string("ppr.worldgen.definition.v2");
        fingerprint.value(definition);
        return fingerprint.hash();
    }

    u64 definitionRulesFingerprint(const Definition &definition) {
        SemanticFingerprint fingerprint{definition};
        fingerprint.string("ppr.worldgen.rules.v2");
        fingerprint.integer(static_cast<u64>(definition.m_worlds.m_entries.size()));
        for (const auto *profile: sortedEntries(definition.m_worlds)) {
            fingerprint.string(profile->m_key);
            fingerprint.value(profile->m_value.m_rules, "worlds." + profile->m_key + ".rules");
        }
        return fingerprint.hash();
    }
}
