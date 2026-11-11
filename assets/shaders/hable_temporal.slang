// Copyright 2025 John Hable. Temporal reconstruction adapted under the MIT
// license; see third_party/HableMSAA/{LICENSE,README.md}. This is the temporal
// algorithm described by "Temporal Super Resolution via Multisampling", not
// the earlier spatial absolute-gradient snippet. Scene-linear and unexposed:
// exposure, bloom and display encoding belong to PostProcessPass.

struct ReconstructionParameters {
    float4x4 m_current_inverse_view_projection;
    float4x4 m_previous_view_projection;
    int2 m_resolved_extent;
    int2 m_raster_extent;
    int2 m_backing_extent;
    int2 m_window_extent;
    int2 m_previous_resolved_extent;
    int2 m_previous_backing_extent;
    int2 m_previous_window_extent;
    float2 m_jitter;
    uint m_pixel_scale;
    uint m_previous_pixel_scale;
    uint m_history_valid;
    uint m_reserved0;
    float2 m_reserved_padding;
    float2 m_reserved_padding1;
};
ConstantBuffer<ReconstructionParameters> g_reconstruction;

Texture2DMS<float4, 4> g_scene;
Texture2D<float4> g_history_previous;
RWTexture2D<float4> g_history_next;

// Standard 4x pattern in raster top-left pixel coordinates, in sample-index
// order. Node (1,0) is sample 0 at (3/8,1/8), and so on.
float2 sampleOffset(int index) {
    if (index == 0) { return float2(3.0 / 8.0, 1.0 / 8.0); }
    if (index == 1) { return float2(7.0 / 8.0, 3.0 / 8.0); }
    if (index == 2) { return float2(1.0 / 8.0, 5.0 / 8.0); }
    return float2(5.0 / 8.0, 7.0 / 8.0);
}

int2 sampleNode(int index) {
    if (index == 0) { return int2(1, 0); }
    if (index == 1) { return int2(3, 1); }
    if (index == 2) { return int2(0, 2); }
    return int2(2, 3);
}

int floorDiv(int value, int divisor) {
    // Do not rely on the sign convention of `%`: derive the remainder from
    // the quotient so truncating and flooring division both normalize.
    int quotient = value / divisor;
    int remainder = value - quotient * divisor;
    if (remainder < 0) { --quotient; }
    return quotient;
}

int floorMod(int value, int divisor) {
    return value - floorDiv(value, divisor) * divisor;
}

float luminance(float3 color) {
    return dot(color, float3(0.25, 0.5, 0.25));
}

int knownSampleIndex(int2 node) {
    int rx = floorMod(node.x, 4);
    int ry = floorMod(node.y, 4);
    [unroll]
    for (int index = 0; index < 4; ++index) {
        int2 known = sampleNode(index);
        if (rx == known.x && ry == known.y) { return index; }
    }
    return -1;
}

// Row residue determines the one known sample X residue in that row; column
// residue determines the one known sample Y residue in that column.
int knownXResidueForRow(int row_residue) {
    [unroll]
    for (int index = 0; index < 4; ++index) {
        int2 known = sampleNode(index);
        if (known.y == row_residue) { return known.x; }
    }
    return 0;
}

int knownYResidueForColumn(int column_residue) {
    [unroll]
    for (int index = 0; index < 4; ++index) {
        int2 known = sampleNode(index);
        if (known.x == column_residue) { return known.y; }
    }
    return 0;
}

float2 stationaryPoint(int2 pixel, int sample, float2 jitter) {
    return 2.0 * (float2(pixel) + sampleOffset(sample) - jitter);
}

bool sourceSampleValid(int2 pixel, int sample, out float2 stationary) {
    stationary = stationaryPoint(pixel, sample, g_reconstruction.m_jitter);
    return pixel.x >= 0 && pixel.y >= 0 && pixel.x < g_reconstruction.m_raster_extent.x &&
           pixel.y < g_reconstruction.m_raster_extent.y && stationary.x >= 0.0 &&
           stationary.x < float(g_reconstruction.m_resolved_extent.x) && stationary.y >= 0.0 &&
           stationary.y < float(g_reconstruction.m_resolved_extent.y);
}

// Nearest valid sample among the clamped source pixel and its clipped 3x3
// neighbourhood, measured in stationary resolved space. Ties use source
// row-major pixel then sample index. Positive extents always provide a
// candidate (including R=1); false reports a contract error.
bool nearestValidSample(int2 requested_pixel, int requested_sample, out int2 pixel, out int sample) {
    const float2 requested = stationaryPoint(
        requested_pixel, requested_sample, g_reconstruction.m_jitter);
    const int2 centre = clamp(requested_pixel, int2(0), g_reconstruction.m_raster_extent - int2(1));

    float best_distance = 3.402823466e+38;
    int best_order = 0x7fffffff;
    pixel = int2(0);
    sample = 0;
    bool found = false;
    [unroll]
    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
        [unroll]
        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
            const int2 candidate_pixel = centre + int2(offset_x, offset_y);
            if (candidate_pixel.x < 0 || candidate_pixel.y < 0 ||
                candidate_pixel.x >= g_reconstruction.m_raster_extent.x ||
                candidate_pixel.y >= g_reconstruction.m_raster_extent.y) {
                continue;
            }

            [unroll]
            for (int candidate_sample = 0; candidate_sample < 4; ++candidate_sample) {
                float2 stationary;
                if (!sourceSampleValid(candidate_pixel, candidate_sample, stationary)) { continue; }

                const float2 delta = stationary - requested;
                const float distance = dot(delta, delta);
                const int order = candidate_pixel.y * g_reconstruction.m_raster_extent.x + candidate_pixel.x;
                if (distance < best_distance || (distance == best_distance && order < best_order)) {
                    best_distance = distance;
                    best_order = order;
                    pixel = candidate_pixel;
                    sample = candidate_sample;
                    found = true;
                }
            }
        }
    }
    return found;
}

bool resolveValidSample(int2 pixel, int sample, out int2 resolved_pixel, out int resolved_sample) {
    float2 stationary;
    if (sourceSampleValid(pixel, sample, stationary)) {
        resolved_pixel = pixel;
        resolved_sample = sample;
        return true;
    }
    return nearestValidSample(pixel, sample, resolved_pixel, resolved_sample);
}

bool resolveKnownNode(int2 node, out float3 color) {
    color = float3(0.0, 0.0, 0.0);
    const int known = knownSampleIndex(node);
    if (known < 0) { return false; }

    int2 requested = int2(floorDiv(node.x, 4), floorDiv(node.y, 4));
    int2 pixel;
    int sample;
    if (!resolveValidSample(requested, known, pixel, sample)) { return false; }
    color = g_scene.Load(pixel, sample).rgb;
    return true;
}

// One lattice node. Known nodes load their sample directly; otherwise the
// closest known node on each side of the row and column supplies L,R,U,D.
// Bounds union every contributing original sample, including both gradients.
bool reconstructNode(int2 node, out float3 color, out float3 minimum, out float3 maximum) {
    color = float3(0.0, 0.0, 0.0);
    minimum = float3(0.0, 0.0, 0.0);
    maximum = float3(0.0, 0.0, 0.0);
    const int known = knownSampleIndex(node);
    if (known >= 0) {
        if (!resolveKnownNode(node, color)) { return false; }
        minimum = color;
        maximum = color;
        return true;
    }

    const int row_residue = floorMod(node.y, 4);
    const int column_residue = floorMod(node.x, 4);
    const int x_residue = knownXResidueForRow(row_residue);
    const int y_residue = knownYResidueForColumn(column_residue);
    const int left_x = node.x - floorMod(node.x - x_residue, 4);
    const int up_y = node.y - floorMod(node.y - y_residue, 4);

    float3 left, right, up, down;
    if (!resolveKnownNode(int2(left_x, node.y), left) ||
        !resolveKnownNode(int2(left_x + 4, node.y), right) ||
        !resolveKnownNode(int2(node.x, up_y), up) ||
        !resolveKnownNode(int2(node.x, up_y + 4), down)) {
        return false;
    }

    const float horizontal = max(1.0e-5, abs(luminance(left) - luminance(right)));
    const float vertical = max(1.0e-5, abs(luminance(up) - luminance(down)));
    const float weight = vertical / (horizontal + vertical);
    color = weight * 0.5 * (left + right) + (1.0 - weight) * 0.5 * (up + down);

    minimum = min(min(left, right), min(up, down));
    maximum = max(max(left, right), max(up, down));
    return true;
}

// Resolved pixel reconstruction: four lattice nodes with the projection's
// +4j shift removed, averaged. Their original-sample bounds are unioned for
// history clamping.
bool reconstructResolved(int2 pixel, out float3 color, out float3 minimum, out float3 maximum) {
    const int2 jitter_shift = int2(int(4.0 * g_reconstruction.m_jitter.x),
                                   int(4.0 * g_reconstruction.m_jitter.y));
    const int2 base = int2(2 * pixel.x + jitter_shift.x, 2 * pixel.y + jitter_shift.y);

    color = float3(0.0, 0.0, 0.0);
    minimum = float3(3.402823466e+38, 3.402823466e+38, 3.402823466e+38);
    maximum = float3(-3.402823466e+38, -3.402823466e+38, -3.402823466e+38);
    [unroll]
    for (int dy = 0; dy < 2; ++dy) {
        [unroll]
        for (int dx = 0; dx < 2; ++dx) {
            float3 node_color;
            float3 node_minimum;
            float3 node_maximum;
            if (!reconstructNode(base + int2(dx, dy), node_color, node_minimum, node_maximum)) {
                return false;
            }
            color += node_color;
            minimum = min(minimum, node_minimum);
            maximum = max(maximum, node_maximum);
        }
    }
    color *= 0.25;
    return true;
}

// Current unjittered world-plane position of a resolved pixel centre, then the
// previous accepted unjittered camera/padded-grid mapping.
bool reprojectPrevious(float2 current_pixel, out float2 previous_pixel) {
    previous_pixel = float2(0.0, 0.0);
    const float px = float(g_reconstruction.m_pixel_scale);
    const float2 ndc = float2(
        2.0 * px * current_pixel.x / float(g_reconstruction.m_window_extent.x) - 1.0,
        1.0 - 2.0 * px * current_pixel.y / float(g_reconstruction.m_window_extent.y));

    float4 near_world = mul(float4(ndc, 0.0, 1.0), g_reconstruction.m_current_inverse_view_projection);
    float4 far_world = mul(float4(ndc, 1.0, 1.0), g_reconstruction.m_current_inverse_view_projection);
    if (abs(near_world.w) < 1.0e-12 || abs(far_world.w) < 1.0e-12) { return false; }
    near_world /= near_world.w;
    far_world /= far_world.w;

    const float depth_delta = far_world.z - near_world.z;
    if (abs(depth_delta) < 1.0e-12) { return false; }
    const float t = -near_world.z / depth_delta;
    const float3 world = near_world.xyz + t * (far_world.xyz - near_world.xyz);

    const float4 previous_clip = mul(float4(world, 1.0), g_reconstruction.m_previous_view_projection);
    if (previous_clip.w <= 1.0e-12) { return false; }
    const float2 previous_ndc = previous_clip.xy / previous_clip.w;
    const float previous_scale = float(g_reconstruction.m_previous_pixel_scale);
    previous_pixel = float2(
        (previous_ndc.x + 1.0) * 0.5 * float(g_reconstruction.m_previous_window_extent.x) / previous_scale,
        (1.0 - previous_ndc.y) * 0.5 * float(g_reconstruction.m_previous_window_extent.y) /
            previous_scale);
    return isfinite(previous_pixel.x) && isfinite(previous_pixel.y);
}

// Bilinear prior history only when every nonzero-weight texel is inside the
// valid R rectangle. Invalid history is never clamped into the image.
bool sampleHistory(float2 previous_pixel, out float3 color) {
    const float2 centre = previous_pixel - 0.5;
    const int2 base = int2(int(floor(centre.x)), int(floor(centre.y)));
    const float2 fraction = centre - float2(base);
    const float2 weights_x = float2(1.0 - fraction.x, fraction.x);
    const float2 weights_y = float2(1.0 - fraction.y, fraction.y);

    color = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int y = 0; y < 2; ++y) {
        [unroll]
        for (int x = 0; x < 2; ++x) {
            const float weight = weights_x[x] * weights_y[y];
            // Numerical dust below the resolved-texel scale must not turn a
            // stationary history sample into an out-of-range footprint.
            if (weight <= 1.0e-4) { continue; }

            const int2 texel = base + int2(x, y);
            if (texel.x < 0 || texel.y < 0 || texel.x >= g_reconstruction.m_resolved_extent.x ||
                texel.y >= g_reconstruction.m_resolved_extent.y) {
                return false;
            }
            color += weight * g_history_previous.Load(int3(texel, 0)).rgb;
        }
    }
    return true;
}

[numthreads(8, 8, 1)]
void reconstructTemporal(uint3 id : SV_DispatchThreadID) {    if (any(id.xy >= uint2(g_reconstruction.m_backing_extent))) { return; }
    const int2 pixel = int2(id.xy);

    float3 current;
    float3 minimum;
    float3 maximum;
    if (!reconstructResolved(pixel, current, minimum, maximum)) {
        // Positive extents and the stated pattern always provide a candidate;
        // a miss is a contract error and is marked invalid.
        g_history_next[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float2 current_pixel = float2(pixel) + 0.5;
    float3 result = current;
    if (g_reconstruction.m_history_valid != 0) {
        float2 previous_pixel;
        float3 history;
        if (reprojectPrevious(current_pixel, previous_pixel) && sampleHistory(previous_pixel, history)) {
            const float motion = length(previous_pixel - current_pixel);
            const float motion_t = saturate((motion - 0.1) / 0.4);
            const float current_weight = lerp(0.05, 0.25, motion_t);
            result = current * current_weight + clamp(history, minimum, maximum) * (1.0 - current_weight);
        }
    }

    const bool valid = pixel.x < g_reconstruction.m_resolved_extent.x &&
                       pixel.y < g_reconstruction.m_resolved_extent.y;
    g_history_next[pixel] = float4(result, valid ? 1.0 : 0.0);
}
