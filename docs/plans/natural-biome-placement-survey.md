# Natural biome interiors: generator survey and proposed direction

Status: research and authoring proposal, 2026-10-12. No generator or content
configuration is changed by this document. Proposed dimensions are starting
points for visual tuning, not measured results or geological constants.

## Approved first implementation, 2026-10-13

Implement the narrower sequence below for Hearth, Lode and Karst first. The
broader survey that follows is a menu of later possibilities, not the current
implementation scope.

1. Add one finite, distorted, stretchable blob primitive for secondary host
   bodies, pockets and lenses, with independent positions, dimensions and seeds.
2. Extend cave worms with smoothly changing direction and width, plus occasional
   chambers. Keep these controls opt-in for existing recipes.
3. Add bounded, seeded multi-material neighborhood relaxation with separate
   read/write states and consistent cross-chunk neighbors. Protect barriers,
   reservations and required connections. Tune the pass count visually rather
   than copying the prototype's 21 iterations unchanged.
4. Apply graded mineral envelopes and restrained local sprinkle after relaxation
   so fine deposits survive. Finish protected content and particle initialization
   in the established safe order.

Defer WFC, branching vein networks, alteration halos, selective dissolution and
surface deposition. Use existing nonvisual contract checks; visual acceptance
belongs to the user. Do not add complicated one-off test harnesses without asking.

## What is producing the current appearance

The current material picker converts a smooth field to a value in `[0,1]`, then
chooses a material using cumulative weights. Those weights divide the field's
value range; they do **not** imply the same proportions of world cells. Adjacent
materials consequently follow contours of one field. Thin value intervals
produce ribbons even when the field itself is complicated.

The problem is particularly visible in Lode: six granite-hosted deposits select
different narrow intervals of the same `ore_seams` field. These are correlated
contour bands, not six independently placed mineral deposits. Hearth similarly
shares `ore_grain` between clay and copper; Karst shares it between crystal and
diamond. Several biomes reuse the same upper/lower host weights, 8:2.5 and 6:3.5.

Most Frontier cave worms also reuse count 7, length 12–50 m, radius 0.8–1.8 m,
and turn probability 0.12. The implementation walks eight compass headings with
fixed segment radius. Ore worms are long, constant-width paths as well. Changing
noise seeds alone cannot remove these common shapes.

Worm counts apply across the eligible biome's world distribution, not separately
to each biome region. Host-filtered deposit rules test the progressively replaced
material, making overlapping deposits order-sensitive. Current threshold caves
filter by biome/depth, not by soluble host material.

There is no erosion, local material scatter, or cave-surface deposition pass.
Existing biome lining marks biome ownership boundaries, **not cave walls**.

Repository evidence:

- [Material selection and cave rasterization](../../lib/engine/sim/worldgen/Sim.WorldGen.Raster.cpp): `baseMaterial`, cave thresholds, biome halo/lining.
- [Worm geometry](../../lib/engine/sim/worldgen/Sim.WorldGen.Geometry.cpp): `compileGenerationGeometry` and worm construction.
- [Biome recipes](../../assets/worldgen/default/biomes.toml), [world rules](../../assets/worldgen/default/worlds.toml), [noise fields](../../assets/worldgen/default/fields.toml).

## Established methods worth adapting

| Reference | Established approach | Application here |
|---|---|---|
| [Luanti ore API](https://api.luanti.org/ores/) | Separate scatter clusters, distorted blobs, sheets/puffs, veins and strata; veins can include random occupancy. | Give materials distinct deposit families rather than assigning all of them field intervals. |
| [Luanti ore definitions](https://api.luanti.org/definition-tables/#ore-definition) | Host replacement, biome restrictions, depth limits, cluster size and scarcity are separate controls. | Express where a mineral is allowed independently from its shape, abundance and grade. |
| [Minetest Game's actual mapgen recipes](https://github.com/luanti-org/minetest_game/blob/master/mods/default/mapgen.lua) | Non-ore material blobs precede host-filtered scatter ores; depth bands use different cluster recipes. | Establish rock bodies first, then selectively mineralize eligible hosts. Rule order changes the result. |
| [Minecraft's large ore veins](https://feedback.minecraft.net/hc/en-us/articles/360060534911-Minecraft-Java-Edition-Snapshot-21w16a) | Rare, long mineral formations mixed with companion rock; copper/granite and iron/tuff are distinct combinations. | A vein should contain host/gangue, gaps and richer pockets, rather than a solid cable of ore. |
| [RogueBasin cave cellular automata](https://www.roguebasin.com/index.php/Cellular_Automata_Method_for_Generating_Random_Cave-Like_Levels) | Local neighbor rules turn random occupancy into connected-looking masses and openings. | Use a small, bounded relaxation pass for cave silhouettes, with protected features and connections. |
| [Bracket Productions' random-walk caves](https://bfnightly.bracketproductions.com/book/chapter_28.html) | Random walkers with controlled lifetime, starting mode and target floor coverage generate differing cave styles. | Retain worms, but vary their width, persistence and branching by biome and by individual path. |
| [Red Blob's noise-generation discussion](https://www.redblobgames.com/maps/terrain-from-noise/) | Noise is a useful building block with practical limits as a complete generator. | Use fields for suitability and deformation; use explicit deposit objects for composition. |

Luanti's intersecting-noise vein construction is three-dimensional. My inference:
copying it directly into this 2D cross-section would change the geometry of the
intersection, often producing isolated crossings rather than long veins. Adapt
the idea of graded mineralization, not the formula unchanged.

## Plausibility without a geology simulator

Real solution caves enlarge fractures and bedding in soluble hosts through
chemical dissolution and physical erosion. This supports connected limestone
passages, junction chambers and localized widening instead of uniform tubes.
[National Park Service](https://www.nps.gov/subjects/caves/solution-caves.htm)

Hydrothermal mineralization can associate veins with altered surrounding rock;
replacement deposits include lenses and pipes in favorable hosts near intrusions.
These justify a few related material groups and halos, rather than unrelated ores
painted everywhere. [USGS hydrothermal overview](https://www.usgs.gov/publications/porphyry-and-epithermal-mineral-deposits),
[USGS replacement-deposit model](https://pubs.usgs.gov/bul/b1693/html/bull7q0k.htm)

These are visual relationships to borrow. Our elemental catalog and gameplay
resources remain stylized; this proposal does not claim literal geological
accuracy for every material.

## Recommended placement recipe

The ONI prototype already supplies a useful reference: `Generator::median`
in [oni_v22.slang](../oni_v22.slang) selects the most common element among eight
neighbors and adopts a different winning element with 50% probability. Its
generation pass repeats this 21 times before landmarks. This is stochastic
multi-material relaxation: it shapes material interfaces as well as cave walls.
Leaving it out of the initial proposal omitted a key contributor to the desired
appearance. The iteration count and probability are tuning references, not
fixed requirements at our different cell scale.

1. **Host bodies.** Start with a coherent principal rock and a few finite secondary
   bodies. Reserve continuous bedding for sedimentary biomes. Break beds into
   lenses, vary their thickness, and avoid giving every biome the same split.
2. **Deposit locations.** Choose reproducible deposit centers from biome, depth,
   host and a broad suitability field. Allow both rich districts and quiet areas.
   Use a distribution of sizes, with many small deposits and occasional larger
   ones; avoid equal-sized patches at regular spacing.
3. **Deposit shapes.** Use compact distorted blobs for pockets, tapered lenses for
   beds, and short branching paths for fracture minerals. Vary orientation,
   width, length and fragmentation independently. Initial scales to try:
   pockets 1–6 m across, lenses 4–20 m long, ordinary veins 8–40 m long, with
   occasional larger connected systems. Tune to the actual viewing scale.
4. **Erosion/relaxation.** After rough host bodies, broad deposits and cave carving,
   run configurable, seeded neighborhood relaxation over eligible bulk materials
   and empty cells. Break up protrusions and soften synthetic material contours
   together. Protect biome barriers and reserved cells. Read the previous pass
   and write the next using separate buffers, with consistent neighbors across
   chunks; do not copy the prototype's in-place workgroup synchronization as a
   world-wide synchronization scheme.
5. **Grade and sprinkle.** A deposit is an envelope, not solid mineral. Give it a
   richer irregular core, a lower-density fringe and small nearby satellite
   clusters. Sample individual eligible cells with coordinate-seeded randomness
   conditioned on that envelope. Keep a low background trace population. Do not
   scatter every element uniformly across the whole biome. Apply fine veins,
   rare inclusions and sparse scatter after bulk relaxation, or explicitly
   protect them, so repeated passes do not erase their intended scale.
6. **Selective cave weathering and surface deposits.** Modify the cave mask locally: remove
   selected fragile protrusions, widen some narrow soluble-rock passages, and
   preserve hard inclusions. Place loose sand, clay, pebbles, ash or snow on
   eligible supported floors and shelves; reserve wall or ceiling coatings for
   materials and local conditions that justify them. Bound the modified depth
   so cave interiors do not become a broad material blur.
7. **Protected gameplay content and particles.** Preserve biome barriers and
   reserved landmarks; establish final connections and stamped structures after
   destructive cave shaping. Apply surface deposits and particle fills in an
   explicit order, respecting reservoirs and protected surfaces.

The initial erosion stage uses the prototype's visual relaxation technique,
not a hydraulic simulation. Material eligibility, protected cells and the order
of fine-detail placement control unwanted homogenization. Binary cave-mask
weathering can supplement this stage; it does not replace relaxation of material
interfaces. This proposal adds no runtime fluid dynamics.

## Biome-specific direction

The following are proposed visual rules using existing catalog materials, not
claims that these combinations already occur in the current recipes.

| Biome | Proposed host, deposits and cave treatment |
|---|---|
| Hearth | Sandstone with discontinuous dirt/clay lenses; modest copper pockets and broken coal seams. Gentle chambers with small pebbled floors. |
| Verdant | Broad dirt bodies around limestone; clustered algae near wet exposed surfaces, sparse phosphorite pockets. Uneven connected galleries. |
| Mire | Clay/slime masses with wet basins and irregular algae fringes. Prefer lobed pockets and short connections over long parallel strands. |
| Dunes | Sandstone/shale beds with thinning and gaps; sand lenses and floor accumulations. Keep some directional layering without repeating it everywhere. |
| Rime | Coherent ice masses cut by granite bodies; snow on exposed upper surfaces and low shelves, polluted-ice pockets. Irregular fractures and cavities. |
| Lode | Granite host with basalt intrusions; distinct iron/copper ore clusters and branching veins, quartz-associated pockets, rare wolframite/diamond. Leave substantial barren host between districts. |
| Karst | Limestone-dominant masses with connected, selectively widened passages. Quartz/crystal pockets near fractures and cavities; diamond exceptional, not a neighboring contour stripe. |
| Saltworks | Salt/limestone beds interrupted by lenses and dissolution cavities. Local brine reservoirs; vary bed thickness and terminate some layers. |
| Fumaroles | Basalt/obsidian bodies with narrow fissures, sulfur concentrations and sparse ash around vent regions. Local halos rather than sulfur ribbons throughout the biome. |
| Petroleum | Shale with finite sandstone bodies and intact caps. Place crude oil/natural gas in appropriate sealed spaces; cave carving must respect those caps. |
| Ember | Basalt flows and localized obsidian margins, ash/regolith pockets and irregular lava passages. Keep magma tied to reservoirs and conduits. |

## What can be configured now, and what needs support

**Available now:** independently seeded/deformed fields; fewer broad material
intervals; different host-selection weights; independent deposit fields; host/depth
filters; different worm scales and turn rates; more varied feature templates and
placement counts. These can reduce correlation immediately, but cannot implement
the complete recipe above.

**Proposed engine additions:** finite host-filtered cluster/blob
deposits with size and density controls; deterministic graded sprinkle; variable
width, smoothly changing headings and branching in vein paths; bounded,
deterministic multi-material erosion/relaxation with material eligibility;
selective cave-mask weathering; surface
orientation/distance conditions for floor and wall deposits. New configuration
must remain canonical, bounded, cancellable and independent of chunk traversal
order. Neighbor-dependent work needs an explicit halo or whole-region stage so
chunk seams do not become geological boundaries.

Include bulk erosion/relaxation in the first visual iteration alongside host
composition, finite deposits and sprinkle. The proposed relaxation-before-fine-
detail-and-features order needs an explicit generation-stage change; it is not
an existing configuration switch.

Recommended first visual iteration: Hearth, Lode and Karst. They distinguish
sedimentary lenses, mineralization and dissolution cavities. Compare a few fixed
seeds at region and digging scales using the existing previews. Let the user
judge the images before expanding to all eleven biomes. Do not create a complex
one-off validation harness without asking first.
