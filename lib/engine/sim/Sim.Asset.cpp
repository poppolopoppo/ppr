module;
#include "pP/Macros.h"

module engine.sim;

import :asset;
import engine.asset;
import engine.core;
import std;

namespace pP::sim::asset {
    using pP::make_error_code;

    PPR_DEFINE_LOG_CATEGORY(SimAssets, debug, none);

    // ------------------------------------------------------------------
    // physical material assembly
    // ------------------------------------------------------------------

    Expected<definition::Material> composeMaterial(
        const MaterialDescriptor &material, const Element &element, config::Diagnostics &diagnostics,
        const std::string_view source) {
        if (Element::density_domain.validate(element.m_density) or
            Element::phase_domain.validate(element.m_phase) or
            definition::Material::representation_domain.validate(material.m_representation)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                SimAssets, pP::asset::reject(
                               diagnostics, source, "element", "Invalid physical Element or material representation",
                               config::EConfigErrc::out_of_range));
        }

        if (material.m_representation == definition::EMaterialRepresentation::terrain) {
            if (element.m_phase != EGenerationMatter::solid or not material.m_diggable.has_value()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    SimAssets, pP::asset::reject(
                                   diagnostics, source, "element",
                                   "Terrain material requires a solid Element and an explicit diggable flag"));
            }
        } else if (material.m_diggable.has_value()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                SimAssets,
                pP::asset::reject(diagnostics, source, "diggable", "Particle materials cannot author diggable"));
        }

        definition::Material result;
        result.m_phase = element.m_phase;
        result.m_density = element.m_density;
        result.m_representation = material.m_representation;
        result.m_diggable = material.m_diggable;
        return result;
    }

    // ------------------------------------------------------------------
    // owned physical element catalog
    // ------------------------------------------------------------------

    Expected<ElementCatalog> ElementCatalog::importDefinitions(
        const pP::asset::PackSet &packs, config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Sim.Asset.ImportElements"};
        ElementCatalog result;

        for (const auto &entry: packs.assets()) {
            if (entry.m_kind != kElementKind) {
                continue;
            }

            auto descriptor = pP::asset::importDescriptor<ElementDescriptor>(
                packs, entry.m_key, kElementKind, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(SimAssets, descriptor);

            result.m_definitions.push_back({std::move(*descriptor), ElementId{*entry.m_key}});
        }

        return result;
    }

    std::span<const ElementDefinition> ElementCatalog::definitions() const noexcept {
        return m_definitions;
    }

    const ElementDefinition *ElementCatalog::find(const ElementId id) const noexcept {
        const auto found = std::ranges::find_if(m_definitions, [&](const auto &entry) { return *entry.m_id == *id; });
        return found == m_definitions.end() ? nullptr : std::addressof(*found);
    }
}
