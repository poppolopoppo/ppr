# lib/engine/rhi/

## Responsibility

`engine.rhi` wraps Slang-RHI into `namespace pP::rhi`: GPU interface/descriptor aliases, Slang-`Result` error
mapping, common projection helpers, and the `IRhiService` device-lifecycle singleton (`engine.rhi` is a single
module — `RHI.cppm` interface + `RHI.cpp` implementation).

## Design

- Thin `using slang_rhi::X` aliases (`namespace slang_rhi { using namespace rhi; }`): devices, adapters,
  buffers, command buffers/encoders/queues, compute/render pipelines and pass encoders, shaders, textures and
  views, surfaces, fences, heaps, samplers, input layouts, `ShaderCursor`, `WindowHandle`, plus all descriptor
  structs (`BufferDesc`, `DeviceDesc`, `RenderPipelineDesc`, `ShaderProgramDesc`, `SurfaceConfig`, …).
- Errors: `using errc = shader::errc`; `error_category()`/`make_error_code(Result|errc)` over a `SlangRhiErrorCategory`
  (`slang-rhi`, `std::errc` conditions for invalid-arg/OOM/not-found/timeout/not-implemented/buffer-too-small);
  `pP::hasFailed(rhi::Result)` (`SLANG_FAILED`) as the ADL target for `PPR_RETURN_*_ON_FAIL`.
- Projection helpers encode the engine-wide convention (Mango-native left-handed view space, row-major
  row-vector `mul(float4, matrix)`, [0,1] depth, no per-backend dispatch): `getOrthoMatrix` →
  `float4x4::orthoD3D(0,w,0,h,0,1)`; `getPerspectiveMatrix` derives horizontal FOV then `perspectiveD3D`.
- `IRhiService : IService` (`pP::`): `initialize(DeviceType, IGlobalSession*)`, `shutdown()`, `getInstance()`,
  `getDevice()`, `createRenderPipeline()`. `SlangRhiService` (`RHI.cpp`): debug-layer + validation in debug
  builds, debug callback → `PPR_LOG(RHI, …)` with source tag, required features `{Surface, Rasterization}`,
  `DeviceDesc` wired to the shader service's global session, then `setTargetFormat(toSlangCompileTarget_(…))`
  (`DXBC` for D3D, `SPIRV` for Vulkan/WGPU, `METAL`, host-callable for CPU, CUDA object code).

## Flow

`IShaderService::initialize()` → `IRhiService::get()->initialize(deviceType, globalSession)` (creates device,
configures shader target) → surfaces/pipelines (`createRenderPipeline` forwards to `IDevice`) →
`Renderer` submits frames → `shutdown()` releases device and destroys the RHI instance.

## Integration

- Depends on: `engine.core` + `engine.math` + `engine.shader` (all public — types, matrices, `errc`/session),
  `slang-rhi` + `slang` (public system deps).
- Consumed by: `engine.app` (renderer, surfaces, pipeline creation), `game` (via `engine.app`).
- Build: `RHI.cppm` in `FILE_SET CXX_MODULES`, `RHI.cpp` private;
  `setup_ppr_project(engine.rhi INTERNAL_PUBLIC_DEPS engine.core engine.math engine.shader
  EXTERNAL_SYSTEM_PUBLIC_DEPS slang-rhi slang)`.

## Key Files

- `RHI.cppm` — aliases, projection declarations, error API, `hasFailed`, `IRhiService`.
- `RHI.cpp` — error category, debug callback, `toSlangCompileTarget_`, projections, `SlangRhiService`.
