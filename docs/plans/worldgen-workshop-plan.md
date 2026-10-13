# Worldgen Workshop — proposed implementation plan

Status: prepared by GPT-6 Astra from the current repository; implementation requested by the user after visual acceptance of the generation changes. Sol owns implementation and integration; Luna explores; Astra reviews without editing. Builds and executable checks remain serialized.

## Goal and first release

Provide an in-game developer workspace to browse the complete content pack, edit biome settings, generate a small deterministic sample, and save authored settings to disk. Opening this workspace must not require generating a complete 4096-square terrain map.

The first release includes a searchable browser for packs, materials/elements, fields, biomes, features, world profiles, backgrounds, and referenced textures. It edits existing biome definitions and their nested composition/deposit records. Other categories initially provide inspection and cross-reference navigation; broader editing follows in a second increment.

“Elements” are the existing numeric terrain/particle IDs and material identities, including reserved vacuum; there is no separate authored element document. “Packs” means explicitly opened manifest roots, not a new mod registry or pack-composition system.

## User flow

Open Workshop from the colony UI, or launch directly into it before world generation. Pause the active simulation and clear held gameplay input when entering authoring mode.

Use a content browser, property inspector, and the existing main scene viewport. The toolbar contains pack root, profile, seed, Preview, Cancel, Save, Reload, and dirty/validation status. Preserve the seed, sample position/depth, camera, zoom, and last successful preview across edits.

Show authored keys, numeric IDs where applicable, source file/location, references, and “used by” links. Clicking a preview cell selects its material; reference controls navigate to the owning definition. Include sample dimensions, material counts, and solid/vacuum fraction for comparisons.

Biome controls cover composition weights and ordered beds, field/material references, pores, relaxation/finishing/erosion passes, lining, formations, deposits, and temperature. Show units, defaults, and validation. Cave thresholds and worms that belong to world-profile rules appear as linked rules with their actual owner, rather than invented biome properties.

Start with explicit Preview, Undo/Redo, and Revert. Add debounced live regeneration after measuring responsiveness. Keep keys and numeric IDs stable initially; renaming, deletion, cloning, and pack creation require a later authoring increment.

## Preview design and limits

Start with a **Biome sample**, not an exact crop of a generated world. Use a deterministic fixed sample domain, selected biome and profile rules, and chosen global position/depth. A starting size of 256×256 fine cells is approximately 85×85 metres at the existing metric; larger selectable sizes follow measurement.

Reuse production geology, caves, pores, material filters, detail, appended prototype erosion, and formation evaluation. Build an explicit preview context without modifying the authored pack to force a biome into the sample. Show which profile rules participate. Allow explicit feature placement for inspection later.

Preserve global noise/random coordinate anchoring. Rasterize support outside the visible sample and crop only for presentation. The existing 24-cell filter halo is only part of the required support: formation roots, support checks, footprints, and rendering seams also need context. Panning within a generated sample must not change its contents.

### Formation prerequisite

`GenerationRaster::chunk` evaluates local terrain, but does not produce final formations by itself. `finishGenerationTerrain` scans formation origins globally, admits the first 4096 valid records, and resolves overlapping writes in stable order. A naive chunk crop would omit or change formations.

Separate shared formation discovery, validation, and application from the whole-world traversal while preserving production behavior. The laboratory uses a documented fixed local candidate domain and the same formation rules. Label it **Biome sample**. Do not claim exact full-world equivalence.

Do not run whole-world backbone/start/resource or connected initial-matter validation on a sparse sample: missing chunks currently behave as empty cells. Initial matter and global playability remain outside the laboratory’s validation scope.

Exact world-crop preview is a later capability: retain production macro layout and geometry, raster only required support chunks, and solve global formation admission/order through sparse discovery or a separately approved generator-version change.

## Existing reuse points

- `loadContentPackSource`, typed decoders, `loadContentCatalog`, and `compileWorldPlan` supply owned source, validation, reference resolution, texture validation, and content identity.
- `compileBiomeLayout`, `compileGenerationGeometry`, `compileGenerationRaster`, and `GenerationRaster::chunk` supply generation context and local evaluation.
- `GridPass::GridStagedFrame`, `SimFramePacket`, and `buildGridStagedFrame` supply owned rendering payloads and material/biome/temperature views.
- `WorldJob`, `ParallelScheduler`, `IContext`, and frame results supply patterns for cancellation, bounded work, and publication.

Use a separate owned preview result. `PreparedWorld`, `GeneratedWorld`, and `ColonyDriver` represent complete worlds; a partially populated instance would violate their contracts.

Present through the real GridPass/main viewport to reuse textures, normals, PBR properties, and contours. Extract catalog-to-presentation conversion only where both callers require it. Arbitrary `ImGui::Image` handles are not a shortcut: detached UI rejects unknown texture identities.

## Source drafts and disk saves

Keep original disk bytes, editable source draft, and last validated catalog/preview separate. Runtime catalog structures are immutable resolved data, not a lossless serialization model.

Add a source-backed catalog-loading seam in `Content.Pack.cpp`; disk loading remains a wrapper. Draft validation must use the same decoder, asset-reference, and fingerprint pipeline without temporary packs. Retain original manifest bytes too: the current loader discards `pack.toml` bytes after decoding.

Extend the existing private TOML codec with narrowly scoped parser-derived source spans and value replacement/insertion. `config::Document` currently keeps flattened values plus origin/line, and `config::encode` canonicalizes text. Serializing that representation would lose comments/layout. Patch only changed values; preserve untouched bytes, comments, line endings, omitted defaults, and textures. Reparse and validate the patched draft.

Initially, Save commits one edited document after validating it against the current on-disk rest of the pack. Do not pretend several file replacements form an atomic pack transaction. Retain dirty drafts on failure.

Before saving, compare current disk bytes with the loaded baseline. On external changes, offer Reload or retain/export the draft; never silently overwrite. Write a sibling temporary file, verify completion, then use a verified atomic replacement primitive. Check existing filesystem/display-persistence facilities and their platform guarantees. Clear dirty state only after replacement and successful reread.

Show the exact editable root. Repository `assets/worldgen/default` is authored source; executable-adjacent `worldgen/default` is staged output that a build may overwrite. Never infer the source root from a deployed directory. An explicitly opened source pack can be previewed immediately; rebuilding later stages it for ordinary execution.

## Exporting preview results

Provide **Export Preview…** independently of Save. Export the last completed preview revision, including unsaved edits that produced it; never combine an older image with a newer draft.

Reuse the P6 PPM writer, material/biome colors, and temperature ramp in `WorldGen.Export.cpp` through a narrow region-export helper consuming the owned preview result. The current exporter requires a complete `GeneratedWorld` and hard-codes whole-world output, so it cannot be called unchanged. Export `material.ppm`, `biome.ppm`, and `temperature.ppm` for the visible interior only, initially one pixel per fine cell with the existing +Y-up convention.

Write `preview.json` containing sample semantics, generator/export versions, pack/profile/biome, full seed, sample/support bounds, metric, participating rules/overrides, formation-domain semantics, content fingerprint, cell checksum, sampling dimensions, and camera/display settings. Include the exact validated manifest and six-document source snapshot used by that preview under `source/`. The smaller first release records referenced asset identities and explicitly requires the original unchanged asset root; a self-contained asset bundle can follow.

Export off-main from immutable owned data into a fresh directory, writing a completion manifest last. Report failure/cancellation and the partial-output location. When textured presentation lands, export a matching `textured.ppm` through asynchronous render-owned readback after GPU completion; do not invoke the blocking diagnostic capture host from the UI thread.

## Ownership and jobs

The game client owns the authoring session. Panels emit intents; they do not perform disk I/O or GPU work. Use a small preview coordinator patterned after `WorldJob`, whose existing callback specifically produces a complete `ColonyDriver`.

Allow one running request and one replacement request. Requests own immutable draft snapshots, revision IDs, and cancellation contexts. Newer requests cancel older work; publish only the matching current revision. Keep the previous successful preview visible with an out-of-date indicator on failure or cancellation.

Use the injected worker pool and clock. Avoid blocking scheduler admission or joins on the UI thread. Shutdown disables requests, cancels/drains CPU work, retires render payloads through acknowledged/fenced ownership, and only then releases catalogs and sources.

## Proposed file boundaries

- `game/colony/Content.Authoring.cppm/.cpp`: source drafts, edit commands, browser references, persistence.
- `game/colony/WorldGen.Preview.cppm/.cpp`: preview specification, owned sample, generation preparation.
- `game/colony/WorldGenPanel.cppm/.cpp`: ImGui views and intents.
- Game application glue for session/job ownership and optional direct Workshop startup.

Existing integration points include `Game.Application.cpp`, `Game.Config.*`, `Content.cppm`/`Content.Pack.cpp`, `WorldGen.Plan.cpp`, and `SimFramePacket.cpp`. Register new source files with their CMake changes and update maps/docs. Numeric region/formation work stays in `engine.sim`; source-preserving TOML support stays behind `engine.core:config.file`. Renderer and engine.app gain no game/content ownership.

## Delivery sequence and acceptance

1. Browser, source drafts, validation, lossless save/reload, Undo/Redo.
2. Deterministic local numerical preview, including shared formation evaluation and explicit sample semantics.
3. Asynchronous real GridPass presentation, cancellation, stale-result rejection, and safe close/retirement.
4. Richer material/field/profile editing and measured optional live regeneration.
5. Exact world-crop preview after global formation semantics are resolved.

Verify byte-preserving saves and external-change conflicts; invalid references/assets/drafts; seed replay and seams; formation roots outside the visible sample; cancellation/stale completion/close races; bounded jobs/resident chunks; actual textured preview and repeated resource retirement. Compare pre-formation crop cells against full generation; test formation equivalence only under the explicitly defined sample semantics.

Export acceptance covers orientation/dimensions, excluded halos, matching image/result revisions, reproduction from the exported source snapshot, and failed/cancelled output handling.

Use existing suites and repository format/style/build gates. Presentation integration requires the full app/asset matrix and applicable three-loop lifetime/concurrency checks. Measure latency, responsiveness, and CPU/GPU memory before claiming speed.

User acceptance: edit a biome, preview at fixed seed/camera, compare, save, reload/restart, and reproduce the result. The central scope decision for review is whether the first release should inspect all categories but edit biomes first, or include material/field/profile editing immediately.
