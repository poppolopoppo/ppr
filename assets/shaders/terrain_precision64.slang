// Hardware-qualified experimental policy for RTX2080 / NVIDIA615.78.08 /
// Slang2026.17.1 / Slang-RHIa78e208. Native one-neighbor add/mul and eight-
// neighbor division bounds are engineering assumptions over the qualified normal
// domain, not universal Vulkan guarantees. RTE64 is requested by each entry.
// Neither FP64 denorm execution mode is supported: reject every subnormal risk.
typedef double TerrainScalar;
typedef double2 TerrainInterval;

// ------------------------------------------------------------------
// representation helpers: two uint words, no new Int64 requirement
// ------------------------------------------------------------------

uint2 terrainBits(double value) {
    uint low, high;
    asuint(value, low, high);
    return uint2(low, high);
}

bool terrainZeroBits(double value) {
    uint2 bits = terrainBits(value);
    return bits.x == 0 && (bits.y & 0x7fffffffu) == 0;
}

bool terrainEqualBits(double a, double b) { return all(terrainBits(a) == terrainBits(b)); }

double terrainSignNeg(double value) {
    uint2 bits = terrainBits(value);
    return asdouble(bits.x, bits.y ^ 0x80000000u);
}

bool terrainNormal(double value) {
    uint2 bits = terrainBits(value);
    uint magnitudeHigh = bits.y & 0x7fffffffu;
    bool zero = magnitudeHigh == 0 && bits.x == 0;
    return zero || (magnitudeHigh >= 0x00100000u && magnitudeHigh < 0x7ff00000u);
}

uint2 terrainWordsAdd(uint2 a, uint2 b) {
    uint low = a.x + b.x;
    return uint2(low, a.y + b.y + (low < a.x ? 1u : 0u));
}

uint2 terrainWordsSub(uint2 a, uint2 b) {
    return uint2(a.x - b.x, a.y - b.y - (a.x < b.x ? 1u : 0u));
}

uint2 terrainWordsHalf(uint2 value) { return uint2((value.x >> 1) | (value.y << 31), value.y >> 1); }
bool terrainWordsLess(uint2 a, uint2 b) { return a.y < b.y || (a.y == b.y && a.x < b.x); }
bool terrainWordsAdjacent(uint2 low, uint2 high) { return all(terrainWordsSub(high, low) <= uint2(1u, 0u)); }

double terrainNext(double value, bool up) {
    if (terrainZeroBits(value)) return asdouble(1u, up ? 0u : 0x80000000u);

    uint2 bits = terrainBits(value);
    bits = (value > 0.0) == up ? terrainWordsAdd(bits, uint2(1u, 0u)) : terrainWordsSub(bits, uint2(1u, 0u));
    return asdouble(bits.x, bits.y);
}

double2 terrainNeg(double2 interval) {
    return double2(terrainSignNeg(interval.y), terrainSignNeg(interval.x));
}

bool terrainSterbenzOperands(double a, double b) {
    uint2 first = terrainBits(a), second = terrainBits(b);
    if (((first.y ^ second.y) & 0x80000000u) != 0 || !terrainNormal(a) || !terrainNormal(b) ||
        terrainZeroBits(a) || terrainZeroBits(b)) return false;

    first.y &= 0x7fffffffu;
    second.y &= 0x7fffffffu;
    uint2 low = terrainWordsLess(first, second) ? first : second;
    uint2 high = terrainWordsLess(first, second) ? second : first;
    // Magnitudes are positive normal finite encodings. One biased-exponent
    // increment doubles the smaller; the integer sum cannot overflow uint64.
    uint2 twice = terrainWordsAdd(low, uint2(0u, 0x00100000u));
    return !terrainWordsLess(twice, high);
}

double terrainDivMaximum() { return asdouble(0u, 0x7fd00000u); }

bool terrainNativePoint(double2 point, out float2 nativePoint) {
    nativePoint = float2(point);
    // A genuinely new F64 singleton is not silently rounded to a free F32 point.
    // The declared native raster score/tie rule remains the original one.
    if (!terrainCanonicalInputNormal(nativePoint.x) || !terrainCanonicalInputNormal(nativePoint.y)) return false;

    return all(isfinite(nativePoint)) && all(double2(nativePoint) == point);
}

// ------------------------------------------------------------------
// squared-bracket square root, same algorithm over ordered binary64 encodings
// ------------------------------------------------------------------

double2 terrainSqrt(double2 value, inout uint status) {
    if (!terrainNormal(value.x) || !terrainNormal(value.y) || !(value.x > 0.0)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
        return 0.0;
    }

    uint2 low = uint2(0u), high = uint2(0xffffffffu, 0x5fefffffu);
    for (uint i = 0; i < 64 && !terrainWordsAdjacent(low, high); ++i) {
        uint2 middle = terrainWordsAdd(low, terrainWordsHalf(terrainWordsSub(high, low)));
        bool below = terrainWordsLess(middle, uint2(0u, 0x20000000u));
        if (!below) {
            double scalar = asdouble(middle.x, middle.y);
            double2 square = terrainMul(double2(scalar), double2(scalar), status);
            below = square.y <= value.x;
        }
        if (below) low = middle; else high = middle;
    }
    double lower = asdouble(low.x, low.y);

    low = uint2(0u);
    high = uint2(0xffffffffu, 0x5fefffffu);
    for (uint i = 0; i < 64 && !terrainWordsAdjacent(low, high); ++i) {
        uint2 middle = terrainWordsAdd(low, terrainWordsHalf(terrainWordsSub(high, low)));
        bool above = false;
        if (!terrainWordsLess(middle, uint2(0u, 0x20000000u))) {
            double scalar = asdouble(middle.x, middle.y);
            double2 square = terrainMul(double2(scalar), double2(scalar), status);
            above = square.x >= value.y;
        }
        if (above) high = middle; else low = middle;
    }
    double upper = asdouble(high.x, high.y);

    double2 square = terrainMul(double2(upper), double2(upper), status);
    if (square.x < value.y || !(upper >= lower)) status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
    return double2(lower, upper);
}

// Canonical binary32 values must pass their original input-domain guard BEFORE
// promotion can hide subnormal input encodings.
bool terrainCanonicalNormal(float value) { return terrainCanonicalInputNormal(value); }

// ------------------------------------------------------------------
// outward coordinate bridge into the actual canonical FP32 source rule
// ------------------------------------------------------------------

bool terrainSourceFp32Interval(double2 coordinate, out float2 canonical) {
    canonical = 0.0;
    if (!terrainNormal(coordinate.x) || !terrainNormal(coordinate.y) || coordinate.x > coordinate.y)
        return false;

    // The canonical score/warp runs in FP32. A binary64 enclosure alone cannot
    // certify that rule's rounding or corner-order tie, even strictly beside a
    // geometric partition. Reject conversion range/domain risks explicitly.
    double maximum = double(asfloat(0x7f7fffffu));
    if (coordinate.x < -maximum || coordinate.y > maximum) return false;

    float lower = float(coordinate.x), upper = float(coordinate.y);
    if (!terrainCanonicalInputNormal(lower) || !terrainCanonicalInputNormal(upper)) return false;

    // Exact normal FP32-to-FP64 promotion compares the candidate endpoints with
    // the ORIGINAL complete enclosure. Neighbor steps only expand; no epsilon,
    // midpoint, clamp or inward-rounding assumption may select a source winner.
    if (double(lower) > coordinate.x) lower = Terrain32::terrainNext(lower, false);
    if (double(upper) < coordinate.y) upper = Terrain32::terrainNext(upper, true);

    if (!terrainCanonicalInputNormal(lower) || !terrainCanonicalInputNormal(upper)) return false;
    if (double(lower) > coordinate.x || double(upper) < coordinate.y || lower > upper) return false;

    canonical = float2(lower, upper);
    return true;
}

bool terrainSourceCertificate(uint labels[4], float roundness[4], float3 sources[4],
    double2 x, double2 y, out float3 source) {
    source = 0.0;
    float2 canonicalX, canonicalY;
    if (!terrainSourceFp32Interval(x, canonicalX) || !terrainSourceFp32Interval(y, canonicalY)) return false;

    // Reuse the actual FP32 certificate over every canonical coordinate that
    // the full FP64 enclosure may round to. Its original FP32 warp/containment,
    // positive-separation and tie safeguards remain the source-admission rule.
    return Terrain32::terrainStraightSource(labels, roundness, sources, canonicalX, canonicalY, source);
}
