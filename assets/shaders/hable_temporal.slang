// Copyright 2025 John Hable. Temporal reconstruction adapted under the MIT
// license; see third_party/HableMSAA/{LICENSE,README.md}. This is the temporal
// algorithm described by "Temporal Super Resolution via Multisampling", not
// the earlier spatial absolute-gradient snippet. Scene-linear and unexposed:
// exposure, bloom and display encoding belong to PostProcessPass.
//
// Two entries share one parameter block and one binding set:
//
//   reconstructTemporalInterior  the proven-interior rectangle. Its block
//                                residue selects one of four static stencils
//                                whose seven distinct source samples are
//                                loaded once and reused. No validation, no
//                                fallback search, no border code.
//   reconstructTemporalBorder    every other backing pixel, including B
//                                padding and every extent too small to have an
//                                interior. Unchanged generic lattice
//                                derivation with the nearest-valid fallback.
//
// ReconstructionPass computes the rectangle on the CPU from the actual phase,
// dispatches it first, then dispatches the border over the whole backing; the
// border entry returns early for pixels the interior entry owns.

struct ReconstructionParameters {
    float4x4 m_current_inverse_view_projection;
    float4x4 m_previous_view_projection;
    int2 m_resolved_extent;
    int2 m_raster_extent;
    int2 m_backing_extent;
    int2 m_window_extent;
    int2 m_previous_window_extent;
    float2 m_jitter;
    uint m_pixel_scale;
    uint m_previous_pixel_scale;
    uint m_history_valid;
    uint m_reserved0;
    int2 m_interior_origin;
    int2 m_interior_extent;
};
ConstantBuffer<ReconstructionParameters> g_reconstruction;

Texture2DMS<float4, 4> g_scene;
Texture2D<float4> g_history_previous;
RWTexture2D<float4> g_history_next;

// ------------------------------------------------------------------
// sample pattern and lattice arithmetic
// ------------------------------------------------------------------
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

// Shifted quarter-raster-pixel block base of one resolved pixel, with the
// projection's +4j displacement applied. Shared by both entries.
int2 latticeBase(int2 pixel) {
    const int2 jitter_shift = int2(int(4.0 * g_reconstruction.m_jitter.x),
                                   int(4.0 * g_reconstruction.m_jitter.y));
    return int2(2 * pixel.x + jitter_shift.x, 2 * pixel.y + jitter_shift.y);
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

// ------------------------------------------------------------------
// border sample resolution
// ------------------------------------------------------------------
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

// ------------------------------------------------------------------
// border generic lattice
// ------------------------------------------------------------------
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
    const int2 base = latticeBase(pixel);

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

// ------------------------------------------------------------------
// interior fixed stencil
// ------------------------------------------------------------------
// A block's residue (base & 3 per axis) fixes which of its four nodes is a
// known sample and which row/column brackets the other three need. Each
// residue therefore has one static stencil whose seven distinct source
// samples are loaded once and reused by every diamond and by the bounds.
// Origins are the block's floor-divided source pixel, so every load is
// `origin + <constant offset>` at a constant sample index.
//
// Residue tables, offsets relative to the block origin and sample index:
//
//   (0,0)  (-1,0)s0  (0,0)s0  (0,-1)s2  (0,0)s2  (-1,0)s1  (0,0)s1  (0,1)s0
//   (2,0)  (0,0)s0   (1,0)s0  (0,-1)s3  (0,0)s3  (0,-1)s1  (0,0)s1  (-1,0)s1
//   (0,2)  (0,0)s2   (1,0)s2  (0,0)s0   (0,1)s0  (-1,0)s3  (0,0)s3  (0,1)s2
//   (2,2)  (0,0)s2   (1,0)s2  (0,-1)s3  (0,0)s3  (0,0)s1   (0,1)s1  (1,0)s3
//
// Node order is the generic loop's: (0,0), (1,0), (0,1), (1,1) in (dx,dy).

struct Reconstructed {
    float3 m_color;
    float3 m_minimum;
    float3 m_maximum;
};

Reconstructed knownValue(float3 color) {
    Reconstructed value;
    value.m_color = color;
    value.m_minimum = color;
    value.m_maximum = color;
    return value;
}

// Same diamond as the generic node: the luminance gradient across the row and
// the column chooses the horizontal/vertical mean, and the bounds cover all
// four contributing original samples.
Reconstructed diamondValue(float3 left, float3 right, float3 up, float3 down) {
    const float horizontal = max(1.0e-5, abs(luminance(left) - luminance(right)));
    const float vertical = max(1.0e-5, abs(luminance(up) - luminance(down)));
    const float weight = vertical / (horizontal + vertical);

    Reconstructed value;
    value.m_color = weight * 0.5 * (left + right) + (1.0 - weight) * 0.5 * (up + down);
    value.m_minimum = min(min(left, right), min(up, down));
    value.m_maximum = max(max(left, right), max(up, down));
    return value;
}

Reconstructed accumulateNode(Reconstructed accumulated, Reconstructed node) {
    accumulated.m_color = accumulated.m_color + node.m_color;
    accumulated.m_minimum = min(accumulated.m_minimum, node.m_minimum);
    accumulated.m_maximum = max(accumulated.m_maximum, node.m_maximum);
    return accumulated;
}

// Four node values averaged, with their bounds unioned, in lattice order.
Reconstructed latticeValue(
    Reconstructed node0, Reconstructed node1, Reconstructed node2, Reconstructed node3) {
    Reconstructed result;
    result.m_color = float3(0.0, 0.0, 0.0);
    result.m_minimum = float3(3.402823466e+38, 3.402823466e+38, 3.402823466e+38);
    result.m_maximum = float3(-3.402823466e+38, -3.402823466e+38, -3.402823466e+38);

    result = accumulateNode(result, node0);
    result = accumulateNode(result, node1);
    result = accumulateNode(result, node2);
    result = accumulateNode(result, node3);
    result.m_color = result.m_color * 0.25;
    return result;
}

// Residue (0,0): known sample 0 at block node (1,0).
Reconstructed interiorResidue00(int2 origin) {
    const float3 d0 = g_scene.Load(origin + int2(-1, 0), 0).rgb;
    const float3 d1 = g_scene.Load(origin, 0).rgb;
    const float3 d2 = g_scene.Load(origin + int2(0, -1), 2).rgb;
    const float3 d3 = g_scene.Load(origin, 2).rgb;
    const float3 d4 = g_scene.Load(origin + int2(-1, 0), 1).rgb;
    const float3 d5 = g_scene.Load(origin, 1).rgb;
    const float3 d6 = g_scene.Load(origin + int2(0, 1), 0).rgb;

    return latticeValue(
        diamondValue(d0, d1, d2, d3),
        knownValue(d1),
        diamondValue(d4, d5, d2, d3),
        diamondValue(d4, d5, d1, d6));
}

// Residue (2,0): known sample 1 at block node (1,1).
Reconstructed interiorResidue20(int2 origin) {
    const float3 d0 = g_scene.Load(origin, 0).rgb;
    const float3 d1 = g_scene.Load(origin + int2(1, 0), 0).rgb;
    const float3 d2 = g_scene.Load(origin + int2(0, -1), 3).rgb;
    const float3 d3 = g_scene.Load(origin, 3).rgb;
    const float3 d4 = g_scene.Load(origin + int2(0, -1), 1).rgb;
    const float3 d5 = g_scene.Load(origin, 1).rgb;
    const float3 d6 = g_scene.Load(origin + int2(-1, 0), 1).rgb;

    return latticeValue(
        diamondValue(d0, d1, d2, d3),
        diamondValue(d0, d1, d4, d5),
        diamondValue(d6, d5, d2, d3),
        knownValue(d5));
}

// Residue (0,2): known sample 2 at block node (0,0).
Reconstructed interiorResidue02(int2 origin) {
    const float3 d0 = g_scene.Load(origin, 2).rgb;
    const float3 d1 = g_scene.Load(origin + int2(1, 0), 2).rgb;
    const float3 d2 = g_scene.Load(origin, 0).rgb;
    const float3 d3 = g_scene.Load(origin + int2(0, 1), 0).rgb;
    const float3 d4 = g_scene.Load(origin + int2(-1, 0), 3).rgb;
    const float3 d5 = g_scene.Load(origin, 3).rgb;
    const float3 d6 = g_scene.Load(origin + int2(0, 1), 2).rgb;

    return latticeValue(
        knownValue(d0),
        diamondValue(d0, d1, d2, d3),
        diamondValue(d4, d5, d0, d6),
        diamondValue(d4, d5, d2, d3));
}

// Residue (2,2): known sample 3 at block node (0,1).
Reconstructed interiorResidue22(int2 origin) {
    const float3 d0 = g_scene.Load(origin, 2).rgb;
    const float3 d1 = g_scene.Load(origin + int2(1, 0), 2).rgb;
    const float3 d2 = g_scene.Load(origin + int2(0, -1), 3).rgb;
    const float3 d3 = g_scene.Load(origin, 3).rgb;
    const float3 d4 = g_scene.Load(origin, 1).rgb;
    const float3 d5 = g_scene.Load(origin + int2(0, 1), 1).rgb;
    const float3 d6 = g_scene.Load(origin + int2(1, 0), 3).rgb;

    return latticeValue(
        diamondValue(d0, d1, d2, d3),
        diamondValue(d0, d1, d4, d5),
        knownValue(d3),
        diamondValue(d3, d6, d4, d5));
}

// Interior resolved pixel: the block residue selects its static stencil.
// Every referenced sample is valid by the dispatch contract, so this path
// never validates an address and cannot fail.
Reconstructed reconstructInterior(int2 pixel) {
    const int2 base = latticeBase(pixel);
    const int2 origin = int2(floorDiv(base.x, 4), floorDiv(base.y, 4));
    const uint residue = uint(((base.x >> 1) & 1) | (base.y & 2));

    if (residue == 0) { return interiorResidue00(origin); }
    if (residue == 1) { return interiorResidue20(origin); }
    if (residue == 2) { return interiorResidue02(origin); }
    return interiorResidue22(origin);
}

// ------------------------------------------------------------------
// temporal tail
// ------------------------------------------------------------------
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
            // Literal contract: only an exactly zero weight is ignored; any
            // nonzero out-of-R texel invalidates the history sample.
            if (weight <= 0.0) { continue; }

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

// Reprojected history clamp and blend, then the history write. Shared by both
// entries so the spatial split cannot change the temporal result.
void resolveTemporal(int2 pixel, float3 current, float3 minimum, float3 maximum) {
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

// ------------------------------------------------------------------
// entries
// ------------------------------------------------------------------

// Interior dispatch domain, relative to m_interior_origin. The CPU-side
// contract guarantees every stencil load is a valid sample.
[numthreads(8, 8, 1)]
void reconstructTemporalInterior(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(g_reconstruction.m_interior_extent))) { return; }

    const int2 pixel = g_reconstruction.m_interior_origin + int2(id.xy);
    const Reconstructed current = reconstructInterior(pixel);
    resolveTemporal(pixel, current.m_color, current.m_minimum, current.m_maximum);
}

// Every backing pixel the interior dispatch does not own: the border strips,
// B padding and all tiny extents. Unchanged generic lattice and fallback.
[numthreads(8, 8, 1)]
void reconstructTemporalBorder(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(g_reconstruction.m_backing_extent))) { return; }

    const int2 pixel = int2(id.xy);
    const int2 interior_offset = pixel - g_reconstruction.m_interior_origin;
    if (interior_offset.x >= 0 && interior_offset.y >= 0 &&
        interior_offset.x < g_reconstruction.m_interior_extent.x &&
        interior_offset.y < g_reconstruction.m_interior_extent.y) {
        return;
    }

    float3 current;
    float3 minimum;
    float3 maximum;
    if (!reconstructResolved(pixel, current, minimum, maximum)) {
        // Positive extents and the stated pattern always provide a candidate;
        // a miss is a contract error and is marked invalid.
        g_history_next[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    resolveTemporal(pixel, current, minimum, maximum);
}
