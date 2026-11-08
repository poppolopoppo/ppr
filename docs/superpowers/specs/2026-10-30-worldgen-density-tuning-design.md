# Worldgen density and physical-scale tuning

Date: 2026-10-30
Status: implemented and verified on Linux (clang-dev); Windows not verified
Revision: 2 (resolves the builder-check findings on eligibility, absent biomes,
required features and warp-rule ambiguity)

## Intent

The generated worlds should read as the same physical world at every supported
map size. A 1024 map must not be a vertically and horizontally squeezed copy of
the 4096 reference: its biomes, cave systems, bed packages, deposits and edges
must keep the authored size in metres. Map size then selects how much of that
world is visible, not how coarse its content becomes. The same pass makes the
world visibly finer and more varied: smaller biomes, denser material changes,
more deposits, and more domain warp on the medium-frequency fields.

Biome size must remain a content-pack property. There is no runtime or
per-map procedural scale control; a pack that wants larger biomes authors a
larger site spacing.

## Goals

- Layout, warp, feature spacing, bed thickness and field frequencies are
  interpreted in metres, independent of `world_edge`.
- Population counts (sites, deposits, features, worms, connections) scale with
  map area so things-per-square-metre stays constant.
- Required biome occurrences become a per-area target: the start biome is
  always present, other required counts scale down and may reach zero on small
  maps. Optional sites are still chosen by authored weight.
- A biome that receives no site is treated as absent for generation: its
  deposits and worms are not generated and its feature placements are omitted.
  Generation must not fail merely because a small map cannot host every biome.
- Content produces smaller biomes, more frequent material banding, more and
  smaller deposits, and stronger domain warp on fields whose base frequency is
  in the 0.018–0.06 per metre band.
- Existing world/checkpoint formats, schemas and UI controls are unchanged.

## Non-goals

- No new schema field, UI slider or runtime scale multiplier.
- No change to the 3 cells-per-metre scale or to the 1024/2048/4096 size set.
- No change to the vertical composition-band schema (depth is still
  normalized 0..1).
- No change to generator algorithm version: the algorithm is unchanged and the
  content fingerprint already distinguishes the new content. Existing
  checkpoints from the old pack are rejected by content identity.

## Constraints from the current implementation

- `kWorldEdge` is 4096 and the pack is authored at that reference.
- `compileRecipe` currently multiplies `site_spacing_m`,
  `boundary_warp_amplitude_m`, biome `distance_min_m`/`distance_max_m`,
  feature `spacing_m`, `surface_elevation_m`, `surface_amplitude_m` and start
  depth by `world_edge / kWorldEdge`.
- `validateLayout` requires biome distances to lie within `0..2 * world_m`.
  Authored `distance_max_m = 2048` is an unbounded marker at the reference but
  is invalid at 1024/2048 once distances stop scaling.
- Biome site placement enforces a minimum centre distance of `site_spacing_m`;
  required occurrences are assigned by augmenting paths, optional sites by
  weighted draw. A site with no eligible biome is a hard failure.
- Every feature placement in the shipped pack has `min_count = 1`; a required
  placement that cannot find its biome is a hard failure.
- `blobs()` and `worms()` iterate active biomes and hard-fail when a biome has
  no site or no sampleable deposit region.
- Material bands are chosen by normalised depth and a field, then split into a
  periodic bed package by `bed_period_m` and `bed_warp_m`.
- Field sampling is domain-warped with an independent warp frequency and
  amplitude in metres.

## Approaches considered

### 1. Fixed horizontal scale, area-scaled counts (chosen)

Remove extent scaling from physical horizontal dimensions. Keep positions and
population counts area-scaled, and make required biome counts and feature
minimum counts area-scaled too. Small maps become correctly-scaled windows that
may lack rare biomes. Vertical surface elevation and start depth stay
extent-scaled so the surface remains in frame.

### 2. Fixed horizontal scale, guaranteed biome variety

Same as 1, but every biome keeps at least one required occurrence. This forces
site spacing down to roughly 70 m so thirteen required sites fit in a 341 m map,
making the smallest map 1.5–2× denser than the reference, and requires
relaxing distance minima. Rejected: it reintroduces map-size-dependent content.

### 3. Content-only compressed copy

Keep the current scaling and only retune frequencies. Rejected: biomes and
edges would still shrink with map size, which is the behaviour being removed.

## Chosen design

### Recipe lowering

In `compileRecipe` (`lib/engine/sim/worldgen/Sim.WorldGen.Definition.cpp`) for
a non-reference extent:

- Remove scaling of `site_spacing_m` and `boundary_warp_amplitude_m`.
- Remove scaling of biome `distance_min_m` and `distance_max_m`. Clamp
  `distance_max_m` to `2 * world_m` after lowering; this expresses the authored
  "unbounded" value on smaller worlds without changing the authored minimum.
- Remove scaling of feature `spacing_m`.
- Scale `required_count` by area with `std::lround` before computing mandatory
  sites. The start biome still consumes one occurrence as today. A scaled
  requirement of zero leaves the biome available through the weighted optional
  draw.
- Compute total site budgets from the authored totals, not from an optional
  remainder:
  `min_sites = max(mandatory, lround(authored_min * scale²))` and
  `max_sites = max(min_sites, lround(authored_max * scale²))`.
  `scale = world_edge / kWorldEdge`.
- Scale both feature `min_count` and `max_count` with `std::lround`, and floor
  `max_count` at one when the authored maximum is non-zero. At the reference
  this is unchanged; at 2048/1024 feature instances become optional rather than
  hard-required.
- Keep scaling start X/Y fine coordinates, surface elevation, surface
  amplitude and start depth. The vertical column is truncated with the map
  while metres-per-bed and field frequencies stay physical.
- Keep area scaling for deposit blob counts, feature counts, worm counts and
  connection endpoint counts.

No other recipe-lowering phase changes.

**Builder clarification.** Reject layout candidates with no eligible biome before
admitting them to the site pool. The interrupted implementation already carried
this bounded rejection using the existing eligibility predicate. It prevents an
unassignable optional site from failing an otherwise reachable small-map layout;
spacing, required matching and weighted assignment retain their existing rules.

### Absent-biome geometry policy

A biome is present when the compiled layout contains at least one site of that
biome. The start biome is always present. In
`lib/engine/sim/worldgen/Sim.WorldGen.Geometry.cpp`:

- `blobs()` skips a biome with no site instead of failing on an empty origin
  region.
- `worms()` skips a worm rule whose biome has no site instead of failing its
  origin search.
- Feature placements whose biome is absent are already optional at
  2048/1024 because their `min_count` lowers to zero; their failed attempts are
  recorded in `m_omitted_instances` as today. No extra branch is required at
  the reference, where every biome keeps its requirement.

Connections have no biome reference and need no change. Matter fills reference
placements and naturally produce nothing when those placements have no
instance. A biome that is present but whose deposit region is genuinely empty
still fails loudly; this policy only removes failures caused by an absent
biome.

### Content tuning rules

All values below are authored in the shared pack and therefore apply to both
`frontier` and `hollows`. Field assets are shared by both worlds.

**Field frequencies.** Multiply every `frequency_per_metre` by 1.4. This is a
uniform pass, from `surface_height` 0.006 → 0.0084 and `warp_x`/`warp_y`
0.0045 → 0.0063 up to `mineral_edges` 0.11 → 0.154. No frequency is close to
the coordinate limit. `warp_frequency_per_metre` is unchanged.

**Warp amplitude.** Select fields whose *authored base* frequency is in the
closed range 0.018–0.06 per metre and which already have a non-zero
`warp_amplitude_m`. Multiply the amplitude by 1.5 and cap it using the
post-1.4 frequency `f'`:

- non-cellular fields: `cap = 0.55 / f'`
- cellular fields (`cellular_value` or `cellular_distance`): `cap = 0.80 / f'`

Never reduce an amplitude below its current value. Fields outside the band keep
their amplitude. Representative results:

| Field | Base | Old amp | New amp |
|---|---:|---:|---:|
| `beds` (cellular) | 0.018 | 28 | 31.7 |
| `branch_caves` | 0.025 | 11 | 15.7 |
| `caverns` | 0.021 | 8 | 12 |
| `hot_chambers` | 0.018 | 12 | 18 |
| `karst_cells` | 0.018 | 14 | 21 |
| `karst_masses` (cellular) | 0.045 | 13 | 13 |
| `low_pools` | 0.018 | 9 | 13.5 |
| `mire_masses` (cellular) | 0.022 | 25 | 26 |
| `organic_beds` (cellular) | 0.025 | 22 | 23 |
| `ore_seams` | 0.027 | 8 | 12 |
| `round_caves` | 0.022 | 10 | 15 |
| `salt_beds` | 0.011 | 14 | 14 |
| `tall_caves` | 0.030 | 5 | 7.5 |

Fields above 0.06 (`blob_edges`, `organic_edges`, `mineral_edges`,
`ore_lenses`) and below 0.018 keep their amplitudes.

**Bed packages.** Multiply every non-zero `bed_period_m` and `bed_warp_m` by
0.7 and round with `std::lround`, minimum 1 m. Zero periods stay zero. This
makes layered material change roughly 40% more often while keeping the
authored warp-to-period ratio.

**Deposits.** Multiply blob `count` by 1.3 and round up, both `min_m` and
`max_m` radii by 0.8 (rounded to 0.1 m, minimum 0.5 m), and
`sprinkle_probability` by 1.2. Coverage fractions and depth ranges are
unchanged.

**Detail probabilities.** Multiply biome `pore.probability` by 1.1,
`formation.probability` by 1.2, and `sprinkle.probability` by 1.15. Pass
counts, radii, rind and surface layers are unchanged.

**Biome distance minima.** The authored minima currently reach 500 m, which is
outside the reachable radius of a 341 m map and would make sites unassignable.
Lower them so every depth band is reachable on the smallest map while keeping
their relative order:

| Biome | Frontier | Hollows |
|---|---:|---:|
| `hearth` | 0 | 0 |
| `verdant` | 70 | 70 |
| `mire` | 80 | 70 |
| `dunes` | 90 | 90 |
| `rime` | 100 | 100 |
| `lode` | 90 | 90 |
| `karst` | 60 | 50 |
| `saltworks` | 80 | 70 |
| `fumaroles` | 90 | 90 |
| `petroleum` | 100 | 100 |
| `ember` | 110 | 110 |

`distance_max_m` stays 2048 in the pack and is clamped at lowering. Depth
ranges, weights and required counts are otherwise unchanged.

**World profiles.**

| Profile | min_sites | max_sites | site_attempts | site_spacing_m | boundary warp |
|---|---:|---:|---:|---:|---:|
| `frontier` | 120 | 150 | 100 | 80 | 28 |
| `hollows` | 135 | 170 | 100 | 72 | 34 |

Boundary warp amplitude is reduced modestly because biomes are smaller; the
warp fields themselves run at the new 0.0063 frequency, so edges remain
irregular without consuming a third of a biome.

At the 4096 reference this yields 120–150 sites for `frontier` (was 64–80) and
135–170 for `hollows` (was 72–96): about 1.9× the site density and roughly 27%
smaller biome diameters. With the rounding rule above the expected total-site
budgets are:

| Extent | Frontier | Hollows |
|---|---:|---:|
| 4096 | 120–150 | 135–170 |
| 2048 | 30–38 | 34–43 |
| 1024 | 8–9 | 8–11 |

## Consequences

- Seed-for-seed terrain changes everywhere; content identity and all preview
  checksums change. Old checkpoints are rejected by content fingerprint.
- A small map may contain only a subset of biomes. Deep biomes are reachable
  because their distance minima are lowered, but they can still lose the
  weighted draw; when they do, their deposits, worms and landmarks are omitted
  rather than failing generation.
- The vertical column is shorter on smaller maps. Because depth bands are
  normalized, shallow bands occupy fewer metres there, although bed thickness
  and field frequencies remain physical. This is documented, not corrected.
- More sites and blobs increase generation work. The diagnostic phase timings
  are the evidence, not a claim of unchanged cost.
- Existing tests that pin the extent scaling of spacing, distance, feature
  minimum counts or site budgets must be updated.

## Verification

- Unit tests: update `selected_world_recipe_scales_layout_and_preserves_physical_contracts`
  for fixed spacing, fixed warp, fixed distance minima, the distance-max clamp,
  area-scaled required counts, total site budgets and optional feature minimum
  counts. Add a case that lowers and compiles the shipped-like profile at
  1024/2048/4096 and asserts the site budgets.
- Layout robustness: compile the layout at 1024 for at least eight seeds and
  assert success with a full site budget. Add a geometry test where a biome is
  absent and assert blobs/worms/placements omit rather than fail.
- CLI: `app.game --validate-worldgen` at 1024, 2048 and 4096 for both
  profiles.
- CPU previews: `--worldgen-preview` for `frontier` and `hollows` at 1024,
  2048 and 4096, seed 42. Check for correctly sized biomes, no boundary
  slivers, no warped-field artifacts, visible material variety, and no
  validation or generation failure. Compare phase timings to the pre-change
  run.
- Formatting and style gates on touched C++.

## Open decisions left to the builder

- Exact per-field values are generated by the rules above; ties in rounded
  values may be resolved to one decimal without changing intent.
- The preview review decides whether the 1.4 frequency and 1.5 warp factors
  are too strong; adjustments stay inside the same rules.
- Whether to commit the content and code as one change or split lowering from
  content tuning; the user's commit workflow decides.
