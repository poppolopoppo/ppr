# Mango

Upstream: https://github.com/poppolopoppo/mango (original project: https://github.com/t0rakka/mango)

Build route: CPM source build from the `poppolopoppo/mango` fork, `GIT_TAG main`
— a **moving branch, not an immutable pin**. The commented-out upstream line
in `cmake/external/Mango.cmake` shows the original `t0rakka/mango` tree was
considered first; the tree publishes from the fork because PPR relies on
fork-side capabilities (the MikkTSpace tangent regeneration path used by
`engine.mesh`, in-fork behavior referenced by `lib/engine/mesh/codemap.md`).

Resolved revision consumed by the local CPM cache when this survey was
written:

- fork: `poppolopoppo/mango` `main`
- commit: `a09154174636fe11eaced1fc9dbe324b7be4c5f4` (2026-09-24)

A moving ref means any later upstream change is picked up by the next clean
configure. If provenance stability is required, pinning an immutable commit
in `Mango.cmake` is the fix and is its own reviewed change.

## License

Mango is under a zlib-style license (commercial use, alteration and
redistribution permitted; altered source must be plainly marked; the notice
must not be removed):

- `LICENSE` preserves the fork's `license` file at the resolved revision
  (Copyright (c) 2012-2026 Twilight Finland 3D Oy Ltd).

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 916 | ca77ba6cb0fb6e85d96b9409d4626d52ba695125d7e05584a08a1177d11f1f9a |

`THIRD-PARTY-LICENSES.txt` preserves `docs/text/licenses.txt` from the same
tree (35,108 bytes,
`2b5965eafb00a806330c0acbe5e6946ece613102258fb34f37f9b3b7b792ff6f`): Mango
bundles code under other notices (concurrentqueue BSD, zstd BSD, lzav, lzfse,
inflatelib, zpng, unrar, among others). PPR builds Mango statically, so
bundled code compiled into `mango*` libraries ends up inside PPR binaries;
keep this consolidated notice with any redistribution. Which entries actually
ship depends on the configured Mango backends (`BUILD_IMPORT3D ON` here;
OpenGL, Vulkan, examples, tests, tools off) — the directory-level lodepng
notice is example-only and is not built.

## Integration scope

- `engine.math`: re-exports `mango::math` into `namespace pP` as a single C++20
  module (`Math.cppm`, `EXTERNAL_SYSTEM_PRIVATE_DEPS mango`); projection
  helpers in `engine.rhi` build on mango's `orthoD3D`/`perspectiveD3D`.
- `engine.image`: PNG/JPG/KTX2/DDS decode through `mango-image`
  (`EXTERNAL_SYSTEM_PRIVATE_DEPS mango-image stb`); one decoder per job, Mango
  headers never exported through the module interface.
- `engine.mesh`: static glTF/GLB import through `mango-import3d`
  (`EXTERNAL_SYSTEM_PRIVATE_DEPS mango-import3d`), with PPR-side GLB-embed
  resolution (`resolveGlbEmbeds`) because raw mango blob views dangle.
- Flag hygiene in `cmake/external/Mango.cmake`: strips `/MP`, `/arch:AVX*`,
  `/Ox`, and the Clang `-m(avx512|avx|sse|bmi|fma|sha|aes|pclmul|popcnt|f16c|lzcnt)`
  switches from every `mango*` target's
  `INTERFACE_COMPILE_OPTIONS` (they would fork std-module synth targets and
  mismatch BMIs), sets `CXX_MODULE_STD OFF`, and mirrors each target's own
  include dir to `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES`; ASan/EnC builds inject
  `/D_ANNOTATE_STL` via `CMAKE_CXX_FLAGS`.
- Build options: `ENABLE_AVX/AVX2/SSE2/SSE4 ON`, `BUILD_OPENGL/VULKAN OFF`,
  `BUILD_IMPORT3D ON`, `BUILD_EXAMPLES/TESTS/TOOLS/SHARED_LIBS OFF`. Mango's
  vcpkg-provided dependencies (fmt, zlib, libdeflate, zstd, lcms2, simdjson)
  get the dev triplet + prefix path passthrough.

Mango exceptions crossing PPR boundaries are translated (`import_failed`,
`invalid_argument`, and so on) by `engine.mesh`/`engine.image`; mango output
chatter is silenced once per-process by `engine.mesh` (Error and above stay
visible).
