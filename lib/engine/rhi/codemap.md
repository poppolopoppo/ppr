# lib/engine/rhi/

## Responsibility

`engine.rhi` wraps Slang-RHI into `namespace pP::rhi`, providing GPU interfaces, resource descriptors, render-pass
types, surface management, error translation, and the `IRhiService` lifecycle interface.

## Design

- Core types include `IDevice`, `IAdapter`, `IBuffer`, `ICommandBuffer`, `ICommandQueue`, `IRenderPipeline`,
  `IShaderProgram`, `ITexture`, `ISurface`, `IFence`, `IHeap`, and `IInputLayout`.
- `IRhiService` is an `IService`-derived singleton that initializes the device, exposes it to the application, creates
  render pipelines, and shuts down the RHI.
- Projection helpers produce the common engine convention: Mango-native left-handed view space, row-major row-vector
  matrices, and [0,1] depth. There is no backend-specific camera-projection dispatch.
- Slang compilation uses the row-major session layout; D3D, Vulkan, Metal, and WGPU consume the same untransposed camera
  matrices.

## Flow

`IRhiService::get()` → initialize device and shared shader session → create surfaces/pipelines → `Renderer` submits
`DrawSubmission` spans (`renderAndPresent`/`submitToTexture`) → shutdown releases device and RHI resources.

## Integration

- **Consumers**: `engine.shader` for shader target/session setup and `engine.app` for device lifecycle, surfaces,
  pipelines, and render submission.
- **Depends on**: `engine.core`, `engine.math`, Slang-RHI SDK headers, and the platform window layer through application
  services.
- **Provides**: RHI interfaces, descriptors, common projection helpers, error categories, and `IRhiService`.

## Key Files

- `RHI.cppm` — exported RHI types, descriptors, projection helpers, and service interface.
- `RHI.cpp` — service implementation, device lifecycle, error translation, and projection helper definitions.
