# WinPixEventRuntime

Upstream: https://devblogs.microsoft.com/directx/pix-in-visual-studio/ (the
runtime behind PIX instrumentation; NuGet package `WinPixEventRuntime`)

Version: **1.0.240308001** — declared by vcpkg (`winpixevent`, windows-only)
and added together with the profiling markers feature (`Core.HAL` PIX-backed
`profileBegin`/`profileEnd`/`ProfileScope`, commit 7fe7395, 2026-09-29).
Slang-RHI's own CMake fetches the identical NuGet version for its D3D12
debug layer, so there is one version in the tree.

## License

MIT (Copyright (c) Microsoft Corporation.) — the notice file that ships with
the installed package. Keep it with redistributed software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1093 | 903df5512f7d02609fed0c780a9b704f5a3eeb6e4d84ebe42a29845c81899a3c |

The preserved file is the installed vcpkg copyright
(`share/winpixevent/copyright`); for binary distribution also mind the
upstream package terms recorded with the NuGet payload.

## Integration scope

- `lib/engine/core/hal/windows/Core.HAL.windows.Debugger.cpp` includes
  `<pix3.h>` and implements `hal::profileBegin`/`profileEnd`/`ProfileScope`
  over it (category coloring via private FNV-1a hue mapping; uncategorized
  scopes hash the first dotted name segment; the engine-wide `Blocking`
  category declared in `Core.HAL.cppm` is reserved for the red override).
  Other HAL platforms provide the same marker surface as no-ops/stubs.
- `WinPixEventRuntime.dll` is staged beside every PPR executable by
  `cmake/RuntimeDlls.cmake` (`ppr_stage_winpix_event_runtime`, Windows +
  D3D12-enabled builds), sharing slang-rhi's runtime copy; the loader must
  resolve precisely that DLL rather than a PATH straggler.
- The header is a PRIVATE HAL detail: profiling vocabulary exported by
  `engine.core:hal` is engine-owned, PIX types never cross a module
  interface. Binary support is Windows-only per the platform dependency.
