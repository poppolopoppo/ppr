// Test-only decode probes for the shared world-lighting codecs. The production
// module is ../world_lighting_codec.slang; no runtime entry point consumes this
// file.

#include "../world_lighting_codec.slang"

struct CodecProbeParameters {
    uint m_count;
    uint m_reserved0;
    uint m_reserved1;
    uint m_reserved2;
};

ConstantBuffer<CodecProbeParameters> g_codec_probe;

// Reflected element layouts for the published records.
StructuredBuffer<PackedInterval> g_codec_intervals;
StructuredBuffer<PackedCH> g_codec_fine_ch;
StructuredBuffer<SourceMaterial> g_codec_source_materials;
StructuredBuffer<uint> g_codec_cells;
StructuredBuffer<uint> g_codec_rgb9e5;

RWStructuredBuffer<float4> g_codec_interval_out;
RWStructuredBuffer<float4> g_codec_ch_out;
RWStructuredBuffer<float4> g_codec_source_emission_out;
RWStructuredBuffer<float4> g_codec_source_diffuse_out;
RWStructuredBuffer<float2> g_codec_cell_out;
RWStructuredBuffer<float4> g_codec_rgb9e5_out;
StructuredBuffer<float4> g_codec_category_inputs;
RWStructuredBuffer<uint> g_codec_category_out;

[shader("compute")]
[numthreads(64, 1, 1)]
void decodeRecords(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_codec_probe.m_count) return;

    Interval interval = unpackInterval(g_codec_intervals[id.x]);
    g_codec_interval_out[id.x] = float4(interval.m_radiance, interval.m_transmittance);

    CH ch = unpackCH(g_codec_fine_ch[id.x]);
    for (uint coefficient = 0u; coefficient < 9u; ++coefficient) {
        g_codec_ch_out[id.x * 9u + coefficient] = float4(ch.m_coefficients[coefficient], 0.0);
    }

    SourceMaterial material = g_codec_source_materials[id.x];
    g_codec_source_emission_out[id.x] = float4(unpackSourceEmission(material), 0.0);
    g_codec_source_diffuse_out[id.x] = float4(unpackSourceDiffuse(material), 0.0);

    uint cell = g_codec_cells[id.x];
    g_codec_cell_out[id.x] = float2(float(unpackCellMaterial(cell)), unpackCellTemperature(cell));

    g_codec_rgb9e5_out[id.x] = float4(unpackRGB9E5(g_codec_rgb9e5[id.x]), 0.0);
}

// Encode-category boundary probe: interval radiance 65,504 is the FP16 maximum
// and must encode; a larger value is a representational overflow.
[shader("compute")]
[numthreads(64, 1, 1)]
void intervalCategories(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_codec_probe.m_count) return;

    float4 value = g_codec_category_inputs[id.x];
    Interval interval;
    interval.m_radiance = value.xyz;
    interval.m_transmittance = value.w;
    g_codec_category_out[id.x] = intervalPackCategory(interval);
}
