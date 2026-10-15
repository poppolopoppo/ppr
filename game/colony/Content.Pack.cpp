module;
#include "pP/Macros.h"
module game.colony.content;
import engine.core;
import engine.image;
import engine.sim;
import std;

namespace pP::colony {
    PPR_DECLARE_LOG_CATEGORY(ColonyContent)

    // ------------------------------------------------------------------
    // texture loading
    // ------------------------------------------------------------------

    namespace {
        using pP::make_error_code;
        struct Fingerprint {
            u64 m_value{14695981039346656037ull};
            void integer(const u64 value, const u32 width) noexcept {
                for (u32 i = 0u; i < width; ++i) {
                    m_value ^= static_cast<u8>(value >> (i * 8u));
                    m_value *= 1099511628211ull;
                }
            }
            void text(const std::string_view value) noexcept {
                integer(value.size(), 8u);
                for (const unsigned char byte: value) {
                    integer(byte, 1u);
                }
            }
        };

        struct PreparedTexture {
            ContentTexture m_texture{};
            std::filesystem::path m_path{};
            std::string m_location{};
            std::string m_property{};
            MappedFile m_mapping{};
            std::exception_ptr m_exception{};
            std::error_code m_error{};
            u64 m_chain_bytes{};
            u64 m_payload_bytes{};
            bool m_finished{};
            bool m_bad_alloc{};
        };

        // Bound admitted base/float payload by the existing maximum-size single-image workload.
        // Decoder/stb scratch and allocator overhead remain separate from this payload estimate.
        constexpr u64 kMaxTextureBaseBytes = u64{kMaxContentTextureDimension} * kMaxContentTextureDimension * 4u;
        constexpr u64 kMaxTextureMipPayloadBytes = kMaxTextureBaseBytes * 5u +
                                                   (kMaxTextureBaseBytes / 4u + kMaxTextureBaseBytes / 16u) * 4u;

        std::error_code textureFailure(
            config::Diagnostics &diagnostics, const std::filesystem::path &path, const MaterialDefinition &material,
            const std::string_view message, const std::error_code error, const bool normal_map = false) {
            diagnostics.add(
                normal_map ? material.m_normal_source_location : material.m_albedo_source_location,
                material.m_key.starts_with("appearance.backgrounds.")
                    ? material.m_key + ".texture"
                    : "appearance.materials." + material.m_key + (normal_map ? ".normal_texture" : ".albedo_texture"),
                std::string{message} + ": " + path.string(), error);
            return error;
        }
        Expected<PreparedTexture> prepareTexture(
            const std::filesystem::path &root, const MaterialDefinition &material, const u64 remaining_input,
            const u64 remaining_decoded, config::Diagnostics &diagnostics,
            const std23::function_ref<std::error_code(u64)> admit_mapping,
            const std23::function_ref<std::error_code(u64)> admit_base, const bool normal_map) {
            const hal::ProfileScope scope{"Content.Texture.Prepare"};
            const auto &relative = normal_map ? *material.m_normal_texture : *material.m_albedo_texture;
            const auto failTexture = [&](config::Diagnostics &target, const std::filesystem::path &failed_path,
                                         const MaterialDefinition &source, const std::string_view message,
                                         const std::error_code failure) {
                return textureFailure(target, failed_path, source, message, failure, normal_map);
            };

            std::error_code error{};
            const auto path = std::filesystem::canonical(root / relative, error);
            if (error) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(
                        diagnostics, root / relative, material, "Cannot resolve required terrain texture", error));
            }

            const auto within = path.lexically_relative(root);
            if (within.empty() or within.is_absolute() or *within.begin() == "..") {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, failTexture(
                                       diagnostics, path, material, "Texture escapes the pack root",
                                       std::make_error_code(std::errc::invalid_argument)));
            }

            const bool regular = std::filesystem::is_regular_file(path, error);
            if (error) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(diagnostics, path, material, "Cannot inspect required terrain texture", error));
            }
            if (not regular) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, failTexture(
                                       diagnostics, path, material, "Texture must be a regular file",
                                       std::make_error_code(std::errc::invalid_argument)));
            }

            const auto size = std::filesystem::file_size(path, error);
            if (error) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(diagnostics, path, material, "Cannot size required terrain texture", error));
            }
            if (size == 0u or size > kMaxTextureInputBytes or size > remaining_input or remaining_decoded == 0u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(
                        diagnostics, path, material, "Texture exceeds per-image or aggregate pack byte limits",
                        std::make_error_code(std::errc::file_too_large)));
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, admit_mapping(size));

            auto mapped = [&] {
                const hal::ProfileScope map_scope{"Content.Texture.Map"};
                return io::mapFile(path);
            }();
            if (not mapped and mapped.error() == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }
            if (not mapped) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(diagnostics, path, material, "Cannot map required terrain texture", mapped.error()));
            }
            if (mapped->size() != size) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, failTexture(
                                       diagnostics, path, material, "Required terrain texture changed while loading",
                                       std::make_error_code(std::errc::resource_unavailable_try_again)));
            }

            const mem::SharedBufferView bytes{reinterpret_cast<const std::byte *>(mapped->c_str()), mapped->size()};
            const image::ImageDecodeDesc desc{
                .m_limits = {
                    .m_max_width = kMaxContentTextureDimension,
                    .m_max_height = kMaxContentTextureDimension,
                    .m_max_decoded_bytes = (std::min) (remaining_decoded,
                        u64{kMaxContentTextureDimension} * kMaxContentTextureDimension * 4u),
                    .m_max_input_bytes = kMaxTextureInputBytes}};

            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, admit_base(desc.m_limits.m_max_decoded_bytes));

            auto decoded = [&] {
                const hal::ProfileScope decode_scope{"Content.Texture.Decode"};
                return image::decodeToRgba8(
                    bytes, path.extension().string(), desc,
                    normal_map ? image::EImageUsage::data : image::EImageUsage::color);
            }();
            if (not decoded and decoded.error() == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }
            if (not decoded) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, failTexture(
                                       diagnostics, path, material,
                                       "Cannot decode required terrain texture within pack limits", decoded.error()));
            }

            const u32 count = std::bit_width((std::max) (decoded->m_width, decoded->m_height));
            u64 chain_bytes{};
            u64 payload_bytes = decoded->m_storage.getBufferData().size_bytes() * 5u;
            for (u32 level = 0u; level < count; ++level) {
                const u64 level_bytes = image::slicePitchFor(
                    image::mipExtentAt(decoded->m_width, level), image::mipExtentAt(decoded->m_height, level),
                    image::EBlockTag::none);
                if (level_bytes > remaining_decoded - chain_bytes) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, failTexture(
                                           diagnostics, path, material, "Cannot generate terrain mip chain",
                                           image::make_error_code(image::errc::invalid_argument)));
                }

                chain_bytes += level_bytes;

                if (level == 1u or level == 2u) {
                    payload_bytes += level_bytes * sizeof(float);
                }
            }

            return PreparedTexture{
                .m_texture = ContentTexture{relative, size, 0u, std::move(*decoded), normal_map},
                .m_path = path,
                .m_location = normal_map ? material.m_normal_source_location : material.m_albedo_source_location,
                .m_property = material.m_key.starts_with("appearance.backgrounds.")
                                  ? material.m_key + ".texture"
                                  : "appearance.materials." +
                                        material.m_key +
                                        (normal_map ? ".normal_texture" : ".albedo_texture"),
                .m_mapping = std::move(*mapped),
                .m_chain_bytes = chain_bytes,
                .m_payload_bytes = payload_bytes};
        }

        void generateTexture(PreparedTexture &prepared) {
            const image::MipGenDesc mip_desc{
                .m_limits = {.m_max_decoded_bytes = prepared.m_chain_bytes},
                .m_full_chain = true,
                .m_normal_map = prepared.m_texture.m_normal_map};

            prepared.m_error = [&] {
                const hal::ProfileScope mip_scope{"Content.Texture.Mips"};
                return image::generateMipChain(prepared.m_texture.m_image, mip_desc);
            }();
            if (prepared.m_error == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }

            if (prepared.m_error) {
                return;
            }

            const auto bytes = prepared.m_mapping.span();
            Fingerprint source{};

            {
                const hal::ProfileScope hash_scope{"Content.Texture.HashSource"};
                source.text("ppr.texture.source.v1");
                source.integer(bytes.size_bytes(), 8u);

                for (const auto byte: bytes) {
                    source.integer(std::to_integer<u8>(byte), 1u);
                }
            }

            prepared.m_texture.m_source_fingerprint = source.m_value;
        }
    }

    // ------------------------------------------------------------------
    // catalog fingerprints
    // ------------------------------------------------------------------

    u64 ContentCatalog::materialFingerprint() const {
        sim::definition::SemanticFingerprint hash{m_definition};
        hash.string("ppr.materials.v4");
        hash.value(m_definition.m_materials);
        hash.value(m_definition.m_appearance.m_materials);
        return hash.hash();
    }
    u64 ContentCatalog::computeContentFingerprint() const {
        sim::definition::SemanticFingerprint hash{m_definition};
        hash.string("ppr.content.v4");
        hash.value(kContentSchemaVersion);
        hash.value(m_pack_id);
        hash.value(static_cast<const sim::definition::Definition &>(m_definition));
        hash.value(m_definition.m_appearance);

        // Asset identities include semantic usage and source bytes, independent of loading order.
        Array<const ContentTexture *> textures;
        textures.reserve(m_textures.size());
        for (const auto &texture: m_textures) {
            textures.push_back(std::addressof(texture));
        }
        std::ranges::sort(textures, [](const auto *left, const auto *right) {
            return std::tie(left->m_path, left->m_normal_map) < std::tie(right->m_path, right->m_normal_map);
        });
        hash.integer(static_cast<u64>(textures.size()));
        for (const auto *texture: textures) {
            hash.value(texture->m_path);
            hash.value(texture->m_normal_map);
            hash.value(texture->m_source_size);
            hash.value(texture->m_source_fingerprint);
        }
        return hash.hash();
    }
    std::string_view ContentCatalog::packId() const noexcept {
        return m_pack_id;
    }
    std::span<const FieldDefinition> ContentCatalog::fields() const noexcept {
        return {m_fields.data(), m_fields.size()};
    }
    std::span<const BiomeDefinition> ContentCatalog::biomes() const noexcept {
        return {m_biomes.data(), m_biomes.size()};
    }
    std::span<const FeatureDefinition> ContentCatalog::features() const noexcept {
        return {m_features.data(), m_features.size()};
    }
    std::span<const WorldProfileDefinition> ContentCatalog::worlds() const noexcept {
        return {m_worlds.data(), m_worlds.size()};
    }
    std::span<const ContentTexture> ContentCatalog::textures() const noexcept {
        return {m_textures.data(), m_textures.size()};
    }
    std::span<const BackgroundStackDefinition> ContentCatalog::backgrounds() const noexcept {
        return {m_backgrounds.data(), m_backgrounds.size()};
    }
    std::optional<u64> ContentCatalog::contentFingerprint() const noexcept {
        return m_content_fingerprint;
    }
    const WorldProfileDefinition *ContentCatalog::findWorld(const std::string_view key) const noexcept {
        const auto found = std::ranges::lower_bound(m_worlds, key, {}, &WorldProfileDefinition::m_key);
        return found == m_worlds.end() or found->m_key != key ? nullptr : std::addressof(*found);
    }

    // ------------------------------------------------------------------
    // catalog loading
    // ------------------------------------------------------------------

    Expected<ContentCatalog> loadContentCatalog(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *const context) {
        auto source = loadContentPackSource(root, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, source);
        return loadContentCatalog(root, *source, diagnostics, context);
    }

    Expected<ContentCatalog> loadContentCatalog(
        const std::filesystem::path &root, const ContentPackSource &source, config::Diagnostics &diagnostics,
        const IContext *const context) try {
        const auto cancelled = [&]() -> std::error_code { return context ? context->error() : std::error_code{}; };
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());

        std::error_code error{};
        const auto canonical_root = std::filesystem::canonical(root, error);
        if (error) {
            diagnostics.add(root.string(), "pack", "Cannot resolve pack root", error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
        }

        auto manifest = decodePackManifest(
            source.m_manifest_source.m_bytes, source.m_manifest_source.m_path.string(), diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, manifest);
        if (manifest->m_files != source.m_manifest.m_files or manifest->m_pack_id != source.m_manifest.m_pack_id) {
            diagnostics.add(
                root.string(), "pack", "Source manifest metadata does not match its bytes",
                std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::invalid_argument);
        }

        std::array<config::Document, kContentDocumentCount> documents;
        for (std::size_t index = 0u; index < documents.size(); ++index) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
            const auto &input = source.m_documents[index];
            if (input.m_bytes.size() > kMaxMaterialDocumentBytes) {
                diagnostics.add(
                    input.m_path.string(), "", "Content document exceeds 1 MiB",
                    config::make_error_code(config::EConfigErrc::out_of_range));
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, config::EConfigErrc::out_of_range);
            }
            auto document = config::decode(
                input.m_bytes, input.m_path.string(), diagnostics, config::EDecodeMode::tree);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
            documents[index] = std::move(*document);
        }
        auto document = mergeContentDocuments(documents, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
        auto definition = resolvePackDefinition(*document, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, definition);
        auto catalog = projectContentCatalog(std::move(*definition), *document, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, catalog);
        catalog->m_pack_id = source.m_manifest.m_pack_id;

        const hal::ProfileScope texture_scope{"Content.Textures.Load"};
        std::array<std::optional<PreparedTexture>, 2u> pending{};
        std::size_t pending_count{};
        u64 pending_input{}, pending_payload{};
        u64 input_bytes{}, decoded_bytes{};

        const auto flush = [&]() -> std::error_code {
            if (pending_count == 0u) {
                return {};
            }

            // forRange drains on return and exceptional unwind before this outer cleanup releases slots.
            PPR_DEFER {
                for (auto &prepared: pending) {
                    prepared.reset();
                }

                pending_count = 0u;
                pending_input = 0u;
                pending_payload = 0u;
            };

            std::exception_ptr dispatch_exception{};
            std::error_code generated{};
            bool dispatch_bad_alloc = false;
            try {
                generated = parallel::forRange(
                    0u, pending_count,
                    [&](const std::size_t index) noexcept {
                        auto &prepared = *pending[index];

                        try {
                            generateTexture(prepared);
                        } catch (const std::bad_alloc &) {
                            prepared.m_exception = std::current_exception();
                            prepared.m_bad_alloc = true;
                        } catch (...) {
                            prepared.m_exception = std::current_exception();
                        }

                        prepared.m_finished = true;
                    },
                    parallel::backgroundPool());
            } catch (const std::bad_alloc &) {
                dispatch_exception = std::current_exception();
                dispatch_bad_alloc = true;
            } catch (...) {
                dispatch_exception = std::current_exception();
            }

            // Allocation failure remains fatal, including from a later speculative slot.
            for (std::size_t index = 0u; index < pending_count; ++index) {
                if (pending[index]->m_bad_alloc) {
                    std::rethrow_exception(pending[index]->m_exception);
                }
            }

            if (dispatch_bad_alloc) {
                std::rethrow_exception(dispatch_exception);
            }

            // Completed earlier textures retain serial error precedence over a later admission exception.
            for (std::size_t index = 0u; index < pending_count; ++index) {
                auto &prepared = *pending[index];
                PPR_ASSERT(prepared.m_finished or generated != std::error_code{} or dispatch_exception);

                if (not prepared.m_finished) {
                    if (dispatch_exception) {
                        std::rethrow_exception(dispatch_exception);
                    }

                    prepared.m_error = generated;
                }

                if (prepared.m_exception) {
                    std::rethrow_exception(prepared.m_exception);
                }

                if (prepared.m_error) {
                    diagnostics.add(
                        prepared.m_location, prepared.m_property,
                        "Cannot generate terrain mip chain: " + prepared.m_path.string(), prepared.m_error);
                    PPR_RETURN_ERROR_ON_FAIL(ColonyContent, prepared.m_error);
                }

                catalog->m_textures.push_back(std::move(prepared.m_texture));
            }

            if (dispatch_exception) {
                std::rethrow_exception(dispatch_exception);
            }

            PPR_RETURN_ERROR_ON_FAIL(ColonyContent, generated);
            return {};
        };

        const auto check_cancelled = [&]() -> std::error_code {
            const auto error = cancelled();
            if (error) {
                // An earlier accepted texture's failure precedes cancellation at this boundary.
                PPR_RETURN_ERROR_ON_FAIL(ColonyContent, flush());
            }

            PPR_RETURN_ERROR_ON_FAIL(ColonyContent, error);
            return {};
        };

        const auto contains = [&](const std::string_view path, const bool normal_map) {
            if (std::ranges::any_of(catalog->m_textures, [&](const ContentTexture &item) {
                    return item.m_path == path and item.m_normal_map == normal_map;
                })) {
                return true;
            }

            for (std::size_t index = 0u; index < pending_count; ++index) {
                const auto &texture = pending[index]->m_texture;
                if (texture.m_path == path and texture.m_normal_map == normal_map) {
                    return true;
                }
            }

            return false;
        };

        const auto load_texture = [&](const MaterialDefinition &material, const bool normal_map) -> std::error_code {
            try {
                const auto admit_mapping = [&](const u64 size) -> std::error_code {
                    if (size > kMaxTextureInputBytes - pending_input) {
                        PPR_RETURN_ERROR_ON_FAIL(ColonyContent, flush());
                    }

                    return {};
                };

                const auto admit_base = [&](const u64 size) -> std::error_code {
                    if (size > kMaxTextureMipPayloadBytes - pending_payload) {
                        PPR_RETURN_ERROR_ON_FAIL(ColonyContent, flush());
                    }

                    return {};
                };

                config::Diagnostics preparation_diagnostics{};
                auto prepared = prepareTexture(
                    canonical_root, material, kMaxPackTextureInputBytes - input_bytes,
                    kMaxPackTextureDecodedBytes - decoded_bytes, preparation_diagnostics, admit_mapping, admit_base,
                    normal_map);
                if (not prepared) {
                    // Preserve the serial first-error owner before forwarding a later preparation failure.
                    PPR_RETURN_ERROR_ON_FAIL(ColonyContent, flush());

                    for (const auto &entry: preparation_diagnostics.entries()) {
                        diagnostics.add(
                            entry.m_location, entry.m_property, entry.m_message, entry.m_code, entry.m_alias,
                            entry.m_related_properties);
                    }

                    diagnostics.addOmitted(preparation_diagnostics.omittedCount());
                    PPR_RETURN_ERROR_ON_FAIL(ColonyContent, prepared.error());
                }

                if (prepared->m_payload_bytes > kMaxTextureMipPayloadBytes - pending_payload) {
                    // The candidate base was admitted before decode; finish earlier work while retaining that base.
                    PPR_RETURN_ERROR_ON_FAIL(ColonyContent, flush());
                }

                input_bytes += prepared->m_texture.m_source_size;
                decoded_bytes += prepared->m_chain_bytes;
                pending_input += prepared->m_texture.m_source_size;
                pending_payload += prepared->m_payload_bytes;
                pending[pending_count].emplace(std::move(*prepared));
                ++pending_count;

                if (pending_count == pending.size()) {
                    PPR_RETURN_ERROR_ON_FAIL(ColonyContent, flush());
                }

                return {};
            } catch (const std::bad_alloc &) {
                const auto exception = std::current_exception();

                // flush drains before inspecting results; preserve the pending fatal cause even if
                // reporting an earlier worker's ordinary exception throws after that drain.
                try {
                    std::ignore = flush();
                } catch (...) {
                    std::rethrow_exception(exception);
                }

                std::rethrow_exception(exception);
            } catch (...) {
                const auto exception = std::current_exception();

                PPR_RETURN_ERROR_ON_FAIL(ColonyContent, flush());

                std::rethrow_exception(exception);
            }
        };

        for (const auto &material: catalog->materials()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, check_cancelled());

            for (const bool normal_map: {false, true}) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, check_cancelled());

                const auto &path = normal_map ? material.m_normal_texture : material.m_albedo_texture;
                if (not path or contains(*path, normal_map)) {
                    continue;
                }

                if (catalog->m_textures.size() + pending_count >= kMaxContentTextures) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, flush());

                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, textureFailure(
                                           diagnostics, *path, material, "Too many distinct texture usages",
                                           std::make_error_code(std::errc::file_too_large), normal_map));
                }

                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, load_texture(material, normal_map));
            }
        }

        for (const auto &stack: catalog->m_backgrounds) {
            for (const auto &layer: stack.m_layers) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, check_cancelled());

                if (contains(layer.m_texture, false)) {
                    continue;
                }

                MaterialDefinition source_material{
                    .m_key = "appearance.backgrounds." + stack.m_key + ".layers." + layer.m_key,
                    .m_albedo_texture = layer.m_texture,
                    .m_albedo_source_location = layer.m_source_location};

                if (catalog->m_textures.size() + pending_count >= kMaxContentTextures) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, flush());

                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::file_too_large);
                }

                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, load_texture(source_material, false));
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, flush());

        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());

        catalog->m_content_fingerprint = catalog->computeContentFingerprint();
        return std::move(*catalog);
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
}
