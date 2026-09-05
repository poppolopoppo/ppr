---
description: Generate PlantUML diagrams using the toy theme with consistent color-coded boxes.
---

Load the `plantuml-skill` skill (repo path `.opencode/skills/plantuml-skill`) and the `verification-planning` skill (user-global) and follow their workflows to generate the diagrams described by the user. The user's diagram description is: `$ARGUMENTS`. If `$ARGUMENTS` is empty or whitespace-only, ask the user for clarification — what diagram type, scope, and key elements to include — and do not proceed until provided.

If `plantuml-skill` is unavailable (not installed or fails to load), stop and tell the user — never draw without it. If `verification-planning` is denied or unavailable, route verification through the orchestrator as a substitute (proportionate claims + `file:line` evidence as above) and record the substitution in the Step-8 report — never proceed silently unverified. Skill names/paths are pinned: `plantuml-skill` = `.opencode/skills/plantuml-skill`, `verification-planning` = user-global.

## Decomposition (N Focused Diagrams)

Split `$ARGUMENTS` into N focused diagrams — one module/aspect per diagram — per the one-module-per-diagram rule in `.opencode/skills/plantuml-skill/references/from-source-code.md` (scope large inputs: split rather than cram). Each diagram gets its own `.puml`/`.svg` pair (see Output Layout). N is capped at 5 per query — if more are needed, split across runs or ask the user which 5 to draw first.

## Verification (Proportionate, First)

Load `verification-planning` BEFORE drawing. Keep it proportionate: 1 factual claim per diagram, each backed by 1–2 codegraph/CLion `file:line` refs — never run a full 6-step verification per diagram. Record each claim→evidence pair, then close the path post-render with an SVG check (claim→evidence→SVG check, reported in Step 8).

## Mandatory Theme Override

EVERY `.puml` file MUST start with `!theme toy` immediately after `@startuml`. This is mandatory — never use `!theme plain`, `!theme cerulean`, `!theme blueprint`, `!theme aws-orange`, `!theme vibrant`, or any other theme. The toy theme is the only allowed theme for this command.

Example header:

```plantuml
@startuml
!theme toy
title Example Architecture
' ... rest of diagram
@enduml
```

## Color Coding Conventions

All diagrams use a consistent palette on top of the `toy` theme. Apply inline `#HexColor` on elements to encode semantics. Colors are chosen to remain accessible and distinct under the toy theme.

| Color | Hex | Usage |
|-------|-----|-------|
| Blue | `#6CB4EE` | entry points / actors / client |
| Green | `#77DD77` | application / services / processing |
| Orange | `#FFB347` | external systems / infrastructure |
| Red | `#FF6961` | errors / failure paths / critical |
| Yellow | `#FDFD96` | data stores / databases / queues |
| Purple | `#CF9FFF` | shared libs / utilities |
| Grey | `#B0B0B0` | legacy / deprecated / optional |

Usage — per-kind WINNER forms only (empirically pinned by Kroki probe 2026-09-05, server `1.2026.6`, `!theme toy`; re-probe if the backend version changes):

Activity actions — post-semicolon stereotype (the ONLY form that fills the box):

```plantuml
:Application::run entry;<<#6CB4EE>>
:initialize - platform init;<<#77DD77>>
```

Declarations (sequence participants, actors, components, databases, clouds) — trailing `#HEX`:

```plantuml
actor "Client" as client #6CB4EE
participant "P" as p #6CB4EE
component "User Service" as svc #77DD77
cloud "Payment Gateway" as pay #FFB347
database "DB" as db #FDFD96
component "Auth Lib" as auth #CF9FFF
rectangle "Legacy Batch" as legacy #B0B0B0
```

Legend swatches — font-colored `■` glyph (renders as a visual square):

```plantuml
legend
<font color="#6CB4EE">■</font> entry points / actors
<font color="#77DD77">■</font> application / services
endlegend
```

(`<color:#HEX>■</color>` renders byte-identically and is an accepted alias; prefer `<font color>` as canonical.)

BANNED forms (probe-proven broken):

- `:Label #HEX;` — suffix inside the action label: box stays gray (`#F1F1F1`) and the hex leaks as literal text (probe `probe-a1-activity-suffix.svg`: `<rect fill="#F1F1F1">` + `<text>Test A #6CB4EE</text>`). This was the Round-1 bug in `01-application-lifecycle`.
- `#HEX:Label;` — prefix: deprecated; gray box plus a rendered deprecation banner (probe `probe-a3-activity-prefix.svg`).
- `<back:#HEX>..</back>` for legend swatches: renders an `feFlood` filter wash, not a crisp square (probe `probe-c3-legend-back.svg`).

Correction 2026-09-05: the pre-probe hypothesis blamed `:Label;<<#HEX>>` for the gray+literal-text bug; the probe refutes it — `:Test B;<<#6CB4EE>>` yields `<rect fill="#6CB4EE">` with clean `<text>Test B</text>` (probe `probe-a2-activity-control.svg`). Pin the winners above, not the hypothesis.

Apply colors consistently — the same semantic category always uses the same hex across all diagrams.

Legend behavior: if a diagram uses 3 or more color types, include a small `legend` block listing each used color as a VISUAL swatch (`<font color="#HEX">■</font>`) followed by its meaning label — never bare hex text (e.g. never write `#6CB4EE = entry`). Cover every color type used in that diagram.

## Output Layout

Write deliverables under `docs/diagrams/<topic>/`:

- One `NN-slug.puml` + `NN-slug.svg` pair per diagram (`NN` = zero-padded index `01`–`05`, `slug` = short kebab-case aspect name).
- One `index.html` gallery: dark theme via an EMBEDDED `<style>` dark palette (no external CSS fetch — must render over `file://` offline). Per-diagram section = `<h2>` heading + structured `<ul>` bullets (what it shows; flow summary; verification evidence with `file:line` refs) + hyperlinks to local files: relative `<a href="NN-slug.svg">` and `<a href="NN-slug.puml">` plus evidence source links (e.g. `<a href="../../../lib/engine/app/...">`) where relevant.
- Reuse the same stable `NN-slug` stem across the `.puml`/`.svg` files and the gallery anchors.

Sanitize `<topic>` and each `slug`: lowercase alphanumeric + dashes, plus single `.` allowed in `<topic>` (e.g. `engine.app`); strip path traversal (`..` sequences, `/`, `\`, `:`); empty result falls back to `diagram`. Derive `<topic>` from the query nouns in kebab-case; fall back to `diagram` when no nouns apply; an explicit user-provided topic always wins. Reruns are idempotent: re-running the same `<topic>` purges that topic directory first and regenerates it; never append duplicate `NN-slug` pairs, never touch other topic directories.

## Workflow

1. Follow `plantuml-skill` Steps 1-3 per diagram: check dependencies (`curl --version`), pick the best diagram type for that diagram's aspect of `$ARGUMENTS`, and write `docs/diagrams/<topic>/NN-slug.puml` with `@startuml`/`@enduml` and the mandatory `!theme toy` header plus color-coded elements from the table above.
2. Export each `.puml` via Kroki per Step 4 — POST the file to `{kroki}/plantuml/svg`, where `{kroki}` resolves by precedence: `$KROKI_URL` if set, else local `http://localhost:8000`, else public `https://kroki.io`. For sensitive diagrams prefer local Kroki and warn before uploading to public kroki.io.
3. Validate and self-correct per Step 5 — treat each export as failed unless ALL hold: HTTP `200`, content-type `image/svg+xml`, body starts with `<svg`, size > 1KB, body ends with `</svg>`. On `400`, `cat` the output file for Kroki's error line, fix the flagged `.puml` line, and re-render. Retry up to 3 times with backoff, then follow the skill's 6-step degradation ladder (SVG → PNG fallback → local/error note) before surfacing the raw error — never fail silently.
4. Self-check per Step 6 — read each rendered SVG for label truncation, overlap, orientation, edge spaghetti, wrong type, or low contrast; fix and re-render/re-validate (max 2 rounds per diagram).
5. After Steps 5-6 pass for all N diagrams, write `docs/diagrams/<topic>/index.html` embedding/linking every SVG with its description, then follow Steps 7-8: show the gallery, handle refinement requests with minimal `.puml` edits, and report per Step 8 below.

## Durability

`docs/diagrams/` is versioned deliverable output (commit-tracked). Scratch/intermediate renders stay in `.slim/tmp/` per AGENTS.md temp rules. Greenfield only — do NOT migrate `.slim/tmp/diagrams*/` pilots into `docs/diagrams/`. `docs/` is a new top-level directory: leave a codemap note for it (pointer in the root `codemap.md`) when first created.

## Step-8 Report

Report: `docs/diagrams/<topic>/` path, N-of-N rendered counts (`.puml` + `.svg` + gallery), per-diagram claim→evidence→SVG check lines, backend used per diagram, whether source left the machine, plus any verification substitution (orchestrator-routed) and the probe backend/version when colors were (re-)pinned.
