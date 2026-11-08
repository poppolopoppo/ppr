# Slang-RHI

Upstream: https://github.com/shader-slang/slang-rhi

Build route: CPM source build, `cmake/external/SlangRHI.cmake`:

- `GIT_TAG a78e208` — prefix of the reviewed
  `a78e208be303df9f934a52e5557665b9b3538cb9` (2026-10-29, #949); the patch's
  hash gate in `PatchSlangRhiSurface.cmake` re-records the reviewed
  `vk-surface.cpp` at this revision, and `SlangRHI.cmake` records the pin
  provenance rather than inferring it.
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
Since 2026-10-29 upstream itself drops an outdated present with `SLANG_OK`;
the patch keeps the observable `SLANG_E_PENDING` so the not-presented image
stays visible to `settlePresent()`. Other Vulkan errors retain upstream
failure behavior; the CPM cache is unchanged.

`PatchSlangRhiMsaa.cmake` is included after the surface patch in `SlangRHI.cmake`;
the initial standard-location/allocation recipe passed coordinated configure,
rebuild and the registered two-leaf Core/GPUAV entry on RTX 2080 (7.01 seconds).
The subsequent sample-rate feature and actual spatial integration remain a
separate source/execution qualification. No post-change ordinary clean gate is
claimed from that opt-in result.
It generates a complete public-header overlay plus a private Vulkan device
source copy. Original public include roots are removed from both the target and
its consumer paths, so quoted sibling includes cannot mix feature enum/count
definitions. The appended `StandardSampleLocations4` feature preserves existing
enum values and `DeviceInfo`; its string is `standard-sample-locations-4`.
Only Vulkan sets it, from the already queried `standardSampleLocations` property.
This reports the core standard indexed table while programmable positions remain
disabled. It is independent of format/count support. Other backends leave it
false; required D3D12 standard-quality texture/PSO coherence remains follow-up
work, with only the new spatial opt-in unavailable there initially.

The spatial integration append also exposes `SampleRateShading` through
`sample-rate-shading`, independently of standard sample locations. Only the
Vulkan creation path with no externally supplied device handle advertises it,
from `m_api.m_deviceFeatures.sampleRateShading`: that supported base-feature
structure is passed to native device creation. The pin's pipeline field remains
unchanged (`sampleShadingEnable = VK_FALSE`); statically used shader `SampleId`
forces full sample shading under the
[Vulkan rasterization contract](https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-sampleshading),
and the enabled feature permits the corresponding
[SPIR-V capability](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html#spirvenv-capabilities).
The appended enum preserves earlier values, including `StandardSampleLocations4`.
RetroPass and GridPass require both features for the new default-off route.
Feature exposure and actual sample-frequency integration await their own review
and execution; the prior two-leaf Core/GPUAV pass does not qualify this append.

The reactive MRT append exposes `IndependentBlendEnabled` through
`independent-blend-enabled`, only for a self-created Vulkan device with
`m_api.m_deviceFeatures.independentBlend`. At this pin `initPhysicalDevice`
queries that base-feature structure, and `deviceCreateInfo.pEnabledFeatures`
passes the same structure to `vkCreateDevice`. A failed creation publishes no
device; an imported device has no enabled-state proof and never advertises the
capability. No backend feature is newly enabled by this append. The existing
normalized source hashes, single-anchor replacement and clean-input guards
remain unchanged; coordinated regeneration/build/runtime qualification is still
required for this capability and the default-off temporal2x route.

The same recipe guards `getTextureAllocationInfo` before its temporary image
creation, for committed, device-local, default-quality, extension-free, nonshared,
nontypeless 2D/2DMS descriptors with these exact format/usage roles:

- RGBA16F: sampled colour attachment.
- D32: sampled depth attachment.
- RGBA16F: single-sample sampled storage output.

Optional copy-source/destination flags are included exactly when requested.
Positive dimensions/layers/mips, single-bit sample counts through 64, type/count
agreement, normalized mip bounds and MSAA mip rules are checked before querying
Vulkan. The query uses the normalized image format/type/tiling/usage/flags.
These come from the pin's existing `getVkImageCreateInfo(desc, false)` helper,
which remains unchanged. Committed creation without initial data uses identical
builder inputs. A non-null `heap` or nonzero `heapOffset` is explicitly outside
the guard; placed creation can add transfer-destination usage and remains unscoped.
Unsupported sample bits, format-specific limits and framebuffer limits are
rejected before `vkCreateImage`. `VK_ERROR_FORMAT_NOT_SUPPORTED` becomes
`SLANG_E_NOT_AVAILABLE`; other query errors retain upstream propagation. A legal
query can still fail during temporary image creation or allocation. This is
neither a universal unsupported-descriptor probe nor a createTexture guard.
Descriptors outside the listed scope retain upstream behavior. No Vulkan
headers, loader or native types enter engine consumers.

The normalized LF source hashes checked by the recipe are:

| Source | SHA-256 |
|---|---|
| `include/slang-rhi.h` | `31bd6ad07b48543a1026b2abab5ba582781d691cfdf9394b3a6a2013e47a69eb` |
| `src/vulkan/vk-device.cpp` | `274541a3e60b84913c24358401a9a09d718946b382ad98c1e47f8a34093a457d` |
| `src/vulkan/vk-texture.cpp` (shared native builder, guarded but unchanged) | `10797bb78d5444f804982e5e7c9f0d92befc28190c19dc6bfa1dc826b3cb6a9d` |

The initial recipe incorrectly recorded hashes from a clean, older CPM checkout
at `ad1b92f161a116716fab2eff34090de6a4b9ff5d`, rather than the selected
`a78e208be303df9f934a52e5557665b9b3538cb9`. First configure rejected those
hashes before generating an overlay. The correction was reviewed against the
actual clean pin, including its newly added heap fields and shared native image
builder; it is not a line-ending adjustment or a dependency version change.

The existing spatial raster-threshold leaf also checks the string/enum feature
boundary and valid, invalid and unsupported allocation queries with this recipe
enabled. It requires the Vulkan standard-location capability on the tested
Vulkan device; the empirical threshold probe remains a separate check. Configure
idempotence and coherent compiler include paths were checked for the initial
recipe. The sample-rate append requires refreshed configure/include-coherence,
dependency rebuild and actual sample-frequency execution evidence.

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


## Triangle draw constants use the unmodified Vulkan layout

The attempted entry-stage/byte-offset overlay was withdrawn after source review
identified `VUID-vkCmdPushConstants-offset-01796`: overlapping stage ranges cannot
hold independent payloads through separate per-stage updates. Its exact sources,
failed-run evidence and removal provenance remain in the receiver-scene diagnostic
artifacts. The pin, CPM shader-layout source and all existing surface/MSAA patches
remain unchanged; no push-constant layout/command patch is active.

PPR instead places the vertex payload base in descriptor-backed
`ConstantBuffer<DrawConstants> g_draw`, a16-byte four-u32 row, alongside the
unchanged288-byte `g_frame`. Global ordinary data remains absent. The vertex entry
has no ordinary parameters; only the fragment texture/sampler descriptor handles
use one ordinary push-constant block. Cached per-slot root/subobject cursors and
existing frame completion fences own updates; no cursor translation is needed.
The original one-sample payload-group check and production HDR/MSAA4 resolve
checks must qualify the revised shader contract under actual Core validation.
This narrow repair does not establish arbitrary multi-stage push-constant support.

## Descriptor batching experiment (withdrawn)

The approved 2026-11-06 experiment used a hash-checked private Vulkan source to
batch up to 32 consecutive raw-buffer descriptors. Exact helper/traversal tests
and independent source review passed. Optimized Nsight captures reduced update
calls by62.5%, but recorded only4.632ms less API time across the whole capture,
with no demonstrated end-to-end improvement. This does not establish a material
benefit sufficient to maintain another private overlay, so its production recipe
and activation were removed under the design's retention criterion. Existing
surface/MSAA overlays and the upstream pin are unchanged.

The experiment, optimized baseline/candidate binaries, source copy, measurements,
and correctness results are preserved in
`out/diagnostics/windowed-native-20261106/descriptor-batching/report.md`.
