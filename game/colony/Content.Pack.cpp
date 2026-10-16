module;
#include "pP/Macros.h"
module game.colony.content;
import engine.core;
import engine.sim;
import std;

namespace pP::colony {
    // ------------------------------------------------------------------
    // selected numeric catalog identity and views
    // ------------------------------------------------------------------

    u64 ContentCatalog::materialFingerprint() const {
        sim::definition::SemanticFingerprint hash{m_definition};
        hash.string("ppr.materials.physical-bindings.v1");
        hash.integer(m_materials.size());

        for (const auto &material: m_materials) {
            hash.string(material.m_key);
            hash.value(material.m_numeric_id);
            hash.value(material.m_phase);
            hash.value(material.m_representation);
            hash.value(material.m_reference_density_kg_m3);
            hash.value(material.m_diggable);
        }

        return hash.hash();
    }

    std::string_view ContentCatalog::packId() const noexcept {
        return m_pack_id;
    }

    std::span<const FieldDefinition> ContentCatalog::fields() const noexcept {
        return m_fields;
    }

    std::span<const BiomeDefinition> ContentCatalog::biomes() const noexcept {
        return m_biomes;
    }

    std::span<const FeatureDefinition> ContentCatalog::features() const noexcept {
        return m_features;
    }

    std::span<const WorldProfileDefinition> ContentCatalog::worlds() const noexcept {
        return m_worlds;
    }

    std::span<const BackgroundStackDefinition> ContentCatalog::backgrounds() const noexcept {
        return m_backgrounds;
    }

    std::optional<u64> ContentCatalog::contentFingerprint() const noexcept {
        return m_content_fingerprint;
    }

    const WorldProfileDefinition *ContentCatalog::findWorld(const std::string_view key) const noexcept {
        const auto found = std::ranges::lower_bound(m_worlds, key, {}, &WorldProfileDefinition::m_key);
        return found == m_worlds.end() or found->m_key != key ? nullptr : std::addressof(*found);
    }
}
