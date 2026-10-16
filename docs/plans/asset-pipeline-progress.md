# Asset pipeline implementation progress

Plan: [Scoped asset pipeline](asset-pipeline-plan.md).

Updated: 2026-10-16. Implementation is in progress; the full plan is not delivered.

## Current atomic migration

The shared preparation checkpoint is committed as `71d3f2e`, using
`std::future<Outcome>` with owner-held cumulative diagnostics and explicit
capture settlement. The production image factory compiled after a separately
verified inline thin-factory workaround; outcome transport alone did not resolve
the original MSVC ICE. Focused common/image/core checks and source review passed;
the execution manifest remains the authority for exact tested leaves and exits.

The current uncommitted batch switches complete owned sources, selected numeric
catalogs, presentation, authoring/save, and their consumers together. Authoring,
formatting/style checks and independent source review passed. The current Dev
configure passed; the first builds exposed ordinary explicit-conversion and
unused/shadowed-variable errors. Five repaired files passed source review and
scoped formatting/style checks. The next retry encountered MSVC C1001 in
`SimFramePacket.cpp:150` (`function-signature.cpp:213`, IFC import detected),
so affected production edits and builds stopped under `BLOCKED_PLAN`. Evidence
is retained in `atomic-migration/build-retry2.log` and its result JSON. Two
isolated full-translation-unit probes also reproduced the ICE: adding only a
direct `engine.asset` import, and moving the identical short-circuit predicate
into a local boolean before the unchanged error macro. The latter reports the
ICE at the predicate itself. A third sequential, null-safe probe localizes the
first triggering statement to `world->presentation()`, before catalog or
generation operations. A coherent baseline/candidate probe rebuilt copied
WorldGen, Colony, Driver and packet interfaces together. Both branches compiled
their interfaces and WorldGen implementation, then reproduced the same consumer
ICE; defining the getter inline is therefore rejected as a remedy. Exact source,
map, compiler and IFC evidence is in `repro/graph-ab-results.json`. A tiny
implementation-unit probe calling only the accessor also reproduces the ICE,
as does the analogous existing `catalogOwner()` getter. Minimal companion
owners with the actual presentation payload and with an empty cross-module
payload both compile. The original PreparedWorld/module context remains
necessary; this is not evidence of a universal shared-pointer-reference defect.
The coherent borrowed-`safe_ptr` view candidate also reproduces the ICE and is
rejected. A subsequent class-context reduction removes only the two friend
factory declarations: all four interfaces and the same tiny getter caller now
compile. This isolates those declarations as a necessary trigger in this exact
compiler configuration, without establishing invalid C++. The private
friend-class construction bridge then passed all six coherent compile stages,
including the real factory implementation and original packet consumer.
Evidence is `repro/private-construction-bridge/compile-results.json`. The
coordinator approved integrating that exact reviewed two-file remedy; public
factories, validation, ownership, private construction and member layout remain
unchanged. This closes the affected-edit design blocker, not the production
build or runtime acceptance gates.

The failing baseline is preserved in `blocked-snapshot/blocked-snapshot.json`:
216 hash-verified source/evidence/IFC files, including 127 unique IFCs. Approved
image reuse implementation proceeds independently against that preserved
diagnostic baseline. Runtime tests remain pending; reuse has not passed its
implementation or execution gates. The next normal build waits for its coherent
implementation handoff. A newer VS18 Insiders channel build is advertised, but
the exact bridge proof passed on the installed compiler. No compiler update has
been performed or proven necessary.

The normal configure passed on the installed compiler. The five-target build
compiled both actual packet consumers, confirming the private bridge at its
original failure boundary, then encountered a separate C1001 in unchanged
`ColonySmoke.cpp:108` at `parallel::backgroundPool()`. That app executable remains
blocked. The verified independent four-test-target closure excludes `app.game`;
its next build exposed ordinary namespace/import/CRT errors being repaired.
No executable linked successfully in these attempts and no new runtime tests
have run. The failure manifests retain source and pre-existing binary identities.
The subsequent four-target retry passed and linked the current app/game test
executables. Focused runtime validation is now authorized against recorded new
binary identities; the separate `app.game` smoke compile blocker remains open.
The first focused common preparation run passed 11 cases. Image preparation
passed nine of eleven requested leaves; two new fixtures incorrectly queried
progress after consuming the request. A reviewed test-only correction snapshots
progress before `take()`, preserving work, drain and budget assertions, and moves
zero-admission checks before consumption as well. After the affected asset
target rebuilt, all eleven requested image-preparation leaves passed. Printed
recursive group nodes are excluded from leaf counts and timing ranks; an earlier
derived count incorrectly classified the `asset/image` group as an extra leaf.
Production behavior was not changed for this repair. Source ownership (8),
descriptor imports (8), sim asset bindings (4), and all five approved numeric
golden/reference leaves passed. CLI checks passed 22 of 23; explicit pack-set
root loading failed with `schema_invalid`. The private selection schema left
`packs` defaulted while requiring at least one item, so its empty default failed
schema validation before authored entries loaded. The bounded repair marks that
collection required and adds missing/empty-table rejection cases. Formatting and
author review passed, the affected `game.tests` build passed (7.551 seconds), and
all 23 CLI leaves passed on rerun (1.290 seconds). These are execution observations,
not a performance comparison. The pack-content group then passed 26 of 28 leaves
(45.828 seconds); diagnostic-location and diagnostic-count assertions failed in
two error-path fixtures. Review found that the shared collector returns on the
first ordinary error before merging later settled outcomes' diagnostics. A
bounded repair will merge all settled diagnostics in request order before
selecting the first ordinary error, retaining fatal-exception precedence and
draining. The fixtures must also derive numeric TextureId order rather than
assuming lexical filenames. These failures are retained and do not count as a
passed suite. All ten world-assembly leaves passed (16.266 seconds). Checkpoint
validation passed six of ten leaves (4.165 seconds); four restore cases fail
`Colony::install` validation. Source review found its remaining comparison with
the old flattened catalog schema version instead of `asset::Manifest::schema_version`.
The bounded correction is authorized, retaining all other install guards.
The failure evidence is retained; the restore rerun remains pending.
Preview validation passed one of three leaves. Bounded sample selection and a
whole-world export-rejection assertion failed; the latter leaf completed after
179.22 seconds. These timings are observations from single executions. A bounded
PIX attachment to that already-running workload failed with `PIXTOOL18 - Process
terminated` after seven seconds; no valid timing capture or performance conclusion
resulted. Source review found invalid MatterFill heights in the custom-grid
fixture and two production writers attempting exclusive creation of `biome.ppm`.
The bounded fixes retain grid validation and destination-exclusive output while
assigning that exact image one writer. The coordinator approved a required,
synchronous borrowed biome-lookup overload for the existing region writer:
uniform samples delegate with a constant lookup, while whole-world crops borrow
the existing plan/layout to resolve each global cell. IDs are validated before
color lookup; the callback is never retained. Their execution gates remain pending.
Broader acceptance remains pending.

Two tiny Smoke implementation-unit probes calling only `backgroundPool()`
compiled successfully with both the original global-module headers and with
those headers removed. This does not establish a header defect or repair the
full translation unit. One next bounded probe preserves the original inputs and
exact following `prepareWorld` call to investigate the necessary surrounding
context. That prefix probe also compiled successfully (1.437 seconds), leaving
the full function/translation-unit interaction unresolved. No production Smoke
workaround is authorized by those observations.

The independent data audit of the shipped format conversion passed: the manifest
declares exactly 513 descriptor files, all declared files exist, the seven legacy
category documents are removed, mapped values/order agree with the frozen
legacy sources, and all 134 PNG hashes are unchanged. The manifest is 62,516
bytes and total manifest/descriptor text is 319,782 bytes. Evidence is
`out/validation/20261016/asset-pipeline/atomic-migration/audit-result.json`.
This establishes data conversion preservation, not runtime/compiler/numeric
parity; those gates remain pending.

## Corrected asynchronous boundary and resume

The user stopped the texture-specific asynchronous implementation and requested
an independent Astra assessment. That assessment found the objection justified:
generation/context reuse alone did not deliver the intended shared asset API.
The user has now explicitly resumed implementation with the corrected approach.
This correction supersedes earlier entries assigning the complete request
lifecycle to image.

`engine.asset` owns the typed shared preparation request, bounded admission,
poll/cancel/take/finish, owned context/snapshot/generation, ordered outcomes,
capture settlement, fatal-exception precedence, and draining/teardown. Image
supplies typed worker stages and owner-side continuations for source processing
and exact image budget decisions. Content composes the resulting presentation
sidecar and activation identities. A flat batch with per-item continuations does
not introduce the deferred universal dependency graph or payload store.

Retain committed foundations (`42b7be5`, `47a4b1a`, `a56a804`, `ea1c385`) and
useful uncommitted domain/source/processing work. Rework the uncommitted
texture-owned lifecycle into the common request instead of rolling back unrelated
work. The stopped image phase was unformatted, unbuilt, and untested; its
authored tests do not establish correctness. Non-image processors and move-only
owning results must verify the shared API before live-loader integration.

The shared request uses the existing scheduler's `std::future` for each worker
stage. It coordinates the larger batch around those futures: bounded occupied
slots, per-item continuations, cancellation, ordered diagnostics, fatal-error
priority and request-local capture settlement. An aggregate future alone would
still need that coordinator. Independent review found this shared owner
proportionate, while identifying direct future-carried outcomes as a possible
local transport simplification, not a required architectural change.

Review exposed an existing scheduler exception-specification defect:
`TaskPromise_` construction cannot be `noexcept` based only on the callable move,
because constructing its `std::promise` may allocate. The bounded correction lets
admission failure reach the request's drain/report boundary. Core config now
owns the local-key length constant used by both key validation and qualified
reference limits. These source changes and the new shared/image fixtures still
require the current batch's execution gate; review alone is not a test result.

The next migration will import complete owned authoring snapshots through a
synchronous `DescriptorReader` returning `Expected<DescriptorSource>` by value.
Disk convenience overloads delegate through the same typed importing path.
Candidate manifests build the checked index first; callbacks receive validated
indexed assets and their returned paths must match indexed provenance. Missing
declared records fail explicitly even if a disk file exists. Deleted records
are absent from the candidate manifest, and empty bytes are decoder input rather
than an absence sentinel. Reader/snapshot observers never escape synchronous
import. This seam is now implemented and independently reviewed in the current
atomic migration; its runtime acceptance checks remain pending.

## Shared preparation execution blocker

Scoped formatting/style and independent source review passed. The current
`msvc-dev` configure passed with captured exit zero. The first asset build
revealed ordinary fixture logging/move-only declaration errors, which were
corrected. Its retry compiled the common module and non-image request fixture,
but MSVC 19.51.36257 (14.51.36231) reported C1001 in
`Image.Preparation.cpp:445`, compiler `function-signature.cpp:213`, with an IFC
import note. Both failed builds and their exit-two records are retained under
`out/validation/20261016/asset-pipeline/shared-preparation/`.

Affected production edits and builds stopped under `BLOCKED_PLAN`; no substitute
lifecycle or speculative production workaround was attempted. A separate image
fixture's const-capture move constraint was corrected without weakening scheduler
constraints. The coordinator authorized one compile-only copied-source probe,
with separate object/PDB outputs and the original flags/import map, to separate
factory signature/import handling from `start` instantiation. Further probes or
production remedies require an evidence-led planning decision. Correctness tests
have not run, and this phase is not complete.

All three authorized copied-TU probes reproduced C1001 with the same IFC-import
note: a stubbed factory body, a renamed implementation-only factory retaining
the original body, and original-name trailing-return spelling. The `start` call
and matching imported declaration are therefore not necessary triggers; syntax
alone did not avoid the failure. These results do not prove an invalid language
construct or justify changing the ownership/module boundary. Source-shape probes
stopped after that finite set. Independent core validation may proceed; image
integration still needs a compiler/reproducer decision. No correctness or
performance claim is established for the blocked batch.

The user resumed execution and suggested future-carried outcomes as a possible
simplification. Independent design review approved evaluating
`std::future<Outcome>`: workers return fresh owned step diagnostics/errors/
exceptions; the request retains cumulative per-item outcomes exclusively on the
owner. Admission must never transfer cumulative diagnostics into a callable
that a full queue can reject. Collection retains exception/error state before
any allocating diagnostic merge. Outcome moves/destruction must be nonthrowing,
and all existing fatal-priority and capture-settlement guarantees remain.
This is not yet evidence that the MSVC ICE is resolved.

Independent core closure now passed with captured exits: the core target build,
the new WaitGroup readiness/capture leaf, and two existing config-key grammar
leaves. A guessed initial filter matched no tests and is retained as a failed
selection attempt; it is not counted as a test pass. Image/common runtime checks
remain unrun. Unexpected factory-signature edits made while work was stopped are
preserved separately from the recorded original compiler baseline.

The finite companion comparison compiled the full copied request and image
processor in both baseline and outcome variants: all six interface/implementation
stages passed. Baseline success means the companion context did not reproduce
the original C1001. Candidate success establishes transport/template and
nonthrowing-move compilation only, not an ICE remedy or runtime correctness.
Ordinary response-map and copied-namespace setup failures are retained in
`repro/companion-results.json`, alongside final source/artifact hashes.

The coordinator accepted integrating the reviewed outcome transport on its
design merits, plus first-fatal preservation and a public-API regression.
Only common request/helper/test edits resume; external image experiments remain
untouched. Original-module compilation and lifecycle/image tests remain gates.

The actual target retry with outcome transport and the preserved const-reference
image signature still failed C1001 at `Image.Preparation.cpp:460`, with the same
compiler signature/IFC note. Source and old executable/PDB hashes were stable;
linking and asset tests did not run. Outcome transport is therefore not an
established remedy. Evidence: `shared-preparation/integration/manifest.json`.

After this result, independent design review approved one interface-only probe:
copy the actual image partition, preserve the active signature/body, and place
the thin factory definition inline at its declaration. This fits the module
contract and tests declaration-time definition versus imported-signature
reconstruction without changing ownership, payload visibility or the common API.
Production image files remain untouched. Interface-only success would still
require a coherent module graph and full target/runtime validation.

The inline partition, copied primary umbrella and factory-removed copied
implementation all passed against a consistent candidate IFC graph. Independent
review accepted the thin inline factory as a same-contract compiler workaround,
without claiming a diagnosed compiler defect. Surgical integration preserved the
active const-reference API/forwarding semantics and disabled experiment tokens;
full pre-change image files are retained in
`shared-preparation/integration/pre-inline-user-snapshot/`. The two-file formatted
source review passed. Actual target linking and runtime validation remain pending.

## Shared preparation checkpoint

The real `engine.tests.asset` target built and linked after the inline relocation,
with captured exit zero; `game.tests` also rebuilt successfully. One-loop focused
checks passed: 11 common preparation leaves, seven image preparation leaves,
nine pack leaves, two selected mip-domain leaves, eight requested descriptor
leaves and nine World assembly leaves. Prefix-matched extras and group nodes are
distinguished in the manifest rather than counted as additional requested tests.
The earlier independent core build and three focused leaves also passed.
Formatted-source review is clear for the common transport/helper/tests and image
factory relocation. All compiled source hashes matched the validation manifest
at the coordinator's checkpoint verification.

The current module graph is unblocked; this does not establish a diagnosed
compiler cause or a performance improvement. Historical failures and the
companion experiment's non-reproduction remain part of the evidence. The active
factory signature/semantics and disabled user experiments are retained, with
pre-inline snapshots available for comparison.

Next is the coherent live-loader, dynamic authoring, presentation and shipped
format migration. Complete snapshots use the required synchronous descriptor
reader without disk fallback. New authoring targets use a fixed injective
kind-directory mapping and `asset-<local_key>.toml`; existing contained paths
retain their spelling. Canonical existing-path indexing, hard-link candidate
checks and exceptional network-parent equivalence preserve save ownership;
missing targets are validated separately from operational metadata failures.
Final interactive/asset acceptance, IDE inspection, representative timing and
documentation/skill closure remain pending.

## Atomic consumer and format migration in progress

Checkpoint `71d3f2e` is the passing shared-preparation baseline. The next batch
adds synchronous complete-snapshot reading, an immutable source/index owner,
dynamic source/history/save state, selected-world recipe projection and joined
presentation readiness before switching all consumers together. Early reader
and source-ownership review found bounded fixture/provenance, network-alias and
repeated-manifest-import issues; their corrections remain part of this unbuilt
batch. Partial-save state uses a separate identity-bound disk ledger, global
phase barriers and tombstone-backed undo restoration; obsolete orphan cleanup
does not block reusable files still referenced by the desired candidate.

The old shipped text is retained under
`out/validation/20261016/asset-pipeline/shipped-source-baseline/`, with ordered
semantic data and hashes for all 134 referenced texture inputs. No extra legacy
runtime benchmark was run. The recorded shipped recipe leaf took 6.86 seconds
in the existing successful run; this is an observation, not a controlled
comparison.

The one-shot port produced 513 descriptors and 319,782 total text bytes.
Its first all-quoted manifest preflight exceeded the cap at 66,626 bytes and
stopped before writing. Approved bare valid TOML keys reduced it to 62,516 bytes
without changing parsed keys/values/order, schema, identities or the 64 KiB cap.
Ordered semantic roundtrips preceded the write; PNG files were untouched.
Old aggregate documents were removed only after all new descriptor files were
written. Consumer conversion is still underway, so no buildable/checkpoint or
runtime parity claim is made for this transitional state.

## Team and execution

- Coordinator: design decisions and acceptance.
- GPT-6.1 Sol, high effort: sole production/test implementation owner.
- GPT-6 Luna, medium effort: exploration, formatting, serialized builds/tests and profiling.
- GPT-6 Astra, high effort: independent read-only correctness review.

Existing user changes in `assets/codemap.md`, `cmake/StageAssets.cmake`,
`codemap.md`, and unrelated untracked content are preserved. No UI control is
used. New work does not resume the canceled oversized PIX trace.

## Decisions during implementation

1. The config tree codec rejects TOML arrays. Manifests therefore use
   `[assets.<local_key>]` tables with `kind`/`descriptor`, and
   `[requires.<alias>]` tables with `pack_id`. This reuses the existing config
   pipeline without relaxing its grammar. The plan records the final syntax.
2. Asset kinds remain neutral textual identities in the foundation; composition
   supplies supported kinds. Foundation modules do not import payload owners.
3. Forced-collision tests use the exact production validator through a private
   header, without a public injected-hash API.
4. Existing numeric Biome/Feature/Profile references remain world-local semantic
   aliases. World bindings map those aliases to qualified assets and explicit
   numeric IDs/indices. Material-to-Element and appearance-to-Texture links are
   actual pack references, checked for dependencies and kind. Diagnostics must
   distinguish missing aliases from missing pack assets.
5. Numeric compilation will accept validated explicit bindings. The existing
   convenience path must produce lexical bindings and feed the same compiler,
   preserving aliases for RNG. Binding content must enter numeric compatibility
   identity. Complete palettes, including unsampled biomes, remain required.
6. `safe_object`/`safe_ptr` should serve checked non-owning references where
   appropriate. They do not replace ownership of snapshots, mappings, jobs, or
   GPU resources through draining/retirement.
7. Multi-pack application requests explicitly carry roots, selected qualified
   World, and generation. A new optional `--worldgen-pack-set=<TOML>` composition
   file uses schema one and `[packs.<alias>]` entries with roots relative to that
   file. No dependency-root discovery or core CLI list parser is introduced.
   The existing content-root option remains the single-pack convenience. Pack-set
   roots take precedence over that default; explicitly conflicting options are
   rejected where existing option provenance supports it. Multiple packs require
   a qualified world/profile selection, with an actionable diagnostic otherwise.
8. Authoring sources become an owned dynamic pack/descriptor set, preserving
   transactional history, manifest conflict detection, partial-save reconciliation,
   newer edits, and undo intent. No synthetic seven-document compatibility layer
   is introduced.

## Foundation gate

The initial foundation subphase is complete; milestone one is not yet complete.
Checkpoint commit: `42b7be5` (`Add typed asset pack foundation and checked lookup`).
The user requested intermediate milestone commits; subsequent passing phases
will be selectively committed without staging unrelated work.

Implemented: core-only `engine.asset`, owned manifest/config import, immutable
pack index, qualified identity with fixed encoding, composition-supplied kind
policy, dependency-checked lookup, containment, and private collision validation.
Existing content loaders are unchanged at this gate.

Independent review initially found input-order-dependent diagnostics for
same-root malformed manifests and two missing paragraph boundaries. Both were
fixed. Regression tests reverse distinct-provenance and same-provenance sources;
the targeted source re-review passed with no new findings.

Executed by Luna in one persistent VS 18 Insiders/MSVC 14.51 shell:

| Check | Result |
|---|---|
| Scoped Format Fix and Check, including explicit private `.hpp` check | Passed |
| Touched-source semantic style | Zero blockers; four comparison advisories; header manually reviewed |
| `cmake --preset msvc-dev -DPPR_TEST_LOOPS=1` | Passed |
| `cmake --build out/build/msvc-dev --target engine.asset engine.tests.asset` | Passed |
| `engine.tests.asset.exe --loop 1 --no-shuffle --run-test asset/packs` | Nine named leaves passed |
| Independent foundation source review and fix delta | Passed |

Initial logs: `.slim/tmp/asset-foundation-configure.log`,
`.slim/tmp/asset-foundation-build.log`, `.slim/tmp/asset-foundation-test.log`.
Durable execution evidence is collected under
`out/validation/20261016/asset-pipeline/foundation/`: manifest, logs, source hashes,
executable/PDB hashes, and all nine leaf names/times. Exact outer process wall
time was not captured for this initial gate; reported CMake times, Ninja action
span, and test-tree elapsed time are recorded separately rather than substituted.

The pack group took approximately 176.5 ms in this correctness run. This is an
incidental observation, not a baseline comparison or performance claim.

## Domain descriptor seam gate

Checkpoint commit: `47a4b1a`
(`Add typed content descriptors and two-pack composition`).

Implemented real `engine.image:texture` and `engine.sim:asset` descriptor schemas,
typed definition-only catalogs, and the composed owned
`ContentDescriptorCatalog`. The latter pins its immutable pack set. Texture
settings reuse the existing Mitchell processor. Real two-root fixtures exercise
all supported kinds, distinct Materials sharing one Element, missing/corrupt
binary sources, provenance lifetime, wrong-kind diagnostics, palette domain
errors, symlink containment, and settings/physical-role rejection.

Scoped Format Check and semantic-style checks passed. Independent source review
passed with no findings in the bounded import/link scope. Two ordinary compile
defects were fixed and reviewed: importing the core `make_error_code` overloads
in nested namespaces, and including `pP/Macros.h` in `Content.cppm`'s global
module fragment. There was no compiler ICE or architectural substitution.

Luna configured `msvc-dev` and successfully built `engine.image`, `engine.sim`,
and `game.tests`. The exact filtered run
`game.tests.exe --run-test game/content_descriptors --loop 1 --no-shuffle`
passed all eight named leaves, including both symlink-containment cases.
Harness group totals include group nodes and are not additional leaf tests.
An initial mistyped executable path failed before launch and is retained in the
execution record. The corrected invocation passed.

Evidence: `out/validation/20261016/asset-pipeline/domain-seam/`. The source scope
does not yet include full World alias/appearance validation, selected numeric
lowering, binary preparation, or consumer readiness. Existing loaders still use
the old format pending their coherent migration batch.

## Explicit bindings and selected World assembly gate

Checkpoint commit: `a56a804`
(`Compile selected worlds with explicit asset bindings`).

Implemented a single numeric compiler path with explicit global bindings;
lexical convenience entry points produce bindings and forward to it. Preflight
checks exact membership, unique aliases, contiguous unique IDs/indices, and
rejects caller-supplied rule/marker maps. Emission follows numeric binding order;
RNG paths retain semantic aliases. Feature markers are attached by the emitted
feature's identity, independently of input binding-vector order.

Selected World assembly resolves only its declared complete palettes, composes
Element physical values into Materials, binds owner-local rules, validates
appearance, and remaps diagnostics to owned descriptor provenance. Duplicate
asset targets within each palette kind are rejected; distinct Materials may
share an Element, and assets may be reused across Worlds.

Numeric identity includes interpreted physical/domain values, semantic aliases,
explicit bindings, compiler/schema versions, and actual compile parameters.
Linkage-only asset-name changes are excluded when those inputs are identical.
Presentation identity is currently descriptor-only; binary source-byte identity
belongs to the forthcoming preparation phase.

Source review passed after three paragraph-boundary corrections, with no
functional findings. Format Check and semantic-style gates passed. Both
`engine.tests.sim` and `game.tests` built from the final source snapshot.

- Four `sim/asset_bindings` leaves passed.
- Eight `game/world_assembly` leaves passed across the initial run and affected
  rerun. Four initially failed because the shared fixture gave a 1.8 m actor
  only 1 m clearance; the fixture was corrected to 2 m. Production validation
  remained unchanged, and the correction passed independent review.
- Eight descriptor regression leaves passed after that fixture correction.
- The approved existing pinned fingerprint, rule canonicalization, deterministic
  ID, binding/output reordering, and semantic reference-domain checks passed.

Evidence: `out/validation/20261016/asset-pipeline/numeric-assembly/`. The execution
record retains all failed attempts and actual matching leaves. An extra legacy
pack filter was started outside the intended narrow golden scope; two leaves
finished successfully in approximately 31 and 50 seconds before the remaining
filter was stopped. This is incidental partial evidence, not a passed full
suite or a performance comparison. No broad raster suite was run for this gate.

Evidence limitation discovered after this checkpoint: the persistent interactive
shell did not capture immediate child process exit codes for numeric configure,
builds, and most focused runs. The logs show completed link actions and named
leaf outcomes; those are not substituted for process exit status. The manifest
marks unavailable statuses explicitly. Only the final approved Sim golden run
has a confirmed immediate exit code of zero. The next applicable stable closure
must capture true exits using a task-local execution wrapper; these process-level
acceptance checks remain pending until then. Do not repeat the incidental old
full-pack workloads to repair metadata.

## Canonical physical domains gate

Implemented the first part of the user's single-domain requirement. A lower
`Sim.Element.cppm` partition owns Element phase and density constraints.
Flattened Material metadata, Material composition, and numeric material checks
reuse them. Material representation likewise has one owning declaration reused
by the asset descriptor. Existing object layouts, field ordering, enum names,
inclusivity, nonfinite policy, error categories, and pinned fingerprints remain
unchanged.

Independent review passed after one paragraph separator in the boundary test.
Scoped formatting/style gates passed. An ignored command wrapper was verified
with harmless exit-zero and exit-seven children before use. Configure and both
`engine.tests.sim`/`game.tests` builds then completed with captured exit zero.
Focused binding, physical-domain boundary, assembly, descriptor, and all five
approved existing semantic golden checks passed with captured exit zero. This
refreshes the missing process-status evidence from the preceding numeric gate;
it does not rerun the incidental old full-pack workloads.

Evidence: `out/validation/20261016/asset-pipeline/canonical-domain/manifest.json`.
Actual leaf lists distinguish prefix-matched extras from requested cases.

The wider changed-scope range audit is not finished by this checkpoint. Next
reuse owning Definition/Profile collection domains in World palettes, align
texture alpha-cutoff declarations with the existing processor contract, and
centralize qualified-reference length domains. Appearance reference metadata
changes belong with the atomic old-format migration. Different intentional
context policies, such as generic decoder capacity versus content preparation
budgets, remain distinct rather than being unified by matching numbers.

PIX status is ready and elevated. The server executable resolves to the pinned
official `local-services/pix/pix-mcp.exe` with SHA-256
`D3649B03359E989B33D16DA06CF2262B7680BCA46BCA77C25D8F0EAC8FA6B026`.
PIX application/tool version and hash are recorded separately. No capture was
needed for the canonical-domain correctness gate.

## Current next step and pending gates

Standalone shared asynchronous preparation and its image processor have passed
their focused checkpoint. The consumer/format migration is authored and its
source review passed. The private construction bridge is integrated and passed
its scoped mechanical gates after the coherent compile proof described above.
Unchanged-image reuse bodies and seven new focused leaves passed author
self-review, formatting and independent source review. The four test targets
build; focused image preparation passes, while consumer and full-suite runtime
acceptance remain in progress.

The all-world descriptor owner remains separate from a selected numeric catalog.
`loadContentCatalog` now returns definitions only. A selected World view is
assembled using actual compile parameters; plan construction consumes its
matching bound recipe without recomputing lexical bindings. Runtime Material IDs
are meaningful only within that selected World.

Asset owns bounded requests and their shared asynchronous lifecycle. Image owns
processing stages and budget decisions; content composes their results with
selected World appearance. The shared request receives an explicit non-owning
scheduler dependency that must outlive the request and accepted captures; the
texture factory supplies `backgroundPool()`. Workshop's external update polling
adopts definition results, pumps image requests, then schedules capture after
readiness. WorldJob's separate
coordinator may wait/drain image jobs. CPU preparation does not perform GPU
uploads. A distinct owning presentation sidecar carries World/numeric/presentation
identity and generation; capture requires a matching ready sidecar. Cancellation
and stale-result handling still drain accepted work and surface fatal allocation
failures at the external reporting boundary.

The user requires one authoritative valid-input domain per descriptor property.
Import, assembly, and processor validation must reuse the descriptor's declared
range instead of repeating numeric bounds. Apply this to changed asset schemas
and their affected validators without an unrelated engine-wide cleanup.

The independent four-test-target closure has built. Run focused
source/pack/checkpoint/preview/CLI and
shipped numeric parity checks, followed by the applicable twelve-entry matrix,
interactive/lifetime gates and IDE inspection. Runtime integration, cancellation,
draining, GPU retirement and numeric parity remain unverified for this migration.
Diagnose the separate smoke compiler blocker before rebuilding `app.game` or
running its shipped validators and interactive gates.
Collect a fresh focused PIX trace from a required natural workload using the
documented query route, and report test timings as observations, not benchmarks.
Finish documentation/skill review and selectively commit passing milestones,
preserving unrelated user changes. Earlier checkpoint passes do not establish
these later behaviors.
