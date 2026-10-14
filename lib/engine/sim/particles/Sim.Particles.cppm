module;
export module engine.sim:particles;
import :chunk_grid;
import :worldgen.matter;
import :worldgen.plan;
import engine.core;
import std;

export namespace pP::sim {
    // ------------------------------------------------------------------
    // particle vocabulary
    // ------------------------------------------------------------------

    // At most 64 MiB of logical live records; explicit encoding uses 59 MiB.
    // Frozen snapshots may retain previous batches across local edits.
    // Allocate only occupied batches, never this maximum up front.
    inline constexpr u32 kMaxMatterParticles = 1024u * 1024u;
    struct MatterParticle {
        u64 m_id{};
        double m_x_m{}, m_y_m{}, m_volume_m3{}, m_mass_kg{}, m_temperature_k{}, m_bulk_density_kg_m3{};
        u16 m_material{};
        EGenerationMatter m_phase{};
        std::array<u8, 5u> m_reserved{};
        constexpr bool operator==(const MatterParticle &) const noexcept = default;
    };
    static_assert(sizeof(MatterParticle) == 64u);
    static_assert(std::is_trivially_copyable_v<MatterParticle>);

    // Immutable shared ownership lets frozen frame/checkpoint packets retain a
    // batch across an edit without copying all world particles each frame.
    struct MatterParticleBatch {
        ChunkPos m_position{};
        std::shared_ptr<const Array<MatterParticle>> m_particles{};
        [[nodiscard]] std::span<const MatterParticle> records() const noexcept;
    };
    struct MatterParticleSnapshot {
        u64 m_next_id{1u};
        Array<MatterParticleBatch> m_chunks{};
    };

    // ------------------------------------------------------------------
    // seeding + validation
    // ------------------------------------------------------------------

    [[nodiscard]] Expected<MatterParticleSnapshot> seedMatterParticles(
        const InitialMatter &matter, const ChunkGrid &grid, const GenerationRecipe &recipe,
        const SharedContext &context = nullptr);
    [[nodiscard]] std::error_code validateMatterParticles(
        const MatterParticleSnapshot &snapshot, std::span<const GenerationMaterial> materials, double cell_size_m,
        const SharedContext &context = nullptr, double reference_thickness_m = 1.0);

    // ------------------------------------------------------------------
    // particle state
    // ------------------------------------------------------------------

    class MatterParticleState {
        std::array<std::shared_ptr<const Array<MatterParticle>>, kChunkCount> m_chunks{};
        u64 m_next_id{1u};
        u32 m_count{};

    public:
        [[nodiscard]] std::error_code restore(
            const MatterParticleSnapshot &snapshot, std::span<const GenerationMaterial> materials, double cell_size_m,
            const SharedContext &context = nullptr, double reference_thickness_m = 1.0);
        [[nodiscard]] MatterParticleSnapshot capture() const;
        [[nodiscard]] std::span<const MatterParticle> chunk(ChunkPos position) const noexcept;
        [[nodiscard]] u32 count() const noexcept;
        // The live grid is original; captured chunks contain the complete edit
        // candidate. Reevaluate the one-cell contour halo, removing point
        // particles only where newly added terrain covers their center.
        [[nodiscard]] Expected<Array<MatterParticleBatch>> prepareTerrainEdit(
            const ChunkGrid &original, std::span<const ChunkGrid::CapturedChunk> candidate, double cell_size_m) const;
        void commitTerrainEdit(std::span<const MatterParticleBatch> batches) noexcept;
    };
}
