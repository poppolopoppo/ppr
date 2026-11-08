// Shared noise-free contour primitives. Tables are uploaded from engine.math.
// Intersection/score arithmetic and ties are shared. Endpoint means use the
// explicit binary32 RTE coefficient contract below, independently of FP mode.

struct ContourTriangle {
    uint m_a;
    uint m_b;
    uint m_c;
    uint m_solid_corners;
};
struct ContourCase {
    uint m_segment_count;
    uint m_triangle_count;
    uint2 m_segments[2];
    ContourTriangle m_triangles[3];
};
StructuredBuffer<float2> g_contour_points;
StructuredBuffer<ContourCase> g_contour_cases;

// ------------------------------------------------------------------
// canonical endpoint coefficient for finite nonnegative roundness in [0,1]
// ------------------------------------------------------------------

uint contourRoundnessShiftJam(uint value, uint distance) {
    if (distance == 0) return value;
    if (distance >= 32) return value != 0 ? 1u : 0u;

    return (value >> distance) | ((value & ((1u << distance) - 1u)) != 0 ? 1u : 0u);
}

uint contourRoundnessMeanBits(uint a, uint b) {
    uint first = a & 0x7fffffffu, second = b & 0x7fffffffu;
    if (first > 0x3f800000u || second > 0x3f800000u ||
        ((a & 0x80000000u) != 0 && first != 0) || ((b & 0x80000000u) != 0 && second != 0))
        return 0x7fc00000u; // Invalid authored input; never a clamped coefficient.

    uint large = max(first, second), small = min(first, second);
    uint largeExponent = max(large >> 23, 1u), smallExponent = max(small >> 23, 1u);
    uint largeSignificand = (large & 0x007fffffu) | (large >= 0x00800000u ? 0x00800000u : 0u);
    uint smallSignificand = (small & 0x007fffffu) | (small >= 0x00800000u ? 0x00800000u : 0u);

    // Three low guard/round/sticky bits suffice for one positive sum. The sum
    // is below 2^28, so uint addition cannot overflow. Halving changes exponent.
    uint sum = (largeSignificand << 3) +
        contourRoundnessShiftJam(smallSignificand << 3, largeExponent - smallExponent);
    uint exponent = largeExponent - 1u;
    if (sum >= 0x08000000u) {
        sum = contourRoundnessShiftJam(sum, 1u);
        ++exponent;
    }
    if (exponent == 0) sum = contourRoundnessShiftJam(sum, 1u);

    uint significand = sum >> 3;
    uint remainder = sum & 7u;
    if (remainder > 4u || (remainder == 4u && (significand & 1u) != 0)) ++significand;

    // Subnormal rounding can carry directly into the smallest normal encoding.
    // Zero is canonical +0, and no floating operation can flush encoded bits.
    if (exponent == 0) return significand;
    if (significand >= 0x01000000u) { significand >>= 1; ++exponent; }
    return (exponent << 23) | (significand & 0x007fffffu);
}

float contourMeanRoundness(float a, float b) {
    return asfloat(contourRoundnessMeanBits(asuint(a), asuint(b)));
}

// ------------------------------------------------------------------
// quadratic containment, closest-point scores and label topology
// ------------------------------------------------------------------

float contourCross(float2 a, float2 b) {
    return a.x * b.y - a.y * b.x;
}

// Inverse of the per-axis quadratic map. Zero roundness keeps straight
// midpoint cuts; one reproduces the original rounded Bezier boundary.
float2 contourWarp(float2 point, float roundness) {
    float2 lower = min(point, 1.0 - point);
    float scale = 1.0 + roundness;
    float2 mapped = 2.0 * lower / (scale + sqrt(max(scale * scale - 8.0 * roundness * lower, 0.0)));
    return select(point <= 0.5, mapped, 1.0 - mapped);
}

float roundedSegmentDistance(float2 point, uint2 segment, float roundness, out float2 nearest, out float2 normal) {
    float2 a = g_contour_points[segment.x];
    float2 b = g_contour_points[segment.y];
    float2 delta = b - a;
    float t = saturate(dot(point - a, delta) / dot(delta, delta));
    nearest = a + t * delta;
    normal = float2(-delta.y, delta.x);
    if (roundness != 0.0 && delta.x != 0.0 && delta.y != 0.0) {
        float2 control = lerp((a + b) * 0.5, float2(0.5), roundness);
        float2 acceleration = 2.0 * (a - 2.0 * control + b);
        float best = min(dot(point - a, point - a), dot(point - b, point - b));
        float best_t = dot(point - a, point - a) < dot(point - b, point - b) ? 0.0 : 1.0;
        // Bounded closest-point solves from both ends and the middle avoid
        // selecting a local maximum on the inside of a convex arc.
        for (uint seed = 0u; seed < 3u; ++seed) {
            t = 0.5 * float(seed);
            for (uint iteration = 0u; iteration < 6u; ++iteration) {
                float2 curve = lerp(lerp(a, control, t), lerp(control, b, t), t);
                float2 tangent = 2.0 * lerp(control - a, b - control, t);
                float numerator = dot(curve - point, tangent);
                float denominator = dot(tangent, tangent) + dot(curve - point, acceleration);
                if (abs(denominator) > 1.0e-8) {
                    t = saturate(t - clamp(numerator / denominator, -0.25, 0.25));
                }
            }
            float2 curve = lerp(lerp(a, control, t), lerp(control, b, t), t);
            float distance = dot(point - curve, point - curve);
            if (distance < best) { best = distance; best_t = t; }
        }
        nearest = lerp(lerp(a, control, best_t), lerp(control, b, best_t), best_t);
        float2 tangent = lerp(control - a, b - control, best_t);
        normal = float2(-tangent.y, tangent.x);
    }
    normal /= max(length(normal), 1.0e-6);
    return length(point - nearest);
}

bool contourContains(uint mask, float2 point) {
    ContourCase shape = g_contour_cases[mask];
    for (uint index = 0u; index < shape.m_triangle_count; ++index) {
        ContourTriangle triangle = shape.m_triangles[index];
        float2 a = g_contour_points[triangle.m_a];
        float2 b = g_contour_points[triangle.m_b];
        float2 c = g_contour_points[triangle.m_c];
        if (contourCross(b - a, point - a) >= 0.0 &&
            contourCross(c - b, point - b) >= 0.0 &&
            contourCross(a - c, point - c) >= 0.0) {
            return true;
        }
    }
    return false;
}

// Label shapes provide continuous scores; their maximum partitions the entire
// occupied region, including gaps between independently rounded label shapes.
uint materialMask(uint materials[4], uint corner) {
    uint mask = 0u;
    for (uint other = 0u; other < 4u; ++other) {
        if (materials[other] == materials[corner]) { mask |= 1u << other; }
    }
    return mask;
}

// A saddle cannot connect both opposing materials in the plane. The lower
// solid ID wins that tie; a repeated solid always connects across empty corners.
// Use the opposite case's boundary and complement only for that saddle label.
bool connectedSaddle(uint materials[4], uint corner, uint mask) {
    if (mask != 5u && mask != 10u) { return false; }
    uint other = materials[(corner + 1u) & 3u];
    uint opposite = materials[(corner + 3u) & 3u];
    return other != opposite || other == 0u || other == 0xffffu || materials[corner] < other;
}

// Signed scores preserve the existing rounded label contours while making
// their bisectors shared interfaces, even outside every individual region.
float contourScore(float2 point, uint mask, float roundness, bool connected, out float2 gradient) {
    ContourCase shape = g_contour_cases[mask];
    float distance = 1.0e30;
    float2 closest = point;
    float2 closest_normal = float2(0.0);
    for (uint edge = 0u; edge < shape.m_segment_count; ++edge) {
        float2 nearest, normal;
        float candidate = roundedSegmentDistance(point, shape.m_segments[edge], roundness, nearest, normal);
        if (candidate < distance) {
            distance = candidate;
            closest = nearest;
            closest_normal = normal;
        }
    }
    bool inside = contourContains(mask, contourWarp(point, roundness)) != connected;
    if (distance > 1.0e-6) {
        gradient = (inside ? 1.0 : -1.0) * (point - closest) / distance;
    } else {
        bool normal_inside = contourContains(mask, contourWarp(closest + 1.0e-4 * closest_normal, roundness)) != connected;
        gradient = normal_inside ? closest_normal : -closest_normal;
    }
    return inside ? distance : -distance;
}

uint occupiedMask(uint materials[4]) {
    uint mask = 0u;
    for (uint corner = 0u; corner < 4u; ++corner) {
        if (materials[corner] != 0u && materials[corner] != 0xffffu) { mask |= 1u << corner; }
    }
    return mask;
}

bool occupiedSaddleConnected(uint materials[4], uint mask) {
    return (mask == 5u && materials[0] == materials[2]) ||
        (mask == 10u && materials[1] == materials[3]);
}
