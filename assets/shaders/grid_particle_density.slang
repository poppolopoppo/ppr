// ------------------------------------------------------------------
// screen-space particle density: kernel and composite mathematics
// ------------------------------------------------------------------
// Shared by grid_tiles.slang's direct particle entries and by the density
// accumulation/sample-rate composite entries that will read the same math.
// Constants and pure functions only: no resource binding, no entry point and no
// frame state, so including this file adds no target, pipeline, pass or indirect
// argument.
//
// Lengths are world native cells unless a name says otherwise. A particle's
// reference area is V = pi r^2 and its kernel support is the existing shared
// radius R = kParticleSupportScale * r = 2 r. The kernel is a compact,
// anisotropic, radially cubic-sigmoid disc:
//
//   q = inverse(A) (x - p) / R,  rho = |q|
//   t = clamp((1 - rho) / (1 - 2/3), 0, 1)
//   s(t) = t^2 (3 - 2t)
//   w(x) = V / (0.7 * pi * R^2 * det(A)) * s(t) = s(t) / (2.8 * det(A))
//
// with A = rotation(axis) * diag(k, eta). The plateau is rho <= 2/3, the ramp
// spans R/3 up to the support at rho = 1 and both joins are C1. The
// normalisation is exact: integrating s over the unit disc gives 0.7 * pi, so
// the integral of w over the plane is V before terrain clipping and raster
// quadrature. Stretch changes the support but never the integrated weight.
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
// s(t) integrates to 0.7 * pi over the unit disc in q space, and the reference
// area over that same disc gives V / (0.7 * pi * R^2 * det(A)) = 1/(2.8 *
// det(A)) because V = pi r^2 and R = 2 r. The 2.8 is therefore the kernel's own
// normalisation, not a tuning factor.
static const float kParticleDensityProfileIntegral = 0.7;
static const float kParticleDensityNormalization =
    kParticleDensityProfileIntegral * kParticleSupportScale * kParticleSupportScale;
// The ramp occupies the band between the plateau and the support, i.e. R/3.
static const float kParticleDensityBandFraction = 1.0 - kParticleFalloffInnerFraction;
// theta = 5/28 ~= 0.178571429. For an isolated neutral particle w = s/2.8, so
// the crossing is s = 0.5, rho = 5/6: the old isolated half-coverage radius is
// preserved before pixel integration. This is a calibrated presentation-domain
// threshold, not exact solver geometry. theta = 0.5 would erase an isolated
// neutral particle whose peak density is 1/2.8, so no single threshold matches
// both the half-plane and the isolated-droplet conventions.
static const float kParticleDensityThreshold = 5.0 / 28.0;
// Numerical divide and zero-gradient guard in density units, not a blur radius.
// It guards both the empty-field mean (D <= this) and the contour width
// (w_AA = max(|dD/dpx_x| + |dD/dpx_y|, this)).
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
// det(K) = k * eta normalises the weight, so the stored record's sqrt(det)
// carries it exactly. eta <= 1 and k <= 1.5 keep the support inside
// max(k, eta) * R = 3.000 r, so the existing 3.0625 r cull still bounds every
// decoded conic. The metric is world XY and camera-independent: the fragment
// evaluates the quadratic at the interpolated world-plane offset, so a camera
// change rescales the raster footprint without re-deriving the shape.

// ------------------------------------------------------------------
// normalized kernel weight
// ------------------------------------------------------------------
// Weight of one particle at a world-plane offset `world_offset` in metres from
// its centre. The caller supplies the conic scaled by 1/R^2 and sqrt(det) of the
// unscaled record, so the metric is evaluated in the dimensionless
// offset = (x - p)/R of the kernel. Exactly 0 at and beyond the support
// (rho = 1); the guard closes the binary32 round-off of a sample constructed on
// that boundary.
float particleDensityWeight(float2 world_offset,float3 conic,float sqrt_determinant) {
    float rho=sqrt(conic.x*world_offset.x*world_offset.x+2.0*conic.y*world_offset.x*world_offset.y+
        conic.z*world_offset.y*world_offset.y);
    if (!(rho<1.0-kParticleDensitySupportClosure)) return 0.0;

    float t=clamp((1.0-rho)/kParticleDensityBandFraction,0.0,1.0);
    return t*t*(3.0-2.0*t)*sqrt_determinant/kParticleDensityNormalization;
}

// The four fixed 2x2 density-pixel quadrature positions, (+/-1/4, +/-1/4).
float2 particleDensityQuadratureOffset(uint index) {
    return float2((index&1u)==0u ? -kParticleDensityQuadratureOffset : kParticleDensityQuadratureOffset,
        (index&2u)==0u ? -kParticleDensityQuadratureOffset : kParticleDensityQuadratureOffset);
}

// ------------------------------------------------------------------
// derivative-width contour and normalized composite
// ------------------------------------------------------------------
// C = smoothstep(theta - w_AA/2, theta + w_AA/2, D), the one-pixel
// derivative-width contour of the accumulated density. `derivative_x` and
// `derivative_y` are dD/dpx of the bilinearly reconstructed field in
// density-pixel units. The width floor is the zero-gradient guard; it is not a
// blur radius. C is exactly 0 below the lower edge and exactly 1 above the upper
// edge of that band.
float particleDensityContour(float density,float derivative_x,float derivative_y) {
    float width=max(abs(derivative_x)+abs(derivative_y),kParticleDensityEpsilon);
    return smoothstep(kParticleDensityThreshold-0.5*width,kParticleDensityThreshold+0.5*width,density);
}

// Composite the accumulated optical field at one scene sample:
//
//   D = sum_p w_p,  N = sum_p w_p * P_p,  O = sum_p w_p * alpha_p
//   P_bar = N / D,  alpha_bar = optical ? O / D : 1
//   out.rgb = C * P_bar,  out.a = C * alpha_bar
//
// P_p is the existing material surface's linear HDR premultiplied RGB before
// geometric edge coverage and alpha_p its authored optical alpha; an opaque lane
// passes optical == false and needs no O target. Returns false for the guarded
// empty field (D <= 1e-6, no mean exists) and for the negligible-coverage floor
// (C <= 1/255, which also writes no depth), so the caller discards instead of
// dividing by zero or compositing an invisible sample.
bool particleDensityComposite(float3 numerator,float density,float optical_numerator,bool optical,
    float coverage,out float4 color) {
    color=float4(0.0);
    if (!(density>kParticleDensityEpsilon) || !(coverage>kParticleCoverageFloor)) return false;

    float3 radiance=numerator/density;
    float alpha=optical ? optical_numerator/density : 1.0;
    color=float4(coverage*radiance,coverage*alpha);
    return true;
}
#endif
