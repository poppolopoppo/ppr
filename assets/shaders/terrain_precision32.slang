typedef float TerrainScalar;
typedef float2 TerrainInterval;

bool terrainNormal(float x) {
    uint bits = asuint(x) & 0x7fffffffu;
    return bits == 0 || (bits >= 0x00800000u && bits < 0x7f800000u);
}

float terrainNext(float x, bool up) {
    if (x == 0.0) return asfloat(up ? 1u : 0x80000001u);
    uint bits = asuint(x);
    return asfloat((x > 0.0) == up ? bits + 1u : bits - 1u);
}

float2 terrainNeg(float2 a) { return float2(asfloat(asuint(a.y) ^ 0x80000000u), asfloat(asuint(a.x) ^ 0x80000000u)); }

bool terrainSterbenzOperands(float a, float b) {
    uint first = asuint(a), second = asuint(b);
    uint low = min(first & 0x7fffffffu, second & 0x7fffffffu);
    uint high = max(first & 0x7fffffffu, second & 0x7fffffffu);
    // Incrementing a normal biased exponent multiplies its magnitude by two.
    // This integer test avoids rounded division and overflow in the premise.
    return ((first ^ second) & 0x80000000u) == 0 && low >= 0x00800000u &&
        high < 0x7f800000u && high <= low + 0x00800000u;
}

float2 terrainSqrt(float2 value, inout uint status) {
    if (!terrainNormal(value.x) || !terrainNormal(value.y) || !(value.x > 0.0)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0;
    }
    uint low = 0u, high = 0x5f7fffffu;
    // Search ordered positive float encodings. Values below 2^-63 square below
    // the smallest normal discriminant without evaluating a denormal product.
    for (uint i = 0; i < 32 && high - low > 1u; ++i) {
        uint middle = low + (high - low) / 2u;
        bool below = middle < 0x20000000u;
        if (!below) {
            float2 square = terrainMul(float2(asfloat(middle)), float2(asfloat(middle)), status);
            below = square.y <= value.x;
        }
        if (below) low = middle; else high = middle;
    }
    float lower = asfloat(low);
    low = 0u; high = 0x5f7fffffu;
    for (uint i = 0; i < 32 && high - low > 1u; ++i) {
        uint middle = low + (high - low) / 2u;
        bool above = false;
        if (middle >= 0x20000000u) {
            float2 square = terrainMul(float2(asfloat(middle)), float2(asfloat(middle)), status);
            above = square.x >= value.y;
        }
        if (above) high = middle; else low = middle;
    }
    float upper = asfloat(high);
    // Verify the upper endpoint, including the largest normal-square range.
    float2 square = terrainMul(float2(upper), float2(upper), status);
    if (square.x < value.y || !(upper >= lower)) status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
    return float2(lower, upper);
}

bool terrainZeroBits(float value) { return (asuint(value) & 0x7fffffffu) == 0; }
bool terrainEqualBits(float a, float b) { return asuint(a) == asuint(b); }
float terrainSignNeg(float value) { return asfloat(asuint(value) ^ 0x80000000u); }
float terrainDivMaximum() { return asfloat(0x7e800000u); }
bool terrainNativePoint(float2 point, out float2 nativePoint) { nativePoint = point; return true; }

// A compile-time identity: FP32 retains each original arithmetic-domain guard.
bool terrainCanonicalNormal(float value) { return true; }

// ------------------------------------------------------------------
// canonical FP32 differing-source certificate
// ------------------------------------------------------------------

bool terrainSourceCertificate(uint labels[4], float roundness[4], float3 sources[4],
    float2 x, float2 y, out float3 source) {
    return terrainStraightSource(labels, roundness, sources, x, y, source);
}
