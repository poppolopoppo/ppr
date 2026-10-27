// Test-only inspection of the production grid-contact operation.
#include "solver.slang"
#include "contact_reference.slang"

RWStructuredBuffer<int> g_contact_result;
// Explicitly test-only dense input to the actual production contact operation.
StructuredBuffer<int> g_gridSrc;

// ------------------------------------------------------------------
// lossless compact terrain decode against independent wide resources
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(64,1,1)]
void inspectCompactTerrain(uint lane : SV_GroupIndex) {
    uint failed = 0u;
    if (lane < 16u) {
        ContourCase actual = contourCase(lane);
        ContourCase expected = g_reference_cases[lane];
        if (actual.segmentCount != expected.segmentCount || actual.triangleCount != expected.triangleCount) failed |= 1u;
        for (uint index = 0u; index < 2u; ++index) {
            if (actual.segments[index].begin != expected.segments[index].begin ||
                actual.segments[index].end != expected.segments[index].end) failed |= 2u;
        }

        for (uint index = 0u; index < 3u; ++index) {
            ContourTriangle a = actual.triangles[index], b = expected.triangles[index];
            if (a.a != b.a || a.b != b.b || a.c != b.c || a.solidCorners != b.solidCorners) failed |= 4u;
        }
    }

    if (lane < 8u && any(asuint(contourPoint(lane)) != asuint(referenceContourPoint(lane)))) failed |= 8u;

    uint cells = g_environment.terrainWidth * g_environment.terrainHeight;
    for (uint index = lane; index < cells; index += 64u) {
        uint actual = (g_terrain_masks[index >> 3u] >> (4u * (index & 7u))) & 15u;
        if (actual != g_reference_masks[index]) failed |= 16u;
    }

    uint columns = (g_environment.terrainWidth + 7u) / 8u;
    uint rows = (g_environment.terrainHeight + 7u) / 8u;
    if (lane == 0u) {
        if ((cells & 7u) != 0u && (g_terrain_masks[cells >> 3u] >> (4u * (cells & 7u))) != 0u) failed |= 64u;
        uint tiles = columns * rows;
        if ((tiles & 31u) != 0u && (g_terrain_boundary_tiles[tiles >> 5u] >> (tiles & 31u)) != 0u) failed |= 128u;
    }

    for (uint tile = lane; tile < columns * rows; tile += 64u) {
        int2 cell = int2((tile % columns) * 8u, (tile / columns) * 8u);
        bool expected = false;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                int2 at = cell + int2(x,y);
                if (terrainCellValid(at)) expected = expected || referenceTerrainCase(at).segmentCount != 0u;
            }
        }

        if (terrainTileHasBoundary(cell) != expected) failed |= 32u;
    }

    g_contact_result[lane] = int(failed);
}

// ------------------------------------------------------------------
// analytical grid-contact probes
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(1,1,1)]
void compareTerrainSearch(uniform float2 g_contact_query) {
    Contact actual = terrainContact(g_contact_query, 0xffffffffu, 0u);
    Contact expected = referenceTerrainContact(g_contact_query, 0xffffffffu, 0u);
    g_contact_result[0] = actual.collides ? 1 : 0;
    g_contact_result[1] = asint(actual.penetration);
    g_contact_result[2] = asint(actual.normal.x);
    g_contact_result[3] = asint(actual.normal.y);
    g_contact_result[4] = asint(actual.point.x);
    g_contact_result[5] = asint(actual.point.y);
    g_contact_result[6] = expected.collides ? 1 : 0;
    g_contact_result[7] = asint(expected.penetration);
    g_contact_result[8] = asint(expected.normal.x);
    g_contact_result[9] = asint(expected.normal.y);
    g_contact_result[10] = asint(expected.point.x);
    g_contact_result[11] = asint(expected.point.y);
}

[shader("compute")]
[numthreads(1,1,1)]
void inspectContact() {
    uint base=(7u*32u+6u)*4u;
    int4 source=int4(g_gridSrc[base],g_gridSrc[base+1],g_gridSrc[base+2],g_gridSrc[base+3]);
    float4 state=contactGridState(int2(6,7),base/4u,source);
    g_contact_result[0]=checkedEncodeNode(state.x,base/4u,0u);
    g_contact_result[1]=checkedEncodeNode(state.y,base/4u,1u);
    g_contact_result[2]=checkedEncodeNode(state.z,base/4u,2u);
    g_contact_result[3]=checkedEncodeNode(state.w,base/4u,3u);
}

// Both paths use the production error helpers with a mixed full-group cohort.
// The candidate compares error acceptance, not its deliberately unused values.
[shader("compute")]
[numthreads(64,1,1)]
void compareBootstrapChecks(uint lane : SV_GroupIndex) {
    uint base = (7u * 32u + 6u) * 4u;
    int4 source = int4(g_gridSrc[base], g_gridSrc[base+1u], g_gridSrc[base+2u], g_gridSrc[base+3u]);
    bool valid = (lane & 1u) == 0u;
    bool certified = bootstrapGridCertified();
    if (certified) {
        checkBootstrapGridCoverage(int2(6,7), base/4u, source, valid);
    } else {
        float4 state = contactGridState<true>(int2(6,7), base/4u, source, valid);
        if (valid) {
            checkedEncodeNode(state.x, base/4u, 0u);
            checkedEncodeNode(state.y, base/4u, 1u);
            checkedEncodeNode(state.z, base/4u, 2u);
            checkedEncodeNode(state.w, base/4u, 3u);
        }
    }

    DeviceMemoryBarrierWithGroupSync();
    if (lane == 0u) {
        for (uint field = 0u; field < 6u; ++field) {
            g_contact_result[field] = int(g_errors[field]);
            g_errors[field] = 0u;
        }
    }

    DeviceMemoryBarrierWithGroupSync();
    float4 state = contactGridState<true>(int2(6,7), base/4u, source, valid);
    if (valid) {
        checkedEncodeNode(state.x, base/4u, 0u);
        checkedEncodeNode(state.y, base/4u, 1u);
        checkedEncodeNode(state.z, base/4u, 2u);
        checkedEncodeNode(state.w, base/4u, 3u);
    }

    DeviceMemoryBarrierWithGroupSync();
    if (lane == 0u) {
        for (uint field = 0u; field < 6u; ++field)
            g_contact_result[6u + field] = int(g_errors[field]);
        g_contact_result[12u] = certified ? 1 : 0;
    }
}

// ------------------------------------------------------------------
// full-group mixed eligibility and exact wave-search comparisons
// ------------------------------------------------------------------

StructuredBuffer<float2> g_wave_query_points;
StructuredBuffer<uint> g_wave_query_enabled;
RWStructuredBuffer<uint> g_wave_query_results;

[shader("compute")]
[numthreads(64,1,1)]
void compareWaveTerrainSearch(uint lane : SV_GroupIndex) {
    float2 point = g_wave_query_points[lane];
    bool enabled = g_wave_query_enabled[lane] != 0u;
    Contact actual = terrainContactWave(point, enabled, lane, lane);
    DeviceMemoryBarrierWithGroupSync();
    if (lane == 0u) {
        for (uint field = 0u; field < 6u; ++field) {
            g_wave_query_results[64u * 13u + field] = g_errors[field];
            g_errors[field] = 0u;
        }
    }

    DeviceMemoryBarrierWithGroupSync();
    Contact expected = {false, 0.0, float2(0.0), float2(0.0)};
    if (enabled) expected = referenceTerrainContact(point, lane, lane);

    uint at = lane * 13u;
    g_wave_query_results[at] = actual.collides ? 1u : 0u;
    g_wave_query_results[at + 1u] = asuint(actual.penetration);
    g_wave_query_results[at + 2u] = asuint(actual.normal.x);
    g_wave_query_results[at + 3u] = asuint(actual.normal.y);
    g_wave_query_results[at + 4u] = asuint(actual.point.x);
    g_wave_query_results[at + 5u] = asuint(actual.point.y);
    g_wave_query_results[at + 6u] = expected.collides ? 1u : 0u;
    g_wave_query_results[at + 7u] = asuint(expected.penetration);
    g_wave_query_results[at + 8u] = asuint(expected.normal.x);
    g_wave_query_results[at + 9u] = asuint(expected.normal.y);
    g_wave_query_results[at + 10u] = asuint(expected.point.x);
    g_wave_query_results[at + 11u] = asuint(expected.point.y);
    g_wave_query_results[at + 12u] = WaveGetLaneCount();

    DeviceMemoryBarrierWithGroupSync();
    if (lane == 0u) {
        for (uint field = 0u; field < 6u; ++field)
            g_wave_query_results[64u * 13u + 6u + field] = g_errors[field];
    }
}

// ------------------------------------------------------------------
// exact node conversion and unchanged scatter-conversion rejection
// ------------------------------------------------------------------
StructuredBuffer<uint> g_conversion_inputs;
RWStructuredBuffer<uint> g_conversion_results;

[shader("compute")]
[numthreads(1,1,1)]
void inspectNodeConversions(uniform uint g_count) {
    // One invocation isolates each case's ordinary production error sink.
    for (uint index=0u;index<g_count;++index) {
        for (uint word=0u;word<6u;++word) g_errors[word]=0u;

        float value=asfloat(g_conversion_inputs[index]);
        uint at=index*12u;
        g_conversion_results[at]=asuint(checkedEncodeNode(value,index,0u));
        for (uint word=0u;word<6u;++word) g_conversion_results[at+1u+word]=g_errors[word];

        for (uint word=0u;word<6u;++word) g_errors[word]=0u;

        g_conversion_results[at+7u]=asuint(checkedEncode(value,index,0u));
        g_conversion_results[at+8u]=g_errors[0];
        g_conversion_results[at+9u]=g_errors[1];
        g_conversion_results[at+10u]=g_errors[2];
        g_conversion_results[at+11u]=g_errors[4];
    }
}
