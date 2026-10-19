module;
export module game.colony.worldgen_panel;
import engine.app;
import engine.core;
import engine.sim;
import engine.asset;
import game.colony.content;
import game.colony.worldgen_preview;
import game.colony.asset_browser_panel;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // intents
    // ------------------------------------------------------------------

    enum class EWorkshopIntent : u8 {
        open,
        preview,
        cancel,
        save,
        reload,
        revert,
        undo,
        redo,
        export_preview,
        close,
        return_to_game,
        start_game,
        edit_biome,
        edit_definition,
        clone_definition,
        delete_definition,
        preview_settings_changed,
        clear_property
    };
    struct WorkshopIntent {
        EWorkshopIntent m_kind{};
        std::string m_biome{}, m_property{};
        config::FileValue m_value{};
        EContentDocument m_document{EContentDocument::biomes};
        std::string m_key{}, m_source_key{};
    };

    // ------------------------------------------------------------------
    // panel state
    // ------------------------------------------------------------------

    struct WorldgenPanelState {
        struct PropertyNode {
            std::string m_name{}, m_path{};
            std::optional<std::size_t> m_entry{};
            std::optional<ContentPropertyDescriptor> m_descriptor{};
            Array<std::string> m_choices{};
            Array<PropertyNode> m_children{};
        };

        struct DescriptorDocument {
            std::filesystem::path m_path;
            std::string m_name, m_cached_bytes;
            config::Document m_document;
            EContentDocument m_category{};
            bool m_decoded{}, m_projected{};
        };

        struct ReferenceUse {
            std::string m_name, m_property, m_world;
            EContentDocument m_category{};
        };

        AssetBrowserPanelState m_browser{};
        std::optional<asset::AssetKey> m_selected_asset{};
        std::string m_pending_category{}, m_pending_key{}, m_pending_property{};

        u64 m_seed{42u};
        std::string m_selected_category{"biomes"}, m_selected_key{};
        std::string m_edit_key{};
        std::string m_numeric_edit_key{};
        Array<char> m_text_edit{};
        config::FileValue m_numeric_edit{};
        Array<DescriptorDocument> m_documents{};
        std::array<Array<std::string>, kContentDocumentCount> m_record_keys{};
        Array<PropertyNode> m_property_tree{};
        std::string m_tree_selection{}, m_focus_property{};
        Array<ReferenceUse> m_uses;
        std::string m_use_selection;

        // owned preview choices, invalidated with document refresh or selected profile:
        Array<std::string> m_sample_biomes{};
        std::string m_sample_profile{};

        u64 m_document_revision{};
        u64 m_document_validated_revision{};
        const ContentSourceSet *m_document_source{};
        const ContentDescriptorCatalog *m_document_catalog{};
        const asset::PackSet *m_document_index{};

        int m_origin[2]{1920, 1920};
        EGridDiagnosticMode m_mode{EGridDiagnosticMode::textured};
        EWorldgenPreviewMode m_preview_mode{EWorldgenPreviewMode::biome_sample};
        bool m_automatic_live{}, m_show_properties{}, m_show_diagnostics{}, m_selection_pending{};
        bool m_sample_choices_current{};
        std::array<char, 1025u> m_root{}, m_export_path{};
        std::array<char, asset::QualifiedName::reference_domain.m_max_length + 1u> m_profile{};
        std::array<char, config::kMaxKeyBytes + 1u> m_biome{}, m_new_key{};
        std::array<char, 129u> m_property_search{};
    };

    struct WorldgenPanelView {
        const ContentSourceSet *m_source{};
        const ContentDescriptorCatalog *m_catalog{};
        const WorldgenPreviewResult *m_preview{};
        u64 m_revision{}, m_validated_revision{};
        std::string_view m_status{}, m_diagnostics{}, m_loaded_root{};
        std::span<const config::Diagnostic> m_property_diagnostics{};
        u64 m_diagnostics_revision{};
        const asset::PackSet *m_index{};
        bool m_dirty{}, m_busy{}, m_can_undo{}, m_can_redo{}, m_publishing{}, m_closing{};
        bool m_preview_current{}, m_can_export{}, m_draft_invalid{}, m_has_game{}, m_starting{}, m_can_start_game{};
    };

    [[nodiscard]] Array<WorkshopIntent> drawWorldgenPanel(WorldgenPanelState &state, const WorldgenPanelView &view);
}
