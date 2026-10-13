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
    };
    [[nodiscard]] Expected<std::shared_ptr<const ContentCatalog>> validateContentDraft(
        const ContentAuthoringSnapshot &snapshot, config::Diagnostics &diagnostics, const IContext *context = nullptr);
    [[nodiscard]] Expected<ContentAuthoringSaveResult> saveContentDraft(
        ContentAuthoringSnapshot snapshot, config::Diagnostics &diagnostics, const IContext *context = nullptr);

    class ContentAuthoringSession {
        std::filesystem::path m_root;
        ContentPackSource m_baseline;
        ContentPackSource m_draft;
        CommandHistory<config::ScalarSourcePatch> m_history;
        u64 m_revision{1u};
        u64 m_validated_revision{1u};
        std::shared_ptr<const ContentCatalog> m_catalog;
        ContentAuthoringSession() = default;
        [[nodiscard]] std::error_code applyPatch(const config::ScalarSourcePatch &patch, bool reverse);

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
        [[nodiscard]] Expected<bool> undo();
        [[nodiscard]] Expected<bool> redo();
        void revert();
        [[nodiscard]] ContentAuthoringSnapshot draftSnapshot() const;
        [[nodiscard]] bool acceptValidation(u64 revision, std::shared_ptr<const ContentCatalog> catalog) noexcept;
        // Only an exact matching draft is cleared. Newer edits remain dirty.
        // Acceptance always advances revision because disk assets are revalidated.
        [[nodiscard]] bool acceptSave(ContentAuthoringSaveResult result);
    };
}
