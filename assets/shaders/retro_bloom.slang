// ------------------------------------------------------------------
// quarter-output scene-linear bloom protocol
// ------------------------------------------------------------------

struct BloomParameters {
    uint2 source_extent;
    uint2 bloom_extent;
    float threshold;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};
ConstantBuffer<BloomParameters> g_bloom_parameters;
Texture2D<float4> g_source;
RWTexture2D<float4> g_output;

float3 retroBloomBright(float3 color) {
    color = max(color, 0.0);
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    if (luminance <= 0.0) {
        return float3(0.0);
    }

    // Exposure changes the threshold, never the stored scene-unit magnitude.
    return color * max(0.0, 1.0 - g_bloom_parameters.threshold / luminance);
}

// ------------------------------------------------------------------
// bounded 4x4 extraction before averaging
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void extractRetroBloom(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_bloom_parameters.bloom_extent)) {
        return;
    }

    float3 sum = float3(0.0);
    uint count = 0u;
    [unroll]
    for (uint y = 0u; y < 4u; ++y) {
        [unroll]
        for (uint x = 0u; x < 4u; ++x) {
            uint2 pixel = id.xy * 4u + uint2(x, y);
            if (all(pixel < g_bloom_parameters.source_extent)) {
                sum += retroBloomBright(g_source.Load(int3(pixel, 0)).rgb);
                ++count;
            }
        }
    }

    // Ceil-divided positive extents guarantee at least one in-domain tap.
    g_output[id.xy] = float4(sum / float(count), 1.0);
}

// ------------------------------------------------------------------
// normalized fixed 5x5 binomial blur in FP32
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void blurRetroBloom(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_bloom_parameters.bloom_extent)) {
        return;
    }

    static const float weights[5] = {1.0, 4.0, 6.0, 4.0, 1.0};
    int2 last = int2(g_bloom_parameters.bloom_extent) - 1;
    float3 sum = float3(0.0);
    [unroll]
    for (int y = -2; y <= 2; ++y) {
        [unroll]
        for (int x = -2; x <= 2; ++x) {
            int2 pixel = clamp(int2(id.xy) + int2(x, y), int2(0), last);
            sum += g_source.Load(int3(pixel, 0)).rgb * (weights[x + 2] * weights[y + 2]);
        }
    }

    g_output[id.xy] = float4(sum / 256.0, 1.0);
}
