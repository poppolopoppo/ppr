# UI font pair — Sour Gummy + Jersey 25

Date: 2026-11-09
Status: proposed (awaiting approval)

## Intent

The current UI pair — Pixelify Sans 16/12 px plus Big Shoulders Display 22 px —
fails the first job of UI type: it cannot be read at the size the engine bakes
it. Pixelify's rounded faux-pixel strokes sit off the pixel grid at 16 px, and
the 12 px Inspector read-out is the worst offender. Nine comparison rounds
established two faces the owner wants to keep:

- a **pixel face for signage** (titles, logo, section rules), and
- a **soft, rounded, slightly irregular face for everything that is read**
  (labels, rows, buttons, telemetry).

## Decision

| Role | `ui::theme::EFont` | File | Size (logical px) |
|---|---|---|---|
| Titles, section headings, logo | `heading` | `Jersey25-Regular.ttf` (Jersey 25, 400) | 28 |
| Body text, buttons, row labels | `body` | `SourGummy-Medium.ttf` (500) + merged FA7 | 20 |
| Emphasis (selected row, active label) | `emphasis` | `SourGummy-Bold.ttf` (700) | 20 |
| Compact read-out, table keys, micro-caps | `tiny` | `SourGummy-Medium.ttf` (500) | 16 |

Font Awesome 7 stays merged into the body face only, so icon-bearing text never
lands on the pixel face. Slot count is unchanged (four base faces + one merge),
which keeps `ui::theme::font`'s `Fonts.Size == 4` contract intact.

## Goals

- Text is legible at the baked size on a 1× framebuffer; the Inspector read-out
  stops being the weak point.
- The pixel/retro identity survives, but only in display type, where coarse
  pixels are an asset rather than a legibility cost.
- Every artifact is an unmodified upstream static TTF, pinned by byte count and
  SHA-256, exactly like the current set.
- Layout stays within the existing 1280×720 default window at 100% scaling.

## Non-goals

- No live font swapping, no font picker, no runtime atlas reload beyond the
  existing DPI density path.
- No new theme tokens and no panel re-layout: the palette changes values and
  the light/dark base style, not the token vocabulary or the composition.
- No variable-font support in the loader. If a future face is variable-only, it
  needs its own decision; this pair avoids that entirely.

## Anti-goals

- Shipping a face the loader cannot bake at the requested weight.
- Silently falling back to the stock ImGui face on a missing or altered file.
- Keeping two font sets staged so the runtime choice becomes ambiguous.

## Constraints discovered

- `UIFontSource` pins `m_path`, `m_expected_bytes`, `m_size_pixels` and
  `m_sha256`; a mismatch fails UI initialization by design
  (`App.Application.Editor.cpp`, `App.Service.UI.cppm`).
- `theme::font` only resolves roles when exactly four base faces were added;
  the icon merge must remain a merge, not a fifth face.
- Sour Gummy **ships static TTFs upstream** (`fonts/ttf/SourGummy-Medium.ttf`,
  `SourGummy-Bold.ttf`), so the "static upstream artifact, no
  conversion/subsetting" rule in `assets/fonts/PROVENANCE.md` still holds — no
  `varLib.instancer` step is needed. Jersey 25 is likewise static.
- Neither upstream declares a Reserved Font Name in its copyright line.
- `assets/fonts` is staged beside the executable by `game/CMakeLists.txt` and
  `ppr_stage_assets`; `StageAssets.cmake` already admits the `fonts` leaf.
- `App.ImGuiRouting.Tests.cpp` re-declares the production font list
  (`FontLoading::conceptSources`) and asserts the four slot order and
  `LegacySize` values, so the test must move with the runtime list.

## Approaches considered

1. **Sour Gummy body + Jersey 25 display** (chosen). One rounded face with three
   weights covers body, emphasis and read-out; one pixel face covers signage.
   Smallest artifact set, no instancing, strongest separation between "read me"
   and "signage" text.
2. **Sour Gummy for everything, Jersey 25 nowhere.** Cohesive but drops the
   retro identity the owner explicitly kept in every round.
3. **Tiny5 "puff preset" + Sour Gummy.** A pixelated DynaPuff; rejected because
   a 5 px grid upscaled 4× is the blockiness that read as "too low resolution".
4. **Variable fonts + `varLib.instancer` instances.** Would work, but adds a
   generated-artifact provenance path for no visual gain; the upstream statics
   make it unnecessary.

## Size decision

The rounds were judged at display 28 / body 20 / caps 15 / read-out 16, up from
22 / 16 / 12 / 12. Larger type is the second half of the legibility fix, not a
cosmetic preference. If the 1280×720 layout feels cramped once built, the
fallback is 26 / 18 / 15, which is a one-line change per slot and no artifact
change.

## Theme palette — dark control room

The mockup (`docs/PPR.png`) is a dark HUD, not the cream "site office" the code
currently implements. Sampled from its UI regions:

| Anchor | Mockup sample |
|---|---|
| Panel body | `#081018` |
| Raised row / lighter surface | `#101820` |
| Primary text (cream) | `#F8E0C0` |
| Positive / check | `#40E880` |
| Negative / alert | `#FF4B4B` |
| Warm highlight, selected tile | `#F8D0A0` – `#F8E0C0` |

The token set and its semantics stay exactly as they are — only the values move
and the ImGui base style flips from light to dark. Chosen values and measured
contrast (WCAG ratio, computed against the panel body unless stated):

| Token | New value | Role | Contrast |
|---|---|---|---|
| `kCanvas` | `#050A10` | app background behind panels | — |
| `kPanel` | `#0B1219` | panel body | — |
| `kRaised` | `#121B24` | raised card, input, menu bar | — |
| `kPaper` | `#1A252F` | highest surface, popup | — |
| `kHover` | `#23313E` | hover wash, table header | — |
| `kBorder` | `#2B3946` | separators, scrollbar, tree lines | 1.59:1 (non-text seam) |
| `kBorderStrong` | `#40536A` | window/frame border, strong seam | 2.4:1 (non-text seam) |
| `kTextPrimary` | `#F2E6D0` | body text, title text | **15.25:1** |
| `kTextSecondary` | `#A2B1BF` | disabled, captions, plots | **8.59:1** |
| `kAccent` | `#F0B44A` | icons, active tool, separators | **10.17:1** |
| `kAccentHover` | `#F8C96F` | accent hover | — |
| `kAccentActive` | `#CE942F` | accent pressed | — |
| `kAccentText` | `#171006` | ink on accent | **10.18:1** on accent, **7.10:1** on pressed |
| `kSelection` | `#E9C896` | selection wash, table row tint | — |
| `kSelectionText` | `#F7E4C4` | check mark, slider grab, links, nav cursor | **15.11:1** |
| `kSuccess` | `#3FD07A` | positive status, colony marker | **9.43:1** |
| `kWarning` | `#F2A93B` | warning status, amber markers | **9.43:1** |
| `kError` | `#F2555A` | error status, destructive action | **5.58:1** |

Structural consequences, not just values:

- `applyThemeStyle()` starts from `StyleColorsDark()` instead of
  `StyleColorsLight()`, so any role this change does not list is dark by
  default rather than light.
- Panel titles stop being amber bands with dark ink. `TitleBg` returns to the
  panel surface, `TitleBgActive` to `kRaised`, and titles read as cream text —
  matching the mockup, where headers are dark with light labels and a thin
  seam. The existing contrast-rationale comment must be rewritten for the
  cream-on-dark case.
- `ImGuiCol_Border` moves from `kBorderStrong` (was near-black ink on cream) to
  the new `kBorderStrong`, which is *lighter* than the surface on dark; the
  same substitution applies to `TableBorderStrong`, `SeparatorHovered`,
  `ScrollbarGrabHovered` and `ResizeGripHovered`.
- `kFloatingPanelAlpha` stays 0.86; the mockup's floating panels are similarly
  translucent over the scene.
- `ImGuiCol_TableRowBgAlt` keeps using `kHover` at low alpha, which now reads as
  a subtle lightening rather than a darkening; the comment that explains why it
  stopped using `kRaised` must be restated for a dark surface.

## Where the change lands

- `assets/fonts/`: add `SourGummy-Medium.ttf`, `SourGummy-Bold.ttf`,
  `Jersey25-Regular.ttf`, `SourGummy-OFL.txt`, `Jersey25-OFL.txt`; retire
  `PixelifySans-*` and `BigShouldersDisplay-*`.
- `assets/fonts/PROVENANCE.md`: new pinned rows (URL, bytes, SHA-256), licence
  notices, and a replacement of the "Concept typography" section.
- `lib/engine/app/App.Application.Editor.cpp`: the `font_sources` list (paths,
  bytes, sizes, digests, unit comments).
- `lib/engine/app/ui/App.UI.ImGui.cppm`: the `EFont` role comment and the
  `theme` token values (the palette above).
- `lib/engine/app/ui/App.UI.ImGui.cpp`: `applyThemeStyle()` base style, title
  roles, border roles and the contrast comments.
- `lib/engine/tests/app/App.ImGuiRouting.Tests.cpp`: `FontLoading::conceptSources`
  and the slot/size assertions (`16`/`22`/`12` → `20`/`28`/`16`); the UI
  transfer test's expected texels, which are derived from `kTextPrimary` and
  `kPanel` and therefore change with the palette.

## Verification

- `python scripts/format_cpp.py` and `scripts/check_semantic_style.py` on the
  touched C++.
- Configure + build the app and test targets; run the ImGui routing test suite
  (it exercises the pinned-byte, missing-file, tampered-digest, merge, slot
  order and transfer-texel cases).
- Launch the game headless/interactive on this host and confirm the atlas bakes
  and panels render; capture a screenshot of the Colony panel and compare it
  against `docs/PPR.png` for surface, text and accent values.
- Re-check the contrast table above against the shipped values; the table is
  the acceptance criterion for the palette, not a suggestion.
- Windows remains unverified from this Linux host; the change is
  platform-neutral (asset files plus the same loader path), but that must be
  stated rather than assumed.

## Left to the builder

- Exact SHA-256 values and byte counts are pinned from the fetched artifacts.
- Whether to delete the retired Pixelify/Big Shoulders binaries or keep them as
  inactive fixtures; the design removes them from the runtime list either way.
- The `docs/font-candidates*.png` comparison sheets are untracked exploration
  artifacts; the owner decides whether any are kept.
