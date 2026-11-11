# UI Font Pair Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the engine's UI font set with Sour Gummy (body/emphasis/read-out) and Jersey 25 (titles), pinned as unmodified upstream static TTFs at larger sizes.

**Architecture:** The UI service already takes a pinned `std::span<const UIFontSource>` and bakes four base faces plus one merged icon face. This change swaps the artifacts, the production source list and the test's mirrored list; no loader code changes.

**Tech Stack:** C++23 modules, ImGui (pinned fork), CMake presets, `PPR_UNIT_TEST` suite, SHA-256-pinned staged assets.

**Spec:** `docs/superpowers/specs/2026-11-09-ui-font-pair-design.md`

## Global Constraints

- Artifacts are unmodified upstream static TTFs. No instancing, no subsetting, no OTF conversion.
- Every `UIFontSource` pins exact `m_expected_bytes` and `m_sha256`; a mismatch must fail UI initialization, never fall back.
- Base face order is `body, emphasis, heading, tiny` after the icon merge is dropped; `ui::theme::font` requires exactly four base faces.
- Sizes (logical px): `heading` 28, `body` 20, `emphasis` 20, `tiny` 16.
- Font Awesome 7 merges into the body face only.
- Palette values are the table in the spec; the token names and their meanings do not change, and `applyThemeStyle()` starts from `StyleColorsDark()`.
- No panel re-layout and no token additions.
- Formatting/style gates: `python scripts/format_cpp.py`, `python scripts/check_semantic_style.py --files <touched>`.

## Review Focus

- **Missing staged font file** — UI initialization must return the I/O error and leave no context, not substitute the stock face.
- **Altered font bytes** — the digest gate must reject even when size, path and name still match.
- **Icon codepoints after the swap** — every `ui::icons::kMergeRanges` codepoint must still bake from source index 1 on the body face.
- **DPI transitions at 1.25/1.5/2.0** — heading/body/emphasis sizes must scale and round as the existing test expects.
- **Long body text at 20 px** — wrapped labels and the Inspector table must still fit the Colony panel at the new size (visual check; no automated coverage).
- **Dark-surface assumptions** — markers, dividers, tooltips and the semi-transparent floating panels must stay readable now that every surface is dark (visual check).

---

### Task 1: Pin the new font artifacts

**Files:**
- Create: `assets/fonts/SourGummy-Medium.ttf`
- Create: `assets/fonts/SourGummy-Bold.ttf`
- Create: `assets/fonts/Jersey25-Regular.ttf`
- Create: `assets/fonts/SourGummy-OFL.txt`
- Create: `assets/fonts/Jersey25-OFL.txt`
- Modify: `assets/fonts/PROVENANCE.md`

**Interfaces:**
- Consumes: nothing.
- Produces: three pinned binaries with the byte counts and digests Tasks 2–3 reference.

- [ ] **Step 1: Fetch the pinned artifacts**

```bash
cd assets/fonts
S=b93db84aea88d697656eb829fdd8472fe5304325
J=220d0c5be4c2b369bd442fd28c162ba08455cfc6
curl -fsSL -o SourGummy-Medium.ttf "https://raw.githubusercontent.com/eifetx/Sour-Gummy-Fonts/$S/fonts/ttf/SourGummy-Medium.ttf"
curl -fsSL -o SourGummy-Bold.ttf   "https://raw.githubusercontent.com/eifetx/Sour-Gummy-Fonts/$S/fonts/ttf/SourGummy-Bold.ttf"
curl -fsSL -o Jersey25-Regular.ttf "https://raw.githubusercontent.com/google/fonts/$J/ofl/jersey25/Jersey25-Regular.ttf"
curl -fsSL -o SourGummy-OFL.txt    "https://raw.githubusercontent.com/eifetx/Sour-Gummy-Fonts/$S/OFL.txt"
curl -fsSL -o Jersey25-OFL.txt     "https://raw.githubusercontent.com/google/fonts/$J/ofl/jersey25/OFL.txt"
```

- [ ] **Step 2: Verify the pins**

Run: `sha256sum SourGummy-Medium.ttf SourGummy-Bold.ttf Jersey25-Regular.ttf && stat -c '%n %s' SourGummy-Medium.ttf SourGummy-Bold.ttf Jersey25-Regular.ttf`

Expected, exactly:

```
4fc77ca2182c2b0f21724f87d7b4343d56c009f13430f174f2820296444bd075  SourGummy-Medium.ttf
62c9d98e95e1bb971e8d7a2d3a6d197575156d5ec69d2498ae8f2a7c925411b1  SourGummy-Bold.ttf
4d4acdd89a748e9548041a3046d95536c500e1a7be9197c6aeef7d980f79d370  Jersey25-Regular.ttf
SourGummy-Medium.ttf 71728
SourGummy-Bold.ttf 72180
Jersey25-Regular.ttf 142372
```

- [ ] **Step 3: Record the new artifacts in `PROVENANCE.md`**

Add a `## UI font pair — selected 2026-11-09` section whose table has one row per binary with: file, pinned upstream commit + path, bytes, SHA-256. Add the two OFL notices to the license table. State that all three are static `glyf` TTFs with no `fvar`, that neither upstream declares a Reserved Font Name, and that no conversion or subsetting was performed.

- [ ] **Step 4: Verify staging picks them up**

Run: `cmake --build --preset clang-dev --target engine.tests.app.assets`
Then: `ls out/build/clang-dev/lib/engine/tests/app/fonts/`
Expected: the three new `.ttf` files and both new `-OFL.txt` files are present beside the executable tree.

- [ ] **Step 5: Commit**

```bash
git add assets/fonts/SourGummy-Medium.ttf assets/fonts/SourGummy-Bold.ttf \
        assets/fonts/Jersey25-Regular.ttf assets/fonts/SourGummy-OFL.txt \
        assets/fonts/Jersey25-OFL.txt assets/fonts/PROVENANCE.md
git commit -m "assets: pin Sour Gummy and Jersey 25 UI faces"
```

---

### Task 2: Mirror the new pin in the app font tests

**Files:**
- Modify: `lib/engine/tests/app/App.ImGuiRouting.Tests.cpp` (`FontLoading::conceptSources`, ~1694-1722; slot-size assertions ~2801-2811; DPI heading assertion ~2470)

**Interfaces:**
- Consumes: the three files and digests from Task 1.
- Produces: `FontLoading::conceptSources(font_root) -> std::array<UIFontSource, 5u>` in production order.

- [ ] **Step 1: Replace the `conceptSources` body**

Order and values: body `SourGummy-Medium.ttf` 71728 @ 20.0f, merge `Font Awesome 7 Free-Solid-900.otf` 414704 @ 20.0f (unchanged digest), emphasis `SourGummy-Bold.ttf` 72180 @ 20.0f, heading `Jersey25-Regular.ttf` 142372 @ 28.0f, tiny `SourGummy-Medium.ttf` 71728 @ 16.0f. Digests are the three from Task 1 Step 2 plus the existing FA digest.

- [ ] **Step 2: Update the slot and size assertions**

```cpp
PPR_TEST_ASSERT(body->LegacySize == 20.0f and io.Fonts->Fonts[2]->LegacySize == 28.0f);
PPR_TEST_ASSERT(io.Fonts->Fonts[3]->LegacySize == 16.0f);
```

and in the DPI test:

```cpp
PPR_TEST_ASSERT(base.m_heading_size == 28.0f and base.m_emphasis_size == base.m_font_size);
```

Update the two comments that name Pixelify (the merge target) and the "12 px" compact binding to say Sour Gummy and 16 px.

- [ ] **Step 3: Run the focused shard**

Run: `ctest --test-dir out/build/clang-dev -R engine.tests.app.services_window --output-on-failure`
Expected: PASS (the shard includes `app/imgui_routing`).

- [ ] **Step 4: Commit**

```bash
git add lib/engine/tests/app/App.ImGuiRouting.Tests.cpp
git commit -m "tests: pin the Sour Gummy / Jersey 25 font set"
```

---

### Task 3: Point the production UI at the new faces

**Files:**
- Modify: `lib/engine/app/App.Application.Editor.cpp` (the `font_sources` array at ~425-460 and the comment above it)
- Modify: `lib/engine/app/ui/App.UI.ImGui.cppm` (the `EFont` role comment, ~47-51)

**Interfaces:**
- Consumes: Task 1 artifacts.
- Produces: the runtime list the Editor passes to `IUIService::initialize`.

- [ ] **Step 1: Replace the `font_sources` entries**

Keep the array shape (five entries, merge second) and replace the four base entries with Sour Gummy Medium @ 20, Sour Gummy Bold @ 20, Jersey 25 @ 28 and Sour Gummy Medium @ 16, using the Task 1 byte counts and digests. Leave the Font Awesome entry and `ui::icons::kMergeRanges` untouched, but raise its `m_size_pixels` to 20.0f so merged glyphs bake at the body size.

- [ ] **Step 2: Rewrite the leading comment**

It must state: Sour Gummy carries body/emphasis/read-out, Jersey 25 carries headings, Font Awesome merges into the body only, four slots plus one merge, and that a missing or altered file fails initialization instead of substituting a default.

- [ ] **Step 3: Update the `EFont` comment in `App.UI.ImGui.cppm`**

Name the actual faces and sizes, and keep the note that `tiny` binds the body family again at a smaller size with no icon merge.

- [ ] **Step 4: Build**

Run: `cmake --build --preset clang-dev --target game engine.tests.app`
Expected: clean build; the `game.assets` staging step reports the new font tree.

- [ ] **Step 5: Commit**

```bash
git add lib/engine/app/App.Application.Editor.cpp lib/engine/app/ui/App.UI.ImGui.cppm
git commit -m "app: load Sour Gummy and Jersey 25 for the UI"
```

---

### Task 4: Retire the replaced artifacts

**Files:**
- Delete: `assets/fonts/PixelifySans-Regular.ttf`, `PixelifySans-SemiBold.ttf`, `PixelifySans-OFL.txt`, `BigShouldersDisplay-Bold.ttf`, `BigShouldersDisplay-OFL.txt`
- Modify: `assets/fonts/PROVENANCE.md`

**Interfaces:**
- Consumes: Tasks 2–3 (nothing references the retired files any more).
- Produces: a font tree with exactly one active set plus the Inter/JetBrains Mono fixtures.

- [ ] **Step 1: Confirm nothing references the retired names**

Run: `grep -rn "Pixelify\|BigShoulders" --include="*.cpp" --include="*.cppm" --include="*.cmake" --include="CMakeLists.txt" .`
Expected: no matches under `lib/` or `game/` (only `PROVENANCE.md` and staged build outputs may still mention them).

- [ ] **Step 2: Delete the files and rewrite the superseded provenance section**

Keep the Inter/JetBrains Mono rows as legacy test fixtures; replace the "Concept typography" paragraph with a short note that Pixelify Sans and Big Shoulders Display were superseded on 2026-11-09 by the UI font pair.

- [ ] **Step 3: Re-stage and confirm the stale files are pruned**

Run: `cmake --build --preset clang-dev --target game.assets`
Then: `ls out/build/clang-dev/game/fonts/`
Expected: no `PixelifySans-*` or `BigShouldersDisplay-*` entries.

- [ ] **Step 4: Commit**

```bash
git rm assets/fonts/PixelifySans-Regular.ttf assets/fonts/PixelifySans-SemiBold.ttf \
       assets/fonts/PixelifySans-OFL.txt assets/fonts/BigShouldersDisplay-Bold.ttf \
       assets/fonts/BigShouldersDisplay-OFL.txt
git add assets/fonts/PROVENANCE.md
git commit -m "assets: retire the replaced Pixelify and Big Shoulders faces"
```

---

### Task 5: Recolour the ImGui theme to the mockup

**Files:**
- Modify: `lib/engine/app/ui/App.UI.ImGui.cppm` (the `theme` token block, ~64-87)
- Modify: `lib/engine/app/ui/App.UI.ImGui.cpp` (`applyThemeStyle()`, ~112-235)
- Modify: `lib/engine/tests/app/App.ImGuiRouting.Tests.cpp` (UI transfer texels, ~1615-1628)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: the same `theme::k*` symbols with new values; no signature changes.

- [ ] **Step 1: Replace the token values**

Exactly these, in token order: `kCanvas #050A10`, `kPanel #0B1219`, `kRaised #121B24`,
`kPaper #1A252F`, `kHover #23313E`, `kBorder #2B3946`, `kBorderStrong #40536A`,
`kTextPrimary #F2E6D0`, `kTextSecondary #A2B1BF`, `kAccent #F0B44A`,
`kAccentHover #F8C96F`, `kAccentActive #CE942F`, `kAccentText #171006`,
`kSelection #E9C896`, `kSelectionText #F7E4C4`, `kSuccess #3FD07A`,
`kWarning #F2A93B`, `kError #F2555A`. Leave `kFloatingPanelAlpha` at 0.86.

- [ ] **Step 2: Flip the base style and the structural roles**

In `applyThemeStyle()`, replace `ImGui::StyleColorsLight()` with
`ImGui::StyleColorsDark()`. Set `TitleBg` to `kPanel`, `TitleBgActive` to
`kRaised`, `TitleBgCollapsed` to `kCanvas`. Keep `ImGuiCol_Text` on
`kTextPrimary`; the title now reads as cream on a dark bar.

- [ ] **Step 3: Restate the two comments that now contradict the code**

The title-bar comment still cites ink-on-amber ratios, and the table-row-alt
comment explains why it avoids `kRaised` on a cream window. Replace both with
the dark-surface rationale from the spec's palette section.

- [ ] **Step 4: Update the transfer test's token-derived texels**

```cpp
assertTexelNear(texel(20u, 16u), std::array{242u, 230u, 208u, 255u});  // kTextPrimary
assertTexelNear(texel(60u, 16u), std::array{11u, 18u, 25u, 255u});     // kPanel
```

and the composited block, which is the two tokens averaged:

```cpp
if (format == rhi::Format::RGBA8UnormSrgb) {
    assertTexelNear(texel(100u, 16u), std::array{178u, 169u, 153u, 255u});
} else {
    assertTexelNear(texel(100u, 16u), std::array{126u, 124u, 116u, 255u});
}
```

The gradient-midpoint expectations (`189`/`129`) are token-independent and stay.

- [ ] **Step 5: Run the focused shard**

Run: `ctest --test-dir out/build/clang-dev -R engine.tests.app.services_window --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add lib/engine/app/ui/App.UI.ImGui.cppm lib/engine/app/ui/App.UI.ImGui.cpp \
        lib/engine/tests/app/App.ImGuiRouting.Tests.cpp
git commit -m "ui: adopt the mockup's dark palette"
```

---

### Task 6: Verify the change end to end

**Files:**
- Modify (only if a gate fails): the files from Tasks 1–4.

**Interfaces:**
- Consumes: all previous tasks.
- Produces: passing evidence recorded in the final report.

- [ ] **Step 1: Format and style**

Run: `python scripts/format_cpp.py`
Then: `python scripts/check_semantic_style.py --files lib/engine/app/App.Application.Editor.cpp lib/engine/app/ui/App.UI.ImGui.cppm lib/engine/tests/app/App.ImGuiRouting.Tests.cpp`
Expected: both exit 0.

- [ ] **Step 2: Build and run the affected tests**

Run: `cmake --build --preset clang-dev --target game engine.tests.app`
Then: `ctest --test-dir out/build/clang-dev -R "engine.tests.app.services_window|engine.tests.app.services_cpu" --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Launch and capture**

Run the game from the staged tree, open the Colony panel, and capture a screenshot showing title (`Jersey 25`), rows and buttons (`Sour Gummy`), and the Inspector read-out. Confirm no missing-glyph boxes on `Énergie`, `Santé`, `·`, or any icon label.

- [ ] **Step 4: Check the palette against the mockup and the spec table**

Compare the capture with `docs/PPR.png`: panel surfaces read as the same dark blue-black, text as warm cream, highlights as amber, checks as green, alerts as red. Recompute the spec's contrast ratios from the shipped values and confirm nothing dropped below the recorded numbers.

- [ ] **Step 5: Run the routine suite**

Run: `ctest --test-dir out/build/clang-dev --parallel --output-on-failure -LE gpu_validation`
Expected: PASS. Record any pre-existing failure separately rather than attributing it to this change.

- [ ] **Step 6: Commit any fixes, then report**

State explicitly that Windows was not exercised from this Linux host and that the change is platform-neutral (asset files plus the existing loader path).
