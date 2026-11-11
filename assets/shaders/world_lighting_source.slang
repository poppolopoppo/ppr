// ------------------------------------------------------------------
// cached outgoing-radiance source
// ------------------------------------------------------------------
// One RGB9E5 word per native cell, rebuilt once per accepted lighting frame
// before any tracing. The source is the only radiance a transport hit returns;
// coarse and fine tracing never evaluate a BRDF, normal, roughness or Fresnel
// factor. Diffuse feedback samples the prior fine CH DC field at the native
// cell centre with the approved isometric normalisation.
#include "thermal_emission.slang"
#include "world_lighting_codec.slang"
#include "numeric_status.slang"

struct SourceFrame {
    uint m_edge;
    uint m_fine_edge;
    uint m_status_record;
    uint m_reserved;
};

ConstantBuffer<SourceFrame> g_source_frame;
StructuredBuffer<uint> g_resolved_cells;
StructuredBuffer<TerrainVisual> g_visuals;
StructuredBuffer<SourceMaterial> g_source_materials;
StructuredBuffer<PackedCH> g_prior_ch;
RWStructuredBuffer<uint> g_cell_source;

// Y0 = 1/sqrt(2*pi): the angular mean of a circular-harmonic field is C0*Y0.
static const float kY0 = 0.39894228040143267794;

// Bilinear DC sample at the native cell centre. Fine probe centres sit at
// (probe+0.5)*2 native cells, so the fine coordinate is (cell+0.5)/2-0.5.
// Clamped edge indices keep their ordinary bilinear weight, whose sum is one.
float3 priorAngularMean(uint2 cell) {
    float2 fine = (float2(cell) + 0.5) * 0.5 - 0.5;
    int2 base = int2(floor(fine));
    float2 fraction = fine - float2(base);
    int2 last = int2(int(g_source_frame.m_fine_edge) - 1);

    float3 result = float3(0.0);
    for (uint tap = 0u; tap < 4u; ++tap) {
        int2 offset = int2(int(tap % 2u), int(tap / 2u));
        float weight = (offset.x == 0 ? 1.0 - fraction.x : fraction.x) *
                       (offset.y == 0 ? 1.0 - fraction.y : fraction.y);
        if (weight <= 0.0) continue;

        int2 index = clamp(base + offset, int2(0), last);
        PackedCH record = g_prior_ch[index.y * int(g_source_frame.m_fine_edge) + index.x];
        result += unpackRGB9E5(record.m_dc) * weight;
    }

    return result * kY0;
}

// ------------------------------------------------------------------
// full-domain source preparation
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(16, 16, 1)]
void prepareSource(uint2 cell : SV_DispatchThreadID) {
    if (cell.x >= g_source_frame.m_edge || cell.y >= g_source_frame.m_edge) return;

    uint index = cell.y * g_source_frame.m_edge + cell.x;
    uint resolved = g_resolved_cells[index];
    uint material = resolved & 0xffffu;

    // Empty cells and receive-only transparent surfaces (including gas) neither
    // emit nor diffuse; their thermal transport is not evaluated at all.
    if (material == 0u) {
        g_cell_source[index] = 0u;
        return;
    }

    TerrainVisual visual = g_visuals[material];
    bool receive_only =
        (visual.surfaceFlags & kOpticalBlendSurfaceFlag) != 0u && visual.emissiveStrength == 0.0;
    if (receive_only) {
        g_cell_source[index] = 0u;
        return;
    }

    SourceMaterial material_source = g_source_materials[material];
    float3 emission = unpackSourceEmission(material_source);
    float3 diffuse = unpackSourceDiffuse(material_source);
    float3 thermal = thermalRGB(unpackCellTemperature(resolved));
    float3 previous = priorAngularMean(cell);

    float3 value = emission + thermal + diffuse * previous;
    uint category = rgb9e5PackCategory(value);
    if (category != 0u) {
        numericRecordFailure(
            g_source_frame.m_status_record, category == 1u ? kNumericInvalidState : kNumericRangeError, index);
        g_cell_source[index] = 0u;
        return;
    }

    g_cell_source[index] = packRGB9E5(value);
}
