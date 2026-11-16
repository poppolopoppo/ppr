#include "../grid_background_coverage.slang"

// ------------------------------------------------------------------
// production background coverage predicate readback
// ------------------------------------------------------------------
StructuredBuffer<float2> g_positions;
RWStructuredBuffer<uint> g_results;

[shader("compute")]
[numthreads(10, 1, 1)]
void probeBackgroundCoverage(uint3 id : SV_DispatchThreadID) {
    g_results[id.x] = backgroundCoveredByOpaqueTerrain(g_positions[id.x]) ? 1u : 0u;
}
