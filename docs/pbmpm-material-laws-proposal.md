# PB-MPM material laws — B1 increment 2

Status: **authored, unrun**. No build, shader execution, recovery verdict or GREEN
claim accompanies this edit-only increment. Coordinator decisions R1–R5 below
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

The leaf imposes 1% simple shear on a 4x4 patch for 120 dt=1/480 steps (0.25 s),
then releases it for 960 steps (2 s), at K5. It reports the first step with
maximum particle `||F^T F-I|| < 1e-2` and the final residual. F^T F removes rigid
rotation; translation is absent from F. Negative recovery reports
`not_established` without failing the leaf; operational GPU errors still fail.
The diagnostic never changes the catalog. Rock/ore stay granular pending review
of executed evidence. **Verdict unrun.** Coordinator-accepted risk: this local
metric does not establish bonded geometry, conservation or a physical elastic law.

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
  dense cap removal and invalid zero singular value.
* `calibrated_admission_and_clay_sand_divergence`: coefficient domains, rejected
  admission preserving epoch, production clay/sand response divergence.
* `elastic_one_percent_shear_release_diagnostic`: R4 report-only verdict.
* Existing reflection/pack, scalar rejection, catalog mapping, exact checkpoint
  roundtrip and v6/v7/v8 migration leaves updated for expanded state/rows.
* Renderer/lighting fixtures follow the ABI. Frozen upstream fixtures remain
  the old-family arithmetic gate; no fixture was regenerated.

All are **authored coverage**, not execution evidence. Linux and Windows/D3D12
builds, shader reflection, tests, integrated behavior and performance remain with
the coordinator's validation lane.
