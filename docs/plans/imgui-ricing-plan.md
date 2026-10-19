# PPR ImGui visual refinement plan

> Current accepted revision: 2026-10-18.
> The newest user feedback (independent windows, compact functional rail,
> consolidated context, Quit, concept fonts, borders and authoring limits) is
> implemented; fresh Dev/profile builds, focused tests and independent source
> review pass. Native interaction and matched trace acceptance remain in progress;
> earlier passing records below belong to the preceding composition.
> The optimized MSVC C1001 at the original WorldGenPanel.cpp:445 reproduced
> with `/O2 /GL` and MSVC14.52.37025. The coordinator-approved final remedy
> removes only an unused direct `engine.math` import; the original full TU
> passed that isolated contrast. The final full profile app build, normal Dev
> app/tests build and all 10 UI test leaves passed with numeric exit zero.
> Original lookup/ID APIs and optimization flags are preserved. Exact evidence
> and unknown compiler internals are recorded in progress.
>
> The user requested a complete UX
> retake after assessing the unstaged work. The rounded Site Office reference
> supersedes Graphite. Scoped correctness and bounded loaded-Workshop performance
> repairs passed for the previous composition. The latest user revision now merges
> authoring into one workspace and restores the reference ribbon/rail hierarchy.
> Its affected builds, source review and focused tests passed. Earlier native
> Start/Return preserved camera/pause; final compact/narrow/unsaved checks remain
> open. An audited loaded-workspace diagnostic shows the preview-choice cost
> reduction. The user later confirmed the merged workspace with 513 assets;
> automated final snapshot, selected profile and matched repeat remain unverified.
> Tail acceptance is unmet. The usual msvc-dev integration build passed;
> this is not a further native acceptance run.
> Deferred gates and exact evidence are recorded in progress.
> Historical milestone passes below are not verification of this candidate.

## Current delivery: Site Office and a gameplay-first UI boundary

Use warm tan canvas and section bands, cream panel cards with hard 2–3 px ink
outlines, dark ink text, modest rounding, yellow primary/selected tools and blue
selection. The concept board is the visual authority for that chrome; the
2026-10-19 revision below records the applied token and geometry deltas. Match
the concept with pinned Pixelify Sans 400 body / 600 emphasis, Big Shoulders
Display 700 headings and Font Awesome icons. Keep ordinary ImGui buttons, menus,
tables, tabs, checkboxes, sliders and scroll regions. The HTML is visual
reference data; its decorative bevels, hazard stripes and invented product
examples are not requirements to rebuild standard controls or add gameplay
systems.

The accepted edit packets are:

1. Preserve the pre-edit tracked diff/untracked UI sources and obtain a bounded
   baseline. Evidence is in `.slim/deepwork/imgui-ricing/assessment-2026-10-18/`:
   HEAD `8594a02`, tracked diff, untracked browser/dependency copies, one baseline
   image and provenance. The capture used the existing M3 Run8 executable;
   no fresh baseline build or new cost measurement was performed.
2. Own game presentation in `game/ui`. Existing panel module names stay stable.
   A concrete adapter reads the const driver before drawing, copies queried
   cell/scalar summaries, and borrows stable names and panel-safe `AgentSummary`
   rows for that synchronous call only. Draw code receives no driver/services;
   actions are dispatched afterwards. No borrowed data enters detached rendering.
3. Use the top simulation/status/management ribbon and a compact left
   Inspect/Dig/Build/Demolish rail fitted to its buttons. Build visibly opens
   the existing native popup, also reachable with B. Colony/context and
   Performance are independent windows; F3 toggles only Performance. Relocate
   selection, pin/follow, gesture feedback and notices into the left context
   window; remove the redundant right inspector and bottom strip. Only the
   full-width ribbon reserves a whole edge: finite windows use existing native
   ImGui capture and background-drag routing. Native X and Menu Quit share the
   existing host dirty-save/discard/cancel and work-drain exit path.
4. Extract world/unsaved-edit modals and background feedback drawing into game UI
   code. Keep world picking/projection, camera, lifecycle, display/workshop
   transactions and descriptor I/O with their existing owners. Shared world-job
   progress vocabulary lives in a narrow domain module, re-exported by WorldJob.
5. Apply the same visual hierarchy to Display, Performance, Workshop and Asset
   Browser. Fit first-use utility windows to the viewport, reflow action rows,
   and use native workspace pane tabs at narrow widths. Embed the browser's actual
   multiselect/box-select/sort/keyboard/zoom in one authoring window, with kind-glyph
   thumbnails and the existing schema property editor. Remove separate raw
   descriptor windows from this composition. Correct the
   zoom anchor when column count changes.
6. Allow the authoring workspace to resize beyond its initial viewport-fitted
   size. Use 3 logical px window/popup and 2 px child borders, preserving a
   separate control border and noncumulative DPI scaling. Raise participant-wide
   sequential halo maxima from 24 to 58 (authored maxima 6/24/4/24), preserving
   numeric per-stage caps 10/25/10/25, dynamic scratch sizing and cancellation.
7. Correct production monitor-scale transitions, test owner teardown, run focused
   behavior checks and independent review, and capture bounded candidate states.
   Reconcile source/test evidence with screenshots before accepting the redesign.

The static RGBA font atlas, revision/retry handshake, detached frame/fence
ownership and content-free Renderer remain. FreeType's opt-in evaluation wiring
is retained OFF by default; the reopened optional M4/M6 projects remain separate
from this UX delivery. Shadows remain deferred.

Optimized uncapped normal-gameplay profiling precedes performance edits. The
2026-10-18 capture measured Update execution median/p95 0.457/0.707 ms and UI
0.069/0.114 ms; platform waits dominate root duration. PIX timer samples identify
packet catalog/image copying as a measurement candidate, not a proven regression
or authorization for speculative caching. Cadence tails remain open. Exact
SQLite results and qualified sampled-stack transcription are in
`out/pix-manual/msvc-profile-uncapped-gameplay-20261018/`.

### Accepted merged-authoring revision — 2026-10-18

The subsequent loaded-workspace attribution found three identical sample-biome
choice builds per frame dominating Worldgen CPU (median summed 10.846 ms).
An accepted bounded private UI cache owns the exact result and reuses existing
document-refresh invalidation plus the selected profile; it does not cache by
validated catalog alone. Empty/invalid choices stay honest and each caller's
reference ends before a later profile-changing query. Native gate/intent tests
passed across warm/revision/profile/source transitions. An audited readiness
diagnostic contains 360 complete Workshop/browser draws and zero PreviewChoices
events: Worldgen CPU execution median/p95 is 0.6659/0.7439 ms versus attribution
11.763/13.24 ms, Update 3.8628/4.2476 ms. Its 359 frame-start deltas have median
13.401 ms, p95 18.568 ms and 31 (8.6%) over 16.67 ms. Arguments/root/pack
fingerprint match, but the failing native window tool prevented a fresh automated
snapshot and the final matched repeat. After the candidate closed, the user replied
that the asked post-fix view was a merged workspace with 513 assets. This supplements
workspace/count identity, not selected-profile confirmation, a new timestamped
native observation or final compact/narrow/unsaved acceptance. This is measured scope
improvement in a qualified diagnostic, not GPU FPS or an all-frame budget pass.
Remaining nested renderer execution and stalls do not independently attribute
every tail interval. The exact artifacts and deferred gates are in progress.

The pack command ribbon owns Open/Reload/Save/history/Preview. Stable AssetKey
navigation uses the current draft ownership index and qualified names, including
invalid editable drafts. The main editor reuses all nine schema control families;
unsupported kinds are explicitly read-only and reserved vacuum remains reachable.
Preview and Diagnostics occupy secondary tabs. Selection from tiles, references,
diagnostics or preview terrain is deferred until the old inspector commits its
text/numeric buffer. The host dispatches owning mutations before transitions and
rechecks current validation/revision after rejected or newly accepted edits.

Return to game is separate from Quit app. It drains preview/capture work and
restores the existing driver, camera and pause state without destroying the
authoring session/selection. Standalone Start game owns an immutable current
authoring snapshot, qualified profile and seed. The existing WorldJob accepts a
per-request preparation callable; it validates the complete owned snapshot,
including restoration proofs, and uses the ordinary prepare/presentation/driver
pipeline with the same explicit clock and cancellation context. Descriptor bytes
are never replaced with startup disk descriptors; presentation image I/O remains
explicit. Preview/render commands drain before admission, generation alone pumps
its acknowledgments, and cancel/error retains the workspace. Only successful
publication commits the authored source used by subsequent New Game requests.
Open/Reload/Quit retain Save/Discard/Cancel for unsaved changes.

Acceptance adds native multi-frame edit-to-tile commit and distinct Start/Return/
Quit tests, stable identity across reindex/filter, rail geometry/native summary
capture, request-override lifetime/cancel/default-policy tests, real owned-draft
validation-to-generation cancellation, icon merge coverage and enlarged-symbol
static-atlas stability. Earlier native authored publication and Return camera/pause
preservation passed before summary compaction. Final compact/narrow visuals,
unsaved transitions and the remaining lifecycle paths remain manual integration
gates. The qualified current diagnostic above supplements historical performance
evidence; final matched timing and tail acceptance are still open.

### Accepted color-contract amendment — 2026-10-18

The first candidate improved composition but failed visual acceptance: authored
ink/tan became gray/cream and muted text lost contrast. Source inspection of the
selected D3D12 dependency and approximate JPEG samples support an sRGB attachment
encoding already-authored sRGB vertex values again. The active attachment format
and monitor color mode were not recorded at runtime; that path remains an
inference, not a GPU capture measurement.

Keep authored palette bytes, ImGui packing and detached vertices unchanged. The
UI-owned vertex shader decodes RGB before interpolation only when the actual
draw-target signature's format metadata is sRGB. One private reflected `uint`
is bound to both existing slot roots at pipeline creation. Reuse the exact
piecewise transfer already present in the background shader; alpha, atlas
sampling, fragment multiplication, Renderer and the static font protocol stay
unchanged. Non-sRGB targets retain their existing numeric behavior. This bounded
amendment supersedes earlier instructions to preserve the UI shader unchanged;
it does not authorize a renderer-wide color system.

Production-service GPU readback must verify opaque ink/tan, half-alpha blending
and black-to-white interpolation on sRGB and UNORM targets, including both slot
roots and return transitions. Candidate visual acceptance must be repeated after
the correction. Colony diagnostics now default closed below the primary
simulation/colonist controls, and a native multi-frame Menu-to-Display test
verifies the actual popup/intent contract without a production test seam.

### Measured loaded-pack performance amendment — 2026-10-18

Loaded Workshop timing isolated the exact clean-state dirty comparison at about
98.9% of the measured main-update batch: ten calls totaled 4.568 seconds wall.
The coordinator approved a private revision-keyed result memo in the owning
`ContentAuthoringSession`, retaining the exact source/adopted-baseline comparison
on misses. Edits, history, revert and save-baseline adoption advance that revision,
including partial/canceled/stale outcomes; validation alone does not mark a draft
clean. No public API, snapshot ownership, filesystem behavior or UI widget changes.

The instrumented executable/PDB/source and repeated loaded-pack traces are
preserved before the repair. Acceptance requires affected Dev builds and owner
authoring tests, independent review, and matched repeated runtime samples plus
timing attribution. The cache's correctness and paired loaded-pack CPU attribution
now pass: median main update is 3.753 / 4.837 ms versus about 407 ms before, and
mean Model is about 13.4 µs. The subsequent accepted native label-batching change
replaces per-tile label clip-stack changes with the pinned public AddText fine
clip parameter, preserving parent/tile bounds and existing native interaction,
defaults and geometry. No new mode, widget framework or cache was introduced.
Normal builds, 56 owner leaves, independent source review and matched clipping
visuals pass. Matched Grid cadence medians fell from 51.693 ms to 13.164 / 13.466 ms
and renderer UI wall medians from 25.684 ms to 1.622 / 1.638 ms. Tail intervals
remain above 16.67 ms (4.3% / 9.2%); their cause is unlocated and the all-frame
budget is not certified. Cadence is not achieved GPU FPS; fractional-edge byte
identity is not claimed. The inconclusive larger-icon experiment stopped and no
List mode exists. The user identified Workshop Close in the reported exit. Its
standalone action already quits the application, so the accepted clarity change
adds a host-supplied `m_quits_application` presentation flag from the exact
existing close-dispatch condition. The native button reads `Quit app` or
`Close workshop` with stable identity and unchanged close intent, confirmation
and teardown. Scoped format/style, author/Astra review, normal app/test builds
and seven UI leaves pass; actual standalone label/native-button exit validation
and remaining utility/narrow visuals are blocked by external UI-tool window-state
acquisition. The final process is open for review; no exit or crash is inferred.
Do not claim a crash fix. Exact evidence
and qualifications live in the progress record. Keep the existing budget and
unrelated passed correctness evidence; global delivery is not complete.

### Concept-board chrome revision — 2026-10-19

`docs/plans/ui_concept.png` is the visual authority for chrome. One header per
decorated panel: a single window title bar in Big Shoulders, yellow with ink
text, drawn through `ui::WindowTitleFont` (construct immediately before
`ImGui::Begin(...)`, `close()` immediately after so only the title bar uses the
heading face), and no second in-window title line. `ui::helpMarker(const char *)`
is the shared documentation-as-icon primitive: `kHelpMarkerGlyph` with its
`SetItemTooltip`, drawn on the icon-merged `body` face whatever face the caller
pushed.

Style-table deltas applied by this revision:

| Role | Before | After |
|---|---|---|
| Window surface | `kPanel #D9D2B4` | `kRaised #F4EED6` |
| Container and control border | `kBorder #8A7F5E` | `kBorderStrong #25221C` |
| Title bar unfocused / focused / collapsed | `kPanel` / `kRaised` / `kPanel` | `kAccent #FFC72C` / `kAccentHover #FFD768` / `kAccentActive #E8B012` |
| Menu bar | `kPanel` | `kRaised` |
| Button | `kRaised` | `kPaper` |
| Table header / alternating row | `kRaised` / `kRaised` @0.35 | `kHover #E9DEB8` / `kHover` @0.35 |
| Selected tab | `kRaised` | `kPaper` |
| Container radius / control radius | `6` / `4` | `2` / `2` |

Unchanged: `ImGuiCol_Separator` `kBorder`, `ImGuiCol_FrameBg` `kPaper`,
`Header*` selection blue, border sizes 3 / 2 / 3 / 1 with the zero-alpha
`BorderShadow`, scrollbar / plot / disabled / nav tokens, left
`WindowTitleAlign`, and `kFloatingPanelAlpha` — the alpha still multiplies the
window and the title background together.

Composed sRGB relative luminance at 100% alpha: body ink on the window surface
13.63:1, secondary ink on the window surface 6.48:1, ink on the three title
yellows 10.16 / 11.46 / 8.04:1, ink on a control frame 14.76:1, secondary on a
control frame 7.02:1, secondary on the table header 5.60:1, body ink on an
alternating row 12.93:1. At
`kFloatingPanelAlpha` over black the worst composited pairs stay ink-on-title
5.96:1 and secondary-on-window 4.72:1. Every text pair is ≥ 4.5:1.

Duplicates removed with it: `App.UI.Widgets.Performance.cpp` now calls the shared
`ui::helpMarker` instead of its own glyph, and the Display window no longer
repeats its own title as `SeparatorText("Display")`.

The layout pass changed the Authoring Workspace body, not its identity: the
window title stays byte-identical, `Authoring Workspace###Worldgen Workshop`.
A `File` / `Edit` / `View` / `Help` menu bar carries the existing save, reload,
revert, undo/redo, live-preview and diagnostics actions. The draft status line
and the pack toolbar follow it; the toolbar keeps the pack path, game
transition, preview and cancel actions. Above the narrow threshold the editor
sits beside a left preview-settings column with the asset browser in a
full-width bottom strip; below it the same three surfaces become native
`Assets | Editor | Preview` pane tabs.

The one-header rule as applied: Colony/selection context, Performance, Help,
Display, the Authoring Workspace and the modal dialogs each draw exactly one
title bar through `ui::WindowTitleFont`; the body draws on the face the caller
had. The former in-window `COLONY`, `PERFORMANCE` and `AUTHORING WORKSPACE`
heading lines are deleted. Section headings that name a subset of a window
(`Colonists`, `Camera`, `Contents`, and similar) remain. Windows drawn with
`NoTitleBar`/`NoDecoration` — the colony ribbon, tool rail, cursor Inspector and
floating performance widget — have no header and do not construct the guard.

`ui::helpMarker` is the shared documentation affordance for the game panels.
Caption lines that only explained a control became the `?` marker's hover
tooltip; data, status, error, section-header and field-label text stayed
visible. The Performance and asset-browser panels use the same marker; the
Performance graph captions no longer carry their own glyph.

The nested-property search now matches rendered property values in addition to
names and paths, so a value such as `2150` finds its field. Descriptor-backed
numeric properties keep a typed exact-value field beside their range slider;
both controls commit through the same pending-edit path, the typed value is
clamped to the schema's legal domain rather than the slider's display hint, and
a cleared or unparsable entry writes nothing.

A range slider spans its property's declared schema domain. The optional
metadata editing interval is reserved for a domain that is the full type range
(the noise frequencies, whose legal span is `0` up to the largest finite
double) and never narrows a domain the schema already states; the game and
worldgen schemas declare no such narrowing.

### DPI units and supported checks

For normal supported scale inputs, physical UI size is base size times the
monitor content scale. UI geometry uses one scalar: maximum valid content axis
(clamped 1–4) divided by maximum actual framebuffer/client ratio. Static font
logical size uses the same scalar; raster density uses that ratio once (existing
1–4 density bounds). Per-axis framebuffer ratios remain unchanged for clipping.
Every transition copies an immutable unscaled style, scales it once, and refreshes
`FontSizeBase`; `FontScaleMain` and `FontScaleDpi` stay 1. Static rebakes clear/re-add
the retained validated bytes/ranges between frames, preserve same-atlas loader
state and predecessor retirement, restore the frozen texture identity, and stop
UI submission on failure. Empty-source callers explicitly re-add the default
face before density assignment. No font pointer may survive a service update.

Acceptance requires production-boundary 1/1.25/1.5/2→1 content-only transitions,
mixed/asymmetric framebuffer ratios, steady-state publication behavior, focused
UI/display/browser/routing tests, affected Dev builds and the accepted serialized
app/asset checks. Visual acceptance covers normal/narrow chrome, display/footer,
selection/focus, menus/modals and Workshop/browser windows. Physical multi-monitor
testing, isolated UI GPU timings, paired cost baselines and broader platform
coverage remain explicitly unverified unless separately recorded. No performance
or full accessibility claim follows from the palette or profiler scopes.

## Earlier milestone detail and retained backend gates

The remaining sections preserve useful implementation rationale. Current scope,
composition and status above override their earlier sequencing/completion wording.


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

Use **Site Office**: warm opaque surfaces, ink text, yellow primary actions and blue selection. Values below are current source tokens; actual composited contrast and interaction states still require visual acceptance.

| Role | Starting value at 100% |
|---|---|
| Canvas / panel / raised surface | `#CFC7AB` / `#D9D2B4` / `#F4EED6` |
| Border / stronger border | `#8A7F5E` / `#25221C` |
| Primary / secondary text | `#25221C` / `#5A5444` |
| Accent / hover / active | `#FFC72C` / `#FFD768` / `#E8B012` |
| Text on filled accent | `#25221C` |
| Success / warning / error | `#2F682B` / `#784C08` / `#A33016` |
| UI typography | Inter Regular, 16 logical px; section headings 18 px if useful |
| Numeric/debug typography | JetBrains Mono Regular, 14–15 px; no ligature/shaping promise |
| Geometry | Container radius 2; control radius 2; border 1; scrollbar 12 |
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
