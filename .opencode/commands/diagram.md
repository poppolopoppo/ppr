---
description: Generate focused PlantUML diagrams for code architecture, workflows, data, timelines, and reports using the toy theme.
---

The user's request is: `$ARGUMENTS`.

If the request is empty or whitespace-only, ask what to visualize and stop until
the user supplies a scope. Otherwise infer reasonable defaults; ask only when
missing information materially affects correctness, disclosure, or diagram choice.

Load `plantuml-skill` from `.opencode/skills/plantuml-skill` and the user-global
`verification-planning` skill before drawing. If `plantuml-skill` cannot load,
stop and explain. If verification-planning is unavailable or denied, use an
explicit proportionate verification plan through the orchestrator; if no
orchestrator is available, state and apply that plan directly. Report the
substitution.

Use the skill for PlantUML syntax and rendering guidance. This command overrides
its defaults for scope, theme, palette, SVG deliverables, backend selection,
retry limits, and publication safety. Never remove or change the toy theme as a
fallback.

## Scope: 1–8 Focused Diagrams

Produce the smallest useful set, not automatically eight diagrams. Each diagram
answers one question or explains one coherent aspect.

- Code: use the skill's `references/from-source-code.md`. Keep detailed views
  focused on one module/package. A dependency overview or cross-module sequence
  may span modules when those relationships are the subject.
- Reports and documents: split by question, process, dataset, or time period,
  not artificial code-module boundaries.
- Mixed requests: identify the evidence source separately for each view.

Choose the type that fits the content: component/class for structure, activity
for workflows, sequence for interactions, ER/data-flow for data, state for
lifecycles, and Gantt or another suitable supported type for timelines.
Do not invent dates, durations, quantities, or causal relationships to fill gaps.
Label illustrative scenarios, proposals, and assumptions explicitly.

Cap the batch at eight diagrams. If more are needed, ask which eight to prioritize
or propose follow-up batches. Do not silently omit requested scope or cram extra
views into unreadable diagrams.

Before drawing, make a compact inventory:
`NN | slug | question/type | evidence source | key checks`.
Reuse it for the gallery and final report.

## Verification: Proportionate and Source-Appropriate

Verify the material entities, relationships, ordering, values, and dates shown.
Use the smallest sufficient evidence:

- Code: relevant source and build declarations; use CodeGraph when available
  under repository policy. Cite `file:line`. Use runtime/debugger evidence only
  when the claim actually requires runtime observations.
- Reports/documents: cite the document and section/page, or URL and section.
- Data: identify the input, relevant rows/columns, units, time range, and any
  transformation used.
- User-described designs: identify the user specification as the source and
  label the result as proposed or illustrative, not independently observed.

Reuse shared evidence across diagrams. Do not repeat a full verification
workflow per diagram. Record one or a few representative claim→evidence checks
per view; these summarize verification rather than excuse unsupported content.
Mark uncertainty and omitted scope.

Content verification, SVG validity, and visual readability are separate checks.
A successful render does not prove the depicted facts.

## Mandatory Theme

Every `.puml` must contain `!theme toy` immediately after its opening directive.
Use matching delimiters for the selected type, such as `@startuml`/`@enduml` or
`@startgantt`/`@endgantt`. Never substitute another theme.

```plantuml
@startuml
!theme toy
title Example Architecture
' ... diagram content
@enduml
```

If the selected type/backend cannot render with toy, report the incompatibility
and propose a compatible representation; do not silently drop the theme.

## Color Coding Conventions

Use this palette consistently across the batch. Select only categories that
apply; not every diagram needs every color. Use labels/shapes as well as color,
and check contrast in the rendered result.

| Color | Hex | Usage |
|-------|-----|-------|
| Blue | `#6CB4EE` | entry points / actors / client |
| Green | `#77DD77` | application / services / processing |
| Orange | `#FFB347` | external systems / infrastructure |
| Red | `#FF6961` | errors / failure paths / critical |
| Yellow | `#FDFD96` | data stores / databases / queues |
| Purple | `#CF9FFF` | shared libs / utilities |
| Grey | `#B0B0B0` | legacy / deprecated / optional |

For non-code reports, use corresponding meanings such as participants, process
steps, external dependencies, risks, data, shared resources, and optional work.
Explain the mapping; do not repurpose colors as an unexplained quantitative scale.

Known-working forms with toy (historical probe: PlantUML `1.2026.6`, 2026-09-05):

Activity actions — post-semicolon stereotype:

```plantuml
:Receive request;<<#6CB4EE>>
:Process request;<<#77DD77>>
```

Declarations — trailing color:

```plantuml
actor "Client" as client #6CB4EE
participant "Processor" as processor #77DD77
component "Service" as service #77DD77
cloud "External System" as external #FFB347
database "Data" as data #FDFD96
component "Shared Utility" as shared #CF9FFF
rectangle "Optional Step" as optional #B0B0B0
```

Do not use `:Label #HEX;` or `#HEX:Label;` for activity fills, or
`<back:#HEX>` for legend swatches. These produced incorrect output in the probe.
For other diagram types, use their native coloring syntax and inspect the result;
do not assume declaration or activity syntax is universal.

When three or more semantic colors are used, include a legend covering every
used category with visual swatches:

```plantuml
legend
<font color="#6CB4EE">■</font> participants
<font color="#77DD77">■</font> processing
<font color="#FFB347">■</font> external dependencies
endlegend
```

Check color application during normal visual inspection. Run a small focused
probe only for an unfamiliar syntax/type, a known backend-version change, or
unexpected rendering—not once per diagram. Never claim the historical probe
version is the current backend version.

## Output and Rerun Safety

Publish under `docs/diagrams/<topic>/`:

- `NN-slug.puml` and `NN-slug.svg` per diagram; `NN` is `01`–`08`.
- `index.html` with a heading, concise description, evidence/assumptions,
  validation status, and relative source/image links for each diagram.
- Display each SVG with a relative `<img src="NN-slug.svg">`, meaningful alt
  text, and responsive sizing. Use embedded CSS with a dark gallery palette.
  Do not fetch scripts, styles, fonts, or images from the network.

Keep stems and anchors stable during refinement. Escape HTML text and attributes;
treat user/report labels as data, not HTML or executable PlantUML directives.
Do not introduce remote includes or externally loaded assets.

An explicit topic takes precedence over an inferred topic, but must still pass
sanitization. Topic components may contain lowercase ASCII letters, digits, and
single dashes, separated by single dots (for example `engine.app`). Slugs use
lowercase ASCII letters/digits separated by single dashes. Remove or replace
other characters; trim separators. Use `diagram` if empty, and prefix Windows
reserved device names with `diagram-`.

Ensure all resolved output paths remain under `docs/diagrams/`; reject symlink
or junction traversal outside that directory. If sanitization collides with an
existing unrelated topic, ask before overwriting it. Quote filesystem arguments;
never interpolate raw request text into shell commands.

Stage new renders in an approved scratch directory. Never purge the topic
directory. Preserve existing deliverables until the replacement batch validates.
Replace only the intended output files and publish the gallery last. Preserve
unrelated files; remove obsolete generated pairs only with explicit approval.
Repeated successful runs must not append duplicate entries or versioned copies.

Source links should be relative where possible. Show `file:line` as citation
text; do not promise local HTML links will navigate to a source line. External
evidence links may be optional navigation, but the gallery must display offline.

## Rendering Workflow

1. **Shared setup, once per batch.**
   Check renderer tooling and choose one backend:
   explicit user choice, otherwise nonempty `KROKI_URL`, otherwise reachable
   `http://localhost:8000`, otherwise consider `https://kroki.io`.
   An explicitly selected backend failing is not permission to switch.

   Before sending source, disclose the destination. For sensitive or uncertain
   content, require explicit approval before using any non-local backend,
   including a remote `KROKI_URL`. If approval is absent, stay local or stop with
   source-only output. Never silently fall back to a public endpoint or follow
   a redirect that changes the approved destination. Use bounded connection and
   request timeouts. Do not install or launch services without authorization.

2. **Generate.**
   Write the planned `.puml` files with matching delimiters, toy, the palette,
   and evidence-grounded content. Share source discovery across views.

3. **Export and structurally validate.**
   POST each file to `{backend}/plantuml/svg`, capturing HTTP status, response
   headers, body, and transport errors separately. Success requires:
   - successful transport and HTTP `200`;
   - media type `image/svg+xml` (allow parameters such as charset);
   - nonempty, well-formed XML whose root is SVG in the SVG namespace.

   Allow an XML declaration, BOM, comments, and surrounding whitespace.
   Do not impose an arbitrary minimum file size. Parse without resolving
   external entities or fetching DTDs. Reject HTML/text error bodies and rendered
   PlantUML error diagnostics; an SVG-shaped error is not a successful diagram.

4. **Correct and visually inspect.**
   Read syntax-error responses and fix the relevant source. Retry transient
   transport/rate-limit/server failures with bounded backoff; do not retry
   authentication or configuration failures blindly.

   Allow at most three export attempts per diagram in this batch, including
   syntax fixes, transient retries, and visual corrections. Inspect the rendered
   image for clipped labels, overlap, flow direction, edge clarity, applied
   colors, contrast, and error messages. XML inspection alone is not a visual
   check. If SVG cannot be viewed directly, use a locally produced raster preview
   where available; it does not replace the SVG deliverable.

   Simplify syntax or layout without changing meaning or removing toy.
   Do not silently remove relationships, notes carrying facts, or requested scope.
   If visual inspection is unavailable, say so rather than claiming readability.
   Stop after the attempt budget and report remaining problems.

5. **Publish or report incomplete output.**
   After all planned SVGs pass structural validation, publish the batch and
   offline gallery, carrying any visual-review limitations explicitly.
   If any SVG fails, preserve the previous gallery and report staged sources,
   successful renders, and failures. Publish a partial gallery only if the user
   requests it, with missing views clearly identified.

   A local renderer is an alternative only when available and explicitly selected
   after a Kroki failure; apply equivalent SVG and visual checks and report it.
   PNG-only output is incomplete for this command, never disguised as SVG success.

6. **Refine.**
   Apply minimal edits to affected diagrams, revalidate their outputs, and update
   their gallery entries. Do not regenerate unaffected views or restart discovery
   without a reason.

## Durability

Successful deliverables belong under versioned `docs/diagrams/`, not scratch
storage. Do not stage or commit automatically. Keep diagnostics and intermediate
renders in approved scratch storage; do not migrate unrelated pilot output.
Update repository navigation only when a genuinely new documented location
requires it.

Review sources, SVG metadata, and gallery text for sensitive content before
publication: local rendering alone does not make an artifact safe to commit.

## Step-8 Report

Report:

- Output/gallery path and requested N; structurally valid SVG count out of N.
- Which artifacts were published versus left in scratch.
- Per-diagram claim→evidence summary and separate structural/visual check status.
- Backend used and whether source left the machine; report version only if known.
- Assumptions, omitted scope, failed renders, and unperformed checks.
- Any verification substitution or focused color-probe result.

Never report N-of-N completion when outputs or required checks are missing.
