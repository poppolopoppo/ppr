// ------------------------------------------------------------------
// shared owned-receiver storage and visual-only continuation score
// ------------------------------------------------------------------

struct TerrainReceiverFeature {
    int baseX, baseY;
    uint curve, component;
    float visualX, visualY; // Never physical contact, transport, bounce or gameplay.
    // Formerly reserved offsets24/28: normalized visual-only outward direction.
    uint visualOutwardXBits, visualOutwardYBits;
};
struct TerrainReceiverResult { float meanR, meanG, meanB; uint status; };
struct TerrainReceiverAngular { float radianceR, radianceG, radianceB; uint status; };
struct TerrainReceiverDirection { float x, y; };
struct TerrainReceiverCellRange { uint offset, count; };
struct TerrainReceiverComponentRange { uint component, offset, count, cornerMask; };

// Capture admits only normalized axis/diagonal midpoint normals, whose nonzero
// components share a positive magnitude. Dot sign therefore equals the bounded
// integer sign-vector dot. Avoid contraction residuals at perpendicular faces;
// both signed zeros represent zero. This is not a general normal-dot facility.
int terrainReceiverVisualDirectionSign(uint bits) {
    return (bits & 0x7fffffffu) == 0u ? 0 : ((bits & 0x80000000u) != 0u ? -1 : 1);
}

bool terrainReceiverVisualSameFacing(TerrainReceiverFeature candidate, TerrainReceiverFeature anchor) {
    int x = terrainReceiverVisualDirectionSign(candidate.visualOutwardXBits) *
        terrainReceiverVisualDirectionSign(anchor.visualOutwardXBits);
    int y = terrainReceiverVisualDirectionSign(candidate.visualOutwardYBits) *
        terrainReceiverVisualDirectionSign(anchor.visualOutwardYBits);
    return x + y > 0;
}

// ------------------------------------------------------------------
// normalized uniform-elevation diffuse response
// ------------------------------------------------------------------

// Twice the uniform-elevation cosine integral. Angular consumers average K*L;
// no additional 1/pi is applied. B>=0 is the admitted front-facing normal.
float terrainReceiverDiffuseLift(float3 normal, float2 direction) {
    float A = dot(normal.xy, normalize(direction));
    float B = normal.z;
    if (B == 0.0) return A > 0.0 ? 1.5707963267948966 * A : 0.0;
    if (A >= 0.0) return B + 1.5707963267948966 * A;

    float magnitude = -A;
    if (B <= 0.25 * magnitude) {
        // x-atan(x)=x^3/3-x^5/5+...-x^13/13. Positive-factor
        // evaluation avoids cancellation; the alternating omitted term is
        // bounded by magnitude*x^15/15 (x<=.25), before FP32 rounding.
        float x = B / magnitude;
        if (x <= 0.25) {
            float square = x * x;
            float factor = 1.0 / 11.0 - square * (1.0 / 13.0);
            factor = 1.0 / 9.0 - square * factor;
            factor = 1.0 / 7.0 - square * factor;
            factor = 1.0 / 5.0 - square * factor;
            factor = 1.0 / 3.0 - square * factor;
            return (B * square) * factor;
        }
    }

    return B + A * atan2(B, magnitude);
}

// Declared FP32 score, not an exact arbitrary-pixel geometric predicate.
// Candidate construction encloses either legal rounding direction/contraction.
// Callers break equal computed scores by the smaller stable feature key.
float terrainReceiverVisualScore(float2 position, TerrainReceiverFeature feature) {
    float dx = position.x - feature.visualX;
    float dy = position.y - feature.visualY;
    return dx * dx + dy * dy;
}
