# Fresh start: application cleanup and Oni lighting port

Date: 2026-11-10
Status: **Owner-approved complete design; independent builder check complete.
Implementation belongs to the future-session plan and its acceptance gates.**

This is a new design based on the current discussion with the project owner.
Earlier lighting plans, recovery checklists and agent approvals are not execution
authority for this work. Do not fill gaps by importing their decisions.
Production changes belong to a future session after this design is completed and
approved. This session prepares the design, implementation plan and handoff prompt.

Status convention: **owner-approved** identifies explicit decisions already made.
**Coordinator technical specification** identifies source-grounded implementation
choices originally proposed as part of the complete draft, not separately approved
owner decisions. The owner has now approved the complete design, including these
choices. Verification obligations are not claims of successful runs.

Revision note (2026-11-11): the owner's all-local-work checkpoint/no-committed-archive
clarification and the coordinator's concrete cleanup-test inventory and race-free
jump-construction details were added after that builder review. They are reflected
in the pending implementation-plan review; the earlier review hash does not cover
these additions.

## 1. Intent and scope — agreed

Reproduce the intended Oni lighting inside the real game engine on a
**1024 x 1024 cell map**. The owner clarified that `docs/oni_v22.slang` includes
unfinished harmonic, spectral-translucency/multibounce and direct-lighting
experiments; its enabled branches are not all accepted reference behavior.
The owner has selected order-four circular harmonics, the signed 32-byte encoding
and the accumulation policy specified below. First restore a simple, usable
application. Preserve useful work through a recovery checkpoint and selective
reuse, rather than carrying forward the existing integration wholesale.

- Larger-map optimization is outside this port.
- Retire all inherited performance gates and limits, including the fixed 6 ms
  field-update target, register-count ceilings, fixed ray budget, larger-map gates
  and historical no-frame-time-regression gate. Measure the complete approved
  implementation after integration; do not invent replacement thresholds.
- Preserve simulation and particle evolution; simplify their presentation and
  lighting integration.
- Use simple 2D per-cell geometry with standard on-screen PBR shading and texture
  bombing. Simplifying geometry and lighting storage does not authorize replacing
  the visible material response with flat shading or a fixed highlight.
- The owner alone accepts visual results. Automated checks cannot establish
  satisfactory lighting or appearance.
- Backward compatibility is out of scope. Do not add migrations, compatibility
  modes, legacy-setting translation, or special handling for obsolete settings.
- No new alternative GI implementations, competing AA modes, or one-off lighting
  test applications/harnesses.
- Durable display feature coverage and repeatable profiler automation are
  explicitly requested exceptions to the earlier prohibition on special tooling.
  Both exercise the real application and production paths.
- Owner YAGNI requirement (2026-11-11): implement the smallest complete approved
  solution. Reuse existing facilities; each new abstraction, API, setting, resource
  or dependency needs a current responsibility and planned consumer. Keep the
  specified pass/tooling boundaries without expanding them into generic frameworks,
  extra modes or speculative optimizations. Enforce this in existing per-task
  reviews and Task19; preserve all required behavior, platform support, failure/
  lifetime handling and verification. The plan spells out the high-risk expansion
  points. This requirement does not grant authority to replace the approved design.
- Owner naming correction (2026-11-11): implementation names describe their
  responsibility, not this reference project. There must be no `Oni`/`oni`
  references anywhere in active code: filenames, symbols, namespaces, modules,
  shaders, tests, scripts, comments, runtime strings and profiler markers included.
  Source research, provenance and attribution belong in documentation. Preserve
  exact recovery archives as historical evidence, outside active code.

## 2. Approaches and selected direction

Considered: port Oni end-to-end, or combine WGSL transport with Oni's tracer.
The owner selected **Oni end-to-end**, avoiding an underspecified hybrid.

Considered: modify the current integration in place, or disconnect it and salvage
individual useful parts. The owner selected **cleanup first**, preserving the work
before removing unwanted active paths. The removal boundary includes settings,
application/game integration and obsolete bookkeeping, not just shader files.

Oni remains the transport reference, but the owner's subsequent clarification
supersedes blanket fidelity to its active experimental path. The comparison of
fixed directional bins and harmonic accumulation concluded with the approved
order-four CH field below. This is not approval for a WGSL/Oni hybrid or multiple runtime GI
modes. Emissive surfaces and sky are the only lighting sources; separate direct
lights and spectral-translucency transport are excluded. Ordinary opaque-surface
bounce remains required. Engine adaptation and the explicitly agreed material
approximation below must be documented. Prototype code is not permission to copy
undefined API behavior or to ignore backend correctness.

## 3. Cleanup milestone — agreed direction

Owner clarification (2026-11-11): Task 0 begins by committing **all current local
modifications**, including unrelated work and currently untracked files, to start
from a clean slate. Inspect/inventory the exact inputs as preflight to that commit.
Verify its contents and recovery before production removal; preserve the paused
Task6b stash by object ID. Cleanup deletions are committed normally, but copied or
archived versions of removed code must never be committed. Do not revert the
workspace broadly. This instruction applies to fresh-session execution, not the
current documentation-only planning session.

Prepare an exact salvage/removal inventory before unplugging code:

- Keep required facilities whose behavior and ownership fit the new design.
- Extract and simplify useful algorithms embedded in unwanted policy.
- Preserve superseded implementations in checkpoint history and, if needed, an
  external recovery archive outside Git for reference.
- Remove their active integration, obsolete controls and implementation-specific
  tests; retain meaningful contracts for behavior that survives.

The ordinary application temporarily renders terrain and particles using simple
material colors/textures, with old lighting disconnected. This is an intermediate
development state, not a runtime-selectable legacy fallback.

The owner accepts the cleaned application before the Oni port begins. It must
support ordinary launch/play/edit/resize/minimize/restore/display changes/exit.
Investigate errors and warnings in those operations rather than hiding them.

Keep GPU resource lifetime and synchronization requirements while removing
unnecessary policy. Simplifying game/application/simulation boundaries does not
authorize duplicating simulation or replacing it with a fixture implementation.

### Cleanup inventory and acceptance boundary — coordinator specification

At implementation start, record HEAD, tracked diffs, untracked paths/content
hashes and stash object IDs in an external recovery manifest, then make the initial
all-local-work checkpoint commit before cleanup. Preserve the former index/worktree
distinction in external diffs; the commit contains the final working-file versions.
Require clean Git status and verify a disposable extraction against the recorded
hashes/modes, including formerly untracked shaders. Record the
object ID and message of `task-6b paused: atlas dilation (pending cascade fix)`;
never pop/drop/replace it merely to obtain a clean tree. The manifest must name
the actual checkpoint location and restoration procedure, not just assert that
Git can recover untracked files. Preserve unrelated IDE/font/image/document work.

The following is the removal/reuse boundary; expand each named family to concrete
paths in that manifest using the then-current source tree and registrations:

| Existing area | Disposition and surviving contract |
| --- | --- |
| `App.Renderer.WorldLightField.*`, `WorldLightTypes.cppm`, `WorldLightRadianceAtlas.*` | Archive old implementation; remove active dirty/repair scheduling, radiance-atlas feedback/ambient, ray-budget and convergence policies. Reuse only independently extracted binding/fence/codec mechanics consistent with this document. |
| `App.Renderer.WorldLightSource.*`, `WorldLightSourceGpu.*`, `WorldLightProducer.*` | Archive old optical publication/producer integration. Replace with packed categorical publication and full-domain source preparation, not an adapter retaining obsolete state. |
| `App.Renderer.OpticalGeometry.*`, `App.Renderer.Lighting.*` | Archive contour/absorbing-volume and old resource-estimator machinery. Salvage positive-length slab tests, interval-composition reasoning and checked allocation arithmetic into the new bounded responsibilities; retain no alternative production tracer. |
| `assets/shaders/world_light_{field,trace,sample,source,matter,interval,radiance_atlas}.slang` | Archive current files before replacing the active shader set. Reuse the analytic sky helper and proven packing where their exact new contracts match; remove old optical/atlas/repair bindings. Include currently untracked files. |
| `App.Renderer.GridPass.*`, `grid_tiles.slang`, particle preparation | Preserve catalog/texture residency, filtered texture means, texture bombing, normal decoding, strong GPU publication ownership and gas drawing. Replace solid/liquid surface reconstruction with the resolved-cell draw path. Retire its now-unused splat/filter/composite shaders, buffers and registrations only after checking their callers. |
| `App.Renderer.RetroPass.*`, `retro_present.slang`, spatial/temporal shaders | Extract required allocation/fence, Hable sample-loading and post-effect helpers; establish independent reconstruction and `PostProcessPass` owners. Archive superseded temporal/AA/palette paths and remove their runtime selectors. |
| `luminance_histogram.slang`, `auto_exposure.slang`, `spd_bloom.slang`, `display_agx.slang` | Retain the selected post algorithms/attribution, correct arbitrary-extent handling under section 7, remove AA-mode validation coupling. |
| `Game.Application.*`, `Game.Config.*`, `Game.Display.*`, `ui/ColonyPanel.*`, `App.UI.Widgets.Display.*`, `App.Application.Editor.*`, `App.Frame.RenderLane.*` | Remove old lighting/AA/palette knobs and bookkeeping end-to-end. Retain live settings propagation, display transactions, real acknowledgments, frame settlement, lifecycle and current sky-hour -1 semantics. |
| `game/colony/SimFramePacket.*`, `MatterRuntime.*` | Replace `buildWorldLightMaterials`' absorbing/volume mapping with the admitted eight-byte palette; add coherent temperature packing. Preserve simulation state, joint identities, source rows and inventory lifecycle. |
| `App.WorldLightField.Tests.cpp`, `App.Lighting.Tests.cpp`, `App.GridPass.Tests.cpp`, game/UI/config tests, asset render gates | Archive implementation-specific old gates; adapt surviving publication, lifecycle, material, geometry, display and numeric contracts. Retire obsolete performance thresholds and AA combinations instead of weakening assertions to make stale tests pass. |
| Owning CMake files, `App.cppm`/renderer umbrellas, shader staging, codemaps and CLI help | Remove old source/import/deployment references in the same changes as their code. Update maps/help to the one resulting runtime pipeline. |

Any additional archive belongs under a discovered durable external
`<recovery-root>/lighting-precleanup/`, preserving relative paths and license
headers. Never add archive source, copied test bodies or recovery bundles to Git;
the initial checkpoint already retains the original implementation. Archive files
are absent from CMake, shader staging and runtime asset discovery. The committed
execution ledger records checkpoint IDs, paths, hashes, dispositions and restoration
instructions, not source snapshots. Reference searches distinguish historical docs
from active integration. Do not delete whole test or GridPass files when they also
own surviving contracts.

The implementation plan's Task 1 now enumerates exact production paths, test
deletions, mixed-file edits and registration removals. In particular, extract the
nine histogram/adaptation/bloom leaves from `App.WorldLightField.Tests.cpp` into
`App.PostProcess.Tests.cpp` before deleting that old suite. Delete the old
`App.Lighting.Tests.cpp` suite with its retired tracer/estimator. Preserve filtered
texture means, content/presentation identity and packet/lifetime tests while removing
their old optical adapters. Retro-specific tests are retired/adapted with Task 2,
and solid/liquid surface-kernel tests with Task 11, alongside their owning code.

The current probe-visualization override in `grid_tiles.slang`, commented
odd-extent checks, and sky-hour -1 preference are intentional user work. Record
their exact pre-cleanup contents. Replace the first two deliberately with the new
inspection overlay and approved R/S/B contract; they are not unsolicited cleanup
targets. Keep the sky setting and meaning. Remove the obsolete `atlas_ambient`
control and single-value `world_lighting` selector as well as the already listed
switches; neither represents a retained responsibility.

Cleanup acceptance uses ordinary gameplay with simple material textures/colors,
moving particles, dig/build, new-world/loading, resize (including odd extents),
minimize/restore, display Apply/Keep/Revert and graceful exit. Preserve standard
post controls where available; report a not-yet-integrated final-stage check as
incomplete rather than claiming the final lighting suite passed. Owner acceptance
of this intermediate application is mandatory before new lighting integration.
Capture any resize/exit error with exact action, backend, log and settlement state;
do not identify its cause from the word `VK_KHR_maintenance4` alone.

## 4. Lighting update contract — approved decisions and technical specification

- Use Oni cascade transport and directional lighting with simplified diffuse
  feedback. Source preparation reads immutable previous CH and writes a single
  isotropic RGB9E5 outgoing-radiance value per cell. Both coarse and fine tracing
  read that source cache; fine reconstruction updates each probe in place in one
  CH field after source preparation has finished reading its prior contents.
  This owner-approved refinement supersedes direct CH/BRDF evaluation at ray hits.
  The signed 32-byte CH record and derived temporal direction identity are
  approved below; the coordinator reconstruction specification is now explicit.
- Coarse cascade scheduling selects one probe from each 2 x 2 probe group and
  updates all its directions each frame. It is not a ray-budgeted global sweep.
- Reconstruct and accumulate fine-probe lighting every frame.
- Retain Oni's world-space temporal lighting accumulation. It is distinct from
  screen-space TAA and independent of camera motion.
- Replace prototype screen-space TAA with **always-on Hable temporal MSAA** in
  the completed port. There is no AA selector or user-visible disable mode.
- Remove edit-driven lighting dirty queues, repair quotas, history-reset
  heuristics and convergence-stop policies. Continuous updates consume current
  world state regardless of terrain edit notifications.
- The previous 262,144-ray cap must not throttle the approved prototype cadence.
- Rebuild visibility and the jump table over the whole 1024 x 1024 domain every
  rendered frame, after publishing coherent current cell/particle data and before
  tracing.
- Use Oni's jump-table DDA. Its active table contains only 0, 1, 2, 4, 8, 16 and
  32; R8_UINT can encode these values exactly. Backend storage support and the
  precise resource binding remain technical verification items.
- Preserve camera-independent fields and two-cell fine-probe spacing.
- Retain the exposure -> bloom -> AgX processing specified in section 7.
  Post-processing consumes resolved
  scene-linear HDR, independently of reconstruction and pixelization settings.

### Directional representation investigation and approved lighting records

Temporal accumulation and angular representation are independent choices. Fixed
world-space bins can accumulate corresponding bins over time; harmonics are not a
prerequisite. A bin must keep a stable angular meaning. Blending changing ray
directions under the same slot without projection into a fixed representation
does not preserve that meaning.

Source findings from the current Oni file:

- `SuperSampling` uses `CircularHarmonics<4>`: nine RGB coefficients over a circle,
  not a full spherical representation. Its packed payload is eight uint32 values
  (32 bytes per fine probe), with the sign-loss issue documented in the review.
- The non-supersampling cascade direction path uses fixed angular midpoints.
  Its `specularRadiance` simply calls irradiance and ignores roughness
  (`:6529-6531`). Selecting this path verbatim would therefore not fulfill the
  retained roughness-dependent PBR requirement.
- `SuperSampling.update2` also fits harmonic coefficients; its existence is not
  evidence of a separate four-bin temporal implementation. The historical WGSL
  file contains a separate per-direction exponential blend, but that observation
  does not authorize importing its transport or replacing the chosen reference.

Four RGBA16F directional values would also occupy 32 bytes per probe: 8 MiB for
one 512 x 512 fine-field copy at the agreed two-cell spacing. This comparison is
not equal-precision storage equivalence: the current harmonic codec loses signs,
and final lighting history does not need interval transmittance. Four RGB9E5
radiance-only bins could instead use 16 bytes per probe. These are comparison
alternatives, not the selected format below; no runtime performance difference
has been measured.

The owner initially preferred CH over four bins if storage was comparable, valued
directionality for normal maps, and had also experimented with spherical Gaussians.
The owner subsequently approved the 32-byte CH record specified below. Using a
different representation for radiance history than for cascade radiance/
transmittance intervals is intentional and appropriate.

Subsequent owner constraint: reject the proposed 36-byte format. Choose a
hardware-appropriate stride and access layout first, then fit directional
information within it through explicit precision tradeoffs. The arithmetic below
is comparison evidence, not approval of a nine-word resource layout. Packed
storage size and decoded register pressure are distinct; a favorable stride does
not establish vectorized loads, low register use or measured throughput.

Signed-storage arithmetic for order-four CH (nine RGB coefficients):

- Current custom block: 27 nine-bit components plus one shared five-bit exponent
  = 248 bits in eight uint32 words. This is not nine standard RGB9E5 values.
- A 32-byte signed variant can use nine-bit signed components (roughly eight
  magnitude bits), trading magnitude precision for signs. Exponent/range and
  rounding rules would need an explicit contract.
- Keeping nine magnitude bits and adding signs for the 24 AC components takes
  272 bits, fitting nine uint32 words (36 bytes). DC remains nonnegative.
- Explicitly packing 27 binary16 components into 14 uint32 words takes 56 bytes
  including padding, with per-component exponents. This was another candidate,
  not the assumed native layout of an array of half3.

At 512 x 512 probes those candidates require 8, 9 and 14 MiB per field copy,
respectively. Copy count follows the selected pass dependencies: an own-probe EMA
can update in place; reading neighboring previous-field probes while overwriting
that field requires a frozen input or a different staged schedule. Do not infer
mandatory ping-pong merely from the word temporal, or permit races to avoid it.
Actual performance measurement remains after full integration.

The owner approved these baseline lighting records:

| Record | Byte offset | Contents |
| --- | --- | --- |
| Coarse interval, 8-byte stride | 0-3 | FP16 radiance R and G |
| Coarse interval, 8-byte stride | 4-7 | FP16 radiance B and scalar transmittance |
| Fine CH, 32-byte stride | 0-3 | RGB9E5 DC coefficient |
| Fine CH, 32-byte stride | 4-7 | FP32 shared AC quantization scale |
| Fine CH, 32-byte stride | 8-31 | Eight RGB AC coefficients, signed eight-bit components |

AC order is cos1, sin1, cos2, sin2, cos3, sin3, cos4, sin4, with RGB
components together for each coefficient. Signed components use [-127,127];
zero directional variation encodes zero scale and zero AC bytes. The scale is
max(abs(AC)) / 127. Accumulate and interpolate decoded FP32 coefficients, never
packed integers. Quantize once when storing the updated CH record. Fine probes
initially use row-major addressing. Do not add tiled/swizzled
addressing without a demonstrated access-pattern benefit.

Separate DC and AC scaling prevents the DC magnitude from consuming AC
quantization range, but the shared AC scale still loses weak components relative
to the strongest AC component. This accepted tradeoff permits byte extraction
without cross-word components. Exact rounding and finite-range handling are
specified below; shader ABI/reflection and quantization checks remain verification
obligations. Format approval is not evidence of validated precision or GPU throughput.
Coarse transmittance remains fractional: merging and angular averaging can produce
intermediate values even with opaque scene geometry.

Normal maps still make directionality relevant in a flat view. Four angular bins
limit angular detail; temporal averaging cannot recover narrow directional
structure once it has been averaged inside a bin. Low-order circular harmonics
also have limited angular resolution and do not recover missing out-of-plane
transport. The selected CH storage does not by itself specify the final
roughness-dependent PBR sampling/filtering contract. The owner has now approved
the concrete circular-filter approximation below.

### Codec, radiance range and GPU status — coordinator specification

Use shared host-reference and Slang codec definitions. Coarse FP16 fields use
IEEE round-to-nearest/ties-to-even, with RG in the low/high half of word 0 and
BT in word 1. Radiance must be finite and nonnegative; T must be finite in [0,1].
FP16 radiance above 65,504 is an error, not infinity or an arbitrary brightness
cap. Denormal underflow follows binary16 rounding and is measured by codec tests.

For RGB9E5, bits 0..8/9..17/18..26 hold R/G/B mantissas and bits 27..31 hold the
shared exponent E. Decode `RGB=mantissa*2^(E-24)`. For nonnegative finite input at
most 65,408, choose the smallest E in [0,31] for which rounded mantissas fit in
[0,511], using nearest/ties-up rounding for this unsigned shared-exponent codec.
Equivalently derive an initial exponent from the largest channel and increment
it if rounding produces 512. Zero packs to zero. This states the tie convention
explicitly; do not rely on an unspecified shader `round` tie rule. Values above
65,408 fail admission/record status before encoding, rather than using a saturating
helper that conceals the loss. Tiny positive values may legitimately quantize to
zero; this accepted format cannot preserve arbitrary dynamic range.

For CH AC, compute scale=max(abs(AC))/127 in FP32. Encode each component with
nearest/ties-to-even integer rounding and clamp only the rounding endpoint to
[-127,127]. Store two's-complement bytes, first component in the least-significant
byte, four components per word; -128 is never emitted. Decode with explicit sign
extension, then multiply by scale. The zero-AC case writes scale zero and all
24 bytes zero. A nonzero AC maximum whose computed scale underflows to zero also
encodes zero AC; its absolute error is below 127 times the smallest positive FP32
value. Nonfinite coefficients/scale are numeric failures. DC is the orthonormal
C0 coefficient, so its 65,408 representable limit differs from that of angular
mean radiance; check C0 before packing rather than assuming a valid source always
implies representable CH.

Publication validates finite linear reflectance/tint/texture means in [0,1],
metallic/roughness/opacity in [0,1], positive finite texture scale, eta>=1, and
nonnegative finite authored emission/strength. Multiply emission in binary64 and
reject overflow or an RGB9E5-out-of-range result before upload. Zero material is
empty; a referenced unknown ID is invalid. Derive source palette, existing visible
material records and classification from one admitted catalog version. Use the
existing visible record's surface/phase classification in preparation; do not add
a separately authored optical material table or embed classification in radiance
bits. Source records retain stride 8 and offsets 0/4. Shader reflection verifies
the unchanged visible-material ABI at the extracted binding boundary.

Runtime overflow/nonfinite data is a visible failed frame computation, not a new
performance limit or silent HDR clamp. Each pass-owned dispatch/draw that can
produce such a failure has one 32-byte status record in its in-flight attempt's
status buffer. Words 0/1 are atomic minima of the invalid-state/range-error index,
respectively, initialized to UINT32_MAX. Words 2..7 are reserved zero, never used
as racing payload. The CPU ticket maps each record to its fixed pass/level and
world/accepted-frame identity. Particle operations report metadata row, cell
operations native row-major cell, coarse/fine operations output record/probe, and
screen operations row-major output pixel. A traversal failure reports the owning
coarse/fine invocation index. UINT32_MAX means no failure. Neither a compound
atomic payload nor a nondeterministic "first error thread" is needed.

Each failing invocation records its category then writes a finite zero result
for its own output (or rejects that particle candidate) to prevent invalid memory
access/NaN propagation. That containment value does not make the frame successful.
Copy the small status record to completion-fenced readback storage at each stage's
end; poll completion through the existing render lane, with no per-frame host
wait. Do not reuse a status/readback slot until its submission is complete and
CPU inspection has consumed it. If a stage is definitely rejected, its unsubmitted
slot can be released; ambiguous submission quarantines it with the other resources.

When a completed ticket reports any error, stop admitting dependent scene work
and propagate `result_out_of_range` for representational overflow or
`state_not_recoverable` for invalid GPU arithmetic/state, with category/index/frame
diagnostics. Retain already-submitted work until completion; do not roll back
in-place CH or describe possibly displayed earlier frames as verified. Teardown
drains/inspects outstanding status tickets before reporting successful completion.
The two separate minima preserve invalid-state versus range classification.
This is bounded status traffic, not particle/image readback or a polling-based synchronization
replacement. Fault-injection tests verify the error reaches the application.

### Visible CH filtering — approved approximation

Source inspection distinguishes the implemented CH filters from the unrelated
raw-cascade `specularRadiance` function that ignores roughness. Oni's
`CircularHarmonics::diffuseIrradiance` and `specularRadiance`
(`docs/oni_v22.slang:5968-6012`) do provide normal-oriented diffuse response and
roughness-dependent directional filtering. They are approximate circular filters,
not a reconstruction of a full 3D GGX environment. The visible material response
still retains textured base color, normal detail, metallic Fresnel and emission.

The owner approved reusing these circular filters, with a defined smooth
out-of-plane limit for specular. For the decoded/interpolated coefficients define
`Hm(theta) = (Cc_m*cos(m*theta) + Cs_m*sin(m*theta))/sqrt(pi)` and
`mean = C0/sqrt(2*pi)`. For unit normal N and unit reflection direction R:

```text
diffuse_irradiance = pi * max(0, mean
    + length(N.xy) * sum(Hm(atan2(N.y,N.x)) / (1+m*m)))

specular_radiance = max(0, mean
    + sum(length(R.xy)^m * exp(-5*m*m*roughness*roughness)
          * Hm(atan2(R.y,R.x))))
```

For exactly zero XY length, return `pi*mean` for diffuse and `mean` for specular
without evaluating atan2(0,0). The `length(R.xy)^m` factor is an explicitly approved
adaptation, **not literal Oni behavior**: it makes every directional term vanish
continuously at the pole instead of selecting an arbitrary world azimuth for a
flat front-facing material. In-plane directions retain Oni's specular formula.
The diffuse expression is Oni's existing approximation. Decode and bilinearly
interpolate coefficients at the shading location before these evaluations.

The owner accepts broadened low-order highlights and the inability to recover sharp
out-of-plane reflections. The material-response formula is specified below; do not
reinstate the old fixed key light, an ambient floor or per-hit transport BRDF.
The alternative requiring a different design is a specifically derived GGX
environment convolution with an explicit out-of-plane radiance model. It must
not be slipped in as a codec or plumbing implementation detail.

### Visible material response — coordinator technical specification

Preserve `grid_tiles.slang`'s texture selection, continuous-UV derivatives,
texture-bombing translations/weights, filtered-mean variance normalization and
normal-map decoding. Use the selected categorical material, not blended IDs or
`gridMaterialDiffuseAlbedo` as the visible base color: that helper already includes
the metallic diffuse reduction. Let A be linear texture RGB times linear tint,
bounded to [0,1]; m and r are authored metallic and roughness in [0,1]. Use the
authored IOR eta (finite, at least 1). N is the mapped unit normal in the same
world basis as CH, and V points from the world-plane sample to the camera. For an
orthographic camera V is constant. Retain the established normal-map orientation;
do not substitute clip-space XY for world XY. Define:

```text
NoV = max(dot(N,V), 1e-5)
F0 = lerp(((eta-1)/(eta+1))^2, A, m)
F = F0 + (max(1-r, F0)-F0)*(1-saturate(NoV))^5
kD = (1-F)*(1-m)*A/pi
R = reflect(-V,N)
```

Reuse Oni's isotropic slice of `BxDF::integrateBRDFAnisotropic` with anisotropy
zero. Precompute 64 NoV by 128 roughness texels once per device, 1,024 samples per
texel, using the reference's golden-ratio/radical-inverse sample sequence and
VNDF/Smith weighting (`docs/oni_v22.slang:4791-4881`). Here microfacet alpha is
`max(r,0.001)` as in that reference, **not r squared**. Texel centers evaluate
`NoV=(x+0.5)/64`, `r=(y+0.5)/128`. Store `(Fc_weighted_visibility,
unweighted_visibility)` in RG16_FLOAT (32 KiB), bilinearly sample with clamp-to-edge
at `(NoV,r)`, and call that pair (b,e). No anisotropy volume or runtime BRDF
integration is required. Preserve the source's attribution when extracting it.

```text
energy_compensation = 1 + F0*(1/max(e,1e-5)-1)
kS = max(0, lerp(b,e,F0)*energy_compensation)
visible_HDR = kD*diffuse_irradiance(N)
            + kS*specular_radiance(R,r)
            + decoded_authored_emission + thermal_RGB(packed_temperature)
```

Use the approved CH filters for the two lighting terms. Emission is added once,
using the same decoded source-material emission word and thermal function as
source preparation. Excluded transparent/gas thermal scope still applies. This
port has no extra AO/atlas-ambient multiplier, fixed key light, radiance-strength
sliders or screen-local emissive compression. Specifically omit Oni shade's
`max_specular_luma/specular_luma` cap (`:9458-9461`): it bounds output independently
of illumination and prevents proportional HDR response. No such cap is part of
the approved circular filtering approximation.

Visible receive-only transparent cells use this reflected response and their
existing authored coverage/opacity in premultiplied-alpha composition; opacity
does not attenuate CH transport. Preserve the separately agreed simple geometry
scope; no refraction/absorption transport or duplicate background source is added.
Validate material scalar ranges on publication rather than silently repairing
invalid authoring in shaders. Tests check black environment/no emission, doubling
environment radiance, metallic endpoints, uniform-environment CH-filter invariance,
roughness response, textured base color and common thermal emission. Final
appearance remains the owner's decision.

### Angular sampling and retained direction identity — approved

The owner approved temporally varying angular samples with direction identity
derived from a fixed four-frame schedule, without a per-probe phase buffer.

- Each 2 x 2 coarse-probe group has a fixed deterministic phase offset and cycles
  through all four probes once per four accepted lighting frames. Each selected
  probe updates all its directions. The frame index advances only with accepted
  lighting work, not CPU attempts, UI-only frames or minimized presentation.
- Derive a probe's most recent update frame from that schedule. Retained radiance
  is interpreted with the angular sampling offset used for its actual trace,
  never the offset belonging to the current frame merely because it is current.
- Angular sectors are fixed. Samples vary within their sectors, preserving
  parent/child sector correspondence. Fine reconstruction projects into a fixed
  world-space CH basis before temporal blending.
- A probe that has not yet received its first scheduled update is explicitly
  uninitialized and contributes the zero-radiance, zero-transmittance interval
  under the approved cold-start policy below.
- Fixed midpoint directions were considered and not selected: repeated frames
  would not improve angular sampling in a static scene.

Coordinator's explicit schedule spelling: let n be the zero-based accepted
lighting-frame index for the current world, h the group's fixed offset in [0,3],
and k = local_x + 2 * local_y. Select k = (n + h) mod 4. For a queried probe,
age = (n + h - k) mod 4; it has prior/current data only if n >= age, and its last
update is n - age. Evaluate modular arithmetic without unsigned-underflow or
frame-counter-wrap assumptions. This derives validity and direction identity
without additional per-probe storage. The coordinator technical specification
below fixes the sequence, group hash and numeric interpretation within this
approved scheduling contract.

### Angular identity — coordinator technical specification

All lighting positions and angles are in world-cell coordinates, with +X at
theta=0 and +Y at theta=pi/2. A sample direction points from receiver towards the
source. Screen jitter and camera matrices never rotate or translate this basis.
Probe centers are `(probe + 0.5) * spacing`, without temporal position jitter.

For level l, spacing is `2 << l`, direction count is `4 << (2*l)`, and the
probe-grid edge is `1024 / spacing`. Use row-major 2x2 group coordinates gx, gy.
Define the following uint32 hash, with multiplication explicitly modulo 2^32:

```text
mix32(x):
    x = (x xor (x >> 16)) * 0x7feb352d
    x = (x xor (x >> 15)) * 0x846ca68b
    return x xor (x >> 16)

group_edge = (1024 / spacing) / 2
h = mix32(gx + group_edge*gy + 0x9e3779b9*l) & 3
```

Keep accepted frame n as a checked uint64; reject counter exhaustion rather than
depending on wraparound. Compute `age = ((n & 3) + h + 4 - k) & 3`, then establish
`n >= age` before subtracting. The trace frame t is `n-age`; the currently selected
probe has t=n. Invalid retained probes return the cold-start interval without
constructing a negative frame index.

Use golden-ratio stepping in fixed-point form rather than converting a growing
frame counter to float, which loses successive-frame identity:

```text
phase_word = uint32(t) * 0x9e3779b9             // modulo 2^32 is intentional here
u = (float(phase_word >> 9) + 0.5) / 8388608   // strictly between 0 and 1
theta(phi, D, t) = 2*pi * (phi + u) / D
direction = (cos(theta), sin(theta))
```

The 23-bit phase fraction is exactly representable in binary32. Trigonometric
evaluation is binary32; CPU reference tests compare directions within floating
point tolerance, not transcendental bit identity across vendors. The phase
sequence repeats after 2^32 accepted frames; the validity/frame counter does not.
Four child sectors remain the fixed parent-sector partition by integer indices;
do not interpolate interval slots as though their changing samples were fixed rays.

This replaces the global `Time.frame` read inside Oni `Cascade::probeDir`
(`docs/oni_v22.slang:6205-6216`) with the supplied actual trace frame. It also
replaces the prototype's `probeFrame`/`probeToUpdate` arithmetic with the already
approved schedule, not a second scheduling policy.

### Coarse geometry, storage and terminal sky — coordinator specification

Fix the reference penumbra-width parameter at 0.5; it is not a retained control.
For l=1..5, interval start is `a_l=0.512*2^(2*l-1)` cells, yielding 1.024,
4.096, 16.384, 65.536 and 262.144. The finite upper end for l<5 is a_(l+1).
No raw L0 allocation/dispatch exists. Each level has a 1024x1024 logical interval
atlas with the reference's per-probe square angular tile. For spacing s=2<<l:

```text
atlas_x = probe.x*s + phi%s
atlas_y = probe.y*s + phi/s
record = (l-1)*1048576 + atlas_y*1024 + atlas_x
```

Store five levels in one structured buffer of uint2 records, stride 8; an
equivalent five-resource split is not a different algorithm but is not required.
The fine CH buffer is a structured record of eight uint32 words, stride 32.
Use explicit word extraction/packing rather than relying on native half3/int8
structure layouts. Slang reflection and host size/offset checks establish these
contracts. Dispatch each coarse level over a 512x512 logical domain, 16x16 threads
per group. For thread coordinate q, group=q/s, phi=(q.y%s)*s+q.x%s; select the
group's scheduled probe as specified above. Fine reconstruction uses 512x512
threads, initially 8x8 groups. These are baseline launch shapes, not occupancy or
register-pressure claims.

For selected l<5 probe c and sector phi, derive d using current accepted frame n.
Set start=c+d*a_l. Bilinearly gather four parent probes around c, clamping each
index before deriving its center p. For each nonzero-weight tap trace from start
to `p+d*a_(l+1)` once, then merge this near interval with each of its four parent
sectors `4*phi+j`, j=0..3. Average those four merged intervals and apply the spatial
weight. Use the parent record-address function separately for each sector; do not
assume an unchecked `base+j` crosses an angular-tile row correctly. Parent records
are already merged far intervals, not point samples to rotate/reproject into the
child direction. No new parent ray is traced while reading a retained parent.
Their stored sector identity stays fixed; fine reconstruction derives the retained
L1 sample direction from its actual trace frame as specified below.

For L5, average four terminal sub-sectors per stored phi. Each uses
`Dterminal=4*D5=16384`, sector index `4*phi+j` and the specified angle function with
that denominator. Start at `c+d*a5` and trace to the forward map exit. The exterior
of the finite map is clear; slab-clip the ray if the start is already outside.
A miss returns `(sky_sector_mean,0)`, a hit `(cached_source,0)`. Terminal T is zero
because the sky closes the path. There is no direct-light dispatch.

Two deliberate source corrections are required here, rather than literal copying
of `computeLastLevel`: it passes a parent-sector index to `probeDir` while using
the child direction-count denominator, and it attaches sky after the finite
1048.576-cell end even when geometry farther along that ray can remain in the
1024-square map. Use the terminal denominator above and actual map exit. This
retains four terminal traces per stored interval and the selected cadence; it
does not add a ray budget or extend the supported map size.

Use Oni's analytic `radianceIntegrateSky_` with SkyColor=(0.2,0.5,1),
SunColor=(20,10,2), SunA=5, SunS=64, and fixed intensity 5. For terminal sector i,
unwrapped angular bounds are `[2*pi*i/Dterminal,2*pi*(i+1)/Dterminal]`, rotated by
`2*pi*hour/24`. Integrate over that narrow interval, splitting at the 2*pi seam,
then multiply by `2*pi*5 / (2*pi/Dterminal)` as in the reference normalization.
Never sort wrapped endpoints: that changes a seam-crossing narrow sector into
almost the whole circle. The within-sector trace jitter changes visibility
sampling, not these fixed sky integration bounds.

The retained sky-hour setting remains -1 by default and keeps its existing
meaning: **black boundary sky**, not an automatic clock. Integer values 0..23
select the sky rotation above. `Game.Config.cppm`'s existing schema explicitly
defines this meaning; an earlier coordinator paragraph incorrectly assigned -1
automatic time and is superseded here. The selected hour is immutable for one
frame and independent of simulation, screen/lighting phases and accumulation time.
At -1 a terminal miss returns (0,0). No new night-ambient floor, automatic day
cycle or sun entity is introduced.

### Visibility, jump construction and traversal — coordinator specification

Use the same categorical material classification for acceleration and source
preparation. Build the reference's three packed visibility levels every scene
frame: each uint2 holds an 8x8 occupancy mask; levels 0/1/2 represent native,
2x2 and 4x4 cell footprints per bit. OR-reduce children, never average. Their
dimensions are 128x128, 64x64 and 32x32 records, respectively, for 172,032 bytes
(168 KiB) of construction storage. Use one structured uint2 buffer, stride 8,
with level record offsets `{0,16384,20480}` and pitches `{128,64,32}`.
Every tile is rebuilt; no dirty tracking or retained visibility bits.

**Construction schedule, coordinator clarification (2026-11-11):** exactly two
GPU dispatches implement the reference's tiled reduction and jump selection:

- `buildVisibility`, 16×16 threads and 32×32 groups: each group owns a 32×32
  native-cell tile; each thread loads the four quadrants at local offsets
  (0,0)/(16,0)/(0,16)/(16,16). Thus each resolved cell is read once, with
  x-adjacent accesses. Occupancy is opaque classification, not brightness or
  nonzero material ID; receive-only cells/gas contribute zero.
- First 4×4 threads clear 16 shared uint2-equivalent masks (two atomic-uint
  arrays, 128 bytes), then all threads synchronize. Occupied cells atomically OR
  their row-major bit `x%8 + 8*(y%8)` into the local 8×8 mask. All threads
  synchronize again before readers. There are no global atomics.
- First 4×4 threads store the 16 level-0 records; first 2×2 threads OR-reduce
  disjoint 2×2 mask blocks into four level-1 records using fixed uint32 shifts,
  masks and bit compaction (2×2 native bits become one parent bit). Store these
  globally and in a **separate 32-byte shared array**. Do not reuse the first
  level's slots as in the prototype: a reducer can overwrite another reducer's
  unread input; the barrier after those writes cannot repair that race.
- After an unconditional third group barrier, thread (0,0) reduces those four
  records and writes level 2. Total shared storage is 160 bytes/group. Each
  global record has one writer, including empty tiles. No wave-width dependency,
  global clear or inter-group reduction is needed.
- After a GPU visibility-write→read dependency, `buildJump`, 16×16 threads
  and 64×64 groups, loads at most the three masks per cell and writes one
  R8_UINT texel using the priority below. Then order jump writes before trace
  reads. Also order resolved-cell writes before visibility, and prior-frame
  readers before reuse of mask/jump allocations. No CPU wait/readback or
  per-frame allocation belongs in construction.

Task 8 specifies exact indexing, word reduction, binding/dispatch checks and GPU
tests. Independent CPU occupancy counts verify **maximal** aligned empty squares,
not just conservative safe skips; every mask bit is checked against native
occupancy. Fixtures enumerate all 1024 local positions across one-cell-per-tile
input and cover reused poisoned/full/empty/moving maps. Two dispatches and bounded
memory work are design facts; actual cost is measured during full integration.

For every native cell, write jump 0 if occupied. Otherwise write the largest
aligned empty square containing it among side lengths 1,2,4,8,16,32. This is exactly
the priority expressed by `passMapSkipDist` (`docs/oni_v22.slang:8563-8585`): the
1/2/4 tests inspect occupancy bits, and the 8/16/32 tests inspect whole masks.
Jump values describe **aligned blocks**, not a radius around the cell. Use a
1024-square R8_UINT UAV/SRV, bind as uint, no filtering/mips/normalized conversion.
After capability checks, verify actual Slang compilation, pipeline creation and
round-trip stores/loads of all seven values. An unavailable R8 storage path is a
reported feasibility blocker; it does not authorize a hidden R32 replacement.

Trace parameterizes a finite segment as p(t)=start+t*(end-start), t in [0,1).
Use a common helper for all near/coarse/fine segments; terminal rays use the same
helper after clipping to the map exit. Zero-length segments return clear.
Slab-clip to [0,1024) in both axes before indexing. Zero direction components are
handled as parallel slabs; do not perturb direction by adding epsilon or form
0*infinity. An entirely exterior segment returns clear. Include the starting
cell's positive-length intersection, including an occupied start: no special
"ignore the receiver material" rule.

At an exact grid boundary choose the cell entered in the ray direction; at an
exact corner crossing advance both axes. Cells touched only at a point have no
positive-length segment and are not hits. A crossing at t=1 is excluded and belongs
to the following segment. This defines CPU-reference and accelerated behavior
together, including negative and axis-aligned rays. Fine reconstruction's explicit
0.001-cell origin offset remains as stated; traversal adds no further bias.

At jump 0 return decoded current source and T=0, independent of source brightness.
At positive jump s, the empty block is `[floor(cell/s)*s, floor(cell/s)*s+s)`.
Advance to its next exiting face along the ray, then select the entered cell with
the same boundary convention; rebuild next-face parameters from the original ray
to avoid accumulating position nudges. Continue until a hit or clipped segment
end; a miss returns (0,1). Never use s as an unconstrained travel distance, skip
over an untested nonempty block, or declare a step-limit exhaustion to be clear.
Nonfinite arithmetic or failure to make geometric progress records a numeric
failure under the status contract. No arbitrary traversal budget is introduced.

Verify the jump output against exhaustive occupancy of its claimed blocks, and
accelerated hits against a simple native-cell DDA with identical boundary rules:
empty/full maps, single cells, diagonal/corner and axis rays, starts inside solids,
sub-cell/zero-length segments, outside starts, chunk boundaries and all jump sizes.
A tiny GPU readback test belongs in the existing tests; production uses the real
map and has no synchronous tracing readback.

### Fine reconstruction and CH meaning — coordinator technical specification

Use the active `SuperSampling::update` connector geometry
(`docs/oni_v22.slang:6796-6829`), not `update2` or the disabled raw-L0 path:

1. Fine center f is `(p+0.5)*2`. Its connector box is `[f-4, f+4]` on both axes.
2. Bilinearly gather the four L1 probes around f, using coordinates `f/4-0.5`.
   At the world boundary, clamp each probe index to the L1 domain **before**
   deriving its center, group, validity, trace frame or storage address. Repeated
   edge taps keep their bilinear weights, whose sum is one.
3. For each tap and all 16 L1 directions, derive its actual trace direction d from
   the schedule above. For a never-initialized tap only, use current frame n for
   the near connector and keep the far interval zero; it has no stored direction
   identity to preserve. Intersect the ray from the L1 center in direction d with
   the connector box's exit boundary. Call that point e. The basis direction v
   is `normalize(e-f)`; use d only for an exactly zero connector length.
4. Trace the near segment from `f + 0.001*v` to e against the current resolved
   occupancy and cached source. Merge with the tap's stored interval using
   `L = Lnear + Tnear*Lfar`, `T = Tnear*Tfar`. An uninitialized far interval is
   `(0,0)`; it does not suppress a real current near hit.
5. Give the merged RGB sample weight `bilinear_weight * 2*pi/16`. A zero-weight
   tap is skipped, not promoted to epsilon weight. Project using the actual
   connector v, not the L1 direction d or a direction from the current global frame.

This is 64 nominal connectors per fine probe before zero-weight elimination;
it is not a DDA step count or a performance budget. Endpoint and map-boundary
handling must use the common traversal contract; they may not differ between
coarse and fine tracing.

Use the orthonormal circular basis from Oni:

```text
Y0 = 1/sqrt(2*pi)
Yc_m(theta) = cos(m*theta)/sqrt(pi)
Ys_m(theta) = sin(m*theta)/sqrt(pi), m=1..4
L(theta) = C0*Y0 + sum(Cc_m*Yc_m + Cs_m*Ys_m)
angular_mean = C0*Y0
```

For samples (Li, theta_i, wi), let W=sum(wi), mean=sum(wi*Li)/W. Normalize
quadrature weights as `qi=wi*(2*pi/W)` and fit:

```text
C0   = mean / Y0
Cc_m = sinc(m*pi/5) * sum(qi*(Li-mean)*cos(m*theta_i)) / sqrt(pi)
Cs_m = sinc(m*pi/5) * sum(qi*(Li-mean)*sin(m*theta_i)) / sqrt(pi)
sinc(x) = sin(x)/x
```

W is positive for valid four-tap reconstruction; zero/nonfinite W is a numeric
failure, not a black substitute. This spells out `CircularHarmonics::Streamer`
(`docs/oni_v22.slang:6015-6102`) with exact zero-weight handling and normalized
quadrature. Subtracting the mean avoids sample-density leakage into AC: a uniform
radiance field has zero AC even when connector angles are not uniformly spaced.
The Lanczos window is applied once when fitting the fresh reconstruction, not
again when loading, interpolating or blending old history.

Blend all nine decoded RGB coefficients with the approved EMA, then encode once.
Keep negative AC components; only a reconstructed radiance value may be clamped
componentwise to nonnegative. Do not clamp the coefficients to nonnegative or
replace RGB9E5 DC with a differently normalized mean under the same codec name.

Source preparation samples prior DC at the native cell center `(cell+0.5)` with
fine-probe coordinates `(cell+0.5)/2-0.5`, ordinary bilinear weights and clamped
edge indices. Decode each C0, interpolate in FP32, then multiply by Y0. This is
the angular mean in the source formula: no additional pi, 2*pi, cosine, normal,
roughness or Fresnel factor is applied to diffuse feedback. Gas uses the same
sampling and normalization on the **updated** CH field.

Verification contracts: constant fields preserve their mean and have zero AC;
positive and negative AC survive encode/decode; a rotated analytic harmonic
rotates the recovered coefficients; zero-weight and repeated edge taps preserve
normalization; retained directions use t rather than n; source preparation sees
only pre-update CH. These are correctness checks, not visual acceptance.

### Lighting cold start — approved

On a new world, initialize all coarse intervals to radiance = 0 and
transmittance = 0, clear the single CH field to zero, and start the accepted
lighting-frame index at zero. Fine reconstruction completely overwrites each
record after source preparation has finished reading the zero initial field.
Every byte of each updated CH record must be written before scene shading reads it.

An uninitialized coarse interval contributes no far lighting; it is not treated
as vacuum with transmittance = 1. This avoids admitting speculative sky through
untraced geometry. Ordinary tracing and merging replace the initialization as
probes are visited.

The first fine reconstruction becomes the first CH field directly, without an
EMA blend against nonexistent history. Subsequent lighting frames accumulate
normally. Keep the ordinary quarter-probe schedule: all coarse probes receive
their first update within four accepted lighting frames, but that is not a
convergence guarantee. No special all-probe warm-up dispatch is added.

The owner accepts dark-to-lit startup. World replacement reinitializes these
resources; window resize, camera movement and ordinary edits do not. Retire the
old world's allocations only after their last GPU readers complete. Allocation
and initialization can be submitted during world preparation, but readiness must
establish their GPU dependency before the first frame consumes them.

### Lighting accumulation clock and rate — approved

Use the reference baseline with learning-rate multiplier fixed at one:

```text
current_weight = 1 - exp(-2 * delta_seconds)
next_CH = lerp(previous_CH, reconstructed_CH, current_weight)
```

Previous_CH and next_CH denote logical values before and after the update, not
separate allocations. Blend decoded FP32 coefficients and quantize once when
overwriting that probe's CH record. The
first fine field bypasses the blend under the approved cold-start policy.
The rate is an implementation constant initially, not a new display control.
For a constant reconstructed input, the filter incorporates approximately 63%
of a change after 0.5 seconds and 90% after 1.15 seconds. These are filter-response
calculations, not full-system convergence or visual-acceptance guarantees; sparse
cascade updates and bounce feedback also affect response.

The application explicitly supplies elapsed active presentation time, independently
of simulation time. Pausing simulation does not pause accumulation while fresh
scene frames continue rendering. Dropped rendering attempts do not advance the
lighting phase; their elapsed active time contributes to the next accepted
lighting update. Exclude minimized/suspended intervals so restoration does not
replace history merely because wall-clock time passed without rendering.

The accepted lighting submission commits both phase/history validity and the timing
baseline. Rejected/unsubmitted work commits neither. Reuse the existing injected
application timing and frame snapshot boundary; lower-level lighting logic must
not read an implicit wall clock. Hable's screen-history weighting remains separate.

### Data ownership and frame timeline — feedback schedule approved

**Approved refinement:** the owner selected a single isotropic RGB9E5 source per
cell after the RC/HRC survey, then approved replacing the CH pair with one in-place
field. Previous-CH diffuse feedback runs in source preparation before tracing, not
in a per-hit BRDF. Finish all source-preparation CH reads before fine writes.
See the approved source model in section 5.

The owner approved the temporal feedback schedule, subsequently refined to share
a precomputed source between coarse/fine tracing and update CH in place. The
coarse interval and fine CH record
layouts above are also approved; the remaining data layouts are coordinator
technical specifications included in the complete-design approval.
The [timeline source](2026-11-10-oni-frame-timeline.puml) records that schedule;
Neither schedule approval nor layout arithmetic establishes implemented behavior;
complete-design approval is recorded separately above.

Approved schedule baseline: one ordered graphics/compute queue for lighting and drawing,
with explicit resource dependencies; no new asynchronous lighting queue. Reuse
the existing owned frame packet, render-lane execution and GPU completion tracking.
Simulation publication dependencies are honored by the explicit shared-queue
contract below. A CPU publication identity or retained resource handle does not
alone prove that its bytes are safe to read or overwrite.

| Data | Representation / count (unresolved items explicitly marked) | Writer and readers | Lifetime |
| --- | --- | --- | --- |
| Committed terrain and particles | Existing publication, numerical64 / readonly24 / metadata32 where required | Existing simulation/publication writes; cell resolver reads | Source owner retains backing and protects contents until GPU readers finish |
| Resolved cell | One packed u32 per cell: low 16 bits material ID, high 16 bits FP16 Kelvin; 4 MiB | Cell resolver writes; acceleration, source preparation and scene shading read | Full rebuild per rendered scene frame; later overwrite ordered after prior readers |
| Current outgoing-radiance source | One RGB9E5 word per native cell; 4 MiB | Source preparation reads current materials and previous CH DC, then writes; coarse/fine tracing read | Full rebuild before tracing; one ordered allocation, no source-history pair |
| Source-material palette | 8 bytes per indexed material: RGB9E5 authored emission + RGB9E5 diffuse reflectance | Material publication writes; source preparation reads | Derived from authored materials at publication, not rebuilt for temperature changes |
| Material data | Shared authored identity; scalar transport lookup and visible texture/PBR bindings | Content upload writes; transport and scene shading read | World/material-resource lifetime, with completion-safe replacement |
| Winner selection | Two uint32 grids, 8 MiB total; coordinator reduction contract below | Particle projection writes; cell resolver reads | Rebuilt scratch, not persistent optical state |
| Acceleration | R8_UINT final jump values; 1 MiB plus required construction scratch | Full-domain construction writes; cascade and fine tracing read | Rebuilt every rendered scene frame |
| Coarse intervals L1-L5 | RGB radiance + scalar transmittance, packed half4 in an 8-byte record | Each cascade writes its selected probes; child/fine reconstruction reads | One persistent store per level; retain the unselected three quarters |
| Fine CH | 512 x 512, approved stride 32 bytes; 8 MiB | Source preparation reads prior DC; each fine invocation reads/overwrites only its own CH record; scene shading reads updated CH | One persistent in-place field; source reads finish before fine writes, which finish before scene reads |
| Screen history / MSAA / post-process | Section 7 specifies RGBA16_FLOAT resources, R/S/B extents and a screen-history pair | Scene, Hable reconstruction and PostProcessPass | Separate from world-space lighting; completion-safe resize |

The 32-byte fine-field record is approved above. For the reference's six-level indexing, L0 is
represented by the fine CH field, not an additional unused raw interval atlas.
At 1024 squared each of L1-L5 contains 1,048,576 directional interval records
(`probe spacing = 2 << level`, `directions = 4 << (2*level)`), or 8 MiB per level.
Thus persistent coarse storage is 40 MiB and the single CH field is 8 MiB.
With cell identity, the final jump table and the RGB9E5 source this subtotal is 57 MiB;
it explicitly excludes source simulation/terrain resources, material textures,
construction/winner scratch and all screen-space attachments. This is allocation
arithmetic, not a claim about measured bandwidth or complete application memory.

Additional specified allocations are packed terrain input 4 MiB, two winner grids
8 MiB, visibility masks 168 KiB, thermal LUT 32 KiB and isotropic BRDF LUT 32 KiB.
A full 65,536-entry source palette adds 512 KiB. Including those gives 69.7265625
MiB, still excluding existing numerical/publication buffers, visible-material
records/textures, upload/readback/in-flight status slots and screen attachments.
Screen color/history/post bytes are `32*Sx*Sy + 16*Bx*By + 8*Rx*Ry`; bloom,
exposure, any retained depth and swapchain allocations are additional. Do not
multiply the world fields by CPU frames in flight: their ordered in-place access
is intentional; upload/readback lifetime has separate accounting.

Approved schedule for lighting frame n:

1. Capture owned camera/settings/timing and coherent world/terrain/particle
   publication identities. Resolve producer dependencies and retain the inputs.
   Reuse existing publication machinery rather than changing solver storage.
2. Resolve the full current cell-material grid; build current visibility and jump
   acceleration, and prepare the current outgoing-radiance source from current
   material/emission plus previous CH DC. Both outputs must be ready before tracing.
   Camera visibility must not truncate this world-space preparation.
3. Update coarse levels L5 through L1, with a dependency between levels. Hits
   decode the current cached source rather than evaluating CH or a BRDF. Selected
   probes are current; retained probes intentionally contain older transport
   results. Direction lookup follows the approved derived last-update schedule
   above and matches each sample's traced direction.
4. Reconstruct all fine probes from these cascades and current geometry. Connector
   hits use the same cached source, replacing the experimental raw-L0 feedback
   dependency. Each invocation loads only its own old CH record for the EMA and
   overwrites that record with the packed result. No invocation reads a neighbor's
   CH during this dispatch. All earlier source-preparation CH reads must be ordered
   before these writes. There is no raw fine-field intermediate or history copy.
5. Draw current material cells using updated CH; run Hable resolve, independent
   post-processing, nearest enlargement and native-resolution UI.
6. Accepted submission advances lighting phase, history validity and the timing
   baseline; no CH allocation-role swap remains. Screen-history commit
   follows its own submitted reconstruction. Rejected/unsubmitted work advances
   neither. A later presentation failure does not undo already submitted GPU work;
   uncertain submission/device loss uses the existing error boundary.

This schedule makes previous-frame feedback explicit in source preparation shared
by coarse and fine tracing. It does not mean one exact physical bounce per frame:
retained coarse data and temporal blending affect response. Fresh emission and
sky enter current traces. The owner selected in-place CH after source preparation
removed neighboring-history reads from tracing. It saves 8 MiB compared with the
earlier pair, not the EMA's read/write traffic; the pair already avoided bulk copies.

GPU dependencies must order next-frame writes after all earlier readers, including
scene shading. One CH field suffices for this ordered schedule even with multiple
CPU frames submitted; CPU-overwritten uploads and replaced/destroyed resources
still require completion-fenced storage. No per-frame CPU wait-for-idle is implied.
World replacement initializes fresh fields and retires old resources after their
last GPU reader. Cold-start values, accumulation rate and suspension time semantics
follow the approved policies above. Projection scratch, packed publication and
screen attachments are specified below. Material ABI and radiance range/error
handling follow the codec/publication contracts above. Operational integration and
backend verification remain distinct from these specified data contracts.

### Publication, submission and failure settlement — coordinator specification

Retain the single render-owner lane and explicitly share its graphics queue with
`MatterRuntime`/`GpuSolver`. Inject that queue at runtime initialization instead
of relying on separate `getQueue` calls to happen to return the same underlying
queue. This is queue/lifetime plumbing, not a solver algorithm or hot64/cold24
layout change. There is no new asynchronous simulation or lighting queue in this
port. A future different-queue producer requires a separately specified fence
handoff; it must not enter this implementation implicitly.

Current source evidence: `MatterRuntime::beginInitialize` acquires the graphics
queue, `GpuSolver::publishTick` swaps committed/working pairs only after completion
and publishes a release-ordered live epoch, and `GridParticleGpuView::current`
checks that epoch with acquire ordering. The new consumer retains this protocol:

1. On the owner lane, validate world/terrain/particle identity and live epoch
   immediately before encoding. A stale, not-yet-encoded view is rejected and
   requests a fresh coherent packet; it is not rebound to unrelated newest bytes.
2. Encode and submit all reads of the chosen committed buffers on the shared
   queue before executing any later lane operation that can reuse those buffers
   as working state. No callback or recursive lane pump may publish a new solver
   state between validation and the end of this submission.
3. Queue order plus explicit resource transitions/dependencies protects these
   reads from later solver writes. ComPtr/shared ownership protects allocation
   lifetime, not byte immutability; both protections are needed.
4. Retain metadata, material resources, upload ranges and shader roots until
   their actual final GPU-use completion value. A CPU consumption acknowledgment
   is not that completion proof. Old terrain upload storage is likewise retired
   by its fence, not by CPU frame number.

Use the existing renderer's offscreen and presentation submission APIs. Define
three ordered submission stages, so a late failure has an unambiguous result:

| Stage | Work | State committed only on confirmed queue acceptance |
| --- | --- | --- |
| A: scene | Current publication uploads; winner reduction; jump construction; source; L5-L1; fine CH; 4x-MSAA material scene and gas | Lighting n, CH validity and active-time baseline |
| B: resolve/post | Hable reconstruction to next screen-history image; exposure, bloom, AgX to resolved-size output | Screen-history role/validity, previous camera/grid, four-phase screen index; exposure adaptation state |
| C: present | Fixed-P nearest enlargement/crop and native UI to acquired surface | Presentation evidence only |

Each stage is one command submission on the same queue; dependencies also cover
multiple render/compute passes within it. This grouping reuses `renderToTexture`
and `renderAndPresent` with pass-owned encoding, not a content-aware Renderer or
new configurable render graph. World/CH resources remain in place; screen history
uses two images because reprojection reads neighboring pixels. Stage B uses the
screen phase chosen for stage A's raster; a rejected stage B leaves that phase
available for the next scene attempt.

Use `FrameSettlement` and the actual returned fence value after **each** renderer
call, before another call can overwrite its last-settlement evidence:

- Definite pre-submit rejection: commit nothing for that stage. Earlier accepted
  stages remain committed. For example A accepted/B rejected keeps updated CH
  but retains the previous screen-history role and screen phase.
- Confirmed acceptance: commit once on the lane even if a later stage fails.
  Present success is not required to commit A or B.
- Possibly executed submit (`FrameSettlement::kSubmitFailed`): stop admission,
  quarantine dependent resources/credits and propagate the failure through
  `RenderFrameResult::m_submit_possibly_executed`. Do not retry the stage, roll
  back in-place CH or release possibly-in-flight storage. Reuse the existing
  renderer/application terminal settlement policy.
- Surface acquisition failure/minimized surface discovered before A: skip fresh
  scene work. If it changes after A/B, settle those actual submissions and report
  no successful presentation; do not manufacture rollback.
- A frame result retains the latest actual accepted completion value even when
  B or C is rejected, plus the failing stage/error and any ambiguous-submit fate.
  Stage-result recording is value-only and carries no borrowed shader resources.

On shutdown/world replacement: stop admissions and detach callbacks, settle or
cancel queued CPU work, drain accepted GPU work with the existing completion
mechanism, then release passes/publications before RHI/shader services. Attempt
independent cleanup and retain its first error. Unknown completion never becomes
permission for ordinary destruction. Lifecycle draining may wait; steady-state
rendering adds no host wait-for-idle.

These requirements need focused failure tests: A accepted/B rejected, A+B
accepted/C presentation failure, stale unencoded epoch, metadata replacement with
prior readers, and ambiguous submission retaining storage. They are not covered
by an ordinary successful screenshot.

## 5. Particle and material contract — agreed

Particles are projected onto a grid for this version, with simple rendering like
terrain. Improving the former smooth solid-particle appearance is out of scope.
Do not reconstruct an elaborate liquid/solid surface pipeline as part of this port.

### Visible PBR and minimal transport — clarified by the owner

Keep standard PBR shading for the on-screen scene, including authored base color,
normal detail, roughness, metallic response and emission. Terrain and projected
solid/liquid particle cells use the same material-shading path and texture bombing.
Evaluate diffuse and roughness-dependent specular lighting from the directional
lighting representation. Do not replace it with the old hard-coded sky/key light.

Keep transport storage minimal: cell identity/classification, the R8 jump table
and one RGB9E5 outgoing-radiance source per native cell. Shared material data is
read during source preparation, not BRDF evaluation at hits. Do not replicate full
PBR texture parameters, normal maps or multiword optical integrals per cell.
Derive any compact transport palette from the same authored material source,
rather than introducing an independently edited lighting-material database.

The owner rejected the subsequent proposal for a 32-byte per-material transport
PBR record with roughness, metallic and IOR evaluated at ray hits. Oni's complex
hit shading belongs to its experimental metallic/specular transport work; it is
not the desired baseline for this simplified port. On-screen textured/normal-mapped
PBR remains required.

After the [RC/HRC source survey](2026-11-10-oni-transport-source-survey.md), the
owner approved a single isotropic RGB9E5 source per cell with diffuse-only feedback
and full on-screen PBR. Prepare it once before coarse/fine tracing:

```text
source = authored_emission + thermal_emission(temperature)
       + diffuse_reflectance * previous_angular_mean_lighting
```

The stored quantity is outgoing radiance under an explicitly normalized isotropic
cell approximation, not radiosity or cosine-weighted surface irradiance. Previous
angular-mean lighting can be reconstructed from CH DC without decoding all AC
coefficients. Sampling/normalization follows the explicit CH contract in section 4;
do not insert an additional pi factor by copying surface-radiosity terminology.
Material mapping must give pure metals zero diffuse reflectance. Their visible
metallic PBR response remains, but specularly reflected light does not illuminate
other cells through this transport cache.

The source contains authored emission with strength already applied, thermal
emission, and reflected lighting. The approved packed-cell temperature is consumed
by both source preparation and visible PBR through the same black-body lookup
table, so no separate per-cell thermal-emission image is required. Do not use
combined outgoing source as visible emissive color and thereby count reflected
lighting twice. The full
1024 squared combined source remains 4 MiB and is rebuilt into one ordered
GPU allocation before tracing. There is no source-history ping-pong pair.

An opaque hit returns decoded cached source with transmittance zero. A clear
segment returns zero radiance with transmittance one; sky is supplied by the
existing terminal-cascade boundary rule. Black opaque cells still stop rays.
Receive-only transparent cells do not acquire a transport source merely because
they are visibly lit. Fine connector tracing and coarse tracing share this rule;
neither performs material BRDF or previous-CH queries at hits.

Accepted approximation: one source value is identical for all outgoing directions
and cannot distinguish differently illuminated opposite faces of one cell.
Acceleration cannot restore that discarded information. This is diffuse/isotropic
bounce, not directional specular multibounce. The thin-wall acceptance scene stays
unchanged so this limitation remains observable. CH retains directional incoming
lighting for final normal-mapped PBR. Its approved signed encoding is unchanged.

### Temperature-dependent emission — visible-band model approved

The owner requires black-body emissive color based on current cell temperature,
including illumination of surrounding cells by very hot material. Evaluate it
during source preparation rather than at each cascade/connector hit. The output
source format and cheap hit lookup remain unchanged. Visible material emission
must use the same thermal color/intensity, not only add bloom or transport light.

Both prototypes add thermal emission to authored material emission. They do not
use identical intensity curves: Oni `Color::BlackBody.color` at
`docs/oni_v22.slang:3510-3548` uses a chromaticity approximation and relative
intensity (T/6500)^4; `Map::Cell::shading` adds it at line 7400. WGSL
`blackBodyLumen` at `docs/prototype.wgpu:5849-5894` uses similar chromaticity but
an active pi*(T/4095)^6 intensity. These are prototype display/radiance models,
not visible-band Planck integration. Stefan-Boltzmann T^4 describes total thermal
power including infrared, not visible brightness; the source comments do not
establish physical photometric accuracy.

The owner selected a visible-band black-body model rather than either prototype
intensity curve. Use a small temperature-to-linear-HDR RGB lookup table derived
from Planck spectral radiance over the visible band and standard color-matching
functions, with an explicit engine radiance scale. Retain absolute relative
brightness across temperatures; do not normalize each temperature to unit RGB or
unit luminance. The output represents emission, independent of diffuse reflectance,
so an opaque hot metal can emit despite zero diffuse bounce.

Generate/prepare the table outside per-hit tracing. Source preparation samples it
using the selected cell's coherent Kelvin temperature and combines thermal and
authored emission before encoding the cached source. The same thermal model must
serve scene emission. The thermal LUT specification below fixes wavelength,
colorimetry, domain, sampling and nonnegative gamut treatment, using the separately
owner-approved engine-unit calibration. This approval selects the model,
not a new heat-transfer or radiative-cooling simulation.

Data publication must preserve temperature from the same selected state as the
cell's material: terrain temperature if terrain wins, otherwise the selected
particle's temperature. Do not sum temperatures/emission from non-winning particles
or introduce a second heat simulation. `sim::Cell` already has float temperature;
`sim::MatterParticle` has double temperature in kelvin. The published GPU solver
hot64/cold24 rows and current presentation metadata32 do not carry temperature.
Extend the publication path deliberately without modifying solver numerical
storage or requiring per-frame CPU readback. The publication specification below
extends terrain's material-only upload to include temperature coherently.

The eight-byte source-material palette (authored emission and diffuse reflectance)
is approved below. Temperature is per-cell/particle state and must not be baked
into that immutable material table.
The visible-band model, thermal transport scope, packed-cell temperature precision
and visible-emission reuse are approved. The publication contract below preserves
metadata32; the thermal specification accounts for its 32 KiB LUT. The resolved
cell remains four bytes and no separate thermal-emission image is introduced.

Thermal emission illuminates the world only from already-opaque transport surfaces
for this port, including liquids already classified under the approved
opaque-emitting-liquid approximation. Receive-only transparent materials and gas
do not become thermal transport sources. Heating alone does not change transport
classification: never turn a transparent material opaque merely because its
thermal RGB is nonzero. No new volumetric-emission or transparent-emitter traversal
rule is added. Gas simulation remains preserved and its receive-only presentation
scope is approved below. Keep visible thermal emission aligned with this supported
opaque-surface scope rather than introducing a visual-only glow for excluded
materials without an owner decision.

### Thermal LUT numerics — coordinator technical specification

Use CIE 1931 2-degree XYZ color-matching functions, 360–830 nm inclusive at
1 nm intervals. Source: [CIE dataset DOI 10.25039/CIE.DS.xvudnb9b](https://cie.co.at/datatable/cie-1931-colour-matching-functions-2-degree-observer),
`CIE_xyz_1931_2deg.csv`, SHA-256
`fa663e3535a7e0763a745993a1f0a192eb0275ac46ad2d1befd7626841e713c1`.
The downloaded metadata identifies CC BY-SA 4.0; retain attribution, license and
derivation metadata with the vendored data/generated table. Generation is offline
and reproducible; ordinary builds/startup never download this data.

Integrate in binary64 with trapezoidal endpoint half-weights and wavelength in
metres. For T>0, use Planck spectral radiance (not exitance):

```text
B(lambda,T) = 2*h*c*c / (lambda^5 * expm1(h*c/(lambda*k*T)))
h = 6.62607015e-34; c = 299792458; k = 1.380649e-23
XYZ(T) = integral_360nm^830nm B(lambda,T)*CMF(lambda) d(lambda)
```

For exponent above 700, use zero for that negligible spectral contribution rather
than overflowing exp. T=0 is zero. There is no pi factor, per-temperature unit
normalization, chromatic adaptation or multiplication by diffuse reflectance.
Convert XYZ to linear sRGB/Rec.709 primaries with the explicit matrix:

```text
 3.2406  -1.5372  -0.4986
-0.9689   1.8758   0.0415
 0.0557  -0.2040   1.0570
```

Project out-of-gamut negative RGB components to zero, then multiply all remaining
components by `Y / dot(RGBpositive, (0.2126,0.7152,0.0722))` to retain photopic
brightness. A zero XYZ returns zero before division. This is an explicit
nonnegative-gamut approximation; it does not preserve the original chromaticity
outside the RGB gamut.

**Owner-approved calibration:** divide every temperature by the one
constant `Y(6500 K)` from the integral, so a 6500 K black body has engine luminance
one. This keeps Oni's reference-temperature scale while replacing its T^4 curve
with visible-band brightness. It is a fixed engine-unit convention, not a new
slider. The owner explicitly approved this calibration; adjusting
it changes thermal strength relative to authored emission and sky, whereas camera
exposure scales all of them together.

The LUT has 2,048 logarithmically spaced temperature samples:
`Ti = 256*(65504/256)^(i/2047)`, i=0..2047. Each row is FP32 RGB plus zero padding,
stride 16, total 32 KiB, uploaded once as a structured buffer. Runtime first
decodes the published half Kelvin, then computes
`q=2047*log(T/256)/log(65504/256)` and linearly interpolates RGB at floor(q) and
min(floor(q)+1,2047); T=65504 addresses the last row exactly. T<=256 returns zero:
the omitted maximum RGB there is approximately 3.63e-35 engine units, far below
the RGB9E5 minimum positive value 2^-24. This is a stated storage-negligible cutoff,
not a visible-glow threshold. Publication rejects invalid/out-of-range Kelvin as
specified below. Runtime uses identical table bytes and sampling code in source
preparation and visible shading.

Binary64 source calculations on 2026-11-10 gave the following calibration evidence
(rounded; not visual acceptance or GPU validation):

| Kelvin | Engine Y | Engine RGB |
| --- | --- | --- |
| 1500 | 0.0000025192 | (0.0000081848, 0.0000010894, 0) |
| 2000 | 0.000150939 | (0.000380344, 0.0000976715, 0.00000309325) |
| 3000 | 0.00983498 | (0.0174141, 0.00830468, 0.00267633) |
| 4095 | 0.0954911 | (0.132717, 0.0886975, 0.0531728) |
| 6500 | 1 | (1.04317, 0.983632, 1.03500) |
| 10000 | 4.31582 | (3.76238, 4.29269, 6.17463) |
| 65504 | 112.467 | (77.0781, 110.264, 238.496) |

`Y(6500)` before engine normalization was 4,498,563.65869 with these conventions.
Checking every logarithmic interval midpoint gave maximum component error divided
by `max(max(exact_RGB),1e-6)` of 0.0002616 (about 0.0262%); maximum absolute error
was 0.0002885. Those checks exclude GPU log/interpolation and FP32 table rounding.
The implementation verification repeats the comparison over every finite
nonnegative FP16 temperature, includes those effects, and requires at most 0.1%
under that same mixed absolute/relative metric. This is a LUT accuracy criterion,
not a runtime performance budget. Report RGB9E5 quantization separately instead of
attributing its larger error to the LUT.

### Temperature publication — coordinator technical specification

Reuse the existing presentation data path, without changing hot64/cold24 solver
rows. Source inspection establishes that `MatterRuntime::presentationMetadata`
already reads and validates each `MatterParticle::m_temperature_k`, sorts rows by
stable ID, and preserves `m_source_row` as the numerical-row mapping. Temperature
is durable particle metadata in the current solver; ordinary motion ticks do not
evolve it. This port does not add a thermal solver or pretend a stale CPU position
is current GPU state.

Keep `GridParticleMetadataRow` at 32 bytes and natural alignment 4. Replace the
u32 material-only word at byte 12 with a named `m_material_temperature` word:

| Byte offset | Metadata field |
| --- | --- |
| 0, 4 | Stable ID low/high uint32 words |
| 8 | Numerical source-row index; UINT32_MAX means gas |
| 12 | Material ID16 + FP16 Kelvin, same encoding as resolved cells |
| 16, 20 | Existing gas X/Y binary32 positions |
| 24 | Existing binary32 radius |
| 28 | Existing physical-fill bits |

Every consumer explicitly decodes the low material bits; update the shader mirror,
metadata validation, material-set collection, census, seed admission and tests in
the same change. Preserve stable IDs, physical fill, radius and gas semantics.
Do not reinterpret this packed word as a material-table index. No temperature
sidecar or wider per-particle row is needed.

Validate and pack temperature on the CPU at presentation publication:

- Negative, NaN or infinite Kelvin: `invalid_argument`.
- Finite Kelvin above 65,504: `result_out_of_range`.
- Valid Kelvin: round to IEEE binary16, round-to-nearest/ties-to-even; canonicalize
  negative zero to positive zero. Preserve the authoritative simulation value.
- ID zero encodes the empty cell as the all-zero word. Existing reserved/unknown
  material IDs remain invalid; admitting a palette hole is not an empty cell.

Return publication failures through the existing error-returning boundary with
particle ID or terrain-cell coordinates. Reject the candidate before GPU upload
and before committing its presentation identity; do not silently publish a
clamped/default temperature or call old data the new state. Empty terrain does
not carry an independently meaningful temperature into rendering.

Terrain: `SimFramePacket::makeSimFramePacket` already owns copied `sim::Cell`
records for dirty/full-refresh chunks. Pack material and temperature together at
the game-to-render presentation adapter, and upload to one full-domain packed
terrain input grid (4 MiB at 1024 squared). A temperature-only change must advance
the chunk presentation revision and upload its packed cells just like a material
change. These upload revisions are retained publication bookkeeping, not lighting
dirty queues: the resolved field and acceleration still rebuild over the full
domain every rendered scene frame. The terrain input is distinct from the
resolved terrain-plus-particle output and is outside the 57 MiB subtotal.

Particle metadata is immutable per published inventory/temperature version.
Initial admission, loading, digging/append, replacement and any existing explicit
temperature edit prepare a complete coherent metadata resource. A temperature-only
edit must refresh metadata even if particle count and terrain revision do not
change; do not retain today's count-only refresh predicate. Track its independent
version with `GridParticleMetadataResource::m_version`, while retaining the
world/particle/terrain joint publication identity. Ordinary position-only solver
ticks reuse the same metadata resource. Stable-ID ordering and source-row mapping
must be validated again after inventory changes. Temperature changes never upload
stale CPU positions into the numerical buffers.

The existing `MatterRuntime` owner lane and immutable resource publication own
replacement and completion-safe retirement. The paired metadata/material/terrain
publication must match the numerical view selected for the draw. Initial particle
seeds also carry real temperature through this path; there is no presentation-only
default-temperature branch. No per-frame CPU particle readback is introduced.

Verification contracts: different winner temperatures with identical materials;
terrain-wins temperature; append/reordered stable IDs; temperature-only refresh;
stale identity rejection; FP16 ties/endpoints/invalid values; checkpoint and
initial-seed publication parity; unchanged solver-row strides and simulation
temperature values.

### Source-material record and mapping — approved

The owner approved an eight-byte source-material record derived from the existing
authored material, addressed by the same material identity used by resolved cells:

| Byte offset | Contents |
| --- | --- |
| 0-3 | RGB9E5 authored emission, strength already multiplied in |
| 4-7 | RGB9E5 diffuse reflectance |

```text
authored_emission = authored_emissive_rgb * authored_emissive_strength
diffuse_reflectance = linear_base_tint * filtered_texture_mean * (1 - metallic)
```

Use linear-light color interpretation and the existing material/texture preparation
facilities. Missing albedo texture contributes white. This is a derived GPU table,
not a separately authored lighting-material database. Temperature is not part of
this record; source preparation adds the selected cell's thermal emission each
rendered scene frame. Roughness, normal detail and IOR remain in on-screen PBR,
not cached-source feedback.

Empty and receive-only transparent materials have zero transport source-material
values, and their cells do not evaluate thermal transport under the approved scope.
Geometry classification remains separate from source brightness; an opaque black
material still blocks. The table is regenerated when material content is published,
not when a cell heats up. Use completion-safe publication/replacement alongside
the existing world material resources.

The palette occupies eight bytes per allocated entry (including any indexed holes
and the empty entry), outside the 57 MiB grid/lighting subtotal. It needs no
per-frame upload or additional per-cell copy. Exact palette capacity/index
validation and RGB9E5 range/error handling follow the codec/publication contract.

### Resolved cell record — approved

Use one u32 per native cell, addressed row-major:

| Bits | Contents |
| --- | --- |
| 0-15 | Material ID; zero means empty |
| 16-31 | IEEE binary16 temperature in kelvin |

This is a GPU presentation/lighting representation, not a change to simulation
storage. Source preparation and visible PBR decode the same selected temperature
and use the same thermal lookup. Material identity and temperature come from the
same terrain cell or selected particle under the already-approved overlap rule.

The owner accepted approximately 2 K spacing near 3000 K, 4 K near 6000 K, and
the maximum finite value of 65,504 K. Kelvin is nonnegative. Inputs outside the
representable supported range require explicit handling; no silent clamp,
wraparound, infinity or substitution of a default temperature is permitted.
The temperature-publication contract above defines rejection and error propagation.
Do not quantize the simulation's authoritative temperature or implement heat
transfer as a consequence of this rendering format.

The grid remains 4 MiB at 1024 squared. An eight-byte FP32-temperature record was
considered and rejected in favor of this compact layout. No per-cell material
classification or PBR payload is added to this word; classification is derived
from the selected material through the shared material data.

### Material transport classification

Material optical behavior, rather than simulation phase alone, determines transport:

| Material | Receives lighting | Stops transport rays | Emits into lighting |
| --- | --- | --- | --- |
| Opaque, no authored emission | Yes | Yes | Thermal emission according to temperature |
| Opaque, authored emission | Yes | Yes | Authored plus thermal emission |
| Transparent, non-emissive | Yes | No | No |
| Emissive liquid | Yes | Yes | Yes |

Emissive solid and liquid particles must illuminate nearby terrain: glowing
resources are gameplay cues, not merely bright pixels or bloom sources.
Emissive liquids are explicitly approximated as opaque emitting surfaces for
transport. A ray receives surface emission and terminates as in Oni. Visible
transparency may differ from transport opacity; no volumetric absorption,
distance-integrated emission, or refracted transport is introduced.

Opaque surfaces use the agreed surface shading/bounce model. Non-emissive
transparent materials receive field lighting but do not attenuate or tint rays.
Do not add artificial ambient/sky lighting inside sealed dark spaces. A lit
background composited through transparency must not be lit a second time.

Terrain and projected particles share material semantics and a coherent source
representation. Particle contributions are cleared/rebuilt every frame so motion
and removal cannot leave stale geometry. Reuse existing GPU particle state where
its contract fits; do not create a second simulation or a per-frame CPU readback.

The categorical projection, overlap priorities and sub-cell approximation below
are approved. The coordinator winner-reduction specification below fixes scratch
storage and the GPU selection implementation for complete-design review.

### Source-grounded categorical projection — approved

The current engine already publishes numerical/readonly GPU buffers and stable
particle metadata through `GridParticleGpuView`. Metadata supplies material and
stable particle ID; the numerical rows supply committed positions. Existing
`particleNativeCell` demonstrates the coordinate conversion without camera
culling. The presentation path currently adds packing, screen-facing splats,
coverage/material fields and separate liquid filtering; that machinery is not
required for a cell-projected version.

Use a categorical native-cell projection rather than density reconstruction:

1. Consume committed GPU particle state before camera culling, once per rendered
   frame, without CPU readback or changing solver storage.
2. Each enabled solid/liquid particle nominates the native cell containing its
   center. Use the existing world metric/origin conversion. A valid position at
   the closed maximum simulation endpoint maps to the last cell; do not clamp
   genuinely invalid/out-of-domain positions into the scene.
3. One selected material represents the entire cell. No density threshold,
   kernel splat, neighbor dilation, smoothing or accumulation history.
4. Existing terrain material takes precedence in a cell. Otherwise particles
   classified as transport surfaces (opaque or emissive liquid) take precedence
   over receive-only transparent particles. Within a class choose the center
   nearest the cell center; exact distance ties use the smaller stable particle
   ID. GPU execution order must not determine the result.
5. Rendering and lighting use that same resolved material identity. The material's
   agreed transport classification determines whether its cell blocks rays;
   visual transparency does not alter that classification.
6. A selected emissive particle emits with its material's surface radiance, not
   emission multiplied by the number of overlapping particles. This preserves
   the chosen surface model instead of introducing volume emission.
7. Rebuild from scratch each frame. Temporary winner-selection storage is not a
   permanent per-cell optical payload. The coordinator reduction below uses
   existing stable-ID ordering and only 32-bit atomic minimum operations;
   backend execution checks remain required.

Accepted tradeoffs: a small selected particle occupies a whole
cell; several particles can collapse to one cell; a non-winning emissive material
is not separately represented. Sub-cell motion and material boundaries are
quantized. This is a presentation/transport approximation, not a change to physical
inventory. No special preference for emissive particles is proposed.

Alternative considered: accumulate per-material particle volume/coverage and pick
the dominant contribution. That represents mixtures more faithfully but requires
additional reduction/storage and coverage policy. It is not recommended for the
minimal first port unless the categorical approximation is unacceptable.

### Winner reduction — coordinator technical specification

Use two row-major uint32 scratch grids, `best_key` and `best_row`, each 4 MiB.
No 64-bit atomics, per-cell lock or append list is required. This exploits the
existing strictly increasing stable-ID metadata ordering, verified by
`prepareGridParticleMetadata` in `App.Renderer.GridPass.Particles.cpp:57-112`.
The metadata row index is therefore an order-preserving stable-ID rank; numerical
source-row index is not.

Dispatches, with UAV memory dependencies between each:

1. Clear both scratch grids to UINT32_MAX over the whole domain.
2. For each enabled moving particle, derive its current native coordinate from
   committed hot64 and the published origin. Reject nonfinite/out-of-domain
   coordinates from the candidate set; record a numeric/publication diagnostic,
   never clamp them into a cell. The exact valid maximum endpoint maps to cell
   1023. Skip gas and cells already containing terrain.
3. Compute binary32 squared distance d2 to that native cell's center, in cell
   units. Use the same explicitly ordered multiply/add expression in both
   reduction dispatches (no reassociation into a different comparison). Since the
   candidate belongs to this cell, d2 is nonnegative and at most 0.5. Canonicalize
   zero before taking its bits. Form
   `key = (transparent_receive_only ? 0x80000000 : 0) | float_bits(d2)`.
   Atomic-min this key into `best_key[cell]`. Positive finite float bits preserve
   distance ordering; bit 31 encodes class precedence without reducing precision.
4. Revisit the identical committed input. Only candidates whose key equals the
   completed `best_key[cell]` atomic-min their metadata row into `best_row[cell]`.
   This resolves exact binary32 distance ties by stable ID. No winning payload
   writes race with the winning-index reduction.
5. A cell dispatch writes the final packed word: terrain if nonempty, otherwise
   the selected metadata row's material/temperature, otherwise zero. Every cell
   is overwritten. No later pass reads scratch to reinterpret the decision.

The two reductions share a single coordinate/key helper and immutable inputs.
Dispatch order or GPU wave layout must not change winners. Source publication
retention and queue ordering cover all reductions, not only the first one.
This defines tie precision as the committed binary32 GPU geometry, not a claim
of bit-identical solver trajectories on different devices. The two grids add
8 MiB of reusable construction scratch outside the persistent 57 MiB subtotal.

The production status path reports rejected invalid candidates; GPU validation
tests must exercise the failure. No synchronous CPU particle readback or per-frame
wait is added to discover it. The status-buffer/error settlement specified above
makes this visible at the application boundary; it is not silent disappearance.

### Gas presentation — approved

Preserve gas simulation and its existing simple alpha-blended particle appearance.
Render gas into the scene before Hable temporal resolve, using CH angular-mean
lighting for its color. Alpha-composite the lit gas over the already shaded scene;
do not light the composited background again or add an artificial ambient floor.

Gas remains separate from the categorical solid/liquid cell grid. It does not
block transport, contribute authored or thermal transport emission, or acquire a
volumetric transport model. Preserve the existing simple appearance without
restoring the removed smooth solid/liquid surface machinery. Gas can move under
ordinary simulation; the approved camera reprojection/current-color-clamp policy
does not require new particle motion vectors. Gas draw resources and metadata
remain completion-safe under the existing publication boundary.

## 6. Synthetic map and acceptance

Agreed: a deterministic synthetic map in the ordinary application, with simple
emitters/occluders, sealed and open boundaries, and predictable sky influence.
Organic world generation and random fixtures are not the visual acceptance scene.
Use normal interactions for edits and particle behavior.

The owner accepted the initial case set with additional paired-room and wall-
thickness requirements. Include all of the following in one fixed 1024 x 1024 map:

| Area | Intended visual observation |
| --- | --- |
| Empty sealed room | No sky or external-emitter illumination inside |
| Matching room with a roof opening | Sky enters through the opening and is occluded by walls |
| Sealed room with one emissive block | Surface emission, propagation and shadows |
| L-shaped room with colored walls | Indirect illumination around the corner and color bounce |
| Thin barriers crossing chunk boundaries | Shadows/leaks, chunk seams and changes after ordinary digging |
| Particle basins with sand, transparent water and emissive liquid | Sand occlusion; ordinary transparent, emissive water: receives and emits; stops rays under the current material classification; and emissive-liquid illumination of neighboring surfaces. Water's visible transparency is a presentation property, distinct from its transport occupancy. |
| Sealed two-compartment room with an open door | Source in the left compartment illuminates the otherwise empty right compartment through the doorway |
| Matched copy with the door closed | Same source/layout, but the opaque closed door isolates the empty right compartment |
| Matched copy with transparent blocks filling the doorway | The right compartment receives light through the blocks under the approved receive-only transparent-material model |
| Separate room with a one-cell-thick enclosure | Deliberately exposes thin-wall/probe leakage rather than hiding it |

All sealed rooms have opaque enclosing borders at least **two native cells thick**,
except the explicitly identified one-cell enclosure. Use two-cell-thick internal
opaque dividers in the paired-room cases as well. The roof opening, open doorway
and transparent doorway are deliberate apertures, not accidental wall gaps.
Wall thickness is an authored scene condition, not proof that leakage cannot occur.

The three two-compartment variants have identical dimensions, source material,
source strength and relative source position. Only the doorway state/material
changes. Their outer enclosures remain sealed. The transparent doorway uses
non-emissive, receive-only transparent material: visible PBR transparency must not
introduce absorption/tint into transport. Do not tune per-room ambient lighting,
exposure or emitter strength to manufacture the expected comparison.

Keep the one-cell enclosure visible and independently identified in acceptance.
Do not silently thicken it, dilate its transport occupancy or add a special
leak-suppression rule. Thin barriers and chunk-boundary edits remain separate
observations even if placed near this case.

### Lighting Lab recipe — coordinator technical specification

Add the normal selectable `lighting_lab` world profile through the existing
game-owned `PreparedWorld`/WorldJob route used by authored showcase worlds. The
recipe fills ordinary `sim::ChunkGrid` and initial particle columns before normal
world admission; no renderer branch recognizes the lab. Expose it in the ordinary
New Game profile list and through the existing profile CLI:
`app.game --worldgen-profile=lighting_lab --map-size=1024 --worldgen-seed=42`.
It uses the default authored pack with explicit lab materials, normal colliders,
solver, tools, save/load and presentation. Fixed geometry does not depend on the
seed; retain seed 42 in captures for an explicit reproducible invocation.

Coordinates below are native cell edges, +Y up, rectangles half-open. Use
1/3 metre per cell and origin (0,0). Initialize unlisted cells empty; non-hot
material/particle temperature is 300 K. A room at (x,y) of size (w,h) fills its
outer rectangle with wall, then clears `[x+t,x+w-t) x [y+t,y+h-t)`, where wall
thickness t=2 unless explicitly stated. Order operations as shell, carve, divider,
aperture, emitter, then particles. This prevents a later room fill from closing a
deliberate aperture.

```text
upper row:    sealed dark | roof aperture | emitter room | colored L
middle row:   open door   | closed door   | glass door   | one-cell shell
lower middle: chunk-crossing barriers    | thermal and PBR swatches
bottom row:   sand basin  | water basin  | magma basin  | gas / actor area
```

| Case | Exact geometry / content |
| --- | --- |
| Sealed dark | Room origin (32,768), size (208,176), no emitter. |
| Roof aperture | Same size at (272,768); remove roof cells `[368,384) x [942,944)`. |
| Emitter | Same size at (512,768); emitter rectangle `[536,544) x [844,852)`. |
| Colored L | Start with wall-filled `[752,960) x [768,944)`; carve corridor union `[754,958) x [770,818)` and `[910,958) x [770,942)`. Emitter `[772,780) x [788,796)`. Set remaining wall cells `[800,910) x [818,820)` red and `[908,910) x [820,920)` green. The upper end of the vertical leg has no direct view of the emitter. |
| Matched doors | Origins (32,512), (272,512), (512,512), size (208,176), t=2. Local divider `[103,105) x [2,174)`; doorway `[103,105) x [64,96)`. First clears doorway; second keeps wall; third replaces it with lab glass. Every source is local `[24,32) x [76,84)`. |
| One-cell shell | Room (752,512), size (208,176), t=1, no internal emitter. Exterior emitter `[740,744) x [588,604)` illuminates its left face. It is separate from the sealed-dark and door comparisons. |
| Chunk seams | Room (32,256), size (416,192), t=2. Internal barriers `[127,129) x [280,424)` and `[224,416) x [383,385)`; source `[64,72) x [328,336)`. They straddle x=128 and y=384 chunk boundaries. Ordinary digging removes `[127,129) x [344,352)` and rebuilding restores it. |
| Basins | Three U-shaped shells at (32,32), (272,32), (512,32), size (208,176), t=2; omit the roof. Contents respectively sand, water, emissive magma. |
| Thermal/PBR | Open station `[512,960) x [256,448)`: bottom slab `[512,960) x [256,258)`. Five hot-metal squares at x=528+80*j, y=288, size16, j=0..4, temperatures 300/1500/3000/6500/10000 K. Four textured PBR squares at x=528+104*j, y=368, size24, roughness/metallic pairs (0.2,0), (0.8,0), (0.2,1), (0.8,1). |

Use the following material semantic keys; numeric IDs are assigned by the
ordinary catalog and validated, never hard-coded in GPU logic. Lab-only variants
are ordinary asset records, not a second lighting palette:

| Key | Physical source / optical parameters |
| --- | --- |
| `lab_wall` | Masonry physics, opaque; linear base (0.6,0.6,0.6), roughness0.8, metallic0, IOR1.5, no emission; white unit texture and flat normal. |
| `lab_red`, `lab_green` | Same physics/roughness, base (0.7,0.05,0.05) / (0.05,0.7,0.05). |
| `lab_emitter` | Opaque masonry; base (0.6,0.6,0.6), authored emission RGB=(1,0.7,0.35), strength8. All room emitters use this exact record. |
| `lab_glass` | Existing glass physics and receive-only transparent classification; base white, opacity0.25, roughness0.1, metallic0, IOR1.5, emission0. No transport tint. |
| `lab_hot_metal` | Iron physics, opaque, base (0.7,0.7,0.7), metallic1, roughness0.4, authored emission0. |
| `lab_pbr_0..3` | Masonry physics, base white; reuse the existing showcase tread albedo/normal texture pair and its texture period/normal scale. Only roughness/metallic vary as listed. Emission0. |
| `lab_magma` | Clone existing magma numerical properties and already-opaque emitting-liquid classification; authored emission=(1,0.25,0.02), strength8, temperature1800 K. |

Sand/water use existing authored materials and numerical properties unchanged.
Initial particle positions in each basin are local `(8+i+a, 80+j+b)` for
i=0..47, j=0..15 and (a,b) in {(0.25,0.25),(0.75,0.25),(0.25,0.75),(0.75,0.75)}:
3,072 particles per basin, zero velocity, volume0.25 cell-area units through the
ordinary seed builder. Assign stable IDs in basin order then row-major cell and
listed sub-cell order. Do not reconstruct numerical hot/cold rows in the recipe.
Place 64 ordinary oxygen gas particles on an 8x8 lattice at `(776+2*i,80+2*j)`;
use the catalog's normal gas seed construction and no authored/thermal emission.
Normal simulation evolves all admitted particles after start; frozen observation
uses the ordinary Pause action rather than a replacement solver.

Add a two-cell floor `[0,1024) x [0,2)` and normal controlled-actor spawn at
(744,16) in the otherwise open bottom passage. The map has no global enclosing
roof: sky tests can reach the world boundary. Initial overview camera centers on
(512,512), roll0, orthographic vertical span1056 cells; horizontal span follows the
real framebuffer aspect. Region closeups center on each case's rectangle and use
vertical span=1.25 times its height. Fit/camera actions remain ordinary client
camera operations and do not alter lighting resources.

The global sky default stays -1. Acceptance explicitly visits -1 (emission-only)
and 12 (sky), through the retained control, using identical scene geometry and
manual EV0/bloom-off captures first, then default auto-exposure/bloom for appearance.
No map-triggered settings override or per-room exposure exists. Record actual
simulation tick, accepted lighting frame and elapsed active time with each image;
elapsed time is observation metadata, not a convergence test. Save/reload a dug
door/barrier and moving-particle state through ordinary checkpoint operations.

For this port, rendered world admission is explicitly 1024-square and the game
startup default becomes 1024. Other engine simulation sizes remain available to
their owning APIs/tests; the interactive port rejects unsupported rendered extents
before expensive candidate preparation, including unsupported checkpoint bounds.
It must not silently resize worlds or select another renderer. The UI reports the
supported scope and does not offer an unsupported size as a usable lighting mode.

The owner validates images and behavior. Engine frame capture is the image source;
profiler screenshots do not replace visual acceptance. Mechanical correctness
checks and operational coverage remain useful but do not constitute visual proof.

## 7. Automated display coverage — required; detailed contract proposed

The owner requires an `app.game` command-line flag that automatically cycles the
display panel's options and resizes the real window. Proposed spelling:
`app.game --exercise-display`.

Proposed implementation contract:

- One small game-owned driver calls the same production actions as the panel.
- Cover every retained boolean/enum, representative numeric boundaries/defaults,
  supported display choices, and Apply/Keep/Revert/timeout/Reset where retained.
- Cover even/odd/small/large supported extents, repeated resize, fullscreen/windowed
  transitions and minimize/restore. Add relevant combinations as pipeline features
  return; do not build an exhaustive Cartesian-product matrix.
- Wait for real application acknowledgments, observed window/framebuffer state
  and resumed rendering, with bounded deadlines. Minimized presentation is
  expected suspension, followed by successful restoration.
- Produce a concise per-step coverage report and machine-readable completion
  result. Unsupported/incomplete coverage must remain distinguishable from pass.
- Isolate exercise settings storage from the user's preferences. This is test
  isolation, not backward compatibility or a migration mechanism.
- No GUI clicking, alternate renderer, mocked window or second settings policy.
- Passing establishes operational behavior only. The owner still judges visuals.

### Retained display controls — approved

| Area | Retained controls |
| --- | --- |
| Window | Window mode, monitor, resolution, refresh rate |
| Pacing | Pacing mode, frame cap, VSync, unfocused throttling |
| Resolution | Pixelization factor |
| Exposure | Automatic/manual mode, EV adjustment, adaptation time, automatic EV limits |
| Bloom | Enabled, intensity, threshold |
| Inspection | Sky hour, including the owner's -1 default, and lighting diagnostic view |

Retain Apply/Keep/Revert/confirmation timeout, Reset and Save where applicable.
Post-processing edits remain independent of world-lighting and reconstruction
history. Preserve the distinction between display-mode confirmation transactions
and immediate live post-process/inspection controls; concrete exercise cases must
cover the actual production action semantics rather than imposing a second policy.

Remove the Retro master switch, palette controls, AA controls, lighting enable
switch, ray budget and cascade-count controls. The completed application has one
lighting/reconstruction pipeline. Diagnostic views inspect its data, not select
an alternative lighting algorithm. The diagnostic enum, numeric domains/defaults
and representative exercise matrix are specified below.

### Presentation responsibility split — owner decisions

- Remove palette quantization, its controls and palette-only processing.
- Retain pixelization as a resolution/performance control compatible with temporal
  MSAA super-resolution. It must lower scene rendering work, not merely add a
  pixelation filter after full-resolution shading.
- Rename `RetroPass` to **`PostProcessPass`**. This is also a responsibility cleanup,
  not just a symbol substitution over the existing combined implementation.
- Post-processing runs after resolve and is independent of temporal supersampling
  and pixelization. Bloom/exposure must not require a specific AA flag or pixel mode.
- Hable temporal MSAA is always enabled in the completed port. Pixelization is
  the rendering-resolution control; remove the AA selector and enable/disable
  combinations. The cleanup milestone may temporarily use simpler rendering
  before Hable integration, but this is not a retained runtime alternative.
- The rename plan must cover class/settings/API names, matching
  `App.Renderer.PostProcessPass.cppm`/`.cpp` files, the
  `engine.app:renderer.post_process_pass` partition, umbrella exports, CMake
  registration, callers, shader names where appropriate, diagnostics and retained
  tests. No compatibility alias for `RetroPass` is required.

Specified responsibility boundary: temporal reconstruction
owns its sampling/history resources and produces a resolved scene-linear HDR
image. `PostProcessPass` consumes that image and owns exposure/adaptation, bloom and
AgX/output treatment. It does not own the scene draw callback, choose an AA mode,
or inspect temporal-history policy. Reuse the existing pass/submission facilities;
do not introduce a configurable generic post-processing graph.

Approved resolution contract: pixelization factor P controls the resolved
scene's size relative to the window. Hable's 2x reconstruction operates upstream
of that resolved size; post-processing runs at resolved scene resolution, followed
by nearest-neighbor enlargement to the window. UI remains native resolution.
For a 1920 x 1080 window, P=1 means 960 x 540 MSAA raster -> 1920 x 1080 resolve;
P=2 means 480 x 270 MSAA raster -> 960 x 540 resolve, then enlargement to the window.
These are divisible examples, not an even-window-size restriction. The owner
approved padding and cropping for arbitrary positive framebuffer extents, preserving
exact 2x reconstruction and integer-sized pixelization rather than stretching.

### Arbitrary presentation extents — approved

For each axis independently, let W be the actual presentation framebuffer extent
in pixels and P the positive integer pixelization factor:

```text
R = ceil(W / P)    visible resolved content extent
S = ceil(R / 2)    4x-MSAA raster extent
B = 2 * S         resolve backing extent
```

Account for the padding in the scene projection so the final visible W-pixel
region preserves the intended camera framing. Reconstruct the full B extent;
only the R content region is valid for post-processing, history and statistics.
Enlarge that content by exactly P with nearest-neighbor sampling, then crop the
R*P-W excess pixels at the right/bottom presentation edges. UI remains at native
framebuffer resolution. Window logical coordinates must not be substituted for
framebuffer pixels on scaled displays.

Padding outside R must not contribute to exposure statistics, bloom filtering or
valid temporal-history samples. Bound filter/reprojection footprints to valid
content; allocation bounds alone are insufficient. A history sample without a
valid contributing footprint falls back to current reconstruction under the
approved history policy. The final partial pixelization block inside R is content,
not backing padding; its output is cropped rather than stretched.

For 1919 x 1079 at P=2: R=960 x 540, S=480 x 270 and B=960 x 540. Enlarge to
1920 x 1080 and crop one pixel at the right and bottom. For 1921 x 1081 at P=2:
R=961 x 541, S=481 x 271 and B=962 x 542; this additionally exercises a padded
resolve row/column outside valid content. These arithmetic cases belong in the
permanent display exercise alongside divisible extents.

If either framebuffer dimension is zero, suspend scene rendering and temporal
advancement normally. Do not fabricate a one-pixel extent for minimized windows.
The coordinator projection/reconstruction specification below supplies the shader
coordinate formulas within this approved extent and framing contract.

Current evidence: `spatial_msaa4.slang` requires exact 2x source/output extents;
`RetroPass::validateFrameSettings` couples that relation to post-effect validation.
Removing validation alone would not repair the shader's dimension contract.

### Temporal reconstruction reference and approved history policy

Primary reference: John Hable, [Temporal Super Resolution via Multisampling](https://filmicworlds.com/blog/temporal-super-resolution-via-multisampling/),
2025-05-10, inspected during this design discussion. The checked-in
`third_party/HableMSAA/UpsamplingViaMultisampling.hlsl` and its attribution are
reuse candidates; the existing README describes only an earlier spatial milestone
and is not authority for this port's temporal integration.

The article specifies 4x MSAA, a four-frame half-source-pixel jitter cycle,
2x reconstruction on each axis, continuous diamond-gradient weights,
reprojected history clamped to the actual contributing sample color bounds,
and current-frame influence from 5% stationary to 25% in motion. Those reference
values are the approved starting point, not a newly exposed tuning panel.
The article's boundary snippet must not be copied blindly: unusable history must
fall back to the current reconstruction, never keep a clamped out-of-frame sample.

Approved screen-history policy:

- Reproject with current/previous cameras for the fixed world-cell presentation.
  Do not add particle trajectory tracking or a second particle representation.
- Keep Hable's contributing-sample color clamp and reference motion weighting;
  do not bring back the alternative temporal filters or add speculative rejection
  systems. Moving contents can still expose temporal artifacts for owner review.
- Reset screen history on first use, world replacement, camera cuts, or changes
  to the raster/resolved sampling grid, including relevant resize/pixelization
  changes. Ordinary pan/zoom uses reprojection where geometrically valid.
- Advance temporal phase and commit history only for successfully submitted
  fresh scene frames, not UI-only frames, minimized frames or rejected work.
- Keep this history scene-linear and unexposed. Exposure/bloom changes do not
  invalidate it. World-space Oni accumulation is independent and is not reset
  by screen resizing or camera motion.
- Odd positive extents use the approved R/S/B padding-and-cropping contract above.
  History validity includes the content region, not merely the backing allocation.

Implementation must preserve sample coverage at shader-defined cell/material
boundaries inside chunk quads; hardware MSAA coverage of the outer quad alone
does not provide coverage for those internal boundaries. Specify the sample-rate
shading path explicitly and profile its real cost rather than assuming four MSAA
samples are free or claiming a performance gain from the resolution ratio alone.

### Screen resources and coordinates — coordinator technical specification

The scene pass, reconstruction and post-processing retain separate ownership.
The reconstruction owner supplies the padded/jittered camera snapshot to the
scene pass; the renderer boundary itself remains camera-free. Allocate:

| Resource | Shape / format | Owner and access |
| --- | --- | --- |
| Scene color | Sx by Sy, RGBA16_FLOAT, 4 samples | Scene writes, Hable reads individual samples |
| Screen history pair | Bx by By, RGBA16_FLOAT, single sample, two images | Hable reads prior / writes next; only R is valid |
| Post output | Rx by Ry, RGBA16_FLOAT, single sample | Post writes display-linear color; presentation reads |
| Bloom pyramid | Ceil-halved R, RGBA16_FLOAT, single sample | Post-owned, valid-region-only filtering |
| Exposure | Existing histogram/reduction storage and adapted scalar EV | Post-owned; no dependency on a legacy AA flag |

No lighting history copy, screen-space velocity texture, reactive-mask image or
per-particle motion vectors are required. For the flat world, camera reprojection
is computed from the world plane and the previous accepted camera. Preserve any
depth attachment needed by unrelated ordinary scene draws; do not manufacture a
depth field for categorical material lookup. Check format/sample/UAV capabilities
through the current RHI at setup; unsupported requirements return an explicit
error, not an unapproved alternate AA path. These attachment choices need actual
backend pipeline/execution validation.

Let c be homogeneous clip coordinates from the **unjittered** camera whose
visible framing/aspect describes framebuffer W. Define per axis `a=W/(P*B)`.
Padding extends right/bottom, so transform clip coordinates before division:

```text
cx_padded = ax*cx + (ax-1)*cw
cy_padded = ay*cy + (1-ay)*cw
cz_padded = cz; cw_padded = cw
```

These signs use the engine's canonical +Y-up clip coordinates and top-left pixel
coordinates. They are not a backend-specific Y flip. The viewport is S, and the
row-vector matrix convention is unchanged. For accepted screen phase s in 0..3,
use Hable's raster-pixel jitter `j=(0.5*(s&1), 0.5*((s>>1)&1))`. A positive j shifts
the rendered image right/down; add `2*j.x/Sx*cw` to padded X and subtract
`2*j.y/Sy*cw` from padded Y. Reconstruction cancels that shift as specified below.

For a stationary resolved-space edge coordinate q, its visible-camera NDC is
`(2*P*q.x/Wx-1, 1-2*P*q.y/Wy)`. This mapping, not `q/R`, is used to reconstruct
world position and to reproject into the previous accepted frame. Thus camera
framing remains exact even when R*P exceeds W. The final native pixel (x,y) loads
post output at `(floor(x/P), floor(y/P))`; no normalized-UV stretch is allowed.

Material lookup is sample-rate: each MSAA sample reconstructs its own world-plane
position, selects exactly one native packed cell and shades that material.
Shader-defined empty/occupied boundaries inside chunk quads therefore have real
per-sample coverage. All four samples may not reuse one pixel-center material ID.
Use explicit per-sample interpolation/evaluation and prove execution/sample-index
mapping on the backend; merely allocating a 4x target is insufficient. Texture
gradients come from continuous world UV before cell selection, fract or texture
bombing. Do not derive a mip level from discontinuous material IDs.

### Hable reconstruction kernel — coordinator technical specification

The checked-in `third_party/HableMSAA/UpsamplingViaMultisampling.hlsl` is the
earlier **spatial** snippet. It does not contain the accepted temporal algorithm.
The temporal article specifies continuous diamond weights, four-phase bucket
offsets and contributing-sample bounds. Translate those explicitly; do not ship
the spatial absolute-gradient selector under a temporal name.

Require the standard 4-sample pattern in raster-pixel top-left coordinates:
sample 0=(3/8,1/8), 1=(7/8,3/8), 2=(1/8,5/8), 3=(5/8,7/8). Confirm actual sample
positions/index order on both target backends. A mismatch is an unsupported
configuration requiring a deliberate port decision, not unnoticed wrong filtering.

Express this pattern on a quarter-raster-pixel integer lattice: node (nx,ny)
is at `((nx+0.5)/4, (ny+0.5)/4)`. In each 4x4 lattice block the known nodes are
(1,0), (3,1), (0,2), (2,3), in sample-index order. Implement one procedural lattice
lookup rather than copying the spatial snippet's hand-written bucket expressions:

- A known node loads its corresponding MSAA pixel/sample directly.
- Otherwise find the closest known node on each side in its row and column.
  Row residues give sample X residues `[1,3,0,2]`; column residues give sample Y
  residues `[2,0,3,1]`. Use signed floor division/modulo for nodes outside the first
  block, then derive the source pixel and sample index from those known nodes.
- Let those colors be L,R,U,D. Use Hable luminance `(0.25,0.5,0.25)` for this
  **gradient test only**, `dh=max(1e-5,abs(lum(L)-lum(R)))`, analogously dv,
  `wh=dv/(dh+dv)`. Return `wh*(L+R)/2 + (1-wh)*(U+D)/2`.
- Expand componentwise RGB min/max with every contributing original sample,
  including both gradients, not only the final interpolated color. A directly
  known node contributes its own sample to the bounds.

For resolved output pixel (x,y), average the four nodes
`(2*x+dx+4*j.x, 2*y+dy+4*j.y)` for dx,dy in {0,1}. Their reconstructed stationary
positions cancel the projection's +j shift. Union their original-sample RGB bounds
for history clamping. This is the article's four offset bucket selections written
as an explicit lattice rule. Reconstruction is evaluated over B; pixels outside
R are initialized but marked invalid and never enter history filtering/statistics.

Boundary sample handling is explicit. A source sample is valid only if its pixel
exists in S and its stationary position `2*(source_pixel+sample_position-j)` lies
in `[0,R)` on both axes. An out-of-range requested sample is replaced by the
nearest valid sample among the clamped source pixel and its clipped 3x3 pixel
neighborhood (all four sample indices), measured in stationary resolved space;
ties use source row-major pixel then sample index. This defines edge replication
without treating padding as real content. Positive extents and the stated pattern
always provide a valid candidate, including R=1. Missing candidates indicate a
contract error. Only boundary requests take this path; ordinary interior loads
use the direct sample lookup. The replicated sample enters the color bounds.

Reproject each resolved pixel's center through the unjittered current world-plane
position and previous accepted camera/padded-grid mapping. Sample prior history
bilinearly only if every nonzero-weight texel is inside its valid R rectangle.
Do not clamp an invalid history position into the image. Motion magnitude is
measured in **resolved pixels**, excluding projection jitter. Define
`motionT=saturate((length(previous_pixel-current_pixel)-0.1)/0.4)` and current
weight `lerp(0.05,0.25,motionT)`. With valid history, clamp it to the contributing
sample bounds and blend; otherwise write the current reconstruction directly.

World replacement, explicit camera-cut generation or changes to W/P/R/S/B reset
screen history. A camera cut is an explicit client discontinuity (teleport/reset/
world activation), not a guessed movement threshold; ordinary pan/zoom reprojects.
Exposure/bloom/sky-hour edits do not reset it. Diagnostics are drawn as an
inspection overlay **after** normal scene reconstruction and post-processing,
so entering/leaving diagnostics cannot contaminate the scene history or exposure.
The ordinary scene/lighting still update while diagnostics are visible. Native UI
is composed last. Diagnostic content/ranges are enumerated below.

Verification contracts: constant-color preservation across all four phases;
known-node and diamond lookup agreement; stationary target positions independent
of phase; padded projection corners; no out-of-range fetch at R=1/odd extents;
padding exclusion from statistics/history; correct rejection of offscreen history;
sample-rate material boundaries; accepted/rejected stage-B history-role changes.

### Post-processing math and interface — coordinator specification

`PostProcessPass` receives an immutable resolved-image view, allocation extent B,
valid rectangle [0,R), finite post settings, injected active delta, world identity
and the renderer's encoding/completion context. It returns pass-owned display-
linear output plus submission-local observations. It neither receives a scene
callback nor selects an AA mode. The reconstruction owner supplies the image after
its writes; encode both owners in stage B with explicit UAV/SRV dependencies.
Post ownership retains its output until all presentation readers complete, including
retained-picture draws. CPU acknowledgments do not release it.

Retain the existing 64-bin log-luminance histogram, log2 range [-8,8], 10–90%
percentile trim, Rec.709 luminance and middle gray0.18. Its input is the **unexposed
resolved HDR** valid R region. Luminance image extent is `min(R,256)` per axis,
R32_FLOAT; texel i covers integer source coordinates
`[floor(i*R/L), floor((i+1)*R/L))` per axis. These nonempty blocks partition R
without overlaps or backing padding. Arithmetic-mean each block, then histogram
`log2(max(Y,2^-8))` with clamped bin index as in the existing shader. Each luminance
texel contributes one count; this is the specified downsampled metering scheme,
not an exact per-source-pixel percentile.

Percentile trimming must retain fractional bin counts as FP32. The current
`uint(hi-lo)` truncation loses the entire valid mass for a one-texel image; update
the CPU oracle and shader together. Compute the count-weighted mean of bin-center
log luminance and target EV=`log2(0.18)-mean`. Positive R always has a positive
retained weight. Invalid/nonfinite input uses the numeric-status contract.

Retain exponential adaptation, `alpha=1-exp(-active_dt/tau)`, then
`next=clamp(previous+(target-previous)*alpha,ev_min,ev_max)`. The 1x1 R32_FLOAT
exposure pair belongs to post-processing. First use/world replacement initializes
previous offset0; resize retains it. Manual mode uses offset0; re-enabling auto
starts from0. Settings edits do not reset Hable/CH. Stage-B acceptance commits
exposure role and its active-time baseline; rejected attempts retain both, and
suspension time is excluded. Manual exposure is an additional EV offset:
`E=exp2(manual_ev+adapted_offset)`.

Reuse `spd_bloom.slang`'s soft-knee threshold (knee0.5 exposed units), Karis
prefilter, level weights [1,0.5,0.25,0.15,0.1,0.05] and progressive tent upsample.
Bloom level0 extent is ceil(R/2); subsequent extents are ceil(previous/2), stop
at 1x1 or six levels. They are separately sized images/views, not native floor-
halved mip levels mislabeled as ceil-sized. A 2x2 source filter clamps each tap
to the last valid texel, so a partial last row/column is replicated rather than
replaced by zeros. Every output texel is initialized at every active level.

Threshold the 2x2 scene average in scene units with threshold=`setting/E` and
knee=`0.5/E`, exactly one exposure conversion. Preserve the existing threshold
curve and Karis weight `1/(1+Ybright)`; black stays black. Build unweighted box-
filtered levels, then descend: coarsest result=w_last*raw_last;
`result_l=w_l*raw_l+tent(result_(l+1))`. The tent is separable [1,2,1]/4 in each
axis, with bilinear samples one coarse texel apart around the mapped fine center,
clamped to valid coarse texel centers. Barriers separate downsample/upsample levels.
This explicitly preserves weights without accidentally multiplying them at every
recursive reduction. Extract the existing helpers; the old shared-memory SPD
launch may be retained only if it produces these same ceil-edge results.

Combine in FP32 at R: `scene + intensity*bilinear(bloom0)`, then invoke the existing
attributed `displayAgxPunchy(combined,E)` from `display_agx.slang` once. Output
RGBA16_FLOAT display-linear RGB, alpha1. Bloom-off skips its compute work and uses
zero contribution; automatic/manual exposure works in either case. No exposure
or AgX is applied in Hable history or duplicated at presentation. Final nearest
enlargement samples exact integer pixels as specified above; an sRGB surface
encodes in hardware, an SDR UNorm surface encodes once in the final shader. Native
UI follows the existing surface color-space convention. Preserve Filament notices.

Correctness checks include R=1/odd/non-square extents, constant/black HDR, first
and resumed adaptation, bounded finite EV extremes, fractional percentile counts,
bloom edges/disabled path, exactly-once exposure/encoding, independent post edits,
and rejected stage-B commits. No old AA-toggle prerequisite remains.

### Display values and diagnostic views — coordinator specification

Preserve the existing game post domains/defaults (not the low-level pass's former
default-off fixture settings): auto exposure=true; manual EV0 in [-16,16]; tau0.7
seconds in [0.1,5]; auto limits[-4,4], each in [-8,8] with min<=max; bloom=true,
intensity0.125 in [0,2], threshold1 in [0,16]. Reject nonfinite and invalid tuples.
Pixelization is an integer P in [1,8], default3, with P1 meaning no pixelization
reduction but still Hable reconstruction. Sky hour is integer[-1,23], default-1
(black boundary sky). These are user-control domains, not inherited lighting work
budgets. Window/pacing defaults and domains remain those of `DisplaySettings`.

Use diagnostic enum `{off, probes, source, occupancy, jump, cascade}`; unknown
values are configuration errors. `probes` shows CH angular mean with the fine
probe lattice; `source` shows cached outgoing RGB; `occupancy` shows empty black,
opaque white, receive-only cyan; `jump` maps 0/1/2/4/8/16/32 to distinct labeled
colors; `cascade` shows the selected L1..L5 interval atlas with angular/spatial
coordinates and decoded RGB/T. Cascade selection (default1) is inspection-only,
not an active cascade-count setting. The probe/source/cascade RGB display curve
is fixed `max(RGB,0)/(1+max(RGB,0))`; UI labels state it is an inspection mapping,
not scene exposure. Clicking/hovering may show decoded numeric values through
existing asynchronous inspection, never synchronous per-frame image readback.

Draw inspection after normal post-processing using the matching submitted world
state and captured camera, then native UI. Off restores normal presentation;
underlying scene, post and histories continue in every mode. The probe overlay
deliberately replaces the current temporary source override after preservation.

### Permanent display-exercise cases — coordinator specification

`--exercise-display` selects a game-owned interactive command; it conflicts with
other command selectors, uses the real window, and runs the normal lighting_lab
profile at1024. `--exercise-output=<directory>` selects its fresh artifact directory.
Use an isolated display overlay within that directory, with ordinary precedence
and Save/Reset code; never load/write the user's overlay. Report the resolved
configuration and any explicit CLI assignments that prevent a tested live value.

The driver is an asynchronous state machine advanced by production action results,
window events and injected deadlines. It calls the same game actions as the panel,
not private renderer setters. For each live post/inspection edit, wait for a fresh
presented frame stamped with the new settings revision. For display transactions,
wait for matching ID/generation/revision, observed geometry and fresh presentation;
start Keep/Revert timeout only after the production successful acknowledgment.
Record requested logical window dimensions separately from observed framebuffer
dimensions and use the latter for all R/S/B assertions.

Run this ordered representative matrix, restoring baseline between groups:

1. Initial load/present; capture baseline IDs, extents, settings and output image.
2. Windowed requests 1280x720, 1919x1079, 1921x1081, 640x481, then back to1280x720;
   repeat the odd/even pair three times. Request the platform's minimum useful
   window size and record its actual clamp. R=1 is additionally a GPU unit case,
   not a claim that every window manager permits a1-pixel framebuffer.
3. P=1,2,3,8, including both1919/1921 cases atP2; verify exact observed R/S/B,
   history reset and unchanged world-lighting identity. Real scaling may yield
   different framebuffer sizes: report those, never forge the desired pixel case.
4. Auto exposure on/off; manual EV=-16,0,16; tau=0.1,0.7,5; valid auto-limit
   pairs[-8,-8],[-4,4],[8,8]. Bloom off/on; intensity0,0.125,2; threshold0,1,16.
   Include auto-off+bloom-on and auto-on+bloom-off at an odd extent. Restore
   baseline after each extreme instead of accidentally combining all extremes.
5. Sky=-1,0,12,23; every diagnostic enum; cascade inspection levels1 and5.
   Verify no world/CH reset and no scene-history contamination by the overlay.
6. Every pacing enum; cap domain minimum/default/maximum; VSync on/off and
   unfocused throttle on/off. Verify acknowledged policy and observed throttle
   reason, not an invented exact-FPS target. Real focus-loss coverage requires an
   acknowledged platform event; if the host cannot request it, record incomplete.
7. For every enumerated unambiguous monitor, exercise fullscreen at its current
   supported mode and return to windowed. On the primary monitor also choose the
   smallest/largest supported resolution and lowest/highest known refresh at a
   supported resolution, deduplicating identical modes. No guessed refresh values.
8. Separate disruptive trials for Apply+Keep, Apply+Revert, and Apply+timeout;
   Reset overrides, Save, reload the isolated overlay through the production
   resolver, and verify saved acknowledged values. Also reset live post/inspection
   fields through their own action. Test invalid drafts before native mutation.
9. Minimize and restore twice via the ordinary window service. Verify real
   suspension, no fresh phase/history advancement during it, and resumed frames.
   Close through the production quit action and require successful drain/exit.

Use a10-second per-transition deadline after readiness, confirmation's existing
production duration plus10 seconds for rollback, a180-second initial-world-load
deadline, and a900-second run deadline. These bound automation, not accepted game
performance. A timeout preserves logs/state and reports incomplete/failure; no
blind retry loop. Capability absence is `unsupported`, contradictory advertised
support or failed operation is `failed`, external interruption/unobserved required
event is `incomplete`. Continue independent non-mutating reporting after failure;
restore baseline where the production transaction permits it and always attempt
normal shutdown. Never overlap an outstanding native trial with a new one.

Write `display-exercise.json` with schema_version1, binary/source identity,
backend/GPU/driver, capability snapshot, ordered cases (requested action, actual
acknowledgments, extents, revisions, timestamps, status/reason, image/log paths),
cleanup outcome and overall status. Write artifacts incrementally/atomically so a
crash leaves useful evidence. Overall `passed` requires every applicable case and
successful cleanup; `unsupported`/`incomplete` are not pass. Exit0=passed,
1=failed,2=invalid invocation,3=incomplete-or-unsupported; retain the precise state
in JSON. Screenshots support owner inspection, not automated visual approval.

## 8. Repeatable GPU profiling — required

The owner has enabled hardware-counter collection and requires reliable,
cost-effective automation using Nsight Systems and Nsight Graphics. Permission
is established; successful acquisition/export still needs an end-to-end check.

Installed paths discovered on this host (re-discover if the host changes):

- Graphics UI: `/home/bek/.local/nvidia/NVIDIA-Nsight-Graphics-2026.4/host/linux-desktop-nomad-x64/ngfx-ui`
- Graphics CLI: `/home/bek/.local/nvidia/NVIDIA-Nsight-Graphics-2026.4/host/linux-desktop-nomad-x64/ngfx`
- Systems UI: `/home/bek/.local/nvidia/nsights-system/host-linux-x64/nsys-ui`
- Systems CLI: `/home/bek/.local/nvidia/nsights-system/target-linux-x64/nsys`

Read-only discovery on 2026-11-10 established:

- `nsys --version`: 2026.5.1.161-265138896106v0.
- `nsys profile --help` advertises Vulkan/API annotations, Vulkan GPU workload
  tracing, GPU metrics, frame/duration bounds and structured exports.
- `ngfx --help-all` advertises GPU Trace Profiler, frame/submit/time capture
  triggers, duration/frame bounds, `--auto-export`, architecture-specific metric
  sets, multipass metrics, shader profiling and graphics-capture replay.
- The Graphics help commands printed their help and returned exit code 1.
  No application capture or export was performed in this design session.

Proposed workflow contract:

1. A maintained command/script launches the real application at repeatable map,
   camera and settings, captures a bounded useful interval and exports results.
2. Use Systems for CPU/GPU timeline, submission/wait diagnosis and coarse metrics;
   use Graphics for pass/shader attribution and targeted hardware counters.
3. Serialize captures; do not run both profilers against the same workload at once.
4. Start with a short capture answering one question. Enable costly counters or
   multipass collection only when needed. Do not capture every display combination.
5. Preserve native reports, raw exported data and a concise machine-readable
   analysis comparable to `out/diagnostics/worldlight/analysis.yaml`. Confirm the
   actual export command/schema before promising a vendor YAML output.
6. Record binary/source identity, tool/driver/GPU, settings, clock policy, map,
   camera, simulation/capture interval and collection mode. Multipass results
   require a stable or replayed workload, not assumed equivalence of moving liquids.
7. Verify expected GPU ranges, frame coverage and requested counters exist; file
   existence and launcher exit status alone do not establish a useful capture.
8. Give acceleration, cascades, fine reconstruction, grid/particles, resolve and
   post effects distinct GPU ranges. Do not sum nested ranges or attribute mixed
   draw/dispatch ranges exclusively to a compute pass.
9. Compare matched captures; distinguish profiler overhead from normal frame
   timing. Reuse artifacts instead of repeating collection without a new question.
10. Bound process/capture/export lifetimes, retain failures, and clean up only owned
    processes. No blind retry loop or manual GUI dependency for routine captures.

The implementation contract below fixes commands, artifacts and readiness/error
semantics. End-to-end acquisition/export remains an implementation verification
obligation; help output does not establish that counters or reports were captured.
The owner explicitly retired the older 6 ms/register/ray-budget/larger-map/
no-regression gates and limits. Performance measurement of the complete application
comes after the approved features are implemented. Build and verify the automation
as tooling, but do not turn intermediate milestones into optimization campaigns or
change prototype cadence to satisfy old budgets. Hardware limits and ordinary
resource correctness still apply; they are not optional performance targets.
Larger-map optimization remains excluded even though profiling capability is
required. New targets or optimization work require a subsequent owner decision.

### Maintained capture interface and readiness — coordinator specification

Implement `scripts/profile_lighting.py` with `discover`, `capture`, `analyze`, `compare`
subcommands. Capture selects `--tool systems|graphics`, explicit `--exe`, `--output`,
and `--workload frozen|live` (default frozen). Discover records executable versions,
GPU/driver/backend, architecture-specific metric sets and current CLI capabilities;
tool locations come from explicit arguments, PATH or documented installation
discovery, never the historical home-directory strings above. A capture takes a
machine-local lock so builds/tests/other captures are not silently interleaved.
Do not automatically change GPU clocks; explicitly request `unaltered` for Graphics
and record the actual policy/clocks in every manifest.

Add permanent `app.game --profile-capture=systems|graphics --profile-output=DIR
--profile-workload=frozen|live` integration to the real interactive application,
mutually exclusive with the display exercise and other command selectors. It
loads the normal Lighting Lab, default overview camera, map1024/seed42, P1,
1280x720 logical window, manual EV0, bloom on at default values, diagnostics off,
sky12, uncapped pacing and VSync off. These explicit profiling settings are
isolated from user persistence and listed in the manifest, not new user defaults.

Frozen mode pauses through the production action after initial world admission,
before the first simulation tick; live mode continues normal simulation. Both
continue normal world-space lighting and presentation. After at least120 accepted
fresh scene frames and at least2 seconds of active presentation, atomically write
`ready.json` containing PID, world/publication identity, current tick, accepted
lighting/screen frame indices, actual camera/extents and settings. This is capture
warm-up, not a claim that lighting converged. Failure to reach readiness in180
seconds is an incomplete capture with retained logs.

Systems uses its interactive named-session commands: launch injected, wait for
the application's ready record, start collection, collect2 seconds, stop and
export. The game continues ordinary rendering while waiting. The supervisor then
writes a session-specific `stop.request`; the main application turns it into its
ordinary quit action. Check that request's session token so a stale file cannot
stop another run. This tiny local-file rendezvous is confined to the maintained
profiling command; it is not a generic remote-control service.

Graphics uses the installed Nsight Graphics SDK's trace control after readiness.
The discovered SDK is0.9.2 under `SDKs/NsightGraphicsSDK/0.9.2`, with Apache-2.0
headers. Integrate its official initialization/status and backend-specific
`NGFX_GPUTrace_StartTrace_Vulkan`/`_D3D12` calls in a small profiling-only adapter
on the device-owner lane. Use the existing RHI native-object access seam; do not
expose SDK types in engine/simulation contracts. SDK initialization/injection
absence fails this requested capture explicitly. Normal gameplay never initializes
or calls the SDK. Its optional build prerequisite is discovered/configured by
the profiling preset, with preserved attribution; do not hand-copy ABI guesses.
Use nonblocking status/file-count polling and actual finalized trace-file paths;
never block the render loop waiting for a capture file. SDK-call acceptance is
not proof of a finalized, useful report.

### Concrete CLI templates — verified option discovery, execution still required

Construct argv arrays with the process API (the templates below illustrate values,
not shell-concatenation code). On this Linux/Vulkan host, installed help confirms:

```text
nsys launch --session-new=SESSION --trace=vulkan,osrt
  --vulkan-gpu-workload=individual EXE APP_ARGS
nsys start --session=SESSION --sample=process-tree --backtrace=fp
  --output=OUT/systems
nsys stop --session=SESSION
nsys export --type=sqlite --output=OUT/systems.sqlite OUT/systems.nsys-rep

ngfx --activity="GPU Trace Profiler" --exe=EXE --dir=WORKDIR --args=APP_ARGS
  --start-with-ngfx-sdk --limit-to-frames=8 --max-duration-ms=2000
  --set-gpu-clocks=unaltered --auto-export --output-dir=OUT/graphics
  --architecture=ARCH --metric-set-name=SET
```

The Systems wrapper places start/stop around the bounded post-readiness interval;
it owns a unique named session and never stops all profiler sessions. Preserve
native `.nsys-rep` and SQLite output. Enable GPU metrics on a later question-driven
capture via `nsys start --gpu-metrics-devices=ID --gpu-metrics-set=ALIAS`, where
ID/ALIAS come from that tool's help discovery, and record sampling frequency.
CPU frame-pointer attribution requires the repository's profiling build; stripped
or missing symbols are reported as unavailable attribution, not zero CPU cost.

For Graphics, choose the discovered architecture's `Top-Level Triage` set where
advertised (2026.4 lists it for Ada, Ampere GA10x, Blackwell and others); Turing's
advertised initial set is `Throughput Metrics`. Record the exact chosen set rather
than assuming IDs are portable. Single-pass is default. `--multi-pass-metrics`
or `--real-time-shader-profiler` is a separate explicitly requested collection.
Multi-pass requires a validated replay or demonstrated repeatable state; pausing
simulation alone does not freeze CH history/phase, so it is insufficient evidence
for arbitrary cross-frame counter equivalence. If valid replay cannot be acquired,
report multi-pass unavailable and retain single-pass data; do not fabricate it.

The eight-frame limit is additionally bounded by2 seconds; if fewer complete
frames are captured, report their actual count. Require at least one complete
fresh A/B/C frame and all enabled pass ranges before calling a capture useful.
Startup/loading-only traces fail that check. Windows/D3D12 runs use the installed
tool's declared D3D12 trace options and matching SDK adapter; Linux help is not
evidence those commands passed on Windows.

Capture/export deadlines:180 seconds to application readiness,2 seconds intended
collection,120 seconds for export/finalization and480 seconds total. On timeout,
request graceful application shutdown, stop only the owned profiler session, and
retain logs/native partial output; terminate only owned processes after the
bounded grace period. A failed export never overwrites a previous successful
artifact directory. Exit/status classes match the display exercise, with failure
cause and cleanup outcome preserved separately.

### Artifacts, parser and analysis contract

Every capture directory contains `manifest.json`, application stdout/stderr,
profiler command argv and stdout/stderr, readiness/final-state records, native
reports, untouched raw exports, `analysis.json` and human-readable `analysis.yaml`.
Use schema_version1 for the maintained analysis, independent of vendor formats:

```text
identity: source commit + dirty-diff hash, binary/shader hashes, build preset,
          tool versions, OS/backend/GPU/driver, map/seed/checkpoint,
          camera, settings, observed W/R/S/B, simulation/lighting frame range
collection: tool, mode, clocks, metric set, replay identity, interval,
            overhead caveats, requested and observed complete frames
ranges[]: id, parent_id, stable_name, frame_id, queue, start_ns, duration_ns,
          inclusive/exclusive provenance, shader/pipeline identity if available
metrics[]: range_id, vendor_id, name, unit, numeric value or null,
           availability/reason, sample aggregation, raw artifact + location
validation: required ranges present, completeness, missing counters,
            numeric/parser failures, shutdown outcome, overall status
```

Retain vendor rule names/explanations/suggestions as attributed annotations, like
the current `analysis.yaml` precedent, not mandatory optimization instructions.
Normalized metrics cover GPU duration, engine activity, SM/warp occupancy, memory
traffic/throughput and cache hit rates where the selected set provides them;
shader register/local-memory attribution is reported only when actually exported.
Unsupported/not-collected is null with a reason, never zero. Preserve metric units
and distinguish peak percentage, frame percentage, count and rate. The current
precedent uses decimal commas: parse using an explicit recorded export locale;
reject ambiguous number formats rather than stripping punctuation speculatively.

`analyze` is offline/read-only and repeatable. Its vendor adapter validates the
actual exported schema/version and required column keys; unknown schemas fail
with an actionable report. The first implementation capture supplies retained
minimal real fixtures for parser tests. Do not invent a vendor YAML command or
assume Graphics auto-export's file names/format from help. Discover finalized
artifacts, retain them, then pin the tested adapter to the observed version.
SQLite queries use observed schema/IDs to join API submits, GPU work and labels;
CPU API-call duration is never mislabeled as GPU execution duration.

Use stable GPU marker names: `WorldLighting.ResolveCells`, `WorldLighting.Visibility`,
`WorldLighting.Jump`, `WorldLighting.Source`, `WorldLighting.Cascade.L5` through `.L1`,
`WorldLighting.FineCH`, `Scene.Materials`,
`Scene.Gas`, `Hable.Resolve`, `Post.Exposure`, `Post.Bloom`, `Post.AgX`,
`Present.Scene`, `Present.UI`; preserve existing `PBMPM.Tick` and meaningful CPU
submission/wait scopes. Mark nested ranges and do not sum parent+children as
independent work. Zero particles/disabled bloom may explain an absent optional
range; omitted required lighting passes cannot.

`compare` refuses an unlabeled like-for-like conclusion if source/settings,
camera, workload, backend, clocks or collection mode differ; it lists differences
and permits a descriptive comparison. Summaries report observed distributions and
sample counts, with no inherited pass/fail timing, ray or register ceilings.
Automation is accepted only after a real Systems capture/export/parse and Graphics
counter capture/export/parse, plus an offline parser replay producing identical
normalized numeric results. This session has only discovered commands/SDK and
specified those checks; it has not run them.

## 9. Review status and implementation evidence obligations

The [initial independent builder review](2026-11-10-oni-port-builder-review.md)
identified source-level ambiguities that must be resolved before finalizing
storage/feedback ownership. In particular: the active fine-probe codec drops
negative harmonic coefficients; fine connector hit shading uses raw cascade zero
although its dispatch is disabled in the active supersampling path; and direction
lookup uses current global frame even for retained probe samples. Their visual
impact has not been measured. The owner subsequently replaced raw-L0 connector
feedback with a cached source prepared from prior CH, then approved in-place CH
updates once source preparation has finished its reads.
The owner also approved the signed CH format and derived retained-direction
identity above. The coordinator now specifies angular identity, CH fitting/source
normalization, temperature publication and winner reduction. Codec/backend
verification remains required. These findings do not establish the cause of the current
engine's failures.

Coordinator specification is complete and has received a fresh independent
[builder check](2026-11-10-oni-port-complete-builder-review.md), whose result was
"nothing important left". The owner subsequently approved the complete document.
This is **not** implementation authorization, proof of feasibility on every
backend, or visual acceptance. In particular:

- The owner approved 6500 K at engine luminance one. The CIE/Planck integration,
  domain, gamut mapping, interpolation and temperature publication are specified,
  with initial numerical evidence.
- Complete-design approval includes the coordinator technical specifications:
  visible BRDF, codec/range/error handling and explicit terminal-sky corrections.
  Implementation qualification does not reopen these choices automatically.
- Lighting Lab coordinates/materials/particle seeds, post-processing internals,
  diagnostic domains, display-exercise cases and profiler commands/artifact contracts
  are explicit coordinator specifications above. Their authored values and
  operational choices are covered by the owner's full-document approval.
- Cleanup inventory covers code families, callers/settings, shaders/registration,
  tests, archive location and the owner-acceptance boundary. The future checkpoint
  manifest must expand that inventory against the actual implementation workspace.

Required implementation evidence, not permission to invent a replacement design:

- Exact resize failure and Vulkan diagnostic from a real failing run.
  `VK_KHR_maintenance4` is a standard Vulkan feature collection promoted to 1.3,
  not an engine repair flag. Its name alone does not diagnose the reported error.
- Current source shows AA-dependent bloom/exposure validation and extent checks
  in `RetroPass::validateFrameSettings`, plus retained-picture presentation checks.
  This identifies coupling to inspect, not proof of the resize crash's root cause.
- Recoverable checkpoint/expanded salvage inventory, and actual cleanup acceptance.
- Verify the specified Oni-derived constants, dispatch order and shared layouts.
- Verify the specified GPU particle publication/reuse boundary and R8 storage capability.
- Verified automated Nsight acquisition, real vendor-export parser fixtures,
  analysis export and matched comparison. Installed-help/SDK inspection is the
  only profiler verification performed in this design session.

Backend qualification occurs early after cleanup acceptance: R8 stores/loads,
32-bit atomic reduction, reflection/packing, RGBA16F four-sample load/index pattern,
sample-rate material shading, resource transitions and shared-queue publication.
An unsupported requirement or unexplained compiler/device blocker is returned to
the planning owner with the exact source/toolchain/log evidence. Do not silently
widen layouts, replace Hable, add an alternative tracer or weaken the gates.

Final readiness also requires the ordinary app's success/failure/teardown checks,
focused CPU/GPU contract tests, permanent display exercise and user visual
acceptance of the lab and normal gameplay. Linux evidence does not certify Windows;
list each unrun host/backend explicitly. The final performance report measures the
complete integrated pipeline and carries no retired thresholds. No production
implementation, build, GPU run or new visual acceptance occurred while drafting
these specifications.

Owner addition (2026-11-11): final Task 19 acceptance requires full clean builds
of both **`clang-dev` and `clang-rel` with zero compilation warnings or errors**,
with complete configure/build stdout and stderr retained and inspected (including
module/link diagnostics). It also requires inspection of complete execution logs
from ordinary gameplay/Lab, display exercise and profiling, through shutdown:
**no warnings, errors or message flooding**. Audit repetition at every enabled
severity using message-template counts/rates and representative lines, including
steady-state intervals; do not hide findings through output filtering or disabled
diagnostics. Deliberate fault-test diagnostics are separately identified evidence,
not an exemption for normal runs. Unresolved findings or unavailable required
checks leave this acceptance gate unmet. These checks have not run in planning.

## 10. Design completion checklist

- [x] Agree on Oni reference family and cleanup/replacement approach.
- [x] Select order-four CH and approve 8-byte coarse interval / 32-byte signed CH
  records, decoded FP32 blending and row-major fine-probe addressing.
- [x] Approve varying within-sector angular samples, fixed four-frame probe
  scheduling and derived last-update direction identity without per-probe metadata.
- [x] Approve black/zero-transmittance coarse initialization, zero previous CH,
  direct first fine-field commit and ordinary-cadence dark-to-lit startup.
- [x] Approve reference exponential CH accumulation at rate 2 per second of
  active presentation time, independent of simulation pause and excluding suspension.
- [x] Complete RC/HRC source survey and approve one RGB9E5 isotropic source per
  cell, diffuse-only feedback and preserved on-screen PBR; remove per-hit BRDF work.
- [x] Approve visible-band black-body thermal emission via a temperature-to-HDR-RGB
  lookup table, replacing prototype T^4/T^6 intensity curves for this port.
- [x] Limit thermal emission to existing opaque transport surfaces; heating does
  not promote transparent materials or gas to opaque/emitting transport classes.
- [x] Approve four-byte resolved cells: material ID16 + FP16 Kelvin, with shared
  thermal lookup for source preparation and scene emission and no thermal image.
- [x] Approve eight-byte source-material palette: RGB9E5 authored emission plus
  RGB9E5 diffuse reflectance, derived from linear tint, texture mean and metallic.
- [x] Approve gas as a separately rendered alpha-blended, CH-mean-lit particle
  overlay before temporal resolve, with no transport blocking or emission.
- [x] Approve always-on Hable temporal MSAA in the completed port; pixelization
  controls rendering resolution, with no AA selector or disable mode.
- [x] Approve retained display inventory and removal of obsolete master switches,
  palette/AA modes and lighting ray-budget/cascade-count controls.
- [x] Draft exact angular sequence, group hash, CH fitting and source normalization
  as coordinator technical choices for complete-design approval.
- [x] Approve visible CH diffuse/specular filtering, including the smooth
  out-of-plane specular limit and its low-order highlight approximation.
- [x] Specify the final PBR material response and isotropic BRDF integration table.
- [x] Specify CIE/Planck thermal LUT generation, sampling and gamut mapping; perform
  source-data checksum and initial binary64 integration/interpolation checks.
- [x] Obtain owner acceptance of 6500 K at engine luminance one.
- [x] Specify codec rounding/ranges, material admission and asynchronous numeric
  status/error settlement without per-frame particle/image readback.
- [x] Specify coarse atlas addressing, intervals, dispatches, terminal sky,
  visibility construction and shared jump-DDA boundary semantics.
- [x] Specify packed metadata32 temperature publication and two-pass uint32 winner
  reduction, preserving solver layouts and stable-ID ordering.
- [x] Specify queue sharing, per-stage submission settlement and ambiguous-submit
  failure handling against existing renderer/matter contracts.
- [x] Specify screen attachment formats, padded camera mapping, sample-rate cell
  shading and the temporal Hable lattice/history kernel.
- [x] Approve single in-place CH after source preparation, own-probe-only EMA,
  ordered source/CH/scene accesses and submit-based history/phase/timing commit.
- [x] Agree on cleanup-first owner acceptance milestone.
- [x] Agree on continuous lighting and full acceleration rebuild.
- [x] Agree on particle transport scope and simple grid presentation.
- [x] Approve categorical particle projection and material/terrain overlap rules.
- [x] Record display automation, profiling and no-compatibility requirements.
- [x] Agree on palette removal, pixelization retention and PostProcessPass split.
- [x] Approve pixelization at resolved scene resolution, post-processing there,
  nearest-neighbor enlargement and native-resolution UI.
- [x] Approve arbitrary positive extents using R=ceil(W/P), S=ceil(R/2), B=2*S,
  preserved visible framing, valid-content-only processing and fixed-P crop.
- [x] Agree on synthetic-map cases, paired door/transparent rooms and explicit
  two-cell versus one-cell enclosure coverage.
- [x] Approve Hable reference temporal weighting and camera reprojection,
  screen-history reset/commit rules and independence from world-space lighting.
- [x] Retire inherited performance gates/limits; measure the completed integrated
  implementation before considering any new targets or optimization campaign.
- [x] Specify cleanup/archive boundary, exact Lighting Lab recipe, post-effect
  internals, diagnostic domains and permanent display-exercise cases.
- [x] Specify bounded Nsight readiness/commands, SDK integration, artifacts,
  normalization/parser requirements and actual-capture verification obligations.
- [x] Complete coordinator draft for independent consistency review.
- [x] Perform initial independent builder gap review; record findings for owner
  decisions without changing the chosen algorithms.
- [x] Perform independent builder/consistency review of the completed design;
  fresh reviewer reported "nothing important left" (record linked in section 9).
- [x] Obtain owner approval of the complete design document.
- [x] Record the owner's 2026-11-11 correction requiring responsibility-based
  naming and no reference-project mentions in active code; update the plan,
  profiler interface/markers and fresh-session handoff accordingly.
- [x] Write the [precise implementation plan](../plans/2026-11-10-oni-port.md)
  and [fresh-session prompt](../plans/2026-11-10-oni-port-handoff.md).
  The owner selected subagent-driven execution. Explicit plan-review confirmation
  remains pending; implementation and both application acceptance gates have not
  started.

Future handoff rules: no legacy-plan authority, no visual self-approval, no
unapproved fallback architecture, no placeholders presented as complete. Escalate
unexpected feasibility blockers instead of accumulating workarounds. Each plan
step must name its edit boundary, reusable facilities, removal obligations,
verification and owner acceptance point.
