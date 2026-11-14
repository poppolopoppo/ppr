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
// 2/3 < rho < 1 and a = 0 beyond. The silhouette is this falloff composited
// premultiplied by both restored classes, so the shape is unchanged and only
// its rasterisation is analytically covered; the resting half-coverage contour
// is rho = 5/6, and the optional tangent cue moves that contour per angle inside
// [2/3, 1] only. The seeded interior's radial falloff is saturated because every
// lattice point sits inside R_in; the *final* solid coverage additionally
// multiplies the analytic SDF rim and the terrain complement, so it is exactly 1
// only where those leave the plateau untouched (about span <= 78 for a typical
// radius; at span220 the rim reaches into the plateau edge and that is the
// intended AA, not a defect).
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
// along the flow, major axis only, by
// k = 1 + (kParticleStretchCap - 1) * saturate(|motion| / kParticleMotionReference).
// `motion` is not the row's own published pair: the vertex stage samples the
// per-cell winner-motion field bilinearly at the particle centre (Task 4
// continuity fix), so neighbouring particles read overlapping filtered windows
// instead of their own post-contact displacements. The field is one value per
// native cell - the committed winner's motion - and it costs four bounded taps
// of the grids the draw already binds; no buffer, binding or dispatch is added.
// The major semi-axis is therefore at most 1.5 R = 3.000 r, inside the 3.0625 r
// cull margin, and the minor semi-axis stays the un-stretched support R = 2.0 r.
// The reference magnitude is a fixed, illustrative curve shape in native cells
// per solver step (0.06, so ordinary flow saturates the cap), not a physical
// speed: the row's motion is per-step and this path introduces no dt. Since
// k >= 1 the stretched shape contains the un-stretched one, so the radial
// falloff of the seeded interior (plan §5.2) stays saturated and the cross-flow
// (diagonal) support never shrinks.
static const float kParticleSupportScale = 2.0;
static const float kParticleCullRadiusScale = 3.0625;
static const float kParticleStretchCap = 1.5;
static const float kParticleMotionReference = 0.06;
// The band between the saturated plateau and the support: the radial falloff is
// exactly 1 for rho <= 2/3 and falls to 0 at rho = 1. Both contour cues below
// are legal only because they move the 0.5 coverage crossing inside this band
// and touch neither join.
static const float kParticleFalloffInnerFraction = 2.0/3.0;
// Tangent-aligned contour cue (Task 11 increment 1, appearance only). The
// half-coverage contour radius, in rho = |p| / R units, is
// k(theta) = kParticleTangentContourScale * (1 + b_eff *
// (cos^2(theta) - 1/2)) with theta measured from the surface tangent read from
// the committed per-cell Q16 fill field. b_eff = b * strength, where strength is
// the smoothstep energy gate above, so an ambiguous field renders round (b_eff =
// 0 and k(theta) = k0 exactly) while a genuine surface keeps the full bias. The
// premultiplied silhouette is longer along the surface and shorter across it.
// k0 = 5/6 is where the resting C1 falloff already reaches 0.5 coverage. b =
// 0.3999 is the strongest four-decimal bias whose two endpoints stay strictly
// inside the legal band in binary32, and b_eff never exceeds it:
//   k_max = k0 * (1 + b/2) = 0.999958277   tangent direction
//   k_min = k0 * (1 - b/2) = 0.666708350   normal direction
// i.e. a 1.49984 : 1 silhouette aspect (1.49971 : 1 after the clamp below).
// b = 0.40 is degenerate, not merely weaker: 0.83333331f * 0.80000001f rounds to
// exactly float32(2/3), so k_min would sit ON the plateau while the falloff warp
// in grid_tiles.slang divides by (contour - 2/3). k_min > 2/3 is therefore a
// *finite-band* requirement (the warp divides by the band offset), while
// k_max <= 1 is the *support* requirement (a contour past R would leave the
// 3.0625 r cull margin). Both ends stay inside [2/3, 1], so the radial falloff
// plateau, the support radius R, the interior coverage and the diagonal seam are
// untouched for any orientation: a wrong tangent costs appearance only. Fixed
// source values, no tuning knob; the endpoint assertion is at the top of
// grid_tiles.slang particleTangentFalloff and proves the maximum b_eff <= b.
static const float kParticleTangentContourScale = 5.0/6.0;
static const float kParticleTangentContourBias = 0.3999;
// Defensive clamp of the contour radius, in rho units: one 1e-4 hair above the
// plateau and the support exactly. With the maximum b = 0.3999 it is nearly
// inert - it bites only within about 0.76 degrees of the normal direction,
// raising k_min from 0.666708 to 0.666767 - and any weaker b_eff is more inert
// still. It removes any possibility of a zero denominator or of a contour at the
// support for any legal bias.
static const float kParticleTangentContourMinimum = kParticleFalloffInnerFraction+1.0e-4;
// Round-off closure of the two contour joins, relative to R. |local| is a rounded
// length, so a sample constructed exactly at the support radius can arrive a few
// ulps inside it; without this hair the warp would return a denormal coverage
// there instead of exactly 0. Closing it keeps the support boundary exactly 0
// coverage and the radial falloff plateau exactly 1 for every tangent. It is a
// numerical closure, not a shape or strength knob: eight binary32 ulps at rho = 1
// (1.2e-7) and about 1e-6 R, i.e. 0.002 px at span64, far below anything visible.
static const float kParticleTangentJoinClosure = 1.0e-6;
// A gradient magnitude at or below this is "no surface": the numerical guard
// against dividing by zero, not a confidence threshold. The fill field carries
// one particle-sized quantum (16384 of the word's 65536 units, a quarter of full
// scale, i.e. 0.25 cell-volume), and changing one stencil cell by that quantum
// moves the gradient by at most 0.5 in these units, far above this; a flat field
// (the dense interior, a paused world, an isolated particle) gives exactly zero.
static const float kParticleTangentGradientEpsilon = 1.0e-4;
// Energy gate on that gradient, in the *unnormalised* Sobel units of
// particleSurfaceTangent (grid_tiles.slang): a full-fill step has |grad| = 4, a
// quarter-fill step 1, and a shallow 1/32-per-cell ramp 0.25. The cue's strength
// is smoothstep(1, 3, |grad|), so a genuine surface keeps full bias while the
// ambiguous fields that flip fade toward the resting round contour. These are
// *proposed* values for P2 appearance calibration, not measured thresholds.
static const float kParticleTangentConfidenceLow = 1.0;
static const float kParticleTangentConfidenceHigh = 3.0;
static const float kParticleRoundedSquareSupport = 1.3106;
static const float kParticleSolidHalfExtentScale = kParticleSupportScale/kParticleRoundedSquareSupport;
static const float kParticleSolidCornerFraction = 0.25;
static const float kParticleSolidCornerScale = kParticleSolidCornerFraction*kParticleSolidHalfExtentScale;
static const float kParticleSolidInnerHalfExtentScale = kParticleSolidHalfExtentScale-kParticleSolidCornerScale;
// A negligible-fringe floor for depth writes, not a shape threshold. The C1
// falloff is now the composited edge coverage, so a fragment is cut only when
// its premultiplied coverage is at or below this: the vanishing tail writes no
// depth, while the silhouette keeps its analytic coverage. The C1 tail ends
// 0.077 px inside the support at span64 (0.022 px at span220), and at 1/255 the
// composite cannot move an 8-bit channel by a full LSB.
static const float kParticleCoverageFloor = 1.0/255.0;

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
