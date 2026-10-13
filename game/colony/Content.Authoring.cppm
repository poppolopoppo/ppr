module;

export module game.colony.content.authoring;

import engine.core;
import game.colony.content;
import std;

export namespace pP::colony {
    // Values cross worker boundaries; no worker borrows the mutable session.
    struct ContentAuthoringSnapshot {
        std::filesystem::path m_root;
        ContentPackSource m_source;
        ContentPackSource m_baseline;
        u64 m_revision{};
    };
    struct ContentAuthoringSaveResult {
        ContentAuthoringSnapshot m_snapshot;
        std::shared_ptr<const ContentCatalog> m_catalog;
        // The dispatched draft, before untouched files adopt fresh disk bytes.
        ContentPackSource m_submitted_source;
        // A value may report a partial commit. Accept it to reconcile the known
        // disk baseline before displaying this error; no catalog is published.
        std::error_code m_error{};
    };
    [[nodiscard]] Expected<std::shared_ptr<const ContentCatalog>> validateContentDraft(
        const ContentAuthoringSnapshot &snapshot, config::Diagnostics &diagnostics, const IContext *context = nullptr);
    [[nodiscard]] Expected<ContentAuthoringSaveResult> saveContentDraft(
        ContentAuthoringSnapshot snapshot, config::Diagnostics &diagnostics, const IContext *context = nullptr);

    class ContentAuthoringSession {
        std::filesystem::path m_root;
        ContentPackSource m_baseline;
        ContentPackSource m_draft;
        struct SourceCommand {
            EContentDocument m_document{};
            config::ScalarSourcePatch m_patch;
        };
        CommandHistory<SourceCommand> m_history;
        u64 m_revision{1u};
        u64 m_validated_revision{1u};
        std::shared_ptr<const ContentCatalog> m_catalog;
        ContentAuthoringSession() = default;
        [[nodiscard]] std::error_code applyPatch(const SourceCommand &command, bool reverse);
        [[nodiscard]] std::error_code executePatch(EContentDocument document, config::ScalarSourcePatch patch);

    public:
        ContentAuthoringSession(ContentAuthoringSession &&) = default;
        ContentAuthoringSession &operator=(ContentAuthoringSession &&) = default;
        ContentAuthoringSession(const ContentAuthoringSession &) = delete;
        ContentAuthoringSession &operator=(const ContentAuthoringSession &) = delete;
        [[nodiscard]] static Expected<ContentAuthoringSession> open(
            const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *context = nullptr);
        [[nodiscard]] const std::filesystem::path &root() const noexcept;
        [[nodiscard]] const ContentPackSource &baselineSource() const noexcept;
        [[nodiscard]] const ContentPackSource &draftSource() const noexcept;
        [[nodiscard]] u64 revision() const noexcept;
        [[nodiscard]] u64 validatedRevision() const noexcept;
        [[nodiscard]] const std::shared_ptr<const ContentCatalog> &validatedCatalog() const noexcept;
        [[nodiscard]] bool dirty() const noexcept;
        [[nodiscard]] bool canUndo() const noexcept;
        [[nodiscard]] bool canRedo() const noexcept;
        [[nodiscard]] std::error_code editBiome(
            std::string_view biome_key, std::string_view relative_property, const config::FileValue &value,
            config::Diagnostics &diagnostics);
        [[nodiscard]] std::error_code editDefinition(
            EContentDocument document, std::string_view key, std::string_view relative_property,
            const config::FileValue &value, config::Diagnostics &diagnostics);
        [[nodiscard]] std::error_code cloneDefinition(
            EContentDocument document, std::string_view source_key, std::string_view new_key,
            std::optional<u16> new_numeric_id, config::Diagnostics &diagnostics);
        [[nodiscard]] std::error_code clearDefinitionProperty(
            EContentDocument document, std::string_view key, std::string_view relative_property,
            config::Diagnostics &diagnostics);
        [[nodiscard]] std::error_code deleteDefinition(
            EContentDocument document, std::string_view key, config::Diagnostics &diagnostics);
        [[nodiscard]] Expected<bool> undo();
        [[nodiscard]] Expected<bool> redo();
        void revert();
        [[nodiscard]] ContentAuthoringSnapshot draftSnapshot() const;
        [[nodiscard]] bool acceptValidation(u64 revision, std::shared_ptr<const ContentCatalog> catalog) noexcept;
        // Reconcile committed disk bytes even on partial failure. Authored and
        // newer draft intent survives; only a full matching success is validated.
        [[nodiscard]] bool acceptSave(ContentAuthoringSaveResult result);
    };
}
