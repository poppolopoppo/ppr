module;
#include "pP/Macros.h"

module game.colony.content;

import engine.asset;
import engine.core;
import engine.image;
import engine.sim;
import std;

namespace pP::colony {
    using pP::make_error_code;

    PPR_DEFINE_LOG_CATEGORY(ContentPresentationLog, debug, none)

    // ------------------------------------------------------------------
    // immutable presentation owners
    // ------------------------------------------------------------------

    class ContentSourceEpoch {};

    ContentSourceEpochToken makeContentSourceEpoch() {
        return std::make_shared<const ContentSourceEpoch>();
    }

    ContentPresentation::ContentPresentation(
        std::shared_ptr<const ContentCatalog> catalog, Array<ContentTexture> textures,
        const asset::RequestGeneration generation, const u64 fingerprint, ContentSourceEpochToken source_epoch,
        const u32 processing_revision, const image::TexturePreparationWork work)
        : m_catalog{std::move(catalog)}, m_source_epoch{std::move(source_epoch)}, m_textures{std::move(textures)},
          m_generation{generation}, m_fingerprint{fingerprint}, m_work{work},
          m_processing_revision{processing_revision} {
    }

    const std::shared_ptr<const ContentCatalog> &ContentPresentation::catalog() const noexcept {
        return m_catalog;
    }

    std::span<const ContentTexture> ContentPresentation::textures() const noexcept {
        return m_textures;
    }

    asset::RequestGeneration ContentPresentation::generation() const noexcept {
        return m_generation;
    }

    u64 ContentPresentation::fingerprint() const noexcept {
        return m_fingerprint;
    }

    const ContentSourceEpochToken &ContentPresentation::sourceEpoch() const noexcept {
        return m_source_epoch;
    }

    u32 ContentPresentation::processingRevision() const noexcept {
        return m_processing_revision;
    }

    const image::TexturePreparationWork &ContentPresentation::work() const noexcept {
        return m_work;
    }

    // ------------------------------------------------------------------
    // shared image request composition
    // ------------------------------------------------------------------

    ContentPresentationRequest::ContentPresentationRequest(
        std::shared_ptr<const ContentCatalog> catalog, image::TexturePreparation request,
        ContentSourceEpochToken source_epoch)
        : m_catalog{std::move(catalog)}, m_source_epoch{std::move(source_epoch)}, m_request{std::move(request)} {
    }

    Expected<ContentPresentationRequest> ContentPresentationRequest::start(
        std::shared_ptr<const ContentCatalog> catalog, const asset::RequestGeneration generation, SharedContext context,
        config::Diagnostics &diagnostics, ContentSourceEpochToken source_epoch,
        const std::shared_ptr<const ContentPresentation> &previous) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentPresentationLog,
            not catalog or not catalog->descriptorOwner() or catalog->selectedRecipe() == nullptr
                ? std::make_error_code(std::errc::invalid_argument)
                : std::error_code{});

        const auto definitions = catalog->textureDefinitions();
        Array<image::TextureDefinition> textures{definitions.begin(), definitions.end()};

        Array<image::TextureReuseCandidate> reusable;
        if (source_epoch and
            previous and
            source_epoch == previous->sourceEpoch() and
            previous->processingRevision() == image::kTextureProcessorVersion) {
            const auto &old_catalog = *previous->catalog();
            const auto &packs = old_catalog.descriptorOwner()->packs();
            reusable.reserve(previous->textures().size());

            for (const auto &texture: previous->textures()) {
                auto name = asset::qualifyName(texture.m_path, {});
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentPresentationLog, name);

                const auto key = asset::identityKey(*name, image::kTextureAssetKind);
                const auto old_definitions = old_catalog.textureDefinitions();
                const auto definition = std::ranges::find(
                    old_definitions, image::TextureId{*key}, &image::TextureDefinition::m_id);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentPresentationLog, definition == old_definitions.end()
                                                ? std::make_error_code(std::errc::state_not_recoverable)
                                                : std::error_code{});

                auto indexed = packs.resolve(asset::AssetKey{*definition->m_id}, image::kTextureAssetKind, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentPresentationLog, indexed);

                image::TextureReuseCandidate candidate;
                candidate.m_prepared.m_image = texture.m_image;
                candidate.m_prepared.m_name = (*indexed)->m_name;
                candidate.m_prepared.m_source_path = texture.m_source_path;
                candidate.m_prepared.m_id = definition->m_id;
                candidate.m_prepared.m_source_fingerprint = texture.m_source_fingerprint;
                candidate.m_prepared.m_source_bytes = texture.m_source_size;
                candidate.m_prepared.m_settings_fingerprint = image::textureSettingsFingerprint(
                    definition->m_descriptor.m_value);
                candidate.m_prepared.m_processor_version = previous->processingRevision();
                candidate.m_descriptor = definition->m_descriptor.m_value;
                candidate.m_pack_root = packs.packs()[(*indexed)->m_pack_index].m_root;
                candidate.m_descriptor_path = definition->m_descriptor.m_source.m_path;

                reusable.push_back(std::move(candidate));
            }
        }

        auto request = image::prepareTextures(
            catalog->descriptorOwner()->packOwner(), textures, generation, context, diagnostics, {}, reusable);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentPresentationLog, request);

        return ContentPresentationRequest{std::move(catalog), std::move(*request), std::move(source_epoch)};
    }

    bool ContentPresentationRequest::poll() noexcept {
        return m_request.poll();
    }

    void ContentPresentationRequest::cancel() noexcept {
        m_request.cancel();
    }

    asset::PreparationProgress ContentPresentationRequest::progress() const noexcept {
        return m_request.progress();
    }

    Expected<std::shared_ptr<const ContentPresentation>> ContentPresentationRequest::take(
        config::Diagnostics &diagnostics) {
        return complete(m_request.take(diagnostics), diagnostics);
    }

    Expected<std::shared_ptr<const ContentPresentation>> ContentPresentationRequest::finish(
        config::Diagnostics &diagnostics) {
        return complete(m_request.finish(diagnostics), diagnostics);
    }

    // ------------------------------------------------------------------
    // joined presentation identity and matched result publication
    // ------------------------------------------------------------------

    Expected<std::shared_ptr<const ContentPresentation>> ContentPresentationRequest::complete(
        Expected<image::TexturePreparation::Result> result, config::Diagnostics &diagnostics) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentPresentationLog, result);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentPresentationLog, not m_catalog or
                                            result->m_packs != m_catalog->descriptorOwner()->packOwner() or
                                            result->m_value.m_textures.size() != m_catalog->textureDefinitions().size()
                                        ? std::make_error_code(std::errc::invalid_argument)
                                        : std::error_code{});

        const hal::ProfileScope scope{"Content.Presentation.Join"};
        sim::definition::SemanticFingerprint fingerprint{m_catalog->definition()};
        fingerprint.string("ppr.worldgen.presentation.v1");
        fingerprint.integer(*m_catalog->contentFingerprint());
        fingerprint.integer(m_catalog->presentationDescriptorFingerprint());
        fingerprint.integer(result->m_processing_revision);
        Array<ContentTexture> textures;
        textures.reserve(result->m_value.m_textures.size());
        std::set<u64> seen;

        for (auto &prepared: result->m_value.m_textures) {
            const auto definitions = m_catalog->textureDefinitions();
            const auto definition = std::ranges::find(definitions, prepared.m_id, &image::TextureDefinition::m_id);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentPresentationLog,
                definition == definitions.end() or not seen.insert(*prepared.m_id).second
                    ? asset::reject(
                          diagnostics, "content presentation", "textures",
                          "Prepared texture is outside the selected World's exact image closure")
                    : std::error_code{});

            fingerprint.integer(*prepared.m_id);
            fingerprint.integer(prepared.m_source_fingerprint);
            fingerprint.integer(prepared.m_settings_fingerprint);
            fingerprint.integer(prepared.m_processor_version);
            textures.push_back(
                {prepared.m_name.string(), std::move(prepared.m_source_path), prepared.m_source_bytes,
                    prepared.m_source_fingerprint, std::move(prepared.m_image),
                    definition->m_descriptor.m_value.m_interpretation == image::ETextureInterpretation::normal});
        }

        // The private constructor is immediately adopted by its shared RAII
        // owner; callers can never manufacture a sidecar without preparation.
        return std::shared_ptr<const ContentPresentation>{new ContentPresentation{
            std::move(m_catalog), std::move(textures), result->m_generation, fingerprint.hash(),
            std::move(m_source_epoch), result->m_processing_revision, result->m_value.m_work}};
    }
}
