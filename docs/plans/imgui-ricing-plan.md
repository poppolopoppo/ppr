# PPR ImGui visual refinement plan

> Revised 2026-10-15 against current PPR source and Dear ImGui
> `v1.92.9b-docking`. Proposed implementation plan; no milestone is claimed
> shipped. This review changes documentation only.

The local [execution plan](../../.slim/deepwork/imgui-ricing/execution-plan.md)
breaks M0–M3 into bounded work packets; [progress](../../.slim/deepwork/imgui-ricing/progress.md)
separates source-verified facts, historical reports and missing evidence. Those
Git-ignored records supplement this durable plan. M0 is partial: assets and
staging registration exist, but deployment, presentation-format, complete capture
coverage and cost evidence remain open. No M1–M3 runtime behavior is claimed.

## 1. Outcome and scope

Make the existing UI feel deliberately designed: readable typography, consistent
spacing and hierarchy, clear interaction states, useful icons, and restrained
depth. Preserve input behavior, client ownership, and detached rendering.

**Recommended first delivery: M0–M3.** Theme, fonts, icons, and panel composition
do not require a dynamic-texture backend. Keep the existing static RGBA atlas
for this delivery. FreeType, shadows, and dynamic fonts have separate gates.

Scope is the current application UI: display controls, performance readouts, and
representative colony/world-generation panels. Reorganizing presentation is in
scope; adding editor features, a new docking workspace, native platform viewports,
localization infrastructure, or a general widget framework is not. Game-specific
presentation stays in `game`; reusable widgets and UI rendering stay in
`engine.app`. `Renderer` remains content-free.

## 2. Review findings and verified baseline

| Original assumption | Correction and consequence |
|---|---|
| Dynamic textures unlock all visual work | Static fonts, merged icons, styles, and ordinary draw-list decoration can use the existing backend. Separate the texture migration. |
| A frozen font ID is sufficient for dynamic atlases | Growth/replacement may involve distinct texture generations. Old detached geometry must retain the texture and UV interpretation it was built against. |
| Alpha8 is a configuration-only saving | PPR validates four bytes per pixel and uploads `RGBA8Unorm`. R8 needs a payload/format and shader sampling design; retain RGBA initially. |
| Bilinear filtering needs adding | `createInvariantRenderState_` already uses linear min/mag/mip filtering and clamp addressing. Preserve it. |
| DPI publication proves scalable fonts | Current scale changes republish atlas pixels; this is not evidence of a font rebake or proportional geometry. Existing DPI tests exercise a local ratio helper, not production font/style transitions. |
| Font uploads are already asynchronous | `installDetachedFontAtlas_` submits and calls `waitOnHost()`. A rebuild may stall; dynamic glyph updates must not inherit this cost unexamined. |
| Demo tooling needs adding to the build | `imgui_demo.cpp` is already in `imgui.base`; expose a development-only inspector entry only if needed. |
| Fonts will be staged with other assets | The current working tree now includes font binaries/notices, a `fonts` staging allowlist entry and conditional game registration. Verify actual synchronization and selected-font loading; configure success is insufficient. Make staging required when runtime depends on it. |

Source anchors: [ImGui service](../../lib/engine/app/ui/App.UI.ImGui.cpp),
[detached vocabulary](../../lib/engine/app/ui/App.UI.ImGui.cppm),
[UI ownership map](../../lib/engine/app/ui/codemap.md),
[frame transport map](../../lib/engine/app/frame/codemap.md),
[dependency setup](../../cmake/external/DearImGui.cmake),
[game staging](../../game/CMakeLists.txt).

Version-specific corrections: the pinned header reports `IMGUI_VERSION_NUM`
19291. It has neither `AddSubtractedRect` nor `ImGuiConfigFlags_IsSRGB`.
`ImDrawListFlags` and `ImDrawFlags` serve different purposes; do not substitute
one for the other. `CurveTessellationTol` remains valid. The obsolete one-argument
`PushFont` overload is not evidence that the new two-argument form enables
dynamic rasterization on PPR's legacy backend. See the
[pinned public header](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/imgui.h).

Remove the blanket API blacklist. Legacy atlas calls and persistent glyph ranges
remain necessary for the first delivery; remove them only when a tested replacement
lands. Do not copy APIs from `master`, forks, or unavailable research-session notes.

## 3. Visual direction and concrete defaults

Use **Graphite** as the proposed default: neutral dark surfaces, a restrained blue
accent, understated borders, and stronger typography. These values are starting
tokens for screenshot review, not measured final colors.

| Role | Starting value at 100% |
|---|---|
| Canvas / panel / raised surface | `#14171C` / `#1C2027` / `#262C35` |
| Border / stronger border | `#343C48` / `#536074` |
| Primary / secondary text | `#E8EDF4` / `#ADB8C8` |
| Accent / hover / active | `#78A9FF` / `#9ABFFF` / `#568DE0` |
| Text on filled accent | `#101722` |
| Success / warning / error | `#83C6A4` / `#E6BD72` / `#F08B8B` |
| UI typography | Inter Regular, 16 logical px; section headings 18 px if useful |
| Numeric/debug typography | JetBrains Mono Regular, 14–15 px; no ligature/shaping promise |
| Geometry | Container radius 6; control radius 4; border 1; scrollbar 12 |
| Spacing | Base unit 4; window padding 12; frame padding `(8,4)`; item spacing `(8,8)` |

Keep one compact token value type and one style mapping function. Start private
to the UI implementation; export only what actual client presentation needs.
No theme service, parser, runtime editor persistence, or token generator.
Map windows, children, popups, headers, tabs, tables, selection, disabled state,
keyboard focus, docking targets, and scrollbars explicitly. An ImGui preset may
seed unmapped defaults; banning `StyleColorsDark()` is not an acceptance test.

Use filled accent sparingly for primary actions. Keep secondary actions neutral;
danger/status also needs text or a symbol. Prefer opaque panels for predictable
legibility over the scene. Use subtle table striping only where it helps scanning.
Docked panels may remain square at shared edges; rounding is for floating chrome.

Proposed icon default: one Font Awesome Free solid font, exact release selected
and pinned during M0. The current assets pin Inter 4.1, JetBrains Mono 2.304 and
FA Free 7.3.1 solid; see [provenance](../../assets/fonts/PROVENANCE.md). Their
identity is verified; runtime bake and legibility remain pending. Pair icons with
text for unfamiliar actions; tooltips and
visible keyboard focus are required for icon-only buttons. Lucide is an alternative
direction, not another dependency. Verify the font artifact's license separately
from its icon artwork and generated C++ header.

## 4. Ordered implementation milestones

### M0 — Baseline, assets, and color-path check

1. Capture representative panels before changes: display settings with confirmation/
   error states, a dense performance table/plot, and a game panel with nested
   sections and editable values. Include hover, active, disabled, focus, popup,
   and narrow-window states. Use a repeatable scene and window size.
2. Inspect the existing UI shader, blend factors, surface formats, and presentation
   path. Record how authored sRGB colors and glyph coverage reach the framebuffer.
   Do not insert a conversion or change the scene color path on assumption.
3. Pin exact font/icon files and header revision, checksums, upstream URLs, licenses,
   and redistribution notices. Use static font files initially. Identify the
   repertoire required by labels, user-visible paths, and icons; document unsupported
   scripts rather than claiming arbitrary Unicode support.
4. Establish executable-relative asset resolution and `fonts` staging through the
   existing asset facility, including licenses and data-only rebuild behavior.
   Verify a packaged launch from a different working directory.
5. Record baseline atlas dimensions/bytes, startup/rebuild time, upload waits, UI
   CPU/GPU time, and draw/vertex counts. These are measurements to take, not existing
   results or an invented engine-wide budget.

Exit: reproducible before captures, asset manifest, and known color/coordinate
contracts. Windows is the first visual test host; preserve platform boundaries
and report other platforms as unverified until exercised.

Current evidence limits: 66 historical captures do not cover every required
state and lack complete reproduction metadata. The runtime surface/view format
is unresolved, so shader inspection alone cannot establish framebuffer encoding
or a scene/UI color-space mismatch. Record an attributable cost baseline before
visual edits; whole-frame HUD timings are not isolated UI GPU measurements.

### M1 — Theme and two representative panel conversions

Implement the tokens and mapping, then apply them to display and performance UI
before larger game panels. Group related controls, align labels/values, clarify
primary actions, remove redundant separators, and standardize section headings
and empty/error states. Avoid oversized cards around every field; retain useful
information density.

Use existing ImGui tables/layout and repository widgets. No bespoke controls yet.
Keep IDs stable when adding icons or changing display labels, so focus, open
sections, and saved state do not change unintentionally. Keep style/font stacks
balanced on all branches.

Exit: paired captures show clearer hierarchy; keyboard/gamepad navigation, mouse
capture, scrolling, and display confirmation still work.

### M2 — Static fonts, icon merge, and correct DPI transitions

Split execution into **M2a feasibility proof**, then **M2b integration**. Before
affected production edits, record explicit font-input injection, recoverable
file/bake-error handling, pinned static bake/density APIs and units, first-frame
scale acquisition (UI currently initializes before main-window creation), and
bounded queued-frame/atlas replacement ordering. An unresolved API or compiler
blocker returns for a bounded design decision; it does not authorize M6 as a
workaround. A named model's availability is not a technical gate.

- Load fonts before initial atlas upload. Keep stb_truetype initially; a chosen
  typeface and correct sizing are independent of the FreeType evaluation.
- Use engine file I/O for recoverable asset errors before calling ImGui. Prefer
  service-owned font byte buffers with atlas ownership disabled; retain them until
  after context/atlas destruction. If transferring ownership instead, allocate
  with the allocator ImGui will free through. Never transfer a vector's data.
- In legacy mode bake explicit sizes and persistent glyph ranges, including
  selected icon codepoints. Merge icons into faces used by icon-bearing controls;
  verify alignment and PUA collisions. Use explicit matching reference sizes for
  base and icon sources when setting advances/offsets. Check cap-height alignment
  with the font inspector. Font coverage, icon coverage, and UTF-8 decoding are
  separate checks. See the [pinned font guide](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/docs/FONTS.md).
- Select the small icon subset actually used. The current files have 558 FA7
  glyphs in U+F000–U+F3FF with no Inter/JB overlap in that range; this is a
  selection envelope, not the default bake list. Cmap/hash checks do not prove
  rasterization. If the recorded FA6 fallback is needed, verify its matching
  notice, mapping, sizing and deployed identity along with the binary.
- Keep an immutable unscaled style. On scale changes derive a fresh style from
  that baseline and scale once for the new value, never cumulatively. Distinguish
  user UI scale, monitor content scale, and framebuffer/logical ratio; document
  units for layout, font bake, mouse input, and clipping to prevent double scaling.
- For the static path, rebuild/select fonts at the required raster density at a
  frame boundary using the pinned legacy API. Prove crisp size and logical advance
  metrics at each supported DPI; setting `FontScaleDpi` alone is not that proof.
  Resolve the bounded API/units choice before production edits, without implicitly
  enabling dynamic textures as a workaround.
- Publish owned pixels and a new version whenever atlas content changes, including
  font changes at unchanged framebuffer scale. Preserve retry of unacknowledged
  publications. Bind each frame's geometry to compatible atlas content; queued old
  frames must finish before an incompatible replacement is used for them.
- Prepare replacement data before committing where practical. Read/bake-validation/
  upload failures report operational errors, never silently replace the chosen
  font with a default. Retain a prior configuration only if rollback is complete;
  otherwise stop UI submission through the existing error path. `std::bad_alloc`
  remains fatal under the repository contract.

Exit: deployed fonts/icons work; 100/125/150/200% and a 100→200→100% transition
have correct text, geometry, input, and clipping. No per-frame rebake/upload at
steady scale. Missing/malformed assets and partial initialization have explicit
error and cleanup coverage.

### M3 — Complete existing surfaces and acceptance evidence

Apply the proven composition to current game panels, keeping state/policy in the
owning client. Use the actual working tree's panel inventory; do not invent
30 editor panels. Test long labels, small windows, scrollbars, selected rows,
disabled controls, popup overlap, and focus over bright and dark scene backgrounds.

Use 4.5:1 as the body-text contrast target on its actual composited background,
including secondary text. Aim for 3:1 for essential non-text state indicators.
These are local visual targets, not a claim of full accessibility conformance.
Do not rely on color alone or claim native screen-reader support.

Exit: compact before/after gallery, verified interactions, focused test results,
and baseline/candidate cost measurements. This is a complete useful delivery
even when M4–M6 remain deferred.

### M4 — Optional FreeType evaluation

Compare identical fonts/sizes with stb and FreeType on the same displays; adopt
FreeType only for a visible benefit worth its dependency/build cost. Check existing
dependencies for reuse first. Pin a concrete version and retain its chosen license
and required notices.

Integrate the vendored wrapper through `imgui.base`; keep configuration consistent
with generated module bindings and consumers. Follow the allocator skill before
changing hooks. Start with default hinting, compare alternatives, and do not
describe a flag as equivalent to DirectWrite without evidence.

FreeType needs a deliberate blending/color-space decision; its
[pinned integration notes](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/misc/freetype/README.md)
describe linear blending. An ImGui flag does not configure PPR's GPU pipeline.
Recheck translucent edges and scene/UI composition after shader/target changes.
Retain stb if that broader work is not justified. Load the selected font files
explicitly rather than making the result depend on ImGui's default font choice.

Exit: matched text/edge captures, documented rasterizer choice, dependency/license
record, and relevant asset/render tests if the rendering path changes.

### M5 — Optional restrained shadows, with a feasibility gate

Implement only if the themed UI still needs depth. Start with one owned floating
window and a popup; keep docked shared edges and ordinary children shadow-free.
Use the existing atlas/shader: CPU-baked white RGB/alpha coverage, padded against
bilinear bleed, with tinted draw-list geometry. This is a raster mask, not
shader-evaluated SDF rendering. Do not adopt an upstream shadow branch/fork.

Before broad rollout prove both:

1. **Geometry and order:** the ring excludes the rounded interior even for
   transparent windows; outer geometry survives content clipping; clip state is
   restored; the shadow stays behind its own window and respects other windows/
   popups. Emitting after `Begin()` or on the global background list does not
   automatically satisfy this. `AddSubtractedRect` and `WindowShadowSize` are
   not stock APIs to call. Modest window rounding does not replace content clipping.
2. **Atlas consistency:** allocate/write the custom rectangle before final static
   atlas publication. Retain its handle, refresh UVs after rebuild/repack, and
   never mutate a generation already referenced by detached frames.

Prefer a bounded helper for explicitly owned floating surfaces. If correct popup
ordering needs ImGui internals, record the exact pinned coupling and maintenance
cost before adoption. If not worthwhile, defer shadows explicitly; rounded panels
and borders still meet the first-delivery goal.

Exit: overlap, rounded transparent interiors, viewport edges, DPI changes, hidden/
collapsed windows, and popup layering pass visual checks; zero unexpected texture/
callback rejections. Measure vertices/draws and CPU cost. No custom shader callback
or separate shadow texture in this milestone.

### M6 — Separate backend project: dynamic fonts/textures

Proposed follow-up, not a prerequisite or architecture change hidden inside
cosmetic work. Adopt when arbitrary sizes/glyphs or measured static rebuild costs
justify it. Keep the ImGui pin unchanged initially.

Resolve the transport design before implementing `RendererHasTextures`:

- Client owns live `ImTextureData`, IDs, statuses, and pixel extraction. Packets
  carry owned create/update/destroy data and logical identity plus generation;
  no ImGui pointers cross to the worker.
- Draw commands reference the generation they were built for. Support simultaneous
  retiring/current atlas textures when growth requires them, or demonstrate a
  bounded serialization design. A universal frozen ID is not sufficient evidence.
- Define acknowledgment after owned capture or after installation, with matching
  pixel retention/retry. Drop, minimization, cancellation, duplicate/stale ack,
  and upload failure must not lose updates or draw missing pixels. Distinguish CPU
  consumption, installation, accepted submission, and GPU completion.
- Specify pitch/extent validation, resource transitions, capacity, and backpressure.
  No queue-wide wait for each new glyph. Full snapshots are an option only with
  measured copy/transfer cost.
- Retire GPU resources after the last accepted-use fence completes; settle pending
  publications during teardown/partial failure. `UnusedFrames` is not GPU completion
  proof. Do not delete library-owned texture records as backend GPU allocations.
- Keep foreign textures and arbitrary callbacks rejected. Supporting atlas
  generations does not authorize user images, blur, or custom shaders.
- Keep RGBA initially. Alpha8 needs explicit coverage-channel sampling and
  format-aware payloads; it is a separate measured change.

Use the [pinned backend guide](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/docs/BACKENDS.md)
and [reference texture lifecycle](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/backends/imgui_impl_dx11.cpp)
for protocol semantics, adapting ownership to PPR rather than copying a synchronous
backend. Enable the flag only when the full supported lifecycle works. Warm
representative glyphs/sizes deliberately; cache compaction is not warm-up.

Exit: create/update/growth/retirement and fault-injection tests in inline/worker
modes, bounded memory under repeated resize, and cold/warm performance evidence.
Remove legacy static atlas handling only after the replacement passes.

## 5. Validation and delivery

Use [validation](../../.opencode/skills/validation/SKILL.md),
[build-system](../../.opencode/skills/build-system/SKILL.md), and
[unit-test-updater](../../.opencode/skills/unit-test-updater/SKILL.md).
Use [module-architect](../../.opencode/skills/module-architect/SKILL.md) for new
partitions; register source/CMake together. Follow
[memory-allocator](../../.opencode/skills/memory-allocator/SKILL.md) and
[concurrency-patterns](../../.opencode/skills/concurrency-patterns/SKILL.md)
when those boundaries change. Choose the error macro appropriate to each return
type and best-effort cleanup; do not prescribe one macro for every operation.

| Changed behavior | Required evidence |
|---|---|
| Theme/panels | Matched captures and mouse/keyboard/gamepad checks; screenshots are not automated snapshot tests unless a real harness exists |
| Fonts/DPI | Extend [DPI tests](../../lib/engine/tests/app/App.ImguiDpi.Tests.cpp) through production behavior, not another copied scale formula; repeat transitions and same-scale atlas changes |
| Detach/atlas | Extend [routing tests](../../lib/engine/tests/app/App.ImGuiRouting.Tests.cpp): valid font/icon/shadow draws, foreign texture/callback rejection, retry after dropped publication, inline/worker parity |
| Assets/upload/lifetime | Missing/corrupt font, failed upload, repeated init/shutdown, partial failure and in-flight replacement checks; applicable asset tests |
| Shared render transport | Full matrix and lifetime/stress gates per validation; not automatically for a theme-only batch |
| Runtime cost | Same scene, size, DPI, fonts, preset and hardware; startup/first-use/transition/steady-state CPU/GPU samples, upload bytes/waits, atlas memory, vertices/draws and sample spread |

One 1024×1024 RGBA8 image is 4 MiB; R8 is 1 MiB, excluding CPU copies, staging,
retirement overlap, and allocator overhead. Do not force a 1024² minimum or promise
a 5 ms hitch ceiling without an agreed workload/budget.

During implementation run touched-C++ formatting/semantic gates, an affected Dev
build, and focused tests; widen only per validation triggers. Keep one independent
review per coherent delivery. Report passed, failed, deferred, and unrun gates.
Visual quality does not establish routing or lifetime correctness.

This document-only revision needs reference/API checks and the docs-only
[code-reviewer](../../.opencode/skills/code-reviewer/SKILL.md) gate, not C++ builds.

## 6. Decisions and deferred work

The defaults provide a concrete starting proposal: Graphite, Inter plus a small
mono face, one solid icon set, existing surfaces, and Windows-first visual evidence.
Product feedback can change them after paired panel captures; no designer-role
dependency blocks implementation. Exact asset versions, supported glyph repertoire,
and the static DPI API/units choice must be recorded during M0/M2 before related
code lands.

Motion and custom widgets wait for a specific surface need. Use existing controls
first. Later animation receives explicit frame delta time, uses bounded per-widget
state with stable IDs, respects reduced-motion policy, and never delays interaction.
Do not pre-install animation, spinner, or toggle libraries for hypothetical use.

MSDF text, color emoji, background blur, theme hot reload, arbitrary user textures,
custom native titlebars, and ImGui upgrades are outside this plan. Optional work
may be omitted with explicit status; first delivery is accepted on M0–M3 evidence.
