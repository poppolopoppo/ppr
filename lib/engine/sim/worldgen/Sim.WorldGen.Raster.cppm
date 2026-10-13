module;
export module engine.sim:worldgen.raster;
import :chunk_grid;
import :worldgen.geometry;
import :worldgen;
import engine.core;
import std;

export namespace pP::sim {
    inline constexpr u32 kMaxGenerationRasterReferences = 16u * 1024u * 1024u;
    /// Borrowed immutable inputs must outlive this index and every raster task.
    /// Each call owns at most one chunk payload and never mutates those inputs.
    class GenerationRaster {
        const GenerationPlan &m_plan;
        const BiomeLayout &m_layout;
        const GenerationGeometry &m_geometry;
        Array<Array<u32>> m_segments;
        Array<Array<u32>> m_instances;
        Array<Array<u32>> m_blobs;
        GenerationRaster(
            const GenerationPlan &, const BiomeLayout &, const GenerationGeometry &, Array<Array<u32>>,
            Array<Array<u32>>, Array<Array<u32>>) noexcept;
        friend Expected<GenerationRaster> compileGenerationRaster(
            const GenerationPlan &, const BiomeLayout &, const GenerationGeometry &, const IContext *);

    public:
        GenerationRaster(GenerationRaster &&) noexcept = default;
        GenerationRaster(const GenerationRaster &) = delete;
        GenerationRaster &operator=(const GenerationRaster &) = delete;
        [[nodiscard]] Expected<Array<Cell>> chunk(ChunkPos position, const IContext *context = nullptr) const;
        [[nodiscard]] bool protectedCell(u32 x, u32 y) const noexcept;
        [[nodiscard]] u64 indexPayloadBytes() const noexcept;
        [[nodiscard]] u64 chunkWorkingPayloadBytes() const noexcept;
    };
    [[nodiscard]] std::error_code validateGenerationBackbone(
        const GenerationPlan &plan, const GenerationGeometry &geometry, const ChunkGrid &grid,
        const IContext *context = nullptr);
    [[nodiscard]] std::error_code finishGenerationTerrain(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry, ChunkGrid &grid,
        const IContext *context = nullptr, u64 *known_scratch_peak = nullptr);
    /// Half-open global fine-cell bounds.
    struct GenerationCellBounds {
        u32 m_left{}, m_bottom{}, m_right{}, m_top{};
        constexpr bool operator==(const GenerationCellBounds &) const noexcept = default;
    };
    /// Applies shared frozen-contact formations over a fixed tile-aligned root
    /// domain. Requires resident support through the longest footprint plus its
    /// validation ring. No whole-world backbone or playability validation runs.
    [[nodiscard]] std::error_code finishGenerationTerrainRegion(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry, ChunkGrid &grid,
        GenerationCellBounds candidates, const IContext *context = nullptr, u64 *known_scratch_peak = nullptr);
    [[nodiscard]] Expected<GenerationRaster> compileGenerationRaster(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry,
        const IContext *context = nullptr);
    /// Runs on a background coordinator or an explicit offline caller. Every
    /// admitted task is drained before returning, including on failure.
    /// The returned grid owns all chunk payloads; no live world is mutated.
    [[nodiscard]] Expected<ChunkGrid> rasterizeGenerationTerrain(
        const GenerationPlan &plan, const BiomeLayout &layout, const GenerationGeometry &geometry,
        const GenerationExecution &execution = {}, u64 *known_scratch_peak = nullptr);
}
