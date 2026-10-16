module;
#include "pP/Macros.h"

module game.colony.content;

import engine.asset;
import engine.core;
import engine.sim;
import engine.image;
import std;

namespace pP::colony {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(ContentAssembly, debug, none);

    // ------------------------------------------------------------------
    // shared appearance interpretation
    // ------------------------------------------------------------------

    Expected<u32> decodeAppearanceColor(
        const std::string_view value, const std::string_view source, const std::string_view property,
        config::Diagnostics &diagnostics) {
        u32 result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result, 16);
        if (value.size() != 8u or error != std::errc{} or end != value.data() + value.size()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAssembly,
                asset::reject(diagnostics, source, property, "Expected exactly eight hexadecimal RRGGBBAA digits"));
        }

        return result;
    }

    // ------------------------------------------------------------------
    // typed binding lookup and source provenance
    // ------------------------------------------------------------------

    namespace {
        struct SourceBinding {
            std::string m_semantic{}, m_relative{};
            const asset::DescriptorSource *m_source{};
            const Array<config::PropertyOrigin> *m_origins{};
        };

        template<typename Descriptor>
        void bindSource(
            Array<SourceBinding> &bindings, std::string semantic, std::string relative,
            const asset::Imported<Descriptor> &descriptor) {
            bindings.push_back(
                {std::move(semantic), std::move(relative), std::addressof(descriptor.m_source),
                    std::addressof(descriptor.m_origins)});
        }

        void appendAssemblyDiagnostics(
            config::Diagnostics &destination, const config::Diagnostics &source,
            const std::span<const SourceBinding> bindings, const std::string_view world) {
            for (const auto &diagnostic: source.entries()) {
                const SourceBinding *owner{};
                for (const auto &binding: bindings) {
                    if ((diagnostic.m_property == binding.m_semantic or
                            diagnostic.m_property.starts_with(binding.m_semantic + ".")) and
                        (owner == nullptr or binding.m_semantic.size() > owner->m_semantic.size())) {
                        owner = std::addressof(binding);
                    }
                }

                std::string property = diagnostic.m_property;
                std::string location{world};
                if (owner != nullptr) {
                    const auto suffix = diagnostic.m_property.substr(owner->m_semantic.size());
                    property = owner->m_relative + suffix;
                    if (property.starts_with('.')) {
                        property.erase(0u, 1u);
                    }

                    location = owner->m_source->m_path.string();
                    const auto origin = std::ranges::find(*owner->m_origins, property, &config::PropertyOrigin::m_name);
                    if (origin != owner->m_origins->end()) {
                        location = origin->m_location;
                    }
                }

                destination.add(
                    std::move(location), std::move(property),
                    "World " + std::string{world} + ": " + diagnostic.m_message, diagnostic.m_code, diagnostic.m_alias,
                    diagnostic.m_related_properties);
            }

            destination.addOmitted(source.omittedCount());
        }

        template<typename Records>
        [[nodiscard]] auto findRecord(const Records &records, const asset::AssetKey key) {
            const auto found = std::ranges::find_if(records, [&](const auto &record) { return *record.m_id == *key; });
            return found == records.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] std::error_code validateMaterialVisual(
            const MaterialAppearance &appearance, const bool terrain, const std::string_view source,
            config::Diagnostics &diagnostics) {
            const auto color = decodeAppearanceColor(
                appearance.m_color_rgba, source, "appearance.color_rgba", diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentAssembly, color);

            if (terrain and (not appearance.m_albedo_texture or not appearance.m_texture_period_m)) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentAssembly, asset::reject(
                                         diagnostics, source, "appearance",
                                         "Terrain appearance requires albedo_texture and texture_period_m",
                                         config::EConfigErrc::missing_value));
            }

            if (not terrain and appearance.m_roundness) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentAssembly, asset::reject(
                                         diagnostics, source, "appearance.roundness",
                                         "Particle appearance cannot declare terrain roundness"));
            }

            if ((appearance.m_albedo_texture or appearance.m_normal_texture) and not appearance.m_texture_period_m) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentAssembly, asset::reject(
                                         diagnostics, source, "appearance.texture_period_m",
                                         "Textured materials require a world-space period"));
            }

            return {};
        }
    }

    // ------------------------------------------------------------------
    // selected world assembly
    // ------------------------------------------------------------------

    Expected<AssembledWorldDefinition> assembleWorld(
        const ContentDescriptorCatalog &catalog, const sim::asset::WorldId world,
        const sim::definition::GenerationCompileParameters parameters, config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Content.Assembly.World"};
        auto world_identity = catalog.packs().resolve(asset::AssetKey{*world}, sim::asset::kWorldKind, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, world_identity);

        const auto *selected = findRecord(catalog.worlds(), (*world_identity)->m_key);
        if (selected == nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, std::errc::state_not_recoverable);
        }

        const auto &descriptor = selected->m_descriptor.m_value;
        const auto &owner = (*world_identity)->m_name.m_pack;
        const auto source = selected->m_descriptor.m_source.m_path.string();
        const auto world_name = (*world_identity)->m_name.string();
        const auto world_path = "worlds." + descriptor.m_semantic_key;
        AssembledWorldDefinition result;
        result.m_packs = catalog.packOwner();
        result.m_world = world;
        sim::definition::DefinitionBindings bindings;
        Array<SourceBinding> provenance;
        bindSource(provenance, world_path, "profile", selected->m_descriptor);

        const auto capture_texture = [&](std::string &name, const std::string_view declaring_pack) -> std::error_code {
            auto identity = catalog.packs().resolve(declaring_pack, name, image::kTextureAssetKind, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentAssembly, identity);

            const auto *texture = catalog.textures().find(image::TextureId{*(*identity)->m_key});
            if (texture == nullptr) {
                PPR_RETURN_ERROR_ON_FAIL(ContentAssembly, std::errc::state_not_recoverable);
            }

            if (std::ranges::none_of(
                    result.m_textures, [&](const auto &entry) { return *entry.m_id == *texture->m_id; })) {
                result.m_textures.push_back(*texture);
            }

            name = (*identity)->m_name.string();
            return {};
        };

        std::set<u64> material_targets;
        for (const auto &binding: descriptor.m_palette.m_materials.m_entries) {
            auto identity = catalog.packs().resolve(
                owner, binding.m_value.m_asset, sim::asset::kMaterialKind, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, identity);

            if (not material_targets.insert(*(*identity)->m_key).second) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentAssembly, asset::reject(
                                         diagnostics, source, "palette.materials." + binding.m_key + ".asset",
                                         "A Material asset cannot occupy more than one binding in the same World",
                                         config::EConfigErrc::duplicate_key));
            }

            const auto *material = findRecord(catalog.materials(), (*identity)->m_key);
            if (material == nullptr) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, std::errc::state_not_recoverable);
            }

            const auto &value = material->m_descriptor.m_value;
            const auto material_source = material->m_descriptor.m_source.m_path.string();
            auto element_identity = catalog.packs().resolve(
                (*identity)->m_name.m_pack, value.m_element, sim::asset::kElementKind, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, element_identity);

            const auto *element = catalog.elements().find(sim::asset::ElementId{*(*element_identity)->m_key});
            if (element == nullptr) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, std::errc::state_not_recoverable);
            }

            auto physical = sim::asset::composeMaterial(
                value, element->m_descriptor.m_value, diagnostics, material_source);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, physical);

            auto appearance = value.m_appearance;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAssembly,
                validateMaterialVisual(
                    appearance, value.m_representation == sim::definition::EMaterialRepresentation::terrain,
                    material_source, diagnostics));

            if (appearance.m_albedo_texture) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentAssembly, capture_texture(*appearance.m_albedo_texture, (*identity)->m_name.m_pack));
            }

            if (appearance.m_normal_texture) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentAssembly, capture_texture(*appearance.m_normal_texture, (*identity)->m_name.m_pack));
            }

            result.m_definition.m_materials.m_entries.push_back({binding.m_key, std::move(*physical)});
            result.m_definition.m_appearance.m_materials.m_entries.push_back({binding.m_key, std::move(appearance)});
            bindings.m_materials.push_back({binding.m_key, static_cast<u16>(binding.m_value.m_id)});
            bindSource(provenance, "materials." + binding.m_key, "", material->m_descriptor);
            bindSource(provenance, "materials." + binding.m_key + ".density", "density", element->m_descriptor);
            bindSource(provenance, "materials." + binding.m_key + ".phase", "phase", element->m_descriptor);
        }

        const auto collect = [&](const auto &palette, const auto &records, auto &definitions, auto &numeric_bindings,
                                 const std::string_view kind, const std::string_view category) -> std::error_code {
            std::set<u64> targets;
            for (const auto &binding: palette.m_entries) {
                auto identity = catalog.packs().resolve(owner, binding.m_value.m_asset, kind, diagnostics);
                PPR_RETURN_ERROR_ON_FAIL(ContentAssembly, identity);

                if (not targets.insert(*(*identity)->m_key).second) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ContentAssembly,
                        asset::reject(
                            diagnostics, source, "palette." + std::string{category} + "." + binding.m_key + ".asset",
                            "An asset cannot occupy more than one binding of this kind in the same World",
                            config::EConfigErrc::duplicate_key));
                }

                const auto *record = findRecord(records, (*identity)->m_key);
                if (record == nullptr) {
                    PPR_RETURN_ERROR_ON_FAIL(ContentAssembly, std::errc::state_not_recoverable);
                }

                definitions.m_entries.push_back({binding.m_key, record->m_descriptor.m_value.m_definition});
                numeric_bindings.push_back({binding.m_key, binding.m_value.m_index});
                if constexpr (requires { record->m_descriptor.m_value.m_appearance.m_preview_color_rgba; }) {
                    const auto &appearance = record->m_descriptor.m_value.m_appearance;
                    auto color = decodeAppearanceColor(
                        appearance.m_preview_color_rgba, record->m_descriptor.m_source.m_path.string(),
                        "appearance.preview_color_rgba", diagnostics);
                    PPR_RETURN_ERROR_ON_FAIL(ContentAssembly, color);

                    result.m_definition.m_appearance.m_biomes.m_entries.push_back({binding.m_key, appearance});
                } else if constexpr (requires { record->m_descriptor.m_value.m_appearance; }) {
                    result.m_definition.m_appearance.m_features.m_entries.push_back(
                        {binding.m_key, record->m_descriptor.m_value.m_appearance});
                }

                bindSource(provenance, std::string{category} + "." + binding.m_key, "definition", record->m_descriptor);
            }

            return {};
        };

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAssembly, collect(
                                 descriptor.m_palette.m_fields, catalog.fields(), result.m_definition.m_fields,
                                 bindings.m_fields, sim::asset::kFieldKind, "fields"));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAssembly, collect(
                                 descriptor.m_palette.m_biomes, catalog.biomes(), result.m_definition.m_biomes,
                                 bindings.m_biomes, sim::asset::kBiomeKind, "biomes"));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAssembly, collect(
                                 descriptor.m_palette.m_features, catalog.features(), result.m_definition.m_features,
                                 bindings.m_features, sim::asset::kFeatureKind, "features"));

        auto profile = descriptor.m_profile;
        std::set<u64> rule_targets;
        for (const auto &binding: descriptor.m_palette.m_rules.m_entries) {
            auto identity = catalog.packs().resolve(owner, binding.m_value.m_asset, sim::asset::kRuleKind, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, identity);

            if (not rule_targets.insert(*(*identity)->m_key).second) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentAssembly, asset::reject(
                                         diagnostics, source, "palette.rules." + binding.m_key + ".asset",
                                         "A Rule asset cannot occupy more than one binding in the same World",
                                         config::EConfigErrc::duplicate_key));
            }

            const auto *rule = findRecord(catalog.rules(), (*identity)->m_key);
            if (rule == nullptr) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, std::errc::state_not_recoverable);
            }

            profile.m_rules.m_entries.push_back({binding.m_key, rule->m_descriptor.m_value.m_definition});
            bindSource(provenance, world_path + ".rules." + binding.m_key, "definition", rule->m_descriptor);
        }

        result.m_definition.m_worlds.m_entries.push_back({descriptor.m_semantic_key, std::move(profile)});

        auto background_identity = catalog.packs().resolve(
            owner, descriptor.m_appearance.m_background_stack, kBackgroundAssetKind, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, background_identity);

        const auto *background = findRecord(catalog.backgrounds(), (*background_identity)->m_key);
        if (background == nullptr) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, std::errc::state_not_recoverable);
        }

        auto layers = background->m_descriptor.m_value.m_definition;
        std::ranges::sort(layers.m_layers.m_entries, {}, [](const auto &layer) { return layer.m_value.m_order; });

        for (std::size_t index = 0u; index < layers.m_layers.m_entries.size(); ++index) {
            auto &entry = layers.m_layers.m_entries[index];
            const auto path = "definition.layers." + entry.m_key;
            const auto location = background->m_descriptor.m_source.m_path.string();
            auto color = decodeAppearanceColor(entry.m_value.m_color_rgba, location, path + ".color_rgba", diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, color);

            if (entry.m_value.m_order != index or (index == 0u and (*color & 255u) != 255u)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentAssembly,
                    asset::reject(
                        diagnostics, location, "definition.layers",
                        "Layer orders must be unique zero through three; the farthest layer tint must be opaque"));
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAssembly, capture_texture(entry.m_value.m_texture, (*background_identity)->m_name.m_pack));
        }

        auto world_appearance = descriptor.m_appearance;
        world_appearance.m_background_stack = "background";
        result.m_definition.m_appearance.m_worlds.m_entries.push_back(
            {descriptor.m_semantic_key, std::move(world_appearance)});
        result.m_definition.m_appearance.m_backgrounds.m_entries.push_back({"background", std::move(layers)});

        bindSource(provenance, "bindings.materials", "palette.materials", selected->m_descriptor);
        bindSource(provenance, "bindings.fields", "palette.fields", selected->m_descriptor);
        bindSource(provenance, "bindings.biomes", "palette.biomes", selected->m_descriptor);
        bindSource(provenance, "bindings.features", "palette.features", selected->m_descriptor);

        config::Diagnostics local{128u};
        auto compiled = sim::definition::compileRecipe(
            result.m_definition, {descriptor.m_semantic_key}, parameters, bindings, local);
        appendAssemblyDiagnostics(diagnostics, local, provenance, world_name);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, compiled);

        result.m_compiled = std::move(*compiled);

        sim::definition::SemanticFingerprint numeric{result.m_definition};
        numeric.string("ppr.worldgen.numeric-world.v1");
        numeric.integer(sim::kGeneratorAlgorithmVersion);
        numeric.integer(sim::definition::definitionFingerprint(result.m_definition, bindings));
        numeric.value(parameters.m_cell_size_m);
        numeric.value(parameters.m_reference_thickness_m);
        numeric.value(parameters.m_actor_half_width_m);
        numeric.value(parameters.m_actor_half_height_m);
        result.m_numeric_fingerprint = numeric.hash();

        std::ranges::sort(result.m_textures, {}, [](const auto &texture) { return *texture.m_id; });

        sim::definition::SemanticFingerprint presentation{result.m_definition};
        presentation.string("ppr.worldgen.presentation-descriptors.v1");
        presentation.value(result.m_definition.m_appearance);
        for (const auto &texture: result.m_textures) {
            presentation.integer(*texture.m_id);
            presentation.integer(image::kTextureProcessorVersion);
            presentation.value(texture.m_descriptor.m_value);
        }

        result.m_presentation_descriptor_fingerprint = presentation.hash();
        return result;
    }

    // ------------------------------------------------------------------
    // selected runtime views using the actual compiled recipe
    // ------------------------------------------------------------------

    Expected<sim::asset::WorldId> resolveContentWorld(
        const ContentDescriptorCatalog &catalog, const std::string_view name, config::Diagnostics &diagnostics) {
        const auto packs = catalog.packs().packs();
        if (packs.empty() or (name.find(':') == std::string_view::npos and packs.size() != 1u)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAssembly,
                asset::reject(
                    diagnostics, "world selection", "world",
                    "Select a qualified World name (pack.namespace:local_key) when mounting multiple packs"));
        }

        auto qualified = asset::qualifyName(name, packs.front().m_manifest.m_value.m_pack_id);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, qualified);
        auto world = catalog.packs().resolve(
            qualified->m_pack, qualified->string(), sim::asset::kWorldKind, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, world);

        return sim::asset::WorldId{*(*world)->m_key};
    }

    Expected<ContentCatalog> selectWorldCatalog(
        std::shared_ptr<const ContentDescriptorCatalog> catalog, const sim::asset::WorldId world,
        const sim::definition::GenerationCompileParameters parameters, config::Diagnostics &diagnostics) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAssembly, not catalog ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});

        auto assembled = assembleWorld(*catalog, world, parameters, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, assembled);
        auto identity = catalog->packs().resolve(asset::AssetKey{*world}, sim::asset::kWorldKind, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, identity);

        // Assembly already validated imported domains and world-context edges.
        // Projection consumes the same recipe; it never serializes or recompiles.
        const config::Document provenance;
        auto selected = ContentCatalog::project(
            std::move(assembled->m_definition), provenance, nullptr, std::addressof(assembled->m_compiled),
            diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, selected);

        selected->m_descriptors = std::move(catalog);
        selected->m_selected_recipe.emplace(std::move(assembled->m_compiled));
        selected->m_texture_definitions = std::move(assembled->m_textures);
        selected->m_world = world;
        selected->m_pack_id = (*identity)->m_name.m_pack;
        selected->m_content_fingerprint = assembled->m_numeric_fingerprint;
        selected->m_presentation_descriptor_fingerprint = assembled->m_presentation_descriptor_fingerprint;
        selected->m_worlds.front().m_key = (*identity)->m_name.string();
        return selected;
    }

    const sim::definition::CompiledRecipe *ContentCatalog::selectedRecipe() const noexcept {
        return m_selected_recipe ? std::addressof(*m_selected_recipe) : nullptr;
    }

    sim::asset::WorldId ContentCatalog::worldId() const noexcept {
        return m_world;
    }

    std::span<const image::TextureDefinition> ContentCatalog::textureDefinitions() const noexcept {
        return m_texture_definitions;
    }

    Expected<asset::QualifiedName> ContentCatalog::materialAssetName(
        const u16 numeric_id, config::Diagnostics &diagnostics) const {
        const auto *const material = findMaterial(numeric_id);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAssembly,
            numeric_id == 0u or not m_descriptors or not m_selected_recipe or material == nullptr
                ? asset::reject(diagnostics, "selected World", "materials", "No selected Material binding for this ID")
                : std::error_code{});

        const auto world = std::ranges::find(m_descriptors->worlds(), m_world, &WorldAsset::m_id);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAssembly, world == m_descriptors->worlds().end()
                                 ? std::make_error_code(std::errc::state_not_recoverable)
                                 : std::error_code{});
        auto owner = m_descriptors->packs().resolve(asset::AssetKey{*m_world}, sim::asset::kWorldKind, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, owner);

        const auto &palette = world->m_descriptor.m_value.m_palette.m_materials.m_entries;
        const auto binding = std::ranges::find_if(palette, [&](const auto &entry) {
            return entry.m_key == material->m_key and entry.m_value.m_id == numeric_id;
        });
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAssembly,
            binding == palette.end() ? std::make_error_code(std::errc::state_not_recoverable) : std::error_code{});
        auto linked = m_descriptors->packs().resolve(
            (*owner)->m_name.m_pack, binding->m_value.m_asset, sim::asset::kMaterialKind, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAssembly, linked);

        return (*linked)->m_name;
    }

    const std::shared_ptr<const ContentDescriptorCatalog> &ContentCatalog::descriptorOwner() const noexcept {
        return m_descriptors;
    }

    u64 ContentCatalog::presentationDescriptorFingerprint() const noexcept {
        return m_presentation_descriptor_fingerprint;
    }
}
