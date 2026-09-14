# lib/engine/rhi/

## Responsibility

`engine.rhi` wraps Slang-RHI into `namespace pP::rhi`: GPU interface/descriptor aliases, Slang-`Result` error
mapping, row-major/row-vector projection helpers, and the `IRhiService` device-lifecycle singleton
(`engine.rhi` is a single module — `RHI.cppm` interface + `RHI.cpp` implementation).

## Design

- Global fragment includes `pP/Macros.h` + `slang.h`/`slang-com-ptr.h`/`slang-com-helper.h`/`slang-rhi.h`/
  `slang-rhi/shader-cursor.h`; `import engine.core; import engine.math; import engine.shader; import std;`.
  `namespace slang_rhi { using namespace rhi; }` normalizes the upstream namespace.
- Thin `using slang_rhi::X` aliases in `pP::rhi`: `ComPtr` (via `Slang::`), `IRHI`/`IAdapter`/`IBuffer`/
  `ICommandBuffer`/`ICommandEncoder`/`ICommandQueue`/`IComputePipeline`/`IComputePassEncoder`/`IDebugCallback`/
  `IDevice`/`IFence`/`IHeap`/`IInputLayout`/`IRenderPipeline`/`IShaderObject`/`IShaderProgram`/`IShaderTable`/
  `ISurface`/`ITexture`/`ITextureView`/`IRenderPassEncoder`/`ISampler`; descriptors `BufferDesc`/`BufferOffsetPair`/
  `BufferRange`/`BufferUsage`/`ColorClearValue`/`ColorTargetDesc`/`ComputePipelineDesc`/`DeviceAddress`/`DeviceDesc`/
  `DeviceInfo`/`DeviceLimits`/`DeviceType`/`DrawArguments`/`Format`/`FormatInfo`/`FormatKind`/`FormatSupport`/
  `IndexFormat`/`InputElementDesc`/`InputLayoutDesc`/`LoadOp`/`LinkingStyle`/`MemoryType`/`PrimitiveTopology`/
  `QueueType`/`RenderPassDesc`/`RenderPassColorAttachment`/`RenderPassDepthStencilAttachment`/`RenderPipelineDesc`/
  `RenderState`/`ResourceState`/`AspectBlendDesc`/`Binding`/`BlendFactor`/`BlendOp`/`CpuAccessMode`/`CullMode`/
  `Extent3D`/`Offset3D`/`RenderTargetWriteMask`/`Result`/`SamplerDesc`/`ScissorRect`/`ShaderOffset`/
  `SubresourceLayout`/`SubresourceRange`/`TextureAddressingMode`/`TextureDesc`/`TextureFilteringMode`/
  `TextureType`/`TextureUsage`/`TextureViewDesc`/`ShaderProgramDesc`/`StoreOp`/`StructType`/`SubresourceData`/
  `SurfaceConfig`/`SurfaceInfo`/`Viewport`/`ShaderCursor`/`WindowHandle`/`DebugLayerOptions`/`DebugMessageSource`/
  `DebugMessageType`/`MarkerColor`; functions `getFormatInfo`/`getTextureDimension`.
- Errors: `using errc = shader::errc` (one shared Slang vocabulary); `error_category()`/
  `make_error_code(Result|errc)` over file-local `SlangRhiErrorCategory` (`name() "slang-rhi"`, full `message()`
  switch with `std::format` fallback, `default_error_condition` mapping invalid-arg→`invalid_argument`,
  OOM→`not_enough_memory`, not-found→`no_such_file_or_directory`, timeout→`timed_out`,
  not-implemented→`function_not_supported`, buffer-too-small→`result_out_of_range` via static
  `slangRhiErrorCondition_`); `make_error_code(Result)` returns success on `SLANG_SUCCEEDED` else
  `{result, g_slang_rhi_error_category}` (`constexpr` instance in `RHI.cpp`). `export namespace Slang
  { using pP::shader::make_error_code; }` plus `export namespace pP { using Slang::make_error_code; }`
  re-export the shader error mapper so `PPR_RETURN_ERROR_ON_FAIL` resolves via ADL on `Slang::Result`;
  the `PPR_RETURN_*_ON_FAIL` failure predicate `Slang::hasFailed(Result)` (`SLANG_FAILED`, `constexpr`) is
  defined in `engine.shader`, not here.
- Projection helpers encode the engine-wide camera contract (Mango-native left-handed view space, row-major
  row-vector `mul(float4, matrix)`, `[0,1]` depth, no per-backend dispatch): `getOrthoMatrix(w,h)` →
  `float4x4::orthoD3D(0,w,0,h,0,1)`; `getPerspectiveMatrix(fov,aspect,near,far)` derives horizontal FOV
  `x_fov = 2·atan(tan(fov/2)·aspect)` then `float4x4::perspectiveD3D(x_fov,fov,near,far)` (`fov` is vertical).
- `IRhiService : IService` (`pP::`, `safe_ptr` singleton via `get()`): `initialize(DeviceType, IShaderService&)`,
  `shutdown()`, `getInstance()` (`*slang_rhi::getRHI()`), `getDevice()` (`*m_device`), `createRenderPipeline(desc,
  out)` (forwards to `m_device->createRenderPipeline`). `SlangRhiService` (`RHI.cpp`, `ComPtr<IDevice> m_device`):
  idempotence guard (`m_device` → warning + `errc::ok`); null `getRHI()` → `no_interface`; `#if PPR_ENABLE_DEBUG`
  debug layers (`setDebugLayerOptions{required, coreValidation, GPUAssistedValidation}` + `enableDebugLayers()`,
  `enableValidation = true`); required `Feature::{Surface, Rasterization}` with `safe_narrowing(size)` count;
  `DeviceDesc{deviceType, slang.slangGlobalSession = shader_service.getGlobalSession(), requiredFeatures,
  debugCallback}` (`#if PPR_ENABLE_LOGGING` wires `SlangRhiDebugCallback::get()` singleton mapping
  `Layer/Driver/Slang` source names and `Info/Warning/Error` → `Log::ELevel` via `logRaw` with `source` attr);
  `createDevice` via `PPR_RETURN_ERROR_ON_FAIL`, then `setTargetFormat(toSlangCompileTarget_(device->getDeviceType()))`
  (targets the actual created device type, not the requested one: `DXBC` for Default/D3D11/D3D12, `SPIRV`
  for Vulkan/WGPU, `METAL`, `SHADER_HOST_CALLABLE` for CPU, `CUDA_OBJECT_CODE` for CUDA;
  `getDeviceTypeName_` (`#if PPR_ENABLE_LOGGING`-gated alongside `SlangRhiDebugCallback`) logs
  `default/d3d11/d3d12/vulkan/metal/cpu/cuda/wgpu`);
  `shutdown()` nulls device, `createRenderPipeline` delegates. Both unreachable switches use `std::unreachable()`.

## Flow

`IShaderService::get()->initialize()` (global + row-major session) →
`IRhiService::get()->initialize(deviceType, shaderService)` (`getRHI()` → debug layers → `DeviceDesc` with
shader global session → `createDevice` → `setTargetFormat(toSlangCompileTarget_(device->getDeviceType()))` →
`m_device` stored + info log) → surfaces/pipelines (`createRenderPipeline` → `IDevice`) → `Renderer` submits
frames → `shutdown()` (`m_device.setNull()`) → shader `shutdown()` drops session then global session.
`getOrthoMatrix`/`getPerspectiveMatrix` are pure free functions consumed at pass/scene level.

## Integration

- Depends on: `engine.core` + `engine.math` + `engine.shader` (all public — `IService`/`safe_ptr`/`Log`/
  `safe_narrowing`, `float4x4`, `errc`/session/targets), `slang-rhi` + `slang` (public system deps for downstream
  `Result`/`ComPtr`/`DeviceType` vocabulary).
- Consumed by: `engine.app` (renderer, surfaces, pipeline creation), `game` (via `engine.app`).
- Build: `RHI.cppm` in `FILE_SET CXX_MODULES`, `RHI.cpp` private;
  `setup_ppr_project(engine.rhi INTERNAL_PUBLIC_DEPS engine.core engine.math engine.shader
  EXTERNAL_SYSTEM_PUBLIC_DEPS slang-rhi slang)`.

## Key Files

- `RHI.cppm` — aliases, projection declarations, error API, `Slang::hasFailed`, `IRhiService`.
- `RHI.cpp` — `SlangRhiErrorCategory`, `SlangRhiDebugCallback`, `getDeviceTypeName_`,
  `toSlangCompileTarget_`, projections, `SlangRhiService`, `PPR_DEFINE_LOG_CATEGORY(RHI, info, none)`.
