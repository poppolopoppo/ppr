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

## Foundation gate

The initial foundation subphase is complete; milestone one is not yet complete.

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
Durable execution evidence is being collected under
`out/validation/20261016/asset-pipeline/`.

The pack group took approximately 176.5 ms in this correctness run. This is an
incidental observation, not a baseline comparison or performance claim.

## Current next step and pending gates

Implement real image/simulation descriptors and a composed two-pack import/link
seam, with filesystem/symlink containment coverage. Then migrate definition-only
composition and explicit numeric assembly, asynchronous image preparation,
publication and consumers, formats, presentation reuse, documentation/skills,
and final validation in the plan's order.

Real domain integration, shipped-content migration, numeric output parity,
cancellation/draining and GPU retirement, full applicable suites/interactive
matrix, IDE inspection, and focused timing evidence remain pending. The
foundation pass does not establish those later behaviors.
