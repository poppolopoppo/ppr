// ------------------------------------------------------------------
// shared committed-row coordinates and half-open native ownership
// ------------------------------------------------------------------
#ifndef PPR_GRID_PARTICLE_NATIVE_SLANG
#define PPR_GRID_PARTICLE_NATIVE_SLANG

// Mutable 64-byte solver row, field-for-field with the CPU twin
// `matter::ParticleWrite` (lib/engine/matter/Matter.Gpu.Types.cppm): position,
// per-step displacement, the APIC F and D matrices (four floats each there),
// the liquid scalars in solver write order, and `enabled` last. layout=4 and
// stride 64; no field may be reordered without changing both sides.
struct NumericalRow {
    float positionX;
    float positionY;
    float displacementX;
    float displacementY;
    float4 deformationGradient;
    float4 deformationDisplacement;
    float liquidDensity;
    float logJp;
    float lambda;
    float enabled;
};
struct MetadataRow {
    uint idLow,idHigh,sourceRow,materialTemperature;
    float gasX,gasY,radius;
    uint physicalFillBits;
};
struct ParticleNativeCell { uint x,y,status; };

// ------------------------------------------------------------------
// presentation support, falloff and the two phase shapes
// ------------------------------------------------------------------
// One constant set, read by the prepare cull and the draw geometry so the two
// sides cannot drift: r is the packed physical radius h*sqrt(V/pi), R = 2.0 r is
// the support radius both phases share (the P1 decision), and the cull margin is
// 2.45 r so the optional 1.2x stretch (2.400 r) still fits.
//
// Edge: one C1 radial falloff for both phases, from the normalised radius
// rho = |p - centre| / R: a = 1 for rho <= 2/3, a = smoothstep(1, 2/3, rho) for
// 2/3 < rho < 1 and a = 0 beyond. The alpha = 0.5 contour is rho = 5/6. The
// seeded interior is saturated because every lattice point sits inside R_in.
//
// Solid: a rounded square inscribed in the same R circle. It is the Minkowski
// sum of a square of half-extent a-c and a disk of radius c, so its support
// radius is S = sqrt(2)*a - c*(sqrt(2)-1). With c = 0.25 a, S = 1.3106 a,
// therefore a = R/1.3106 = 1.5260 r (0.4305 cell at r = 0.2821 cell) and
// c = 0.25 a (0.1076 cell). Liquid: a disk of radius R, support exactly R.
//
// Every shape's support must stay at or below the 2.45 r margin, including the
// optional 1.2x stretch: max(S, 1.2 S) = 2.400 r <= 2.45 r. Rejected variants,
// kept so they cannot return unnoticed: a rounded square of half-extent a = R
// has support 2.621 r, the unrounded square of half-extent a = R has 2.828 r and
// the earlier 0.5-cell half-extent square has 2.507 r; all exceed the margin and
// would clip at the frustum edge.
static const float kParticleSupportScale = 2.0;
static const float kParticleCullRadiusScale = 2.45;
static const float kParticleStretchCap = 1.2;
static const float kParticleRoundedSquareSupport = 1.3106;
static const float kParticleSolidHalfExtentScale = kParticleSupportScale/kParticleRoundedSquareSupport;
static const float kParticleSolidCornerFraction = 0.25;
static const float kParticleSolidCornerScale = kParticleSolidCornerFraction*kParticleSolidHalfExtentScale;
static const float kParticleSolidInnerHalfExtentScale = kParticleSolidHalfExtentScale-kParticleSolidCornerScale;
static const float kParticleFalloffInnerFraction = 2.0/3.0;
static const float kParticleDiscardThreshold = 0.5;

// The caller proves the sourceRow before loading Hot. Gas never uses a Hot row.
// This is shared by census and immutable optical snapshot, before any culling.
ParticleNativeCell particleNativeCell(MetadataRow metadata,float2 movingPosition,float enabled,bool sourceValid,
    float cellMetres,float origin,float2 minimum,uint2 extent) {
    ParticleNativeCell result={0xffffffffu,0xffffffffu,0u};
    float2 native;
    if (metadata.sourceRow!=0xffffffffu) {
        if (!sourceValid) {result.status=1u;return result;}

        native=movingPosition-origin-minimum;
        if (!isfinite(enabled)) result.status|=1u;
        else if (enabled<=0) result.status|=4u;
    } else {
        native=float2(metadata.gasX,metadata.gasY)/cellMetres-minimum;
    }

    if (!all(isfinite(native))) result.status|=1u;
    else if (any(native<0) || any(native>=float2(extent))) result.status|=2u;
    else {
        uint2 cell=uint2(floor(native));
        result.x=cell.x;result.y=cell.y;
    }
    return result;
}
#endif
