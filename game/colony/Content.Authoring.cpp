module;
#include "pP/Macros.h"

module game.colony.content.authoring;
import engine.core;
import game.colony.content;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ContentAuthoring, debug, none)

    // ------------------------------------------------------------------
    // session helpers
    // ------------------------------------------------------------------

    namespace {
        bool validKey(const std::string_view key) {
            return config::isValidCanonicalName(key);
        }
        Expected<config::Document> definitionDocument(
            const ContentPackSource &source, const EContentDocument kind, const std::string_view key,
            config::Diagnostics &diagnostics) {
            const auto index = static_cast<std::size_t>(kind);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring, index < kContentDocumentCount and validKey(key)
                                      ? std::error_code{}
                                      : std::make_error_code(std::errc::invalid_argument));

            const auto &document = source.m_documents[index];
            auto decoded = config::decode(
                document.m_bytes, document.m_path.string(), diagnostics, config::EDecodeMode::tree);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, decoded);

            const auto root = contentDocumentPath(kind, key);
            const auto prefix = root + ".";
            config::Document record;
            bool present = false;

            for (const auto &entry: decoded->m_entries) {
                if (entry.m_key.starts_with(prefix)) {
                    present = true;
                    record.m_entries.push_back({entry.m_key.substr(prefix.size()), entry.m_value, entry.m_location});
                }
            }

            for (const auto &group: decoded->m_groups) {
                if (group.m_path == root) {
                    present = true;
                } else if (group.m_path.starts_with(prefix)) {
                    present = true;
                    record.m_groups.push_back({group.m_path.substr(prefix.size()), group.m_location});
                }
            }

            if (not present) {
                diagnostics.add(
                    document.m_path.string(), std::string{key}, "Unknown authored definition",
                    config::make_error_code(config::EConfigErrc::unknown_property));
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, config::EConfigErrc::unknown_property);
            }
            return record;
        }

        [[nodiscard]] std::error_code conflict(config::Diagnostics &diagnostics, const std::filesystem::path &path) {
            const auto error = std::make_error_code(std::errc::resource_unavailable_try_again);
            diagnostics.add(path.string(), "save", "Pack changed on disk; reload or retain the draft", error);
            return error;
        }
        [[nodiscard]] bool sameDocument(const ContentSourceDocument &left, const ContentSourceDocument &right) {
            return left.m_path == right.m_path and left.m_bytes == right.m_bytes;
        }
        [[nodiscard]] bool sameSource(const ContentPackSource &left, const ContentPackSource &right) {
            if (not sameDocument(left.m_manifest_source, right.m_manifest_source)) {
                return false;
            }
            for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
                if (not sameDocument(left.m_documents[index], right.m_documents[index])) {
                    return false;
                }
            }
            return true;
        }
        [[nodiscard]] std::error_code verifyStagingFile(
            const std::filesystem::path &path, const std::string_view bytes) {
            auto mapped = io::mapFile(path);
            if (not mapped and mapped.error() == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, mapped);
            if (mapped->size() != bytes.size() or std::string_view{mapped->c_str(), mapped->size()} != bytes) {
                PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, std::errc::io_error);
            }
            return {};
        }
    }

    // ------------------------------------------------------------------
    // session lifecycle
    // ------------------------------------------------------------------

    Expected<ContentAuthoringSession> ContentAuthoringSession::open(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *const context) {
        auto source = loadContentPackSource(root, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, source);

        auto catalog = loadContentCatalog(root, *source, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, catalog);

        std::error_code error;
        auto canonical_root = std::filesystem::canonical(root, error);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, error);

        ContentAuthoringSession session;
        session.m_root = std::move(canonical_root);
        session.m_baseline = *source;
        session.m_draft = std::move(*source);
        session.m_catalog = std::make_shared<const ContentCatalog>(std::move(*catalog));
        return session;
    }
    const std::filesystem::path &ContentAuthoringSession::root() const noexcept {
        return m_root;
    }
    const ContentPackSource &ContentAuthoringSession::baselineSource() const noexcept {
        return m_baseline;
    }
    const ContentPackSource &ContentAuthoringSession::draftSource() const noexcept {
        return m_draft;
    }
    u64 ContentAuthoringSession::revision() const noexcept {
        return m_revision;
    }
    u64 ContentAuthoringSession::validatedRevision() const noexcept {
        return m_validated_revision;
    }
    const std::shared_ptr<const ContentCatalog> &ContentAuthoringSession::validatedCatalog() const noexcept {
        return m_catalog;
    }
    bool ContentAuthoringSession::dirty() const noexcept {
        return not sameSource(m_draft, m_baseline);
    }
    bool ContentAuthoringSession::canUndo() const noexcept {
        return m_history.canUndo();
    }
    bool ContentAuthoringSession::canRedo() const noexcept {
        return m_history.canRedo();
    }

    // ------------------------------------------------------------------
    // draft edits
    // ------------------------------------------------------------------

    std::error_code ContentAuthoringSession::applyPatch(const SourceCommand &command, const bool reverse) {
        auto &document = m_draft.m_documents[static_cast<std::size_t>(command.m_document)];
        auto bytes = config::applySourcePatch(document.m_bytes, command.m_patch, reverse);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, bytes);

        document.m_bytes = std::move(*bytes);
        ++m_revision;
        return {};
    }
    std::error_code ContentAuthoringSession::executePatch(
        const EContentDocument document, config::ScalarSourcePatch patch) {
        if (patch.m_before == patch.m_after) {
            return {};
        }

        const auto payload_bytes = static_cast<u64>(patch.m_before.size() + patch.m_after.size());
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring, m_history.execute(
                                  SourceCommand{document, std::move(patch)}, payload_bytes,
                                  [&](const SourceCommand &command) { return applyPatch(command, false); }));
        return {};
    }
    std::error_code ContentAuthoringSession::editBiome(
        const std::string_view biome_key, const std::string_view relative_property, const config::FileValue &value,
        config::Diagnostics &diagnostics) {
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring,
            editDefinition(EContentDocument::biomes, biome_key, relative_property, value, diagnostics));
        return {};
    }
    std::error_code ContentAuthoringSession::editDefinition(
        const EContentDocument kind, const std::string_view key, const std::string_view relative_property,
        const config::FileValue &value, config::Diagnostics &diagnostics) {
        auto record = definitionDocument(m_draft, kind, key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, record);

        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring, config::isValidCanonicalName(relative_property)
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));

        if (relative_property.ends_with(".tag")) {
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, value.m_kind == config::EFileKind::text
                                      ? std::error_code{}
                                      : config::make_error_code(config::EConfigErrc::type_mismatch));
            const auto current = std::ranges::find(record->m_entries, relative_property, &config::Entry::m_key);
            if (current != record->m_entries.end() and
                current->m_value.m_kind == config::EFileKind::text and
                current->m_value.m_text == value.m_text) {
                return {};
            }

            const auto variant = relative_property.substr(0u, relative_property.size() - 4u);
            auto subtree = contentVariantSubtree(kind, key, variant, value.m_text);
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, subtree);
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, replaceDefinitionSubtree(kind, key, variant, *subtree, diagnostics));
            return {};
        }

        const auto index = static_cast<std::size_t>(kind);
        const auto &document = m_draft.m_documents[index];
        auto patch = config::patchScalar(
            document.m_bytes, contentDocumentPath(kind, key, relative_property), value, document.m_path.string(),
            diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, executePatch(kind, std::move(*patch)));
        return {};
    }
    std::error_code ContentAuthoringSession::clearDefinitionProperty(
        const EContentDocument kind, const std::string_view key, const std::string_view relative_property,
        config::Diagnostics &diagnostics) {
        auto record = definitionDocument(m_draft, kind, key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, record);

        const auto index = static_cast<std::size_t>(kind);
        const auto &source = m_draft.m_documents[index];
        auto document = config::decode(source.m_bytes, source.m_path.string(), diagnostics, config::EDecodeMode::tree);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, document);

        const auto descriptor = contentPropertyDescriptor(kind, relative_property, *document, key);
        const auto qualified = contentDocumentPath(kind, key, relative_property);
        if (not descriptor or not descriptor->m_optional) {
            diagnostics.add(
                source.m_path.string(), qualified, "Only optional configuration properties can be unset",
                config::make_error_code(config::EConfigErrc::cross_field_invalid));
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, config::EConfigErrc::cross_field_invalid);
        }

        const auto prefix = qualified + ".";
        if (std::ranges::none_of(
                document->m_entries,
                [&](const auto &entry) { return entry.m_key == qualified or entry.m_key.starts_with(prefix); }) and
            std::ranges::none_of(document->m_groups, [&](const auto &group) {
                return group.m_path == qualified or group.m_path.starts_with(prefix);
            })) {
            return {};
        }

        auto patch = config::patchTree(
            source.m_bytes, qualified, config::ETreeSourceEdit::remove, {}, source.m_path.string(), diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, executePatch(kind, std::move(*patch)));
        return {};
    }
    std::error_code ContentAuthoringSession::replaceDefinitionSubtree(
        const EContentDocument kind, const std::string_view key, const std::string_view relative_property,
        const config::Document &subtree, config::Diagnostics &diagnostics) {
        const auto record = definitionDocument(m_draft, kind, key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, record);
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring, config::isValidCanonicalName(relative_property)
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));

        const auto &source = m_draft.m_documents[static_cast<std::size_t>(kind)];
        auto patch = config::patchTree(
            source.m_bytes, contentDocumentPath(kind, key, relative_property), config::ETreeSourceEdit::replace,
            subtree, source.m_path.string(), diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, executePatch(kind, std::move(*patch)));
        return {};
    }
    std::error_code ContentAuthoringSession::cloneDefinition(
        const EContentDocument kind, const std::string_view source_key, const std::string_view new_key,
        config::Diagnostics &diagnostics) {
        auto record = definitionDocument(m_draft, kind, source_key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, record);

        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring,
            validKey(new_key) ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

        const auto index = static_cast<std::size_t>(kind);
        const auto &document = m_draft.m_documents[index];

        auto patch = config::patchTree(
            document.m_bytes, contentDocumentPath(kind, new_key), config::ETreeSourceEdit::insert, *record,
            document.m_path.string(), diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, executePatch(kind, std::move(*patch)));
        return {};
    }
    std::error_code ContentAuthoringSession::deleteDefinition(
        const EContentDocument kind, const std::string_view key, config::Diagnostics &diagnostics) {
        auto record = definitionDocument(m_draft, kind, key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, record);

        const auto index = static_cast<std::size_t>(kind);
        const auto &document = m_draft.m_documents[index];

        auto patch = config::patchTree(
            document.m_bytes, contentDocumentPath(kind, key), config::ETreeSourceEdit::remove, {},
            document.m_path.string(), diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, executePatch(kind, std::move(*patch)));
        return {};
    }
    Expected<bool> ContentAuthoringSession::undo() {
        auto result = m_history.undo([&](const SourceCommand &command) { return applyPatch(command, true); });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, result);
        return *result;
    }
    Expected<bool> ContentAuthoringSession::redo() {
        auto result = m_history.redo([&](const SourceCommand &command) { return applyPatch(command, false); });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, result);
        return *result;
    }
    void ContentAuthoringSession::revert() {
        // Copy first: allocation failure must not partially reset the session.
        auto restored = m_baseline;
        m_draft = std::move(restored);
        m_history.clear();
        ++m_revision;
    }
    ContentAuthoringSnapshot ContentAuthoringSession::draftSnapshot() const {
        return {m_root, m_draft, m_baseline, m_revision};
    }

    // ------------------------------------------------------------------
    // validation and save
    // ------------------------------------------------------------------

    bool ContentAuthoringSession::acceptValidation(
        const u64 revision, std::shared_ptr<const ContentCatalog> catalog) noexcept {
        if (revision != m_revision or not catalog) {
            return false;
        }
        m_catalog = std::move(catalog);
        m_validated_revision = revision;
        return true;
    }
    bool ContentAuthoringSession::acceptSave(ContentAuthoringSaveResult result) {
        if (result.m_snapshot.m_root != m_root) {
            return false;
        }

        const bool matching = result.m_snapshot.m_revision == m_revision and
                              sameSource(result.m_submitted_source, m_draft);
        const bool successful = matching and not result.m_error and result.m_catalog != nullptr;
        auto draft = result.m_snapshot.m_source;

        // Preserve desired bytes for every authored document, even when a
        // partial save committed a different value and the user then undid it.
        // Untouched documents adopt fresh disk bytes unless edited meanwhile.
        for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
            if (not sameDocument(
                    result.m_submitted_source.m_documents[index], result.m_snapshot.m_baseline.m_documents[index]) or
                not sameDocument(m_draft.m_documents[index], result.m_submitted_source.m_documents[index])) {
                draft.m_documents[index] = m_draft.m_documents[index];
            }
        }

        m_baseline = std::move(result.m_snapshot.m_source);
        m_draft = std::move(draft);

        // Saving revalidates on-disk pack inputs and texture bytes. Retire all
        // older previews/validations even when the authored source bytes match.
        ++m_revision;

        if (successful) {
            m_catalog = std::move(result.m_catalog);
            m_validated_revision = m_revision;
        }
        return successful;
    }
    Expected<std::shared_ptr<const ContentCatalog>> validateContentDraft(
        const ContentAuthoringSnapshot &snapshot, config::Diagnostics &diagnostics, const IContext *const context) {
        auto catalog = loadContentCatalog(snapshot.m_root, snapshot.m_source, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, catalog);
        return std::make_shared<const ContentCatalog>(std::move(*catalog));
    }

    Expected<ContentAuthoringSaveResult> saveContentDraft(
        ContentAuthoringSnapshot snapshot, config::Diagnostics &diagnostics, const IContext *const context) try {
        const auto cancelled = [&] { return context ? context->error() : std::error_code{}; };
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, cancelled());

        auto current = loadContentPackSource(snapshot.m_root, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, current);
        if (not sameDocument(current->m_manifest_source, snapshot.m_baseline.m_manifest_source)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, conflict(diagnostics, snapshot.m_root));
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAuthoring, sameDocument(snapshot.m_source.m_manifest_source, snapshot.m_baseline.m_manifest_source)
                                  ? std::error_code{}
                                  : std::make_error_code(std::errc::invalid_argument));

        auto submitted_source = snapshot.m_source;
        const ContentPackSource disk_before = *current;
        std::array<bool, kContentDocumentCount> changed{};

        for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
            const auto &requested = snapshot.m_source.m_documents[index];
            changed[index] = not sameDocument(requested, snapshot.m_baseline.m_documents[index]);
            if (requested.m_path != current->m_documents[index].m_path) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, conflict(diagnostics, requested.m_path));
            }
            if (changed[index]) {
                // A failed multi-document commit may already have replaced this
                // exact document. A retry accepts only baseline or requested bytes.
                if (not sameDocument(current->m_documents[index], snapshot.m_baseline.m_documents[index]) and
                    not sameDocument(current->m_documents[index], requested)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, conflict(diagnostics, requested.m_path));
                }
                current->m_documents[index] = requested;
            }
        }

        auto validated = loadContentCatalog(snapshot.m_root, *current, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, validated);
        auto catalog = std::make_shared<const ContentCatalog>(std::move(*validated));

        std::array<std::filesystem::path, kContentDocumentCount> temporary{};
        std::array<bool, kContentDocumentCount> acquired{}, replaced{};
        PPR_DEFER {
            for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
                if (acquired[index] and not replaced[index]) {
                    std::error_code cleanup;
                    std::filesystem::remove(temporary[index], cleanup);
                    // Staging cleanup is best effort; preserve the save failure.
                    PPR_LOG_WARNING_ON_FAIL(ContentAuthoring, cleanup);
                }
            }
        };

        // Acquire and verify every changed document before committing any one.
        for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
            if (not changed[index] or sameDocument(disk_before.m_documents[index], current->m_documents[index])) {
                continue;
            }
            const auto &target = current->m_documents[index];
            temporary[index] = target.m_path;
            temporary[index] += ".workshop.tmp";
            std::ofstream stream{temporary[index], std::ios::binary | std::ios::out | std::ios::noreplace};
            if (not stream) {
                diagnostics.add(
                    temporary[index].string(), "save", "Cannot exclusively create document staging file",
                    std::make_error_code(std::errc::io_error));
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring, stream ? std::error_code{} : std::make_error_code(std::errc::io_error));
            acquired[index] = true;
            stream.write(target.m_bytes.data(), static_cast<std::streamsize>(target.m_bytes.size()));
            stream.flush();
            if (not stream) {
                diagnostics.add(
                    temporary[index].string(), "save", "Cannot write complete document staging file",
                    std::make_error_code(std::errc::io_error));
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring, stream ? std::error_code{} : std::make_error_code(std::errc::io_error));
            stream.close();
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring, stream ? std::error_code{} : std::make_error_code(std::errc::io_error));
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, verifyStagingFile(temporary[index], target.m_bytes));
        }

        auto checked = loadContentPackSource(snapshot.m_root, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, checked);
        if (not sameSource(*checked, disk_before)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, conflict(diagnostics, snapshot.m_root));
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, cancelled());

        // Each sibling rename is atomic; the pack is deliberately not described
        // as atomic. After commit starts, finish independent replacements even
        // if cancellation arrives, retaining the disk baseline for every outcome.
        auto committed_source = disk_before;
        std::error_code first_error;

        for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
            if (not acquired[index]) {
                continue;
            }
            std::error_code error;
            std::filesystem::rename(temporary[index], current->m_documents[index].m_path, error);
            if (error) {
                diagnostics.add(
                    current->m_documents[index].m_path.string(), "save",
                    "Document replacement failed; draft retained. Other listed documents may have committed", error);
            } else {
                replaced[index] = true;
                committed_source.m_documents[index] = current->m_documents[index];
            }
            PPR_RETAIN_ERROR_ON_FAIL(ContentAuthoring, first_error, error);
        }

        // Reconcile even when a rename or verification fails. The submitted
        // baseline remains in the result so acceptance can preserve undo intent.
        auto reread = [&]() -> Expected<ContentPackSource> {
            try {
                return loadContentPackSource(snapshot.m_root, diagnostics);
            } catch (const std::bad_alloc &) {
                throw;
            } catch (const std::system_error &error) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentAuthoring,
                    error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
                std::unreachable();
            } catch (...) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, std::errc::state_not_recoverable);
                std::unreachable();
            }
        }();
        if (not reread) {
            PPR_RETAIN_ERROR_ON_FAIL(ContentAuthoring, first_error, reread.error());
        } else {
            if (not sameSource(*reread, *current)) {
                PPR_RETAIN_ERROR_ON_FAIL(ContentAuthoring, first_error, conflict(diagnostics, snapshot.m_root));
            }
            committed_source = std::move(*reread);
        }

        if (first_error) {
            for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
                if (replaced[index]) {
                    diagnostics.add(
                        committed_source.m_documents[index].m_path.string(), "save",
                        "This document committed before the reported pack save failure; its disk baseline was reconciled",
                        first_error);
                }
            }
            catalog.reset();
        }

        snapshot.m_source = std::move(committed_source);
        return ContentAuthoringSaveResult{
            std::move(snapshot), std::move(catalog), std::move(submitted_source), first_error};
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAuthoring, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, std::errc::state_not_recoverable);
        std::unreachable();
    }
}
