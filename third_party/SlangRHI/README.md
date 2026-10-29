# Slang-RHI

Upstream: https://github.com/shader-slang/slang-rhi

Build route: CPM source build, `cmake/external/SlangRHI.cmake`:

- `GIT_TAG ad1b92f` — prefix of the fully verified
  `ad1b92f161a116716fab2eff34090de6a4b9ff5d` (2026-09-17, #864), matching the
  implementation-verified working-copy HEAD in the CPM cache; the comment in
  `SlangRHI.cmake` records that provenance rather than inferring it.
- `SLANG_RHI_ENABLE_CUDA/OPTIX/CPU/D3D11` all `OFF`; Vulkan and D3D12 keep
  their upstream defaults (`ON`). Runtime device creation goes through
  `DeviceType::Default` (slang-rhi's own API resolution) with an
  engine-side Vulkan adapter-probe override (`RHI.cpp` protects the Vulkan
  path from a broken DZN bridge ICD); D3D12/DXIL targets map to `sm_6_6`
  pinned DXBC/DXIL compilation.
- `SLANG_RHI_BUILD_SHARED OFF` static build; `unity build`, `CXX_MODULE_STD
  OFF`, SYSTEM includes; ASan/EnC injects `/D_ANNOTATE_STL` via
  `CMAKE_CXX_FLAGS`.

## License

Apache-2.0 WITH LLVM-exception (SPDX header in the upstream `LICENSE`).
Third-party code inside the tree carries its own notices under `LICENSES/`;
with redistribution, keep the applicable notices.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1519 | 61f6d4d529404f6f8270c30bbe15d755157424cc83a1af563b4840adfd8b5466 |

## Integration scope

`PatchSlangRhiSurface.cmake` builds a hash-checked private copy of the Vulkan
surface implementation. Acquisition/presentation `VK_ERROR_OUT_OF_DATE_KHR`
returns `SLANG_E_PENDING`, allowing Renderer to fence and reconfigure the
swapchain without claiming presentation or treating resize as device failure.
Other Vulkan errors retain upstream failure behavior; the CPM cache is unchanged.

- `engine.rhi` (EXTERNAL_SYSTEM_PUBLIC_DEPS slang-rhi slang) wraps the RHI
  interfaces into `pP::rhi`: `using`-aliases for `IDevice`, buffers, textures,
  pipelines, encoders, projections and descriptors; one shared Slang result
  error vocabulary with `engine.shader`; projection helpers are the canonical
  camera-contract implementation (row-major, row-vector, LH, `[0,1]` depth).
- The only PPR code touching slang-rhi/slang headers lives under
  `lib/engine/rhi`, `lib/engine/shader`, the two render passes and asset GPU
  fixtures (plus the game's capture/smoke clients).

## Second-order fetch targets (owned by slang-rhi's CMake)

When the Vulkan/D3D12 build is configured, slang-rhi itself fetches pinned
URL+SHA-256 archives (records live in its `CMakeLists.txt`) and stages what it
needs:

- **Slang** 2026.17.1 prebuilt (see [Slang](../Slang/README.md) — the only
  fetch target whose version PPR controls, via `SLANG_RHI_FETCH_SLANG_VERSION`).
- **DirectXShaderCompiler** `v1.9.2602` (`dxc_2026_02_20.zip`) — DXIL
  compilation for the D3D12 backend.
- **Direct3D 12 Agility SDK** `Microsoft.Direct3D.D3D12` 1.619.1 NuGet.
- **NVAPI** R610 (`NVIDIA/nvapi` commit `cd6918f60b3c9a0476fdfe7e89bb32330602049d` archive).
- **Vulkan-Headers** tag `v1.4.347` — slang-rhi always fetches its own copy
  when `SLANG_RHI_ENABLE_VULKAN` is on (its `FetchPackage(vulkan_headers)`
  call), so the vcpkg `vulkan-headers` manifest entry is not what this fetch
  consumes (see [Vulkan-Headers](../Vulkan-Headers/README.md)).
- **WinPixEventRuntime** 1.0.240308001 NuGet (same version PPR also declares
  via vcpkg; the DLL staged beside PPR executables is slang-rhi's copy — see
  [WinPixEventRuntime](../WinPixEventRuntime/README.md)).
- **Dawn/WebGPU binaries** 138.0.7204.168 for the WGPU backend.

Not fetched in the PPR configuration: OptiX, Nsight Aftermath, Metal C++
headers (CUDA/OPTIX off; Aftermath not enabled; no Apple presets in use for
this pin). These are upstream pinned details; do not reconcile them here —
updating `slang-rhi` takes them along.
