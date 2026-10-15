# Asset pipeline implementation progress

Plan: [Scoped asset pipeline](asset-pipeline-plan.md).

Updated: 2026-10-16. Implementation is in progress; the full plan is not delivered.

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

Next implement and verify standalone asynchronous image preparation and joined
presentation results. Then switch the live loader, dynamic authoring sources,
startup/preview/checkpoint consumers, and checked-in formats in one coherent
migration, followed by reuse, documentation/skills, and final validation.

The all-world descriptor owner remains separate from a selected numeric catalog.
`loadContentCatalog` will return definitions only. A selected World view will be
assembled using actual compile parameters; plan construction will consume its
matching bound recipe, not recompute lexical bindings. Runtime Material IDs
are meaningful only within that selected World.

Image owns bounded batch requests and processing; content only composes their
results with selected World appearance. Workshop currently runs on
`backgroundPool()`, so external update polling must adopt definition results,
pump image requests, then schedule capture after readiness. WorldJob's separate
coordinator may wait/drain image jobs. CPU preparation does not perform GPU
uploads. A distinct owning presentation sidecar carries World/numeric/presentation
identity and generation; capture requires a matching ready sidecar. Cancellation
and stale-result handling still drain accepted work and surface fatal allocation
failures at the external reporting boundary.

The user requires one authoritative valid-input domain per descriptor property.
Import, assembly, and processor validation must reuse the descriptor's declared
range instead of repeating numeric bounds. Apply this to changed asset schemas
and their affected validators without an unrelated engine-wide cleanup.

Real domain integration, shipped-content migration, numeric output parity,
cancellation/draining and GPU retirement, full applicable suites/interactive
matrix, IDE inspection, and focused timing evidence remain pending. The
foundation pass does not establish those later behaviors.
