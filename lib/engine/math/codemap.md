# lib/engine/math/

## Responsibility

`engine.math` re-exports `mango::math` into `namespace pP` as a single C++20 module (`export module engine.math;`
— no partitions, header-shaped `Math.cppm`). Provides SIMD vector/matrix aliases, scalar/vector math and
quaternion re-exports, geometry primitives, `Intersect*`/`easeIn/Out/InOut*` utilities, polymorphic math constants,
integer/float conversion helpers, predicates, and `hashValue`/`opaqueValue` integration for mango vectors.

## Design

- Global fragment includes `pP/Macros.h` + `<mango/math/math.hpp>`; `import engine.core; import std;`.
  `export namespace pP { using namespace mango::math; }` plus explicit aliases: `int2/3/4` = `int32x2/3/4`,
  `uint2/3/4` = `uint32x2/3/4`, `float2/3/4` = `float32x2/3/4`, `double2/3/4` = `float64x2/3/4`,
  `float3x3` = `Matrix3x3`, `float4x4` = `Matrix4x4`.
- Scalar/vector `using mango::math::X` re-exports: `abs`/`ceil`/`clamp`/`cross`/`distance`/`dot`/`floor`/`fract`/
  `hmax`/`hmin`/`inverse`/`length`/`lerp`/`max`/`min`/`mod`/`normalize`/`rcp`/`reflect`/`refract`/`round`/`rsqrt`/
  `sign`/`smoothstep`/`sqrt`/`square`/`transpose`/`trunc`; rotation `AngleAxis`/`EulerAngles`/`Quaternion` +
  `conjugate`/`slerp`/`squad`; geometry `Box`/`Cone`/`FastRay`/`Frustum`/`Plane`/`Quadratic`/`Ray`/`RayFrustum`/
  `Rectangle`/`Sphere`/`Triangle`; all arithmetic/comparison/bitwise `operator*`/`==`/etc. re-exports.
- `pP::math` sub-namespace: left-handed axis constants (`axis_x/y/z`, `right/left`, `up/down`,
  `forward(+Z)/backward` as `inline const float3`); `Intersect`/`IntersectBarycentric`/
  `IntersectBarycentricTwosided`/`IntersectRange`/`IntersectSolid`; 27 easing curves (`easeIn/Out/InOut` ×
  `Back/Bounce/Circular/Cubic/Elastic/Exponential/Quadratic/Quartic/Sine`); generic `Vector<T,DimV>`/
  `Matrix<T,W,H>` constrained by `details::TArithmetic` (`std::is_arithmetic_v`); SIMD masks grouped by width
  (128-bit `mask8x16/16x8/32x4/64x2`, 256-bit `mask8x32/16x16/32x8/64x4`, 512-bit `mask8x64/16x32/32x16/64x8`) +
  `maskToInt`; elementwise `add/sub/mul/div`; `all_of/any_of/none_of`.
- `math::details`: `roundHalfAwayFromZero` (scalar `std::round`, vector via `static_iota<u32,DimV>` fold);
  `vectorCast<ToT,FromT,DimV>` (generic `static_cast` fold + 4 specializations: `float4→int4` via
  `mango::truncate`, `int4/uint4→float4` and `float4→uint4` via `mango::convert`); `scalarCast` (`static_cast`).
  `TValueGenerator` concept (callable NTTP with `template operator()<T>()`), `PolymorphicConstant` (`operator T`,
  `operator==`, `operator<=>`), `InvalidConstantType` (`N4950 [math.constants]/3` static assert),
  `Number<T,ValueGenerator>` primary + constrained + `void` specializations, `IdentityValue`
  (`IdentityT::identity()` for types exposing it).
- Polymorphic constants via `PPR_POLYMORPHIC_*` macros: `std::numbers` mirrors (`e_v`, `log2e_v`, `log10e_v`,
  `pi_v`, `inv_pi_v`, `inv_sqrtpi_v`, `ln2_v`, `ln10_v`, `sqrt2_v`, `sqrt3_v`, `inv_sqrt3_v`, `egamma_v`, `phi_v`),
  `pi_over_2/3/4_v` (`pi_v<T>/N`), type-relative `epsilon_v` (floats `10×numeric_limits::epsilon` — covers
  `float32` `normalize()` output, `3.58e-7` over 20k-direction sweep — ints `T{0}`), `infinity_v`, `identity_v`.
- Engine helpers (all `[[nodiscard]]` `noexcept`; `makeJitterMatrix` plus the cast/convert/predicate
  helpers are `constexpr`, while `makeZeroToOneFrustum`/`quaternionTransform`/`angularVelocity` are runtime
  `noexcept`): `makeJitterMatrix(jitter)` (TAA offset into `result[3][0..1]` over `identity()`); `makeZeroToOneFrustum(viewProjection)` (`viewProjection * depth_transform`
  with `z: 2z−1` remap → `Frustum`); `quaternionTransform(q,v)` (`operator*(v,q)`); `angularVelocity(seconds,from,to)`
  (`PPR_ASSUME(seconds>0)`, shortest-arc `dot<0 ? −to : to`, `normalize(actual_to*conjugate(from))`,
  `atan2(|v|,w)`, `epsilon_v<>` fallback to `axis_z`, returns `axis*(2·half_angle/seconds)` with MSVC-ADL
  `NOLINTNEXTLINE(clang-diagnostic-error)` note); `dot2` overloads (dot-productable `dot(x,x)` vs arithmetic `x*x`);
  `checked_cast<Vector>` (integrals via `static_iota` + core scalar `checked_cast`); `ceil/floor/round/truncToInt`
  and `ToUInt` (scalar + vector; `round*` routes through `roundHalfAwayFromZero`); `toFloat` (integral→`float`
  scalar/vector); `saturate` (`clamp(v,0,1)`); `isNan` (scalar `std::isnan`, `Quaternion` any-component,
  `Vector` `static_iota` fold, `Matrix` per-`column<idx>()` fold); `isNormalized` (`|1−dot2|<epsilon`, vector +
  quaternion overloads defaulting to `epsilon_v`); `safeNormalize(v,fallback,epsilon)`
  (`dot>epsilon ? v/sqrt(norm_sq) : fallback`).
- `export namespace mango::math`: `hashValue(Vector)` → `hash::contiguousRange(span<T,DimV>(data,DimV))`;
  `opaqueValue(Vector)` → `pP::opaqueValue(span)` (span captured by value so the `TransformView` never dangles).

## Flow

Single-`Math.cppm` compile: `pP::math::details` casts (`roundHalfAwayFromZero`, `vectorCast` + 4 SIMD
`float4↔int4/uint4` specializations, `scalarCast`) → `using namespace mango::math` + explicit vector/matrix
aliases → scalar/vector/quaternion/geometry `using` re-exports → `math::` axis constants, `Intersect*`,
easing curves, `TArithmetic`/`TValueGenerator`/`PolymorphicConstant`/`Number`/`IdentityValue` machinery,
`Vector`/`Matrix`/mask aliases → `PPR_POLYMORPHIC_*` constants (`std::numbers` mirrors, `pi_over_N`,
`epsilon_v`, `infinity_v`, `identity_v`) → runtime helpers (`makeJitterMatrix`, `makeZeroToOneFrustum`,
`quaternionTransform`, `angularVelocity`) → operator re-exports → `dot2` overloads, `checked_cast<Vector>`,
`ceil/floor/round/trunc ToInt/ToUInt`, `toFloat`, `saturate`, `isNan`, `isNormalized`, `safeNormalize` →
`mango::math` `hashValue`/`opaqueValue` injection. Consumers `import engine.math;` and call
`pP::float4x4`, `pP::math::easeInCubic`, `pP::pi_v<T>`, `pP::epsilon_v<T>`, `pP::roundToInt(v)`,
`pP::safeNormalize(v, fallback)`. Projection matrices are NOT here — they live in `engine.rhi`
(`getOrthoMatrix`/`getPerspectiveMatrix` over mango `orthoD3D`/`perspectiveD3D`).

## Integration

- Depends on: `engine.core` (public — `u32/i32`, `checked_cast`, `safe_narrowing`, `hash_t`,
  `hash::contiguousRange`, `opaque::Value`, `static_iota`, `PPR_ASSUME`), `mango` headers (private system dep).
- Consumed by: `engine.rhi` (matrix types for projection helpers), `engine.app` (camera/transform math),
  `game`, tests.
- Build: single `Math.cppm` in `FILE_SET CXX_MODULES`; `setup_ppr_project(engine.math
  INTERNAL_PUBLIC_DEPS engine.core EXTERNAL_SYSTEM_PRIVATE_DEPS mango)`.

## Key Files

- `Math.cppm` — whole module: aliases, `math::` utilities, constants, helpers, hash/opaque integration.
- `CMakeLists.txt` — `engine.math` target registration (see Integration).
