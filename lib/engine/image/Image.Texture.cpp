module;
#include "pP/Macros.h"

module engine.image;

import :texture;
import :types;
import engine.asset;
import engine.core;
import std;

namespace pP::image {
    using pP::make_error_code;

    // ------------------------------------------------------------------
    // owned definition-only texture catalog
    // ------------------------------------------------------------------

    Expected<TextureCatalog> TextureCatalog::importDefinitions(
        const asset::PackSet &packs, config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Image.Texture.ImportDefinitions"};
        TextureCatalog result;

        for (const auto &entry: packs.assets()) {
            if (entry.m_kind != kTextureAssetKind) {
                continue;
            }

            auto imported = asset::importDescriptor<TextureDescriptor>(
                packs, entry.m_key, kTextureAssetKind, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Image, imported);

            const auto &descriptor = imported->m_value;
            if (descriptor.m_interpretation == ETextureInterpretation::normal and descriptor.m_preserve_coverage) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Image, asset::reject(
                               diagnostics, imported->m_source.m_path.string(), "preserve_coverage",
                               "Normal textures cannot preserve alpha coverage"));
            }

            auto source = packs.containedPath(entry.m_name.m_pack, descriptor.m_source, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Image, source);

            result.m_definitions.push_back({std::move(*imported), std::move(*source), TextureId{*entry.m_key}});
        }

        return result;
    }

    std::span<const TextureDefinition> TextureCatalog::definitions() const noexcept {
        return m_definitions;
    }

    const TextureDefinition *TextureCatalog::find(const TextureId id) const noexcept {
        const auto found = std::ranges::find_if(m_definitions, [&](const auto &entry) { return *entry.m_id == *id; });
        return found == m_definitions.end() ? nullptr : std::addressof(*found);
    }
}
