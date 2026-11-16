// ------------------------------------------------------------------
// screen-space particle surface: kernel, selection and contour mathematics
// ------------------------------------------------------------------
// Shared by grid_tiles.slang's direct particle entries, the per-phase label
// selection, the attribute accumulation and the sample-rate composite. Constants
// and pure functions only: no resource binding, no entry point and no frame
// state, so including this file adds no target, pipeline, pass or indirect
// argument.
//
// Lengths are world native cells unless a name says otherwise. A particle's
// reference area is V = pi r^2 and its kernel support is the shared surface
// radius R = kParticleSurfaceRadiusScale * r (revision 5: R = 1.525948559 r =
// 2 r / (0.25 + 0.75 sqrt 2), the old solid's axis half-extent). The kernel is a
// compact, anisotropic, radially cubic-sigmoid disc:
//
//   q = inverse(A) (x - p) / R,  rho = |q|
//   t = clamp((1 - rho) / (1 - 2/3), 0, 1)
//   s(t) = t^2 (3 - 2t)
//   w(x) = V * A_2 * sqrt(det(M)) / R^2 * s(t),  A_2 = 10 / (7 pi)
//
// with A = rotation(axis) * diag(k, eta) and M = A^-T A^-1. The plateau is
// rho <= 2/3, the ramp spans R/3 up to the support at rho = 1 and both joins are
// C1. The normalisation is exact: integrating s over the unit disc gives
// 0.7 * pi, so the integral of w over the plane is V before terrain clipping and
// raster quadrature. Stretch changes the support but never the integrated weight.
//
// The amplitude is the whole normalisation: with V = pi r^2 and R = c r,
// V * A_2 / R^2 = pi * A_2 / c^2 = 10 / (7 c^2) = 0.613510744960683 at V = .25.
// That is also the neutral isolated peak, so the retained theta = 5/28 crosses
// the profile at rho = 0.881068630, i.e. an apparent radius of 0.379266689 cell
// (1.344465407 r). The old 5R/6 point is only the half-PEAK radius (0.306755372)
// under this calibration and is not the drawn contour.
//
// Physical meaning: D = sum_p w_p(x) is a reconstructed reference-area
// concentration per unit world area, sampled in screen space. Under a uniform
// rest distribution its spatial mean is one. It is not particles per pixel, not
// the liquid solver variable J, not the mass density of mixed substances and not
// an actual optical thickness along a three-dimensional ray.
#ifndef PPR_GRID_PARTICLE_DENSITY_SLANG
#define PPR_GRID_PARTICLE_DENSITY_SLANG

// ------------------------------------------------------------------
// kernel constants
// ------------------------------------------------------------------
// Revision 5 calibration, shared by both phases: one radius, one threshold, one
// profile. kParticleSupportScale stays the DIRECT solid silhouette's R = 2 r
// until the solid migration; this radial scale is the surface reconstruction's
// R and the two must not be conflated.
static const float kParticleSurfaceRadiusScale = 1.5259485586443693;
// A_2 = 10/(7 pi) = 1/(0.7 pi) is the second moment of the unit-disk profile: the
// profile integrates to 0.7 pi, so A_2 * 0.7 pi = 1 is exactly the identity that
// makes the plane integral of w equal V. A_2 is computed from the 10/(7 pi) form.
static const float kParticleDensitySecondMoment = 10.0 / (7.0 * 3.141592653589793);
// The amplitude of w = V * A_2 * sqrt(det M)/R^2 * s. V = pi r^2 and
// R^2 = (c r)^2 cancel r, so it is pi * A_2 / c^2, which equals (10/7)/c^2 =
// 0.613510745 because pi A_2 = 10/7. The profile's 0.7 is ALREADY inside A_2
// (A_2 = 1/(0.7 pi)); it is not a factor of the amplitude, and using it as one
// underweights every splat by pi/0.7 = 4.488. The amplitude is also the neutral
// isolated peak and the only place r enters the weight. The CPU mirror in
// App.WorldLighting.Material.Tests.cpp evaluates this identical expression.
static const float kParticleDensityAmplitude =
    kParticleDensitySecondMoment * 3.141592653589793 /
    (kParticleSurfaceRadiusScale * kParticleSurfaceRadiusScale);
// The ramp occupies the band between the plateau and the support, i.e. R/3.
static const float kParticleDensityBandFraction = 1.0 - kParticleFalloffInnerFraction;
// theta = 5/28 ~= 0.178571429, the retained presentation threshold. With the
// smaller shared R the neutral isolated peak rises to 0.613510745, so theta is
// well below it and the half-coverage contour is rho = 0.881068630 rather than
// the old 5/6. This is a calibrated presentation-domain threshold, not exact
// solver geometry.
static const float kParticleDensityThreshold = 5.0 / 28.0;
// Numerical divide guard in density units, not a blur radius and not a coverage
// ramp width. It guards the empty-field mean (D <= this) and the attribute
// denominator of the reconstruction.
static const float kParticleDensityEpsilon = 1.0e-6;
// Round-off closure of the support join, relative to R. A sample constructed
// exactly at the support can arrive a few binary32 ulps inside it after the
// anisotropic transform, and the ramp would then return a denormal weight
// instead of exactly 0. This is a numerical closure, not a shape or radius
// change: 1e-6 R is about 0.0002 px at span64 and far below the 1/255 floor.
static const float kParticleDensitySupportClosure = 1.0e-6;
// Fixed 2x2 quadrature offset of one density pixel, in density-pixel units. A
// splat entry evaluates w_p at the four offsets and averages them: four kernel
// evaluations in one 1x fragment, not four material shader invocations.
static const float kParticleDensityQuadratureOffset = 0.25;
// Largest FP32 below one: a positive-support selection depth that rounds to the
// 1.0 clear value would be rejected by the strict Less test, so the open support
// is clamped to this instead. It is not a tie epsilon.
static const float kParticleDensityDepthCeiling = 0.9999999403953552;

// ------------------------------------------------------------------
// conic world-plane metric
// ------------------------------------------------------------------
// The shape of a splat is the per-row six-byte conic record, decoded by the
// vertex stage (grid_particle_native.slang particleConicMetric). The retained
// kernel, not an individual silhouette, carries the orientation cue:
//
//   b = kParticleTangentContourBias * bias8/255
//   k = 1 + (kParticleStretchCap - 1) * stretch8/255
//   eta = (1 - b/2) / (1 + b/2)
//   a,b,c = coefficients of the inverse metric of rotation(axis)*diag(k, eta)
//
// det(M) = 1/(k eta)^2 normalises the weight, so the stored record's sqrt(det)
// carries it exactly. eta <= 1 and k <= 1.5 keep the support inside
// max(k, eta) * R = 1.5 * 1.5259 r = 2.288923 r, inside the retained 3.0625 r
// cull. The metric is world XY and camera-independent: the fragment evaluates
// the quadratic at the interpolated world-plane offset, so a camera change
// rescales the raster footprint without re-deriving the shape.

// ------------------------------------------------------------------
// normalized kernel weight
// ------------------------------------------------------------------
// Squared normalized radius of one particle at a world-plane offset. The caller
// supplies the conic scaled by 1/R^2, so the metric is evaluated in the
// dimensionless offset = (x - p)/R of the kernel. Selection depth uses this
// quantity directly: its minimum over the eligible quadrature points is the
// stored synthetic distance.
float particleDensityRhoSquared(float2 world_offset,float3 conic) {
    // Clamped at zero so a negative round-off near the metric's isotropic case
    // cannot turn into a NaN through sqrt.
    return max(0.0,conic.x*world_offset.x*world_offset.x+2.0*conic.y*world_offset.x*world_offset.y+
        conic.z*world_offset.y*world_offset.y);
}

// Weight of one particle at a world-plane offset `world_offset` in metres from
// its centre. The caller supplies the conic scaled by 1/R^2 and sqrt(det) of the
// unscaled record. Exactly 0 at and beyond the support (rho = 1); the guard
// closes the binary32 round-off of a sample constructed on that boundary.
float particleDensityWeight(float2 world_offset,float3 conic,float sqrt_determinant) {
    float rho2=particleDensityRhoSquared(world_offset,conic);
    float rho=sqrt(rho2);
    if (!(rho<1.0-kParticleDensitySupportClosure)) return 0.0;

    float t=clamp((1.0-rho)/kParticleDensityBandFraction,0.0,1.0);
    return t*t*(3.0-2.0*t)*sqrt_determinant*kParticleDensityAmplitude;
}

// The four fixed 2x2 density-pixel quadrature positions, (+/-1/4, +/-1/4).
float2 particleDensityQuadratureOffset(uint index) {
    return float2((index&1u)==0u ? -kParticleDensityQuadratureOffset : kParticleDensityQuadratureOffset,
        (index&2u)==0u ? -kParticleDensityQuadratureOffset : kParticleDensityQuadratureOffset);
}

// ------------------------------------------------------------------
// binary body occupancy
// ------------------------------------------------------------------
// B = [D > theta]: the body OCCUPIES a scene sample when the reconstructed
// density at that sample exceeds the retained threshold. There is no per-sample
// ramp and no per-sample fractional coverage, so nothing here approximates an
// output-pixel footprint. The silhouette's antialiasing is the existing MSAA
// contract: the composite evaluates this decision at each of the four spatially
// located scene samples, and the reconstruction pass reads those samples
// individually (hable_temporal.slang g_scene.Load(pixel, sample)) to rebuild the
// content-pixel lattice and filter the output footprint once.
//
// Occupancy and material opacity are separate factors: a transparent interior
// stays transparent because the authored opacity is applied once after this
// decision, never re-applied as coverage. An exact equality does not occupy; that
// is measure-zero on a continuous field and needs no tie rule.
float particleDensityOccupancy(float density) {
    return density>kParticleDensityThreshold ? 1.0 : 0.0;
}

// ------------------------------------------------------------------
// minimum-presence marker
// ------------------------------------------------------------------
// Density is not presence. A particle whose reconstructed support is far below one
// raster pixel loses every quadrature point to phase, so no label, no density and
// therefore no occupied sample can exist at any zoom beyond the sampled extinction.
// This marker is the separate screen-space guarantee of design revision 8-B, unioned
// once with the physical field:
//
//   d = |x_sample_px - x_centre_px|
//   F_p = saturate((R - d) / (R - core))
//   F = max_p F_p
//   C = max(B, F)
//
// UNITS. The design contract of revision 8-B is in CONTENT pixels, and this code
// runs in RESOLVED RASTER SAMPLES: the composite is one decision per scene sample
// at raster resolution (makePresentationGrid: resolved = ceil(window/P),
// raster = ceil(resolved/2)), which the reconstruction then enlarges to the
// content grid, so one raster sample is two content pixels in the padded grid.
// Every radius below is therefore the content-pixel radius divided by that
// factor, and the conversion is the only unit step:
//
//   opaque core  1.0 content px / 2 = 0.5 raster samples
//   half coverage 1.5 content px / 2 = 0.75 raster samples  (F = .5)
//   outer        2.0 content px / 2 = 1.0 raster samples    (F = 0)
//
// i.e. a 2-content-pixel opaque diameter, a 3-content-pixel half-coverage
// diameter and a 4-content-pixel outer diameter, with a one-content-pixel radial
// ramp. The dot is a presentation guarantee, so it must not shrink with the zoom
// and is never expressed in world cells or the field's texel pitch. F is never
// filtered, never enters Q.a and never changes theta, R, A_2 or the label rule.
// The CPU mirror in App.WorldLighting.Material.Tests.cpp pins these values.
static const float kParticleMarkerContentPixelsPerRasterSample = 2.0;
static const float kParticleMarkerCoreRadius = 0.5;
static const float kParticleMarkerRadius = 1.0;
// Half a raster sample's diagonal. A candidate quad must still write the record of
// a pixel whose CENTRE lies outside the coverage radius, because a scene sample of
// that pixel can be inside it: the pixel's farthest interior point is sqrt(2)/2
// away, so this is the exact bound that keeps every sample with F > 0 represented.
// The reduced radius keeps the bound at 1.7071 raster samples, still inside the
// record's representable +/-4-sample offset range.
static const float kParticleMarkerQuadRadius = kParticleMarkerRadius+0.7071067811865476;
// The winner record's distance channels are unorm, encoded as offset/8 + 0.5: the
// representable centre offset is +/-4 raster samples and the quantisation is
// 8/65535 = 1.22e-4 raster sample, eight times finer than the 1/1024 coverage
// error the selection contract allows. A written record's offset is at most
// kParticleMarkerQuadRadius by the fragment's own bound, so the range is never
// approached from inside.
static const float kParticleMarkerRecordScale = 8.0;
// The record's third channel carries the winner's material id as m/65535, which
// every 16-bit integer round-trips exactly through unorm. Zero is the empty record.
static const float kParticleMarkerMaterialScale = 65535.0;
// The record's fourth channel carries the sample's linear ENERGY, because presence
// is not radiance. A marker sample is shaded at its material's full radiance, which
// is truthful only while the particle's projected reference area V = pi r^2 covers
// a content pixel; below that the dot is a sub-pixel speck and the full-radiance
// shading is a hot pixel. The factor is
//
//   E = saturate(V_content_px2 / kParticleMarkerEnergyAreaContentPx2)
//
// and the composite multiplies its premultiplied colour and alpha by it. At this
// reference exactly one content pixel of projected area is fully bright, so the
// fade starts at r = 1/sqrt(pi) = 0.5641896 content pixels and is continuous
// everywhere: a distant particle becomes a dim mark instead of a hot dot, and
// nothing pops as it crosses the threshold. This is the ONE fade-start knob: it
// changes brightness, never the radii, the spatial extent (2..4 content pixels) or
// the union C = max(B, F).
static const float kParticleMarkerEnergyAreaContentPx2 = 1.0;

// One linear ramp between the two pinned endpoints: F is 1 up to the opaque core,
// falls one coverage unit per CONTENT pixel - two per raster sample - and reaches
// 0 at the coverage radius, so it is exactly .5 at 1.5 content pixels (the
// design's half-coverage radius).
float particleMarkerCoverage(float distance_px) {
    float span=max(kParticleMarkerRadius-kParticleMarkerCoreRadius,kParticleDensityEpsilon);
    return saturate((kParticleMarkerRadius-distance_px)/span);
}

// The projected reference area in CONTENT pixels squared: pi r^2 projected through
// the local world-area-per-raster-sample Jacobian and converted by the marker's own
// content-pixel factor. One raster sample is two content pixels, so an area
// converts by that factor squared. `raster_area_m2` is |det J| of the world-plane
// map at the sample, so the projection is exact there and needs no per-draw pixel
// scale; the divide guard only keeps a degenerate Jacobian finite, and it saturates
// that case at the fully bright end.
float particleMarkerReferenceAreaContentPx2(float radius_m,float raster_area_m2) {
    float content_area_scale=
        kParticleMarkerContentPixelsPerRasterSample*kParticleMarkerContentPixelsPerRasterSample;
    return 3.141592653589793*radius_m*radius_m*content_area_scale/max(raster_area_m2,kParticleDensityEpsilon);
}

// The marker's energy factor at one projected reference area in content pixels
// squared: zero area is zero energy, and the reference area and everything above it
// is exactly 1.
float particleMarkerEnergy(float reference_area_content_px2) {
    return saturate(reference_area_content_px2/kParticleMarkerEnergyAreaContentPx2);
}

// Recording a centre offset relative to the texel's own centre, not in absolute
// raster coordinates: the offset stays inside the quad's own few pixels, so one
// fixed-point quantum serves every raster size without a large-coordinate loss.
float2 particleMarkerEncodeCentre(float2 centre_offset_px) {
    return centre_offset_px/kParticleMarkerRecordScale+0.5;
}

float2 particleMarkerDecodeCentre(float2 record) {
    return (record-0.5)*kParticleMarkerRecordScale;
}

float particleMarkerEncodeMaterial(uint material) {
    return float(material)/kParticleMarkerMaterialScale;
}

uint particleMarkerDecodeMaterial(float record) {
    return uint(record*kParticleMarkerMaterialScale+0.5);
}
#endif
