# FreeType

Upstream: https://freetype.org/ (source: https://gitlab.freedesktop.org/freetype/freetype)

Pinned version: **FreeType 2.13.3** (port-version 0), consumed through the
vcpkg registry port `freetype`. The port resolves to registry git-tree
`8a2c633dcc14eaabdb31cf4637242f4e3c2f3fa2`, looked up from the repository
baseline `544a4c5c297e60e4ac4a5a1810df66748d908869` recorded in `vcpkg.json`
(baseline commit date 2025-12-16). The version pin therefore lives in the
existing baseline; no CPM download or moving tag is involved.

## Why 2.13.3

2.14.x/2.15.0 are not present at this baseline, so pinning them would require a
baseline bump. A baseline bump changes dependency resolution for **every**
preset, including the default configuration, and M4 (optional evaluation,
`docs/plans/imgui-ricing-plan.md` §M4) does not authorize that. 2.13.3 is also
the version the pinned ImGui FreeType wrapper was tested against upstream, and
the wrapper carries an explicit note about 2.11.0 bitmap/colored-font crashes
(not applicable to 2.13.3). If a later port is required, bump the baseline as
its own reviewed change and re-run the rasterizer evidence.

## License

The port declares `FTL OR GPL-2.0-or-later`; PPR uses the **FTL** (FreeType
Project License). `FTL.TXT` preserves the license file shipped with
2.13.3, unaltered:

| File | Bytes | SHA-256 |
|---|---:|---|
| FTL.TXT | 6743 | 08c135755dd589039470f1fdbb400daaabaaa50d0b366d19cebff4d22986baa1 |

The copy above was retrieved from
`https://raw.githubusercontent.com/freetype/freetype/VER-2-13-3/docs/FTL.TXT`
and its bytes/hash were re-verified on the date this file was added.
`FTL.TXT` itself carries the required credit text ("Portions of this software
are copyright c `<year>` The FreeType Project (www.freetype.org). All rights
reserved."). For 2.13.3 the copyright year range ends in 2024
(`include/freetype/freetype.h` at tag `VER-2-13-3`: "Copyright (C) 1996-2024"),
so the rendered credit is:

> Portions of this software are copyright © 2024 The FreeType Project
> (https://freetype.org). All rights reserved.

Binary redistribution additionally requires a disclaimer in the distribution
documentation stating that the software is based in part on the work of the
FreeType Team (FTL §2, second bullet). Any PPR build that ships a FreeType
binary must carry that disclaimer; no such build ships today.

## Integration scope

- Wired only when `PPR_IMGUI_FREETYPE=ON`: `cmake/external/FreeType.cmake`
  (included from `cmake/Dependencies.cmake`) plus the wrapper registration in
  `cmake/external/DearImGui.cmake`. The only preset that sets it is
  `msvc-dev-freetype`; no default or release preset links this dependency.
- The consumer is the vendored wrapper `misc/freetype/imgui_freetype.cpp` from
  the same pinned ImGui tree (`v1.93.0-docking`) already used by `imgui.base`.
- FreeType assumes **linear** blending upstream. PPR renders the UI in gamma
  space today, so adopting FreeType requires a separate blending/color-space
  decision that is explicitly out of scope here (see
  `.slim/deepwork/imgui-ricing/m4-freetype-evaluation.md`).
