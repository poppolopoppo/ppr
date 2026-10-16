module;
export module game.colony.worldgen_preview;
import engine.core;
import engine.sim;
import engine.asset;
import game.colony.content;
import game.colony.metric;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // preview constants
    // ------------------------------------------------------------------

    inline constexpr u32 kWorldgenPreviewEdge = 256u;
    inline constexpr u32 kWorldgenPreviewMaxResidentChunks = 25u;
    enum class EWorldgenPreviewMode : u8 {
        biome_sample,
        whole_world_crop
    };
    struct WorldgenPreviewSpec {
        std::string m_profile{}, m_biome{};
        u64 m_seed{}, m_revision{};
        sim::GlobalCellPos m_origin{1920u, 1920u};
        EWorldgenPreviewMode m_mode{EWorldgenPreviewMode::biome_sample};
        bool operator==(const WorldgenPreviewSpec &) const noexcept = default;
    };

    // ------------------------------------------------------------------
    // preview world
    // ------------------------------------------------------------------

    /// Immutable production generation, prepared once in the background. The
    /// global formation admission cap requires this whole-world preparation.
    struct WorldgenPreviewWorld {
        std::string m_profile;
        u64 m_content_fingerprint{};
        sim::GeneratedWorld m_world;
        sim::MatterParticleSnapshot m_particles;
        struct MatterSpan {
            u32 m_region{};
            sim::InitialMatterSpan m_span;
        };
        std::array<Array<MatterSpan>, sim::kChunkCount> m_matter_chunks;
        u64 m_known_retained_payload_bytes{}, m_known_preparation_scratch_peak_bytes{};
        [[nodiscard]] bool matches(const ContentCatalog &catalog, const WorldgenPreviewSpec &spec) const noexcept;
    };

    // ------------------------------------------------------------------
    // preview world preparation
    // ------------------------------------------------------------------

    [[nodiscard]] Expected<std::shared_ptr<const WorldgenPreviewWorld>> prepareWorldgenPreviewWorld(
        const ContentCatalog &catalog, const WorldgenPreviewSpec &spec, const ITimerClock &clock,
        const SharedContext &context = {}, ParallelScheduler *generation_pool = nullptr);

    // ------------------------------------------------------------------
    // preview result
    // ------------------------------------------------------------------

    /// A bounded biome sample or an exact crop of validated production terrain.
    /// Support chunks are owned and globally anchored; camera movement is external.
    struct WorldgenPreviewResult {
        std::shared_ptr<const ContentCatalog> m_catalog;
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
        sim::InitialMatter m_matter;
        u64 m_known_world_cache_payload_bytes{};
        std::shared_ptr<const WorldgenPreviewWorld> m_exact_world;
        sim::MatterParticleSnapshot m_particles;
        std::shared_ptr<const ContentPresentation> m_presentation;
        /// Request attempts and draft revisions have independent identities.
        asset::RequestGeneration m_presentation_generation{};

        [[nodiscard]] std::error_code attachPresentation(
            std::shared_ptr<const ContentPresentation> presentation, asset::RequestGeneration expected_generation);

        [[nodiscard]] const sim::GenerationGeometry &geometry() const noexcept;
    };

    // ------------------------------------------------------------------
    // preview preparation
    // ------------------------------------------------------------------

    /// Blocking CPU work for an offline caller or background coordinator.
    /// On failure/cancellation the caller's previous result is untouched.
    [[nodiscard]] Expected<WorldgenPreviewResult> prepareWorldgenPreview(
        ContentCatalog catalog, WorldgenPreviewSpec spec, const SharedContext &context = {},
        std::shared_ptr<const WorldgenPreviewWorld> world = {});
    [[nodiscard]] Expected<WorldgenPreviewResult> prepareWorldgenPreview(
        std::shared_ptr<const ContentCatalog> catalog, WorldgenPreviewSpec spec, const SharedContext &context = {},
        std::shared_ptr<const WorldgenPreviewWorld> world = {});

    // ------------------------------------------------------------------
    // preview display
    // ------------------------------------------------------------------

    struct WorldgenPreviewDisplay {
        double m_center_x_m{}, m_center_y_m{}, m_zoom{1.0};
        u32 m_view_mode{};
    };

    // ------------------------------------------------------------------
    // preview export
    // ------------------------------------------------------------------

    /// Fresh directory only. Completion manifest is written last; failures may
    /// leave partial output. Source must be the exact snapshot used by the result.
    [[nodiscard]] std::error_code exportWorldgenPreview(
        const WorldgenPreviewResult &preview, const ContentSourceSet &source, const WorldgenPreviewDisplay &display,
        const std::filesystem::path &directory, const IContext *context = nullptr);
}
