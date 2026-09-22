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

- `Renderer` remains content-free. It owns device-facing frame orchestration,
  surfaces, command submission, and presentation.
- A render pass owns its content-specific shader program, pipeline variants,
  buffers, textures, samplers, and encoding logic.
- Scene, camera, player, and UI state remain client/editor-owned. Passes consume
  immutable snapshots such as `CameraSnapshot`; shaders do not justify moving
  scene ownership into `Renderer`.
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
  every C++ lookup and relevant test.
- Keep matching cross-stage fields in the same order and with matching types and
  semantics. Some targets match by location/index and others retain semantic
  names; satisfying both is the portable rule.
- Use `SV_Position`, `SV_VertexID`, `SV_Target`, and other `SV_*` semantics only
  for their defined stage roles. User varyings use explicit, matching semantics
  such as `TEXCOORD0`, `NORMAL`, or `COLOR0`.
- Entry-point parameters are varying unless marked `uniform`. PPR uses `uniform`
  entry-point parameters for per-draw ordinary data and descriptor handles in
  the mesh pass.
- A pass using fixed-function vertex buffers must keep its input layout and
  shader vertex input synchronized. A pass using `SV_VertexID` plus
  `StructuredBuffer` fetch must not also add fixed-function geometry bindings.

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
- `PushScalars` is a scalar-only, 20-byte per-draw entry-point parameter; the
  model matrix is uploaded separately.

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
| bindless `.Handle` value | `setDescriptorHandle(...)` in the current mesh seam |

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

## Current bindless mesh seam

The bindless mesh path contains deliberate, dependency-version-specific rules:

- Global scope contains one `ConstantBuffer` (`g_frame`), structured buffers,
  and no ordinary scalar data.
- Per-instance model/push data is carried as vertex entry-point `uniform`
  parameters.
- Texture and sampler handles are fragment entry-point `uniform` parameters.
- The current vendored Slang-RHI path uses one scalar
  `Texture2D<float4>.Handle` per material slot and one scalar
  `SamplerState.Handle`. Do not replace these with handle arrays until current
  Slang-RHI array support is verified on PPR's required backends.
- Missing texture slots use `0xFFFFFFFF` in `GpuMaterial`; C++ still binds the
  white fallback descriptor so every handle parameter is valid.
- The material's metallic/roughness texture is ORM: R is occlusion, G is
  roughness, and B is metallic.
- Descriptor handles and classic resource arrays indexed with
  `NonUniformResourceIndex` are different bindless models. Do not mix their
  rules. If a classic resource array is indexed divergently, use the required
  non-uniform annotation and validate generated code on the target backend.

Treat comments naming a Slang-RHI revision or upstream bug as evidence of a
workaround, not a permanent language rule. Reproduce the problem before
removing the workaround, and test the replacement before generalizing it.

## Shader compilation and programs

PPR's normal graphics-program flow is:

1. Initialize `IShaderService`; its session fixes row-major matrix layout.
2. Initialize RHI; it maps the created device type and selects the Slang compile
   target through `IShaderService::setTargetFormat`.
3. Load the module from a file or source string and capture diagnostics.
4. Find explicitly attributed entry points by name.
5. Build a `ShaderProgramDesc` with `LinkingStyle::SingleProgram`, the module as
   global scope, and the selected entry points.
6. Create the shader program through the RHI device.
7. Create pass-owned render/compute pipeline variants from that program.
8. Bind the pipeline, obtain its shader object, and populate it through
   `ShaderCursor` before encoding work.

Do not bypass `IShaderService` with an unrelated compiler session. The RHI
device receives PPR's global Slang session, and target selection/lifetime must
remain coherent.

Slang-RHI is actively refactored and does not advertise a stable public API.
When adding a new cursor or program operation, inspect the exact dependency
source/API used by the configured build instead of relying on examples from a
different revision.

## Source placement and staging

- Runtime shader assets belong under `assets/shaders/`.
- Small implementation-local shaders may remain embedded when there is a clear
  reason, as with the current ImGui integration. Do not move shaders between
  assets and C++ strings without considering deployment and rebuild behavior.
- Asset shaders are staged beside executables by CMake. When adding a new
  shader, confirm the relevant app/test target copies the containing shader
  directory and that the pass builds its path from the supplied content root.
- Avoid maintaining a hard-coded shader inventory in this skill. Inspect
  `assets/shaders/**/*.slang` because the inventory can outpace documentation.

## Common failure signatures

| Symptom | First checks |
|---|---|
| geometry clipped or behind camera | `mul(vector, matrix)`, `view * projection`, point `w`, no transpose/Y flip |
| parameter silently unchanged | exact reflected name, cursor level, `uniform`, `getDereferenced` for constant buffer |
| corrupted vertices/materials | CPU offsets, signedness, `elementSize`, structured-buffer stride |
| pipeline creation failure | diagnostics blob, entry-point names/stages, target compatibility, attachment signature |
| works on one backend only | explicit bindings, unsupported feature, target profile, non-uniform indexing, layout assumption |
| wrong texture sampled | material slot mapping, fallback sentinel, descriptor handle, sampler, ORM channel convention |
| shader file missing at runtime | asset path, content root, CMake staging, working directory |
| shutdown crash/double release | `SharedModule` ownership, pass/RHI/shader teardown order |

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

Prefer these official sources over search snippets or HLSL folklore:

- Matrix layout: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a1-01-matrix-layout.html>
- Conventional language and resource features: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/02-conventional-features.html>
- Compilation API: <https://shader-slang.org/docs/compilation-api/>
- Compilation and targets: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/08-compiling.html>
- Shader cursor model: <https://docs.shader-slang.org/en/latest/shader-cursors.html>
- Parameter blocks: <https://docs.shader-slang.org/en/latest/parameter-blocks.html>
- SPIR-V target behavior: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a2-01-spirv-target-specific.html>
- Metal target behavior: <https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/a2-02-metal-target-specific.html>
- Bindless convenience features: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/03-convenience-features.html>
- Slang target compatibility: <https://github.com/shader-slang/slang/blob/master/docs/target-compatibility.md>
- Slang-RHI status and source: <https://github.com/shader-slang/slang-rhi>

When an official statement conflicts with observed behavior in PPR's current
Slang-RHI revision, record the minimal reproducer and treat the local behavior
as a version-specific integration constraint, not as a general Slang fact.
