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

    PPR_DEFINE_LOG_CATEGORY(ContentDescriptors, debug, none)

    // ------------------------------------------------------------------
    // contextual descriptor references
    // ------------------------------------------------------------------

    namespace {
        Expected<const asset::IndexedAsset *> reference(
            const asset::PackSet &packs, const std::string_view declaring_pack, const std::string_view name,
            const std::string_view kind, const std::string_view source, const std::string_view property,
            config::Diagnostics &diagnostics) {
            config::Diagnostics local{1u};
            auto result = packs.resolve(declaring_pack, name, kind, local);
            if (not result) {
                // Translate lookup provenance to the descriptor property that owns the reference.
                diagnostics.add(
                    std::string{source}, std::string{property},
                    local.entries().empty() ? "Cannot resolve asset reference" : local.entries().front().m_message,
                    result.error());
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, result);
            }

            return result;
        }

        template<typename Collection>
        std::error_code validatePalette(
            const asset::PackSet &packs, const Collection &collection, const std::string_view declaring_pack,
            const std::string_view kind, const std::string_view source, const std::string_view path,
            const u32 first_index, config::Diagnostics &diagnostics) {
            Array<u32> positions;
            positions.reserve(collection.m_entries.size());

            for (const auto &entry: collection.m_entries) {
                const auto property = std::string{path} + "." + entry.m_key;
                auto linked = reference(
                    packs, declaring_pack, entry.m_value.m_asset, kind, source, property + ".asset", diagnostics);
                PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, linked);

                if constexpr (requires { entry.m_value.m_id; }) {
                    positions.push_back(entry.m_value.m_id);
                } else {
                    positions.push_back(entry.m_value.m_index);
                }
            }

            std::ranges::sort(positions);

            for (std::size_t index = 0u; index < positions.size(); ++index) {
                if (positions[index] != first_index + index) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ContentDescriptors,
                        asset::reject(
                            diagnostics, source, path,
                            "World binding indices must be unique and contiguous in their numeric domain"));
                }
            }

            return {};
        }
    }

    // ------------------------------------------------------------------
    // complete owned typed descriptor import
    // ------------------------------------------------------------------

    ContentDescriptorCatalog::ContentDescriptorCatalog(
        std::shared_ptr<const asset::PackSet> packs, sim::asset::ElementCatalog elements,
        image::TextureCatalog textures)
        : m_packs{std::move(packs)}, m_elements{std::move(elements)}, m_textures{std::move(textures)} {
    }

    Expected<ContentDescriptorCatalog> ContentDescriptorCatalog::importDefinitions(
        std::shared_ptr<const asset::PackSet> packs, config::Diagnostics &diagnostics) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentDescriptors, not packs ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});

        const auto reader = [&](const asset::IndexedAsset &entry, config::Diagnostics &local) {
            return packs->readDescriptor(entry.m_key, entry.m_kind, local);
        };

        return importDefinitions(packs, reader, diagnostics);
    }

    Expected<ContentDescriptorCatalog> ContentDescriptorCatalog::importDefinitions(
        std::shared_ptr<const asset::PackSet> packs, const asset::DescriptorReader reader,
        config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Content.Descriptors.ImportDefinitions"};
        if (not packs) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, std::errc::invalid_argument);
        }

        auto elements = sim::asset::ElementCatalog::importDefinitions(*packs, reader, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, elements);

        auto textures = image::TextureCatalog::importDefinitions(*packs, reader, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, textures);

        ContentDescriptorCatalog result{std::move(packs), std::move(*elements), std::move(*textures)};

        for (const auto &entry: result.m_packs->assets()) {
            const auto append = [&]<typename Descriptor, typename Id>(
                                    Array<ContentDescriptor<Descriptor, Id>> &destination) {
                auto imported = asset::importDescriptor<Descriptor>(
                    *result.m_packs, entry.m_key, entry.m_kind, reader, diagnostics);
                PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, imported);

                destination.push_back({std::move(*imported), Id{*entry.m_key}});
                return std::error_code{};
            };

            if (entry.m_kind == sim::asset::kElementKind or entry.m_kind == image::kTextureAssetKind) {
                continue;
            }

            if (entry.m_kind == sim::asset::kMaterialKind) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, append(result.m_materials));
            } else if (entry.m_kind == sim::asset::kFieldKind) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, append(result.m_fields));
            } else if (entry.m_kind == sim::asset::kBiomeKind) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, append(result.m_biomes));
            } else if (entry.m_kind == sim::asset::kFeatureKind) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, append(result.m_features));
            } else if (entry.m_kind == sim::asset::kRuleKind) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, append(result.m_rules));
            } else if (entry.m_kind == sim::asset::kWorldKind) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, append(result.m_worlds));
            } else if (entry.m_kind == kBackgroundAssetKind) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, append(result.m_backgrounds));
            } else {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentDescriptors, asset::reject(
                                            diagnostics, entry.m_descriptor.string(), "kind",
                                            "Unsupported content asset kind", config::EConfigErrc::unknown_enum_value));
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentDescriptors, result.validateLinks(diagnostics));
        return result;
    }

    // ------------------------------------------------------------------
    // cross-domain links and world palettes
    // ------------------------------------------------------------------

    std::error_code ContentDescriptorCatalog::validateLinks(config::Diagnostics &diagnostics) const {
        const auto texture = [&](const std::string_view declaring_pack, const std::string_view name, const bool normal,
                                 const std::string_view source, const std::string_view property) {
            auto linked = reference(
                *m_packs, declaring_pack, name, image::kTextureAssetKind, source, property, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, linked);

            const auto *definition = m_textures.find(image::TextureId{*(*linked)->m_key});
            if (definition == nullptr) {
                PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, std::errc::state_not_recoverable);
            }

            const bool is_normal = definition->m_descriptor.m_value.m_interpretation ==
                                   image::ETextureInterpretation::normal;
            if (normal != is_normal) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentDescriptors,
                    asset::reject(
                        diagnostics, source, property, "Texture interpretation does not match its appearance role",
                        config::EConfigErrc::type_mismatch));
            }

            return std::error_code{};
        };

        for (const auto &entry: m_materials) {
            auto identity = m_packs->resolve(asset::AssetKey{*entry.m_id}, sim::asset::kMaterialKind, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, identity);

            const auto &owner = (*identity)->m_name.m_pack;
            const auto &value = entry.m_descriptor.m_value;
            const auto source = entry.m_descriptor.m_source.m_path.string();
            auto linked = reference(
                *m_packs, owner, value.m_element, sim::asset::kElementKind, source, "element", diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, linked);

            const auto *element = m_elements.find(sim::asset::ElementId{*(*linked)->m_key});
            if (element == nullptr) {
                PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, std::errc::state_not_recoverable);
            }

            auto physical = sim::asset::composeMaterial(value, element->m_descriptor.m_value, diagnostics, source);
            PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, physical);

            if (value.m_appearance.m_albedo_texture) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentDescriptors,
                    texture(owner, *value.m_appearance.m_albedo_texture, false, source, "appearance.albedo_texture"));
            }

            if (value.m_appearance.m_normal_texture) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentDescriptors,
                    texture(owner, *value.m_appearance.m_normal_texture, true, source, "appearance.normal_texture"));
            }
        }

        for (const auto &entry: m_backgrounds) {
            auto identity = m_packs->resolve(asset::AssetKey{*entry.m_id}, kBackgroundAssetKind, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, identity);

            const auto source = entry.m_descriptor.m_source.m_path.string();
            for (const auto &layer: entry.m_descriptor.m_value.m_definition.m_layers.m_entries) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentDescriptors, texture(
                                            (*identity)->m_name.m_pack, layer.m_value.m_texture, false, source,
                                            "definition.layers." + layer.m_key + ".texture"));
            }
        }

        for (const auto &entry: m_worlds) {
            auto identity = m_packs->resolve(asset::AssetKey{*entry.m_id}, sim::asset::kWorldKind, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, identity);

            const auto &owner = (*identity)->m_name.m_pack;
            const auto &value = entry.m_descriptor.m_value;
            const auto &palette = value.m_palette;
            const auto source = entry.m_descriptor.m_source.m_path.string();
            if (not config::isValidKey(value.m_semantic_key)) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentDescriptors,
                    asset::reject(diagnostics, source, "semantic_key", "Invalid world semantic alias"));
            }

            if (not value.m_profile.m_rules.m_entries.empty()) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentDescriptors, asset::reject(
                                            diagnostics, source, "profile.rules",
                                            "World rules are named descriptors declared through palette.rules"));
            }

            PPR_RETURN_ERROR_ON_FAIL(
                ContentDescriptors, validatePalette(
                                        *m_packs, palette.m_materials, owner, sim::asset::kMaterialKind, source,
                                        "palette.materials", 1u, diagnostics));

            PPR_RETURN_ERROR_ON_FAIL(
                ContentDescriptors, validatePalette(
                                        *m_packs, palette.m_fields, owner, sim::asset::kFieldKind, source,
                                        "palette.fields", 0u, diagnostics));

            PPR_RETURN_ERROR_ON_FAIL(
                ContentDescriptors, validatePalette(
                                        *m_packs, palette.m_biomes, owner, sim::asset::kBiomeKind, source,
                                        "palette.biomes", 0u, diagnostics));

            PPR_RETURN_ERROR_ON_FAIL(
                ContentDescriptors, validatePalette(
                                        *m_packs, palette.m_features, owner, sim::asset::kFeatureKind, source,
                                        "palette.features", 0u, diagnostics));

            for (const auto &rule: palette.m_rules.m_entries) {
                auto linked = reference(
                    *m_packs, owner, rule.m_value.m_asset, sim::asset::kRuleKind, source,
                    "palette.rules." + rule.m_key + ".asset", diagnostics);
                PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, linked);
            }

            auto background = reference(
                *m_packs, owner, value.m_appearance.m_background_stack, kBackgroundAssetKind, source,
                "appearance.background_stack", diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentDescriptors, background);
        }

        return {};
    }

    // ------------------------------------------------------------------
    // immutable typed catalog views
    // ------------------------------------------------------------------

    const asset::PackSet &ContentDescriptorCatalog::packs() const noexcept {
        return *m_packs;
    }

    std::shared_ptr<const asset::PackSet> ContentDescriptorCatalog::packOwner() const noexcept {
        return m_packs;
    }

    const sim::asset::ElementCatalog &ContentDescriptorCatalog::elements() const noexcept {
        return m_elements;
    }

    const image::TextureCatalog &ContentDescriptorCatalog::textures() const noexcept {
        return m_textures;
    }

    std::span<const MaterialAsset> ContentDescriptorCatalog::materials() const noexcept {
        return m_materials;
    }

    std::span<const FieldAsset> ContentDescriptorCatalog::fields() const noexcept {
        return m_fields;
    }

    std::span<const BiomeAsset> ContentDescriptorCatalog::biomes() const noexcept {
        return m_biomes;
    }

    std::span<const FeatureAsset> ContentDescriptorCatalog::features() const noexcept {
        return m_features;
    }

    std::span<const RuleAsset> ContentDescriptorCatalog::rules() const noexcept {
        return m_rules;
    }

    std::span<const WorldAsset> ContentDescriptorCatalog::worlds() const noexcept {
        return m_worlds;
    }

    std::span<const BackgroundAsset> ContentDescriptorCatalog::backgrounds() const noexcept {
        return m_backgrounds;
    }
}
