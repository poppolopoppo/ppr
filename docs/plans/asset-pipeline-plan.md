# Scoped asset pipeline implementation plan

Date: 2026-10-16

Status: implementation in progress; shared asynchronous API boundary corrected
after the user's 2026-10-16 review and independent Astra assessment.
Prepared by the original Astra reviewer after the independent max-effort audit
and the user's subsequent identity and future-audio requirements. This replaces
the broader asset-pipeline proposal from this conversation for this task; it is
not a description of functionality already implemented.

## Goal and scope

Build a small shared asset pipeline with typed catalogs and explicit processing
stages. Support named descriptors, multiple packs, asynchronous textures, and
contracts that accommodate future audio and mesh integration. Do not introduce
a universal object store, dependency scheduler, or live mutable asset manager.

`loadContentCatalog` loads and validates definitions only. It performs no texture
mapping, source hashing, decoding, mip generation, or texture waits. Definition
loading may read texture descriptors, but it does not open their binary sources.

The immediate performance problem is that definition loading and draft validation
inherit texture processing and repeated config conversions. Separating those
responsibilities addresses that problem; it does not by itself fix raster
sampling cost or establish a measured performance gain.

Backward compatibility is not required. Port shipped content and fixtures
directly; reject unsupported old formats clearly rather than maintaining two
import paths.

## Ownership and module boundaries

The current split has a reason: `engine.sim` owns file-neutral definitions and
numeric world generation; Colony content adds pack files, appearance, textures,
and application-facing composition. Available Git history describes that split.
Move generic pack mechanics below both domains without moving application policy
into simulation.

| Owner | Responsibility |
|---|---|
| New `engine.asset`, depending only on core | Pack identity and immutable request indices; qualified names; checked lookup and containment; source provenance; reusable typed config importing; shared typed asynchronous preparation API, admission, cancellation, polling, settlement, draining, and outcome handling. |
| Existing `engine.image` | Texture descriptors and settings, typed texture processor inputs/results, source mapping/hash/decode/mip stages, and image-specific budget decisions using the shared asset request. |
| Existing `engine.sim` | Physical Element/Material definitions, fields, biomes, features, rules, world definitions, domain validation, and world-context numeric lowering. |
| Existing `game.colony.content` | Compose simulation definitions with material appearance and backgrounds; cross-domain reference checks; definition catalog and presentation requests. |
| Game/application and existing render passes | Candidate activation, editor/export/checkpoint workflows, GPU uploads and residency, gameplay state, and retirement. |

Appearance does not become simulation-owned merely because it contains no GPU
objects. Color, surface rendering parameters, backgrounds, and texture links
remain in existing content composition. Texture interpretation and mip settings
belong to image. This intentionally refines the earlier proposal to move the
entire content model into sim.

```text
engine.core
    ↑
engine.asset
    ↑                    ↑
engine.image         engine.sim
          ↑          ↑
       game.colony.content
                ↑
      application/editor/export
```

Arrows show consumers depending on lower facilities. Existing math dependencies
remain. Neither asset nor sim imports image payloads, app, RHI, or GLFW. Game
composition submits presentation work to the existing render passes; passes own
their GPU resources and fenced retirement.

Add `lib/engine/asset` and its CMake target, then add the necessary asset
dependencies to existing image and sim targets. New declarations and
implementations enter their owning targets together. Do not add
`engine.asset.image` or `game.colony.assets` modules. No new GPU manager is needed.

## Identity and immutable pack indices

The authoritative identity is a stable qualified name, such as `ppr.base:rock`,
with its declared kind.

| Concept | Representation |
|---|---|
| Authoring identity | Stable pack namespace and stable local key; manifest declares kind. |
| Linked identity | Neutral strong `AssetKey` holding a specified deterministic `u64`; typed APIs use `Numeric<u64, DistinctTag>`. |
| Revision identity | Separate semantic, source, and processing fingerprints. |
| Runtime addressing | Separate request generations, catalog slots, runtime handles, and world-local numeric IDs. |

Specify a fixed identity hash format: FNV-1a-64 over a versioned domain and
length-delimited canonical namespace, key, and kind. Freeze the exact byte
encoding with test vectors before persistence. Retain canonical names alongside
hashes and reject collisions. Never derive persistent identity from `std::hash`,
C++ type names, variant ordinals, filesystem paths, or mount order.

Use lowercase ASCII names with a defined grammar. Preserve existing simple local
key grammar where practical; namespace separators belong to the qualified-name
parser, not a blanket relaxation of core config key validation. Freeze grammar
and case rules in the first milestone.

The existing wrapper is `Numeric<T, TagT>`. Distinct tags produce distinct types;
aliases using the same underlying type and tag do not. Its underlying conversion
remains available, so construction is not validation: checked resolution must
verify kind and membership. Existing mesh asset IDs are scene-local indices and
must not become persistent pack IDs.

A closed variant of typed IDs is useful for heterogeneous editor selections or
application dispatch. Keep it at the composition boundary where needed; the
foundation need not enumerate every future domain type or import payload
modules. Never serialize its alternative index as a kind identifier.

Identity behavior is explicit:

- Moving a descriptor or relocating a pack preserves identity.
- Changing a display name preserves identity.
- Changing a stable key or pack namespace changes identity and requires reference updates.
- Cloning an asset gives it a new key; forking a pack gives it a new namespace.
- Mounting two copies of the same namespace is an error.

Do not use generated UUIDs in this version. Name-based references already supply
the required identity contract. GUIDs would be useful if identity must survive
changing stable keys or namespaces, but do not automatically repair textual
references. If adopted later, generate once during authoring and persist them,
never generate per load. The existing facility is `hal::Uuid`; its current
Windows implementation does not explicitly set RFC version/variant bits or
provide recoverable generation failure. Fixing UUID generation is outside this
plan.

## Descriptor format and multiple packs

Each manifest declares an explicit index. Example shape:

```toml
schema_version = 1
pack_id = "ppr.base"
[requires.shared]
pack_id = "ppr.shared"

[assets.rock]
kind = "sim.material"
descriptor = "materials/rock.toml"

[assets.rock_albedo]
kind = "image.texture"
descriptor = "textures/rock_albedo.toml"
```

A Material descriptor may compose reusable simulation fields and content-owned
appearance in one typed wrapper:

```toml
schema_version = 1
element = "rock_element"
representation = "terrain"
diggable = true

[appearance]
display_name = "Rock"
color_rgba = "808080ff"
albedo_texture = "rock_albedo"
normal_texture = "ppr.shared:stone_normal"
texture_period_m = 1.0
roughness = 0.8
```

A Texture descriptor owns image processing settings:

```toml
schema_version = 1
source = "images/rock.png"
interpretation = "color"
mip_policy = "mitchell"
```

These examples show the intended separation; milestone one freezes schemas and
supported setting values against existing processors.

The domain descriptor seam uses `sim::asset::Element` for phase/density and
Material descriptors for Element references, representation and optional
diggability. Existing typed Biome/Feature/Rule/Field models are wrapped as
`schema_version = 1` plus `[definition]`; content composes `[appearance]` where
needed. A World owns `semantic_key`, `[profile]` using `ProfileDefinition`,
`[palette.materials.<alias>]` with `asset`/`id`, and indexed field/biome/feature
palettes with `asset`/`index`. `[palette.rules.<alias>]` names standalone Rule
descriptors; rule order and profile ownership remain in the existing schema.
Inline `profile.rules` is rejected rather than becoming a second rule format.

Descriptor-declared property domains are authoritative. The lower sim Element
descriptor owns phase/density constraints reused by flattened Material metadata
and runtime validation; Material owns its representation domain. Processors and
authoring controls reuse these constraints instead of restating scalar ranges.
Collection capacities, contiguous binding membership and cross-field safety
relationships remain distinct invariants. Image request budget defaults have
one image-owned declaration reused by content policy aliases.

World palette entry keys and references inside reused numeric descriptors are
world-local semantic aliases (maximum 128 bytes). They resolve against the
selected World's exact declared bindings, including unsampled biomes. Material
to Element, appearance to Texture/background, and palette `asset` fields are
pack asset references (maximum 257 bytes), checked against the declaring pack
and its dependencies. Marker and Rule edges retain their feature/profile owner
scope. Diagnostic locations remain owned descriptor provenance with world
context supplied at assembly.

Texture descriptors support `color`, `data`, and `normal` interpretation and
the existing `mitchell` downsampling policy. Optional `full_chain` (true),
`high_quality` (false), `preserve_coverage` (false), and `alpha_cutoff` (0.5)
map directly to existing mip processing. Normal maps reject coverage
preservation. The current pinned stb float-linear default downsampler is
Mitchell; changing processor behavior requires its separate processor version.

Implementation decisions frozen for milestone one: local keys use the existing
`[a-z0-9_-]{1,128}` grammar. Pack namespaces are 1..128 bytes of nonempty,
dot-separated local-key segments. References are either a local key or exactly
`namespace:local_key`; uppercase and non-ASCII text are rejected. Manifest
dependency aliases use local keys and their `pack_id` values name namespaces.
Two aliases naming the same dependency namespace are rejected. Keyed tables
reuse the actual core tree codec, which rejects TOML arrays; no array-based
manifest format or core key-grammar relaxation is introduced.

Identity bytes are ASCII `ppr.asset.key`, one NUL byte, version byte `0x01`,
then namespace, local key, and textual kind in that order. Each text field has
an unsigned 32-bit little-endian byte-length prefix followed by its ASCII bytes.
FNV-1a-64 uses offset basis `14695981039346656037` and prime `1099511628211`,
with unsigned 64-bit wraparound. Frozen known answers:
`ppr.base:rock` / `sim.material` = `feb0b05e2204a87b`;
`ppr.shared:rock_albedo` / `image.texture` = `5c7286efae017167`;
`a:b` / `c` = `63b514d865ddaf22`. Foundation kind policy is supplied by the
composition boundary and does not enumerate payload domains.

The request first owns and indexes all manifests, then imports descriptors and
validates links. Apply these policies:

- Unqualified names resolve only in the declaring pack.
- Cross-pack references are qualified and require a declared dependency.
- Duplicate namespaces or local keys, hash collisions, missing targets, and wrong kinds are errors.
- There are no implicit overrides, search-path resolution, or last-mounted-wins rules.
- Dependencies require presence; there is no version-range solver, fetching, or plugin discovery.
- Descriptor and binary paths remain within their declaring pack root. Cross-pack access uses asset references.
- Dependency declarations and symbolic reference cycles are not automatically errors. Reject recursive computation and invalid domain relationships, not every reference cycle.

Unknown or unsupported kinds fail clearly. Audio and mesh processing are future
deliveries, not placeholder successes in this one.

## Element, Material, and world lowering

Retain the current meaning of cell IDs: **cells identify Material, not Element**.

Element extracts the existing physical properties shared by materials. Material
references an Element and retains its distinct simulation identity; appearance is
composed alongside it. Two Materials may share an Element while retaining
different cell IDs and visual definitions. Material is therefore not described
as presentation-only.

A content-owned Material wrapper reuses the sim schema for the physical
reference and the content schema for appearance. Resolve it once, then pass its
owned typed portions onward. Use the same composition approach where a World
adds presentation references.

Standalone authoring files do not imply independently compiled numeric objects.
Biome lowering depends on a world's palette; Rule lowering depends on profile
ownership. Retain linked symbolic descriptors until the selected World is
assembled, then use the existing sim compiler as the numeric lowering boundary.

Each World declares explicit deterministic palettes/bindings:

- Preserve vacuum ID zero, reserved values, and existing capacity checks.
- Preserve Material IDs, field/biome/feature ordering, and rule ownership/order.
- Preserve semantic RNG keys; do not substitute asset hashes.
- Reject duplicate, missing, or out-of-range bindings.
- Never append per-pack palettes or derive offsets from mount order.

The explicit compiler overload validates exact coverage and contiguous numeric
domains before emission. Its convenience overload builds lexical bindings and
uses the same lowering path. Rule and marker binding maps remain compiler-owned
output. A selected World rejects duplicate asset targets within each palette
kind; distinct Material assets may share one Element. Numeric identity includes
interpreted definitions, semantic aliases, explicit IDs/indices and compiler
parameters/versions, while excluding pack-qualified linkage names when the
interpreted result is identical. Presentation descriptor identity is separate;
binary source-byte identities enter only explicit preparation.

Migrate the complete current palette, including unsampled biomes. Global raster
pass maxima depend on those definitions; direct-reference pruning can change
output even if the omitted biome is absent from sampled chunks. Unrelated
mounted packs remain outside a world's declared palette.

Retain the selected recipe compiled with actual supplied parameters; runtime
plan construction consumes that recipe rather than rebuilding lexical bindings.
Numeric selection/preview support their explicit parameter domains. Prepared
runtime generation/restoration preserves the existing canonical application
metric and actor-size restriction, rejecting other recipes before work instead
of mislabeling them with default metadata. Driver spawn sizes reuse the owning
application compile-parameter declaration.

## Import each descriptor once

Reuse the actual configuration facilities; there is no current
`config::pipeline` API:

```text
owned descriptor bytes
 → config::decode(..., EDecodeMode::tree)
 → config::toTreeLayer(...)
 → config::resolveRecursive<Descriptor>(...)
 → owned typed value and provenance
```

Reuse existing `describe()` metadata through composed descriptor types. Do not
duplicate schemas or serialize assembled definitions back to TOML.

The common importer supplies owned provenance, bounded diagnostics, and checked
name/kind lookup. Domains supply explicit typed importer/processor functions or
narrow adapters at application composition. Typed sim and image catalogs own
their results; asset does not store all payloads behind type erasure.

Replace the `projectContentCatalog` typed-value-to-document-to-resolve roundtrip.
Assemble typed values directly, validate cross-references at the appropriate
boundary, and lower the complete selected World.

Use a few explicit stages:

1. Read and index manifests.
2. Import typed descriptors.
3. Validate links and domain constraints.
4. Assemble and compile the selected numeric World.
5. Independently prepare its requested presentation assets.

## Asynchronous preparation and future audio

Texture processing reuses `mem::SharedBuffer::mapFile()` and
`parallel::backgroundPool()`. Do not introduce another thread pool or general
dependency scheduler.

The asynchronous API itself belongs in `engine.asset`, shared by asset types.
Providing only a generation typedef and cancellation context is insufficient.
Do not implement a complete texture-owned request lifecycle for later audio or
mesh processors to duplicate.

Use a small typed request abstraction, provisionally
`asset::PreparationRequest<Processor>`, with `Processor::Result` as its owning
result type. Concrete names may follow existing conventions. The common request
owns its pack snapshot/context and generation, bounded admission through the
existing pool, polling, cancellation, completion publication, ordered outcomes,
fatal-exception precedence, draining, and external-owner take/finish/teardown.
Shared lifecycle behavior is implemented and tested once. A texture convenience
factory returns this shared request rather than another independent lifecycle.
Each worker stage returns a fresh owned outcome through `std::future<Outcome>`.
Cumulative per-item diagnostics, errors and exceptions remain exclusively on
the request owner; tentative admission never transfers that cumulative state.
Collection retains exception/error state and transitions the consumed slot
before any allocating diagnostic merge. Outcome moves and destruction must be
nonthrowing. Preserve the first retained fatal allocation exception when later
failures arrive, while promoting an earlier ordinary exception to fatal.
The request receives an explicit non-owning scheduler dependency; its owner
keeps that scheduler alive through request destruction and accepted-capture
settlement. The texture factory supplies `backgroundPool()` at its boundary.
Blocking guards use that exact scheduler, and the request never cancels or joins
the scheduler itself. Isolated local-pool tests exercise the same admission path.

The concrete processor supplies owned per-item inputs, its processing revision,
metadata validation, worker steps, typed result, and domain-specific decisions.
Support an owner-side continuation between completed processing steps: the image
processor needs decoded dimensions before deciding exact mip-output admission.
The asset request schedules a continuation or settles the item while retaining
the same bounded item slot. Image owns its base/decode/mip state and byte-budget
calculations; asset owns admission and asynchronous settlement. Completed bases
waiting for an earlier item's budget decision still occupy slots. This is a
flat batch with sequential per-item steps, not a processing dependency graph.

The shared contract carries explicit source/settings/processor version,
generation, and cancellation. Typed results report success, cancellation, or
failure explicitly and own their data. All admitted work and worker captures
settle before request storage is released. Consumers pin result versions; stale
adoption is rejected at the application boundary using the carried generation.
Domain worker state must never own the coordinating request or cause blocking
teardown on the shared pool's worker threads.

Audio is a firm future consumer. Do not define every prepared result as a fully
decoded, permanently resident buffer. Later audio can prepare metadata plus an
owning source suitable for streaming; playback voices and device resources remain
audio-owned. Common contracts do not prescribe representation, domain readiness,
or device activation. This creates a place for audio without implementing it now
or pretending all future streaming requirements are solved.

For textures, use bounded job-count admission, initially two concurrent jobs,
adjustable from evidence. Remove bespoke byte-wave estimates. Two jobs bound
concurrency, not total retained image memory.

Retain per-image limits and existing aggregate input/output/count limits across
the complete presentation request, including assets from multiple packs. Pack
splitting cannot bypass request limits. Definition limits keep their documented
scope; account for overlapping active/candidate/retiring ownership.

Jobs retain mapping owners through hashing/decoding and own resulting mip
buffers. Jobs produce private results and diagnostics, merged deterministically
in request order. No worker waits on unfinished work queued to the same pool;
existing nested mip `forRange` work uses its same-pool inline behavior.

Drain every admitted job before releasing captured state. Ordinary failures
remain contextual error results. Capture worker exceptions privately; fatal
`bad_alloc` reaches the owning reporting boundary after draining accepted work.
Never cancel or join the shared pool itself.

The common request preserves allocation-failure priority over earlier ordinary
errors, cancellation, and staleness. Cover partial submission, terminal polling,
explicit finish, destruction, and replacement. Future readiness alone does not
prove callable-capture destruction; use request-local WaitGroup settlement.
Its nonblocking readiness query is a snapshot and does not imply success or
prevent future admission. Noexcept cleanup fallback must drain before reporting
a fatal allocation failure; it cannot silently downgrade it.

Prove the shared boundary with concrete non-image test processors, including a
move-only owning result that does not represent a resident decoded buffer.
Exercise delayed/out-of-order completion, partial admission failure,
cancellation, fatal-error precedence, capture destruction, and bounded
continuation slots through the real shared API. These processors test a genuine
architectural contract, rather than adding a test-only injection API.

## Source stability

An owning file mapping is not an immutable byte snapshot. `makeOwned()` can
retain the mapping, and hashing after decoding does not prove both operations
saw identical bytes during external modification.

Adopt an explicit contract: binary sources are stable for an admitted request.
Application-controlled updates replace complete files atomically where supported;
otherwise drain affected requests before replacement. External in-place mutation
during a request is unsupported. Detect observable changes and report conflicts,
but do not claim metadata checks prove immutability.

Supporting arbitrary concurrent external writes later requires a separately
designed capture/locking policy. Immutable pack-set metadata does not itself
guarantee immutable source bytes.

## Publication, reload, and teardown

Reuse the existing candidate/world-job and GPU activation transaction:

- A candidate pins one immutable pack-set snapshot and request generation.
- Numeric readiness and presentation readiness are separate.
- Activation requires matching numeric identity, presentation identity, and generation.
- Failure, cancellation, or stale completion leaves the old usable session intact.
- Ready CPU images do not imply GPU residency.
- Existing pass-owned uploads and fences determine GPU activation and retirement.

Keep active, candidate, and necessary retiring owners bounded; do not accumulate
an unbounded sequence of pending candidates. Reuse independently owned
presentation results when only definitions change. A small image-specific reuse
table keyed by source revision and processing settings is permissible if needed;
it is not a universal incremental store.

Explicit reload builds a replacement pack-set candidate. Unmount changes future
snapshots. Existing worlds, jobs, and GPU work retain their owners until
retirement; immediate release is not promised. Removing a pack with remaining
dependent references fails candidate validation.

Shutdown stops submissions, detaches callbacks, cancels/wakes and drains owned
requests, then releases resources in dependency order. Draining belongs to the
external coordinator; last worker-owned references must not trigger blocking
same-pool teardown. Preserve best-effort cleanup and first-error reporting.

## Owned authoring sources

Authoring owns a complete candidate root set, manifests and per-asset descriptor
bytes. A required synchronous `DescriptorReader` returns owned source values
through the ordinary typed import path; disk convenience overloads delegate to
that same path. Index/kind checks precede the callback, returned provenance must
match the indexed path, and missing snapshot records fail without disk fallback.
Readers and snapshot views never escape synchronous importing.
An immutable `ContentSourceSnapshot` pins its exact owned source set and built
pack index. Disk loading, source validation and catalog import reuse that index;
mutable/invalid draft bytes stay separate and explicitly build a fresh snapshot.

Reuse foundation per-file limits (1 MiB descriptors, 64 KiB manifests). One
composition-owned source limit retains the former aggregate text ceiling,
`7 * asset::kMaxDescriptorBytes + asset::kMaxManifestBytes`, across all manifests
and descriptors in the candidate's mounted roots. This counts logical retained
source bytes once, not scratch, history or simultaneous candidate-copy peaks.
Validate owned candidates before typed importing; keep disk retention bounded.

New authoring targets use an injective supported-kind directory mapping and
`asset-<local_key>.toml`, keeping valid keys such as `con` safe on Windows.
Existing valid contained paths retain their spelling. Index canonical existing
paths including manifests, then inspect hard-link counts once and compare only
alias candidates. Exceptional network namespaces require conservative
equivalence checks; missing targets compare existing parents plus the generated
suffix. Those exceptional groups may remain quadratic. Metadata errors are not
absence, a new action cannot take over an unrelated existing file, and save
revalidates containment and ownership under the stable-file contract.

Partial saves use a separate owned per-file disk ledger, including expected
absence, rather than importing orphan files as a complete candidate. Commit
descriptor creations/edits first; replace manifests only after all prerequisites
reach desired bytes; remove obsolete files only after all affected manifests
reach desired bytes. Reconcile actual successes without replacing newer drafts
or undo intent. This is not a global filesystem-atomicity promise or journal.
Save preflight rejects unexpected external changes even for descriptors left
untouched by the draft. Adopt external content through explicit whole-candidate
reload, never by silently merging disk bytes into the validated owned candidate.
Whole-candidate draft/save validation lowers every declared World with explicit
application compile parameters, reusing the imported descriptors. It performs
no image work; a valid selected preview does not hide a broken unselected World.
Restore an arbitrary deleted path only for the same prior owned asset identity,
kind and path, backed by its tombstone/history. Retire unreachable tombstones;
reconcile owned unreferenced files before admitting additional fresh file growth
after a partial failure. Cleanup failure remains visible and retains newer intent.

## Fingerprints and workflow migration

Keep these identities separate:

- **Numeric world identity:** physical semantics, biome/field/feature/rule data, explicit bindings and RNG keys, relevant schema/compiler versions.
- **Presentation identity:** appearance, texture descriptors, source-byte identities, and processing versions.
- **Provenance:** locations and comments used for diagnostics and editing.

Tint changes do not rebuild terrain. Texture-byte changes affect presentation.
Density and world-generation rules affect numeric compatibility. Comments update
provenance without semantic changes. Stable asset identity is not revision
identity. Absolute roots, mount order, request generations, and runtime slots
never enter persistent fingerprints. Unrelated pack contents do not invalidate a
world's semantic closure.

Authoring distinguishes definition validation from full presentation validation.
Headless generation and definition validation never touch texture sources.
Exports requiring complete assets explicitly request and await presentation
preparation.

Port checked-in assets, fixtures, and checkpoint identities directly. Reject
unsupported old formats clearly, with no compatibility adapters or dual formats.
World checkpoint envelope version 4 records the owning manifest schema version;
the earlier flattened content model's version is not a manifest version. Reject
earlier checkpoint envelopes at the header. Raw sim snapshot version 4 remains
an independent unchanged format.
Numeric output stability does not imply old combined fingerprints stay identical
after splitting their contract.

## Implementation milestones and acceptance gates

### 1. Foundation and two-pack descriptors

Add identity/index/import contracts and typed catalogs. Freeze naming grammar,
hash encoding/test vectors, explicit kind identifiers, and descriptor schemas.
Prove canonical names, declared cross-pack references, duplicate and collision
errors, wrong kinds, containment, and deterministic diagnostics. Include a World
referencing definitions and textures across two packs. Prove the actual module
and config-instantiation seam before expanding it.

### 2. Definition-only composition and numeric assembly

Remove catalog config roundtrips. Migrate Element/Material and explicit world
bindings. Verify each descriptor resolves once per requested import, definition
validation performs no binary image I/O or processing, unrelated packs cannot
change numeric bindings, and shipped numeric/golden outputs remain unchanged.
Exercise both Materials sharing an Element and full unsampled-biome palettes.

### 3. Shared preparation, image processing, and transactional activation

First implement and verify the shared `engine.asset` request with non-image
processors, then supply the image processor and thin texture factory. Rework the
uncommitted image-specific lifecycle into that common owner; retain useful
processing/source/budget code and existing committed foundations.

Replace synchronous catalog textures and byte-wave machinery. Verify identical
mip output, bounded admission and aggregate limits, cancellation, partial failure,
exception draining, deterministic diagnostics, stale completion rejection, owned
storage after scratch reuse, and GPU activation/retirement. Image and content
must not retain a second implementation of the shared request lifecycle.

### 4. Consumer and format migration

Update authoring, exports, preview, startup, and checkpoint workflows. Verify
definition-only versus full-readiness behavior, clear old-format rejection,
whole-candidate pack replacement/removal, and the existing required interactive
transaction matrix. Retire the old seven-document loading path when consumers
have moved.

### 5. Reuse, documentation, and final validation

Reuse unchanged presentation results across definition edits. Verify processing
counts, source/presentation identity changes, provenance-only edits, and
whole-candidate reload behavior. Update codemaps, profiling and validation
documentation, and relevant skills to retain discoveries from this task.

Use test-suite timings to track high-level gains; report revision/environment and
variance rather than claiming controlled benchmark results. Follow the task's
PIX profiling requirement for fresh performance runs and the documented focused
capture/analysis workflow. Do not resume analysis of the canceled oversized
`Timing 1.wpix` trace. Implementation validation executes only the applicable
checks; a design assessment alone establishes no runtime or performance result.

## Execution and review

At each milestone, GPT-6.1 Sol at high effort implements and integrates the
bounded accepted design. GPT-6 Luna at medium effort handles exploration,
formatting, test execution, and profiling. GPT-6 Astra independently reviews
correctness without editing files. Serialize builds and checkpoint coherent
passing milestones, preserving existing staged and unstaged user work.

The coordinator owns design decisions. Implementation must consult before
changing ownership, schema semantics, numeric bindings, supported behavior, or
the chosen facilities. An unexplained compiler ICE or consequential feasibility
blocker is `BLOCKED_PLAN`; report evidence and stop affected production edits,
rather than trying speculative workarounds or replacing config infrastructure.
Read-only diagnosis and independent approved work can continue.

Apply the repository's validation and specialist procedures at implementation
time. Record passed evidence and unrun gates; documentation is not implementation
verification. No additional user decision is needed for this refined design's
recommended defaults, but adopting it does not authorize unrelated expansion.

## Explicit deferrals

- Universal type-erased processed-payload storage.
- Generic processing DAG or ready-job scheduler.
- Fine-grained reverse-dependency invalidation.
- Filesystem watchers and persistent caches.
- Mutation/unloading inside active pack snapshots.
- Audio and mesh processors, playback, and streaming implementation.
- New GPU managers or renderer ownership changes.
- Arbitrary concurrent external source-write support.

The two deliberate policies are name-based identity, where changing a stable key
changes identity, and stable/complete-file source replacement during requests.
These make the first implementation bounded while leaving future audio and mesh
processors an explicit integration path.
