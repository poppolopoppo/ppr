# lib/engine/image/

## Responsibility

`engine.image` owns CPU-side image assets (decode to native format + plain layout; no GPU/RHI
types cross — RHI mapping happens at upload inside `engine.app` caches). P1 implements the frozen
`docs/plans/asset-pipeline.md` §2.2 contract: `:types` vocabulary + `:decode` PNG/JPG/KTX2/DDS
decode (P1a RGBA first, then P1b blocks).

## Design

- Partitioned umbrella: `Image.cppm` re-exports `:types` + `:decode` only.
- `:types` holds `image::errc` (+ category, `function_not_supported`/`invalid_argument` map to
  `std::errc`), `BlockTag`, `NativeImageFormat`, `ImageUsage`, `ImageDimension`, `ImageDecodeDesc`
  (multithread=false policy, flip_v=false), `ImageAsset` (frozen `SharedBuffer` storage +
  `ImageSubresource` views with positive pitches), block-geometry helpers, and `contentHash`
  (`hash::contiguousRange` — never owner identity). P0 Gate-1 `GpuU32TestTag` asserts stay put.
- `:decode` declares `decodeToRgba8`/`decodeToBlocks` (passthrough/transcode only — PNG/JPG are
  never recompressed); `Image.Decode.cpp` implements them on a per-job Mango `ImageDecoder` +
  `UniqueBuffer` (materialize → checked view → `moveToShared` freeze, blob cloned immediately
  while the decoder lives — no shared lock; decoders are never shared across jobs), sRGB from
  `!header.linear` (data usage forces linear).

## Flow

Bytes in → `ImageAsset` out: caller `SharedBuffer::mapFile`s (missing file surfaces
`no_such_file_or_directory` there, not in the byte decoders) → per-job decode → frozen asset
shares by value across threads; upload borrows `view.data()` while the asset (or cache's strong
hold) lives. Caches/consumers live in `engine.app`.

## Integration

- Depends on: `engine.core` + `engine.math` (public), `mango-image` (private).
- Consumed by: (future) `engine.app` caches; `engine.tests.asset` (image decode/format/block/
  hash/error tests with runtime-generated fixtures).
- Build: `Image.cppm`, `Image.Types.cppm`, `Image.Decode.cppm` in `FILE_SET CXX_MODULES`;
  `Image.Types.cpp` + `Image.Decode.cpp` as PRIVATE sources;
  `setup_ppr_project(engine.image INTERNAL_PUBLIC_DEPS engine.core engine.math
  EXTERNAL_SYSTEM_PRIVATE_DEPS mango-image)`.

## Key Files

- `Image.cppm` — umbrella re-export only.
- `Image.Types.cppm` — `:types` frozen vocabulary + helpers.
- `Image.Types.cpp` — `image::errc` category implementation.
- `Image.Decode.cppm` — `:decode` declarations (no Mango in the interface).
- `Image.Decode.cpp` — Mango-backed implementation (private dep, never exported).
- `CMakeLists.txt` — `engine.image` target registration (see Integration).
