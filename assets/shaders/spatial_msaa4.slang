// Copyright 2025 John Hable. Adaptation under the MIT license; see
// third_party/HableMSAA/{LICENSE,README.md} and the preserved upstream source.
// Current-frame only: no exposure, gamut mapping, encoding or temporal history.
// Caller contract: finite RGBA16F source, admitted canonical 4x sample pattern
// (including its measured index permutation), exact 2x output extent, distinct
// source/output resources, and a writable RGBA16F output view.

struct SpatialParameters {
    uint2 source_extent;
    uint2 output_extent;
    uint4 sample_indices;
};
ConstantBuffer<SpatialParameters> g_spatial;
Texture2DMS<float4, 4> g_source;
RWTexture2D<float4> g_output;

float3 spatialSample(int2 pixel, uint canonical) {
    int2 bounded = clamp(pixel, int2(0), int2(g_spatial.source_extent) - 1);
    return g_source.Load(bounded, g_spatial.sample_indices[canonical]).rgb;
}

float3 spatialDiamond(float3 left, float3 right, float3 up, float3 down) {
    // Continuous weighting from Hable's later temporal article, used without
    // history here. This differs from the original spatial winner-takes-axis.
    float horizontal = max(1.0e-5, abs(dot(left - right, float3(0.25, 0.5, 0.25))));
    float vertical = max(1.0e-5, abs(dot(up - down, float3(0.25, 0.5, 0.25))));
    float weight = vertical / (horizontal + vertical);
    return (left + right) * (0.5 * weight) + (up + down) * (0.5 * (1.0 - weight));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resolveSpatialMsaa4(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_spatial.source_extent) ||
        any(g_spatial.output_extent != 2u * g_spatial.source_extent)) { return; }

    int2 p = int2(id.xy);
    float3 a0 = spatialSample(p + int2(0, -1), 0);
    float3 a1 = spatialSample(p + int2(0, -1), 1);
    float3 a2 = spatialSample(p + int2(0, -1), 2);
    float3 a3 = spatialSample(p + int2(0, -1), 3);
    float3 l0 = spatialSample(p + int2(-1, 0), 0);
    float3 l1 = spatialSample(p + int2(-1, 0), 1);
    float3 l2 = spatialSample(p + int2(-1, 0), 2);
    float3 l3 = spatialSample(p + int2(-1, 0), 3);
    float3 c0 = spatialSample(p, 0);
    float3 c1 = spatialSample(p, 1);
    float3 c2 = spatialSample(p, 2);
    float3 c3 = spatialSample(p, 3);
    float3 r0 = spatialSample(p + int2(1, 0), 0);
    float3 r1 = spatialSample(p + int2(1, 0), 1);
    float3 r2 = spatialSample(p + int2(1, 0), 2);
    float3 r3 = spatialSample(p + int2(1, 0), 3);
    float3 b0 = spatialSample(p + int2(0, 1), 0);
    float3 b1 = spatialSample(p + int2(0, 1), 1);
    float3 b2 = spatialSample(p + int2(0, 1), 2);
    float3 b3 = spatialSample(p + int2(0, 1), 3);

    // Grid coordinates are (row,column) in Hable's source. Each output bucket
    // contains its four distinct grid points, including the corrected lower
    // buckets; never repeat the diagonal pair from the upstream transcription.
    float3 g44 = spatialDiamond(l0, c0, a2, c2);
    float3 g45 = c0;
    float3 g46 = spatialDiamond(c0, r0, a3, c3);
    float3 g47 = spatialDiamond(c0, r0, a1, c1);
    float3 g54 = spatialDiamond(l1, c1, a2, c2);
    float3 g55 = spatialDiamond(l1, c1, c0, b0);
    float3 g56 = spatialDiamond(l1, c1, a3, c3);
    float3 g57 = c1;
    float3 g64 = c2;
    float3 g65 = spatialDiamond(c2, r2, c0, b0);
    float3 g66 = spatialDiamond(c2, r2, a3, c3);
    float3 g67 = spatialDiamond(c2, r2, c1, b1);
    float3 g74 = spatialDiamond(l3, c3, c2, b2);
    float3 g75 = spatialDiamond(l3, c3, c0, b0);
    float3 g76 = c3;
    float3 g77 = spatialDiamond(c3, r3, c1, b1);

    uint2 output = 2u * id.xy;
    g_output[output] = float4(0.25 * (g44 + g45 + g54 + g55), 1.0);
    g_output[output + uint2(1, 0)] = float4(0.25 * (g46 + g47 + g56 + g57), 1.0);
    g_output[output + uint2(0, 1)] = float4(0.25 * (g64 + g65 + g74 + g75), 1.0);
    g_output[output + uint2(1, 1)] = float4(0.25 * (g66 + g67 + g76 + g77), 1.0);
}
