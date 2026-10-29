# stb

Upstream: https://github.com/nothings/stb

Build route: CPM source pinned to `2b2e64bc4778df6a06fbea3a5dbe61f7a76ecf19`,
before upstream moved `stb_image_resize2.h` into `deprecated/`.

`PatchStbResize.cmake` verifies the resize header hash and generates a private
copy whose scalar coefficient-packing operations use constant-sized `memmove`.
The original integer-pointer copies violated float alignment and aliasing rules;
packing can also overlap. SIMD paths and sanitizer instrumentation remain intact.
The INTERFACE IMPORTED `stb` target exposes that copy before the original SYSTEM
include directory; no library target is built and CPM sources are not modified.

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
