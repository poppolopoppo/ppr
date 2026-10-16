module;
#include "pP/Macros.h"

export module engine.image:texture;

import :mips;
import engine.asset;
import engine.core;
import std;

export namespace pP::image {
    // ------------------------------------------------------------------
    // named texture descriptors
    // ------------------------------------------------------------------

    struct TextureIdTag;
    using TextureId = Numeric<u64, TextureIdTag>;

    inline constexpr std::string_view kTextureAssetKind = "image.texture";
    inline constexpr u32 kTextureProcessorVersion = 1u;

    enum class ETextureInterpretation : u8 {
        color,
        data,
        normal
    };

    enum class ETextureMipPolicy : u8 {
        mitchell
    };

    struct TextureDescriptor {
        std::string m_source{};
        u32 m_schema_version{};
        float m_alpha_cutoff{0.5f};
        ETextureInterpretation m_interpretation{ETextureInterpretation::color};
        ETextureMipPolicy m_mip_policy{ETextureMipPolicy::mitchell};
        bool m_full_chain{true};
        bool m_high_quality{};
        bool m_preserve_coverage{};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;
            return std::tuple{
                field(
                    &TextureDescriptor::m_schema_version, asset::metadata("schema_version"), Range{1u, 1u},
                    EPresence::required),
                field(
                    &TextureDescriptor::m_source, asset::metadata("source"), StringDomain{1024u, false},
                    EPresence::required),
                field(
                    &TextureDescriptor::m_interpretation, asset::metadata("interpretation"),
                    EnumDomain<ETextureInterpretation, 3>{
                        {EnumEntry{ETextureInterpretation::color, "color"},
                            EnumEntry{ETextureInterpretation::data, "data"},
                            EnumEntry{ETextureInterpretation::normal, "normal"}}},
                    EPresence::required),
                field(
                    &TextureDescriptor::m_mip_policy, asset::metadata("mip_policy"),
                    EnumDomain<ETextureMipPolicy, 1>{{EnumEntry{ETextureMipPolicy::mitchell, "mitchell"}}},
                    EPresence::required),
                field(&TextureDescriptor::m_full_chain, asset::metadata("full_chain"), boolean_domain),
                field(&TextureDescriptor::m_high_quality, asset::metadata("high_quality"), boolean_domain),
                field(&TextureDescriptor::m_preserve_coverage, asset::metadata("preserve_coverage"), boolean_domain),
                field(
                    &TextureDescriptor::m_alpha_cutoff, asset::metadata("alpha_cutoff"),
                    MipGenDesc::alpha_cutoff_domain)};
        }
    };

    struct TextureDefinition {
        asset::Imported<TextureDescriptor> m_descriptor{};
        std::filesystem::path m_source_path{};
        TextureId m_id{};
    };

    // ------------------------------------------------------------------
    // owned definition-only texture catalog
    // ------------------------------------------------------------------

    class TextureCatalog {
        Array<TextureDefinition> m_definitions{};

        TextureCatalog() = default;

    public:
        /// Imports descriptors and checks source containment without opening binary sources.
        [[nodiscard]] static Expected<TextureCatalog> importDefinitions(
            const asset::PackSet &packs, config::Diagnostics &diagnostics);

        /// Observers remain valid only while this immutable catalog stays at its current address.
        [[nodiscard]] std::span<const TextureDefinition> definitions() const noexcept;
        [[nodiscard]] const TextureDefinition *find(TextureId id) const noexcept;
    };
}
