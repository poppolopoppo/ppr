module;
#include "pP/Macros.h"

module engine.image;

import :preparation;
import :decode;
import :mips;
import engine.asset;
import engine.core;
import std;

namespace pP::image {
    using pP::make_error_code;

    // ------------------------------------------------------------------
    // private image processing state
    // ------------------------------------------------------------------

    struct TextureProcessor::Item {
        enum class EPhase : u8 {
            base,
            base_ready,
            mips,
            finished
        };

        TextureDefinition m_definition{};
        PreparedTexture m_prepared{};
        mem::SharedBuffer m_mapping{};
        std::filesystem::path m_path{};
        std::filesystem::file_time_type m_write_time{};
        TexturePreparationLimits m_limits{};
        u64 m_expected_bytes{}, m_chain_bytes{};
        EPhase m_phase{EPhase::base};
    };

    // ------------------------------------------------------------------
    // canonical source and processing identities
    // ------------------------------------------------------------------

    namespace {
        using Item = TextureProcessor::Item;

        struct Fingerprint {
            u64 m_value{14695981039346656037ull};

            void integer(const u64 value, const u32 width) noexcept {
                for (u32 index = 0u; index < width; ++index) {
                    m_value = (m_value ^ static_cast<u8>(value >> (index * 8u))) * 1099511628211ull;
                }
            }

            void text(const std::string_view value) noexcept {
                integer(value.size(), 8u);

                for (const unsigned char byte: value) {
                    integer(byte, 1u);
                }
            }
        };

        [[nodiscard]] u64 sourceFingerprint(const mem::SharedBufferView bytes) noexcept {
            const hal::ProfileScope scope{"Image.Preparation.HashSource"};
            Fingerprint result;
            result.text("ppr.texture.source.v1");
            result.integer(bytes.size_bytes(), 8u);

            for (const auto byte: bytes) {
                result.integer(std::to_integer<u8>(byte), 1u);
            }

            return result.m_value;
        }

        template<typename Descriptor>
        [[nodiscard]] std::error_code validateValues(
            const Descriptor &descriptor, const std::string_view source, config::Diagnostics &diagnostics) {
            std::error_code first;
            config::visit(descriptor, [&](const auto &member, const auto &value) {
                if (const auto error = member.m_constraint.validate(value)) {
                    diagnostics.add(
                        std::string{source}, std::string{member.m_metadata.m_name},
                        "Value is outside its descriptor domain", error);
                    if (not first) {
                        first = error;
                    }
                }
            });

            PPR_RETURN_ERROR_ON_FAIL(Image, first);
            return {};
        }

        [[nodiscard]] std::error_code fail(
            Item &item, config::Diagnostics &diagnostics, const std::error_code error, const std::string_view message) {
            diagnostics.add(
                item.m_definition.m_descriptor.m_source.m_path.string(), "source",
                std::string{message} + ": " + item.m_path.string(), error);
            PPR_RETURN_ERROR_ON_FAIL(Image, error);
            return {};
        }

        [[nodiscard]] std::error_code checkSource(
            Item &item, const asset::PackSet &packs, config::Diagnostics &diagnostics) {
            auto contained = packs.containedPath(
                item.m_prepared.m_name.m_pack, item.m_definition.m_descriptor.m_value.m_source, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(Image, contained);

            std::error_code error;
            const bool regular = std::filesystem::is_regular_file(*contained, error);
            if (error) {
                PPR_RETURN_ERROR_ON_FAIL(Image, fail(item, diagnostics, error, "Cannot inspect texture source type"));
            }

            if (not regular) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Image, fail(
                               item, diagnostics, std::make_error_code(std::errc::invalid_argument),
                               "Texture source is no longer a regular file"));
            }

            const auto size = std::filesystem::file_size(*contained, error);
            if (error) {
                PPR_RETURN_ERROR_ON_FAIL(Image, fail(item, diagnostics, error, "Cannot inspect texture source"));
            }

            const auto time = std::filesystem::last_write_time(*contained, error);
            if (error) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Image, fail(item, diagnostics, error, "Cannot inspect texture source revision"));
            }

            if (*contained != item.m_path or size != item.m_expected_bytes or time != item.m_write_time) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Image, fail(
                               item, diagnostics, std::make_error_code(std::errc::resource_unavailable_try_again),
                               "Texture source changed during preparation"));
            }

            return {};
        }

        [[nodiscard]] std::error_code prepareBase(
            Item &item, const asset::PackSet &packs, const IContext &context, config::Diagnostics &diagnostics,
            const TexturePreparationLimits limits) {
            const hal::ProfileScope scope{"Image.Preparation.Base"};
            PPR_RETURN_ERROR_ON_FAIL(Image, context.error());
            PPR_RETURN_ERROR_ON_FAIL(Image, checkSource(item, packs, diagnostics));

            auto mapped = [&] {
                const hal::ProfileScope map_scope{"Image.Preparation.Map"};
                return mem::SharedBuffer::mapFile(item.m_path);
            }();
            if (not mapped and mapped.error() == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }

            if (not mapped) {
                PPR_RETURN_ERROR_ON_FAIL(Image, fail(item, diagnostics, mapped.error(), "Cannot map texture source"));
            }

            item.m_mapping = std::move(*mapped);
            const auto bytes = item.m_mapping.getBufferData();
            if (bytes.size_bytes() != item.m_expected_bytes) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Image, fail(
                               item, diagnostics, std::make_error_code(std::errc::resource_unavailable_try_again),
                               "Mapped texture size changed"));
            }

            item.m_prepared.m_source_fingerprint = sourceFingerprint(bytes);
            PPR_RETURN_ERROR_ON_FAIL(Image, context.error());

            const auto &descriptor = item.m_definition.m_descriptor.m_value;
            const ImageDecodeDesc decode{
                .m_limits = {
                    .m_max_width = limits.m_max_dimension,
                    .m_max_height = limits.m_max_dimension,
                    .m_max_decoded_bytes = u64{limits.m_max_dimension} * limits.m_max_dimension * 4u,
                    .m_max_input_bytes = limits.m_max_image_input_bytes}};
            auto image = [&] {
                const hal::ProfileScope decode_scope{"Image.Preparation.Decode"};
                return decodeToRgba8(
                    bytes, item.m_path.extension().string(), decode,
                    descriptor.m_interpretation == ETextureInterpretation::color ? EImageUsage::color
                                                                                 : EImageUsage::data);
            }();
            if (not image and image.error() == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }

            if (not image) {
                PPR_RETURN_ERROR_ON_FAIL(
                    Image, fail(item, diagnostics, image.error(), "Cannot decode texture within image limits"));
            }

            item.m_prepared.m_image = std::move(*image);
            const auto &base = item.m_prepared.m_image;
            const auto count = descriptor.m_full_chain ? std::bit_width((std::max) (base.m_width, base.m_height))
                                                       : mipCountFor(base.m_width, base.m_height);

            for (u32 level = 0u; level < count; ++level) {
                const auto bytes_at_level = slicePitchFor(
                    mipExtentAt(base.m_width, level), mipExtentAt(base.m_height, level), EBlockTag::none);
                if (bytes_at_level > limits.m_max_output_bytes - item.m_chain_bytes) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        Image, fail(
                                   item, diagnostics, std::make_error_code(std::errc::file_too_large),
                                   "Texture mip chain exceeds output limit"));
                }

                item.m_chain_bytes += bytes_at_level;
            }

            PPR_RETURN_ERROR_ON_FAIL(Image, checkSource(item, packs, diagnostics));
            PPR_RETURN_ERROR_ON_FAIL(Image, context.error());
            return {};
        }

        [[nodiscard]] std::error_code prepareMips(
            Item &item, const asset::PackSet &packs, const IContext &context, config::Diagnostics &diagnostics) {
            const hal::ProfileScope scope{"Image.Preparation.Mips"};
            PPR_RETURN_ERROR_ON_FAIL(Image, context.error());
            const auto &descriptor = item.m_definition.m_descriptor.m_value;
            const MipGenDesc mip{
                .m_limits = {.m_max_decoded_bytes = item.m_chain_bytes},
                .m_alpha_cutoff = descriptor.m_alpha_cutoff,
                .m_is_high_quality = descriptor.m_high_quality,
                .m_has_preserve_coverage = descriptor.m_preserve_coverage,
                .m_full_chain = descriptor.m_full_chain,
                .m_normal_map = descriptor.m_interpretation == ETextureInterpretation::normal};
            const auto error = generateMipChain(item.m_prepared.m_image, mip);
            if (error == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }

            if (error) {
                PPR_RETURN_ERROR_ON_FAIL(Image, fail(item, diagnostics, error, "Cannot generate texture mip chain"));
            }

            PPR_RETURN_ERROR_ON_FAIL(Image, checkSource(item, packs, diagnostics));
            PPR_RETURN_ERROR_ON_FAIL(Image, context.error());

            item.m_mapping.reset();
            return {};
        }

        // ------------------------------------------------------------------
        // owned reuse candidate checks
        // ------------------------------------------------------------------

        [[nodiscard]] bool sameDescriptor(const TextureDescriptor &left, const TextureDescriptor &right) noexcept {
            return left.m_source == right.m_source and
                   left.m_schema_version == right.m_schema_version and
                   left.m_interpretation == right.m_interpretation and
                   left.m_mip_policy == right.m_mip_policy and
                   left.m_full_chain == right.m_full_chain and
                   left.m_high_quality == right.m_high_quality and
                   left.m_preserve_coverage == right.m_preserve_coverage and
                   std::bit_cast<u32>(left.m_alpha_cutoff) == std::bit_cast<u32>(right.m_alpha_cutoff);
        }

        [[nodiscard]] bool reusableImage(
            const PreparedTexture &prepared, const TextureDescriptor &descriptor,
            const TexturePreparationLimits limits) {
            const auto &image = prepared.m_image;
            if (prepared.m_source_bytes == 0u or
                not image.m_storage.isOwned() or
                image.m_width > limits.m_max_dimension or
                image.m_height > limits.m_max_dimension or
                image.m_width == 0u or
                image.m_height == 0u) {
                return false;
            }

            const auto count = descriptor.m_full_chain ? std::bit_width((std::max) (image.m_width, image.m_height))
                                                       : mipCountFor(image.m_width, image.m_height);
            const auto format = descriptor.m_interpretation == ETextureInterpretation::color
                                    ? ENativeImageFormat::rgba8_srgb
                                    : ENativeImageFormat::rgba8_linear;
            if (image.m_mip_count != count or image.m_format != format) {
                return false;
            }

            // A malformed optional reuse candidate becomes a fresh preparation;
            // it never grants ownership or bypasses the canonical layout check.
            return not validateMipChain(image);
        }
    }

    u64 textureSettingsFingerprint(const TextureDescriptor &descriptor) noexcept {
        Fingerprint result;
        result.text("ppr.texture.settings.v1");
        result.integer(kTextureProcessorVersion, 4u);
        result.integer(descriptor.m_schema_version, 4u);
        result.integer(static_cast<u8>(descriptor.m_interpretation), 1u);
        result.integer(static_cast<u8>(descriptor.m_mip_policy), 1u);
        result.integer(descriptor.m_full_chain, 1u);
        result.integer(descriptor.m_high_quality, 1u);
        result.integer(descriptor.m_preserve_coverage, 1u);
        result.integer(std::bit_cast<u32>(descriptor.m_alpha_cutoff), 4u);
        return result.m_value;
    }

    // ------------------------------------------------------------------
    // metadata preflight and ordered image continuation
    // ------------------------------------------------------------------

    TextureProcessor::TextureProcessor(
        Array<TextureDefinition> definitions, const TexturePreparationLimits limits,
        Array<TextureReuseCandidate> reusable)
        : m_definitions(std::move(definitions)), m_reusable(std::move(reusable)), m_limits(limits) {
    }

    Expected<Array<std::shared_ptr<TextureProcessor::Item>>> TextureProcessor::prepare(
        const asset::PackSet &packs, const IContext &context, config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Image.Preparation.Preflight"};
        PPR_RETURN_UNEXPECTED_ON_FAIL(Image, validateValues(m_limits, "texture request", diagnostics));

        if (m_definitions.size() > m_limits.m_max_count) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Image, asset::reject(
                           diagnostics, "texture request", "textures",
                           "Selected texture closure exceeds request count limit", config::EConfigErrc::out_of_range));
        }

        Array<std::shared_ptr<Item>> items;
        items.reserve(m_definitions.size());
        m_reused.resize(m_definitions.size());
        m_chain_sizes.resize(m_definitions.size());
        m_sources.reserve(m_definitions.size());
        m_ordinals.reserve(m_definitions.size());
        m_budget_decisions.resize(m_definitions.size());

        std::set<u64> identities;

        for (auto &definition: m_definitions) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(Image, context.error());
            auto identity = packs.resolve(asset::AssetKey{*definition.m_id}, kTextureAssetKind, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Image, identity);

            const auto source = definition.m_descriptor.m_source.m_path.string();
            if (not identities.insert(*definition.m_id).second or
                definition.m_descriptor.m_source.m_path != (*identity)->m_descriptor) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Image, asset::reject(
                               diagnostics, source, "", "Texture definition must match one unique indexed descriptor",
                               config::EConfigErrc::duplicate_key));
            }

            const auto &descriptor = definition.m_descriptor.m_value;
            PPR_RETURN_UNEXPECTED_ON_FAIL(Image, validateValues(descriptor, source, diagnostics));
            if (descriptor.m_interpretation == ETextureInterpretation::normal and descriptor.m_preserve_coverage) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Image,
                    asset::reject(
                        diagnostics, source, "preserve_coverage", "Normal textures cannot preserve alpha coverage"));
            }

            auto path = packs.containedPath((*identity)->m_name.m_pack, descriptor.m_source, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Image, path);

            const auto ordinal = m_sources.size();
            m_sources.push_back(definition.m_descriptor.m_source.m_path);
            const auto &root = packs.packs()[(*identity)->m_pack_index].m_root;
            const auto reusable = std::ranges::find_if(m_reusable, [&](const TextureReuseCandidate &candidate) {
                const auto &prepared = candidate.m_prepared;
                return prepared.m_id == definition.m_id and
                       candidate.m_pack_root == root and
                       candidate.m_descriptor_path == definition.m_descriptor.m_source.m_path and
                       prepared.m_name.m_pack == (*identity)->m_name.m_pack and
                       prepared.m_name.m_local == (*identity)->m_name.m_local and
                       prepared.m_source_path == *path and
                       prepared.m_processor_version == kTextureProcessorVersion and
                       prepared.m_settings_fingerprint == textureSettingsFingerprint(descriptor) and
                       sameDescriptor(candidate.m_descriptor, descriptor) and
                       reusableImage(prepared, descriptor, m_limits);
            });

            if (reusable != m_reusable.end()) {
                const auto size = reusable->m_prepared.m_source_bytes;
                if (size == 0u or
                    size > m_limits.m_max_image_input_bytes or
                    size > m_limits.m_max_input_bytes - m_input_bytes) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        Image,
                        asset::reject(
                            diagnostics, source, "source", "Texture source exceeds per-image or aggregate input limit",
                            config::EConfigErrc::out_of_range));
                }

                m_input_bytes += size;
                m_chain_sizes[ordinal] = reusable->m_prepared.m_image.m_storage.getBufferData().size_bytes();
                m_reused[ordinal] = std::move(reusable->m_prepared);
                continue;
            }

            std::error_code error;
            const bool regular = std::filesystem::is_regular_file(*path, error);
            if (error) {
                diagnostics.add(source, "source", "Cannot inspect texture source: " + path->string(), error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Image, error);
            }

            if (not regular) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Image, asset::reject(diagnostics, source, "source", "Texture source must be a regular file"));
            }

            const auto size = std::filesystem::file_size(*path, error);
            if (error) {
                diagnostics.add(source, "source", "Cannot size texture source: " + path->string(), error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Image, error);
            }

            if (size == 0u or
                size > m_limits.m_max_image_input_bytes or
                size > m_limits.m_max_input_bytes - m_input_bytes) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Image,
                    asset::reject(
                        diagnostics, source, "source", "Texture source exceeds per-image or aggregate input limit",
                        config::EConfigErrc::out_of_range));
            }

            const auto time = std::filesystem::last_write_time(*path, error);
            if (error) {
                diagnostics.add(source, "source", "Cannot inspect texture source revision: " + path->string(), error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Image, error);
            }

            auto item = std::make_shared<Item>();
            item->m_limits = m_limits;
            item->m_path = std::move(*path);
            item->m_expected_bytes = size;
            item->m_write_time = time;
            item->m_prepared.m_id = definition.m_id;
            item->m_prepared.m_name = (*identity)->m_name;
            item->m_prepared.m_source_path = item->m_path;
            item->m_prepared.m_source_bytes = size;
            item->m_prepared.m_settings_fingerprint = textureSettingsFingerprint(descriptor);
            item->m_definition = std::move(definition);

            m_input_bytes += size;
            m_ordinals.push_back(ordinal);
            items.push_back(std::move(item));
        }

        m_definitions.clear();
        m_reusable.clear();

        PPR_RETURN_UNEXPECTED_ON_FAIL(Image, admitReadyPrefix(diagnostics));

        return items;
    }

    std::error_code TextureProcessor::admitReadyPrefix(config::Diagnostics &diagnostics) {
        while (m_budget_next < m_chain_sizes.size() and m_chain_sizes[m_budget_next].has_value()) {
            PPR_RETURN_ERROR_ON_FAIL(
                Image, m_budget_decisions[m_budget_next] == EBudgetDecision::rejected
                           ? std::make_error_code(std::errc::file_too_large)
                           : std::error_code{});

            const auto bytes = *m_chain_sizes[m_budget_next];
            if (bytes > m_limits.m_max_output_bytes - m_output_bytes) {
                m_budget_decisions[m_budget_next] = EBudgetDecision::rejected;
                diagnostics.add(
                    m_sources[m_budget_next].string(), "source",
                    "Selected texture closure exceeds aggregate mip output limit",
                    std::make_error_code(std::errc::file_too_large));
                PPR_RETURN_ERROR_ON_FAIL(Image, std::errc::file_too_large);
            }

            m_output_bytes += bytes;
            m_budget_decisions[m_budget_next] = EBudgetDecision::admitted;
            ++m_budget_next;
        }

        return {};
    }

    std::error_code TextureProcessor::process(
        Item &item, const asset::PackSet &packs, const IContext &context, config::Diagnostics &diagnostics) {
        if (item.m_phase == Item::EPhase::base) {
            PPR_RETURN_ERROR_ON_FAIL(Image, prepareBase(item, packs, context, diagnostics, item.m_limits));

            item.m_phase = Item::EPhase::base_ready;
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            Image, item.m_phase != Item::EPhase::mips ? std::make_error_code(std::errc::state_not_recoverable)
                                                      : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(Image, prepareMips(item, packs, context, diagnostics));

        item.m_phase = Item::EPhase::finished;
        return {};
    }

    Expected<asset::EPreparationStep> TextureProcessor::advance(
        const std::size_t index, Item &item, const bool succeeded, config::Diagnostics &diagnostics) {
        const auto ordinal = m_ordinals[index];

        if (not succeeded) {
            // The common request retains the processing failure. A zero-size
            // decision lets later accepted items settle without masking it.
            m_chain_sizes[ordinal] = 0u;
            PPR_RETURN_UNEXPECTED_ON_FAIL(Image, admitReadyPrefix(diagnostics));

            return asset::EPreparationStep::complete;
        }

        if (item.m_phase == Item::EPhase::finished) {
            return asset::EPreparationStep::complete;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Image, item.m_phase != Item::EPhase::base_ready ? std::make_error_code(std::errc::state_not_recoverable)
                                                            : std::error_code{});

        m_chain_sizes[ordinal] = item.m_chain_bytes;
        PPR_RETURN_UNEXPECTED_ON_FAIL(Image, admitReadyPrefix(diagnostics));

        if (m_budget_decisions[ordinal] != EBudgetDecision::admitted) {
            return asset::EPreparationStep::defer;
        }

        item.m_phase = Item::EPhase::mips;
        return asset::EPreparationStep::again;
    }

    Expected<TextureProcessor::Result> TextureProcessor::finish(
        const std::span<const std::shared_ptr<Item>> items, config::Diagnostics &) {
        Result result;
        result.m_input_bytes = m_input_bytes;
        result.m_output_bytes = m_output_bytes;
        result.m_textures.reserve(m_reused.size());

        for (std::size_t index = 0u; index < items.size(); ++index) {
            auto &prepared = items[index]->m_prepared;
            result.m_work.m_read_bytes += prepared.m_source_bytes;
            m_reused[m_ordinals[index]] = std::move(prepared);
        }

        result.m_work.m_read_count = static_cast<u32>(items.size());
        result.m_work.m_prepared_count = static_cast<u32>(items.size());
        result.m_work.m_reused_count = static_cast<u32>(m_reused.size() - items.size());

        for (auto &prepared: m_reused) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Image, not prepared ? std::make_error_code(std::errc::state_not_recoverable) : std::error_code{});

            result.m_textures.push_back(std::move(*prepared));
        }

        return result;
    }
#if 0
    Expected<TexturePreparation> prepareTextures(
        std::shared_ptr<const asset::PackSet> packs, Array<TextureDefinition> textures,
        const asset::RequestGeneration generation, SharedContext context, config::Diagnostics &diagnostics,
        const TexturePreparationLimits &limits) {
        return TexturePreparation::start(
            parallel::backgroundPool(), std::move(packs), TextureProcessor{std::move(textures), limits}, generation,
            std::move(context), diagnostics);
    }
#else
#endif
}
