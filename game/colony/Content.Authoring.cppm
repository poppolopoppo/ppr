module;

export module game.colony.content.authoring;

import engine.core;
import engine.asset;
import engine.sim;
import game.colony.content;
import game.colony.worldgen;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // draft snapshots
    // ------------------------------------------------------------------

    struct ContentDiskRecord {
        std::shared_ptr<const ContentSourceSnapshot::Restoration> m_target;
        /// Absence is known ownership history, never permission to take over a
        /// file that reappears. Orphans/tombstones never enter a complete snapshot.
        std::optional<std::string> m_bytes;
    };

    struct ContentDiskBaseline {
        Array<asset::PackSource> m_packs;
        Array<ContentDiskRecord> m_descriptors;
    };

    // Values cross worker boundaries; no worker borrows the mutable session.
    struct ContentAuthoringSnapshot {
        std::filesystem::path m_root;
        ContentSourceSet m_source;
        ContentDiskBaseline m_baseline;
        sim::definition::GenerationCompileParameters m_parameters{kWorldCompileParameters};
        u64 m_revision{};
        ContentSourceEpochToken m_source_epoch{};
    };

    struct ContentAuthoringSaveResult {
        ContentAuthoringSnapshot m_snapshot;
        std::shared_ptr<const ContentDescriptorCatalog> m_catalog;
        // Dispatched draft retained for matching and newer-intent reconciliation.
        ContentSourceSet m_submitted_source;
        // A value may report a partial commit. Accept it to reconcile the known
        // disk baseline before displaying this error; no catalog is published.
        std::error_code m_error{};
    };

    struct ContentDraftValidation {
        std::shared_ptr<const ContentDescriptorCatalog> m_definitions;
        /// Present only when a World was requested; any requested failure rejects
        /// the entire validation rather than returning an empty selected view.
        std::shared_ptr<const ContentCatalog> m_selected;
    };

    [[nodiscard]] Expected<ContentDraftValidation> validateContentDraft(
        const ContentAuthoringSnapshot &snapshot, config::Diagnostics &diagnostics, const IContext *context = nullptr,
        std::optional<std::string_view> selected_world = std::nullopt);
    [[nodiscard]] Expected<ContentDraftValidation> validateContentDraft(
        const ContentSourceSnapshot &snapshot, sim::definition::GenerationCompileParameters parameters,
        config::Diagnostics &diagnostics, const IContext *context = nullptr,
        std::optional<std::string_view> selected_world = std::nullopt);
    [[nodiscard]] Expected<ContentAuthoringSaveResult> saveContentDraft(
        ContentAuthoringSnapshot snapshot, config::Diagnostics &diagnostics, const IContext *context = nullptr);

    // ------------------------------------------------------------------
    // authoring session
    // ------------------------------------------------------------------

    class ContentAuthoringSession {
        // state:
        std::filesystem::path m_root;
        ContentDiskBaseline m_baseline;
        ContentSourceSet m_draft;
        std::shared_ptr<const asset::PackSet> m_index;
        ContentSourceEpochToken m_source_epoch;
        sim::definition::GenerationCompileParameters m_parameters{kWorldCompileParameters};

        // dependent commands:
        struct FileChange {
            std::filesystem::path m_path;
            std::optional<std::string> m_before, m_after;
        };

        struct SourceCommand {
            Array<FileChange> m_changes;
            std::shared_ptr<const ContentSourceSnapshot::Restoration> m_target;
        };
        CommandHistory<SourceCommand> m_history;

        // revisions:
        u64 m_revision{1u};
        u64 m_validated_revision{1u};
        std::shared_ptr<const ContentDescriptorCatalog> m_catalog;

        // owner-lane dirty memo:
        // Draft and adopted disk-baseline commits advance m_revision.
        // Workers own snapshots and never query the mutable session.
        mutable u64 m_dirty_revision{};
        mutable bool m_dirty_value{};

        // construction and command application:
        ContentAuthoringSession() = default;

        [[nodiscard]] std::error_code applyPatch(
            const SourceCommand &command, bool reverse, std::shared_ptr<const asset::PackSet> prepared = {});
        [[nodiscard]] std::error_code executePatch(
            SourceCommand command, std::shared_ptr<const asset::PackSet> prepared = {});
        [[nodiscard]] std::error_code editSource(const asset::IndexedAsset &entry, config::ScalarSourcePatch patch);
        void pruneTombstones() noexcept;

    public:
        ContentAuthoringSession(ContentAuthoringSession &&) = default;
        ContentAuthoringSession &operator=(ContentAuthoringSession &&) = default;

        ContentAuthoringSession(const ContentAuthoringSession &) = delete;
        ContentAuthoringSession &operator=(const ContentAuthoringSession &) = delete;

        /// Open mints a fresh epoch when none is supplied. A supplied token must
        /// belong to this fresh Open/Reload candidate, never a prior session.
        [[nodiscard]] static Expected<ContentAuthoringSession> open(
            const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *context = nullptr,
            sim::definition::GenerationCompileParameters parameters = kWorldCompileParameters,
            ContentSourceEpochToken source_epoch = {});

        [[nodiscard]] static Expected<ContentAuthoringSession> open(
            std::span<const std::filesystem::path> roots, config::Diagnostics &diagnostics,
            const IContext *context = nullptr,
            sim::definition::GenerationCompileParameters parameters = kWorldCompileParameters,
            ContentSourceEpochToken source_epoch = {});

        // queries:
        [[nodiscard]] const std::filesystem::path &root() const noexcept;
        [[nodiscard]] const ContentDiskBaseline &baselineSource() const noexcept;
        [[nodiscard]] const ContentSourceSet &draftSource() const noexcept;
        [[nodiscard]] const asset::PackSet &sourceIndex() const noexcept;
        [[nodiscard]] const ContentSourceEpochToken &sourceEpoch() const noexcept;
        [[nodiscard]] u64 revision() const noexcept;
        [[nodiscard]] u64 validatedRevision() const noexcept;
        [[nodiscard]] const std::shared_ptr<const ContentDescriptorCatalog> &validatedCatalog() const noexcept;
        [[nodiscard]] bool dirty() const noexcept;
        [[nodiscard]] bool canUndo() const noexcept;
        [[nodiscard]] bool canRedo() const noexcept;

        // edits:
        [[nodiscard]] std::error_code editBiome(
            std::string_view biome_key, std::string_view relative_property, const config::FileValue &value,
            config::Diagnostics &diagnostics);
        [[nodiscard]] std::error_code editDefinition(
            EContentDocument document, std::string_view key, std::string_view relative_property,
            const config::FileValue &value, config::Diagnostics &diagnostics);
        [[nodiscard]] std::error_code cloneDefinition(
            EContentDocument document, std::string_view source_key, std::string_view new_key,
            config::Diagnostics &diagnostics);
        [[nodiscard]] std::error_code clearDefinitionProperty(
            EContentDocument document, std::string_view key, std::string_view relative_property,
            config::Diagnostics &diagnostics);
        [[nodiscard]] std::error_code replaceDefinitionSubtree(
            EContentDocument document, std::string_view key, std::string_view relative_property,
            const config::Document &subtree, config::Diagnostics &diagnostics);
        [[nodiscard]] std::error_code deleteDefinition(
            EContentDocument document, std::string_view key, config::Diagnostics &diagnostics);

        // history and result adoption:
        [[nodiscard]] Expected<bool> undo();
        [[nodiscard]] Expected<bool> redo();
        [[nodiscard]] std::error_code revert();
        [[nodiscard]] ContentAuthoringSnapshot draftSnapshot() const;
        [[nodiscard]] bool acceptValidation(
            u64 revision, std::shared_ptr<const ContentDescriptorCatalog> catalog) noexcept;
        // Reconcile committed disk bytes even on partial failure. Authored and
        // newer draft intent survives; only a full matching success is validated.
        [[nodiscard]] bool acceptSave(ContentAuthoringSaveResult result);
    };
}
