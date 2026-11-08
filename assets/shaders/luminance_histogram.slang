// ------------------------------------------------------------------
// log-luminance histogram and percentile-clipped target EV reduction
// ------------------------------------------------------------------
// Downsamples the resolved HDR scene to a small luminance image, builds a
// log2-luminance histogram with workgroup-local bins merged into one global
// histogram, then reduces a percentile-clipped average to a target EV relative
// to 18% middle gray. The CPU oracle lives in RetroPass (averageLog2Luminance /
// targetEv) and must stay bit-for-bit consistent with this reduction.

static const uint kHistogramBins = 64u;
static const float kLog2Min = -8.0;
static const float kLog2Max = 8.0;
static const float kLowPercentile = 0.10;
static const float kHighPercentile = 0.90;
static const float kMiddleGray = 0.18;

struct LuminanceHistogramParameters {
    uint2 sourceExtent;
    uint2 luminanceExtent;
    uint reserved0;
    uint reserved1;
};
ConstantBuffer<LuminanceHistogramParameters> g_luminance;

Texture2D<float4> g_scene;
RWTexture2D<float4> g_luminance_image; // R32Float luminance in .x
RWStructuredBuffer<uint> g_histogram;  // kHistogramBins u32 counts
RWStructuredBuffer<float> g_target_ev; // one float: reduced target EV offset

// One workgroup-local histogram, shared by the buildHistogram threads.
groupshared uint localHistogram[kHistogramBins];

float luminanceOf(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float binCenterLog2(uint bin) {
    return kLog2Min + (float(bin) + 0.5) * (kLog2Max - kLog2Min) / float(kHistogramBins);
}

uint luminanceBin(float logL) {
    float normalized = (logL - kLog2Min) / (kLog2Max - kLog2Min);
    uint bin = uint(clamp(normalized, 0.0, 1.0) * float(kHistogramBins));
    return min(bin, kHistogramBins - 1u);
}

// Zero the global histogram before the workgroup-local merge.
[numthreads(64, 1, 1)]
void clearHistogram(uint3 id : SV_DispatchThreadID) {
    if (id.x < kHistogramBins) {
        g_histogram[id.x] = 0u;
    }
}

// Average each luminance texel over its source block.
[numthreads(8, 8, 1)]
void computeLuminance(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_luminance.luminanceExtent)) {
        return;
    }

    float2 scale = float2(g_luminance.sourceExtent) / float2(g_luminance.luminanceExtent);
    uint2 lo = uint2(float2(id.xy) * scale);
    uint2 hi = min(uint2(ceil(float2(id.xy + 1u) * scale)), g_luminance.sourceExtent);

    float sum = 0.0;
    uint count = 0u;
    for (uint y = lo.y; y < hi.y; ++y) {
        for (uint x = lo.x; x < hi.x; ++x) {
            sum += luminanceOf(g_scene.Load(int3(x, y, 0)).rgb);
            ++count;
        }
    }
    g_luminance_image[id.xy] = float4(count != 0u ? sum / float(count) : 0.0, 0.0, 0.0, 1.0);
}

// Workgroup-local histogram over the luminance image, merged into one global
// histogram with a single atomic add per bin per group.
[numthreads(8, 8, 1)]
void buildHistogram(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {
    uint lane = gtid.y * 8u + gtid.x;

    if (lane < kHistogramBins) {
        localHistogram[lane] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (!any(id.xy >= g_luminance.luminanceExtent)) {
        float luminance = g_luminance_image.Load(int2(id.xy)).x;
        float logL = log2(max(luminance, exp2(kLog2Min)));
        InterlockedAdd(localHistogram[luminanceBin(logL)], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    if (lane < kHistogramBins) {
        InterlockedAdd(g_histogram[lane], localHistogram[lane]);
    }
}

// Reduce the percentile-clipped average log2 luminance to a target EV offset.
[numthreads(1, 1, 1)]
void reduceHistogram(uint3 id : SV_DispatchThreadID) {
    uint total = 0u;
    for (uint bin = 0u; bin < kHistogramBins; ++bin) {
        total += g_histogram[bin];
    }

    float low = float(total) * kLowPercentile;
    float high = float(total) * kHighPercentile;

    float accumulated = 0.0;
    uint accumulatedCount = 0u;
    uint cumulative = 0u;
    for (uint bin = 0u; bin < kHistogramBins; ++bin) {
        uint binStart = cumulative;
        cumulative += g_histogram[bin];

        float lo = max(float(binStart), low);
        float hi = min(float(cumulative), high);
        if (lo < hi) {
            uint count = uint(hi - lo);
            accumulated += binCenterLog2(bin) * float(count);
            accumulatedCount += count;
        }
    }

    float average = accumulatedCount != 0u ? accumulated / float(accumulatedCount) : kLog2Min;
    g_target_ev[0] = average - log2(kMiddleGray);
}
