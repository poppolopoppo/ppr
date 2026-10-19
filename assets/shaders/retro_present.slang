// Same 528-byte per-slot constant as RetroPass::RetroParameters in
// App.Renderer.RetroPass.cpp: 32 sRGB palette entries, the low-resolution
// source extent, and the dither scale. Row-major float4 palette stride keeps
// the constant-buffer layout free of packing rules.
struct RetroParameters {
    float4 m_palette[32];
    float2 m_source_extent;
    float m_dither_strength;
    uint m_reserved;
};
ConstantBuffer<RetroParameters> g_retro;
// The low-resolution scene resolve. Pixel-center fractional taps plus an
// explicit clamp make the read exactly one source pixel per output pixel,
// independent of the bound sampler's filtering mode.
Texture2D<float4> g_scene;
struct RetroVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
RetroVertexOutput vertexRetroPresent(uint vertex : SV_VertexID) {
    float2 uv = float2((vertex << 1u) & 2u, vertex & 2u);
    RetroVertexOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

float3 linearToSrgb(float3 linear_color) {
    return select(linear_color <= 0.0031308, linear_color * 12.92,
        1.055 * pow(linear_color, float3(1.0 / 2.4)) - 0.055);
}

float3 srgbToLinear(float3 srgb_color) {
    return select(srgb_color <= 0.04045, srgb_color / 12.92,
        pow((srgb_color + 0.055) / 1.055, float3(2.4)));
}

[shader("fragment")]
float4 fragmentRetroPresent(RetroVertexOutput input,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    float2 extent = max(g_retro.m_source_extent, float2(1.0));
    float2 source_pixel = floor(clamp(input.uv, 0.0, 1.0) * extent);
    float2 source_uv = clamp((source_pixel + 0.5) / extent, 0.0, 1.0);
    float3 source_linear = g_scene.SampleLevel(g_sampler, source_uv, 0.0).rgb;
    float3 source_srgb = linearToSrgb(source_linear);
    // Ordered 4x4 Bayer threshold in [-0.5, 0.5): biases each output pixel
    // around its true color before palette matching, so gradients resolve into
    // stable dither patterns instead of banding.
    static const uint kBayer4x4[16] = {0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u,
        3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u};
    uint2 pixel = uint2(input.position.xy);
    float threshold = (kBayer4x4[(pixel.y & 3u) * 4u + (pixel.x & 3u)] + 0.5) / 16.0 - 0.5;
    float3 target = clamp(source_srgb + threshold * g_retro.m_dither_strength, 0.0, 1.0);
    // Luma-weighted squared distance in sRGB keeps perceived error dominant
    // over the chroma channels, matching how the palette was authored.
    float3 best = g_retro.m_palette[0].rgb;
    float best_distance = 1.0e30;
    [unroll]
    for (uint index = 0u; index < 32u; ++index) {
        float3 candidate = g_retro.m_palette[index].rgb;
        float3 difference = target - candidate;
        float distance = dot(difference * difference, float3(0.2126, 0.7152, 0.0722));
        if (distance < best_distance) {
            best_distance = distance;
            best = candidate;
        }
    }
    // The palette is stored in sRGB; the surface target expects linear values.
    return float4(srgbToLinear(best), 1.0);
}
