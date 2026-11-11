// ------------------------------------------------------------------
// bloom: exposure-aware prefilter, ceil-sized box levels and weighted
// tent upsample over separately allocated FP16 images
// ------------------------------------------------------------------
// The resolved scene-linear HDR picture is prefiltered to ceil(R/2), then each
// coarser level is a plain 2x2 box reduction with taps clamped to the last valid
// texel (a partial last row/column is replicated, never replaced by zeros).
// Every output texel is initialized. Levels are weighted once when built; the
// progressive upsample adds the tent-filtered coarser result without repeating
// the per-level weight. All targets are RGBA16F.

static const float kBloomLevelWeights[6] = {1.0, 0.5, 0.25, 0.15, 0.1, 0.05};

// Mirrors PostProcessPass::BloomParameters. Plain scalars keep the CPU/GPU
// constant-buffer layout free of vector packing rules.
struct BloomParameters {
    uint m_source_width;
    uint m_source_height;
    uint m_src_width;
    uint m_src_height;
    uint m_dst_width;
    uint m_dst_height;
    float m_threshold;
    float m_soft_knee;
    float m_manual_ev;
    uint m_level;
    uint m_reserved0;
    uint m_reserved1;
    uint m_reserved2;
    uint m_reserved3;
    uint m_reserved4;
    uint m_reserved5;
};
ConstantBuffer<BloomParameters> g_bloom;

Texture2D<float4> g_scene;
Texture2D<float4> g_exposure;
Texture2D<float4> g_down_src;
RWTexture2D<float4> g_down_dst;
Texture2D<float4> g_up_src;
RWTexture2D<float4> g_up_dst;
SamplerState.Handle g_linear;

float bloomLuminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Energy-conserving soft-knee bright pass. Threshold and knee arrive in scene
// units (already divided by the total exposure), so black stays black and the
// exposure conversion happens exactly once on the host.
float3 bloomThreshold(float3 color, float threshold, float softKnee) {
    float luma = bloomLuminance(color);
    float brightness = max(luma - threshold, 0.0);
    if (brightness < 2.0 * softKnee) {
        brightness = brightness * brightness / (4.0 * softKnee + 1e-4);
    } else {
        brightness = brightness - softKnee;
    }

    float scale = brightness / max(luma, 1e-4);
    return color * scale;
}

// Karis average: suppresses a single very bright tap while leaving a uniformly
// bright block mostly intact. Applied once at the prefilter.
float bloomKarisWeight(float3 color) {
    return 1.0 / (1.0 + bloomLuminance(color));
}

// ------------------------------------------------------------------
// prefilter: ceil(R/2) bright pass
// ------------------------------------------------------------------

[numthreads(8, 8, 1)]
void prefilterBloom(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(g_bloom.m_dst_width, g_bloom.m_dst_height))) {
        return;
    }

    uint2 source = max(uint2(g_bloom.m_source_width, g_bloom.m_source_height), uint2(1u, 1u));
    uint2 lo = min(id.xy * 2u, source - 1u);
    uint2 hi = min(lo + 1u, source - 1u);
    float3 average = (g_scene.Load(int3(lo.x, lo.y, 0)).rgb + g_scene.Load(int3(hi.x, lo.y, 0)).rgb +
                      g_scene.Load(int3(lo.x, hi.y, 0)).rgb + g_scene.Load(int3(hi.x, hi.y, 0)).rgb) *
                     0.25;

    // Convert the exposed-unit controls to scene units exactly once with the
    // total exposure (manual EV plus the accepted adapted offset).
    float exposure = exp2(g_bloom.m_manual_ev + g_exposure.Load(int3(0, 0, 0)).x);
    float threshold = g_bloom.m_threshold / exposure;
    float soft_knee = g_bloom.m_soft_knee / exposure;
    float3 bright = bloomThreshold(average, threshold, soft_knee);
    bright *= bloomKarisWeight(bright);
    g_down_dst[id.xy] = float4(bright, 1.0);
}

// ------------------------------------------------------------------
// 2x2 box downsample into one coarser level
// ------------------------------------------------------------------

[numthreads(8, 8, 1)]
void downsampleBloom(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(g_bloom.m_dst_width, g_bloom.m_dst_height))) {
        return;
    }

    uint2 source = max(uint2(g_bloom.m_src_width, g_bloom.m_src_height), uint2(1u, 1u));
    uint2 lo = min(id.xy * 2u, source - 1u);
    uint2 hi = min(lo + 1u, source - 1u);
    float4 value = (g_down_src.Load(int3(lo.x, lo.y, 0)) + g_down_src.Load(int3(hi.x, lo.y, 0)) +
                    g_down_src.Load(int3(lo.x, hi.y, 0)) + g_down_src.Load(int3(hi.x, hi.y, 0))) *
                   0.25;
    // The level weight is applied exactly once, when the raw level is built.
    g_down_dst[id.xy] = value * kBloomLevelWeights[min(g_bloom.m_level, 5u)];
}

// ------------------------------------------------------------------
// progressive tent upsample (adds the coarser accumulated level)
// ------------------------------------------------------------------

[numthreads(8, 8, 1)]
void upsampleBloom(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(g_bloom.m_dst_width, g_bloom.m_dst_height))) {
        return;
    }

    uint2 destination = max(uint2(g_bloom.m_dst_width, g_bloom.m_dst_height), uint2(1u, 1u));
    uint2 source = max(uint2(g_bloom.m_src_width, g_bloom.m_src_height), uint2(1u, 1u));

    // Separable [1,2,1]/4 tent sampled one coarse texel apart around the mapped
    // fine center. Clamp addressing replicates the valid coarse edge texels.
    float2 centre = (float2(id.xy) + 0.5) * float2(source) / float2(destination);
    float2 offset = 1.0 / float2(source);
    float3 accumulated = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            float2 uv = (centre + float2(dx, dy)) / float2(source);
            float2 weight = float2(float(2 - abs(dx)), float(2 - abs(dy))) * 0.25;
            accumulated += g_up_src.SampleLevel(g_linear, uv, 0.0).rgb * (weight.x * weight.y);
        }
    }

    float3 current = g_up_dst.Load(int2(id.xy)).rgb;
    g_up_dst[id.xy] = float4(current + accumulated, 1.0);
}
