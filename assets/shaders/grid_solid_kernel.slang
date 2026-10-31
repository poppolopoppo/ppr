// Equal-area world-XY octagonal grains. The stored particle radius represents
// a disk of area pi*r*r; converting to octagon apothem preserves that area.
// Keep splat geometry, visibility, occupancy and isolated-contour calibration
// on these shared constants. Support is deliberately separate from liquids.
static const float kSolidOctagonArea = 8.0 * (1.4142135623730951 - 1.0);
static const float kSolidApothemPerRadius = sqrt(3.141592653589793 / kSolidOctagonArea);
static const float kSolidSupport = 2.5;
static const float kSolidAmplitude = 4.0 / (kSolidSupport * kSolidSupport);
static const float kSolidRadiusLevel = 1.0 - 1.0 / (kSolidSupport * kSolidSupport);
static const float kSolidIsoLevel = kSolidAmplitude * kSolidRadiusLevel * kSolidRadiusLevel * kSolidRadiusLevel;

float solidOctagonDistance(float2 offset) {
    float2 magnitude = abs(offset);
    return max(max(magnitude.x, magnitude.y), (magnitude.x + magnitude.y) * 0.7071067811865476);
}
