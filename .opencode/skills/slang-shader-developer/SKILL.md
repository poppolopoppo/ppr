---
name: slang-shader-developer
description: >
  Authoritative PPR Slang shader authoring and integration guide. Use whenever
  editing .slang files, embedded Slang source, shader entry points, CPU/GPU
  parameter layouts, ShaderCursor bindings, Slang reflection, shader programs,
  shader-derived render/compute pipelines, or Slang-RHI resource bindings.
---

# Slang Shader Developer

## Authority and scope

This skill is the PPR-specific authority for writing, reviewing, integrating,
and validating Slang shaders. It supplements the Slang documentation with the
engine contracts that generic examples do not know: matrix convention, CPU/GPU
ABI mirrors, pass ownership, runtime compilation, reflection names, and the
current Slang-RHI binding seams.

Use official Slang documentation for language features not covered here. Do
not guess from HLSL, GLSL, or an unrelated engine. Slang is broadly
HLSL-compatible, but target lowering, layout, reflection, and resource binding
can differ in important ways.

This is not a static inventory of every shader. Before an edit, inspect the
current shader and its C++ consumer. Shader assets and Slang-RHI behavior change
faster than this contract.

## Task selection

Read the matching reference before implementing or reviewing the selected
task. The authoring, ABI, cursor/reflection, and validation rules below apply
to every shader task.

| Task | Required reference |
|---|---|
| Mesh-specific work: `mesh_bindless.slang`, `TrianglePass`, mesh mirrors, payload grouping, heaps, bindings, or failures | [Current bindless mesh seam](references/bindless-mesh.md); load only for mesh-specific work |
| Language/compiler features, matrix layout, resource or cursor semantics, target portability | [Official sources](references/official-sources.md); use official documentation for features not covered here |
| Creating/loading shader programs or pipelines, new/moved shader assets, runtime staging, failure triage | [Programs and staging](references/programs-and-staging.md) |
| Rendering ownership and camera boundaries | [AGENTS.md rendering/camera contract](../../../AGENTS.md#rendering-and-camera-contract) |
| Shader authoring, CPU/GPU ABI, binding/reflection changes, validation | Entry sections below |

## Mandatory first pass

Before changing shader code or its C++ integration:

1. Read `assets/shaders/codemap.md` and the codemap for the owning pass, then
   enumerate the current `.slang` files rather than relying on an inventory.
2. Inspect the current `.slang` file or embedded source and its complete C++
   binding path. Search every shader parameter and entry-point name on both
   sides.
3. Identify the CPU mirror for every uploaded struct and verify size, alignment,
   member order, offsets, signedness, and buffer element stride.
4. Identify whether each value is global ordinary data, a
   `ConstantBuffer<T>`, an entry-point `uniform` parameter, a resource binding,
   or a descriptor handle. These are not interchangeable binding locations.
5. Check `cmake/external/SlangRHI.cmake`. PPR currently follows Slang-RHI
   `main`, so never assume an upstream workaround or API remains necessary
   without checking the current source and behavior.
6. State which target backends the change must support. PPR selects the Slang
   target from the created RHI device; shader code must not silently become
   backend-specific.

Use CodeGraph first for C++ symbols and call paths. Use direct reads for
`.slang`, CMake, and other files CodeGraph does not cover completely.

## PPR rendering contract

These rules are engine contracts, not suggestions:

- Matrices use row-major storage and row-vector math.
- View space is left-handed: `+Z` forward and `+Y` up.
- Clip-space depth is `[0,1]`.
- Transform vectors with `mul(vector, matrix)`, for example:

```slang
float4 world = mul(float4(position, 1.0), model);
float4 clip = mul(world, viewProjection);
```

- Compose view-projection as `view * projection`.
- Do not transpose matrices, flip Y, or switch multiplication order for an
  individual backend.
- Use `w = 1.0` for points and `w = 0.0` for directions.
- Slang's matrix/vector `operator*` is element-wise. Use `mul()` for linear
  algebra.
- Memory layout and mathematical vector convention are separate concepts.
  PPR intentionally configures both coherently: the shader session sets
  `SLANG_MATRIX_LAYOUT_ROW_MAJOR`, while shaders use row vectors.

The canonical implementation is the Slang session setup in
`lib/engine/shader/Shader.cpp`, the projection helpers in
`lib/engine/rhi/RHI.cpp`, and the transforms in current shader assets. Do not
copy matrix multiplication from a compilation-only test snippet; tests that
exercise reflection may not execute the transform.

## Architecture and ownership

Apply [AGENTS.md's rendering and camera contract](../../../AGENTS.md#rendering-and-camera-contract)
for Renderer/pass/client ownership and snapshot boundaries. Pass-owned shader
resources include textures, samplers, and pipeline variants.

- Shader loading is synchronous through `IShaderService`. Asset shaders are
  compiled at runtime from the staged content directory; embedded shaders use
  `loadModuleFromSource`.
- `SharedModule` is a borrowed view owned by the active Slang session. Do not
  release it or keep it beyond shader-service shutdown.
- Production target selection belongs to `IRhiService::initialize`, which maps
  the created device type and calls `IShaderService::setTargetFormat` before any
  module load. Do not add a redundant manual production call. Direct calls are
  appropriate only in focused shader-service tests. PPR rejects target changes
  after a module has loaded.
- Preserve teardown order: stop submissions, wait as required, release
  pass-owned caches/resources and pipelines/programs, then shut down RHI before
  the shader session.

## Entry points and stage I/O

Use explicit Slang stage attributes:

```slang
[shader("vertex")]
VertexOutput vertexMain(VertexInput input) { ... }

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target { ... }
```

- PPR normally discovers `vertexMain` and `fragmentMain` with
  `IModule::findEntryPointByName()`. Renaming an entry point requires changing
  every C++ lookup and relevant test. Asset shaders may use a domain-specific
  name instead: the bindless mesh asset's only vertex entry point is
  `vertexIndirectMain`, so do not assume a `vertexMain` lookup will resolve for
  it.
- Keep matching cross-stage fields in the same order and with matching types and
  semantics. Some targets match by location/index and others retain semantic
  names; satisfying both is the portable rule.
- Use `SV_Position`, `SV_VertexID`, `SV_Target`, and other `SV_*` semantics only
  for their defined stage roles. User varyings use explicit, matching semantics
  such as `TEXCOORD0`, `NORMAL`, or `COLOR0`.
- Entry-point parameters are varying unless marked `uniform`. The mesh pass uses
  fragment entry-point uniforms for its texture heap and shared sampler, and a
  vertex entry-point uniform for `g_payload_base`; it has no module-scope
  ordinary data. Per-draw ordinary data as a module global breaks D3D12 root
  signature creation; see [Current bindless mesh seam](references/bindless-mesh.md).
- A pass using fixed-function vertex buffers must keep its input layout and
  shader vertex input synchronized. A pass using `SV_VertexID` plus
  `StructuredBuffer` fetch must not also add fixed-function geometry bindings.
- A fragment entry point must not call another entry point. Slang 2026.17.1
  crashes in `TransformParamsToConstRef` when it does, so an entry that shares a
  body calls an unannotated helper instead.
- The stage interface belongs to the **entry pair**, not the struct. When one
  vertex expansion feeds several fragments, give each pair its own output type
  and copy only the fields that fragment reads. Widening it warns
  (`WARNING-Shader-OutputNotConsumed`) on every attribute the fragment's own
  reads leave out, because Slang strips unread inputs from the fragment side;
  narrowing it below what the fragment reads is `VUID-RuntimeSpirv-OpEntryPoint-08743`,
  which is an outright violation that feeds undefined values. Check both
  directions, not the warning count alone.
- `discard` executes in the callee. An entry that returns a value before calling
  the shading function skips that discard and paints pixels that should reject,
  so a diagnostic tint must transform the function's result, never replace it.

## CPU/GPU ABI rules

Never infer ABI compatibility from visually similar declarations.

- For every mirrored type, keep field order and scalar/vector widths identical.
- Use fixed-width engine integers on the CPU and matching signed or unsigned
  32-bit Slang types. Do not use C++ `bool`, C++ bitfields, pointers, ownership
  wrappers, or non-trivial math objects in GPU protocol structs.
- Prefer trivially copyable, standard-layout CPU structs with explicit padding.
- Add focused `static_assert`s for `sizeof`, `alignof`, and important offsets.
- Verify the actual GPU buffer `elementSize` against the mirrored struct before
  drawing when a stride mismatch could reinterpret memory.
- Treat constant-buffer layout and structured-buffer element layout as separate
  ABI problems. Do not reuse an assertion from one as proof for the other.
- Do not assume `float3` has the same padding in every aggregate or target.
  Prove offsets and stride for the exact storage class in use.
- Keep reserved/padding members initialized. Never upload indeterminate bytes.

Current examples worth inspecting, not blindly copying:

- `TrianglePass::FrameConstants` mirrors four `float4x4` values and two
  `float4` values, with a 288-byte assertion.
- `mesh::StaticMeshVertex` mirrors `BagVertex` as 64 bytes, including the exact
  offsets of position, normal, UV, tangent, and color.
- `GpuMaterial` mirrors five 16-byte rows for an 80-byte structured-buffer
  stride and uses an explicit integer mask instead of C++ bitfields.
- `InstancePayload` mirrors a 16-byte-aligned model matrix plus seven 4-byte
  fields for a 96-byte structured-buffer stride. It is a structured-buffer
  element, not an entry-point parameter: the deleted per-draw `PushScalars`
  entry-point parameter and its separately uploaded model matrix are no longer
  part of the seam.

## Parameters and ShaderCursor

PPR binds by reflected shader names. A rename is an API change between the
shader and its pass.

Use the binding operation that matches the reflected parameter kind:

| Shader declaration | PPR binding pattern |
|---|---|
| ordinary scalar/vector/matrix data | `cursor["name"].setData(...)` |
| `ConstantBuffer<T>` | get the dereferenced cursor, then `setData(...)` |
| texture, sampler, or buffer resource | `setBinding(...)` |
| full structured buffer in the mesh pass | `setBinding(Binding(buffer, makeFullRange(buffer)))` |
| bindless `.Handle` in a `StructuredBuffer` heap | `setBinding(...)` on the heap buffer; the shader indexes the `DescriptorHandle<T>` element |

Rules:

- Check every cursor operation's `Result`; propagate failures through the
  established `PPR_RETURN_ERROR_ON_FAIL` path.
- For `ConstantBuffer<T> g_frame`, write to the dereferenced sub-object. Writing
  bytes to the root cursor is not equivalent.
- `makeFullRange` is a PPR-local workaround in the current mesh pass:
  Slang-RHI's header-local `kEntireBuffer` does not survive the C++ module
  boundary there, and a default `Binding()` creates an empty SRV. At those
  sites, use `makeFullRange` and do not substitute `kEntireBuffer` or a bare
  `Binding()` unless the module-boundary behavior has been reverified.
- Keep shader names and C++ cursor strings synchronized, including
  `g_frame`, resource globals, and entry-point `uniform` parameters.
- Reflection-driven binding avoids hard-coding backend descriptor locations,
  but it does not remove the ABI or name contract.
- Explicit `register(...)` annotations are valid when a pass intentionally
  requires them. Do not add register numbers mechanically: Slang's automatic
  layout plus reflection is usually more portable, and manual claims can
  collide with generated layout.

## Compilation and staging invariants

Do not bypass `IShaderService` with an unrelated compiler session. The RHI
device receives PPR's global Slang session, and target selection/lifetime must
remain coherent.

Slang-RHI is actively refactored and does not advertise a stable public API.
When adding a new cursor or program operation, inspect the exact dependency
source/API used by the configured build instead of relying on examples from a
different revision.

Slang compiles at runtime, so a shader that fails to compile still passes the
build and only fails when the application starts. After editing a shader,
compile each touched entry point directly before launching (for example
`slangc <file> -entry <name> -stage <stage> -target spirv`), and treat a launch
failure whose message carries `category=slang` as a compile failure in the file
just edited, not as an unrelated application fault.

Read [program creation, source placement/staging, and failure signatures](references/programs-and-staging.md)
for the selected integration task. Runtime assets belong under `assets/shaders/`;
new or moved shaders require checking content-root paths and CMake staging.

## Change workflow

1. Define the pass-level behavior and required backend set.
2. Map shader inputs, outputs, resources, ordinary data, and CPU mirrors.
3. Make the smallest coherent shader and C++ binding change together.
4. Update ABI assertions and focused tests whenever layout or reflection changes.
5. Preserve diagnostics and check all Slang/Slang-RHI results.
6. Inspect the final shader/C++ name map and resource ownership.
7. Validate proportionately using the repository `validation` skill.

For shader changes, validation should include:

- Compilation through PPR's `IShaderService`, not only an editor plugin or a
  standalone compiler invocation.
- Entry-point discovery and program/pipeline creation.
- Reflection checks for changed global or entry-point parameters.
- CPU mirror size/offset/stride assertions.
- A focused render or readback test when output behavior changes.
- A debug run with Slang-RHI core and GPU-assisted validation when feasible.
- More than one backend when the change touches portability-sensitive layout,
  bindings, bindless access, or target-specific features.

Do not claim backend portability from successful parsing alone. Compilation,
pipeline creation, binding, and execution prove different parts of the path.

## Official references

Prefer the [official sources](references/official-sources.md) over search
snippets or HLSL folklore.

When an official statement conflicts with observed behavior in PPR's current
Slang-RHI revision, record the minimal reproducer and treat the local behavior
as a version-specific integration constraint, not as a general Slang fact.
