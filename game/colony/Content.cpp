module;
#include "pP/Macros.h"
module game.colony.content;
import engine.core;
import engine.sim;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyContent, info, none)

    // ------------------------------------------------------------------
    // decode helpers
    // ------------------------------------------------------------------

    namespace {
        using namespace config;
        constexpr Metadata metadata(const std::string_view name) {
            return {name, "World content", "Content", EMutability::startup, ESource::file};
        }
        constexpr std::array<ContentDocumentBinding, kContentDocumentCount> kBindings{
            {{EContentDocument::materials, "materials", "Materials"}, {EContentDocument::biomes, "biomes", "Biomes"},
                {EContentDocument::fields, "fields", "Fields"}, {EContentDocument::features, "features", "Features"},
                {EContentDocument::worlds, "worlds", "Worlds"}, {EContentDocument::rules, "rules", "Rules"},
                {EContentDocument::elements, "elements", "Elements"},
                {EContentDocument::textures, "textures", "Textures"},
                {EContentDocument::backgrounds, "backgrounds", "Backgrounds"}}};
        struct WorldGenDefinitionEnvelope : sim::definition::Definition {
            u32 m_definition_schema_version{};
            static constexpr auto describe() {
                return std::tuple_cat(
                    std::tuple{field(
                        &WorldGenDefinitionEnvelope::m_definition_schema_version, metadata("definition_schema_version"),
                        Range{sim::definition::kDefinitionSchemaVersion, sim::definition::kDefinitionSchemaVersion},
                        EPresence::required)},
                    sim::definition::Definition::describe());
            }
        };
        std::error_code reject(
            Diagnostics &diagnostics, const std::string_view location, const std::string_view path,
            const std::string_view message, const EConfigErrc code = EConfigErrc::cross_field_invalid) {
            const auto error = config::make_error_code(code);
            diagnostics.add(std::string{location}, std::string{path}, std::string{message}, error);
            return error;
        }
        std::string_view location(const Document &document, std::string_view path) {
            while (not path.empty()) {
                const auto entry = std::ranges::find(document.m_entries, path, &Entry::m_key);
                if (entry != document.m_entries.end()) {
                    return entry->m_location;
                }
                const auto group = std::ranges::find(document.m_groups, path, &GroupPresence::m_path);
                if (group != document.m_groups.end()) {
                    return group->m_location;
                }
                const auto dot = path.rfind('.');
                path = dot == std::string_view::npos ? std::string_view{} : path.substr(0u, dot);
            }
            return "pack";
        }
        void appendDiagnostics(Diagnostics &to, const Diagnostics &from, const Document &document) {
            for (const auto &entry: from.entries()) {
                to.add(
                    std::string{location(document, entry.m_property)}, entry.m_property, entry.m_message, entry.m_code,
                    entry.m_alias, entry.m_related_properties);
            }
            to.addOmitted(from.omittedCount());
        }
        bool schemaMatches(std::string_view schema, std::string_view path) {
            while (not schema.empty() and not path.empty()) {
                const auto schema_dot = schema.find('.');
                const auto path_dot = path.find('.');
                const auto a = schema.substr(0u, schema_dot);
                const auto b = path.substr(0u, path_dot);
                if (a != "{key}" and a != b) {
                    return false;
                }
                if (schema_dot == std::string_view::npos or path_dot == std::string_view::npos) {
                    return schema_dot == path_dot;
                }
                schema.remove_prefix(schema_dot + 1u);
                path.remove_prefix(path_dot + 1u);
            }
            return schema.empty() and path.empty();
        }

        template<typename Visitor>
        void visitDescriptorSchema(const EContentDocument category, Visitor &&visitor) {
            switch (category) {
                case EContentDocument::elements: visitor(std::type_identity<sim::asset::ElementDescriptor>{}); break;
                case EContentDocument::materials: visitor(std::type_identity<MaterialAssetDescriptor>{}); break;
                case EContentDocument::fields: visitor(std::type_identity<sim::asset::FieldDescriptor>{}); break;
                case EContentDocument::biomes: visitor(std::type_identity<BiomeAssetDescriptor>{}); break;
                case EContentDocument::features: visitor(std::type_identity<FeatureAssetDescriptor>{}); break;
                case EContentDocument::rules: visitor(std::type_identity<sim::asset::RuleDescriptor>{}); break;
                case EContentDocument::worlds: visitor(std::type_identity<WorldAssetDescriptor>{}); break;
                case EContentDocument::textures: visitor(std::type_identity<image::TextureDescriptor>{}); break;
                case EContentDocument::backgrounds: visitor(std::type_identity<BackgroundAssetDescriptor>{}); break;
                default: break;
            }
        }

        template<typename T>
        const T *findEntry(const KeyedCollection<T> &collection, const std::string_view key) {
            const auto entry = std::ranges::find(collection.m_entries, key, &KeyedEntry<T>::m_key);
            return entry == collection.m_entries.end() ? nullptr : std::addressof(entry->m_value);
        }
        template<typename T>
        Array<const KeyedEntry<T> *> sorted(const KeyedCollection<T> &collection) {
            Array<const KeyedEntry<T> *> result;
            for (const auto &entry: collection.m_entries) {
                result.push_back(std::addressof(entry));
            }
            std::ranges::sort(result, {}, [](const auto *entry) { return entry->m_key; });
            return result;
        }
        Expected<u32> color(
            const std::string &value, const std::string &path, const Document &document, Diagnostics &diagnostics) {
            return decodeAppearanceColor(value, location(document, path), path, diagnostics);
        }
    }

    // ------------------------------------------------------------------
    // document bindings
    // ------------------------------------------------------------------

    std::span<const ContentDocumentBinding> contentDocumentBindings() noexcept {
        return kBindings;
    }
    // ------------------------------------------------------------------
    // property metadata
    // ------------------------------------------------------------------

    std::optional<ContentPropertyDescriptor> contentPropertyDescriptor(
        const EContentDocument category, const std::string_view relative, const Document &, const std::string_view) {
        const std::string_view path = relative;
        std::optional<ContentPropertyDescriptor> result;
        const auto inspect = [&](const std::string &schema_path, const auto &member, const auto) {
            const auto descriptor = config::describeProperty(member);
            const bool tag = descriptor.m_kind == ENodeKind::variant and
                             path.ends_with(".tag") and
                             schemaMatches(schema_path, std::string_view{path}.substr(0u, path.size() - 4u));
            if (schemaMatches(schema_path, path) or tag) {
                ContentPropertyDescriptor value;
                static_cast<PropertyDescriptor &>(value) = descriptor;
                if constexpr (requires { member.m_constraint.m_max_length; }) {
                    value.m_text_length = member.m_constraint.m_max_length;
                }
                if (tag) {
                    value.m_kind = ENodeKind::scalar;
                }
                for (const auto &binding: kBindings) {
                    if (value.m_reference_target and value.m_reference_target->m_collection == binding.m_collection) {
                        value.m_reference_category = binding.m_document;
                    }
                }
                result = std::move(value);
            }
        };

        visitDescriptorSchema(
            category, [&]<typename T>(const std::type_identity<T>) { config::describeRecursive<T>(inspect); });

        return result;
    }

    // ------------------------------------------------------------------
    // worldgen definition envelope
    // ------------------------------------------------------------------

    Expected<sim::definition::Definition> decodeWorldGenDefinition(
        const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics) try {
        auto document = decode(bytes, origin, diagnostics, EDecodeMode::tree);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
        const TreeLayer layers[] = {toTreeLayer(*document, std::string{origin})};
        Diagnostics local{64u};
        auto resolved = resolveRecursive<WorldGenDefinitionEnvelope>(layers, local);
        const auto error = resolved ? sim::definition::validateDefinition(resolved->m_value, local) : resolved.error();

        // Sim reports logical paths only. Prefer the exact scalar's location,
        // then the nearest authored table, then the supplied document origin.
        for (const auto &diagnostic: local.entries()) {
            std::string_view location = origin;
            std::string_view path = diagnostic.m_property;
            while (not path.empty()) {
                const auto entry = std::ranges::find(document->m_entries, path, &Entry::m_key);
                if (entry != document->m_entries.end()) {
                    location = entry->m_location;
                    break;
                }
                const auto group = std::ranges::find(document->m_groups, path, &GroupPresence::m_path);
                if (group != document->m_groups.end()) {
                    location = group->m_location;
                    break;
                }
                const auto dot = path.rfind('.');
                path = dot == std::string_view::npos ? std::string_view{} : path.substr(0u, dot);
            }
            diagnostics.add(
                std::string{location}, diagnostic.m_property, diagnostic.m_message, diagnostic.m_code,
                diagnostic.m_alias, diagnostic.m_related_properties);
        }
        diagnostics.addOmitted(local.omittedCount());
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
        return std::move(static_cast<sim::definition::Definition &>(resolved->m_value));
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::state_not_recoverable);
        std::unreachable();
    }

    // ------------------------------------------------------------------
    // content catalog
    // ------------------------------------------------------------------

    const PackDefinition &ContentCatalog::definition() const noexcept {
        return m_definition;
    }
    std::span<const MaterialDefinition> ContentCatalog::materials() const noexcept {
        return m_materials;
    }
    const MaterialDefinition *ContentCatalog::findMaterial(const u16 id) const noexcept {
        const auto found = std::ranges::find(m_materials, id, &MaterialDefinition::m_numeric_id);
        return found == m_materials.end() ? nullptr : std::addressof(*found);
    }
    const MaterialDefinition *ContentCatalog::findMaterial(const std::string_view key) const noexcept {
        const auto found = std::ranges::find(m_materials, key, &MaterialDefinition::m_key);
        return found == m_materials.end() ? nullptr : std::addressof(*found);
    }

    Expected<ContentCatalog> projectContentCatalog(
        PackDefinition definition, const Document &document, Diagnostics &diagnostics) {
        // Programmatic callers validate the typed tree's declared domains.
        // Descriptor imports have already resolved these domains once.
        Diagnostics visual_diagnostics{64u};
        config::details::validateStoredTreeValue(definition.m_appearance, "", visual_diagnostics);
        for (const auto &entry: visual_diagnostics.entries()) {
            const std::string path = "appearance." + entry.m_property;
            diagnostics.add(std::string{location(document, path)}, path, entry.m_message, entry.m_code);
        }
        diagnostics.addOmitted(visual_diagnostics.omittedCount());
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent,
            visual_diagnostics.hasErrors() ? visual_diagnostics.entries().front().m_code : std::error_code{});

        Diagnostics compile_diagnostics{128u};
        auto compiled = sim::definition::compileDefinition(definition, 1.0 / 3.0, compile_diagnostics);
        appendDiagnostics(diagnostics, compile_diagnostics, document);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, compiled);

        return ContentCatalog::project(
            std::move(definition), document, std::addressof(*compiled), nullptr, diagnostics);
    }

    Expected<ContentCatalog> ContentCatalog::project(
        PackDefinition definition, const Document &document,
        const sim::definition::CompiledDefinition *const standalone,
        const sim::definition::CompiledRecipe *const selected, Diagnostics &diagnostics) {
        ContentCatalog catalog;
        catalog.m_definition = std::move(definition);
        const auto &source = catalog.m_definition;
        const auto &appearance = source.m_appearance;
        const auto &bindings = selected ? selected->m_bindings : standalone->m_bindings;
        const auto &materials = selected ? selected->m_recipe.m_materials : standalone->m_materials;
        const auto &fields = selected ? selected->m_recipe.m_fields : standalone->m_fields;
        const auto &biomes = selected ? selected->m_recipe.m_biomes : standalone->m_biomes;
        const auto &features = selected ? selected->m_recipe.m_features : standalone->m_features;

        // Appearance entries must name a real simulation owner; every owner needs its visual projection.
        const auto owners = [&](const auto &visuals, const auto &simulation,
                                const std::string_view root) -> std::error_code {
            for (const auto &entry: visuals.m_entries) {
                if (findEntry(simulation, entry.m_key) == nullptr) {
                    const std::string path = "appearance." + std::string{root} + "." + entry.m_key;
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(diagnostics, location(document, path), path, "Appearance owner does not exist"));
                }
            }
            for (const auto &entry: simulation.m_entries) {
                if (findEntry(visuals, entry.m_key) == nullptr) {
                    const std::string path = "appearance." + std::string{root} + "." + entry.m_key;
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, location(document, path), path,
                                           "Simulation owner requires appearance", EConfigErrc::missing_value));
                }
            }
            return {};
        };
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, owners(appearance.m_materials, source.m_materials, "materials"));
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, owners(appearance.m_biomes, source.m_biomes, "biomes"));
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, owners(appearance.m_features, source.m_features, "features"));
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, owners(appearance.m_worlds, source.m_worlds, "worlds"));

        for (std::size_t i = 0u; i < bindings.m_materials.size(); ++i) {
            const auto &binding = bindings.m_materials[i];
            const auto &numeric = materials[i];
            const auto &visual = *findEntry(appearance.m_materials, binding.m_key);
            const auto &semantic = *findEntry(source.m_materials, binding.m_key);
            const std::string root = "appearance.materials." + binding.m_key;
            auto tint = color(visual.m_color_rgba, root + ".color_rgba", document, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, tint);
            if (numeric.m_terrain and (not visual.m_albedo_texture or not visual.m_texture_period_m)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, location(document, root), root,
                        "Terrain appearance requires albedo_texture and texture_period_m", EConfigErrc::missing_value));
            }
            if (not numeric.m_terrain and visual.m_roundness) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location(document, root + ".roundness"), root + ".roundness",
                                       "Particle appearance cannot declare terrain roundness"));
            }
            if ((visual.m_albedo_texture or visual.m_normal_texture) and not visual.m_texture_period_m) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location(document, root), root + ".texture_period_m",
                                       "Textured materials require a world-space period"));
            }
            for (const auto &name: {"albedo_texture", "normal_texture"}) {
                const auto &path = std::string_view{name} == "albedo_texture" ? visual.m_albedo_texture
                                                                              : visual.m_normal_texture;
                if (path and not asset::qualifyName(*path, "programmatic")) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, location(document, root + "." + name), root + "." + name,
                                           "Expected a valid local or qualified Texture asset name"));
                }
            }
            catalog.m_materials.push_back(
                {binding.m_key, binding.m_id, visual.m_display_name, static_cast<EMaterialPhase>(numeric.m_matter),
                    numeric.m_terrain ? EMaterialRepresentation::terrain : EMaterialRepresentation::particle, *tint,
                    visual.m_albedo_texture, visual.m_texture_period_m, semantic.m_diggable,
                    numeric.m_reference_density_kg_m3, std::string{location(document, root + ".albedo_texture")},
                    visual.m_roundness.value_or(1.0), visual.m_metallic, visual.m_roughness,
                    {visual.m_emissive_r, visual.m_emissive_g, visual.m_emissive_b}, visual.m_emissive_strength,
                    visual.m_alpha_mode, visual.m_ior, visual.m_normal_texture, visual.m_normal_scale,
                    visual.m_refraction_depth_m, std::string{location(document, root + ".normal_texture")},
                    visual.m_texture_sampling});
        }
        for (const auto &binding: bindings.m_fields) {
            catalog.m_fields.push_back({binding.m_key, fields[binding.m_index].m_noise});
        }
        const auto fieldKey = [&](const u32 index) -> const std::string & { return bindings.m_fields[index].m_key; };
        const auto biomeKey = [&](const u32 index) -> const std::string & { return bindings.m_biomes[index].m_key; };
        const auto featureKey = [&](const u32 index) -> const std::string & {
            return bindings.m_features[index].m_key;
        };
        for (const auto &binding: bindings.m_biomes) {
            const auto &numeric = biomes[binding.m_index];
            const auto &semantic = *findEntry(source.m_biomes, binding.m_key);
            const auto &visual = *findEntry(appearance.m_biomes, binding.m_key);
            auto tint = color(
                visual.m_preview_color_rgba, "appearance.biomes." + binding.m_key + ".preview_color_rgba", document,
                diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, tint);
            BiomeDefinition biome{
                .m_key = binding.m_key,
                .m_display_name = visual.m_display_name,
                .m_preview_color_rgba = *tint,
                .m_temperature_base_k = numeric.m_temperature_base_k,
                .m_temperature_variation_k = numeric.m_temperature_variation_k,
                .m_temperature_field = fieldKey(numeric.m_temperature_field)};
            biome.m_lining_material_id = numeric.m_lining_material;
            biome.m_lining_thickness_fine = numeric.m_lining_thickness_fine;
            biome.m_relaxation_passes = numeric.m_relaxation_passes;
            biome.m_relaxation_adoption = numeric.m_relaxation_adoption;
            biome.m_sprinkle_probability = numeric.m_sprinkle_probability;
            if (semantic.m_pore) {
                biome.m_pore_field = fieldKey(numeric.m_pore_field);
            }
            biome.m_pore_probability = numeric.m_pore_probability;
            biome.m_pore_radius_min_m = numeric.m_pore_radius_min_m;
            biome.m_pore_radius_max_m = numeric.m_pore_radius_max_m;
            biome.m_pore_affinity_min = numeric.m_pore_affinity_min;
            biome.m_pore_passes = numeric.m_pore_passes;
            biome.m_finish_passes = numeric.m_finish_passes;
            biome.m_finish_adoption = numeric.m_finish_adoption;
            biome.m_erosion_passes = numeric.m_erosion_passes;
            biome.m_soil_material_id = numeric.m_soil_material;
            biome.m_weathered_material_id = numeric.m_weathered_material;
            biome.m_soil_depth_m = numeric.m_soil_depth_m;
            biome.m_weathered_depth_m = numeric.m_weathered_depth_m;
            biome.m_formation_material_id = numeric.m_formation_material;
            biome.m_rind_material_id = numeric.m_rind_material;
            biome.m_formation_probability = numeric.m_formation_probability;
            biome.m_formation_length_m = numeric.m_formation_length_m;
            for (const auto &band: numeric.m_bands) {
                const auto entry = std::ranges::find_if(semantic.m_composition.m_entries, [&](const auto &item) {
                    return item.m_value.m_order == band.m_order;
                });
                BiomeCompositionBand target{
                    entry->m_key, band.m_order, band.m_depth_min, band.m_depth_max, fieldKey(band.m_field), {}, 0.0,
                    band.m_bed_period_m, band.m_bed_warp_m, band.m_bed_dip};
                const auto choices = sorted(entry->m_value.m_materials);
                for (std::size_t i = 0u; i < choices.size(); ++i) {
                    target.m_materials.push_back(
                        {choices[i]->m_key, band.m_choices[i].m_material, band.m_choices[i].m_weight});
                    target.m_total_weight += band.m_choices[i].m_weight;
                }
                biome.m_composition.push_back(std::move(target));
            }
            for (const auto &deposit: numeric.m_deposits) {
                const auto entry = std::ranges::find_if(semantic.m_deposits.m_entries, [&](const auto &item) {
                    return item.m_value.m_order == deposit.m_order;
                });
                biome.m_deposits.push_back(
                    {entry->m_key, deposit.m_order, fieldKey(deposit.m_field), deposit.m_material, deposit.m_host,
                        deposit.m_depth_min, deposit.m_depth_max, deposit.m_threshold_min, deposit.m_threshold_max,
                        deposit.m_shape, deposit.m_stage, deposit.m_blob_count, deposit.m_radius_min_m,
                        deposit.m_radius_max_m, deposit.m_stretch_min, deposit.m_stretch_max, deposit.m_distortion,
                        deposit.m_core_coverage, deposit.m_fringe_coverage, deposit.m_sprinkle_probability,
                        deposit.m_orientation_min_degrees, deposit.m_orientation_max_degrees});
            }
            catalog.m_biomes.push_back(std::move(biome));
        }
        for (const auto &binding: bindings.m_features) {
            const auto &numeric = features[binding.m_index];
            const auto &semantic = *findEntry(source.m_features, binding.m_key);
            const auto &visual = *findEntry(appearance.m_features, binding.m_key);
            FeatureDefinition feature{
                .m_key = binding.m_key,
                .m_display_name = visual.m_display_name,
                .m_allow_horizontal_reflection = numeric.m_allow_reflection,
                .m_reservation = {
                    numeric.m_bounds.m_min_x, numeric.m_bounds.m_min_y, numeric.m_bounds.m_max_x,
                    numeric.m_bounds.m_max_y}};
            for (const auto &operation: numeric.m_operations) {
                const auto entry = std::ranges::find_if(semantic.m_operations.m_entries, [&](const auto &item) {
                    return item.m_value.m_order == operation.m_order;
                });
                feature.m_operations.push_back(
                    {entry->m_key, operation.m_order, operation.m_carve ? EFeatureAction::carve : EFeatureAction::paint,
                        static_cast<EFeatureShape>(operation.m_shape), operation.m_x, operation.m_y, operation.m_width,
                        operation.m_height, operation.m_end_x, operation.m_end_y, operation.m_radius_m,
                        operation.m_material});
            }
            for (const auto &marker: binding.m_markers) {
                const auto &value = numeric.m_markers[marker.m_index];
                feature.m_markers.push_back(
                    {marker.m_key, static_cast<EFeatureMarkerRole>(value.m_role), value.m_x, value.m_y});
            }
            catalog.m_features.push_back(std::move(feature));
        }
        for (const auto *entry: sorted(source.m_worlds)) {
            std::optional<sim::definition::CompiledRecipe> programmatic;
            const auto *compiled_world = selected;
            if (compiled_world == nullptr) {
                Diagnostics recipe_diagnostics{128u};
                auto compiled = sim::definition::compileRecipe(
                    source, {entry->m_key}, {1.0 / 3.0, 1.0, 0.4, 0.9}, recipe_diagnostics);
                appendDiagnostics(diagnostics, recipe_diagnostics, document);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, compiled);

                programmatic.emplace(std::move(*compiled));
                compiled_world = std::addressof(*programmatic);
            }

            const auto &recipe = compiled_world->m_recipe;
            const auto &visual = *findEntry(appearance.m_worlds, entry->m_key);
            WorldProfileDefinition world{
                .m_key = entry->m_key,
                .m_display_name = visual.m_display_name,
                .m_default_wall_material_id = recipe.m_boundary_material,
                .m_boundary_fine = recipe.m_boundary_fine,
                .m_min_sites = recipe.m_min_sites,
                .m_max_sites = recipe.m_max_sites,
                .m_site_attempts = recipe.m_site_attempts,
                .m_site_spacing_m = recipe.m_site_spacing_m,
                .m_boundary_warp_amplitude_m = recipe.m_boundary_warp_amplitude_m,
                .m_boundary_warp_x_field = fieldKey(recipe.m_warp_x_field),
                .m_boundary_warp_y_field = fieldKey(recipe.m_warp_y_field),
                .m_start_biome = biomeKey(recipe.m_start_biome),
                .m_start_feature = featureKey(recipe.m_start_feature),
                .m_start_marker = bindings.m_features[recipe.m_start_feature].m_markers[recipe.m_start_marker].m_key,
                .m_start_x_fine = recipe.m_start_x_fine,
                .m_start_y_fine = recipe.m_start_y_fine,
                .m_start_clearance_m = recipe.m_start_clearance_m,
                .m_start_route_width_m = recipe.m_start_route_width_m,
                .m_background_stack = visual.m_background_stack};
            if (findEntry(appearance.m_backgrounds, visual.m_background_stack) == nullptr) {
                const std::string path = "appearance.worlds." + entry->m_key + ".background_stack";
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(diagnostics, location(document, path), path, "Unknown background stack"));
            }
            if (recipe.m_open_surface) {
                world.m_surface_field = fieldKey(recipe.m_surface_field);
                world.m_bedrock_field = fieldKey(recipe.m_bedrock_field);
            }
            world.m_surface_elevation_m = recipe.m_surface_elevation_m;
            world.m_surface_amplitude_m = recipe.m_surface_amplitude_m;
            world.m_bedrock_thickness_m = recipe.m_bedrock_thickness_m;
            world.m_bedrock_variation_m = recipe.m_bedrock_variation_m;
            world.m_bedrock_material_id = recipe.m_bedrock_material;
            world.m_start_depth_m = recipe.m_start_depth_m;
            const auto biome_choices = sorted(entry->m_value.m_biomes);
            for (std::size_t i = 0u; i < recipe.m_constraints.size(); ++i) {
                const auto &value = recipe.m_constraints[i];
                world.m_biomes.push_back(
                    {biome_choices[i]->m_key, biomeKey(value.m_biome), value.m_weight, value.m_required_count,
                        value.m_depth_min, value.m_depth_max, value.m_distance_min_m, value.m_distance_max_m});
            }
            const auto resources = sorted(entry->m_value.m_resources);
            for (std::size_t i = 0u; i < recipe.m_resources.size(); ++i) {
                const auto &value = recipe.m_resources[i];
                world.m_start_resources.push_back(
                    {resources[i]->m_key, bindings.m_features[recipe.m_start_feature].m_markers[value.m_marker].m_key,
                        value.m_material, value.m_minimum_fine_cells, value.m_max_distance_m});
            }
            world.m_rule_keys.resize(recipe.m_rules.size());
            for (const auto &binding: compiled_world->m_bindings.m_rules) {
                world.m_rule_keys[binding.m_index] = binding.m_key;
            }
            catalog.m_worlds.push_back(std::move(world));
        }
        for (const auto *entry: sorted(appearance.m_backgrounds)) {
            BackgroundStackDefinition stack{.m_key = entry->m_key};
            for (const auto *layer: sorted(entry->m_value.m_layers)) {
                const auto &value = layer->m_value;
                const std::string path = "appearance.backgrounds." + entry->m_key + ".layers." + layer->m_key;
                auto tint = color(value.m_color_rgba, path + ".color_rgba", document, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, tint);
                if (not asset::qualifyName(value.m_texture, "programmatic")) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, location(document, path + ".texture"), path + ".texture",
                                           "Expected a valid local or qualified Texture asset name"));
                }
                stack.m_layers.push_back(
                    {layer->m_key, value.m_texture, std::string{location(document, path + ".texture")}, value.m_order,
                        *tint, value.m_repeat_period_m, value.m_parallax_factor});
            }
            std::ranges::sort(stack.m_layers, {}, &BackgroundLayerDefinition::m_order);
            for (u32 i = 0u; i < 4u; ++i) {
                if (stack.m_layers[i].m_order != i or (i == 0u and (stack.m_layers[i].m_color_rgba & 255u) != 255u)) {
                    const std::string path = "appearance.backgrounds." + entry->m_key + ".layers";
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, location(document, path), path,
                            "Layer orders must be unique zero through three; the farthest layer tint must be opaque"));
                }
            }
            catalog.m_backgrounds.push_back(std::move(stack));
        }
        return catalog;
    }
    Expected<Document> contentVariantSubtree(
        const EContentDocument category, const std::string_view key, const std::string_view relative,
        const std::string_view tag) {
        const std::string path{relative};
        const Document draft;
        const auto descriptor = contentPropertyDescriptor(category, relative, draft, key);
        if (not descriptor or
            descriptor->m_kind != ENodeKind::variant or
            std::ranges::find(descriptor->m_choices, tag) == descriptor->m_choices.end()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::invalid_argument);
        }
        const std::string selected = path + "." + std::string{tag};
        std::optional<Document> result;
        std::error_code error;
        const auto inspect = [&]<typename T>(
                                 const std::string &schema_path, const auto &, const std::type_identity<T>) {
            if constexpr (config::details::TDescribed<T>) {
                if (schemaMatches(schema_path, selected)) {
                    auto payload = config::documentFrom(T{});
                    if (not payload) {
                        error = payload.error();
                        return;
                    }
                    Document subtree;
                    subtree.m_entries.push_back({"tag", FileValue::text(std::string{tag}), "default"});
                    subtree.m_groups.push_back({std::string{tag}, "default"});
                    for (auto &entry: payload->m_entries) {
                        entry.m_key = std::string{tag} + "." + entry.m_key;
                        subtree.m_entries.push_back(std::move(entry));
                    }
                    for (auto &group: payload->m_groups) {
                        group.m_path = std::string{tag} + "." + group.m_path;
                        subtree.m_groups.push_back(std::move(group));
                    }
                    result = std::move(subtree);
                }
            }
        };

        visitDescriptorSchema(
            category, [&]<typename T>(const std::type_identity<T>) { config::describeRecursive<T>(inspect); });

        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
        if (not result) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::invalid_argument);
        }
        return std::move(*result);
    }

    Expected<ContentCatalog> projectContentCatalog(PackDefinition definition, Diagnostics &diagnostics) {
        const Document provenance;
        return projectContentCatalog(std::move(definition), provenance, diagnostics);
    }
}
