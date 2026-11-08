// ------------------------------------------------------------------
// SPD-style bloom: exposure-aware prefilter, single-pass multi-mip
// downsample, and a progressive tent upsample over a FP16 mip chain.
// ------------------------------------------------------------------
// The resolved scene-linear HDR picture is prefiltered to half resolution
// (soft-knee threshold plus Karis firefly weighting), then a single dispatch
// downsamples mip 1..5 with a workgroup-local 2x2 reduction (AMD SPD style,
// 256 threads, one 32x32 mip0 tile per workgroup, odd extents floor-halved), and
// a short chain of per-level tent upsamples accumulates every level back into
// mip 0. The present shader adds that mip 0 once, scaled by the bloom
// intensity setting. All targets are RGBA16F.

static const uint kBloomMaxMips = 6u;
// Per-level bloom weights (UE4-style "bloom size" curve). Each mip's contribution
// is attenuated by its weight before the tent upsample, so a uniform bright frame
// sums to ~2x the prefilter instead of accumulating ~6x. The finer mips dominate.
static const float kBloomLevelWeights[6] = {1.0, 0.5, 0.25, 0.15, 0.1, 0.05};

// Same 32-byte constant as RetroPass::BloomParameters in
// App.Renderer.RetroPass.cpp. Plain uint2/float fields keep the constant-buffer
// layout free of vector packing rules.
struct BloomParameters {
    uint2 sourceExtent; // resolved HDR scene extent (full resolution)
    uint2 mip0Extent;   // half-resolution prefilter extent (the chain base)
    float threshold;    // exposed-unit soft-knee threshold (manual EV folded in)
    float softKnee;     // exposed-unit soft-knee half width (> 0)
    uint mipCount;      // total mips including mip 0 (<= kBloomMaxMips)
    uint upsampleLevel; // for upsampleBloom: source mip level being upsampled
};
ConstantBuffer<BloomParameters> g_bloom;

// Full-resolution resolved scene (prefilter input) and the per-mip FP16 chain
// views. Compute passes read and write through UAV views only, so no dispatch
// aliases an SRV and a UAV of the same subresource.
Texture2D<float4> g_scene;
// 1x1 adapted-exposure offset (EV). The prefilter divides the exposed-unit
// threshold back to scene units so the bright pass is exposure-aware.
Texture2D<float4> g_exposure;
RWTexture2D<float4> g_bloom_mip0;
RWTexture2D<float4> g_bloom_mip1;
RWTexture2D<float4> g_bloom_mip2;
RWTexture2D<float4> g_bloom_mip3;
RWTexture2D<float4> g_bloom_mip4;
RWTexture2D<float4> g_bloom_mip5;
// Upsample reads the coarser mip and read-modify-writes the finer mip; the host
// rebinds these two views to the level pair before each dispatch.
RWTexture2D<float4> g_up_src;
RWTexture2D<float4> g_up_dst;

// Workgroup-local intermediates for the SPD 2x2 reduction. Two arrays alternate
// so each level reads a disjoint buffer from the one it writes, keeping the
// in-place chain free of shared-memory read/write races.
groupshared float4 g_shared_a[16][16];
groupshared float4 g_shared_b[16][16];

float luminanceOf(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Energy-conserving soft-knee bright pass (Call of Duty: Advanced Warfare). A
// luminance at or below the threshold contributes nothing, then a smooth
// quadratic ramp spans [threshold, threshold + 2*softKnee] before the linear
// tail. Black input stays exactly black. The threshold/soft knee are in scene
// units (already divided by the total exposure).
float3 bloomThreshold(float3 color, float threshold, float softKnee) {
    float luma = luminanceOf(color);
    float brightness = max(luma - threshold, 0.0);
    if (brightness < 2.0 * softKnee) {
        brightness = brightness * brightness / (4.0 * softKnee + 1e-4);
    } else {
        brightness = brightness - softKnee;
    }

    float scale = brightness / max(luma, 1e-4);
    return color * scale;
}

// Karis average: suppresses fireflies (a single very bright tap) while leaving a
// uniformly bright block mostly intact. Applied once at the prefilter.
float karisWeight(float3 color) {
    return 1.0 / (1.0 + luminanceOf(color));
}

float4 loadMip0(uint2 pixel) {
    uint2 extent = max(uint2(1, 1), g_bloom.mip0Extent);
    return g_bloom_mip0.Load(int2(min(pixel, extent - 1u)));
}

// ------------------------------------------------------------------
// prefilter: half-resolution bright pass
// ------------------------------------------------------------------

[numthreads(8, 8, 1)]
void prefilterBloom(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_bloom.mip0Extent)) {
        return;
    }

    // Divide the exposed-unit threshold/soft knee back to scene units using the
    // adapted exposure offset; a fixed-exposure path binds the zero offset.
    float adapted = g_exposure.Load(int3(0, 0, 0)).x;
    float exposure_scale = exp2(-adapted);
    float threshold = g_bloom.threshold * exposure_scale;
    float soft_knee = g_bloom.softKnee * exposure_scale;

    uint2 lo = id.xy * 2u;
    uint2 hi = min(lo + 1u, g_bloom.sourceExtent - 1u);
    float3 a = g_scene.Load(int3(lo.x, lo.y, 0)).rgb;
    float3 b = g_scene.Load(int3(hi.x, lo.y, 0)).rgb;
    float3 c = g_scene.Load(int3(lo.x, hi.y, 0)).rgb;
    float3 d = g_scene.Load(int3(hi.x, hi.y, 0)).rgb;
    float3 average = (a + b + c + d) * 0.25;

    float3 bright = bloomThreshold(average, threshold, soft_knee);
    bright *= karisWeight(bright);
    g_bloom_mip0[id.xy] = float4(bright, 1.0);
}

// ------------------------------------------------------------------
// single-pass multi-mip downsample (mips 1..5)
// ------------------------------------------------------------------

[numthreads(16, 16, 1)]
void downsampleBloom(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint2 t = gtid.xy;
    uint2 mip0 = g_bloom.mip0Extent;
    uint mipCount = g_bloom.mipCount;

    // mip 1 = 16x16 per workgroup tile, a plain 2x2 box of mip 0. The mipCount
    // guard keeps a single-mip chain (output <= 2x2) from aliasing mip 1 onto
    // mip 0's view and halving the prefilter result.
    uint2 mip1Extent = max(uint2(1, 1), mip0 >> 1u);
    uint2 dst = gid.xy * 16u + t;
    float4 value = float4(0.0, 0.0, 0.0, 0.0);
    if (mipCount > 1u && all(dst < mip1Extent)) {
        uint2 src = dst * 2u;
        value = (loadMip0(src) + loadMip0(src + uint2(1, 0)) +
                 loadMip0(src + uint2(0, 1)) + loadMip0(src + uint2(1, 1))) *
                0.25;
        g_bloom_mip1[dst] = value * kBloomLevelWeights[1];
    }
    g_shared_a[t.y][t.x] = value;
    GroupMemoryBarrierWithGroupSync();

    // mip 2 = 8x8, reduced from the 16x16 mip 1 staged above.
    if (mipCount > 2u && all(t < 8u)) {
        float4 reduced = (g_shared_a[2 * t.y][2 * t.x] + g_shared_a[2 * t.y][2 * t.x + 1u] +
                          g_shared_a[2 * t.y + 1u][2 * t.x] + g_shared_a[2 * t.y + 1u][2 * t.x + 1u]) *
                         0.25;
        uint2 out = gid.xy * 8u + t;
        if (all(out < (mip0 >> 2u))) {
            g_bloom_mip2[out] = reduced * kBloomLevelWeights[2];
        }
        g_shared_b[t.y][t.x] = reduced;
    }
    GroupMemoryBarrierWithGroupSync();

    // mip 3 = 4x4.
    if (mipCount > 3u && all(t < 4u)) {
        float4 reduced = (g_shared_b[2 * t.y][2 * t.x] + g_shared_b[2 * t.y][2 * t.x + 1u] +
                          g_shared_b[2 * t.y + 1u][2 * t.x] + g_shared_b[2 * t.y + 1u][2 * t.x + 1u]) *
                         0.25;
        uint2 out = gid.xy * 4u + t;
        if (all(out < (mip0 >> 3u))) {
            g_bloom_mip3[out] = reduced * kBloomLevelWeights[3];
        }
        g_shared_a[t.y][t.x] = reduced;
    }
    GroupMemoryBarrierWithGroupSync();

    // mip 4 = 2x2.
    if (mipCount > 4u && all(t < 2u)) {
        float4 reduced = (g_shared_a[2 * t.y][2 * t.x] + g_shared_a[2 * t.y][2 * t.x + 1u] +
                          g_shared_a[2 * t.y + 1u][2 * t.x] + g_shared_a[2 * t.y + 1u][2 * t.x + 1u]) *
                         0.25;
        uint2 out = gid.xy * 2u + t;
        if (all(out < (mip0 >> 4u))) {
            g_bloom_mip4[out] = reduced * kBloomLevelWeights[4];
        }
        g_shared_b[t.y][t.x] = reduced;
    }
    GroupMemoryBarrierWithGroupSync();

    // mip 5 = 1x1 per workgroup.
    if (mipCount > 5u && all(t < 1u)) {
        float4 reduced = (g_shared_b[0][0] + g_shared_b[0][1u] + g_shared_b[1u][0] + g_shared_b[1u][1u]) * 0.25;
        uint2 out = gid.xy;
        if (all(out < (mip0 >> 5u))) {
            g_bloom_mip5[out] = reduced * kBloomLevelWeights[5];
        }
    }
}

// ------------------------------------------------------------------
// progressive tent upsample (accumulates mip K back into mip K-1)
// ------------------------------------------------------------------

[numthreads(8, 8, 1)]
void upsampleBloom(uint3 id : SV_DispatchThreadID) {
    uint level = g_bloom.upsampleLevel;
    uint2 dstExtent = max(uint2(1, 1), g_bloom.mip0Extent >> (level - 1u));
    if (any(id.xy >= dstExtent)) {
        return;
    }

    uint2 srcExtent = max(uint2(1, 1), g_bloom.mip0Extent >> level);

    // 3x3 tent filter sampled at the destination texel's mapped centre.
    float2 source = (float2(id.xy) + 0.5) * float2(srcExtent) / float2(dstExtent) - 0.5;
    int2 base = int2(floor(source));
    int2 clamp_max = int2(srcExtent) - 1;
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            float weight = float((2 - abs(dx)) * (2 - abs(dy)));
            int2 sample = clamp(base + int2(dx, dy), int2(0, 0), clamp_max);
            sum += g_up_src.Load(sample).rgb * weight;
        }
    }

    float3 upsampled = sum / 16.0;
    float3 current = g_up_dst.Load(int2(id.xy)).rgb;
    g_up_dst[id.xy] = float4(current + upsampled, 1.0);
}
