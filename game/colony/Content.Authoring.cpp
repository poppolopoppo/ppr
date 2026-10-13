module;
#include "pP/Macros.h"

module game.colony.content.authoring;
import engine.core;
import game.colony.content;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ContentAuthoring, debug, none)

    namespace {
        constexpr std::size_t biome_index = static_cast<std::size_t>(EContentDocument::biomes);

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
        return m_draft.m_documents[biome_index].m_bytes != m_baseline.m_documents[biome_index].m_bytes;
    }
    bool ContentAuthoringSession::canUndo() const noexcept {
        return m_history.canUndo();
    }
    bool ContentAuthoringSession::canRedo() const noexcept {
        return m_history.canRedo();
    }

    std::error_code ContentAuthoringSession::applyPatch(const config::ScalarSourcePatch &patch, const bool reverse) {
        auto &document = m_draft.m_documents[biome_index];
        auto bytes = config::applySourcePatch(document.m_bytes, patch, reverse);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, bytes);
        document.m_bytes = std::move(*bytes);
        ++m_revision;
        return {};
    }
    std::error_code ContentAuthoringSession::editBiome(
        const std::string_view biome_key, const std::string_view relative_property, const config::FileValue &value,
        config::Diagnostics &diagnostics) {
        if (biome_key.empty() or
            biome_key.find('.') != std::string_view::npos or
            not config::isValidCanonicalName(biome_key) or
            not config::isValidCanonicalName(relative_property) or
            relative_property == "numeric_id") {
            diagnostics.add(
                m_root.string(), "biomes", "Expected an existing biome and a stable scalar property",
                std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, std::errc::invalid_argument);
        }
        // Stable catalog keys identify records even while their typed values are invalid.
        if (std::ranges::none_of(
                m_catalog->biomes(), [&](const BiomeDefinition &biome) { return biome.m_key == biome_key; })) {
            diagnostics.add(
                m_root.string(), std::string{biome_key}, "Unknown biome",
                config::make_error_code(config::EConfigErrc::unknown_property));
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, config::EConfigErrc::unknown_property);
        }
        const auto &document = m_draft.m_documents[biome_index];
        auto patch = config::patchScalar(
            document.m_bytes, "biomes." + std::string{biome_key} + "." + std::string{relative_property}, value,
            document.m_path.string(), diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);
        if (patch->m_before == patch->m_after) {
            return {};
        }
        const auto payload_bytes = static_cast<u64>(patch->m_before.size() + patch->m_after.size());
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring,
            m_history.execute(std::move(*patch), payload_bytes, [&](const config::ScalarSourcePatch &command) {
                return applyPatch(command, false);
            }));
        return {};
    }
    Expected<bool> ContentAuthoringSession::undo() {
        auto result = m_history.undo(
            [&](const config::ScalarSourcePatch &command) { return applyPatch(command, true); });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, result);
        return *result;
    }
    Expected<bool> ContentAuthoringSession::redo() {
        auto result = m_history.redo(
            [&](const config::ScalarSourcePatch &command) { return applyPatch(command, false); });
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
        if (result.m_snapshot.m_root != m_root or not result.m_catalog) {
            return false;
        }
        const bool matching = result.m_snapshot.m_revision == m_revision and
                              sameDocument(
                                  result.m_snapshot.m_source.m_documents[biome_index],
                                  m_draft.m_documents[biome_index]);
        auto draft = result.m_snapshot.m_source;
        if (not matching) {
            draft.m_documents[biome_index] = m_draft.m_documents[biome_index];
        }
        m_baseline = std::move(result.m_snapshot.m_source);
        m_draft = std::move(draft);
        // Saving revalidates on-disk pack inputs and texture bytes. Retire all
        // older previews/validations even when the authored source bytes match.
        ++m_revision;
        if (matching) {
            m_catalog = std::move(result.m_catalog);
            m_validated_revision = m_revision;
        }
        return matching;
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
        if (not sameDocument(current->m_manifest_source, snapshot.m_baseline.m_manifest_source) or
            not sameDocument(current->m_documents[biome_index], snapshot.m_baseline.m_documents[biome_index])) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, conflict(diagnostics, snapshot.m_root));
        }
        // This transaction edits exactly one document. Other on-disk documents
        // are read afresh and participate in the ordinary full pack validation.
        if (not sameDocument(snapshot.m_source.m_manifest_source, snapshot.m_baseline.m_manifest_source)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, std::errc::invalid_argument);
        }
        for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
            if (index != biome_index and
                not sameDocument(snapshot.m_source.m_documents[index], snapshot.m_baseline.m_documents[index])) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, std::errc::invalid_argument);
            }
        }
        const ContentPackSource disk_before = *current;
        auto &target = current->m_documents[biome_index];
        if (target.m_path != snapshot.m_source.m_documents[biome_index].m_path) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, conflict(diagnostics, snapshot.m_root));
        }
        target.m_bytes = snapshot.m_source.m_documents[biome_index].m_bytes;
        auto validated = loadContentCatalog(snapshot.m_root, *current, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, validated);
        auto catalog = std::make_shared<const ContentCatalog>(std::move(*validated));

        auto temporary = target.m_path;
        temporary += ".workshop.tmp";
        bool acquired = false;
        bool replaced = false;
        PPR_DEFER {
            if (acquired and not replaced) {
                std::error_code cleanup;
                std::filesystem::remove(temporary, cleanup);
                // Staging cleanup is best effort; preserve the original save failure.
                PPR_LOG_WARNING_ON_FAIL(ContentAuthoring, cleanup);
            }
        };
        {
            // Exclusive creation cannot truncate another process's staging file.
            std::ofstream stream{temporary, std::ios::binary | std::ios::out | std::ios::noreplace};
            if (not stream) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, std::errc::io_error);
            }
            acquired = true;
            stream.write(target.m_bytes.data(), static_cast<std::streamsize>(target.m_bytes.size()));
            stream.flush();
            if (not stream) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, std::errc::io_error);
            }
            stream.close();
            if (not stream) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, std::errc::io_error);
            }
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, verifyStagingFile(temporary, target.m_bytes));
        auto checked = loadContentPackSource(snapshot.m_root, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, checked);
        if (not sameSource(*checked, disk_before)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, conflict(diagnostics, snapshot.m_root));
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, cancelled());
        std::error_code error;
        // Sibling rename stays on one volume. POSIX rename replaces atomically;
        // installed MSVC STL __std_fs_rename uses MoveFileExW(REPLACE_EXISTING).
        // No destination deletion precedes replacement. Conflict detection is
        // optimistic: external writers must not modify the pack during commit.
        std::filesystem::rename(temporary, target.m_path, error);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, error);
        replaced = true;
        auto reread = loadContentPackSource(snapshot.m_root, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, reread);
        if (not sameSource(*reread, *current)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, conflict(diagnostics, snapshot.m_root));
        }
        snapshot.m_source = std::move(*reread);
        snapshot.m_baseline = snapshot.m_source;
        return ContentAuthoringSaveResult{std::move(snapshot), std::move(catalog)};
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
