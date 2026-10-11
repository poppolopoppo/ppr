module;
export module engine.sim:worldgen.result;
import :worldgen.raster;
import :worldgen.matter;
import :worldgen.start;
import :worldgen.plan;
import :worldgen.layout;
import :worldgen.geometry;
import :worldgen;
import :chunk_grid;
import engine.core;
import std;

export namespace pP::sim {
    struct GenerationMaterialCount {
        u16 m_material{};
        u64 m_cells{};
    };
    struct GenerationReport {
        // Layout, nonlocal geometry, raster, matter, final validation/hash.
        std::array<TimeSpan, 5u> m_phase_times{};
        Array<GenerationMaterialCount> m_material_counts;
        u64 m_terrain_checksum{}, m_output_checksum{}, m_grid_payload_bytes{};
        // Known vector payload capacities (raster chunk buffers use a live-task bound).
        // Excludes allocator/reallocation overhead, opaque noise/task allocations and stacks.
        std::array<u64, 5u> m_known_phase_scratch_bytes{};
        u64 m_retained_known_payload_bytes{};
    };
    /// Entire numeric candidate, including inputs needed for later inspection.
    /// No catalog/parser storage or executor is borrowed by the result.
    struct GeneratedWorld {
        GenerationPlan m_plan;
        BiomeLayout m_layout;
        GenerationGeometry m_geometry;
        ChunkGrid m_grid;
        InitialMatter m_matter;
        GenerationStart m_start;
        GenerationReport m_report;
    };
    /// Offline caller or background coordinator only. Clock is explicit and
    /// affects report timings only, never generation or persistent checksums.
    [[nodiscard]] Expected<GeneratedWorld> generateWorld(
        GenerationPlan plan, const ITimerClock &clock, const GenerationExecution &execution = {});
}
