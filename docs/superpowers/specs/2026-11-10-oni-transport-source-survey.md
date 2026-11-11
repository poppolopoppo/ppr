# Transport-source representation investigation

Date: 2026-11-10
Status: **Initial source survey complete; isotropic RGB9E5 source model approved.**

## Question and owner constraint

The owner rejected the proposed 32-byte PBR transport-material record evaluated
at every ray hit. Probe update was the bottleneck in the earlier implementation.
The intended direction is simpler transport with a potentially precomputed,
RGB9E5-packed per-cell source and geometry visibility handled by acceleration.
Normal-mapped, textured on-screen PBR remains required. Historical performance
gates remain retired; no speedup is claimed from this source investigation.

Survey the original Radiance Cascades and Holographic Radiance Cascades papers
and author implementations before selecting the simplified bounce model. A
dedicated read-only research sublane gathered the online evidence. The coordinator
also directly checked the pinned RC traversal and HRC feedback/normalization code.

## Primary references and evidence limits

- [Sannikov RC manuscript](https://github.com/Raikiri/RadianceCascadesPaper/blob/9d1fd290bb08f2589024ce83b75a7cc3323c6a4d/RadianceCascades.tex),
  especially Radiance Interval, Calculating indirect lighting, Radiant 2d and
  Path of Exile 2 GI. This is an unpublished WIP, despite the JCGT template.
- [Author flatland exploration, Scaling.ls](https://github.com/Raikiri/LegitCascades/blob/82993030166be694004fee806c81d9cc2e496316/Scaling.ls).
  This is an author implementation, not established as the exact original Radiant
  2d or production PoE2 implementation. Production PoE2 shaders were unavailable.
- [Holographic Radiance Cascades paper, arXiv v1](https://arxiv.org/pdf/2505.02041v1),
  sections 3, 4.2 and 5.
- [HRC author implementation, Amitabha](https://github.com/entropylost/amitabha/tree/38b7ee2364807a4523af6de93cbb2871968c3b85).
  Inspected scene, color, fluence, trace, voxel traversal, render and multibounce
  example source. Together with Scaling.ls this was an eight-source-file survey.

### What the inspected implementations actually do

| Aspect | RC author flatland example | HRC author implementation |
| --- | --- | --- |
| Source | Isotropic RGB source plus opacity in a scene image | Separate isotropic RGB emission/source-function and RGB extinction images |
| Interval | RGB radiance + scalar transmittance, explicit RGBA16F atlases (8 bytes) | RGB half radiance + RGB half transmittance; six half components (12-byte payload), actual backend allocation stride not verified |
| Traversal work | Sample scene source/opacity and compose | DDA through homogeneous cells, integrate source/extinction over segment length and compose |
| Hit BRDF | None | None |
| Bounce feedback | Not present in this example; previous cascade atlas is not reflected-light feedback | Sum four incoming quadrant contributions, multiply by RGB diffuse factor, write the isotropic source for the next solve |
| Final shading in inspected demo | Average directional radiance | Sum four incoming quadrants; no final normal-mapped PBR shader |
| Metallic/specular transport | Not demonstrated | Not implemented in the inspected multibounce model |

Neither inspected implementation establishes RGB9E5 as its chosen format. RGB9E5
is a proposed PPR adaptation for nonnegative cached radiance, not a copied author
storage contract. HRC's RGB extinction and interval transmittance are unnecessary
for PPR's approved binary/no-tint transport model; do not import them by analogy.

### Direct source excerpts

[RC traversal, lines 563-581](https://github.com/Raikiri/LegitCascades/blob/82993030166be694004fee806c81d9cc2e496316/Scaling.ls#L563-L581):

```glsl
vec4 ray_sample = textureLod(scene_tex, uv_pos, 0.0);
radiance += ray_sample.rgb * transmittance * ray_sample.a;
transmittance *= (1.0f - ray_sample.a);
```

Scene alpha is opacity; interval alpha is transmittance. The scene and cascade
atlases are explicitly RGBA16F at lines 271-277. The example supplies an emitting
circle and black opaque circle, not a shaded material scene.

[HRC homogeneous-cell integration](https://github.com/entropylost/amitabha/blob/38b7ee2364807a4523af6de93cbb2871968c3b85/src/color.rs):

```rust
let transmittance = (-color.opacity * segment_length.cast_f16()).exp();
Fluence::expr(color.emission * (f16::ONE - transmittance), transmittance)
```

Here opacity means extinction coefficient, not ordinary alpha. Emission behaves
as a source function S in S * (1 - exp(-sigma * distance)), approaching returned
radiance in the opaque limit. There is no normal or outgoing-direction argument.

[HRC multibounce feedback, lines 157-171](https://github.com/entropylost/amitabha/blob/38b7ee2364807a4523af6de93cbb2871968c3b85/src/bin/multibounce.rs#L157-L171):

```rust
for i in 0_u32..4_u32 {
    *radiance += renderer.radiance.read(cell.extend(i));
}
let radiance = base_fluence.over_radiance(**radiance);
let diffuse = diffuse_tex.read(cell);
if (world.read(cell).opacity < f16::from_f32(200.0)).any() {
    let emission = diffuse * radiance;
    world.write_emission(cell, emission);
}
```

[Angular restriction](https://github.com/entropylost/amitabha/blob/38b7ee2364807a4523af6de93cbb2871968c3b85/src/fluence.rs#L110-L113)
scales contributions by angle / TAU. The summed result uses normalized angular
contributions, not automatically a cosine-weighted surface irradiance integral.
The four incoming quadrants are not four independent outgoing wall-face sources.

### What the papers do not establish for this port

The RC formulation permits directional outgoing radiance and discusses diffuse,
normal-mapped and roughness-dependent specular evaluation. Its generality does
not mean the inspected flatland example implements these features. PoE2 images
do not reveal its exact source-cache or hit-shading implementation.

HRC section 4.2 explicitly identifies the four-direction output as limiting its
implemented specular reflection; higher-cascade querying is discussed as future
work. The multibounce example is volumetric: its solid(diffuse) helper uses
extinction 0.5, while emitters use infinite extinction; feedback excludes very
opaque cells. It is not a validation of independently shaded opposite faces of
opaque cell walls. Main HRC timing comparisons are predominantly single-bounce;
neither their results nor the temporal multibounce example establish PPR speed or
visual correctness.

## Local source evidence

In `docs/oni_v22.slang:9475-9520`, `CascadeRadiance<TIndirect>::shade` fetches
the material, constructs hit geometry and calls `::shade<TIndirect>` at each hit.
The latter evaluates diffuse and specular material response, including BRDF
integration lookup and indirect-light queries (`:9423-9473`).

`RadianceSingleBounce` is only an alias for
`CascadeRadiance<Shading::NullIndirect>` (`:9560`); `RadianceMultiBounce` uses
`Radiance::SuperSampling` (`:9561`). Both use the same hit-shading function.
The single-bounce name does not establish an existing precomputed source cache.

Static loop arithmetic explains why per-hit work deserves attention. For the
reference's 1024 squared sizing and approved cadence, fine reconstruction has
262,144 probes x four neighboring L1 probes x 16 directions = 16,777,216
connector-trace calls per frame (`:6796-6828`). L1-L4 each update 262,144 interval
records with four connectors per record; L5 uses one segment per updated record
because its parent is clamped to itself (`:6157-6161`, `:6320-6375`). That adds
4,456,448 calls, for 21,233,664 total nominal trace calls. These are source-loop
counts, not measured execution time, successful hits, ray steps, unique rays or
DRAM traffic; early exits, compiler transformations and scene visibility matter.
No new ray cap or optimization requirement follows from this arithmetic.

The existing engine's `gridMaterialDiffuseAlbedo` already combines linear material
tint, a filtered texture mean and `(1 - metallic)`
(`App.Renderer.GridPass.cpp:230-265`). That is a diffuse-reflectance candidate,
not a full PBR base color. Reuse depends on the selected simplified bounce model;
it would suppress diffuse feedback for a pure metal. On-screen specular response
is independent and must not be removed along with transport hit BRDFs.

## Consequences to resolve after the survey

### Source quantity and directionality

A scalar RGB source must have explicit units and angular meaning. Outgoing
radiance and radiosity are not interchangeable names; Lambertian surface
radiosity divided by pi gives outgoing radiance in the usual 3D convention,
while a normalized 2D isotropic-cell model requires its own stated normalization.
Do not apply or omit pi merely because a reference calls its texture light.

One RGB value per cell represents the same source in every queried direction.
It cannot preserve view-dependent metal/specular bounce or different outgoing
values for opposite cell faces. CH can still represent directional incoming
lighting for the final scene; this does not restore directional information lost
when producing a scalar bounce source.

### Candidate dependency change

The candidate schedule is current material/geometry plus previous CH -> current
source cache -> coarse/fine tracing -> next CH -> on-screen PBR. This moves
feedback evaluation before tracing instead of performing it at every hit.
The source cache can combine emission with reflected lighting; a separate
per-cell emissive image is not automatically necessary when authored emission
already resides in the material data used to build it.

Visibility still controls hit/miss independently of source brightness. A black
opaque cell must stop rays; a receive-only transparent cell must not become a
transport emitter merely because it is visibly lit.

### Storage and traffic arithmetic, not performance evidence

One uint32 RGB9E5 source per 1024 x 1024 cell map is 4 MiB. A separate equally
sized emissive image would add another 4 MiB; it needs an actual consumer/reuse
reason before allocation. A source cache fully rebuilt before current tracing
can use one ordered GPU allocation rather than another temporal ping-pong pair.
Earlier readers still have to precede the next overwrite.

If an isotropic bounce model uses only angular-mean incoming radiance, the CH DC
coefficient may be sufficient. In the approved CH layout that is one packed word
per probe; four-probe interpolation would request 16 bytes logically, versus
128 bytes for four full 32-byte records. This is conditional on the selected
model and normalization. AoS stride, cache-line transactions, sharing and hit rate
prevent equating those logical byte counts with measured DRAM traffic.

## Evidence checklist

- [x] Identify the actual Oni per-hit shading path and single-bounce alias.
- [x] Separate source representation from interval visibility and final PBR.
- [x] Inspect primary RC paper and relevant author implementation.
- [x] Inspect primary HRC paper and relevant author implementation.
- [x] Record actual storage formats, hit/source equations and temporal feedback.
- [x] Compare source-cache approximations and missing metallic/specular transport.
- [x] Discuss findings with the owner before revising the frame/data contracts.

## Owner decision

The owner approved one isotropic RGB9E5 outgoing-radiance source per native cell,
combining authored emission and diffuse feedback from previous angular-mean
lighting. The single rebuilt source is shared by coarse and fine tracing. There
is no per-hit BRDF and no separate per-cell emission image initially. Preserve
full on-screen PBR and directional CH. Pure metals contribute no diffuse feedback;
the cache cannot represent directional specular bounce or independently lit
opposite cell faces. These approximations were explicitly included in the approval.

The main design incorporates the source pass and its 4 MiB allocation. In a
subsequent explicit decision, the owner approved replacing the CH pair with one
8 MiB field: source preparation finishes all prior-CH reads before fine
reconstruction reads and overwrites each probe's own record. Tracing no longer
reads neighboring CH. Scene shading follows the completed fine writes, and
next-frame writes follow earlier readers. This saves 8 MiB of allocation, not the
EMA's logical read/write traffic. The listed world/lighting subtotal is now 57 MiB,
excluding simulation, material/texture, scratch and screen-space allocations.
