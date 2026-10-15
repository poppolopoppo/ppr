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

## Current next step and pending gates

Next implement explicit compiler bindings, selected typed World assembly, and
binding-aware numeric identity. Then migrate the live definition loader,
authoring and application consumers and shipped formats; add asynchronous image
preparation/publication, presentation reuse, documentation/skills, and final
validation in the plan's order. Reject duplicate asset targets within each
World palette kind, while allowing distinct Materials to share an Element and
the same asset to be used in different Worlds. Numeric identity excludes source
asset-name changes when interpreted semantics, aliases, and bindings are equal.

Real domain integration, shipped-content migration, numeric output parity,
cancellation/draining and GPU retirement, full applicable suites/interactive
matrix, IDE inspection, and focused timing evidence remain pending. The
foundation pass does not establish those later behaviors.
