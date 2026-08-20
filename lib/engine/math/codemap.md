# lib/engine/math/

## Responsibility
engine.math wraps the mango::math library into the engine's `namespace pP`, providing type aliases for 2/3/4-component vectors and matrices, free functions for common math operations (dot, lerp, normalize, distance, etc.), quaternion helpers, and a `lookAt` function. It also integrates hashing and opaque serialization infrastructure for vector types, exposing a C++20-module interface (`engine.math`) that engine and RHI consumers depend on for math utility without mango dependency exposure.

## Design
Type aliases map mango SIMD types directly: `float2/3/4` = `float32x2/3/4`, `int2/3/4` = `int32x2/3/4`, `uint2/3/4` = `uint32x2/3/4`; `float3x3` = `Matrix3x3`, `float4x4` = `Matrix4x4` (both row-major). Free functions `dot`, `lerp`, `normalize`, `distance`, `inverse`, `transpose`, `cross`, `clamp` delegate to `mango::math` counterparts. `lookAt(eye, target, up)` constructs a row-major view matrix from basis vectors, with +X right and -Z forward in view space. Quaternion helpers use mango's `rotateXYZ` (pitch→X, yaw→Y, roll→Z). Two `dot2` overloads: one concept-constrained for SIMD vectors, one for arithmetic types. `vector_cast` and `checked_cast` provide safe typed conversions. `hashValue` and `opaqueValue` enable serialization integration. All functions are `[[nodiscard]]` and `noexcept`.

## Flow
Math operations are inlined and compile to direct mango::math calls — no runtime overhead. The module is imported by `engine.rhi` (projection matrix helpers), `engine.app` (renderer math), and test targets. Users include `<engine.math>` or import the module partition (`engine.math:core` etc.) and access `pP::float4x4`, `pP::lookAt`, etc. The header-only `Math.cppm` interface exports `namespace pP` with all types and functions; implementations are entirely within the `.cppm` since mango functions are header-only.

## Integration
Consumers: `engine.rhi` uses `projectionConventionFromDeviceType`, `getOrthoMatrix`, `getPerspectiveMatrix`; `engine.app` uses math for camera and transform calculations. Depends on `engine.core` for `safe_ptr`, `hash_t`, `opaque::Value`. Wraps `mango::math/math.hpp` — the mango library is a third-party dependency kept outside the module system. The module partition syntax `engine.math` is used via `import engine.math;` or `import engine.math:core;` for selective inclusion.

## Key Files
- `Math.cppm` — interface export of `namespace pP` with type aliases, math functions, quaternion helpers, `hashValue`/`opaqueValue` integration
- No separate `.cpp` — all definitions reside in the `.cppm` interface since mango::math is header-only