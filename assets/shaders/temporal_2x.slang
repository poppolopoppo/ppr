// ------------------------------------------------------------------
// output-resolution temporal protocol and actual Hable sample support
// ------------------------------------------------------------------

struct Temporal2xParameters {
    float4x4 inverse_view_projection;
    float4x4 previous_view_projection;
    // Physical UV correction to unjittered sample positions, in raster units.
    float4 sample_offsets;
    uint2 raster_extent;
    uint2 output_extent;
    uint4 sample_indices;
    uint flags;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};
ConstantBuffer<Temporal2xParameters> g_tsr;
Texture2D<float4> g_current;
Texture2DMS<float4, 4> g_raw;
Texture2DMS<float, 4> g_depth;
Texture2DMS<float, 4> g_reactive;
Texture2D<float4> g_history;
RWTexture2D<float4> g_output;

// Each corrected 2x2 Hable bucket has seven distinct nonzero raw taps.
// These are (raster offset x,y,canonical sample), not averaged raster pixels.
static const int3 kTemporal2xSupport[28] = {
    int3(-1,0,0), int3(-1,0,1), int3(0,0,0), int3(0,0,1), int3(0,0,2), int3(0,-1,2), int3(0,1,0),
    int3(0,0,0), int3(0,0,1), int3(0,0,3), int3(1,0,0), int3(0,-1,3), int3(0,-1,1), int3(-1,0,1),
    int3(0,0,0), int3(0,0,2), int3(0,0,3), int3(1,0,2), int3(0,1,0), int3(0,1,2), int3(-1,0,3),
    int3(0,0,1), int3(0,0,2), int3(0,0,3), int3(1,0,2), int3(1,0,3), int3(0,-1,3), int3(0,1,1)
};
static const float2 kTemporal2xPositions[4] = {
    float2(0.375,0.125), float2(0.875,0.375), float2(0.125,0.625), float2(0.625,0.875)
};

// ------------------------------------------------------------------
// full-support history reconstruction and metadata validation
// ------------------------------------------------------------------

bool reconstructTemporal2xHistory(float2 uv, float2 validation_uv, float expected_metadata,
    out float3 color, out float metadata) {
    float2 position = uv * g_tsr.output_extent - 0.5;
    int2 base = int2(floor(position));
    float2 f = frac(position);
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = -0.5 * f + f2 - 0.5 * f3;
    float2 w1 = 1.0 - 2.5 * f2 + 1.5 * f3;
    float2 w2 = 0.5 * f + 2.0 * f2 - 1.5 * f3;
    float2 w3 = -0.5 * f2 + 0.5 * f3;
    float4 wx = float4(w0.x, w1.x, w2.x, w3.x);
    float4 wy = float4(w0.y, w1.y, w2.y, w3.y);
    int2 limit = int2(g_tsr.output_extent) - 1;
    float4 taps[16];
    [unroll]
    for (uint row = 0u; row < 4u; ++row) {
        [unroll]
        for (uint column = 0u; column < 4u; ++column) {
            int2 offset = int2(column, row) - 1;
            taps[row * 4u + column] = g_history.Load(int3(clamp(base + offset, int2(0), limit), 0));
        }
    }

    // Reactive history is not depth. Inspect every contributing tap before
    // fitting metadata, including the negative Catmull-Rom lobes.
    [unroll]
    for (uint row = 0u; row < 4u; ++row) {
        [unroll]
        for (uint column = 0u; column < 4u; ++column) {
            if (wx[column] * wy[row] != 0.0 && taps[row * 4u + column].a == -2.0) {
                color = 0.0;
                metadata = -2.0;
                return false;
            }
        }
    }

    float a = taps[5u].a, b = taps[6u].a, c = taps[9u].a, d = taps[10u].a;
    // Fit one affine plane rather than accepting a bilinear depth discontinuity.
    // The tolerance covers extrapolation of half-precision quantization across
    // the footprint (including eight minimum half-float subnormal steps).
    float center = (a + b + c + d) * 0.25;
    float2 gradient = float2(b - a + d - c, c - a + d - b) * 0.5;
    metadata = center + dot(gradient, validation_uv * g_tsr.output_extent - (float2(base) + 1.0));
    float tolerance = max(8.0 * 5.960464477539063e-8, abs(expected_metadata) * 0.005);
    bool moving = (g_tsr.flags & 4u) == 0u;
    bool coherent = true;
    color = 0.0;
    [unroll]
    for (uint row = 0u; row < 4u; ++row) {
        [unroll]
        for (uint column = 0u; column < 4u; ++column) {
            float weight = wx[column] * wy[row];
            float4 tap = taps[row * 4u + column];
            color += tap.rgb * weight;
            if (moving && weight != 0.0) {
                float predicted = center + dot(gradient, float2(column, row) - 1.5);
                bool same_class = (tap.a < 0.0) == (expected_metadata < 0.0);
                coherent = coherent && same_class && abs(tap.a - predicted) <= tolerance;
            }
        }
    }

    // Stationary jitter may alternate foreground/background coverage. Preserve
    // that integration, but never turn a blended sentinel into geometric depth.
    if (!moving) {
        bool mixed = (a < 0.0) != (b < 0.0) || (a < 0.0) != (c < 0.0) || (a < 0.0) != (d < 0.0);
        if (mixed) {
            int2 nearest = clamp(int2(uv * g_tsr.output_extent), int2(0), limit);
            metadata = g_history.Load(int3(nearest, 0)).a;
        }
    }

    return coherent;
}

// ------------------------------------------------------------------
// scene-linear temporal resolve at the unjittered output grid
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(8,8,1)]
void resolveTemporal2x(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_tsr.output_extent)) {
        return;
    }

    int2 pixel = int2(id.xy);
    int2 raster_pixel = pixel / 2;
    uint bucket = (id.x & 1u) + 2u * (id.y & 1u);
    float3 current = g_current.Load(int3(pixel,0)).rgb;
    float3 low = float3(1.0e30), high = float3(-1.0e30);
    float depth = 1.0;
    float2 sample_uv = (float2(pixel) + 0.5) / float2(g_tsr.output_extent);
    bool reactive = false;
    [unroll]
    for (uint tap = 0u; tap < 7u; ++tap) {
        int3 support = kTemporal2xSupport[bucket * 7u + tap];
        int2 p = clamp(raster_pixel + support.xy, int2(0), int2(g_tsr.raster_extent) - 1);
        uint sample = g_tsr.sample_indices[support.z];
        float3 color = g_raw.Load(p,sample).rgb;
        low = min(low,color);
        high = max(high,color);
        reactive = reactive || g_reactive.Load(p,sample) > 0.0;
        float candidate_depth = g_depth.Load(p,sample);
        if (candidate_depth < depth) {
            depth = candidate_depth;
            sample_uv = (float2(p) + kTemporal2xPositions[support.z]) / float2(g_tsr.raster_extent);
        }
    }

    float2 uv = (float2(pixel) + 0.5) / float2(g_tsr.output_extent);
    bool geometry = depth < 1.0;
    bool perspective = (g_tsr.flags & 2u) != 0u;
    float4 sample_world = mul(float4((sample_uv + g_tsr.sample_offsets.xy) * float2(2,-2) + float2(-1,1),depth,1),
        g_tsr.inverse_view_projection);
    float metadata = geometry ? (perspective ? sample_world.w : depth) : -1.0;

    // Reciprocal clip W is a history identity, never a clamped color value.
    // Reject values that would become infinity/zero in RGBA16F metadata.
    bool valid_metadata = !geometry || !perspective ||
        (isfinite(metadata) && metadata >= 5.960464477539063e-8 && metadata <= 65504.0);
    reactive = reactive || !valid_metadata;
    float4 result = float4(current,reactive ? -2.0 : metadata);
    if (reactive || (g_tsr.flags & 1u) == 0u) {
        g_output[pixel] = result;
        return;
    }

    float4 world = mul(float4(uv * float2(2,-2) + float2(-1,1),depth,1),g_tsr.inverse_view_projection);
    world /= world.w;
    float4 previous = mul(world,g_tsr.previous_view_projection);
    if (previous.w <= 0.0 || previous.z < 0.0 || previous.z > previous.w) {
        g_output[pixel] = result;
        return;
    }

    float2 previous_uv = previous.xy / previous.w * float2(0.5,-0.5) + 0.5;
    if (!geometry) {
        // Layered/parallax backgrounds have no single valid moving plane.
        if ((g_tsr.flags & 4u) == 0u) {
            g_output[pixel] = result;
            return;
        }
        previous_uv = uv;
    }

    float2 border = 0.5 / float2(g_tsr.output_extent);
    if (any(previous_uv < border) || any(previous_uv > 1.0 - border)) {
        g_output[pixel] = result;
        return;
    }

    float2 validation_uv = uv;
    float expected_metadata = -1.0;
    if (geometry) {
        sample_world /= sample_world.w;
        float4 previous_sample = mul(sample_world,g_tsr.previous_view_projection);
        if (previous_sample.w <= 0.0 || previous_sample.z < 0.0 || previous_sample.z > previous_sample.w) {
            g_output[pixel] = result;
            return;
        }

        validation_uv = previous_sample.xy / previous_sample.w * float2(0.5,-0.5) + 0.5 - g_tsr.sample_offsets.zw;
        expected_metadata = perspective ? 1.0 / previous_sample.w : previous_sample.z / previous_sample.w;
        if (any(validation_uv < border) || any(validation_uv > 1.0 - border)) {
            g_output[pixel] = result;
            return;
        }
    }

    if (geometry && perspective && (!isfinite(expected_metadata) ||
        expected_metadata < 5.960464477539063e-8 || expected_metadata > 65504.0)) {
        g_output[pixel] = result;
        return;
    }

    float3 history;
    float history_metadata;
    bool coherent = reconstructTemporal2xHistory(previous_uv,validation_uv,expected_metadata,history,history_metadata);
    float tolerance = max(8.0 * 5.960464477539063e-8,abs(expected_metadata) * 0.005);
    bool coverage_change = geometry != (history_metadata >= 0.0);
    bool depth_change = geometry && history_metadata >= 0.0 && abs(history_metadata - expected_metadata) > tolerance;
    if (!coherent || depth_change || (coverage_change && (g_tsr.flags & 4u) == 0u)) {
        // Old reactive taps reject this read without poisoning a now-static
        // current metadata value: the next accepted frame may recover history.
        g_output[pixel] = result;
        return;
    }

    float motion = length((previous_uv - uv) * float2(g_tsr.output_extent));
    float influence = lerp(0.05,0.5,saturate(motion / 0.25));
    float3 clamped_history = clamp(history,low,high);
    float3 difference = abs(history - clamped_history) / max(high - low,float3(0.01));
    influence = max(influence,saturate(max(difference.x,max(difference.y,difference.z))));
    if (coverage_change) {
        influence = max(influence,0.25);
    }

    result.rgb = lerp(clamped_history,current,influence);
    g_output[pixel] = result;
}
