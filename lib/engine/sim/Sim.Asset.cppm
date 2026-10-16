module;
#include "pP/Macros.h"

export module engine.sim:asset;

import :worldgen.definition;
import :element;
import engine.asset;
import engine.core;
import std;

export namespace pP::sim::asset {
    // ------------------------------------------------------------------
    // simulation asset identities
    // ------------------------------------------------------------------

    PPR_DECLARE_LOG_CATEGORY(SimAssets);

    struct ElementIdTag;
    struct MaterialIdTag;
    struct WorldIdTag;
    struct FieldIdTag;
    struct BiomeIdTag;
    struct FeatureIdTag;
    struct RuleIdTag;
    using ElementId = Numeric<u64, ElementIdTag>;
    using MaterialId = Numeric<u64, MaterialIdTag>;
    using WorldId = Numeric<u64, WorldIdTag>;
    using FieldId = Numeric<u64, FieldIdTag>;
    using BiomeId = Numeric<u64, BiomeIdTag>;
    using FeatureId = Numeric<u64, FeatureIdTag>;
    using RuleId = Numeric<u64, RuleIdTag>;

    inline constexpr std::string_view kElementKind = "sim.element";
    inline constexpr std::string_view kMaterialKind = "sim.material";
    inline constexpr std::string_view kFieldKind = "sim.field";
    inline constexpr std::string_view kBiomeKind = "sim.biome";
    inline constexpr std::string_view kFeatureKind = "sim.feature";
    inline constexpr std::string_view kRuleKind = "sim.rule";
    inline constexpr std::string_view kWorldKind = "sim.world";

    // ------------------------------------------------------------------
    // physical elements and material composition
    // ------------------------------------------------------------------

    struct ElementDescriptor : Element {
        u32 m_schema_version{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple_cat(
                Element::describe(), std::tuple{config::field(
                                         &ElementDescriptor::m_schema_version, pP::asset::metadata("schema_version"),
                                         config::Range{1u, 1u}, config::EPresence::required)});
        }
    };

    struct MaterialDescriptor {
        std::string m_element{};
        u32 m_schema_version{};
        std::optional<bool> m_diggable{};
        definition::EMaterialRepresentation m_representation{definition::EMaterialRepresentation::terrain};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;
            return std::tuple{
                field(
                    &MaterialDescriptor::m_schema_version, pP::asset::metadata("schema_version"), Range{1u, 1u},
                    EPresence::required),
                field(
                    &MaterialDescriptor::m_element, pP::asset::metadata("element"),
                    pP::asset::QualifiedName::reference_domain, EPresence::required),
                field(
                    &MaterialDescriptor::m_representation, pP::asset::metadata("representation"),
                    definition::Material::representation_domain, EPresence::required),
                field(
                    &MaterialDescriptor::m_diggable, pP::asset::metadata("diggable"), boolean_domain,
                    EPresence::optional)};
        }
    };

    /// Assemble the existing numeric compiler input without changing Material cell identity.
    [[nodiscard]] Expected<definition::Material> composeMaterial(
        const MaterialDescriptor &material, const Element &element, config::Diagnostics &diagnostics,
        std::string_view source);

    // ------------------------------------------------------------------
    // versioned existing simulation schemas
    // ------------------------------------------------------------------

    template<typename Definition>
    struct Descriptor {
        Definition m_definition{};
        u32 m_schema_version{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &Descriptor::m_schema_version, pP::asset::metadata("schema_version"), config::Range{1u, 1u},
                    config::EPresence::required),
                config::group(
                    &Descriptor::m_definition, pP::asset::metadata("definition"), config::EPresence::required)};
        }
    };

    using FieldDescriptor = Descriptor<NoiseFieldDesc>;
    using BiomeDescriptor = Descriptor<definition::Biome>;
    using FeatureDescriptor = Descriptor<definition::FeatureDefinition>;
    using RuleDescriptor = Descriptor<definition::RuleDefinition>;

    // ------------------------------------------------------------------
    // explicit world palettes and world-context aliases
    // ------------------------------------------------------------------

    struct MaterialAssetBinding {
        std::string m_asset{};
        u32 m_id{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &MaterialAssetBinding::m_asset, pP::asset::metadata("asset"),
                    pP::asset::QualifiedName::reference_domain, config::EPresence::required),
                config::field(
                    &MaterialAssetBinding::m_id, pP::asset::metadata("id"), config::Range{1u, 65534u},
                    config::EPresence::required)};
        }
    };

    struct IndexedAssetBinding {
        std::string m_asset{};
        u32 m_index{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &IndexedAssetBinding::m_asset, pP::asset::metadata("asset"),
                    pP::asset::QualifiedName::reference_domain, config::EPresence::required),
                config::field(
                    &IndexedAssetBinding::m_index, pP::asset::metadata("index"),
                    config::Range{0u, (std::numeric_limits<u32>::max)()}, config::EPresence::required)};
        }
    };

    struct RuleAssetBinding {
        std::string m_asset{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{config::field(
                &RuleAssetBinding::m_asset, pP::asset::metadata("asset"), pP::asset::QualifiedName::reference_domain,
                config::EPresence::required)};
        }
    };

    struct WorldPalette {
        config::KeyedCollection<MaterialAssetBinding> m_materials{};
        config::KeyedCollection<IndexedAssetBinding> m_fields{}, m_biomes{}, m_features{};
        config::KeyedCollection<RuleAssetBinding> m_rules{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                config::collection(
                    &WorldPalette::m_materials, pP::asset::metadata("materials"),
                    definition::Definition::materials_domain),
                config::collection(
                    &WorldPalette::m_fields, pP::asset::metadata("fields"), definition::Definition::fields_domain),
                config::collection(
                    &WorldPalette::m_biomes, pP::asset::metadata("biomes"), definition::Definition::biomes_domain),
                config::collection(
                    &WorldPalette::m_features, pP::asset::metadata("features"),
                    definition::Definition::features_domain),
                config::collection(
                    &WorldPalette::m_rules, pP::asset::metadata("rules"), definition::ProfileDefinition::rules_domain)};
        }
    };

    /// Palette entry keys and all reused numeric payload references are semantic
    /// world-local aliases. Only binding.asset values are pack asset references.
    /// Marker/rule references keep their existing feature/profile owner scope.
    struct WorldDescriptor {
        definition::ProfileDefinition m_profile{};
        WorldPalette m_palette{};
        std::string m_semantic_key{};
        u32 m_schema_version{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &WorldDescriptor::m_schema_version, pP::asset::metadata("schema_version"), config::Range{1u, 1u},
                    config::EPresence::required),
                config::field(
                    &WorldDescriptor::m_semantic_key, pP::asset::metadata("semantic_key"),
                    config::StringDomain{128u, false}, config::EPresence::required),
                config::group(&WorldDescriptor::m_palette, pP::asset::metadata("palette"), config::EPresence::required),
                config::group(
                    &WorldDescriptor::m_profile, pP::asset::metadata("profile"), config::EPresence::required)};
        }
    };

    // ------------------------------------------------------------------
    // owned physical element catalog
    // ------------------------------------------------------------------

    struct ElementDefinition {
        pP::asset::Imported<ElementDescriptor> m_descriptor{};
        ElementId m_id{};
    };

    class ElementCatalog {
        Array<ElementDefinition> m_definitions{};

        ElementCatalog() = default;

    public:
        [[nodiscard]] static Expected<ElementCatalog> importDefinitions(
            const pP::asset::PackSet &packs, config::Diagnostics &diagnostics);

        [[nodiscard]] std::span<const ElementDefinition> definitions() const noexcept;
        [[nodiscard]] const ElementDefinition *find(ElementId id) const noexcept;
    };
}
