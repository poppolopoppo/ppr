module;

export module engine.image:preparation;

import :types;
import :texture;
import engine.asset;
import engine.core;
import std;

export namespace pP::image {
    // ------------------------------------------------------------------
    // explicit texture request policy
    // ------------------------------------------------------------------

    struct TexturePreparationLimits {
        static constexpr config::Range<u32> count_domain{1u, 256u};
        static constexpr config::Range<u32> dimension_domain{1u, 2048u};
        static constexpr config::Range<u64> image_input_domain{1u, 16u * 1024u * 1024u};
        static constexpr config::Range<u64> total_input_domain{1u, 64u * 1024u * 1024u};
        static constexpr config::Range<u64> total_output_domain{1u, 128u * 1024u * 1024u};

        u64 m_max_image_input_bytes{image_input_domain.m_max};
        u64 m_max_input_bytes{total_input_domain.m_max};
        u64 m_max_output_bytes{total_output_domain.m_max};
        u32 m_max_count{count_domain.m_max};
        u32 m_max_dimension{dimension_domain.m_max};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &TexturePreparationLimits::m_max_image_input_bytes, asset::metadata("max_image_input_bytes"),
                    image_input_domain),
                config::field(
                    &TexturePreparationLimits::m_max_input_bytes, asset::metadata("max_input_bytes"),
                    total_input_domain),
                config::field(
                    &TexturePreparationLimits::m_max_output_bytes, asset::metadata("max_output_bytes"),
                    total_output_domain),
                config::field(&TexturePreparationLimits::m_max_count, asset::metadata("max_count"), count_domain),
                config::field(
                    &TexturePreparationLimits::m_max_dimension, asset::metadata("max_dimension"), dimension_domain)};
        }
    };

    // ------------------------------------------------------------------
    // independently owned CPU image results
    // ------------------------------------------------------------------

    struct PreparedTexture {
        ImageAsset m_image{};
        asset::QualifiedName m_name{};
        TextureId m_id{};
        u64 m_source_fingerprint{};
        u64 m_settings_fingerprint{};
        u64 m_source_bytes{};
        u32 m_processor_version{kTextureProcessorVersion};
    };

    struct TexturePreparationResult {
        Array<PreparedTexture> m_textures{};
        u64 m_input_bytes{}, m_output_bytes{};
    };

    [[nodiscard]] u64 textureSettingsFingerprint(const TextureDescriptor &descriptor) noexcept;

    // ------------------------------------------------------------------
    // image processor for the shared asset request
    // ------------------------------------------------------------------

    /// Binary files must remain stable during the request; updates replace whole
    /// files after affected requests drain. Mappings are not immutable snapshots.
    class TextureProcessor {
        Array<TextureDefinition> m_definitions{};
        Array<u8> m_budget_decisions{};
        TexturePreparationLimits m_limits{};
        u64 m_input_bytes{}, m_output_bytes{};
        std::size_t m_budget_next{};

    public:
        struct Item;
        using Result = TexturePreparationResult;
        static constexpr std::size_t maximum_active = 2u;
        static constexpr u32 processing_revision = kTextureProcessorVersion;

        TextureProcessor(Array<TextureDefinition> definitions, TexturePreparationLimits limits);

        [[nodiscard]] Expected<Array<std::shared_ptr<Item>>> prepare(
            const asset::PackSet &packs, const IContext &context, config::Diagnostics &diagnostics);

        [[nodiscard]] static std::error_code process(
            Item &item, const asset::PackSet &packs, const IContext &context, config::Diagnostics &diagnostics);

        [[nodiscard]] Expected<asset::EPreparationStep> advance(
            std::size_t index, Item &item, bool succeeded, config::Diagnostics &diagnostics);

        [[nodiscard]] Expected<Result> finish(
            std::span<const std::shared_ptr<Item>> items, config::Diagnostics &diagnostics);
    };

    using TexturePreparation = asset::PreparationRequest<TextureProcessor>;

#if 0
    /// Thin factory; lifecycle, generation and owning result stamps are shared
    /// engine.asset behavior. No worker step uploads GPU data.
    [[nodiscard]] Expected<TexturePreparation> prepareTextures(
        std::shared_ptr<const asset::PackSet> packs, Array<TextureDefinition> textures,
        asset::RequestGeneration generation, SharedContext context, config::Diagnostics &diagnostics,
        const TexturePreparationLimits &limits = {});
#else
    /// Thin factory; lifecycle, generation and owning result stamps are shared
    /// engine.asset behavior. No worker step uploads GPU data.
    [[nodiscard]] inline Expected<TexturePreparation> prepareTextures(
        const std::shared_ptr<const asset::PackSet> &packs, const Array<TextureDefinition> &textures,
        const asset::RequestGeneration &generation, const SharedContext &context, config::Diagnostics &diagnostics,
        const TexturePreparationLimits &limits = {}) {
        return TexturePreparation::start(
            parallel::backgroundPool(), packs, TextureProcessor{textures, limits}, generation, std::move(context),
            diagnostics);
    }
#endif
}
