# lib/engine/math/

## Responsibility

`engine.math` re-exports `mango::math` into `namespace pP` as a single C++20 module (`export module engine.math;`
— no partitions). Provides vector/matrix aliases, scalar math and quaternion helpers, geometry primitives,
easing/intersect utilities, polymorphic math constants, and `hashValue`/`opaqueValue` integration for vectors.

## Design

- Type aliases map mango SIMD types directly: `float2/3/4` = `float32x2/3/4`, `double2/3/4` = `float64x2/3/4`,
  `int2/3/4` = `int32x2/3/4`, `uint2/3/4` = `uint32x2/3/4`; `float3x3` = `Matrix3x3`, `float4x4` = `Matrix4x4`.
- Scalar/vector functions are `using mango::math::X` re-exports (`abs`/`clamp`/`cross`/`distance`/`dot`/`lerp`/
  `normalize`/`inverse`/`transpose`/`reflect`/`refract`/`slerp`/`squad`/quaternions/`Box`/`Frustum`/`Ray`/etc.),
  plus `using` operator re-exports (`operator*`, `operator==`, ...).
- `pP::math` sub-namespace holds axis constants (`axis_x/y/z`, `right/left`, `up/down`, `forward/backward`),
  `Intersect*` overloads, `easeIn/Out/InOut*` curves, generic `Vector<T,DimV>`/`Matrix<T,W,H>` aliases, SIMD
  masks (`mask8x16`…`mask64x8`, `maskToInt`), elementwise `add/sub/mul/div`, and `all_of/any_of/none_of`.
- Polymorphic constants via local `PPR_POLYMORPHIC_*` macros over `math::details::Number`/`PolymorphicConstant`:
  `std::numbers` mirrors (`e_v`, `pi_v`, `sqrt2_v`, `phi_v`, …), `pi_over_2/3/4_v`, type-relative `epsilon_v`
  (10× eps for floats, `0` for ints), `infinity_v`, and `identity_v` (`IdentityValue` → `T::identity()`).
- Engine helpers (all `[[nodiscard]]`, `noexcept`): `makeJitterMatrix` (TAA jitter into row 3), 
  `makeZeroToOneFrustum` (view-projection × depth remap), `quaternionTransform`, `angularVelocity`
  (shortest-arc, `epsilon_v<>` fallback to `axis_z`), `dot2` overloads (dot-productable vs arithmetic),
  `checked_cast`/`vector_cast`/`ceilToInt`/`floorToInt`/`roundToInt`/`truncToInt`/`saturate`,
  `isNan` (scalar/vector/matrix/quaternion), `isNormalized`/`safeNormalize` (default `epsilon_v`).
- `hashValue`/`opaqueValue` for `Vector<T,DimV>` are injected into `namespace mango::math` (span over
  `value.data()`), so mango vectors hash/serialize without engine-type coupling.

## Flow

Header-shaped single `Math.cppm` — everything inline/`using`-re-exported, compiling straight to mango calls.
Consumers write `import engine.math;` and use `pP::float4x4`, `pP::math::easeInCubic`, `pP::pi_v<>`,
`pP::epsilon_v<T>`. Projection matrices live in `engine.rhi` (`getOrthoMatrix`/`getPerspectiveMatrix` over
mango `orthoD3D`/`perspectiveD3D`), not here.

## Integration

- Depends on: `engine.core` (public — `u32`, `safe_narrowing`, `hash_t`, `opaque::Value`, `static_iota`),
  `mango` headers (private system dep).
- Consumed by: `engine.rhi` (matrix types for projection helpers), `engine.app` (camera/transform math),
  `game`, tests.
- Build: single `Math.cppm` in `FILE_SET CXX_MODULES`; `setup_ppr_project(engine.math
  INTERNAL_PUBLIC_DEPS engine.core EXTERNAL_SYSTEM_PRIVATE_DEPS mango)`.

## Key Files

- `Math.cppm` — whole module: aliases, `math::` utilities, constants, helpers, hash/opaque integration.
- `CMakeLists.txt` — `engine.math` target registration (see Integration).
