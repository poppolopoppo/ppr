// Reader for the WorldLightingPass::bind contract: the same resolved-cell,
// fine-CH, source-material, thermal-table and numeric-status names a
// material/inspection shader consumes.
#include "../world_lighting_codec.slang"
// Shared committed-ownership rule, also used by the terrain fragment.
#include "../grid_particle_native.slang"

struct BindProbe {
    uint cell;
    uint reserved[3];
};

ConstantBuffer<BindProbe> g_bind_probe;
StructuredBuffer<uint> g_resolved_cells;
StructuredBuffer<PackedCH> g_fine_ch;
StructuredBuffer<SourceMaterial> g_source_materials;
StructuredBuffer<float4> g_thermal_lut;
StructuredBuffer<uint> g_numeric_status;
RWStructuredBuffer<uint> g_bind_out;

[shader("compute")]
[numthreads(256, 1, 1)]
void probeBindings(uint3 id: SV_DispatchThreadID) {
    if (id.x != 0u) return;

    g_bind_out[0] = g_resolved_cells[g_bind_probe.cell];
    g_bind_out[1] = g_numeric_status[0];
    g_bind_out[2] = g_numeric_status[1];
    g_bind_out[3] = g_numeric_status[2];
}

// ------------------------------------------------------------------
// categorical base-retirement decision
// ------------------------------------------------------------------
// One record per probed native cell: the resolved word, the committed winner
// grid word and the ownership decision the terrain fragment makes. The pair of
// words proves the decision is the committed ownership grid alone: two cells may
// carry the same resolved material and still differ, because ownership never
// consults the material.
struct OwnershipProbe {
    uint count;
    uint reserved[3];
};
struct OwnershipProbeResult {
    uint resolved;
    uint best_row;
    uint owned;
    uint reserved;
};

ConstantBuffer<OwnershipProbe> g_ownership_probe;
StructuredBuffer<uint> g_ownership_cells;
StructuredBuffer<uint> g_best_row;
RWStructuredBuffer<OwnershipProbeResult> g_ownership_out;

[shader("compute")]
[numthreads(64, 1, 1)]
void probeOwnership(uint3 id: SV_DispatchThreadID) {
    if (id.x >= g_ownership_probe.count) return;

    uint cell = g_ownership_cells[id.x];
    OwnershipProbeResult result = {g_resolved_cells[cell], g_best_row[cell], 0u, 0u};
    result.owned = particleOwnsCell(cell, g_best_row) ? 1u : 0u;
    g_ownership_out[id.x] = result;
}
