# lib/engine/image/

## Responsibility

`engine.image` owns CPU-side image assets (decode to native format + plain layout; no GPU/RHI
types cross — RHI mapping happens at upload inside `engine.app` caches). P1 implements the frozen
`docs/plans/asset-pipeline.md` §2.2 contract: `:types` vocabulary + `:decode` PNG/JPG/KTX2/DDS
decode (P1a RGBA first, then P1b blocks) + `:mips` CPU chain generation (Phase 8 M1).

## Design

- Partitioned umbrella: `Image.cppm` re-exports `:types` + `:decode` + `:mips`.
- `:types` holds `image::errc` (+ category, `function_not_supported`/`invalid_argument` map to
  `std::errc`), `EBlockTag`, `ENativeImageFormat`, `EImageUsage`, `EImageDimension`, `ImageLimits`/
  `kDefaultImageLimits` (8K / 256 MiB production caps — over-limit inputs fail closed with
  `invalid_argument`), `ImageDecodeDesc` (`m_limits`, `m_use_simd`, `m_use_flip_v`),
  `ImageAsset` (frozen `SharedBuffer` storage +
  `ImageSubresource` views with positive pitches), block-geometry helpers (`isSrgb`/`isBlocked`/
  `blockTagOf`/`blockWidthOf`/`blockHeightOf`/`bytesPerBlockOf`/`rowPitchFor`/`slicePitchFor`),
  decode-invariant docs, and `contentHash`
  (`hash::contiguousRange` — never owner identity).
  Declare error-category instances `static const` — `std::error_category` is polymorphic/non-literal
  (virtuals + `std::string`); `Image.Types.cpp` keeps its instance function-local
  (`static const ImageErrorCategory g_image_error_category{}` inside `error_category()`), the same
  non-literal pattern fixed in `Shader.cpp`/`RHI.cpp` — use `static const` for consistency/portability.
- `:decode` declares `decodeToRgba8` (any supported source → RGBA8)/`decodeToBlocks`
  (KTX2/DDS passthrough/transcode only — PNG/JPG are never recompressed, callers fall back to RGBA8);
  `Image.Decode.cpp` implements them on a per-job Mango decoder +
  `UniqueBuffer` (materialize → `moveToShared` freeze, no shared lock; decoders are never shared
  across jobs), sRGB from `!header.linear` (`EImageUsage::data` forces linear),
  `options.simd = desc.m_use_simd` with `options.multithread` hard `false` (engine policy — no Mango
  pool in the RT path, not caller-selectable), Y-flip as a positive-pitch CPU blit when `m_use_flip_v`, production
  `m_limits` checks bounding input/header/decoded bytes before allocation, and `checkInvariants_`
  (`PPR_ASSERT` + `invalid_argument` on violation) at return.
- `:mips` declares `mipCountFor`/`mipExtentAt`/`generateMipChain` (`MipGenDesc`: incremental
  default, HQ-from-top opt-in, coverage preserve, alpha cutoff, limits); `Image.Mips.cpp`
  implements them in a float-linear workspace via `stbir_resize_float_linear` (vendored `stb`
  target, private — Mango has no float resample), sRGB to linear before the filter with
  re-encode after, chamfer distance-field color expansion before each level, upscale-only
  binary-search coverage restore (20 steps, abort-below-1). Blocked assets reject with
  `function_not_supported` so KTX2-embedded chains pass through untouched; the chain lands in
  `m_subresources[m_mip_count]` (one frozen storage, tight pitches) with chain bytes counted
  against the decode cap.

## Flow

Bytes in → `ImageAsset` out: caller `SharedBuffer::mapFile`s (missing file surfaces
`no_such_file_or_directory` there, not in the byte decoders) → per-job decode → frozen asset
shares by value across threads; upload borrows `view.data()` while the asset (or cache's strong
hold) lives. Caches/consumers live in `engine.app`.

## Integration

- Depends on: `engine.core` + `engine.math` (public), `mango-image` + `stb` (private).
- Consumed by: `engine.app` pass caches (upload consumes all mips, per-level pitch
  validated) + scene ownership; `engine.tests.asset` (image decode/format/block/hash/error
  tests plus `mips_chain`/`mips_coverage`/`mips_bleed`/`mips_quality` goldens with
  runtime-generated fixtures).
- Build: `Image.cppm`, `Image.Types.cppm`, `Image.Decode.cppm`, `Image.Mips.cppm` in
  `FILE_SET CXX_MODULES`; `Image.Types.cpp` + `Image.Decode.cpp` + `Image.Mips.cpp` as PRIVATE
  sources; `setup_ppr_project(engine.image INTERNAL_PUBLIC_DEPS engine.core engine.math
  EXTERNAL_SYSTEM_PRIVATE_DEPS mango-image stb)`.

## Key Files

- `Image.cppm` — umbrella re-export only.
- `Image.Types.cppm` — `:types` frozen vocabulary + helpers.
- `Image.Types.cpp` — `image::errc` category implementation.
- `Image.Decode.cppm` — `:decode` declarations (no Mango in the interface).
- `Image.Decode.cpp` — Mango-backed implementation (private dep, never exported).
- `Image.Mips.cppm` — `:mips` declarations (`mipCountFor`/`mipExtentAt`/`generateMipChain`, no
  stb types in the interface).
- `Image.Mips.cpp` — stb-backed implementation (private dep, never exported).
- `CMakeLists.txt` — `engine.image` target registration (see Integration).
