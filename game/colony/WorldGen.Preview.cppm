module;
export module game.colony.worldgen_preview;
import engine.core;
import engine.sim;
import game.colony.content;
import game.colony.metric;
import std;

export namespace pP::colony {
    inline constexpr u32 kWorldgenPreviewEdge = 256u;
    inline constexpr u32 kWorldgenPreviewMaxResidentChunks = 25u;
    struct WorldgenPreviewSpec {
        std::string m_profile{}, m_biome{};
        u64 m_seed{}, m_revision{};
        sim::GlobalCellPos m_origin{1920u, 1920u};
        bool operator==(const WorldgenPreviewSpec &) const noexcept = default;
    };
    /// A terrain-only Biome sample, never a complete or playable generated world.
    /// Support chunks are owned and globally anchored; camera movement is external.
    struct WorldgenPreviewResult {
        ContentCatalog m_catalog;
        WorldgenPreviewSpec m_spec;
        sim::GenerationPlan m_plan;
        sim::BiomeLayout m_layout;
        sim::GenerationGeometry m_geometry;
        sim::ChunkGrid m_grid;
        sim::GenerationCellBounds m_visible{}, m_support{}, m_formation_domain{};
        u32 m_biome_index{}, m_filter_halo_fine{};
        u64 m_checksum{}, m_solid_cells{}, m_known_scratch_peak_bytes{}, m_known_retained_payload_bytes{};
        Array<sim::GenerationMaterialCount> m_material_counts;
        Array<std::string> m_participating_rules;
        Array<std::string> m_omissions;
    };
    /// Blocking CPU work for an offline caller or background coordinator.
    /// On failure/cancellation the caller's previous result is untouched.
    [[nodiscard]] Expected<WorldgenPreviewResult> prepareWorldgenPreview(
        ContentCatalog catalog, WorldgenPreviewSpec spec, const IContext *context = nullptr);
    struct WorldgenPreviewDisplay {
        double m_center_x_m{}, m_center_y_m{}, m_zoom{1.0};
        u32 m_view_mode{};
    };
    /// Fresh directory only. Completion manifest is written last; failures may
    /// leave partial output. Source must be the exact snapshot used by the result.
    [[nodiscard]] std::error_code exportWorldgenPreview(
        const WorldgenPreviewResult &preview, const ContentPackSource &source, const WorldgenPreviewDisplay &display,
        const std::filesystem::path &directory, const IContext *context = nullptr);
}
