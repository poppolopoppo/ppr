// ------------------------------------------------------------------
// analytic terminal sky
// ------------------------------------------------------------------
// The terminal cascade closes every path: a miss returns the fixed sky-sector
// mean with zero remaining transmittance, never vacuum. The integral uses the
// approved analytic sky/sun constants, and the normalisation is
// 2*pi*intensity/(2*pi/directions) = intensity*directions, i.e. the mean
// radiance over the sector.
#ifndef PPR_WORLD_LIGHTING_SKY_SLANG
#define PPR_WORLD_LIGHTING_SKY_SLANG

// One turn in radians; Slang's core module does not define M_TAU.
static const float kSkyTwoPi = 6.28318530717958647692;

static const float3 kSkyColor = float3(0.2, 0.5, 1.0);
static const float3 kSunColor = float3(20.0, 10.0, 2.0);
static const float kSkySunAngle = 5.0;
static const float kSkySunSharpness = 64.0;
static const float kSkyIntensity = 5.0;

// Unwrapped interval integral; the caller supplies bounds whose width is one
// narrow sector, so at most one 2*pi seam split occurs.
float3 worldLightingSkySegment(float a0, float a1) {
    float sharpness = sqrt(kSkySunSharpness);
    float inverse_sharpness = 1.0 / sharpness;

    float3 result = kSkyColor * (a1 - a0 - 0.5 * (cos(a1) - cos(a0)));
    result += kSunColor * (atan(sharpness * (kSkySunAngle - a0)) - atan(sharpness * (kSkySunAngle - a1))) *
              inverse_sharpness;
    return result / 6.0;
}

// Integrate [angle.x, angle.y) without sorting or reordering the endpoints:
// the bounds are unwrapped by whole turns first, then split once at 2*pi so a
// seam-crossing narrow sector keeps its narrow extent instead of becoming its
// complement.
float3 worldLightingSkyIntegrate(float2 angle) {
    float turns = floor(angle.x / kSkyTwoPi);
    float a0 = angle.x - turns * kSkyTwoPi;
    float a1 = angle.y - turns * kSkyTwoPi;

    if (a1 <= kSkyTwoPi) return worldLightingSkySegment(a0, a1);
    return worldLightingSkySegment(a0, kSkyTwoPi) + worldLightingSkySegment(0.0, a1 - kSkyTwoPi);
}

// One fixed stored-sector mean. Negative hours are the black boundary sky; 0..23
// rotate the whole sky by 2*pi*hour/24. The rotation is immutable for a frame
// and independent of simulation, presentation and accumulation time.
float3 worldLightingSkySectorMean(uint sector, uint directions, int hour) {
    if (hour < 0) return float3(0.0);

    float width = kSkyTwoPi / float(directions);
    float rotation = kSkyTwoPi * float(hour) / 24.0;
    float2 bounds = float2(float(sector), float(sector + 1u)) * width + rotation;
    return worldLightingSkyIntegrate(bounds) * (kSkyTwoPi * kSkyIntensity / width);
}

#endif
