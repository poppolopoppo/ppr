# PB-MPM material laws: B1 decisions and replacement proposals

Status: **B1 decisions approved; plumbing authored, execution pending. No new law implemented or validated**.
This document does not certify the retained sand law or Elastic recovery.

## 1. Evidence and scope

The inspected implementation is `assets/shaders/pbmpm/fork_core.slang`
(`constraint`, `integration`), `solver.slang` (`applySandVolumeImpulse`),
`lib/engine/matter/Matter.Gpu.Types.cppm`, and
`game/colony/{MatterRuntime.cpp,Checkpoint.Codec.cpp}`.

The [sand derivation](pbmpm-sand-history-derivation.md), especially sections
3–4 and 7, requires a B1 decision before freezing sand property semantics.
The [Phase A evidence](pbmpm-baseline-and-multilayer-results.md) records the
rejected sand law, ill-conditioning at K10, amplification by orders of
magnitude, and undetermined Elastic recovery. These are not calibration data
for physically meaningful coefficients.

B1 should route the existing laws through substance rows. Replacing a return,
altering history, correcting transfer precision, and changing the pinned
no-adapter arithmetic are separate work. Frozen fixtures and tolerances remain
unchanged. All tests below are proposed acceptance tests, not execution results.

## 2. B1 schema and compatibility decisions

### Approved B1 storage

Own `GpuMaterialProperties` in `engine.matter:gpu.types`. Ten naturally aligned
32-bit words (40 bytes, alignment 4):

| Field | Type / units | Proposed admission |
|---|---|---|
| stable substance id | `u32`, catalog identity | known, unique; no vacant id |
| constitutive family | `u32` enum | liquid=0, elastic=1, sand=2, visco=3 |
| elasticity ratio | FP32, dimensionless constraint blend | finite (strict A2 predicate) |
| elastic relaxation | FP32, dimensionless correction per iteration | finite (strict A2 predicate) |
| liquid relaxation | FP32, dimensionless correction per iteration | finite (strict A2 predicate) |
| liquid viscosity | FP32, dimensionless symmetric-gradient damping | finite (strict A2 predicate) |
| friction angle | FP32, degrees | finite, `0 <= angle < 90` |
| plasticity | FP32, dimensionless | finite and `exp(1-plasticity)` finite and positive |
| reference density | FP32, `rho / (1000 kg/m^3)` | finite, positive, with usable reciprocal |
| `sandYieldRatio` | FP32, dimensionless calibration, **not a modulus ratio** | finite; default equals the former elasticity ratio |

Reserve family values 4 (cohesive frictional) and 5 (compacting porous);
**reject their activation** until implementations land. Elastic remains family 1.
A reserved enum value is not a fallback law.

The final word is deliberately **not called a Lamé modulus ratio**. Current sand
uses `(elasticityRatio + 1) * alpha` while constraint uses that same value as
a blend. Splitting storage must not silently make either operation physical.
For a replacement effective-2D Hencky law, use a separately versioned physical
`lambda/mu > -1`, or `K2/mu > 0`, after the dimensional choice is approved.

The first four coefficient domains above reuse existing A2 `validProfileDomain`,
which checks finiteness rather than positivity or stability. Admission is strict:
invalid rows are rejected, never clamped. Inventing a blend-in-[0,1] or nonnegative
damping restriction would exceed the approved A2 domain. Finite
coefficients do not guarantee stable dynamics; existing derived-operand,
produced-state and scatter guards remain necessary.

Keep a `u32` row index in each particle's immutable device data, separate from
the stable catalog id in physical metadata. Do not repurpose the existing float
family word as an identity or overload color bits. The device Cold ABI is now
28 bytes, including `m_property_row` at offset 24; renderer consumers and accounting
follow that stride. The 64-byte hot and 88-byte semantic records stay unchanged.
The game reconstructs indices from physical records' stable catalog IDs and the
owned property table at preparation. Snapshots do not persist ephemeral indices.
`UINT_MAX` is the explicit profile-only engine fixture path. The no-adapter
conformance shader retains its original 24-byte cold layout and global arithmetic.

### Units and density: a decision, not an extra pressure factor

With cell width `h` metres and thickness `b` metres:

* length unit: `h`; volume unit: `h*h*b`;
* mass unit: `1000*h*h*b` kg;
* velocity/displacement conversion remains the existing timestep convention;
* fixed-point multiplier is the dimensionless encoding factor `10^7`, not a
  modulus, density, or precision correction;
* an eventual stress unit needs an explicit time scale `t0`: force unit is
  `mass_unit*h/t0^2` and cross-section unit is `h*b`, so pressure unit is
  `mass_unit/(b*t0^2) = rho_unit*h^2/t0^2`. With `t0 = 1 s`, this is
  `1000*h*h` Pa when h is expressed in metres. It must not be inferred from
  the fixed-point multiplier.

`liquidDensity` is advanced multiplicatively by `1 + trace(D)` and constraint
uses its reciprocal. It must not be relabeled SI density or multiplied by the
new reference density without a separate derivation. B1 recommendation:
reference density describes reference inventory (`mass = rho0 * volume`),
while the current normalized liquid state and pressure arithmetic stay intact.
Physical mass and volume remain authoritative for loaded particles; changing a
catalog definition cannot rewrite either silently. Density changes therefore
affect initialization/inventory, not a second independent pressure coefficient.

**Approved B1-1:** retain the legacy sand calibration as `sandYieldRatio`.
**Approved B1-2:** reference density comes from catalog physical inventory;
saved mass/volume and normalized liquid arithmetic remain authoritative.
**Approved B1-3:** strict A2 domain predicates, with legacy values as defaults.

### Ownership, identity, mapping and migration

Build and validate an owned table in the game catalog adapter, never by resolving
game catalog services inside `engine.matter`. Reject unknown and duplicate ids,
unsupported families and invalid coefficients before GPU allocation/publication.
The engine validates row indices and numerical domains; the game validates ids
against its catalog. Publish table and particle indices in the existing prepared
replacement transaction. Retain the immutable buffer through all tick fences;
append operations must preserve existing indices or replace the whole mapping
transactionally. Active family selection comes from active particles, never
from the set of definitions present in the table.

**Approved B1-4:** envelope v8 appends policy u32 (currently 1), canonical identity
u64, count u32, then full ten-word rows after numerical particles and before the
raw sim snapshot. Rows retain their storage order; the identity is FNV-1a over
little-endian policy/count/row words sorted by stable ID. Stable IDs, not indices,
persist. Unknown policy or identity/value mismatch returns `protocol_not_supported`;
unknown/duplicate IDs, invalid coefficients or incompatible active family return
`invalid_argument`, before live publication. Existing catalog fingerprint and
id/key compatibility checks still apply. This fingerprint is an identity check,
not an authentication mechanism.

v6/v7 migration builds current legacy-valued rows for saved active substances,
using catalog density without rewriting inventory or any solver-state word.
v6 keeps K2; v7 keeps saved K. Resaving writes v8. Family/history conversion is
not a B1 operation. Unused definitions are admitted but do not select dispatches.

Proposed catalog policy (values need approval/calibration, not physical claims):

| Elements | B1 family | Coefficient source / intended distinction |
|---|---|---|
| water | liquid | catalog density; baseline damping |
| brine | liquid | catalog density; legacy damping default, independently editable row |
| crude oil / oil | liquid | catalog density; legacy damping default, independently editable row |
| magma | liquid | catalog density; legacy damping default, independently editable row |
| sand | sand | legacy coefficients |
| sandstone, basalt, coal | sand | individual legacy-valued rows, explicitly provisional |
| copper/iron/uranium ores, wolframite, autunite, azurite | sand | individual legacy-valued rows |
| clay / wet sand | sand placeholder | target family 4 after approval |
| snow | sand placeholder | target family 5 after approval |
| pebbles / gravel | sand placeholder | size-dependent solid decision below |
| known material without override | liquid iff liquid phase, otherwise sand | exact former profile |

Only existing catalog ids may receive rows; placeholders must not invent numeric
ids for elements absent from a loaded catalog. Gas remains outside this solver.
Do not make all geological material Elastic until recovery has been established.

No new-world viscosity/friction tuning was approved: every default keeps the old
profile values (blend 1, elastic relaxation 1.5, liquid relaxation 1, viscosity
0.01, friction 30 degrees, plasticity 0, sand calibration 1). The table in
`MatterRuntime.cpp::catalogFamily` names existing catalog keys and aliases; it
does not invent IDs for absent definitions. The public candidate setter admits
independently authored rows without changing live state on failure.

## 3. Common conventions for replacement laws

Recommendation: initially use effective 2D, tension-positive logarithmic elastic
strain `e`, Kirchhoff stress `tau`, and compression-positive
`p = -tr(tau)/2`. Define `s = dev2(tau)` and `q = ||s||`; this is **not** von
Mises `sqrt(3 J2)`. The energy is

`W = mu * ||dev2(e)||^2 + K2/2 * tr(e)^2`,
`tau = 2*mu*dev2(e) + K2*tr(e)*I`, with `mu > 0`, `K2 > 0`.

Use positive-orientation `F = Fe Fp`. Rotation and stretches reconstruct `Fe`;
`logJp` alone cannot represent arbitrary anisotropic plastic history. Store
the plastic tensor or a demonstrated sufficient reduced state. Stress quantities
below have Pa units (Kirchhoff stress per reference volume); convert explicitly
at the solver boundary. Specifying these laws does not supply a PB-MPM stiffness
mapping: the current target blend is not a physical elastic constitutive solver.

**Owner L-1:** effective 2D versus true 3D plane strain, including out-of-plane
plastic closure. Recommended effective 2D is not a plane-strain claim.
**Owner L-2:** physical stiffness/energy-to-PB-MPM correction mapping, timestep/K
dependence, and whether legacy clamps/volume impulses are removed for new families.
No return-map-only test can settle whole-step mechanical energy.

## 4. Cohesive frictional: clay and wet sand

### State and yield

State: `Fe`, plastic state `Fp`, signed plastic volumetric strain,
maximum compaction `kappa_c >= 0`; constant wetness for this increment.
Wetness transport, drying, damage and fracture are future features.

Next increment: use the two DP/tension yield surfaces below **plus a volumetric
compressive cap**, as approved by the plan owner. Cap parameters, its hardening
and all cap/shear/tension intersection returns still require specification before
implementation. The following two equations alone are not the approved full law:

`f_s = q - a*p - c(kappa_c) <= 0`,
`f_t = lambda_max(tau) - T(kappa_c) <= 0`.

`a >= 0` is dimensionless; `c >= 0` and `T >= 0` are stresses. Proposed bounded
hardening: `c = c0 + Hc*min(kappa_c,kappa_sat)` and similarly
`T = T0 + HT*min(kappa_c,kappa_sat)` with nonnegative slopes.
Use an associated multisurface return initially:

`tau_new = tau_trial - C : sum_i(delta_gamma_i * grad_tau(f_i))`,
`delta_gamma_i >= 0`, `f_i <= 0`, `delta_gamma_i*f_i = 0`.

Update plastic strain and compaction consistently inside that solve, not after
returning to an obsolete surface. At `q=0` use the norm subgradient/apex solve;
at repeated principal stresses use the spectral subgradient. Do not divide by
zero or select an arbitrary principal direction. Enumerate shear, tensile and
intersection active sets; reject nonconvergence transactionally.

### Parameters and dissipation

Parameters: `mu,K2 > 0` Pa; reference density `rho0 > 0` kg/m³;
friction angle in `[0,90)` degrees with approved conversion to `a`;
`c0,T0 >= 0` Pa; `Hc,HT >= 0` Pa per unit compaction;
`kappa_sat >= 0` dimensionless. All finite. For the section-3 convention,
`a=2*alpha`; choose either `alpha=sin(phi)/sqrt(2)` or the legacy 3D matching
formula, explicitly. Cohesion here is the yield intercept, not automatically
the Mohr–Coulomb cohesion in a material handbook.

At fixed hardening, associated flow gives shear plastic work
`delta_gamma_s*(q-a*p) = delta_gamma_s*c >= 0`; tensile flow gives
`delta_gamma_t*T >= 0`. These are endpoint work statements for the chosen
stress/log-strain convention. If hardening stores energy, subtract its increment
and prove the remaining dissipation; otherwise explicitly model hardening as
dissipative evolution without a separate stored-energy reservoir.

**Important limitation:** associated DP shear dilates, while hydrostatic
compression stays elastic. It cannot generate virgin plastic compaction and
therefore cannot by itself supply the requested compaction hardening. A
compressive cap or another explicitly approved compaction mechanism is needed.
Nonassociated dilation may be physically preferable but needs its own work
proof; zero dilation does not create volumetric compaction either.

**Approved C-1:** add the volumetric compressive cap; capless associated DP is
insufficient. Parameters and the coupled intersection return belong to the next
laws increment, not this plumbing increment.
**Owner C-2:** associated versus nonassociated dilation, strength evolution and
hardening energy convention; moisture dependence remains out of scope.

### Acceptance and failure modes

Analytical tests: elastic interior unchanged; pure shear returns to `q=c` at
`p=0`; uniaxial tension reaches `T`; combined loading satisfies both surfaces
and complementarity; zero-cohesion limit recovers the approved friction cone;
zero norm and repeated eigenvalues are finite and rotation invariant. Repeated
return is idempotent at fixed history. Compaction/unloading/reloading must show
the prescribed strength increase without history decay at rest. Integrate work
along loading paths and compare elastic plus hardening energy, not just endpoints.

Failures: tensile/shear intersection mistakes, capless hardening that never
activates, excessive dilation, inverted `Fe`, nonunique spectral branches,
unbounded cohesion, inappropriate SI-to-solver mapping and K-dependent strength.
The sand derivation contributes trace-free strain decomposition, cone reduction,
zero-norm handling and ledger tests; its nonnegative expansion reservoir is
**not** clay compaction history.

## 5. Compacting porous: snow

### State, cap and return

State: `Fe`, plastic state, signed compaction `kappa = -log(det Fp)`, maximum
compaction `kappa_c`, initial solid fraction `phi0`, current
`phi = phi0 * exp(kappa)`. Maintain `0 < phi <= 1`; maximum history cannot be
substituted for current volume when plastic dilation occurs.

Propose a shifted critical-state ellipse with a small tensile intercept:

`f = q^2 + M^2*(p + pt)*(p - pc) <= 0`,
`pc = pc0 * exp(H*kappa_c)`, `kappa_c_new = max(kappa_c_old,kappa_new)`.

Parameters: `mu,K2 > 0` Pa; solid density `rho_s > 0` kg/m³;
`0 < phi0 <= 1`; reference density `rho0 = phi0*rho_s`;
`M > 0` dimensionless in this document's q convention;
`pc0 > 0` Pa; `0 <= pt <= pc0` Pa; `H >= 0` dimensionless.
All finite; derived `pc`, solid fraction and stiffness must remain finite.
“Small” tensile strength requires calibration; `pt <= pc0` is only a domain.

Use the associated implicit return and complementarity from section 4 with this
`f`. In particular `d kappa = delta_gamma * df/dp` and
`df/dp = M^2*(2p+pt-pc)`. Compressive-side plastic flow increases solid fraction
and `pc`; the low-pressure side can dilate. Solve stress, compaction and hardening
together. Critical state is `df/dp=0`, at `p=(pc-pt)/2`, not automatically `pc/2`.

For a fixed cap on the yield surface:

`tau : d ep = delta_gamma * (2*q^2 + p*df/dp)`
`= delta_gamma*M^2*((pc-pt)*p + 2*pt*pc) >= 0`

for `p in [-pt,pc]` and `pc >= pt >= 0`. Evolving hardening must preserve that
domain and use an explicit hardening-energy convention. This local work result
does not prove a discrete PB-MPM energy inequality.

**Owner S-1:** approve shifted ellipse, tensile calibration and empirical
exponential hardening versus a calibrated critical-state compression law.
**Owner S-2:** closure at `phi=1`: reject compaction beyond full density or switch
to an approved dense elastic law. Do not clamp phi while discarding volume.
**Owner S-3:** stiffness dependence on solid fraction and the hardening-energy
model. A family transition needs an explicit state/energy migration.

### Acceptance and failure modes

Hydrostatic compression at `q=0` returns to `p=pc`; unloading is elastic and
reload yields at increased `pc`. Verify `phi/phi0=exp(kappa)` and conserved
particle mass; pure tension stops at `p=-pt`. Test the critical-state stationary
volume point, elastic interior, cap/tension limits, rotation, idempotence and
complementarity. Densification toward unity must exercise the selected closure.
Track full loading/unloading work and energy as well as local plastic work.

Failures: overflow in hardening, negative or super-unity fraction, artificial
mass creation, confusing bulk density with liquid solver state, spurious
hardening during unloading and false “porosity” from the old sand history.
The sand derivation supplies log-volume bookkeeping and zero-deviator handling,
but neither its expansion reservoir nor its cone is a snow cap model.

## 6. Rigid/elastic solids: rock, ores and coarse pebbles

Elastic today is a **candidate** for bonded solid behavior, not a demonstrated
recovering material. For the proposed elastic law use `Fe`, fixed reference
geometry/volume, `mu,K2,rho0` as above, `Fp=I`, and no plastic return:
`Fe_new = Fe_trial`. Stress follows the section-3 energy; its replacement solver
mapping is still decision L-2. The current SVD target, relaxation and singular
value clamps are legacy numerical operations, not this energy law.

Rigid particles are not obtained by sending an elastic coefficient to infinity.
A connected rock or a sufficiently large pebble may instead require a rigid
body with a contact/coupling boundary. Granular contacts between independent
pebbles do not become bonded simply by assigning the Elastic family.

### Measurement that settles recovery

Use a bonded patch with stable particle correspondences, apply a prescribed
displacement-controlled strain, hold, then remove the load. Repeat with identical
achieved strain (not equal brush force) at K2/K5/K10. Measure:

1. residual positions after best-fit rigid translation and rotation removal;
2. pair-distance and boundary-shape errors, volume and elastic stretch;
3. kinetic energy/velocity during the recovery window, work input and released
   elastic energy, and any damping loss;
4. clamp activations, determinant and singular-value minima throughout loading.

Compare a small-strain case with the analytical elastic response and an independent
reference integration, then test a larger strain below the proposed failure
domain. Distinguish recovery from rearrangement or continued rigid translation.
`||F^T F-I||` alone and a smaller load at higher K cannot establish recovery.
Record the settled residual and decay history, not one convenient frame.

**Owner R-1:** approve displacement amplitude, strain regime, observation horizon
and quantitative acceptance budgets before running; derive numerical floors from
the existing transfer precision evidence without widening frozen tolerances.
**Owner R-2:** bonded versus independent particle semantics, coarse-pebble size
threshold in metres, and MPM Elastic versus driver-owned rigid-body ownership.
**Owner R-3:** failure/fracture policy and whether legacy clamp activation rejects
the recovery experiment. Recommended: reject it for the small-strain proof.

Elastic energy should be conservative absent declared damping, contacts and
transfer error. Any numerical damping must have measured nonnegative loss;
unexplained energy generation fails. Large coefficients, inversion, clamp-induced
permanent strain and lack of a stress-free reference are explicit failure modes.
The sand derivation contributes dimensional/modulus warnings only; its plastic
history must not migrate into Elastic without a reference-state decision.

## 7. B1 verification contract and handoff

Authored acceptance coverage (execution belongs to the orchestrator):

* Identical-profile rows: compare all former semantic solver fields to the same
  accepted Phase A path for each existing family and K; table indices/identity
  are checked separately. Do not regenerate reference data.
* Same-family heterogeneous response: isolated particles with identical initial
  shear and different liquid viscosity; assert their measured affine shear
  response differs beyond the known
  quantization floor. Swap row order and substance ids to exclude an index bug.
* Invalid known/unknown/duplicate ids, reserved families, nonfinite/domain-invalid
  coefficients and bad indices: assert rejection and unchanged committed epoch,
  rows, table identity, inventory and ability to run a subsequent valid tick.
* v8 round trip preserves exact rows, stable ids, policy and coefficients;
  explicit v6/v7 migration preserves legacy numerical state and saved K policy.
  Unknown table versions and identity/value mismatches reject before publication.
* Add unused definitions, including other admitted families: assert identical
  active dispatch accounting and exact results. Reordering definitions must
  preserve results after stable-id remapping.
* Append/replacement/cancel/snapshot paths retain correct buffer lifetimes and
  mapping. Reflection verifies every changed ABI stride/offset on supported
  backends; the unchanged no-adapter oracle remains a mandatory gate.

No build or test result is claimed. B1 schema, bindings, catalog adapter, codec
and executable assertions are authored. Cohesive DP + cap, compacting snow,
pebble/rigid behavior and elastic recovery remain next-increment work. Clay,
snow and pebbles/gravel are still sand placeholders. No frozen fixture was
regenerated, tolerance widened, precision limit corrected, or K default changed.
