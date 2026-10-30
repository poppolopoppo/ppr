// Native-resolution temporal supersampling, adapted from Filmic Worlds:
// https://filmicworlds.com/blog/temporal-super-resolution-via-multisampling/
// Keep individual MSAA samples for the color box instead of clamping against
// an already resolved neighborhood. Four half-pixel jitter phases balance the
// sample lattice. This pass does not perform the article's 2x spatial upscale.
struct TemporalParameters {
    float4x4 m_inverse_view_projection;
    float4x4 m_previous_view_projection;
    // Physical UV displacement caused by jitter: current.xy, previous.zw.
    float4 m_sample_offsets;
    float2 m_extent;
    // bit0: valid history, bit1: perspective, bit2: unchanged camera
    uint m_flags;
    uint m_reserved;
};
ConstantBuffer<TemporalParameters> g_temporal;
Texture2DMS<float4, 4> g_current;
Texture2DMS<float, 4> g_depth;
// Linear RGB. Alpha is device Z for orthographic projection or reciprocal
// clip W for perspective projection; -1 marks background. These depth values
// are screen-affine on planes and retain useful precision in RGBA16Float.
Texture2D<float4> g_history;

struct TemporalVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
TemporalVertexOutput vertexTemporal(uint vertex : SV_VertexID) {
    float2 uv = float2((vertex << 1u) & 2u, vertex & 2u);
    TemporalVertexOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

// Validate the same support that reconstructs RGB: every nonzero Catmull-Rom
// tap, including its negative lobes, must belong to the reprojected surface
// during camera motion. Sixteen point loads supply both color and metadata.
bool reconstructHistory(float2 uv, float2 validation_uv, float expected_metadata,
    out float3 color, out float metadata) {
    float2 position = uv * g_temporal.m_extent - 0.5;
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
    int2 limit = int2(g_temporal.m_extent) - 1;
    float4 taps[16];
    [unroll]
    for (uint row = 0u; row < 4u; ++row) {
        [unroll]
        for (uint column = 0u; column < 4u; ++column) {
            int2 offset = int2(column, row) - 1;
            taps[row * 4u + column] = g_history.Load(int3(clamp(base + offset, int2(0), limit), 0));
        }
    }

    float a = taps[5u].a, b = taps[6u].a, c = taps[9u].a, d = taps[10u].a;
    // Fit one affine plane rather than accepting a bilinear depth discontinuity.
    // The tolerance covers extrapolation of half-precision quantization across
    // the footprint (including eight minimum half-float subnormal steps).
    float center = (a + b + c + d) * 0.25;
    float2 gradient = float2(b - a + d - c, c - a + d - b) * 0.5;
    metadata = center + dot(gradient, validation_uv * g_temporal.m_extent - (float2(base) + 1.0));
    float tolerance = max(8.0 * 5.960464477539063e-8, abs(expected_metadata) * 0.005);
    bool moving = (g_temporal.m_flags & 4u) == 0u;
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
            int2 nearest = clamp(int2(uv * g_temporal.m_extent), int2(0), limit);
            metadata = g_history.Load(int3(nearest, 0)).a;
        }
    }

    return coherent;
}

[shader("fragment")]
float4 fragmentTemporal(TemporalVertexOutput input) : SV_Target {
    int2 extent = int2(g_temporal.m_extent);
    int2 pixel = clamp(int2(input.position.xy), int2(0), extent - 1);
    float3 current = 0.0;
    float3 low = float3(1.0e30), high = float3(-1.0e30);
    float depth = 1.0;
    // The cross includes raw coverage samples, preserving thin features that
    // would disappear from a color box built from resolved pixels alone.
    static const int2 cross[5] = {int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    [unroll]
    for (uint tap = 0u; tap < 5u; ++tap) {
        int2 p = clamp(pixel + cross[tap], int2(0), extent - 1);
        [unroll]
        for (uint sample = 0u; sample < 4u; ++sample) {
            float3 color = g_current.Load(p, sample).rgb;
            low = min(low, color);
            high = max(high, color);
            if (tap == 0u) {
                current += color * 0.25;
                depth = min(depth, g_depth.Load(p, sample));
            }
        }
    }

    float2 uv = (float2(pixel) + 0.5) / g_temporal.m_extent;
    // History is indexed by unjittered output pixels. Reconstruct the output
    // center with unjittered transforms so sample jitter never becomes motion.
    // Nearest MSAA depth approximates center depth: exact XY motion for the
    // game's orthographic camera, approximate on translated perspective slopes.
    float4 world = mul(float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0),
        g_temporal.m_inverse_view_projection);
    bool geometry = depth < 1.0;
    bool perspective = (g_temporal.m_flags & 2u) != 0u;
    float metadata = geometry ? (perspective ? world.w : depth) : -1.0;
    world /= world.w;
    float4 result = float4(current, metadata);
    if ((g_temporal.m_flags & 1u) == 0u) {
        return result;
    }

    float4 previous = mul(world, g_temporal.m_previous_view_projection);
    if (previous.w <= 0.0 || previous.z < 0.0 || previous.z > previous.w) {
        return result;
    }

    float2 previous_uv = previous.xy / previous.w * float2(0.5, -0.5) + 0.5;
    if (!geometry) {
        // Layered parallax backgrounds have no single motion vector. On camera
        // motion, use this frame's MSAA instead of accumulating false history.
        if ((g_temporal.m_flags & 4u) == 0u) {
            return result;
        }
        previous_uv = uv;
    }

    float2 border = 0.5 / g_temporal.m_extent;
    if (any(previous_uv < border) || any(previous_uv > 1.0 - border)) {
        return result;
    }

    // Depth came from the jitter-displaced raster sample, while RGB history
    // is indexed by output pixels. Reproject an auxiliary phase-aligned point
    // for the metadata comparison, then remove history's phase displacement.
    // This leaves RGB motion and the full-support rejection test unchanged.
    float2 validation_uv = uv;
    float expected_metadata = -1.0;
    if (geometry) {
        float2 sample_uv = uv + g_temporal.m_sample_offsets.xy;
        float4 sample_world = mul(
            float4(sample_uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0),
            g_temporal.m_inverse_view_projection);
        sample_world /= sample_world.w;
        float4 previous_sample = mul(sample_world, g_temporal.m_previous_view_projection);
        if (previous_sample.w <= 0.0 || previous_sample.z < 0.0 || previous_sample.z > previous_sample.w) {
            return result;
        }

        validation_uv = previous_sample.xy / previous_sample.w * float2(0.5, -0.5) + 0.5 -
            g_temporal.m_sample_offsets.zw;
        expected_metadata = perspective ? 1.0 / previous_sample.w : previous_sample.z / previous_sample.w;
        if (any(validation_uv < border) || any(validation_uv > 1.0 - border)) {
            return result;
        }
    }

    float3 history;
    float history_metadata;
    bool coherent = reconstructHistory(previous_uv, validation_uv, expected_metadata, history, history_metadata);
    float tolerance = max(8.0 * 5.960464477539063e-8, abs(expected_metadata) * 0.005);
    bool coverage_change = geometry != (history_metadata >= 0.0);
    bool depth_change = geometry && history_metadata >= 0.0 &&
        abs(history_metadata - expected_metadata) > tolerance;
    if (!coherent || depth_change || (coverage_change && (g_temporal.m_flags & 4u) == 0u)) {
        return result;
    }

    float motion = length((previous_uv - uv) * g_temporal.m_extent);
    // Retain half the current signal under motion instead of repeatedly
    // resampling 75-95% old color. Keep temporal integration (rather than exposing
    // a lone jitter phase) even when the camera crosses multiple pixels/frame.
    float influence = lerp(0.05, 0.5, saturate(motion / 0.25));
    // Coverage may alternate across jitter phases even at a static silhouette.
    // Preserve valid in-box variation; react only to colors outside the current
    // raw-sample box. Without object motion vectors, dynamic silhouettes under
    // a stationary camera remain ambiguous; use faster decay at class changes.
    float3 clamped_history = clamp(history, low, high);
    float3 span = max(high - low, float3(0.01));
    float3 difference = abs(history - clamped_history) / span;
    float reactive = saturate(max(difference.x, max(difference.y, difference.z)));
    influence = max(influence, reactive);
    if (coverage_change) {
        influence = max(influence, 0.25);
    }

    result.rgb = lerp(clamped_history, current, influence);
    return result;
}
