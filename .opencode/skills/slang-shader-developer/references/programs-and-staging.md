# Shader programs, staging, and failure triage

Read the [entry skill](../SKILL.md) for shader-authoring, CPU/GPU ABI,
ShaderCursor/reflection, ownership, and validation requirements. For mesh
failures also read the conditional [bindless mesh seam](bindless-mesh.md).

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
| every instance draws the first instance's model/material | `g_payload_base` bound per draw group, `startInstanceLocation` (must be 0), `SV_InstanceID` assumed draw-local |
| pipeline creation failure | diagnostics blob, entry-point names/stages, target compatibility, attachment signature |
| works on one backend only | explicit bindings, unsupported feature, target profile, non-uniform indexing, layout assumption |
| wrong texture sampled | material slot mapping, fallback sentinel, descriptor handle, sampler, ORM channel convention |
| shader file missing at runtime | asset path, content root, CMake staging, working directory |
| shutdown crash/double release | `SharedModule` ownership, pass/RHI/shader teardown order |
