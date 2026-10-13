module;
export module game.colony.worldgen_panel;
import engine.app;
import engine.core;
import engine.sim;
import game.colony.content;
import game.colony.worldgen_preview;
import std;

export namespace pP::colony {
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
        edit_biome
    };
    struct WorkshopIntent {
        EWorkshopIntent m_kind{};
        std::string m_biome{}, m_property{};
        config::FileValue m_value{};
    };
    struct WorldgenPanelState {
        std::array<char, 1024u> m_root{}, m_export_path{};
        std::array<char, 129u> m_profile{}, m_biome{}, m_search{};
        u64 m_seed{42u};
        int m_origin[2]{1920, 1920};
        EGridDiagnosticMode m_mode{EGridDiagnosticMode::textured};
        std::string m_selected_category{"biomes"}, m_selected_key{};
        std::string m_edit_key{};
        std::array<char, 1024u> m_text_edit{};
        std::array<config::Document, kContentDocumentCount> m_documents{};
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
        std::string_view m_status{}, m_diagnostics{};
    };
    [[nodiscard]] Array<WorkshopIntent> drawWorldgenPanel(WorldgenPanelState &state, const WorldgenPanelView &view);
}
