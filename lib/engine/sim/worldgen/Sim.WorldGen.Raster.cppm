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
        GenerationRaster(
            const GenerationPlan &, const BiomeLayout &, const GenerationGeometry &, Array<Array<u32>>,
            Array<Array<u32>>) noexcept;
        friend Expected<GenerationRaster> compileGenerationRaster(
            const GenerationPlan &, const BiomeLayout &, const GenerationGeometry &, const IContext *);

    public:
        GenerationRaster(GenerationRaster &&) noexcept = default;
        GenerationRaster(const GenerationRaster &) = delete;
        GenerationRaster &operator=(const GenerationRaster &) = delete;
        [[nodiscard]] Expected<Array<Cell>> chunk(ChunkPos position, const IContext *context = nullptr) const;
        [[nodiscard]] u64 indexPayloadBytes() const noexcept;
    };
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
