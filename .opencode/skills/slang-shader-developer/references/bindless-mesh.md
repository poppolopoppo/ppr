# Current bindless mesh seam

Read this reference only for mesh-specific work: `mesh_bindless.slang`,
`TrianglePass`, mesh CPU/GPU mirrors, payload grouping, texture/material heaps,
or their bindings and failures. Apply the [entry skill](../SKILL.md) first;
its shader-authoring, ABI, reflection, and validation rules remain mandatory.

`assets/shaders/mesh_bindless.slang` is the live bindless mesh program; loaded by
`TrianglePass::createShaderProgram_` from
`<content_dir>/shaders/mesh_bindless.slang`. It contains deliberate,
dependency-version-specific rules:

- Global scope contains one `ConstantBuffer` (`g_frame`) plus structured buffers
  (`g_vertices`, `g_indices`, `g_materials`, `g_payloads`) and NO ordinary data.
  This is not stylistic: a module-scope ordinary global makes
  `D3D12SerializeRootSignature` fail, because Slang emits it into a generated
  `globalParams` CBV at b0 that overlaps the shifted `g_frame` at b1, and program
  creation fails for every D3D12 test. Verified with the vendored slangc
  2026.17.1: the base-as-global form emitted `globalParams`@b0 + `g_frame`@b1;
  the base-as-entry-point form emits `g_frame`@b0 + `entryPointParams`@b1 with no
  collision. Never promote per-draw ordinary data to a global to "clean up" the
  entry point — that reintroduces this failure.
- Per-instance payload data rides `StructuredBuffer<InstancePayload> g_payloads`.
  The deleted `vertexMain` seam used `uniform float4x4 g_model` plus a scalar
  `PushScalars` entry-point parameter; both are gone.
- `g_payload_base` is the one per-draw uniform, and it is a `uniform` parameter on
  the `vertexIndirectMain` entry point — not a global. Globals and entry-point
  `uniform` params resolve through the SAME flat cursor, so the binding call is
  identical either way and did not change when this moved:
  `shader_cursor["g_payload_base"].setData(&base, sizeof(base))`. Precedent is
  the deleted `g_model`/`g_push` pair, bound identically at
  `shader_cursor["g_model"].setData(&m_model, sizeof(float4x4))` and
  `shader_cursor["g_push"].setData(&scalars, sizeof(scalars))` (commit
  `5d65612`, `App.Renderer.TrianglePass.cpp`). The cursor is one
  `rhi::ShaderCursor(shader_object)` over the bound pipeline's shader object, not
  a per-entry-point cursor. Plain data needs bare `setData`; only a container such
  as `g_frame` needs `getDereferenced` first.
- `g_payload_base` is mandatory, not an optimization. `SV_InstanceID` is
  draw-local on D3D12, SPIR-V, and Metal alike, so the CPU's
  `startInstanceLocation` never reaches the shader. Indexing
  `g_payloads[sv_instance_id]` therefore makes every instanced draw read
  payload 0, and makes a single-instance indirect draw (always instance 0)
  render payload 0 for the whole scene. C++ encodes `startInstanceLocation = 0`
  and sets `g_payload_base` per draw group to the group's first payload index;
  the shader computes `g_payloads[g_payload_base + sv_instance_id]` with a plain
  `u32` addition, which matches the payload index domain exactly. If C++ ever
  reinstates a nonzero `startInstanceLocation`, these two must change together.
- The texture heap and the shared sampler are FRAGMENT entry-point `uniform`
  parameters: `StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures`
  and one scalar `SamplerState.Handle g_sampler`. Textures are a heap in one
  `StructuredBuffer`, not one `Texture2D<float4>.Handle` parameter per material
  slot. Do not "simplify" this back to per-slot handle parameters or to a
  handle array without checking current Slang-RHI support on every required
  backend.
- Missing texture slots use `0xFFFFFFFF` in `GpuMaterial`. The white fallback is
  in-heap slot 0: `resolveTextureSlot` maps `0xFFFFFFFF` to slot 0, and C++
  writes the fallback descriptor into heap slot 0 at cache initialization and
  allocates host texture slots from 1. C++ does not bind a fallback per material
  slot. Because albedo, ORM, and emissive MULTIPLY the factor values, an absent
  map must read as white (1.0), not black.
- Heap residency is not one-slot-per-load, and the caches differ here. Texture
  slots ARE content-deduped and refcounted, so one heap slot serves many loads.
  Material slots are NOT deduped: one slot per `pack` call, allocated from slot 0
  (material slot 0 is a real material, not a fallback), and `release` rewrites the
  slot to a zeroed `GpuMaterial` so a stale index still samples the fallback.
  Mesh bags are never content-deduped either, so a `vb_offset`/`ib_start` pair
  always identifies exactly one bag region.
- Draw grouping, and therefore payload layout: C++ groups staged draws by
  `(variant, bag bucket, vb_offset, ib_start, count, base_vertex)` and lays each
  group's payloads out contiguously in first-group-first order, so one draw's
  instances are `base .. base + count - 1`. That grouping key is the contract the
  shader's `g_payload_base + sv_instance_id` arithmetic relies on.
- The heap buffer is created with a null init payload and holds
  `rhi::kBindlessTextureBudget` (4096) entries, so a never-written slot reads
  back a garbage handle. `resolveTextureSlot` therefore maps any out-of-bounds
  slot — anything other than a valid resident slot or the `0xFFFFFFFF` sentinel —
  to slot 0, so a bad index degrades to defined factor-only rendering instead of
  aliasing the last heap slot. That fallback is an explicit fail-safe decision,
  not silent clamping, and it is unreachable today because host slots are
  bump-allocated below the budget.
- The material's metallic/roughness texture is ORM: R is occlusion, G is
  roughness, and B is metallic. Albedo, ORM, and emissive all MULTIPLY the
  factor values, so an absent texture must read as white (1.0), not black.
- Descriptor handles and classic resource arrays indexed with
  `NonUniformResourceIndex` are different bindless models. Do not mix their
  rules. They are also not symmetric in support, which is easy to get backwards:
  `nonuniform()` is a function over `DescriptorHandle<T>` ONLY, so it cannot
  wrap a `StructuredBuffer` element read at all; the classic-SRV counterpart is
  `NonUniformResourceIndex`. But with the vendored Slang 2026.17.1 that
  counterpart FAILS to compile in the fragment stage for the `metal` and `wgsl`
  targets ("uses features that are not available in 'fragment' stage"), while
  the plain index compiles for `spirv`, `hlsl`, `metal`, and `wgsl`. Since
  `input.material` indexes a `StructuredBuffer` — which tolerates a divergent
  index on every backend — the mesh seam deliberately leaves `g_materials`
  unannotated. Do not "make it consistent" with the heap reads: doing so with
  `nonuniform()` is a type error, and doing so with `NonUniformResourceIndex`
  trades a working shader on Metal/WGSL for a D3D12/SPIR-V-only build. Re-verify
  across backends before changing either form.
- `mul(vector, model)` for directions is exact for a rigid transform or a
  uniform scale only. The shader states that contract in a comment; do not add
  an inverse-transpose without also changing `InstancePayload`'s size and the
  CPU-side `static_assert`s that lock it.

Treat comments naming a Slang-RHI revision or upstream bug as evidence of a
workaround, not a permanent language rule. Reproduce the problem before
removing the workaround, and test the replacement before generalizing it.
