# stb

Upstream: https://github.com/nothings/stb

Build route: CPM source, `GIT_TAG master` — a **moving branch, not an
immutable pin**. Resolved revision consumed by the local CPM cache when this
survey was written:

- commit: `2c980bb59875b0d32144a71867fbdebb2f77cd20` (2026-08-01)

`STB.cmake` only exposes the source dir as a SYSTEM include via an INTERFACE
IMPORTED `stb` target; no library targets are built.

## License

Dual-licensed — choose public domain or MIT. The preserved `LICENSE` carries
both alternates (Copyright (c) 2017 Sean Barrett). Keep a notice with
redistributed software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 2547 | 11dd149dbc50748419a22fcec275b5bcfb4c2d86d9fba8802e5c4ffd40c94609 |

## Integration scope

- Single consumer: `engine.image` mip generation (`Image.Mips.cpp` PRIVATE
  dep). `stb_image_resize2.h` is the only upstream header used, with
  `STB_IMAGE_RESIZE_IMPLEMENTATION` defined in that one TU; the float-linear
  workspace runs through `stbir_resize_float_linear` because Mango exposes no
  float resampler.
- The `:mips` interface exports engine vocabulary only (`MipGenDesc`,
  mip extents, generated chains); no stb type crosses the module boundary and
  no other module links `stb`.
- Separate from stb_truetype: the UI font atlas rasterizer bundled inside
  `imgui_draw.cpp` is ImGui's vendored copy at the pinned ImGui revision, and
  the opt-in FreeType swap for it is covered by
  [FreeType](../FreeType/README.md).
