# UI Font Pair and Dark Theme Implementation Plan

> **For agentic workers:** This plan is executed inline (native). Steps use checkbox (`- [ ]`) syntax.

**Goal:** Give the engine the Sour Gummy + Jersey 25 UI faces and the reference mockup's dark palette and geometry.

**Architecture:** Two contained edits. The font swap replaces the pinned `UIFontSource` list and the staged artifacts; the theme edit changes `theme::k*` values and the `ImGuiStyle` geometry in `applyThemeStyle()`. No loader, panel or composition code changes.

**Tech Stack:** C++23 modules, ImGui (pinned fork), CMake `clang-dev`.

**Spec:** `docs/superpowers/specs/2026-11-09-ui-font-pair-design.md`

## Global Constraints

- Artifacts are unmodified upstream static TTFs, pinned by exact byte count and SHA-256.
- Four base faces + one merged icon face, in order `body, emphasis, heading, tiny`.
- Sizes: `heading` 21, `body` 15, `emphasis` 15, `tiny` 13 (logical px).
- Palette and geometry values are the tables in the spec; token names do not change.
- No new tests and no automated harness. Existing tests that pin the font list, slot sizes or token-derived texels are updated because they would otherwise fail; nothing else is added.
- Visual acceptance is the owner's own review of a running build.

---

### Task 1: Swap the UI fonts

**Files:**
- Create: `assets/fonts/SourGummy-Medium.ttf`, `SourGummy-Bold.ttf`, `Jersey25-Regular.ttf`, `SourGummy-OFL.txt`, `Jersey25-OFL.txt`
- Delete: `assets/fonts/PixelifySans-Regular.ttf`, `PixelifySans-SemiBold.ttf`, `PixelifySans-OFL.txt`, `BigShouldersDisplay-Bold.ttf`, `BigShouldersDisplay-OFL.txt`
- Modify: `assets/fonts/PROVENANCE.md`
- Modify: `lib/engine/app/App.Application.Editor.cpp` (`font_sources`, ~425-460)
- Modify: `lib/engine/app/ui/App.UI.ImGui.cppm` (`EFont` comment only)
- Modify: `lib/engine/tests/app/App.ImGuiRouting.Tests.cpp` (`FontLoading::conceptSources`, slot sizes ~2809, DPI heading size ~2470)

- [ ] **Step 1: Fetch and verify the artifacts**

```bash
cd assets/fonts
S=b93db84aea88d697656eb829fdd8472fe5304325
J=220d0c5be4c2b369bd442fd28c162ba08455cfc6
curl -fsSL -o SourGummy-Medium.ttf "https://raw.githubusercontent.com/eifetx/Sour-Gummy-Fonts/$S/fonts/ttf/SourGummy-Medium.ttf"
curl -fsSL -o SourGummy-Bold.ttf   "https://raw.githubusercontent.com/eifetx/Sour-Gummy-Fonts/$S/fonts/ttf/SourGummy-Bold.ttf"
curl -fsSL -o Jersey25-Regular.ttf "https://raw.githubusercontent.com/google/fonts/$J/ofl/jersey25/Jersey25-Regular.ttf"
curl -fsSL -o SourGummy-OFL.txt    "https://raw.githubusercontent.com/eifetx/Sour-Gummy-Fonts/$S/OFL.txt"
curl -fsSL -o Jersey25-OFL.txt     "https://raw.githubusercontent.com/google/fonts/$J/ofl/jersey25/OFL.txt"
sha256sum SourGummy-Medium.ttf SourGummy-Bold.ttf Jersey25-Regular.ttf
```

Expected:

```
4fc77ca2182c2b0f21724f87d7b4343d56c009f13430f174f2820296444bd075  SourGummy-Medium.ttf  (71728 bytes)
62c9d98e95e1bb971e8d7a2d3a6d197575156d5ec69d2498ae8f2a7c925411b1  SourGummy-Bold.ttf    (72180 bytes)
4d4acdd89a748e9548041a3046d95536c500e1a7be9197c6aeef7d980f79d370  Jersey25-Regular.ttf  (142372 bytes)
```

- [ ] **Step 2: Update the production `font_sources` list**

Body `SourGummy-Medium.ttf` 71728 @ 15.0f; Font Awesome 7 (unchanged file/digest) @ 15.0f with `m_merge_into_previous`; emphasis `SourGummy-Bold.ttf` 72180 @ 15.0f; heading `Jersey25-Regular.ttf` 142372 @ 21.0f; tiny `SourGummy-Medium.ttf` 71728 @ 13.0f. Rewrite the leading comment to name the new pair, the four slots plus merge, and the fail-instead-of-fallback rule.

- [ ] **Step 3: Update the test's mirrored list and size assertions**

Same five entries in the same order in `FontLoading::conceptSources`. Then `body->LegacySize == 15.0f`, `Fonts[2]->LegacySize == 21.0f`, `Fonts[3]->LegacySize == 13.0f`, and the DPI test's `base.m_heading_size == 21.0f`. Rename the two comments that still say Pixelify / 12 px.

- [ ] **Step 4: Delete the replaced artifacts and record provenance**

`git rm` the five Pixelify/Big Shoulders files. In `PROVENANCE.md`, add the three new rows (pinned commit, URL, bytes, SHA-256) and the two licence notices, replace the "Concept typography" section with a one-line supersession note, and keep the Inter/JetBrains Mono fixture rows.

- [ ] **Step 5: Build**

Run: `cmake --build --preset clang-dev --target game engine.tests.app`
Expected: clean build, and `out/build/clang-dev/game/fonts/` holds the new files only.

- [ ] **Step 6: Commit**

```bash
git add -A assets/fonts lib/engine/app/App.Application.Editor.cpp \
        lib/engine/app/ui/App.UI.ImGui.cppm lib/engine/tests/app/App.ImGuiRouting.Tests.cpp
git commit -m "ui: use Sour Gummy and Jersey 25"
```

---

### Task 2: Adopt the reference palette and geometry

**Files:**
- Modify: `lib/engine/app/ui/App.UI.ImGui.cppm` (`theme` tokens, ~64-87)
- Modify: `lib/engine/app/ui/App.UI.ImGui.cpp` (`kContainerRounding`/`kControlRounding` ~104-105, `applyThemeStyle()` ~112-235)
- Modify: `lib/engine/tests/app/App.ImGuiRouting.Tests.cpp` (transfer texels ~1615-1628)

- [ ] **Step 1: Replace the token values**

`kCanvas #050A10`, `kPanel #0B1219`, `kRaised #121B24`, `kPaper #1A252F`, `kHover #23313E`, `kBorder #2B3946`, `kBorderStrong #40536A`, `kTextPrimary #F2E6D0`, `kTextSecondary #A2B1BF`, `kAccent #F0B44A`, `kAccentHover #F8C96F`, `kAccentActive #CE942F`, `kAccentText #171006`, `kSelection #E9C896`, `kSelectionText #F7E4C4`, `kSuccess #3FD07A`, `kWarning #F2A93B`, `kError #F2555A`. Leave `kFloatingPanelAlpha` at 0.86.

- [ ] **Step 2: Set the geometry**

`kContainerRounding = 8.0f`, `kControlRounding = 6.0f`;
`WindowBorderSize`/`ChildBorderSize`/`PopupBorderSize` = 1.0f;
`WindowPadding {10,9}`; `FramePadding {7,3}`; `ItemSpacing {6,6}`;
`CellPadding {6,3}`; `ScrollbarSize 8.0f`. Everything else in
`applyThemeStyle()` keeps its current value.

- [ ] **Step 3: Flip the base style and the title roles**

`StyleColorsLight()` → `StyleColorsDark()`; `TitleBg` = `kPanel`, `TitleBgActive` = `kRaised`, `TitleBgCollapsed` = `kCanvas`. Rewrite the amber title-bar contrast comment for cream-on-dark, and the table-row-alt comment for a dark surface.

- [ ] **Step 4: Update the token-derived texels in the transfer test**

```cpp
assertTexelNear(texel(20u, 16u), std::array{242u, 230u, 208u, 255u});  // kTextPrimary
assertTexelNear(texel(60u, 16u), std::array{11u, 18u, 25u, 255u});     // kPanel
if (format == rhi::Format::RGBA8UnormSrgb) {
    assertTexelNear(texel(100u, 16u), std::array{178u, 169u, 153u, 255u});
} else {
    assertTexelNear(texel(100u, 16u), std::array{126u, 124u, 116u, 255u});
}
```

The gradient-midpoint expectations are token-independent and stay.

- [ ] **Step 5: Build, format and commit**

```bash
python scripts/format_cpp.py
python scripts/check_semantic_style.py --files lib/engine/app/ui/App.UI.ImGui.cppm \
    lib/engine/app/ui/App.UI.ImGui.cpp lib/engine/tests/app/App.ImGuiRouting.Tests.cpp
cmake --build --preset clang-dev --target game engine.tests.app
git add lib/engine/app/ui/App.UI.ImGui.cppm lib/engine/app/ui/App.UI.ImGui.cpp \
        lib/engine/tests/app/App.ImGuiRouting.Tests.cpp
git commit -m "ui: dark mockup palette and geometry"
```

---

### Task 3: Hand over for visual review

- [ ] **Step 1: Run the game and confirm it starts**

Launch the staged executable, open the Colony panel and confirm panels, controls, icons and text render with no missing-glyph boxes.

- [ ] **Step 2: Report**

State the build result, list what changed, note that Windows was not exercised from this Linux host, and hand the running build to the owner for visual acceptance. No automated suite run is required for this change; the owner reviews visuals directly.
