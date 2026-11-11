// Backend qualification probes for the world-lighting storage, winner
// reduction, sample-rate and status contracts. Test-only: no runtime entry
// point consumes this module.

// Coarse interval record: stride 8, two words of two binary16 values.
struct CoarseInterval {
    half m_radiance_r;
    half m_radiance_g;
    half m_radiance_b;
    half m_transmittance;
};

// Fine probe record: stride 32 (DC word, shared FP32 AC scale, six AC words).
struct FineCH {
    uint m_dc;
    float m_scale;
    uint m_ac0;
    uint m_ac1;
    uint m_ac2;
    uint m_ac3;
    uint m_ac4;
    uint m_ac5;
};

// Word-for-word mirror of the published particle metadata row.
struct MetadataRow {
    uint m_id_low;
    uint m_id_high;
    uint m_source_row;
    uint m_material_temperature;
    float m_gas_x;
    float m_gas_y;
    float m_radius;
    uint m_physical_fill_bits;
};

// Winner scratch keyed by stable-ID row rank, not by numerical source row.
struct WinnerCandidate {
    uint m_cell;
    uint m_key;
    uint m_row;
};

struct ProbeParameters {
    uint2 m_extent;
    uint m_status_invalid_index;
    uint m_status_range_index;
    uint m_winner_cells;
    uint m_winner_candidates;
    float m_cells_per_pixel;
    uint m_reserved;
};

ConstantBuffer<ProbeParameters> g_probe;

// Jump storage stays an R8_UINT texel bound as uint: no filtering, no mips,
// no normalized conversion.
[format("r8ui")] RWTexture2D<uint> g_jump_write;
[format("r8ui")] Texture2D<uint> g_jump_read;
RWStructuredBuffer<uint> g_jump_out;

StructuredBuffer<WinnerCandidate> g_candidates;
RWStructuredBuffer<uint> g_best_key;
RWStructuredBuffer<uint> g_best_row;

StructuredBuffer<uint> g_keys;
StructuredBuffer<CoarseInterval> g_intervals;
StructuredBuffer<FineCH> g_fine_ch;
StructuredBuffer<MetadataRow> g_metadata;

Texture2DMS<float4, 4> g_msaa_color;
RWTexture2D<float4> g_sample_dump;
Texture2D<float4> g_history_prior;
RWTexture2D<float4> g_history_next;

RWStructuredBuffer<uint> g_status;
RWStructuredBuffer<uint> g_status_result;

// ------------------------------------------------------------------
// jump storage
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(16, 16, 1)]
void storeJumps(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_probe.m_extent)) return;
    uint values[7] = {0u, 1u, 2u, 4u, 8u, 16u, 32u};
    g_jump_write[id.xy] = values[(id.x + id.y * 3u) % 7u];
}

[shader("compute")]
[numthreads(16, 16, 1)]
void loadJumps(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_probe.m_extent)) return;
    g_jump_out[id.y * g_probe.m_extent.x + id.x] = g_jump_read[id.xy];
}

// ------------------------------------------------------------------
// winner reduction
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void clearWinners(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_probe.m_winner_cells) return;
    g_best_key[id.x] = 0xffffffffu;
    g_best_row[id.x] = 0xffffffffu;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void reduceWinnerKeys(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_probe.m_winner_candidates) return;
    WinnerCandidate candidate = g_candidates[id.x];
    InterlockedMin(g_best_key[candidate.m_cell], candidate.m_key);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void reduceWinnerRows(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_probe.m_winner_candidates) return;
    WinnerCandidate candidate = g_candidates[id.x];
    if (candidate.m_key == g_best_key[candidate.m_cell]) {
        InterlockedMin(g_best_row[candidate.m_cell], candidate.m_row);
    }
}

// ------------------------------------------------------------------
// sample-rate reconstruction
// ------------------------------------------------------------------

struct SampleVertexOutput {
    float4 m_position : SV_Position;
    float2 m_uv : TEXCOORD0;
};

[shader("vertex")]
SampleVertexOutput vertexSampleProbe(uint id : SV_VertexID) {
    float2 uv = float2((id << 1u) & 2u, id & 2u);
    SampleVertexOutput output;
    output.m_position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.m_uv = uv;
    return output;
}

[shader("fragment")]
float4 fragmentSamplePositions(SampleVertexOutput input, uint sample : SV_SampleIndex) : SV_Target {
    float2 uv = EvaluateAttributeAtSample(input.m_uv, sample);
    return float4(uv * float2(g_probe.m_extent), float(sample), 1.0);
}

[shader("fragment")]
float4 fragmentSampleCells(SampleVertexOutput input, uint sample : SV_SampleIndex) : SV_Target {
    float2 uv = EvaluateAttributeAtSample(input.m_uv, sample);
    float2 pixel = uv * float2(g_probe.m_extent);
    uint cell = uint(floor(pixel.x * g_probe.m_cells_per_pixel));
    bool even = (cell % 2u) == 0u;
    return float4(even ? 1.0 : 0.0, even ? 0.0 : 1.0, 0.0, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void dumpSamples(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_probe.m_extent)) return;
    for (uint index = 0u; index < 4u; ++index) {
        g_sample_dump[uint2(id.x * 4u + index, id.y)] = g_msaa_color.Load(int2(id.xy), index);
    }
}

// ------------------------------------------------------------------
// history and status
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void writeHistory(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_probe.m_extent)) return;
    g_history_next[id.xy] = float4(1.0, 0.5, 0.25, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void advanceHistory(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_probe.m_extent)) return;
    g_history_next[id.xy] = g_history_prior[id.xy] + float4(1.0, 0.0, 0.0, 0.0);
}

[shader("compute")]
[numthreads(8, 1, 1)]
void writeStatus(uint3 id : SV_DispatchThreadID) {
    if (id.x == g_probe.m_status_invalid_index) {
        InterlockedMin(g_status[0], id.x);
        g_status_result[id.x] = 0u;
        return;
    }
    if (id.x == g_probe.m_status_range_index) {
        InterlockedMin(g_status[1], id.x);
        g_status_result[id.x] = 0u;
        return;
    }
    g_status_result[id.x] = 1u;
}
