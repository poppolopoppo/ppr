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
// 3.0625 r so the retained 1.5x flow stretch (3.000 r) still fits.
//
// Edge: one C1 radial falloff for both phases, from the normalised radius
// rho = |p - centre| / R: a = 1 for rho <= 2/3, a = smoothstep(1, 2/3, rho) for
// 2/3 < rho < 1 and a = 0 beyond. The resting alpha = 0.5 contour is rho = 5/6;
// the optional tangent cue moves that contour per angle inside [2/3, 1] only. The
// seeded interior is saturated because every lattice point sits inside R_in.
//
// Solid: a rounded square inscribed in the same R circle. It is the Minkowski
// sum of a square of half-extent a-c and a disk of radius c, so its support
// radius is S = sqrt(2)*a - c*(sqrt(2)-1). With c = 0.25 a, S = 1.3106 a,
// therefore a = R/1.3106 = 1.5260 r (0.4305 cell at r = 0.2821 cell) and
// c = 0.25 a (0.1076 cell). Liquid: a disk of radius R, support exactly R.
//
// Every shape's support must stay at or below the 3.0625 r margin, including the
// retained 1.5x flow stretch: max(S, 1.5 S) = 3.000 r <= 3.0625 r (2.08%
// headroom). Rejected variants, kept so they cannot return unnoticed: a rounded
// square of half-extent a = R has support 2.621 r, the unrounded square of
// half-extent a = R has 2.828 r and the earlier 0.5-cell half-extent square has
// 2.507 r. All three are larger than the adopted 2.0 r support, so they remain
// rejected as appearance (a bigger silhouette and more fill); the raised margin
// no longer makes them cull-infeasible, so they are recorded rather than gated.
//
// Retained flow cue (Task 4, appearance only): the drawn shape is stretched
// along the published per-step motion, major axis only, by
// k = 1 + (kParticleStretchCap - 1) * saturate(|motion| / kParticleMotionReference).
// The major semi-axis is therefore at most 1.5 R = 3.000 r, inside the 3.0625 r
// cull margin, and the minor semi-axis stays the un-stretched support R = 2.0 r.
// The reference magnitude is a fixed, illustrative curve shape in native cells
// per solver step (0.06, so ordinary flow saturates the cap), not a physical
// speed: the row's motion is per-step and this path introduces no dt. Since
// k >= 1 the stretched shape contains the un-stretched one, so the seeded
// interior of plan §5.2 stays saturated and the cross-flow (diagonal) support
// never shrinks.
static const float kParticleSupportScale = 2.0;
static const float kParticleCullRadiusScale = 3.0625;
static const float kParticleStretchCap = 1.5;
static const float kParticleMotionReference = 0.06;
// Tangent-aligned contour cue (Task 11 increment 1, appearance only). The
// alpha = 0.5 contour radius, in rho = |p| / R units, is
// k(theta) = kParticleTangentContourScale * (1 + kParticleTangentContourBias *
// (cos^2(theta) - 1/2)) with theta measured from the surface tangent read from
// the committed ownership mask, so the silhouette is longer along the surface and
// shorter across it. k0 = 5/6 is where the resting C1 falloff already crosses 0.5
// and b = 0.30 keeps k inside [0.708, 0.958], both ends inside [2/3, 1]: the
// plateau, the support radius R, the interior coverage and the diagonal seam are
// untouched for any orientation, so a wrong tangent costs appearance only and
// the 3.0625 r cull cannot be exceeded. Fixed source values, no tuning knob.
static const float kParticleTangentContourScale = 5.0/6.0;
static const float kParticleTangentContourBias = 0.30;
// Round-off closure of the two contour joins, relative to R. |local| is a rounded
// length, so a sample constructed exactly at the support radius can arrive a few
// ulps inside it; without this hair the warp would return a denormal alpha there
// instead of exactly 0. Closing it keeps the support boundary exactly alpha = 0
// and the plateau exactly alpha = 1 for every tangent. It is a numerical closure,
// not a shape or strength knob: eight binary32 ulps at rho = 1 (1.2e-7) and about
// 1e-6 R, i.e. 0.002 px at span64, far below anything visible.
static const float kParticleTangentJoinClosure = 1.0e-6;
// A gradient magnitude at or below this is "no surface": a zero test for the
// one-bit ownership mask, not a smoothing knob.
static const float kParticleTangentGradientEpsilon = 1.0e-4;
static const float kParticleRoundedSquareSupport = 1.3106;
static const float kParticleSolidHalfExtentScale = kParticleSupportScale/kParticleRoundedSquareSupport;
static const float kParticleSolidCornerFraction = 0.25;
static const float kParticleSolidCornerScale = kParticleSolidCornerFraction*kParticleSolidHalfExtentScale;
static const float kParticleSolidInnerHalfExtentScale = kParticleSolidHalfExtentScale-kParticleSolidCornerScale;
static const float kParticleFalloffInnerFraction = 2.0/3.0;
static const float kParticleDiscardThreshold = 0.5;

// ------------------------------------------------------------------
// committed particle ownership
// ------------------------------------------------------------------
// One rule for categorical base retirement, shared by the terrain fragment and
// its nonvisual probe: a native cell is particle-owned exactly when the
// committed projection recorded a candidate winner for it, i.e. the winner grid
// still holds the no-winner word 0xffffffff. Terrain wins precedence before
// nomination, so a terrain-owned cell always keeps that word. Material identity
// is never consulted: a dug particle can share its material id with the terrain
// in the same cell, and that terrain is still terrain-owned.
//
// The grid is a parameter rather than a global in this include: the projection
// owns an RW view of the same 4 MiB buffer while the source, terrain and probe
// readers own read-only views, and all of them include this header.
bool particleOwnsCell(uint cell, StructuredBuffer<uint> best_row) {
    return best_row[cell] != 0xffffffffu;
}

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
