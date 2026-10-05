# Away Team — UX / UI + Art Direction (Clone Spec)

> Companion to [AwayTeam-Gameplay-Inventory.md](AwayTeam-Gameplay-Inventory.md)
> (systems, numbers, scope). This file covers how the clone *looks, feels and
> reads*: controls, camera, build placement, overlays, HUD, tooltips, art
> direction, accessibility, onboarding.
>
> Title: **Away Team** (Steam 2168390, Klei). 2D side-scroller, one directly
> controlled duplicant, full tile simulation. Not a colony manager.
>
> Tags used here (unified with the gameplay and architecture specs):
> `[OBSERVED]` = seen in footage or store page,
> `[INFERRED-PT]` = playtest inference (partial footage, needs proof-check),
> `[TUNING-UNKNOWN]` = threshold / ratio / rate unconfirmed; value is a clone
> placeholder, `[DECISION]` = clone choice (not observed; decided for MVP),
> `[ONI-REF]` = reused ONI baseline (not re-derived here),
> `[PPR-FACT]` = verified in PPR source / codemaps.
> `[GAP]` is retained as a capability marker (not evidence): engine support that
> does not exist yet and is out of scope for v1 unless stated.

---

## 1. Design principles

Five rules the whole UI is judged against.

1. **The body is the instrument.** There is one duplicant, so vitals live on
   the body and at the screen edge — not in a panel you open. Panels are for
   depth, never for "am I about to die". `[DECISION]`
2. **Overlays answer "where should I go", not "how is the colony doing".**
   ONI overlays exist to find the weakest of 20 dupes. Here the question is
   scouting: where is breathable, where is warm, what will hurt me, what did I
   leave behind. Overlays are a map tool, not a management tool. `[DECISION]`
3. **Say it in the world first.** A gas leak is a hiss and a drifting plume at
   the leak; the number is what you read when you hover. Text never explains
   what a visual already shows. `[ONI-REF]`/`[DECISION]`
4. **Placement is forgiving.** You must be able to size, re-size, cancel and
   commit a footprint deliberately, in pause, without the preview fighting you.
   The playtest complaint "could not shrink" is treated as a bug class, not a
   learning curve. `[OBSERVED]`
5. **Nothing needs a mouse.** Every gameplay verb has a keyboard path and an
   abstract action name, so a controller layout can land later without
   redesigning the verbs. `[DECISION]` / controller support is unannounced.

---

## 2. Controls scheme

### 2.1 Input model

Actions are declared once as named, abstract `InputAction`s (move, jump, tool,
interact, overlay-n, pause), bound per device. The current engine already
separates action from key and supports modifiers, per-binding callbacks and an
explicit `trigger_when_paused` flag — pause behavior is a property of the
action, not of a global input switch. `[PPR-FACT]`

Arbitration order stays the one the demo established: ImGui gets first refusal,
then tool gesture, then camera, then world. Tool and camera actions start
transparent and only consume while they actually own a gesture, so a build drag
never pans the camera (the background-drag latch already handles this).
`[PPR-FACT]`

### 2.2 Keyboard + mouse (primary, ship first)

Movement and tools are split: keys move the body, the mouse aims and places —
and every mouse placement has a full keyboard-only equivalent, so no verb
requires a pointer.

| Action | Bind | Notes |
|---|---|---|
| Move left / right | `A` `D` / `←` `→` | Runs; hold `Shift` to walk |
| Climb up / down, mount ladder | `W` `S` / `↑` `↓` | Mount when overlapping a ladder; drag speed while climbing `[OBSERVED]` |
| Jump (variable height) | `Space` | Release early = short hop. Hold-to-also-jump (up) `[OBSERVED]` |
| Interact (crank, enter, harvest) | `E` | Context prompt appears at the object |
| Tool 1…5 | `1` `2` `3` `4` `5` | Dig, Vacuum, Build, Deconstruct, Inspect |
| Tool primary (use on world) | Left mouse | Hold for dig swings / vacuum stream `[OBSERVED]` |
| Tool aim / place preview | Mouse cursor | World-space, snaps to cells for build, free cone for vacuum |
| Keyboard aim / placement cursor | `I` `J` `K` `L` step cursor cell-by-cell | Keyboard-only path: steps the placement cursor without a mouse; works paused; `Enter` commits, `Esc` cancels `[DECISION]` |
| Cancel current tool/gesture | `Esc` or right mouse | Esc runs the existing cancel chain (overlay → gesture → tool → follow) `[PPR-FACT]` |
| Build menu | `B` | Menu opens; placement completes via click OR keyboard cursor-step + commit — never click-only `[DECISION]` |
| Pause / resume | `P` | `Space` must stay jump — this moves the demo's pause bind `[DECISION]` |
| Step one tick (paused) | `N` | Unchanged from demo `[PPR-FACT]` |
| Sim speed 1× / 2× / 3× | `[` `]` or top-bar buttons | Frees `1`–`5` for tools `[DECISION]` |
| Re-centre camera on dup | `C` | Also exits free-cam `[DECISION]` |
| Overlays | `F2`…`F9` | See §5; `F1` stays Help `[DECISION]` |
| Performance/debug overlay | `F12` | Moved off `F3` so `F3` can be Temperature `[DECISION]` |
| Chunk grid (dev) | `V` | Keep as-is; already verified free repo-wide `[PPR-FACT]` |

Conflicts to resolve at bind time, not later: the demo currently binds `Space`
(pause), `F` (find colonist), `F1` (help), `F3` (performance tab) and `1`/`2`/
`3` (speed). Platformer expectations win for `Space`, `F` and `1`–`5`; the
displaced functions move to `P`, the top bar, and `F12`. `F1` Help is kept.
Camera bindings (WASD/QE, arrows, page keys, mouse, wheel, gamepad shoulders)
only apply in free-cam mode, which is what keeps `W`/`S` unambiguous between
climbing and camera. `[PPR-FACT]`

### 2.3 Controller (unannounced — design for it, do not promise it)

Away Team has no announced controller or Steam Deck support, and none is
required for v1. The verbs are still written so a pad layout can be added
without redesign. Controller scope is design-for only. `[DECISION]`

| Verb | Pad suggestion | The hard part |
|---|---|---|
| Move / climb | Left stick (digital threshold) | — |
| Jump | South button | — |
| Interact / crank | West button (hold) | Crank needs a hold + a visible charge ring `[OBSERVED]` |
| Tool primary | Right trigger | Aim defaults to facing direction |
| Tool aim | Right stick | Emits a virtual cursor; for build, snaps cell-by-cell |
| Tool select | D-pad left/right or bumper | Tool strip highlights the active slot |
| Overlays | D-pad up + face-button radial | Radial beats 9 hard-coded binds |
| Pause / speed | Start; face button for speed | — |
| Camera free-pan | Right stick while holding a modifier | Follow mode ignores it by default |
| Read a tooltip | Modifier + right stick | Native ImGui tooltip hover is mouse-only today `[GAP]` |

Two real gaps, stated plainly: **touch is unsupported entirely** (the input
front-end is GLFW keyboard/mouse/gamepad; there is no pointer/touch device, no
virtual joystick, and building with a rectangle drag does not map to a finger
without a redesign) `[GAP]`, and **gamepad tooltip reading** needs an explicit
inspect mode rather than relying on hover `[GAP]`. Both are out of scope for
v1; the abstract action layer is what keeps the door open.

---

## 3. Camera

One camera, three states, and the follow target is the single source of truth
(the controller owns it; UI and input only read/set it) — this is already how
the demo's pan controller works. `[PPR-FACT]`

| State | Entered by | Behavior |
|---|---|---|
| **Follow** (default) | Spawn, `C`, re-centre | Smoothly tracks the dup with a dead-zone rectangle (not a hard center), a small look-ahead in the facing direction, and a slight downward bias so ground underfoot is visible. Zoom is independent of follow. |
| **Free** | Middle-drag, or any pan input while following, or pause | Pans freely; follow flag clears. Wheel zooms via the same clamped ortho scale the demo uses. A pill at the screen edge points back at the dup when it leaves view. |
| **Paused** | `P` | Sim freezes (fixed-timestep accumulator clamps); camera, build preview, overlays and hover all keep working. This is the survey workflow `[OBSERVED]`. Single-player: pause is always legal. Co-op: host pauses, clients vote (host commits pause/resume) `[DECISION]`. |

Specifics:

- **Pause never freezes planning.** Tool placement, deconstruct, overlays,
  hover and camera must set the paused-action flag so they keep firing while
  the sim is stopped; body movement, dig swings and cranking must not. `[PPR-FACT]`
- **`N` single-step** while paused, unchanged from the demo. `[PPR-FACT]`
- **Zoom range is clamped** (`setOrthoScale` already rejects anything outside
  [0.001, 1000]); gameplay sits around the demo's 0.5 default, with a narrower
  clamp for the follow state so a scroll flick can't zoom the player off-screen.
  `[PPR-FACT]`
- **Camera cuts are flagged**, not smoothed: teleporting to a new area or
  leaving a pause survey uses a camera-cut flag so smoothing doesn't smear the
  move across a second. `[PPR-FACT]`
- **Free-cam in pause should feel like a survey tool**: a faint vignette and a
  small "PAUSED — survey mode" chip near the dup's last position, so a player
  who alt-tabs back knows the sim is stopped.

---

## 4. Build preview and placement

Placement has two gestures, because the game has two kinds of buildable.
Staged-then-commit is canonical: press stages, an explicit commit confirms;
commit-on-release is rejected. `[DECISION]`

### 4.1 Rect footprint (bricks, farm plots, deconstruct area)

Anchored drag: press sets the anchor, drag sizes, release *stages* — it does
not commit. This is the fix for "could not shrink".

| Requirement | Behavior |
|---|---|
| Shrink | Dragging back past the anchor shrinks, always. Past the anchor flips the anchor to the opposite corner so the rect can never go negative-sized. `[DECISION]` |
| Re-size after release | Staged rect stays editable: drag any edge or corner handle to adjust, before committing. `[DECISION]` |
| Discrete shrink | `Backspace` / `-` removes one cell from the far edge; `Shift`+click removes the whole staged rect. Also `-`/`+` steppers in the tool strip for keyboard-only players. `[DECISION]` |
| Keyboard-only placement | `I` `J` `K` `L` step the cursor cell-by-cell with no mouse; `Enter` commits, `Esc` cancels. The full anchor → size → stage → commit flow is completable keyboard-only. `[DECISION]` |
| Commit | Second click, or `Enter`. Explicit, not implicit on release. Commit-on-release is rejected. `[DECISION]` |
| Cancel | Right mouse, `Esc`, or picking another tool; staged rect and reservations drop. `[DECISION]` |
| Clamp / cap | Existing pattern kept: outline goes broken-line when invalid, clamp badges on capped sides, the reason string shown in the tool strip in words — never only as a colour. `[PPR-FACT]` |
| Cost feedback | Reserved materials shown live in the strip ("Brick ×42 — have 30"); shortfall is a reason string, not a red flash. `[DECISION]` |

Outline style: the demo's pixel-derived band weights (band scales with zoom,
clamped) are correct and stay; color encodes state — cyan = committable,
orange = will be clamped, broken-line = invalid. `[PPR-FACT]`

### 4.2 Path-laid infra (wires, pipes, ducts)

Infra is not grid-aligned and can run diagonally `[OBSERVED]`, so it never uses
the rect gesture:

- **Drag a polyline.** Each click (or each stick flick) drops a node; the
  preview shows the actual diagonal segments, direction arrows, and the port
  rings of whatever it touched.
- **Validation is about ports, not cells.** The preview turns invalid only when
  a segment crosses a solid brick it cannot pass or ends on a port of the wrong
  kind — not because a segment isn't axis-aligned.
- **Edit before commit.** The whole polyline is staged like a rect: drag any
  node to move it, `Backspace` removes the last node, `Enter` commits,
  right-click cancels.
- **Bricks over infra, never ladders over infra** `[OBSERVED]`: when a brick
  footprint overlaps live infra, show a "seals 3 wire segments" line in the
  strip; when a ladder would cross infra, show the reason "ladders can't cross
  wiring" and refuse.

### 4.3 Preview feedback loop

Every placement attempt produces exactly one of: commit (build-thunk VFX at
each cell, staggered), clamp (reason line + badge), or refuse (reason line +
a short deny sound + the outline breaks). No silent no-ops.

---

## 5. Overlays (F-key, single-body adaptation)

### 5.1 What we keep, drop and add

ONI's overlay row is Power / Temp / Light / Decor / Germs / O2 / Radiation /
Element / Room. For one duplicant with no germs, decor or rooms, the kept set
is Power / Temp / Light / Gas / Liquid plus Hazard / Climb / Vitals-halo.
`[DECISION]`

| Overlay | Key | Kept? | Question it answers here |
|---|---|---|---|
| Power routing | `F2` | `[ONI-REF]` kept | Which consumers are on this hub, and where does the wire actually run? |
| Temperature | `F3` | `[ONI-REF]` kept | Which way do I walk to get comfortable? |
| Light | `F4` | `[ONI-REF]` kept | Where can crops grow, and where is it too dark to see? |
| Gas / element | `F5` | `[ONI-REF]` kept (merged) | What's in this cell, and is it breathable? |
| Liquid | `F6` | `[ONI-REF]` kept | Where is water, and where is it about to flood me? |
| Hazard | `F7` | **added** `[DECISION]` | Poison, cold, fire, over-pressure — all "you will take damage here" in one view. Replaces the germs/decor slots with the thing that actually kills you. |
| Climb / reach | `F8` | **added** `[DECISION]` | What can I climb, jump to, or not cross? A platformer-native overlay ONI never needed. |
| Vitals (local) | `F9` | **added** `[DECISION]` | A soft halo around the dup showing the O2, temperature and pressure of the cell she's standing in — the one overlay that is about the body rather than the map. |
| Decor / Germs / Room / Radiation | — | dropped `[DECISION]` | No systems to show. Do not bind the keys; leave the gaps so DLC can fill them. |

One active overlay at a time (ONI's rule, kept). Pressing the active key
toggles it off; `Esc` closes it as the first step of the cancel chain. The
active key is lit in a small overlay strip at the top-left so it's obvious why
the world suddenly turned orange.

### 5.2 How overlays render (PPR reality)

`GridPass` is flat tint, no lighting, no GI, and its tint today is derived from
a small material palette `[PPR-FACT]`. So an overlay is a *re-encode* of the
cell data, not a shader effect. Tint, transparency and gameplay-lighting are
three independent channels — do not conflate them:

- **Bootstrap (zero shader change):** the translator writes a bucket id as the
  material — e.g. temperature quantised into 8 steps, hazard into a 4-step
  ramp. Coarse, but free, and enough to ship a first pass. The 8-bucket
  bootstrap requires a palette remap: the `grid_tiles.slang` palette is not an
  ordered ramp, so bucket indices must go through a remap table, not a direct
  cast. `[DECISION]`
- **Target (small shader change):** an optional per-cell packed-RGB tint word
  alongside the existing material word. Packed RGB is coloration only — it
  carries the overlay ramp value in the data; it is not a light. `[DECISION]`
- **Vapor / haze first:** gas, vapor and haze render as mask-alpha cut-out
  cards first; true additive blending is deferred (blend is rejected in
  `TrianglePass` today). `[DECISION]`
- **Gameplay lighting is a deferred system:** light propagation with occlusion
  and cross-chunk invalidation is not part of the MVP tile substrate. It
  invalidates presentation tiles independently of sim dirt when it lands.
  `[DECISION]`
- **Cache rule:** changing the overlay invalidates the presentation cache even
  when the sim is clean. `[DECISION]`
- **Never** draw overlays as screen-space tint over the world: they must
  respect zoom and pan and stay aligned to cells. `[DECISION]`

### 5.3 Overlay details worth specifying

- **Power routing overlay** draws connection lines along the *actual* polyline
  of each wire/duct, diagonals included, with small direction arrows and a
  distinct marker at the first consumer that stalls when the hub is over
  capacity. Clicking a consumer highlights its whole route back to the hub —
  this is the "power-view overlay routing" job `[OBSERVED]`. `ONI`'s wire-width
  colouring is the reference `[ONI-REF]`.
- **Hazard overlay** uses one hue family with different luminance steps plus a
  glyph in the tooltip; it must survive colour-blindness (§9).
- **Light overlay** shows a stepped lux ramp around each source. Glow ferns and
  goo light are first-class sources here, not afterthoughts `[INFERRED-PT]`;
  their gameplay-lighting role (safe farmable pockets, light-as-resource) is
  `[INFERRED-PT]` with propagation/occlusion deferred per §5.2.
- **Climb overlay** tints climbable faces (ladder, climbable wall) with a
  distinct edge marker and dims unreachable ledges — a small amount of data,
  a large amount of "I know where to go next".

---

## 6. HUD: vitals, bubbles, notifications

### 6.1 Vitals cluster (always on, bottom-left, over the world)

Single duplicant, so a fixed cluster of read-outs — no scrolling, no panel:

| Vital | Form | Behaviour |
|---|---|---|
| **O2** | Vertical bar + seconds remaining | Seconds, not just %: "1:12". Turns amber under 30 %, pulses under 10 %, and gains a "thin air" marker when the surrounding cell is below the breathable threshold — the bar drains anyway there `[OBSERVED]`. |
| **Stamina** | Row of pips, not a bar | Pips read "how many more swings" at a glance. Empty = exhausted: move and mine speed penalties are shown as a small icon, not a number. `[OBSERVED]` |
| **HP** | Thin bar under O2 | Only widens/animates when damaged. |
| **Bladder %** | Small numeric chip, optional | Off by default; if on, it never flashes — it just ticks. `[INFERRED-PT]` |
| **Body temperature** | Numeric with a comfort band | Displays a comfort band with notches `[INFERRED-PT]`; band limits (≈ −10…60 °C) are `[TUNING-UNKNOWN]` placeholders; the value sits outside the notch when you're taking damage. |
| **kcal** | Numeric chip | Turns red only at zero (starvation → HP). kcal-positive via farm/cook is MVP-required for loop closure (§10) `[DECISION]`. |

Above the cluster, a compact **inventory strip** (vacuum-gun contents, mass per
stack) and a **power chip** (battery %, hub load `84/240 W`) so cranking
decisions don't require opening anything.

### 6.2 Smoothed display values `[ONI-REF]`

Every number the player reads is exponentially smoothed before display —
roughly a 100–150 ms first-order lag, the same shape as the input filter the
engine already ships — so a bar breathes instead of vibrating at tick rate.
Rules:

- Display is smoothed; *logic* uses raw values. Thresholds, death and the
  "you are suffocating" state never use the smoothed number.
- Round what a player acts on (percent whole, seconds whole, watts to the
  nearest 5) and show exact values in the tooltip.
- Smooth in the same direction only: bars never overshoot then settle, because
  overshoot reads as "it went back up".

### 6.3 Status bubbles over the dup `[ONI-REF]`

A stack of at most three bubbles anchored above the duplicant, strictly ordered
by severity, each an icon plus at most one short line:

1. Damage / suffocation / on fire — persists until resolved.
2. Need — hungry, exhausted, bladder, too cold/hot.
3. Status — cranking, mining, climbing, "out of reach".

Bubbles are strictly severity-ordered (damage > need > status). The newest
bubble queues within its tier; it never reorders severity — older higher-tier
bubbles stay on top and newer lower-tier bubbles slide below. They pause their
fade while the sim is paused. `[DECISION]`

### 6.4 Diegetic notifications `[ONI-REF]`/`[OBSERVED]`

Not a toast list in a corner. An event announces itself *where it happened*:

- A world-anchored chip at the site ("Water reaching the lab", "Wire severed
  here") with a thin leader line, fading after a few seconds.
- One persistent ticker line at the top for things with no single location
  (cycle change, power brownout, research complete).
- Everything the player missed is retrievable from a small log in the pause
  menu — so nothing is lost, but nothing nags.

Event catalogue to design VFX for (each pairs a visual with a sound):
presence of the catalogue is `[OBSERVED]`; per-event sound specifics below are
`[INFERRED-PT]` with rates/pitches `[TUNING-UNKNOWN]` — see the proposed sound
column in §8.4. Catalogue: dig chips, vacuum stream + intake gulp, build
thunk, deconstruct puff, damage tick, suffocation gasp, cold breath, fire
ignition, pipe burst, vent hiss, electrolyzer hum shift, crank spark,
battery-full chime, harvest pop, ladder slide dust, landing thud, critter
flee.

---

## 7. Tooltips and information hierarchy

Four tiers, and never more than two visible at once.

| Tier | Where | Content |
|---|---|---|
| 0 — glance | HUD + bubbles | Am I safe? (vitals, active hazard icons) |
| 1 — point | World pips/bubbles | What is happening, and where? |
| 2 — hover | Tooltip on a cell, building, pipe, plant, critter | What exactly is this, and why is it like this? |
| 3 — open | Side panel / pause / tech tree | Depth: full tables, recipes, research costs |

### 7.1 Progressive hover tooltips `[ONI-REF]`

Onion layers — the first layer is always the same size, deeper layers only
appear when the player keeps hovering (or holds a modifier):

- **Layer 1 (instant):** name, one-line state. *Lab — running, 60 W.*
- **Layer 2 (~0.4 s):** the key numbers. *Power: 60 W of 240 W hub · Efficiency
  92 % · Contents: 12 kg water.*
- **Layer 3 (~1 s, or hold `Alt`):** contributors itemised, one row each, sorted
  by magnitude, with a small bar per row. Temperature is not additive, so
  contributors are heat-transfer rates / interval deltas per contributor —
  never additive °C. Example:

  *Cell heat balance, 41 °C (Δ +0.4 K/s over 5 s)* →
  `rock tile (left): +18 W (+0.3 K/s over interval)`
  `lab at (12,7): +6 W (+0.1 K/s over interval)`
  `ambient loss: −2 W (−0.0 K/s over interval)`

  Never `+38 °C conduction from rock tile (left)`: that form is wrong because
  temperature does not sum across contributors.

  *This pump: stalled* →
  `hub load 240/240 W`
  `pump demand 120 W`
  `battery 8 %`

Contributor rows are data-driven (a span of name/value/colour rows assembled by
the caller each frame), not hand-written strings per tooltip — the same
structure the demo's panel already uses for its view structs. `[PPR-FACT]`

### 7.2 Placement rules for tooltip text

- Numbers with units, always: `4.2 kg`, `120 W`, `−18 °C`, `1:12`.
- One verb per line, present tense: "Seals 3 wire segments", not
  "This footprint would cause sealing of…".
- Say *why* a placement fails in the strip, in words, next to the button —
  never only by changing an outline colour. The demo already does this; keep
  it as a hard rule. `[PPR-FACT]`
- Nothing essential lives in layer 3. If it matters at 1 % O2, it's in tier 0.

---

## 8. Art direction

### 8.1 Lineage: Klei hand-drawn, not ONI's lab panel

The target look is the store page's own tags: *stylized, hand-drawn, colourful*
`[OBSERVED]`. Klei's lineage (Don't Starve, Rotwood, ONI) shares a house style —
confident ink outlines, painted texture inside flat shapes, chunky readable
silhouettes, warm/cool colour blocking, character animation with real
squash-and-stretch.

Where we deliberately differ from ONI:

| ONI (leave behind) | Away Team (do this) |
|---|---|
| Diagram-first: clean UI chrome, science-poster palette, information-dense | Adventure-first: the world is the picture, chrome is small and warm |
| Even, cool lighting; a laboratory | Strong key colour per biome, with real darks — a place |
| Wide static views of a colony | Composed side-scroller framing: foreground layer, play layer, background layer |
| Flat UI panels in a neutral grey-blue | UI panels that borrow the world's ink-outline language: rounded, slightly irregular, hand-inked borders, paper/plate fills |

UI and world must share one outline weight and one corner language, otherwise
the HUD reads as a screenshot pasted on top of a drawing.

### 8.2 Layering and silhouette

- Three parallax layers minimum: distant biome backdrop (very low contrast),
  play layer (the simulated tiles), and a foreground silhouette strip that
  never covers the dup's lane. `[DECISION]`
- The duplicant is always the highest-contrast element on screen: brightest
  value, warmest accent, thickest outline. Everything else recedes at least one
  value step when she's on screen. `[DECISION]`
- Simulation reads through *material* cues before numbers: dripping for
  liquid, shimmer and rising motes for heat, slow curl for gas, crystalline
  bloom for cold, soot and flicker for fire. `[DECISION]`

### 8.3 Lighting under the PPR constraints `[PPR-FACT]`

Be honest about what exists: `GridPass` has **no lighting and no GI** — cells
come out as flat tints. `TrianglePass` has **one fixed directional key light**
(a hardcoded direction with a diffuse + specular term) plus **emissive
factors** on materials, and **alpha blending is rejected** — only opaque and
mask (cut-out) rendering is available. The only blended layer today is the
ImGui overlay. So:

| Effect | How |
|---|---|
| Ambient world light, glow ferns, goo light | **Baked per-cell tint (coloration only)** computed on the CPU when staging chunks (the target path in §5.2). Cheap, static-ish, refreshed only for dirty chunks. Not a light *system* — a light *value* written into the data. Glow ferns as safe farmable pockets and light-from-goo as a resource are `[INFERRED-PT]`; propagation with occlusion and cross-chunk invalidation is deferred per §5.2. `[DECISION]` |
| Character and building shading | Emissive + the fixed key light on `TrianglePass` quads; shading that must move (a rim on the dup near fire) is painted into the sprite or driven by tint. `[PPR-FACT]` |
| Soft glow, fire halo, vacuum beam, vapor | Blend is rejected in `TrianglePass` today, so vapor/haze/glow ships as **mask-alpha cut-out cards first** — crisp and Klei-ish rather than soft. A new additive pipeline variant is deferred until profiled need (renderer sign-off required). `[DECISION]` / `[GAP]` for the deferred additive variant |
| Fog of unexplored area | Not blur or darkness (no lighting to darken). Use an ink-hatched mask with the hand-drawn line language — which fits the style better than a gradient anyway. `[DECISION]` |

Glow-fern / goo gameplay lighting reads from **one light value** — a single
source of truth per cell, not three approximations — when the deferred lighting
pass lands.

### 8.4 VFX per event

Tile-aligned, shape-driven, brief — particles are drawn from the same palette
as the material they come from. The sound column is proposed-inferred:
per-event sounds are `[INFERRED-PT]` and all rates/pitches are
`[TUNING-UNKNOWN]`.

| Event | VFX | Sound `[INFERRED-PT]` |
|---|---|---|
| Dig swing | Radial chip burst, 6–10 shards, dust settle | Tick per swing, thud on break |
| Vacuum stream | Bezier suction cone into the muzzle, item shrinks along the path | Loop + intake gulp per stack |
| Gas leak / vent | Drifting plume with curl noise, tinted by element | Hiss, pitch by pressure |
| Liquid pour / splash | Sheet + droplets + ripple ring on landing | Splash, tail by volume |
| Fire | Flicker core, rising soot, heat shimmer above | Crack |
| Cold | Ground frost crawl, visible breath | Wind, crunch |
| Poison gas | Slow green haze with a wobbly edge | Cough cue when entered |
| Crank | Hand-wheel spin, spark at the brushes | Rhythm loop, pitch rises with charge |
| Build | Staggered thunk per cell, brief dust | Thunk |
| Power flow | Pulse travelling the wire polyline (diagonals!) | Hum |
| Harvest | Pop, a couple of loose sprites that fall and settle | Pop |
| Suffocation | Screen edge desaturates, gasp | Gasp |

All of it respects a "reduce VFX" toggle (§9) and dies with the event — no
particle is left looping after the cause stops.

---

## 9. Accessibility

| Area | Requirement |
|---|---|
| Remapping | Every action rebindable, stored as one flat table of action→key rows; the settings screen rebuilds its ImGui widgets from that table each frame (no retained widget system exists `[PPR-FACT]`, so the table *is* the UI). |
| One-hand / motor | Hold-to-toggle for vacuum and dig; auto-vault low ledges; jump assist (press once to jump, no hold); "always climb" mode that auto-mounts ladders. |
| Colour | All overlays are readable with deuteranopia/protanopia/tritanopia: luminance steps + glyph markers, never hue alone. A high-contrast palette option; an outline-only mode that strips texture but keeps shapes. |
| Text size | Independent UI scale slider on top of the existing DPI/content-scale handling `[PPR-FACT]`; min body text ≥ 16 px at 1080p. |
| Motion | Reduce/turn off screen shake, chromatic pulse on damage, parallax drift, particle density (three levels). Vitals pulse becomes a static colour change when pulses are off. |
| Hearing | Captions for every sound cue in the event table (§8.4), with direction where relevant ("hiss, right"). |
| Photosensitivity | No flashes above 3 Hz anywhere, including fire and lightning-like geyser events; the fire flicker stays low-amplitude. |
| Difficulty | Affects vitals drain rates and hazard DPS only — never the simulation rules, so a relaxed run still teaches the same physics. |
| Language | Numbers and units stay Latin-script and unpadded; labels localise, values do not. |

---

## 10. Onboarding for sim-first verbs

The player already knows jump, dig, build, collect. They do **not**
automatically know: gas sinks, heat conducts along a chain, fire needs oxygen,
thin air drains you, a sealed pocket is a trap. Onboarding has to teach
*physics as verbs*, in the order the player first needs them. A full MVP run
must additionally pass through research, farm/cook (kcal-positive), and
battery-to-hub power for loop closure
(see [AwayTeam-Gameplay-Inventory.md](AwayTeam-Gameplay-Inventory.md) §1–§2);
mission imperatives ship as a placeholder `[DECISION]`.

### 10.1 First ten minutes (scripted, skippable, diegetic)

| Beat | Player does | Is taught |
|---|---|---|
| 1 | Land, wake, look around | Camera: `C`, free-cam, pause `P` (the pod's display says it, not a modal) |
| 2 | First ladder + brick placed in pause | Build preview: anchor, shrink, stage, commit |
| 3 | Dig to loose debris, vacuum it | Tool cycle `1`–`5`, stamina pips |
| 4 | Start O2 pocket visibly shrinking | The O2 bar, and that gas has a *volume* |
| 5 | Dig through to a second pocket | Pressure equalisation — the ahh-moment, shown with a flow VFX, not a sentence |
| 6 | Crank to light the lab | Hand-wheel → battery → hub, power chip |
| 7 | Find a glow fern pocket `[INFERRED-PT]` | Light as a resource, `F4` |
| 8 | Green haze appears when a slime tile is cut | Hazard overlay `F7`, poison is a *place* |
| 9 | Plant and cook a first meal | Farm/cook progression; kcal-positive is MVP-required `[DECISION]` |
| 10 | Staff the lab, unlock a tier | Research progression `[DECISION]` |
| 11 | Sustain hub power, meet the placeholder quota | Battery-to-hub closure + imperative placeholder `[DECISION]` |

Each beat uses a **hint chip**: anchored to the relevant object, one line, one
key, dismisses itself as soon as the action is performed, and never comes back.
No modal tutorial windows, no wall of text. `[DECISION]`

### 10.2 Ongoing support

- **Verb glossary** in the pause menu: every sim term (pressure, conduction,
  phase change, brownout-lite) with a one-line plain-English rule and a button
  that centres the camera on a live example if one exists. `[DECISION]`
- **Causal failure screen.** Death/fail shows three lines of cause, newest
  first — `O₂ 0 % ← thin air 41 kPa ← you sealed the only vent` — plus a
  "show me" button that reopens the last seconds in free-cam. Never a stat
  dump. `[DECISION]`
- **Pause is always legal (single-player).** Nothing in the game punishes
  pausing to think; onboarding says so explicitly, because sim-first players
  otherwise assume they're meant to be fast. Co-op: host pauses, clients vote;
  only the host commits pause/resume. `[DECISION]`
- **Character select (Ren cameo)** `[OBSERVED]` skin present /
  `[INFERRED-PT]` cosmetic-only (no stat difference): one screen, cosmetic
  only, from title and pause. It's the first place the hand-drawn style gets
  to show a large character — treat it as the style's reference shot.

---

## 11. Where this lands in PPR (implementation map)

Kept short on purpose; the codemaps and AGENTS.md are authoritative.

| UX piece | Lands in | Constraint to respect |
|---|---|---|
| Panels, tooltips, strip, settings | Game-owned ImGui draw functions returning an intent + chrome-rect frame (the `drawColonyUi` pattern) `[PPR-FACT]` | ImGui only, no retained widget system; one intent per frame; chrome geometry returned so world feedback can clip around it |
| World feedback (outlines, preview, bubbles, world chips) | Foreground `ImDrawList` screen-space overlay, clipped to the world region `[PPR-FACT]` | Never draws over panels; skip on viewport/IO mismatch rather than misplace |
| Actions & pause gating | `InputAction` + mapping with the paused flag per action `[PPR-FACT]` | Tool/camera actions consume only while owning a gesture; background-drag latch keeps drags out of the camera |
| Camera states | Pan controller's follow target + ortho scale + camera-cut flag `[PPR-FACT]` | Row-major, row-vector, left-handed, `view * projection`, no backend transpose/flip |
| Overlay recoding | `ColonyTranslator` staging path into `GridPass` `[PPR-FACT]` | Flat tint, no lighting/GI; start with 8-step bucket ids through a palette remap (palette is not an ordered ramp), then an optional per-cell packed-RGB tint word (coloration only); overlay changes invalidate the presentation cache even when the sim is clean |
| Glow / additive VFX | Mask-alpha cut-out cards first | Blend is rejected today — additive variant deferred, renderer sign-off needed `[GAP]` |
| Smoothed read-outs | A small display-side filter in the view assembly (same first-order-lag shape as the input filter) `[PPR-FACT]` | Smoothing is display-only; logic reads raw |
| Controller / touch | Abstract actions now, layouts later | Touch has no input device at all `[GAP]` |

---

## 12. Open decisions

1. **Per-cell tint word in `GridPass`** — recommended (enables smooth overlay
   ramps as coloration-only data per §5.2 and §8.3), but it is a shader +
   translator change. Until it lands, overlays ship at 8-step buckets through
   the palette remap. `[GAP]`
2. **Speed control bind** — `[`/`]` vs. only the top bar; affects how often
   players change speed mid-platforming.
3. **Footprint commit model** — decided: staged-then-commit (§4.1) is
   canonical; commit-on-release is rejected. `[DECISION]`
4. **Additive pipeline for glow/beam/fire** — decided for v1: cut-out glow
   cards first; the blended variant is deferred until profiled need (see §8.3).
   `[DECISION]` / `[GAP]` for the deferred variant.
5. **Controller scope** — decided for v1: design-for only (see §2.3); nothing
   announced. Touch stays unsupported. `[DECISION]` / `[GAP]` for touch and
   pad tooltip inspect mode.

---

*End of AwayTeam-UX.md — one-file UX/art spec; no other writes.*
