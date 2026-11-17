# Selected-fork PB-MPM compute port

The numerical authority is [ErikSom/pbmpm at
859c4490c749c1cc364d099543dbf5aa0bd9be1b](https://github.com/ErikSom/pbmpm/tree/859c4490c749c1cc364d099543dbf5aa0bd9be1b).
The original BSD-3-Clause notice is in `LICENSE.md`. Historical EA pins remain
provenance, not alternative production backends.

## B1 property ownership

Production cold rows are now 28 bytes: the six original FP32 fields followed by
one u32 property-row index. `g_properties` holds immutable 40-byte substance rows
through every tick fence. `GpuSolver::prepareReplacement`/`prepareAppend` validate
and own them; every fused root binds them. `particleProperties(row)` resolves the
index for `constraint` and `integration`. The 64-byte hot and 88-byte CPU semantic
records are unchanged. The no-adapter conformance path retains Cold24 and its
global profile arithmetic. An engine cold index of UINT_MAX explicitly selects
the global profile for Phase A fixtures.

The global profile continues to own grid dimensions, dt, gravity, volume-feedback
selection and fixed-point multiplier. Rows own elasticity blend, both relaxation
coefficients, viscosity, friction degrees, plasticity and `sandYieldRatio`.
The last is **calibration, not a modulus ratio**. Reference density uses rho/1000
from catalog physical inventory and does not rescale liquidDensity or pressure.
Legacy default values, sampling, units, multiplier, K2 source fixtures and K5
new-world policy remain unchanged. A2 predicates strictly reject invalid rows.

The game catalog maps water/brine/oil/crude oil/magma to liquid and
sand/sandstone/basalt/coal/ores to sand, with independent legacy-valued rows.
**Clay, snow and pebbles/gravel are sand placeholders until their laws exist.**
Cohesive and compacting enum slots cannot activate yet. Cohesive DP requires the
approved volumetric cap; neither that law nor snow/pebble laws land in B1.

Checkpoint envelope v8 stores policy 1, full rows keyed by stable catalog ID and
a canonical identity. v6/v7 migration synthesizes current legacy-valued defaults
without changing physical inventory or numerical state; v6 retains K2, v7 its
saved K. Unknown policy or identity mismatch returns `protocol_not_supported`
before replacing a world; invalid IDs/domains return `invalid_argument`.

The B1 GPU leaves assert exact default-row equivalence at K2/K5/K10, distinct
same-family viscosity response, untouched state after rejected admission, and
unchanged dispatch counts/results with unused definitions. The frozen fork
comparison also exercises mapped rows without regenerating fixtures or widening
tolerances. These are authored assertions, not an execution claim.

## Retained numerical profile

### Calibrated-return rejection evidence

Porous history uses bounded relaxation:
`phi_next = phi + (1-epsilon_D-phi)*(1-exp(-beta*max(0,t_returned-t_trial)))`.
Remaining compactable porosity decays exponentially under sustained plastic
loading. At fixed solid content, `V_dense/V=phi/phi_dense`, hence the porous
cap is `D_eff=ln(phi_dense/phi)`, evaluated before and after history update.
The return enforces `t>=-D_eff`; at `phi=phi_dense` it enforces `t>=0`.
Cohesive hardening uses `min(D0+HD*C, ln(phi_dense/phi))`; its phi stays fixed.
For snow, `phi0=0.3`, `phi_dense=0.99`: initial `D_eff=1.193922`, and the
fixed-width dense-height prediction is `0.3030303*initial_height`.
Porosity bounds the physics. The exponent band is only a second numerical
safeguard: a returned exponent still outside it is a defect (field 24), not
an accepted state silently repaired by saturation.
Mass and reference volume are unchanged. This is a deformation/history bound;
spatial occupied-volume convergence has its own sustained-load regression and
is not established by this bound alone. See the edit-only evidence and remaining
gate in `docs/pbmpm-baseline-and-multilayer-results.md`.

The cohesive/compacting `calibratedReturn` distinguishes arithmetic admission
from yield-surface projection. Flag 8 names the first rejected operand at both the
constraint and integration call sites. Fields 22/23 coincide with the semantic
record's C/phi words; 24–39 are derived-operand witness IDs in the existing u32
field slot, not new particle fields:

| Field | Rejected operand |
|---|---|
| 22 | Compaction C domain |
| 23 | Solid fraction phi domain |
| 24 | Nonfinite or out-of-band projected exponent, before bounded `exp(projected)` |
| 25 | Ellipse ratio / cap radicand |
| 26 | Cohesive cap depth D |
| 27 | Singular values |
| 28 | Legacy reservoir H (must be zero) |
| 29 | Trial log-strain trace t |
| 30 | Trial deviatoric norm n |
| 31 | Friction denominator |
| 32 | Reserved former input void-fraction witness |
| 33 | Input porosity-derived cap depth D |
| 34 | Phi increment |
| 35 | Strength phi |
| 36 | Reserved former updated void-fraction witness |
| 37 | Updated porous cap depth D |
| 38 | Cohesion c / shear edge |
| 39 | Projection divisor n |

Field 25 rejects a nonfinite axial ratio or a negative/nonfinite square-root
argument, not a finite ratio above one. The trace is cap-projected first; the
ratio is then projected with `min(ratio,1)` before forming `1-ratio*ratio`.
For a nonnegative finite ratio this implements the existing
`sqrt(max(0,1-ratio*ratio))` edge without rejecting cap-boundary rounding or
overflowing the square. Excess deviatoric norm still activates the radial return.
No other guard tests yield-surface membership. C/phi/H guards retain the admitted
history invariants; zero shear edge is valid because the return never divides by it.

Set **`PPR_PBMPM_RETURN_DIAGNOSTIC=1` before creating solver storage** to
extend the existing first-error channel from 6 to 31 u32 words (24 to 124 bytes).
The first-error CAS winner alone captures values; other rejecting particles
cannot overwrite its witness or payload. `solver.slang::captureCalibratedRejection`
replays only that rejecting particle's arithmetic prefix through its identified
guard. `GpuSolver::pollTick` emits the payload through the existing `MatterGpu`
error logger as **"Calibrated return rejection field=... particle_row=..."**, before the ordinary
candidate-rejected message. The payload includes singular values, t, trial dev,
n, input phi/C, D/c, projected ellipse radicand, projected exponents, H, proposed
phi/C, ratio, denominator, returned t, increment, shear edge, strength phi,
void fraction and `update_history` (0 constraint, 1 integration).
Unreached/not-applicable operands are NaN; projected exponents are deliberately
not evaluated past an earlier rejected guard. Every captured value is included
in the message text with nine significant digits as well as in structured fields;
the ratio is the unprojected axial ratio. Replay is diagnostic FP32
arithmetic, not a claim of bit-identical compiler register capture.

With the switch off, allocation/readback remain six words. With either setting,
accepted particles execute no capture, replay, buffer-size query, diagnostic
stores or logging; accepted constitutive arithmetic is unchanged. The switch
adds status clear/copy bytes when enabled, not per-particle accepted-path work.
Arithmetic rejection still discards the candidate.

The structural loaded-snow regression uses 600 compression substeps and snapshots
every committed substep. Its rejection assertion reports `max_committed_phi`,
including the phase's initial committed snapshot and all successful substeps
before rejection; a successful phase reports `max_phi` as well.

PPR uses liquid relaxation **1**, inherited from its K2 stability tuning, rather than the
selected fork's default of1.5. The historical EA pin defaulted to2, which is the
value the instability analysis below was computed at. The liquid equations
remain unchanged. With default viscosity0.01, the isolated isotropic pressure
mode linearized about unit density has an amplifying eigenvalue about−2.402 at
relaxation2; unit relaxation removes that mode, and increasing relaxation
amplifies it. That analysis describes2, not the selected fork's1.5; the
regression below covers the unit production value. This is an explicit profile
correction, not a scatter clamp or a change to the dense source oracle. Both
production defaults and oracle use these same values. See the isolated pressure
and frontier5×7-dig regression tests. Re-derivation for the new-world K5 schedule
and investigation of its first-step clamp activation remain follow-up work.

`solver.slang` has two production entry points, `fused0` and `fused1`. `fused0`
is the stage-0 bootstrap and `fused1` serves every later stage, so both keep the
node contact/error stage and the shared barriers while K stays a runtime count.
The host encodes `substeps * K` immutable roots and one rooted dispatch per
stage. `sparse_grid.slang` discovers compact buckets/pages, counts and inserts
rows, and initializes the destination through separately ordered GPU passes.
Each substep constructs buckets from current enabled rows, then dispatches
stages0..K-1 indirectly. Stage0 bootstraps scatter without gathering; stages
1..K-2 gather, constrain and scatter; stage K-1 additionally integrates, so
gravity, the engine radial brush and the sand pressure impulse run exactly once
per substep. The stage index and K travel in the runtime parameter row
(`reserved0`, `reserved1`); the shader integrates when
`reserved0 + 1 >= reserved1`. The engine admits K∈{2,5,10} and rejects every
other count before any GPU work. Production submits the world's saved schedule;
new worlds use `kDefaultIterations` (K5) by reference-baseline decision, v6
checkpoints restore K2, and v7 checkpoints retain their saved K. The pinned
no-adapter fixtures keep `kSourceIterations` (K2).
Production advances eight evolving dt=1/480 substeps per fixed tick; drawing does
not advance simulation. Each fused dispatch owns its immutable stage/K,
ping-pong-side, source-generation and predicted-body-base constants through
command completion.

The two sparse payload/index sides strictly alternate per stage, so consecutive
stages never write the side they read. The host, not stage parity, tracks the
side the schedule finished on and that side's write generation: discovery
republishes only the page set of the first stage's destination, and the side the
previous substep finished on keeps the index and generation its reader names.
An odd K therefore starts the next substep on the other side than the previous
one, which parity arithmetic cannot express.

`fork_core.slang` supplies shared64/32 storage access and selected-fork SVD,
constraint and integration arithmetic. `types.slang` supplies scalar column
algebra, shapes and quadratic weights. Arithmetic expression forms and operation
order are retained from the conformance port. The production-only insertion
sites preserve nonfinite endpoint rejection, terrain/body contact before the
guardian and the six-word first-error protocol. The no-hook conformance path
keeps its separate three-word sink.

Two sparse page payload pools and one shared absolute-budget pool replace the
dense production node arrays. Canonical 8-by-8 pages preserve the legacy flat
node key, including its right-edge alias. Source-shape/body contact retains the
original coordinate tuple; terrain uses the grid sample centres described below.
A dispatch reads its predecessor's source: previous A survives iteration0,
then the current page list remaps A for iteration1. Tick-start A is logically
empty. Each destination and its budget are initialized before scatter; absent
source pages return zero words. Shared tile storage is explicitly zeroed and
all lanes cross the shared barriers.


Bucket addressing converts each already-validated nonnegative position to its
integer cell before unsigned division by6. This preserves the mathematical
partition required by `floor(position)-1` and its3-by-3 stencil; a floating
reciprocal near a six-cell boundary can select the next bucket prematurely.
Both production stencil traversals check their signed2D local coordinates
before flattening or shared access and reject a malformed candidate through the
existing structural-error sink. No valid contribution is clamped or omitted.
The pinned WGSL uses floating bucket division and assumes this support invariant;
this is a native discrete-address correction, not a PB-MPM equation change.
The failure probe proved local word index-16; exact failing substep position and
compiler lowering were not captured, so those remain unobserved.

Buckets use six-cell tiles and64-row records. Per-storage row and bucket
capacities and sparse pools are prepared transactionally for the actual admitted
row count with both hot/cold row buffers, before publication. Sparse admission
is independent of spare row-buffer capacity. Bounded atomic reservation rejects overflow rather than clamping
or issuing an out-of-range indirect dispatch. Stable rows are only permuted in
the bucket index buffer; particle storage/order and metadata identity stay fixed.

The production device ABI is a64-byte hot row plus a28-byte cold row (the
no-adapter oracle retains cold24). The saved semantic
record remains22 fields/88 bytes, packed by field name; no v5 wire-layout change.
All evolving fields retain FP32 storage. Terrain masks use4 bits per cell,
boundary tile occupancy uses1 bit per8x8 tile, and the exact contour tables use
8 bytes per case plus one4-byte word for all8 contour points.
CPU admission retains `enabled==1`;
the fork's disabled-row branch is unreachable in production. Every saved row
passes complete semantic validation. Ordinary completion reads only the24-byte
status; full committed readback requires an explicit frozen-epoch snapshot.

The numerical branches include liquid, elastic, sand and visco, guarded zero-pair
SVD, liquid density floor0.05, singular floor0.1, source volume lerp and unilateral
guardian contact with fixed tangential friction0.5. Per-field unsigned CAS
absolute-sum budgets prove signed scatter reductions stay within INT_MAX.
Each contribution keeps its original conversion and rounding, then reserves its
absolute encoded magnitude in a workgroup-local budget before the signed shared
addition. After the shared barrier, each nonempty node/field reserves its complete
local absolute total against the same cross-group sparse budget before merging
the signed sum; a zero signed sum still consumes its nonzero absolute total.
Global rejection skips that signed merge. Two256-word shared uint arrays retain
the absolute totals and actual contributing-row witnesses, adding2048 bytes for
4096 shared bytes for the source/budget tiles per64-thread group, plus the512-byte Sand contact tile described below. First-error race ordering may change
which contributing row is reported; the cause/node/field/valid-row contract stays.
Nonfinite and scatter conversion/overflow errors reject the entire candidate.
The four nonaccumulating post-contact NODE stores follow WGSL2026-10-21
section15.7.6: scale once, reject nonfinite, then clamp/truncate to the
f32-representable i32 endpoints[-2147483648,2147483520]. This approved source-language
conversion exception does not change scatter guards or feed INT_MIN into signed
absolute-sum accounting.
Old v5 semantic fields still load exactly; subsequent trajectories follow this
replacement solver implementation.

Sand has two explicit production adaptations, outside the frozen fork core.
The objective reference-volume sample gathered before the source reciprocal/J
blend supplies a unilateral density impulse after integration. With the legacy
`useGridVolumeForLiquid` flag enabled and `referenceVolume > 1`, Sand retains
`0.5 * 0.1 * max(referenceVolume - 1 - trace(D), 0)` on each diagonal of D.
The next transfer applies this expansive affine motion; the current position,
translation, F, J and plastic logJp are not rewritten. The dimensionless gain0.1
is calibrated for the admitted eight-substep K2 profile, not a timestep-independent
bulk modulus. Flag0 bypasses it. In particular, elastic det(F) after plastic
projection is not a measure of total packing density. This grid-scale controller
does not impose a hard pairwise minimum distance in underfilled regions.

Sand terrain contact uses Coulomb coefficient0.5, distinct from the material's
internal friction angle and the guardian's existing tangent damping. Grid nodes
retain the original base displacement and a separate Sand candidate: terrain
normal projection bounds the tangent reduction, then each candidate passes
through the existing body and guardian order. Sand gathers both translation and
APIC affine motion from its candidate; other materials retain the original path.
Swept endpoint transport applies the same friction to each actual contact's
retained tangent, while geometric recovery and rounding use the original
unfrictioned projection. Recovery distance is never a friction impulse. Tangent
motion without a closing normal projection is unchanged. A64-element float2
shared tile adds512 bytes, bringing the fused group's shared storage to4608
bytes; no dispatch, device buffer, semantic field, binding or ordinary readback
is added. The fork conformance path and `fork_core.slang` remain unchanged.

The game radial brush is another explicit engine adapter, enabled only under
`PBMPM_ENGINE_ADAPTERS`. A separate16-byte constant supplies centre, radius and
signed acceleration in solver-cell units, and the host binds it only to the
final stage's root. That stage adds
`direction * (1 - distance/radius)^2 * acceleration * dt^2` to displacement
beside gravity, before endpoint constraints. The exact centre receives no radial
impulse. Zero acceleration bypasses this arithmetic; the independent source
conformance path has no brush binding or altered equations.

Constitutive operands are admitted, not repaired. `solver.slang` checks the
stored deformation gradient and the trial `(I + D) * F` immediately before the
production `constraint` and `integration` calls. An unusable operand is a
nonfinite matrix, a zero or nonfinite determinant, an unusable inverse (an
overflowing reciprocal or inverse component) or an overflowing decomposition
radicand; it rejects that particle alone through
`recordError(8u, 0xffffffffu, field, row)` with field4 for the stored gradient and
field8 for the trial deformation. The radicand test evaluates the source
decomposition's own `E*E + H*H` and `F*F + G*G` half-sum combinations instead of a
conservative per-entry bound, so the accepted domain is exactly where `forkSvd`'s
squares and both square roots stay finite (`diag(2e19, 1)` is admitted) and no
decomposition is evaluated in the guard. A liquid's `1/density` is validated the
same way as field12, so a positive subnormal density does not depend on the
device flushing it to zero. Group barriers, the other rows and the kernel's
remaining work are unaffected. Deliberate upstream limiting stays legal: the
singular-value clamps, `clamp(abs(df), 0.1, ...)` and the guardian only ever see
finite operands, and no clamped value is re-inspected. After the step, a
nonfinite produced `deformationDisplacement` rejects the row as field8 and a
nonfinite, nonpositive or noninvertible liquid density as field12. That backstop
is what catches a coefficient product that overflows although every admitted
operand stayed finite, and a rejected row contributes nothing to the grid.
Inside the `PBMPM_ENGINE_ADAPTERS` blocks of `fork_core.slang` the sand history
projection reports field17 when the deviatoric norm has no finite value (a huge
but finite `logJp`) or a projected exponent is nonfinite. Finite projected
exponents saturate to `[log(0.1), log(10000)]` before exponentiation, matching
the existing integration singular-value band. Cohesive/compacting integration
and frozen-history constraint targets use the same helper only after checking
that their physical return is already inside the band; an excursion rejects
with field24. This deliberately
bounds reconstruction, not only `exp`: the old `[-87,88]` range permitted
values whose subsequent matrix/SVD products overflowed. The visco plastic
state reports field16 when `exp(1 - plasticity)`, its reciprocal, `J` or `newJ`
is unusable; rescaling now uses `log(sigma)+(log(J)-log(newJ))/2` and the same
bound rather than forming the potentially overflowing `J/newJ`. A rejected
row still rejects the working transaction. The no-adapter conformance path keeps
its original arithmetic. Profile
constants whose derived operands cannot be finite are refused before any work
instead: resident-tick admission returns `invalid_argument` unless
`deltaTime > 0` with a finite `1/deltaTime`, the fixed-point multiplier is at
least one, elasticity ratio, liquid/elastic relaxation and liquid viscosity are
finite, the friction angle lies in `[0, 90)` so `sin` and `3 - sin` stay usable,
and `exp(1 - plasticity)` is finite and positive. Admission bounds that domain,
not coefficient amplitudes; produced-state and scatter checks cover the rest. The
objective-volume gather only forms `1/volume` when the node has support, so an
empty reciprocal is an explicit no-feedback branch rather than an infinity the
comparison happens to discard. No error word, semantic field, binding or wire
record is added.

Encodability is enforced on the actual scatter operands, including
`weight * mass * (displacement + D * offset)`, by checked conversion and the
absolute-sum budgets; finite raw `D` has no fixed-point bound. Adapter guards
precede their dependent operations: finite `deltaGamma` precedes the sand branch,
the norm check precedes its division, the finite yield reciprocal precedes visco
clamp bounds, and valid `newJ` precedes logarithmic rescaling.

Terrain and one-way predicted bodies are engine adapters. Terrain grid contact
uses `node + 0.5`, matching the quadratic weights and APIC offsets. Source shapes,
grid bodies and the guardian retain their selected-fork coordinate conventions.
Grid moving-body contact projects relative to local boundary displacement.
Particle body carry terms are evaluated at the original integrated endpoint;
terrain then applies the final constraint when both adapters are present.

`terrain_motion.slang` separates geometric transport, initial-overlap recovery,
and retained motion. The existing F/D/J update still occurs once from the gathered
grid state. Recovery moves the published point to the nearest canonical boundary
in the same substep, without turning its distance into carry or strain. Particle
transport sweeps directed canonical segments using a slab-clipped 3x3-supercover
DDA, then consumes admissible tangential remainder. Convex and concave vertices
use their distinct local free-space cones. Physical samples remain points; the
rendered sprite radius is not a collision radius.

Boundary rounding preserves already-certified representable points exactly.
Equal ULP steps at different coordinate magnitudes can otherwise turn a valid
concave-corner contact into interior penetration. Uncertified rounding uses at
most three representable outward steps, not a fixed skin. The two transport exits
that publish a chosen point (a representable leg with no motion, and a no-hit
sweep) are the exception to the immediate rejection: when the chosen point is not
certified they first attempt a bounded exterior projection (the nearest
3x3 canonical feature, then at most three representable outward steps), and
reject only if that also fails. The published point therefore moves to the nearest
certified boundary within that bound, never by a silent clamp, and a passing
position is returned untouched so a successful tick is bit-identical. Successful
positions are certified against strict solid interior, including
after the guardian. Closed, zero-depth boundary contacts are allowed. Incomplete
finite-domain entry reports flag 16 rather than inventing a domain collider.
Ambiguous clipping, malformed feature topology, failed rounding certification,
or unresolved transport beyond 32 contact events reports flag 256 and rejects the
working transaction; a failed exit repair records its own rounding or
missing-feature field first and the final-position field 6 only when the repair
also failed. For flag 256 the field records the resolution reason:
1 reversed straight edge, 2 malformed vertex, 3 outward rounding, 4 missing local
feature, 5 DDA exhaustion, 6 final position, 7 contact budget, 8 domain-entry proof.
The budget counts contacts; a final no-hit sweep can complete sliding motion.

Each geometric leg sweeps the saved representable FP32 target, rather than an
unrounded displacement whose endpoint can round across a diagonal at large
coordinates. Slang 2026.17.1 ignores per-variable `precise` on direct SPIR-V
(upstream #12198/#12208), so the terrain-local rounded-leg helper emits explicit
`NoContraction` for just its add/subtract pair; HLSL uses local `precise` values.
No global floating-point mode or constitutive expression changes. Recheck this
pinned-version workaround when upgrading Slang.

`environment.slang` preserves the complete nearest-contour search and its
explicit incomplete-boundary errors. Terrain masks, tables, resource bindings,
and ownership are unchanged. No reciprocal reaction readback or ordinary
per-particle CPU loop is introduced.

`fork_conformance.slang` retains the explicitly test-only dense storage route and
full frozen-WGSL stage/grid comparison. It is not a production fallback.
Production parity observes stages1,3,...,15, tick2 and restored22, using the same
fixed batch5 word bounds. The production fixture projects out the oracle's
disabled row while preserving active row order and original-index comparisons;
the full conformance fixture retains disabled-row coverage. Production exposes
no grid readback. No claim of full material fidelity follows from the two oracle
scenarios. `contact_checks.slang` is test-only inspection of the production grid
contact helper; it is not another solver or product entry. `source_checks.slang`
inspects the actual shared algebra/validation helpers.

Build, conformance, production parity, contact and lifetime gates must run after
independent adapter/root review; source editing alone is not validation evidence.
