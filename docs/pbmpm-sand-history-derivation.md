# A5 gate: sand plastic-history law

**Disposition: rejected for Phase A; current law retained, reconsider at B1.**
The plan owner's [ruling and characterization](pbmpm-baseline-and-multilayer-results.md#a5--documented-rejection-for-phase-a-reconsider-at-b1)
supersede this artifact's original blocked recommendation. Reopen before sand
property semantics are frozen at B1, or earlier on a demonstrated
supported-workload failure. Constitutive and constraint-policy choices remain
prerequisites for replacement. The
candidate conserves a scalar log-volume ledger and is dissipative under the
conditions below. Those results do not establish compatibility with the current
PB-MPM constraint. This is an analysis artifact only; no implementation or
build/test execution accompanies it.

## Sources and conventions

- [K] Klár et al., *Drucker–Prager Elastoplasticity for Sand Animation* (2016),
  [primary paper](https://math.ucdavis.edu/~jteran/papers/KGPSJT16.pdf),
  §§6.3, 7.1–7.3, equations (25)–(31).
- [T] Their [technical supplement](https://www.cs.ucr.edu/~craigs/papers/2016-sand/tech-doc.pdf),
  §§2.1–2.3, equations (32)–(34), and §3 (plastic flow and dissipation).
- [P] [`fork_core.slang`](../assets/shaders/pbmpm/fork_core.slang),
  `constraint` (especially lines 120–138) and `integration` (163–213), inspected
  in this checkout. This is the port of ErikSom/pbmpm revision `859c449`.

Use tension-positive logarithmic principal elastic strain
\(e=\log\sigma\in\mathbb R^2\), \(s=e_1+e_2\), \(H=\mathtt{logJp}\),
\(\mathbf1=(1,1)\), and Euclidean/Frobenius norm. Assume orientation-preserving
deformation and finite positive singular values. “Unclipped” means neither [P]'s
\([0.1,10000]\) singular-value clamp nor its exponent-domain guard modifies or
rejects the step. Otherwise the ledger must explicitly include the clamp's
\(\Delta\log J\); there is no balance theorem for the original trial deformation.

## 1. Meaning of history; the current deviator is not trace-free

Multiplicative plasticity gives

\[
F=F_EF_P,\quad J_P=\det F_P>0,\quad
\log J=\log\det F_E+\log J_P=\operatorname{tr}(\log V_E)+H.
\]

Thus the proposed ledger is
\[
\boxed{t=s+H=\operatorname{tr}(\log V_E)+\mathtt{logJp}.}
\]

A general \(\log\det F_P\) can have either sign. **For this candidate's particular
history policy**, \(H\ge0\) represents retained stress-free expansion/separation
(\(J_P\ge1\)): subsequent compression consumes that reservoir before creating
elastic compression. It is not accumulated equivalent plastic strain, density,
occupied cell volume, or the hardening variable \(q_p\) in [K] equation (29).
The candidate also chooses isotropic reinsertion of this scalar history; a scalar
cannot recover the full plastic tensor.

[K]'s trace-preserving shear return is the primary law; [K] does **not** specify
this candidate's scalar-history reinsertion rule. [P]'s variable name suggests
plastic log-volume, but its update fails that interpretation quantitatively:
at \(t\ge0\), it sets \(e'=0,H'=t/2\), losing half the ledger. Reading \(H\) as a
per-axis average does not fix this: then the input trace would need \(s+2H\).

In [P],
\[
\widehat e_{\rm port}=e-\tfrac{s+H}{2}\mathbf1
=\operatorname{dev}_2 e-\tfrac H2\mathbf1,
\quad \operatorname{tr}\widehat e_{\rm port}=-H,
\quad \|\widehat e_{\rm port}\|^2=\|\operatorname{dev}_2e\|^2+H^2/2.
\]
It is deviatoric only when \(H=0\). In contrast, for
\(q=e+(H/2)\mathbf1\),
\[
\operatorname{tr}q=t,\qquad d=q-(t/2)\mathbf1
=e-(s/2)\mathbf1,\qquad \operatorname{tr}d=0.
\]

## 2. Exact balance of the candidate

Let \(n=\|d\|\), \(c\ge0\). The division-free definition is
\[
(e',H')=\begin{cases}
(0,t),&t\ge0,\\
((t/2)\mathbf1,0),&t<0,\ n=0,\\
((t/2)\mathbf1+\min(1,-ct/n)d,0),&t<0,\ n>0.
\end{cases}
\]
This equals the proposed \(\Delta\gamma=\max(0,n+ct)\) formula for \(n>0\).
For \(n=0,t<0\), \(\Delta\gamma=0\); retain isotropic compression rather than
evaluating \(0/0\) or returning to the apex. In [K], the elastic-case test precedes
the zero-norm apex test, which matters here.

In every branch,
\[
\boxed{\operatorname{tr}e'=\min(t,0),\quad H'=\max(t,0),\quad
\operatorname{tr}e'+H'=s+H.}
\]
The last identity even holds algebraically for negative input \(H\), but that
does not make such input admissible. It proves conservation only during this
constitutive return. A new deformation increment \(A\) legitimately adds
\(\log\det A\) to the ledger. It proves neither occupied-volume preservation,
mass-density consistency, dissipation, nor physical correctness of the solver.

## 3. Yield coefficient and dimensional choice

The port uses the **effective two-dimensional** cone from [T],
\[
f(\tau)=\|\operatorname{dev}_2\tau\|+\alpha\operatorname{tr}\tau\le0,
\qquad \text{cohesion}=0.
\]
Using [K]'s Hencky energy and its work-conjugate Kirchhoff stress,
\[
W(q)=\mu\|q\|^2+\tfrac\lambda2t^2,
\quad \tau=2\mu q+\lambda t\mathbf1,
\quad \operatorname{tr}\tau=(2\mu+2\lambda)t,
\quad \operatorname{dev}_2\tau=2\mu d,
\]
gives
\[
\frac{f}{2\mu}=n+ct,\qquad
\boxed{c=\alpha\frac{2\lambda+2\mu}{2\mu}
=\alpha(1+\lambda/\mu).}
\]
This is [K] equation (27), with \(d=2\), not an independently fitted coefficient.
Require \(\mu>0\), \(K_2=\lambda+\mu>0\), and \(0\le\phi<\pi/2\).

There are two distinct friction calibrations:

- Retaining [P] and [K] equation (31) uses
  \(\alpha_{\rm port}=\sqrt{2/3}\,2\sin\phi/(3-\sin\phi)\). This is a
  three-dimensional Mohr–Coulomb matching convention applied to a 2D cone.
- Exact 2D Coulomb traction maximization in [T] equation (32), with
  \(\widetilde\alpha=\tan\phi\), gives
  \(\alpha_{2D}=\widetilde\alpha/\sqrt{2(1+\widetilde\alpha^2)}
  =\sin\phi/\sqrt2\). These are not interchangeable calibrations.

**Effective 2D is not automatically physical 3D plane strain.** For example,
if elastic \(q_3=0\), a true 3D cone instead gives
\[
\frac{f_3}{2\mu}=\sqrt{n^2+t^2/6}
+\alpha_{\rm port}\frac{3\lambda+2\mu}{2\mu}t.
\]
Its 2D radial bound is different, and total plane strain can still permit
out-of-plane plastic strain. A genuine plane-strain model needs that closure;
the candidate's two-component formula does not provide it.

**`elasticityRatio` cannot currently be justified as \(\lambda/\mu\).** In
`constraint`, it blends a singular-value target with the determinant-normalized
target \(Q\); `elasticRelaxation` subsequently scales the correction. A blend
weight is not a measured modulus ratio. [P]'s
\(c_{\rm port}=(1+\mathtt{elasticityRatio})\alpha\) has the right algebraic
shape only if that identification is independently established.

Replace that identification with an explicit constitutive \(\lambda/\mu\) (or
directly specified \(K_2/\mu\) or \(c\)) after choosing the dimensional/friction
convention. An authored \(E,\nu\) with a declared reduction would fix it; for
3D isotropic Lamé constants \(\lambda/\mu=2\nu/(1-2\nu)\). Alternatively, measure
small-strain volumetric and shear response of the actual constraint at its
iteration count, timestep and relaxation, and establish the applicable mapping.
No such calibration follows from the inspected expressions.

## 4. Interaction with the exact-zero constraint branch

The candidate makes \(H'=0\) **exactly whenever \(t\le0\)**. That state was
already reachable in [P] at \(t<0\); it is not a newly introduced branch, but
history consumption and corrected strains change its inputs and timing.

For every candidate output with \(t<0\),
\[
e'_1+e'_2=t<0\quad\Longrightarrow\quad\min_i e'_i<0
\quad\Longrightarrow\quad\min_i\sigma'_i<1.
\]
Consequently, with no intervening deformation, [P]'s
`if (p.logJp == 0.0) clamp(Sigma, 1, 1000)` necessarily changes at least one
singular value. At the target level it replaces
\[
e'_i\mapsto\min(\max(e'_i,0),\log1000),
\]
whose summed trace is nonnegative instead of \(t<0\), while leaving \(H=0\).

Concrete unclipped case: \(e=(-0.3,-0.3),H=0.4\) gives
\(e'=(-0.1,-0.1),H'=0,t=-0.2\). At the next `constraint` with \(D=0\),
both the clamped SVD target and \(Q\) equal \(I\), since
\(\det F=\exp(-0.2)\in[0.1,1]\). Thus this example is independent of the blend
weight. With full relaxation and zero viscosity, the next trial \(F\) is \(I\),
and its ledger is zero, not \(-0.2\). Partial positive relaxation moves the
isotropic stretch toward one and also changes the ledger.

Important distinction: `constraint` changes \(D\), not directly the stored
\(F,H\). Its later integration is a new mechanical increment. Therefore this
does **not** refute §2's return-only identity: the mechanical budget must include
that increment. It **does** refute an unchanged-ledger claim for the composite
zero-displacement constraint/return experiment, and prevents treating the
constraint clamp as a history-neutral projection. Whether this rebound is the
intended compressive elastic response is a physical/solver choice, not algebra.

## 5. Dissipation, monotonicity and admissibility

For \(t<0\), write \(r=\min(n,-ct)\), \(\gamma=n-r\). For fixed \(n\), increasing
\(t\) toward zero makes \(r\) nonincreasing and \(\gamma\) nondecreasing.
Both \(\operatorname{tr}e'=\min(t,0)\) and \(H'=\max(t,0)\) are monotone in
\(t\). This is scalar monotonicity, not a proof of operator monotonicity or of
stable coupled dynamics.

With \(\mu>0,K_2>0,c\ge0,H\ge0\), the return is dissipative under the Hencky
energy of §3 and a history reservoir with no extra stored energy:
\[
W(e)=\mu n^2+\tfrac{K_2}{2}(t-H)^2,\quad
W(e')=\mu r^2+\tfrac{K_2}{2}t^2,
\]
\[
W(e)-W(e')=\mu(n^2-r^2)+\tfrac{K_2}{2}(H^2-2tH)\ge0.
\]
The coaxial plastic log increment is
\(\Delta e_P=e-e'=\gamma d/n-(H/2)\mathbf1\), with the shear term defined as
zero at \(n=0\). Final-stress plastic work is
\[
\tau':\Delta e_P=2\mu r\gamma-K_2tH\ge0.
\]
One continuous dissipative realization consumes history from \(e\) to \(q\)
isotropically while the trace stays negative, then reduces the deviatoric norm
from \(n\) to \(r\). Each leg has nonnegative plastic work. This extends [T]'s
non-associative, volume-preserving shear argument; the history leg is a new
modeling choice, not a quotation of [K].

For \(t\ge0\), \(e'=0\), so \(W(e')=0\); a proportional unloading
\(e(a)=(1-a)e\), \(H(a)=H+as\), \(0\le a\le1\), keeps the ledger and
nonnegative history and has nonnegative plastic work. Endpoint stress is zero;
zero endpoint work does not mean zero integrated dissipation.

- \(n=0,t<0\): no shear correction, only isotropic history consumption.
- \(t=0\): return to \(e'=0,H'=0\); the compressive-side limit agrees, with
  no division by zero. At \(c=0\), the compressive branch removes all shear.
- \(H<0\): balance survives but dissipation need not. For \(e=0,H=-h<0\),
  the candidate creates \(e'=-(h/2)\mathbf1\), increasing energy by
  \(K_2h^2/2\) and giving endpoint plastic work \(-K_2h^2\). Reject this input
  or define a separate, owner-approved migration/energy interpretation; silently
  clamping history loses the ledger.

The prompt's premise that current integration *produces* negative history is
not supported by [P]: its finite accepted assignments are \(t/2\) when
\(t\ge0\), or zero otherwise. Error rollback can preserve a negative imported
history, and CPU particle admission in `Matter.Gpu.cpp` checks finiteness rather
than nonnegativity. Creation from nonnegative history requires evidence of a
different writer/revision; it cannot be attributed to these assignments.

## 6. Exact experiment gates (specified, not executed)

Use a pure log-space oracle first. The following dimensionless fixtures use
\(c=1/2\) solely to fix analytical expected values, not as a material calibration.
Tuple entries below are \((e_1,e_2;H)\).

| Case | Input | Required output |
|---|---|---|
| Positive-history rest | \((0,0;0.4)\) | \((0,0;0.4)\), unchanged on every repeated return |
| Partial isotropic consumption | \((-0.1,-0.1;0.4)\) | \((0,0;0.2)\) |
| Exhausted history, zero deviator | \((-0.3,-0.3;0.4)\) | \((-0.1,-0.1;0)\) |
| Apex boundary | \((-0.2,-0.2;0.4)\) | \((0,0;0)\) |
| Isotropic compression without history | \((-0.1,-0.1;0)\) | unchanged |
| Anisotropic active yield | \((0.1,-0.5;0.2)\) | \((-0.1+0.1/\sqrt2,-0.1-0.1/\sqrt2;0)\) |
| Anisotropic elastic interior | \((-0.19,-0.21;0.2)\) | \((-0.09,-0.11;0)\), \(\gamma=0\) |
| Tensile trace with shear | \((0.3,-0.1;0)\) | \((0,0;0.2)\) |
| Invalid negative history | \((0,0;-0.2)\) | explicit rejection under the proposed admissible domain |

[P] provably fails the first fixture: it outputs \(H=0.2\), then \(0.1\),
etc., at rest. In its active compressive branch, with
\(a=\Delta\gamma/\|\widehat e_{\rm port}\|\), it gives
\(\operatorname{tr}e'=s+aH\), so its ledger residual is \((a-1)H\);
an inactive return discards \(H\) altogether. Neither generally conserves \(t\).

Proposed tolerances for these bounded fixtures (\(|e_i|,|H|\le1\), no clipping):

- Float64 algebra reference: absolute residual \(\le10^{-12}\).
- FP32 log-space return: component and scalar residual
  \(\le\epsilon=2\times10^{-6}\max(1,|e_1|+|e_2|+|H|)\).
- FP32 SVD/exp/log reconstruction: \(\le2\times10^{-5}\) in log components
  and ledger for these fixtures. No branch masking by this tolerance: assigned
  \(H'=0\) must be exact zero; use exactly representable zero-boundary fixtures
  as well as offsets \(t=\pm10^{-4}\).
- With \(\mu=K_2=1\), energy increase and negative plastic work may not exceed
  the respective numerical tolerance above. These tolerances are proposed
  experiment acceptance thresholds, not measured backend error bounds.

For every accepted fixture require finite positive reconstructed stretches,
\(H'\ge0\), the §2 balance, \(\operatorname{tr}d=0\),
\(\|\operatorname{dev}e'\|+c\operatorname{tr}e'\le\epsilon\), and
\(\|\operatorname{dev}e'\|=\min(n,-ct)\) for \(t<0\). Active yield must land
on the boundary, inactive yield retain \(q\), and the return must be idempotent.
Repeat positive-history rest 1,024 times without secular decay; swap axes and
rotate the reconstructed deformation to check objectivity. For general
nonnegative-history cases, use \(t=-1,-10^{-4},0,10^{-4},1\) and deviatoric
norms below, on and above \(-ct\) where that bound is nonnegative.

Run the §4 constraint interaction as a separate analytical acceptance case:
its mechanical contribution must be explicitly budgeted, never counted as a
passing unchanged-ledger return. If the experiment requires the combined
constraint/return to preserve the pre-constraint ledger with no external step,
the present branch fails by \(0.2\), far above tolerance. Clipped singular
values, invalid orientation, nonfinite operands and out-of-range exponents
require separate rejection/clipping-budget cases; excluding them silently is
not acceptance. Any failed identity, history decay, negative work beyond
tolerance, or unexplained constraint contribution fails the experiment.

## 7. Recommendation to the plan owner

**Do not authorize an integrated A5 implementation from the candidate alone.**
Algebra establishes the trace-free return, exact ledger, positive-history rest,
and conditional dissipativity. A standalone test-only algebra oracle is
mathematically justified with explicit \(c\); it would not validate the material.

Before a shader experiment, choose and record:

1. Effective 2D versus true plane strain, friction-angle calibration, and an
   explicit modulus ratio or calibrated \(c\); the blend parameter is insufficient.
2. Whether \(H\) is the nonnegative stress-free expansion reservoir assumed here,
   and how negative imported histories are rejected or migrated.
3. Whether the zero-history clamp's compressive rebound is intended mechanics
   (with its own deformation/work budget), or whether that constraint must be
   redesigned alongside the return. An unchanged whole-step invariant is
   incompatible with the current branch.

These are physical and solver-policy decisions. Passing scalar conservation
tests cannot make them implicitly, nor demonstrate preservation of occupied
sand volume or dissipation of the complete PB-MPM update.
