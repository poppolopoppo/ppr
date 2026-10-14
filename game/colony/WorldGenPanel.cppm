module;
export module game.colony.worldgen_panel;
import engine.app;
import engine.core;
import engine.sim;
import game.colony.content;
import game.colony.worldgen_preview;
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
        std::array<char, 1024u> m_root{}, m_export_path{};
        std::array<char, 129u> m_profile{}, m_biome{}, m_search{};
        std::array<char, 129u> m_property_search{};
        u64 m_seed{42u};
        int m_origin[2]{1920, 1920};
        EGridDiagnosticMode m_mode{EGridDiagnosticMode::textured};
        EWorldgenPreviewMode m_preview_mode{EWorldgenPreviewMode::biome_sample};
        bool m_automatic_live{};
        std::array<char, 129u> m_new_key{};
        std::string m_selected_category{"biomes"}, m_selected_key{};
        std::string m_edit_key{};
        std::string m_numeric_edit_key{};
        config::FileValue m_numeric_edit{};
        std::array<char, 1024u> m_text_edit{};
        std::array<config::Document, kContentDocumentCount> m_documents{};
        std::array<Array<std::string>, kContentDocumentCount> m_record_keys{};
        Array<PropertyNode> m_property_tree{};
        std::string m_tree_selection{}, m_focus_property{};
        bool m_show_properties{}, m_show_diagnostics{};
        u64 m_document_revision{};
        u64 m_document_validated_revision{};
        const ContentPackSource *m_document_source{};
        const ContentCatalog *m_document_catalog{};
    };

    struct WorldgenPanelView {
        const ContentPackSource *m_source{};
        const ContentCatalog *m_catalog{};
        const WorldgenPreviewResult *m_preview{};
        u64 m_revision{}, m_validated_revision{};
        bool m_dirty{}, m_busy{}, m_can_undo{}, m_can_redo{}, m_publishing{}, m_closing{};
        bool m_preview_current{}, m_can_export{};
        bool m_draft_invalid{};
        std::string_view m_status{}, m_diagnostics{};
        std::span<const config::Diagnostic> m_property_diagnostics{};
        u64 m_diagnostics_revision{};
    };

    [[nodiscard]] Array<WorkshopIntent> drawWorldgenPanel(WorldgenPanelState &state, const WorldgenPanelView &view);
}
