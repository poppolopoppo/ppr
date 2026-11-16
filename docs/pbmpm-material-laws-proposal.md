# PB-MPM material laws — B1 increment 2

Status: **review revision authored, unrun**. The coordinator reports prior gates
at commit `a98f399d`; that evidence predates this revision and does not validate
its constraint-target coupling. Exact prior command lines and the external
ledger are not present in this checkout and must be supplied by the validation
owner before the evidence reconciliation is complete. No GREEN claim accompanies
this edit-only revision. New-law acceptance is **authored-analytical**, with no
upstream oracle. Coordinator decisions R1–R5 below
supersede the earlier stress-space proposals. [Phase A evidence](pbmpm-baseline-and-multilayer-results.md)
and the [sand investigation](pbmpm-sand-history-derivation.md) remain historical evidence.

## R1 — calibration space and units

`e = log(sigma)`, `t = sum(e) + H`, `dev = e - (t/2)*1`, `n = length(dev)`.
These dimensionless calibration-space quantities are **not stresses**. Reference
density remains inventory metadata (`rho/1000 kg/m^3`), not an additional factor
in the normalized liquid law. Existing mass and volume are authoritative.

New families initialize `H=logJp=0` and retain zero: the legacy sand expansion
reservoir is not their compaction history. Admission requires this invariant;
it makes dev trace-free and permits the constant-t radial return. Legacy sand
retains its existing H arithmetic. Migration retains saved families/H rather
than reinterpreting a sand reservoir as cohesive or porous state.

Coordinator-accepted risk: no physical modulus, stress, energy or timestep-
independent strength interpretation. The source alpha conversion is retained:
`alpha = sqrt(2/3)*2*sin(angle)/(3-sin(angle))`, angle in degrees.

## R2 — cohesive clay and wet sand

Admissible set: `-D <= t <= T`, `n <= alpha*max(-t,0)+c`. State C starts at zero.

1. Evaluate `D = D0 + h_D*C_old`; clamp trial t to `[-D,T]` at constant dev.
2. Update `C = max(C_old, -min(t_returned,0))`.
3. Evaluate `c = c0 + h_c*C`; radially clamp dev to the shear edge at constant t.
4. Reconstruct `sigma = exp(dev_returned + t_returned/2)`.

Shear projection is **non-associated**. Volumetric clamps are non-expansive
metric projections onto a convex interval at fixed history; an active
deviatoric projection strictly reduces n; hardening is monotone.
**No dissipation claim.** A dissipation proof is out of scope because R1 defines
no stress-space mapping. No work-sign inequality is asserted.

Coordinator-accepted risk: sequential empirical returns are not an associated
coupled stress return. Hardening enlarges the next cap; returned t remains
admissible. Saturation, moisture transport and fracture are outside this increment.

### C1 — constraint-target coupling

For cohesive and compacting only, `constraint()` decomposes the trial
`(I+D)*F_stored`, projects its log-strain onto the admissible set with history
and history-dependent strength frozen, and reconstructs
`U*diag(exp(e_projected))*Vt`. The existing determinant-normalized Q blend and
difference/relaxation are retained. This is a calibration-space constraint target,
not a stress derivation. Integration alone updates C/phi. Sand, elastic, visco,
liquid and the no-adapter path retain their existing target arithmetic.

## R3 — compacting snow, two explicit regimes

For `phi < 1-epsilon_D` and `t <= 0`:
`(n/(alpha*max(-t,0)+c))^2 + ((-t)/D)^2 <= 1`,
`c=c0*phi^kappa`, `D=D0/(1-phi)`.
For positive t use the cohesive edge c and tension cutoff T. A zero edge means
n=0; the implementation never divides by that edge.

For `phi >= 1-epsilon_D`, use **cone only** with
`c=c0*(1-epsilon_D)^kappa` and cutoff T. This is an explicit branch, not the
limit of the regularized ellipse. Epsilon is a **numerical regime boundary**,
not a physical material parameter.

Clamp t to the old cap and T first. Only removed over-compression updates phi:
`delta_phi = beta*max(0,t_returned-t_trial)`,
`phi_new = clamp(phi_old+delta_phi,phi0,1-epsilon_D)`.
Re-evaluate regime/strength/cap using phi_new, then radially project dev to
`(alpha*max(-t,0)+c)*sqrt(max(0,1-(-t/D)^2))` in the compressive ellipse,
or the cone edge in the dense branch. Phi never decreases and is unchanged on
release. A cap clamp that reaches the boundary selects the dense branch in the
same return. C is carried unchanged for snow.

**Observable edge continuity** means continuity at zero confinement (`t=0`):
both edges share `n=c0*(1-epsilon_D)^kappa`; one-sided phi values produce
converging returned n there within FP32 tolerance. At fixed negative t, removing
the ellipse factor produces a finite edge jump. There is no global yield-surface
continuity or limit argument. Coordinator-accepted risk: deliberate cap removal
and a finite-compression strength jump at the numerical boundary.

## Coefficient ranges and element mapping

`GpuMaterialProperties`: 19 scalar words, 76 bytes, alignment 4. The first ten
retain B1-1 layout/semantics. All added coefficients must be finite:

| Field | Domain | Default |
|---|---|---:|
| c0, T | nonnegative | .05, .02 |
| D0 | positive, finite reciprocal | .2 |
| h_c, h_D | nonnegative | .2, .5 |
| phi0 | `0 < phi0 <= 1-epsilon_D` | .3 |
| beta, kappa | nonnegative | .5, 2 |
| epsilon_D | `(0,1)`, FP32 `1-epsilon_D < 1` | .01 |

Friction remains `[0,90)` degrees. Existing blend/relaxation/viscosity and sand
yield ratio remain finite-only legacy coefficients. Plasticity must produce a
finite positive legacy exponential; reference density is positive with a finite
reciprocal. Invalid rows, incompatible state, unknown families and bad indices
reject before publication. New families require a property table, not UINT_MAX.
Derived denominators, norms and exponents are guarded before unsafe operations.
The new return bypasses the source singular-value clamp; exponents outside
`[-87,88]` reject the working transaction.

Clay/wet sand map to cohesive (wet sand c0=.02); snow maps to compacting.
Pebbles/gravel stay granular with independent friction 38 degrees and
sandYieldRatio 1.25. Rock/ore remain granular. Only existing catalog IDs get rows.
These are authored calibration defaults, not measured material properties.

## R4 — elastic diagnostic verdict path

M1 ruling: initialize an undeformed 4-by-4 patch with affine velocity
`vx=.01*(y-ycentre)/.25 s`, `vy=0`, including its APIC contribution. Solver
velocities are displacements per dt: upload `.01*(y-ycentre)/120` and D01
`.01/120`. Advance 120 dt=1/480 steps (0.25 s), K5, zero gravity, no contacts.
At release subtract only the prescribed velocity/D contribution once; re-upload
all other captured state unchanged and advance 960 steps (2 s). No F/position
reset or pin/weld seam is used.

Every substep reports maximum particle Frobenius `||F^T F-I||` and best-fit
proper-rotation/translation-aligned positional RMS divided by initial RMS radius.
Alignment uses atan2(cross covariance, dot covariance), not an affine fit.
Both metrics must remain strictly `<1e-3` throughout release steps 721–960
(final 0.5 s), including the endpoint. The summary reports drive peaks, final
values and final-window maxima. Both drive peaks must exceed the recovery
thresholds or recovery is not established. Nominal 1% shear norms are ~1.41e-2
and ~5e-3, ~14 and ~5 times these thresholds; achieved excitation is measured,
not assumed to equal nominal free transport. The recovery outcome is report-only,
including insufficient excitation. Rock/ore remain granular. **Authored, unrun:
no measured values or corrected recovery verdict are available.**

### S1 — loaded production-law scenes

`calibrated_loaded_clay_and_snow_diagnostic` drives the real K5 solver with default
production cohesive/compacting coefficients, identity F, zero initial velocity,
and a 4-by-12 unit-spaced column on a static box floor at y=12. Gravity is
9.81 m/s² at h=1/3 m on the 64-cell grid. Clay and the granular control differ
only in family, preserving coefficients, mass, support and geometry. After 1 s,
report tagged top-minus-bottom layer height, horizontal spread, top-layer height
above the floor, and clay-minus-control differences. Greater height and less
spread report greater column retention; otherwise not established.

Snow advances 1 s under gravity, 1 s release at zero gravity with the floor
retained, then 1 s reload under the same gravity. Gravity is immutable in the
public profile, so phase boundaries restart the solver, transferring and exactly
byte-comparing all particle state. No F, position, velocity or history reset is
injected. Every 0.25 s reports geometry and mean phi. The summary reports initial,
compressed, released and reloaded phi, retained height loss, and first/reload
axial shortening normalized by initial layer height. Retained compaction requires
increased phi retained after release. Increased structural resistance means less
reload shortening under equal load/duration, with positive first shortening and
retained phi. This geometry-dependent compliance proxy is not a stress modulus.
Both scene outcomes are report-only. **Authored, unrun: no measured values or
successful behavior verdicts are available.**

## R5 — state, ABI and checkpoint migration

Fresh/dug particles start with `C=0`, `phi=phi0`; digging reuses saved property
rows, including custom phi0 and migrated families. C/phi persist independently
of mass, density and H. Semantic record96, hot device row72, cold row28 bytes;
offsets and natural alignment are asserted. Renderer layout5 uses scalar72.
No-adapter conformance retains hot64/cold24 and compares the original fields.

Envelope **v9**, table policy **2**: 24 particle words and 19 property words.
Canonical FNV-1a covers little-endian policy/count/full rows sorted by stable ID;
device indices never persist. v8 policy1 identity is verified against its original
ten words before defaulting the extension. v6/v7 synthesize legacy-valued rows
(v6 K2, v7 saved K). v6–v8 preserve the original 22 particle words and families,
append C=0 and phi=.3 (migration phi0), and resave v9. Unknown policy or identity
mismatch rejects. Coordinator-accepted risk: old saves retain old family behavior;
there is no silent conversion of old plastic history.

## Authored coverage — execution pending

* `calibrated_caps_hardening_and_two_regime_edges`: actual shader helper; cap
  consumption, monotone C, release history, cohesion at t=0, tension, phi
  compaction/release, snow strengthening, zero-confinement edge continuity,
  dense cap removal and invalid zero singular value; review revision adds a
  porous-to-exact-dense crossing during the return with simultaneous shear.
* `calibrated_admission_and_clay_sand_divergence`: coefficient domains, rejected
  admission preserving epoch, production clay/sand response divergence; review
  revision adds exact replacement/append/re-upload preservation of non-default
  C/phi for both new families, including exact-boundary dense phi.
* `elastic_one_percent_shear_release_diagnostic`: R4 report-only verdict.
* Existing reflection/pack, scalar rejection, catalog mapping, exact checkpoint
  roundtrip and v6/v7/v8 migration leaves updated for expanded state/rows.
* Renderer/lighting fixtures follow the ABI. Frozen upstream fixtures remain
  the old-family arithmetic gate; no fixture was regenerated.

The review additions, including M1 and loaded S1 scenes, are **authored, unrun
coverage**, not execution evidence. Prior gates identify source `a98f399d`, not
this working-tree revision. S2 is deferred: **no independent historical v8
artifact exists in the repository. Migration evidence is reconstruction-based,
shared with the CPU mirror** (`Checkpoint.Tests.Support.h`). A future independent
artifact can replace this fixture; it is not a historical compatibility oracle.

Exact reconstruction: start with the current v9 water/default checkpoint. Let L
be encoded length, S raw snapshot length, M property count, N particle count.
Table offset is `L-S-8-(16+76*M)`, particles `table-96*N`, schedule `particles-72`.
Remove the table and each particle's bytes [88,96) in reverse row order. Insert
policy1, little-endian 64-bit FNV-1a identity over policy/count/ID-sorted first
ten property words, count M, then the first ten words per stored row (40 bytes).
Keep K, the 64-byte profile and snapshot; set envelope byte 8 to version 8.
The shared helper and added GPU-consumer assertion check the section sum
`schedule_offset+4+64+4+88*N+(16+40*M)+8+S` before decoding. This water/default
fixture does not establish historical clay/snow or non-default coefficient
compatibility. No payload was invented.

Historical raw 88-byte captures
are decoded as their original 22 words, with default C/phi appended only after
the original file hash is checked. Frozen fixtures were not changed.

Linux and Windows/D3D12 builds, shader reflection, tests, integrated behavior and
performance for this revision remain with the coordinator's validation lane.
