# Slang

Upstream: https://github.com/shader-slang/slang (documentation: https://shader-slang.org/)

Build route: **prebuilt release**, fetched by slang-rhi
(`SLANG_RHI_FETCH_SLANG ON`, `SLANG_RHI_FETCH_SLANG_VERSION 2026.17.1`) into
the build tree; PPR has no direct CMake for it. The fetched tree carries
`bin/`, `include/`, `lib/`, `LICENSE`, and `LICENSES/`.

## License

Top-level license: Apache-2.0 WITH LLVM-exception. The release tree also
carries `LICENSES/` with the third-party notice texts bundled in/with the
toolchain (`Apache-2.0.txt`, `BSL-1.0.txt`, `CC-BY-4.0.txt` (documentation),
`LicenseRef-UOI-NCSA.txt`, `LLVM-exception.txt`, `MIT.txt`,
`Unlicense.txt`). PPR ships the prebuilt `slang` compiler DLL next to its
executables; keep the applicable notice files with redistributed binaries.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1519 | 61f6d4d529404f6f8270c30bbe15d755157424cc83a1af563b4840adfd8b5466 |
| LICENSES/Apache-2.0.txt | 10353 | 9bf4e882bdd75e8d10fc788c53d0a787ef0f59cead1fe1f0878c240896fc8610 |
| LICENSES/BSL-1.0.txt | 1345 | 5adb64129956e45be2b27cba102d99425d82bcb81cf6c81c5a43f27c8959a097 |
| LICENSES/CC-BY-4.0.txt | 17179 | 4500200b91acb1324538d8e8f9665d5f4142d3f1c5cd9730564cf1895b6170f7 |
| LICENSES/LicenseRef-UOI-NCSA.txt | 2778 | 5389e656edf87841e249ad249284553004e09c7dfb1304162ded06b1a39f5c0f |
| LICENSES/LLVM-exception.txt | 934 | 4e6a1c0b79241b02764099699f1dbd2c606d85b1fdf44037e3c60f32083ab549 |
| LICENSES/MIT.txt | 1087 | d62678ed4c712a4b33f043790f28b00a8d74986c3eba4e01b06f1ba767dcaabc |
| LICENSES/Unlicense.txt | 1221 | 57eb25943fb7e6400920a434acc7fc2aed841b2a28cfdabc62306aa0e06198f0 |

(Hashes taken from the release tree consumed by the current slang-rhi build.)

## Integration scope

- `engine.shader`: `IShaderService` owns `IGlobalSession` + `ISession`; the
  session is created **row-major** (`SLANG_MATRIX_LAYOUT_ROW_MAJOR`) and
  single-target; `slang.h`/`slang-com-ptr.h` live in private implementation
  units only.
- `engine.rhi` re-derives the session target from the created device type and
  builds devices through slang-rhi; DXIL targets pin `sm_6_6`.
- Shader source is compiled at startup from `assets/shaders/` by
  `engine.shader` (`mesh_bindless.slang`, `grid_tiles.slang`), and the game's
  smoke/capture clients compile modules the same way. Embedded shader strings
  (UI overlay) follow the Slang source conventions (see the
  `slang-shader-developer` skill for authoring rules).
- Errors surface only as engine error codes (`shader::errc`, the shared Slang
  `Result` mapping); no Slang type crosses a module interface.

## Why 2026.17.1

The version is pinned by PPR through slang-rhi's documented option and the
matching prebuilt release; changing it means re-verifying the whole shader
stack (module compile, reflection/CPU-GPU layouts, DXBC/DXIL targets) as one
reviewed change — see the Slang-RHI entry for the fetch wiring.

## Runtime distribution note

The fetched slang compiler binary (`slang-compiler.dll`) and the staged
`WinPixEventRuntime.dll` sit beside PPR executables after
`cmake/RuntimeDlls.cmake`'s POST_BUILD staging, as do the vcpkg runtime DLLs
of the x64-windows triplet. The guard reason is historical but real: the
Windows loader must not resolve slang's compiler from a foreign PATH
location (a system `C:\VulkanSDK` copy previously shadowed it for test
executables).
