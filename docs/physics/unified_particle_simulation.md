# Adaptive Unified Particle Matter Framework

## Implementation specification for coding agents

**Status:** Research-driven architecture proposal
**Primary target:** Real-time game simulation on GPU
**Primary objective:** Unified simulation substrate for gases, liquids, granular matter, and eventually deformable solids, with dynamic spatial resolution through particle splitting and merging.
**Design priority:** Scale and emergent behavior over strict physical accuracy.

---

# 0. Agent instructions

Treat this document as an implementation specification.

When implementing:

- **MUST** preserve the invariants explicitly listed in this document.
- **MUST NOT** advance to the next implementation phase until the current phase's gate passes.
- **MUST** implement validation scenes and diagnostic counters together with each feature.
- **MUST** collect GPU timings separately for every simulation pass.
- **MUST** favor simple, deterministic implementations before optimizing them.
- **MUST NOT** introduce PB-MPM, FLIP, a pressure Poisson solver, or a dense Eulerian world grid unless a later phase explicitly calls for one.
- **SHOULD** use GPU-friendly structure-of-arrays storage.
- **SHOULD** make algorithms independent of rendering.
- **SHOULD** keep physics parameters dimensionally meaningful where practical, even when the simulation itself is deliberately approximate.
- **MAY** replace an algorithm with a faster one only after demonstrating equivalent behavior with the validation suite.

All speculative algorithms in this document are marked **PROPOSED**.

Algorithms backed directly by prior work are marked **ESTABLISHED**.

---

# 1. Problem statement

Build a particle-based simulation framework capable of representing:

```text
gas
liquid
granular material
rigid/cohesive matter
eventually deformable solids
```

using one underlying material-particle representation.

The framework must support:

- many material species;
- arbitrary local mixtures;
- density differences;
- buoyancy;
- viscosity;
- compressibility;
- phase changes;
- material interfaces;
- particle splitting;
- particle merging;
- spatially adaptive resolution;
- sleeping;
- large sparse worlds;
- GPU execution;
- eventual multi-rate temporal integration.

Physical realism is secondary to:

1. stability;
2. performance;
3. qualitative correctness;
4. emergent interaction;
5. controllability by gameplay.

The framework must scale according to **complexity of matter**, not total world volume.

---

# 2. Why this architecture

Existing work establishes several relevant facts independently.

NVIDIA's Unified Particle Physics demonstrated that a particle/constraint representation can support gases, liquids, rigid bodies, deformable solids and cloth with two-way interaction in a real-time framework. This work became the basis of FleX, although FleX itself is now a legacy SDK.

Adaptive SPH work has demonstrated dynamic particle splitting and coalescing while conserving mass and momentum. Vacondio et al. showed variable-resolution SPH with dynamic split/merge and reported substantial efficiency gains relative to uniformly fine resolution in their test cases.

GPU adaptive particle splitting and merging has also been demonstrated; Xiong et al. implemented adaptive particle refinement on single and multiple GPUs and combined it with multiple time stepping.

Adaptive multiphase SPH has demonstrated refinement driven by distance to interfaces between phases.

The Phantom SPH code currently ships an Adaptive Particle Refinement implementation with particle split/merge, discrete refinement levels and mass ratios of two between adjacent refinement levels. Its documentation explicitly recommends allowing several sound-crossing times between repeated refinement operations to reduce noise.

Position-Based Fluids and NVIDIA's unified particle work establish constraint-based particle simulation as a robust real-time approach, while XPBD provides a way to reduce timestep- and iteration-dependence of constraint stiffness.

EA's PB-MPM demonstrates another successful game-oriented attempt at robust material simulation, but PB-MPM retains repeated particle/grid transfers and is therefore a reference for material modeling and stability rather than the base architecture selected here.

The architecture specified below combines ideas from these lines of work but is **not itself a published solver**.

---

# 3. Central design decision

The authoritative simulation state is a set of **material parcels represented by particles**.

A particle represents:

> a finite amount of one material occupying a finite volume.

Particles are not visual droplets.

Particles are Lagrangian continuum samples.

The core particle representation is:

```cpp
struct Particle
{
    float3 position;
    float3 velocity;

    float mass;
    float volume;

    MaterialId material;

    uint8_t level;

    // introduced in later phases
    float temperature;
    float internalEnergy;
};
```

The fundamental quantities are:

```text
mass_i
volume_i
density_i = mass_i / volume_i
position_i
velocity_i
material_i
resolution_level_i
```

The same representation must remain valid for gas, liquid and granular matter.

---

# 4. Fundamental invariants

These invariants have priority over visual quality.

## 4.1 Mass conservation

Except for explicit gameplay creation/destruction/reactions:

\[
\sum_i m_i = constant
\]

Splitting:

\[
m_p = \sum_c m_c
\]

Merging:

\[
m_p = \sum_c m_c
\]

Numerical tolerance must be tracked every frame.

---

## 4.2 Momentum conservation

For split and merge:

\[
m_p v_p = \sum_c m_c v_c
\]

Particle interactions should be pairwise symmetric wherever practical.

Track total momentum drift in isolated validation scenes.

---

## 4.3 Material conservation

Unless a chemical or phase-change operation explicitly transforms species:

```text
mass(material X) before
=
mass(material X) after
```

Never merge particles with different `MaterialId` during the initial implementation.

---

## 4.4 Represented volume conservation

For non-reactive matter:

\[
V_p = \sum_c V_c
\]

Volume may evolve through compression when the material model permits it.

---

## 4.5 Positive state

Always enforce:

```text
mass   > 0
volume > 0
radius > 0
```

Never allow NaN or infinity into persistent particle buffers.

Add a GPU validation pass in debug builds.

---

# 5. Resolution hierarchy

Do not support arbitrary particle radii initially.

Use discrete resolution levels:

```text
L0
L1
L2
L3
...
```

Define:

```text
linear scale:
    h(L) = h0 * 2^L

represented volume:
    V(L) = V0 * 8^L
```

for three dimensions.

Therefore:

```text
L0 =   1 × base volume
L1 =   8 × base volume
L2 =  64 × base volume
L3 = 512 × base volume
```

This is intentionally octree-like.

A coarse particle can be split into eight children of the next finer level.

```text
                 L2
                  O
                  |
        +---------+---------+
        | | | | | | | |
        o o o o o o o o
                 L1
```

The hierarchy need not be stored explicitly.

Only leaves exist.

---

# 6. Refinement adjacency rule

Maintain a **2:1 linear resolution rule**:

```text
abs(level_i - level_j) <= 1
```

for particles close enough to interact strongly.

Do not allow:

```text
L0 directly adjacent to L4
```

Instead generate transition layers:

```text
L0 L0 L0 | L1 | L2 | L3
```

This rule reduces numerical discontinuities at resolution boundaries.

Phantom similarly restricts adjacent APR levels and uses discrete particle-mass refinement levels.

---

# 7. Core material model

Start with:

```cpp
enum class MaterialPhase : uint8_t
{
    Gas,
    Liquid,
    Granular,
    Solid
};

struct Material
{
    MaterialPhase phase;

    float referenceDensity;

    float bulkStiffness;
    float viscosity;

    float cohesion;
    float friction;

    float thermalCapacity;
    float thermalConductivity;

    float meltingTemperature;
    float boilingTemperature;
};
```

Do not make the solver switch between completely unrelated algorithms solely based on phase.

Instead interpret the material parameters as constitutive behavior.

Conceptually:

```text
gas
    low bulk stiffness
    low density
    negligible cohesion

liquid
    high bulk stiffness
    low shear resistance
    material-dependent viscosity

granular
    compression resistance
    friction
    yield-like behavior

solid
    compression resistance
    shear resistance
    cohesion
```

The goal is a common particle substrate with phase-dependent constraints, similar in spirit to Unified Particle Physics rather than one universal constitutive equation.

---

# 8. Spatial acceleration structure

## 8.1 Do not use a dense world grid

The world may be arbitrarily large.

The acceleration structure must depend only on active particles.

Use a hierarchical spatial grid.

For particle level `L`:

```text
cellSize(L) = kernelScale * h(L)
```

Compute:

```text
cellCoord = floor(position / cellSize)
```

Use a key:

```text
key =
    morton(cellCoord)
    + encodedLevel
```

Sort particles by this key.

---

# 9. GPU particle pipeline

Initial implementation:

```text
Particle buffer
      |
      v
Generate spatial key
      |
      v
Radix/counting sort
      |
      v
Build CellRange table
      |
      v
Neighbor queries
      |
      v
Physics passes
      |
      v
Integrate
```

Prefer:

```text
SoA
```

over:

```text
AoS
```

for hot particle properties.

Suggested buffers:

```text
positionRadius[]
velocity[]
massVolume[]
materialLevel[]
```

Later optional buffers:

```text
temperature[]
affineVelocity[]
deformation[]
```

---

# 10. Neighbor search with mixed resolutions

A particle interaction must be symmetric.

Define particle support radius:

\[
h_i = h_0 2^{L_i}
\]

For particles `i` and `j`, use a symmetric interaction radius such as:

\[
h_{ij} = \max(h_i,h_j)
\]

for the first implementation.

Do not use:

```text
h_i only
```

because that creates asymmetric neighbor relationships.

A pair is considered interacting when:

\[
|x_i-x_j| < k h_{ij}
\]

where `k` depends on the selected kernel.

---

# 11. Initial kernel

Do not begin with a complicated SPH kernel family.

Use one compact, monotonic kernel.

Candidate:

```text
poly6 / cubic spline / Wendland C2
```

The exact kernel is less important initially than:

```text
symmetry
compact support
positive weights
continuous first derivative
```

Expose:

```cpp
weight(distance, hij)
gradient(distanceVector, hij)
```

through one module so kernels can later be replaced.

---

# 12. Solver philosophy

The first solver should combine:

```text
SPH-style local density estimation
+
PBD/XPBD-style positional correction
```

rather than implementing full incompressible SPH.

Position Based Fluids showed that density constraints can provide robust real-time liquid behavior at larger timesteps than traditional force-based incompressibility methods.

XPBD should later be considered for parameter stability across varying timestep and iteration count.

---

# 13. Density estimate

Estimate local density:

\[
\rho_i =
\sum_j m_j W(r_{ij},h_{ij})
\]

with variable particle masses and symmetric support.

Do not assume:

```text
m_i == m_j
```

because adaptive resolution explicitly violates this.

The variable-resolution SPH literature should be treated as the reference for problems caused by unequal masses and smoothing lengths.

---

# 14. Volume occupancy

Also compute local occupied volume:

\[
\phi_i =
\sum_j V_j W(r_{ij},h_{ij})
\]

This quantity is valuable because:

```text
mass density
```

and:

```text
space occupancy
```

should not be conflated in a heterogeneous-material simulation.

For example:

```text
same volume water particle
same volume oil particle
```

have different mass.

---

# 15. Phase-independent pressure concept

Use local compression relative to each material's preferred density or occupied volume.

First prototype:

\[
C_i = \frac{\rho_i}{\rho_{0,i}} - 1
\]

or, if volume behavior is more stable:

\[
C_i = \phi_i - \phi_0
\]

Apply a position-based correction.

Do not initially enforce strict incompressibility.

Allow material stiffness to control compressibility.

Example:

```text
air:
    low stiffness

water:
    high stiffness

oil:
    moderately high stiffness
```

This is one of the main mechanisms by which gas and liquid can share the same solver.

---

# 16. Pressure correction

Start with a PBF-like density constraint.

Conceptually:

```text
for each solver iteration:
    compute density constraint
    compute lambda
    compute symmetric position corrections
```

Do not copy PBF blindly because classic PBF assumes uniform particle properties.

The implementation must account for:

```text
different masses
different rest densities
different support radii
```

Treat variable-resolution SPH literature as the reference for conservation and kernel consistency.

---

# 17. External forces

At minimum:

```text
gravity
user/gameplay forces
collision response
```

Gravity must operate as acceleration:

```text
v += gravity * dt
```

not force proportional to particle mass after acceleration has already been computed.

This guarantees equal gravitational acceleration across materials.

Buoyancy should emerge from pressure/density interaction, not an explicit:

```text
if material == oil:
    move upward
```

rule.

However, a later gameplay correction may be added if the emergent buoyancy is too weak.

---

# 18. Phase 0 — infrastructure

## Goal

Build the GPU particle engine without fluid physics.

## Implement

- particle allocator;
- particle destruction;
- SoA storage;
- GPU compaction;
- spatial key generation;
- GPU sorting;
- cell range construction;
- neighbor enumeration;
- simulation statistics;
- debug validation;
- simple rendering.

## Required validation

Create:

```text
1k particles
10k particles
100k particles
1M particles
```

randomly distributed.

Verify every GPU neighbor query against a CPU brute-force query on reduced datasets.

## Metrics

Measure:

```text
key generation
sort
range construction
neighbor enumeration
```

independently.

## Gate 0

Proceed only if:

```text
neighbor query correctness = 100%
particle creation/deletion stable
no GPU validation errors
no NaNs
```

and complexity scales plausibly with particle count.

---

# 19. Phase 1 — fixed-resolution single-material liquid

## Goal

Establish a robust baseline solver before introducing adaptivity.

Use:

```text
one material
one particle level
one particle mass
one particle radius
```

Implement:

```text
gravity
density estimation
pressure/density constraint
2–4 solver iterations
boundary collisions
basic viscosity
```

Use water only.

---

## Validation scenes

### Hydrostatic box

Container filled halfway.

Expected:

```text
fluid settles
no long-term explosive instability
bounded compression
```

Measure:

```text
mean density
max density
kinetic energy decay
```

### Dam break

Standard column collapse.

Expected:

```text
qualitatively fluid-like spreading
stable wall impact
```

### Compression torture test

Move two walls toward one another.

Expected:

```text
solver remains finite
fluid compresses rather than exploding
```

### Rotating container

Used to expose excessive damping and neighbor anisotropy.

---

## Gate 1

Proceed if:

```text
simulation runs continuously without numerical explosion;
mass error == 0 except floating-point accumulation noise;
density remains bounded;
no persistent particle clumping;
collision handling is stable.
```

Do not require visual realism.

---

# 20. Phase 2 — multiple liquid materials

## Goal

Verify the central material-representation premise.

Add:

```text
water
oil
methanol
```

with different:

```text
reference density
viscosity
```

Do not add splitting yet.

---

## Mixture representation

Each particle retains one `MaterialId`.

A local mixture is represented implicitly by the local particle population.

Example:

```text
cell neighborhood:

W W M
W M M
O W M
```

No fixed concentration array is stored.

---

## Interaction rule

Initially, allow all liquid particles to contribute to the same density/pressure neighborhood.

Use particle-specific mass and rest density.

Test whether:

```text
oil rises above water
water/methanol remain dynamically mixed
```

without explicit material-separation forces.

---

## Gate 2A — density stratification

Scene:

```text
bottom half oil
top half water
```

Expected:

```text
configuration reverses over time
water moves below oil
```

If it does not happen adequately:

implement a **symmetric pairwise density segregation correction**.

This correction must:

```text
preserve pair momentum
depend on gravity direction
depend on rest-density difference
be zero for equal densities
```

Keep its strength tunable.

---

## Gate 2B — mixture preservation

Initialize:

```text
50% water
50% methanol
```

randomly intermixed.

Expected:

```text
no spontaneous macroscopic separation
```

Do not introduce actual molecular diffusion yet.

---

# 21. Phase 3 — discrete adaptive resolution

## Goal

Introduce particle levels without dynamic split/merge.

Create particles at:

```text
L0
L1
L2
```

but do not change levels during simulation.

This phase isolates mixed-resolution numerical problems.

---

## Required scenarios

### Homogeneous mixed-level liquid

Create a static water volume with graded levels:

```text
L0 L0 L1 L1 L2 L2
```

Expected:

```text
no visible pressure wall
no persistent gap
no runaway density error
```

### Mixed-level flow

Drive fluid through a region where initial resolution changes.

Expected:

```text
flow crosses boundary
no reflection wall
```

---

## Key implementation work

Correct:

```text
variable mass
variable volume
variable support radius
```

interactions.

Use the variable-resolution SPH literature as guidance; these methods explicitly handle unequal masses and smoothing lengths and demonstrate the need for carefully conservative formulations.

---

## Gate 3

Do not implement dynamic refinement until:

```text
static mixed resolution is numerically stable
```

and crossing a refinement boundary does not create severe:

```text
density spikes
vacuum bands
particle clumping
momentum discontinuities
```

---

# 22. Phase 4 — particle splitting

## Goal

Dynamically refine coarse particles.

Split:

```text
one level L particle
```

into:

```text
eight level L-1 particles
```

in 3D.

---

## Split state

For a parent:

```text
mass_p
volume_p
position_p
velocity_p
```

children receive:

\[
m_c = m_p/8
\]

\[
V_c = V_p/8
\]

Child positions use a symmetric cubic pattern around the parent.

The center of mass must remain exactly at the parent position.

---

## Child velocity — first implementation

Initially:

```text
v_child = v_parent
```

This exactly preserves momentum.

Do not attempt gradient reconstruction yet.

---

## Split conservation checks

For every split verify:

```text
Σ child mass     == parent mass
Σ child volume   == parent volume
Σ child momentum == parent momentum
center of mass   == parent position
material IDs     == parent material
```

Add GPU assertions/counters.

---

# 23. Split hysteresis

Never allow immediate:

```text
split → merge → split
```

oscillation.

Track:

```cpp
uint refinementAge;
```

or equivalent.

Require a configurable minimum time or number of timesteps before a particle may reverse its refinement direction.

Phantom's APR documentation similarly warns that repeated refinement events should be separated sufficiently to reduce noise.

---

# 24. Phase 4 refinement criteria

Initially use only geometric criteria.

Example:

```text
distance to player
```

or:

```text
inside explicit refinement volume
```

Do not begin with automatic physics-driven refinement.

This isolates split mechanics from refinement heuristics.

---

## Gate 4

Move a refinement volume through a settled tank.

Expected:

```text
particles split as volume approaches
simulation remains stable
total mass constant
no persistent pressure wave from splitting
```

Do not continue until split-induced energy injection is acceptably small.

---

# 25. Phase 5 — particle merging

## Goal

Recover coarse particles when high resolution is unnecessary.

Only merge:

```text
same MaterialId
same refinement level
spatially compatible particles
```

initially.

---

## Parent state

For children `i`:

\[
m_p = \sum_i m_i
\]

\[
x_p =
\frac{\sum_i m_i x_i}
{\sum_i m_i}
\]

\[
v_p =
\frac{\sum_i m_i v_i}
{\sum_i m_i}
\]

\[
V_p = \sum_i V_i
\]

This conserves:

```text
mass
linear momentum
center of mass
represented volume
```

but not necessarily:

```text
angular momentum
kinetic energy
fine-scale velocity variance
```

Track these losses.

---

# 26. Merge selection

Do not pick arbitrary nearest eight particles.

Prefer sibling-compatible spatial groups.

A simple approach:

```text
parentCell =
    floor(baseCoordinate / 2)
```

Group same-level particles sharing:

```text
parentCell
material
level
```

Merge only if enough compatible particles exist.

Do not create partial parents in the initial implementation.

---

## Gate 5

Move a refinement volume through fluid repeatedly.

Expected:

```text
coarse → fine → coarse
```

without significant drift in:

```text
mass
center of mass
bulk momentum
```

Measure energy loss separately.

---

# 27. Phase 6 — affine coarse-particle state

## Motivation

Naive merging destroys unresolved flow.

Suppose children contain:

```text
rotation
shear
expansion
```

Their average velocity cannot represent this.

Introduce an optional affine velocity field:

```cpp
struct AffineState
{
    float3x3 C;
};
```

such that locally:

\[
v(x)
=
v_p + C_p(x-x_p)
\]

This concept is closely related to APIC-style transfer representations.

---

## Merge

Fit `C` from child particle velocities.

## Split

Assign:

\[
v_c =
v_p + C_p(x_c-x_p)
\]

This allows a coarse particle to store unresolved first-order flow.

---

## Gate 6

Use:

```text
rigid rotation
linear shear
uniform expansion
```

as analytic tests.

Compare:

```text
split → merge → split
```

against original fine particles.

Require significantly lower velocity reconstruction error than Phase 5.

---

# 28. Phase 7 — automatic refinement criteria

Now introduce physics-driven adaptivity.

Compute a refinement score:

\[
E =
w_m E_m +
w_\rho E_\rho +
w_v E_v +
w_T E_T +
w_c E_c +
w_g E_g
\]

where terms may represent:

```text
material diversity
density gradient
velocity gradient
temperature gradient
collision proximity
gameplay importance
```

Do not enable every criterion simultaneously at first.

---

# 29. Material-interface refinement

For each particle, inspect its neighborhood.

Define:

```text
materialBoundary = true
```

if a significant fraction of neighbors use another material.

Increase refinement near:

```text
oil/water
water/air
sand/water
lava/rock
```

interfaces.

Adaptive multiphase SPH literature explicitly demonstrates refinement based on distance to moving phase interfaces.

---

# 30. Gradient refinement

Estimate:

```text
|∇density|
|∇velocity|
|∇temperature|
```

using local neighbors.

Refine if normalized gradient exceeds threshold.

Use hysteresis:

```text
splitThreshold > mergeThreshold
```

Example:

```text
split if E > 1.0
merge if E < 0.6
```

---

# 31. Refinement propagation

Before splitting a particle to level `L-1`, ensure neighboring particles do not violate the 2:1 rule.

If necessary:

```text
request refinement
```

on neighbors first.

Perform refinement planning in a separate pass:

```text
evaluate desired level
       |
       v
propagate level constraints
       |
       v
commit split/merge
```

Do not mutate topology while still computing desired levels.

---

# 32. Phase 8 — sleeping

## Goal

Make cost depend on active complexity.

A particle may sleep when:

```text
velocity below threshold
density error below threshold
temperature change below threshold
no nearby active particle
no nearby moving collider
no refinement change pending
```

Do not sleep particles individually initially.

Sleep spatial groups.

---

# 33. Sleep groups

Use coarse spatial blocks:

```text
SleepBrick
```

containing particle ranges or references.

A brick sleeps when all contained particles satisfy conditions.

Sleeping bricks:

```text
skip solver
skip sorting where possible
skip neighbor interactions
```

Wake when:

```text
active neighbor approaches
collider enters
gameplay event occurs
temperature/pressure disturbance arrives
refinement region approaches
```

---

## Gate 8

Create a large settled reservoir.

Expected:

```text
majority of particles eventually sleep
simulation cost decreases correspondingly
```

Wake one region with an impact.

Expected:

```text
disturbance propagates
neighbor regions wake
```

---

# 34. Phase 9 — gas

Gas must use the same particle storage.

Do not initially create a separate Eulerian gas solver.

Gas differences:

```text
low reference density
low bulk stiffness
high compressibility
low cohesion
```

The same local density interaction should therefore behave much more softly.

---

# 35. Gas refinement strategy

Gas is where adaptivity provides the largest potential benefit.

A homogeneous room may contain extremely coarse particles.

Refine around:

```text
doors
pressure gradients
fire
shock fronts
liquid surfaces
moving objects
vacuum boundaries
```

Merge aggressively when gas returns to homogeneous equilibrium.

---

## Validation

### Sealed room

Uniform gas.

Expected:

```text
coarsens aggressively
remains approximately uniform
```

### Door opening

Two chambers with different gas density/pressure.

Expected:

```text
particles refine near opening
flow occurs through opening
disturbance propagates
```

### Hot plume

Localized heating.

Expected eventually:

```text
hot gas expands/rises
refinement follows plume
```

Do not require accurate acoustic waves.

---

# 36. Critical gas risk

Very coarse particles fundamentally cannot represent short-wavelength pressure disturbances.

This is acceptable.

The refinement system must detect emerging gradients rapidly enough to resolve important disturbances.

Mitigation:

```text
predictive refinement radius
```

around active events.

For explosions/fire:

```text
refine before applying the event
```

rather than waiting for the coarse solver to detect it afterward.

---

# 37. Phase 10 — temperature

Add:

```cpp
float temperature;
float internalEnergy;
```

or one equivalent energy representation.

Prefer energy-conserving exchange.

Pairwise heat exchange:

\[
\Delta E_{ij}
=
k_{ij}(T_j-T_i)dt
\]

Apply symmetrically:

```text
E_i += ΔE
E_j -= ΔE
```

so total thermal energy remains conserved.

---

# 38. Temperature and refinement

Refine near:

```text
large temperature gradients
phase boundaries
reactions
```

Homogeneous hot or cold regions can remain coarse.

---

# 39. Phase 11 — phase changes

Phase changes should alter material identity or material state while preserving:

```text
mass
momentum
energy where modeled
```

Example:

```text
water
    |
    +-- freeze --> ice
    |
    +-- boil --> steam
```

Do not spawn arbitrary numbers of particles solely because phase changed.

Instead:

```text
change MaterialId
```

and let adaptive refinement subsequently adjust resolution.

---

# 40. Phase-change volume changes

When changing:

```text
liquid → gas
```

the preferred density changes dramatically.

Therefore the same mass corresponds to a much larger equilibrium volume.

Do not instantaneously enlarge particle radius by enormous factors.

Instead:

1. change material;
2. change target/reference density;
3. allow volume/pressure dynamics to expand material;
4. trigger refinement if local gradients grow.

This avoids catastrophic topology discontinuities.

---

# 41. Phase 12 — granular matter

Start with simple granular behavior.

Use the common density/compression solver plus:

```text
friction
velocity damping under contact
limited tensile cohesion
```

Do not implement Drucker-Prager or full MPM plasticity initially.

The goal is qualitative:

```text
sand piles
flows under gravity
displaces liquid
settles
```

Unified Particle Physics provides precedent for using constraints rather than one constitutive PDE for every simulated material class.

---

# 42. Granular refinement

Refine around:

```text
free surface
liquid interface
moving obstacles
active flow
```

Allow settled bulk sand to coarsen.

This is potentially one of the highest-value use cases for adaptive particles.

---

# 43. Phase 13 — coherent/solid matter

Do not attempt fully deformable solids first.

Begin with particle clusters.

A solid object consists of particles sharing:

```text
clusterId
```

with a shape-preservation constraint.

Possible progression:

```text
rigid shape matching
       ↓
soft shape matching
       ↓
strain-based constraints
       ↓
optional continuum solid model
```

The unified particle work demonstrated rigid and deformable material using constraint-based particle representations.

---

# 44. Solid split/merge warning

Solid particles retain mechanical history.

Merging them is substantially harder than merging gas/liquid parcels.

Therefore:

```text
DO NOT enable automatic solid coarsening
```

until a dedicated deformation-preserving scheme exists.

Initial policy:

```text
gas       split + merge
liquid    split + merge
granular  split + merge cautiously
solid     split only or fixed resolution
```

---

# 45. Phase 14 — chemistry

Do not represent composition as:

```cpp
float species[MAX_SPECIES];
```

per particle.

This defeats the sparse-material architecture.

Initially keep particles pure:

```text
one MaterialId per particle
```

Local mixtures are represented by neighboring particles.

---

# 46. Reactions

Define reaction rules:

```cpp
Reaction
{
    MaterialId A;
    MaterialId B;

    MaterialId products[];
    float rate;
    float activationTemperature;
    float energy;
};
```

Search neighboring unlike particles.

Reaction consumes mass from reactants and creates/transforms product particles.

Always enforce explicit mass accounting.

---

# 47. Reaction resolution policy

Chemical reactions usually require finer spatial resolution.

Therefore:

```text
reaction candidate
    ↓
request refinement
    ↓
perform reaction after refinement
```

unless reaction is intentionally modeled at coarse resolution.

This prevents one huge particle from reacting instantaneously with an entire tiny neighboring parcel.

---

# 48. Optional future mixed-composition particles

Only introduce composition-bearing particles if particle counts from miscible substances become unacceptable.

Possible structure:

```cpp
struct Composition
{
    MaterialId a;
    MaterialId b;
    uint16 fraction;
};
```

Do not introduce arbitrary-length composition arrays.

This is explicitly deferred.

---

# 49. Temporal adaptivity

Spatial refinement alone is insufficient for a large world.

Later introduce simulation-rate classes.

Example:

```text
Rate 0: every frame
Rate 1: every 2 frames
Rate 2: every 4 frames
Rate 3: every 8 frames
Sleeping: never
```

GPU adaptive SPH work has demonstrated combining particle refinement with multiple time stepping.

---

# 50. Time-step safety rule

Particles interacting strongly must not run at incompatible time rates without synchronization.

Require:

```text
neighbor rate difference <= one level
```

analogous to spatial refinement.

When a fast particle approaches a slow region:

```text
promote neighboring region temporally
```

before strong interaction occurs.

---

# 51. GPU scheduling model

Target eventually:

```text
Pass 0  process gameplay events
Pass 1  wake regions
Pass 2  evaluate refinement
Pass 3  propagate allowed levels
Pass 4  split/merge
Pass 5  compute spatial keys
Pass 6  sort particles
Pass 7  build cell ranges
Pass 8  predict positions
Pass 9  neighbor properties
Pass 10 solve constraints × N
Pass 11 viscosity/material interaction
Pass 12 collisions
Pass 13 integrate velocity/position
Pass 14 thermal/reactions
Pass 15 update sleeping state
Pass 16 diagnostics
```

Not every pass must run every frame.

---

# 52. Solver iterations

Use a small fixed budget initially:

```text
2 iterations
```

Benchmark:

```text
1
2
4
```

Do not design assuming convergence to a fully incompressible solution.

Artificial softness/compressibility is explicitly acceptable.

---

# 53. Constraint LOD

Iteration count may eventually become another LOD dimension.

Potential policy:

```text
critical nearby region   3–4 iterations
normal active region     2
background region        1
sleeping region          0
```

If constraint stiffness varies too much with iteration count, migrate the relevant constraints toward XPBD-style compliance. XPBD was designed specifically to reduce dependence of effective stiffness on timestep and iteration count.

---

# 54. Rendering must be decoupled

Do not make physical particle resolution equal rendering resolution.

Render liquids using:

```text
surface reconstruction
screen-space splatting
sparse density field
```

Render gas using:

```text
coarse volumetric density reconstruction
```

Render granular matter directly or through instancing.

The simulation may contain extremely coarse particles that should never become visibly giant blobs.

---

# 55. GPU diagnostics

Maintain these counters every frame:

```text
particleCount
particleCountByLevel[]
particleCountByMaterial[]

activeParticles
sleepingParticles

splitCount
mergeCount

neighborPairCount
maxNeighbors
averageNeighbors

massTotal
massByMaterial[]

momentumTotal

kineticEnergy
thermalEnergy

nanCount
invalidParticleCount

densityErrorMean
densityErrorMax

refinementBoundaryCount
```

Expose them to a debug UI.

---

# 56. Performance telemetry

Timestamp every major compute pass:

```text
sort
neighbor build
density
constraint iteration
material interactions
collisions
split
merge
sleep
thermal
```

Never optimize using whole-frame timing alone.

---

# 57. Target scaling experiment

Construct a scene containing a large reservoir.

Compare:

```text
uniform L0
uniform L1
adaptive L0/L1/L2/L3
```

Measure:

```text
particle count
neighbor pair count
GPU milliseconds
mass error
density error
visual error
```

The adaptive framework is justified only if coarse homogeneous regions substantially reduce total simulation cost.

---

# 58. Critical proof-of-concept experiment

Before implementing solids or chemistry, create this scenario:

```text
large sealed room

contains:
    air
    water reservoir
    oil layer

plus:
    hot object
    movable obstacle
```

Start with coarse matter.

Trigger:

```text
door opening
object entering water
local heating
```

Expected behavior:

```text
refinement follows interfaces and disturbances;
oil/water stratification survives;
air responds to door opening;
coarse distant regions remain cheap;
refined regions eventually merge again.
```

If this scenario fails fundamentally, reconsider the architecture before adding more systems.

---

# 59. Risk register

## Risk R1 — variable-resolution pressure artifacts

### Symptom

```text
density spike
pressure wall
gap
reflection
```

where levels meet.

### Cause

Different masses/support radii generate inconsistent density estimates.

### Mitigation

- symmetric kernels;
- 2:1 refinement rule;
- transition layers;
- variable-resolution SPH formulations;
- particle shifting if required.

Variable-resolution SPH literature should be the primary reference.

---

## Risk R2 — split energy injection

### Symptom

Fluid starts vibrating after refinement.

### Mitigation

- symmetric child placement;
- conserve center of mass;
- preserve momentum;
- refinement hysteresis;
- later introduce affine velocity reconstruction.

---

## Risk R3 — merge destroys flow

### Symptom

Turbulence/rotation disappears when regions coarsen.

### Mitigation

- delay merging in high velocity-gradient regions;
- retain affine velocity moments;
- include velocity variance in refinement score.

---

## Risk R4 — excessive neighbor count near coarse particles

Large particles have large support radii and may overlap many fine particles.

### Mitigation

- enforce 2:1 refinement;
- aggressively refine around coarse/fine interaction boundaries;
- cap refinement-level difference;
- measure pair count, not just particle count.

This risk can erase the theoretical benefit of coarse particles if ignored.

---

## Risk R5 — adaptivity costs more than it saves

### Mitigation

Track:

```text
GPU cost of refinement
GPU cost saved through reduced interactions
```

Split/merge should happen much less frequently than ordinary simulation.

Do not repartition aggressively every frame.

---

## Risk R6 — large gas particles miss events

### Mitigation

Gameplay systems must be allowed to request refinement explicitly.

Examples:

```text
explosion
door opening
fire ignition
projectile
fast rigid body
```

Refinement should precede application of high-frequency disturbances.

---

## Risk R7 — gas pressure propagation too slow

Local particle pressure solvers propagate information gradually.

### Acceptable initial result

Gas is compressible and pressure waves are approximate.

### Future mitigation

Consider:

```text
coarse auxiliary pressure field
```

only if gameplay requires long-range rapid pressure propagation.

Do not add it prematurely.

---

## Risk R8 — immiscible fluids overmix

### Mitigation progression

1. test base density solver;
2. add material-affinity/segregation pair force;
3. refine interface more strongly;
4. only if necessary, introduce separate mechanical phases.

Do not immediately introduce per-material velocity grids.

---

## Risk R9 — sorting dominates GPU time

### Mitigation

Investigate:

```text
incremental spatial ordering
cell-local reorder
Morton ordering
persistent brick membership
```

only after measuring the baseline.

Particles generally move locally, so full global radix sorting may eventually be unnecessary every frame.

---

## Risk R10 — adaptive hierarchy thrashing

### Mitigation

Use:

```text
splitThreshold > mergeThreshold
minimum refinement age
minimum coarsening age
spatial smoothing of desired levels
```

---

## Risk R11 — coarse particles create obvious visual LOD

### Mitigation

Simulation particles are not rendering primitives.

Reconstruct visual fields independently.

---

## Risk R12 — unified solver becomes phase-specific spaghetti

### Mitigation

Keep architecture layered:

```text
particle substrate
neighbor system
common state
constraint modules
material table
reaction modules
```

Do not embed:

```text
if water
if oil
if air
```

throughout solver kernels.

Dispatch behavior using material parameters and constraint capabilities.

---

# 60. Architecture modules

Suggested code structure:

```text
simulation/
    ParticlePool
    ParticleAllocator

    SpatialIndex/
        MortonKey
        ParticleSort
        CellRange
        NeighborIterator

    Resolution/
        RefinementScore
        LevelPropagation
        ParticleSplit
        ParticleMerge
        ResolutionValidation

    Solver/
        Density
        PressureConstraint
        Viscosity
        Collision
        Integrator

    Material/
        MaterialDatabase
        MaterialInteraction
        PhaseModel

    Thermal/
        HeatTransfer
        PhaseChange

    Chemistry/
        ReactionDatabase
        ReactionSolver

    Solid/
        ShapeMatching
        Deformation

    Scheduling/
        SleepManager
        TimeRateManager

    Debug/
        SimulationCounters
        ConservationCheck
        GPUProfiler
```

Avoid circular dependencies.

---

# 61. Recommended development order

Implement strictly in this order:

```text
0 GPU particle infrastructure
      ↓
1 fixed-resolution water
      ↓
2 multiple liquids
      ↓
3 static mixed resolution
      ↓
4 dynamic splitting
      ↓
5 dynamic merging
      ↓
6 affine coarse state
      ↓
7 physics-driven refinement
      ↓
8 sleeping
      ↓
9 gas
      ↓
10 temperature
      ↓
11 phase transitions
      ↓
12 granular material
      ↓
13 solids
      ↓
14 chemistry
      ↓
15 temporal adaptivity
```

Do not reorder:

```text
gas before adaptive liquid
solid before stable variable resolution
chemistry before material conservation
```

because failures will become impossible to isolate.

---

# 62. Explicit non-goals for version 1

Do **not** implement:

- strict incompressibility;
- pressure Poisson solves;
- PB-MPM;
- dense world grids;
- fully physical multiphase CFD;
- Navier-Stokes validation-grade behavior;
- arbitrary continuous particle radii;
- arbitrary per-particle mixtures;
- fully deformable solids;
- accurate acoustics;
- accurate turbulence;
- physically correct surface tension;
- conservation of fine-scale kinetic energy through merges;
- molecular diffusion.

These can be introduced later if required.

---

# 63. Success criteria for the framework

The architecture should be considered successful if a test world can contain:

```text
air
water
oil
methanol
sand
solid obstacles
temperature
```

with:

```text
dynamic interaction
density-driven stratification
local mixtures
phase-independent collision handling
dynamic split/merge
sleeping
```

while coarse homogeneous regions use substantially fewer particles than interfaces and active regions.

The strongest architectural proof is not visual water quality.

It is this:

> Increasing world size while leaving the amount of active material complexity approximately constant should increase simulation cost slowly.

That is the reason for building the adaptive system.

---

# 64. Decision checkpoints

At the end of each major milestone, explicitly decide whether to continue.

## Checkpoint A — after Phase 3

Question:

> Can mixed particle resolutions interact without unacceptable pressure artifacts?

If **no**, investigate variable-resolution SPH formulations before proceeding.

---

## Checkpoint B — after Phase 5

Question:

> Does split/merge actually reduce total pair interactions enough to offset topology-management cost?

If **no**, the adaptive architecture is not justified.

---

## Checkpoint C — after Phase 7

Question:

> Can refinement follow interfaces without thrashing?

If **no**, simplify the refinement metric and increase hysteresis.

---

## Checkpoint D — after Phase 9

Question:

> Can coarse particle gas provide useful gameplay-level gas dynamics?

If **no**, introduce a separate coarse gas-pressure field while keeping material mass particle-based.

Do not abandon particles automatically.

---

## Checkpoint E — before solids

Question:

> Does the unified representation still provide implementation simplicity?

If introducing solids would force excessive per-particle state for all materials, split storage into specialized SoA state sets.

Unified representation does not imply identical memory layout.

---

# 65. Important proposed optimization: state specialization

Eventually use:

```text
CommonParticle
    position
    velocity
    mass
    volume
    material
    level
```

plus optional state indexed only by relevant particles:

```text
FluidState
ThermalState
AffineState
SolidState
ReactionState
```

Do not make every air particle carry:

```text
deformation gradient
plasticity tensor
solid cluster data
```

The conceptual model is unified.

The physical storage does not need to be.

---

# 66. Research-backed principles to retain

The following principles are well supported by prior work:

### Particle-based unified interactions

Unified Particle Physics demonstrated real-time two-way interaction between multiple kinds of particle-represented matter.

### Position constraints for robust real-time fluid behavior

Position Based Fluids demonstrated density constraints suitable for comparatively large timesteps in real-time contexts.

### Compliance rather than raw stiffness

XPBD provides a principled path if constraint behavior becomes excessively dependent on timestep/iteration count.

### Dynamic particle split/merge

Adaptive SPH has repeatedly demonstrated dynamic coarsening/refinement while conserving mass and momentum.

### GPU adaptivity

GPU particle splitting and merging is demonstrated and can be combined with multiple time stepping.

### Interface-driven adaptivity

Multiphase adaptive SPH has demonstrated refining according to distance from material interfaces.

### Practical modern APR

Phantom currently contains usable adaptive particle refinement machinery and documents practical restrictions that should inform this implementation.

---

# 67. Speculative parts that require validation

The following are architectural proposals rather than established results:

- one adaptive particle solver spanning gas, liquid and granular gameplay simulation;
- level-based octree-like particle parcels without an explicit octree;
- refinement driven jointly by material interfaces, physics gradients and gameplay interest;
- very coarse gas parcels representing large homogeneous volumes;
- affine moments stored preferentially by coarse particles;
- combining PBD-like density constraints with aggressive variable particle masses;
- large-scale simulation where particle resolution itself acts as world simulation LOD;
- sparse pure-material particles as the primary representation of arbitrary mixtures.

Agents must not describe these features as proven until validated experimentally.

---

# 68. Reference implementations and reading order

## 1. Unified Particle Physics / FleX

Read first to understand the unified-particle philosophy.

Miles Macklin, Matthias Müller, Nuttapong Chentanez, Tae-Yong Kim, **Unified Particle Physics for Real-Time Applications**, SIGGRAPH 2014.

NVIDIA FleX SDK overview. FleX is now legacy but remains useful as an implementation reference for unified constraints.

---

## 2. Position Based Fluids

Read for the baseline density-constraint formulation.

Miles Macklin, Matthias Müller, **Position Based Fluids**, SIGGRAPH 2013.

---

## 3. XPBD

Read before exposing material stiffness/compliance as gameplay parameters.

Miles Macklin, Matthias Müller, Nuttapong Chentanez, **XPBD: Position-Based Simulation of Compliant Constrained Dynamics**, MIG 2016.

---

## 4. Variable-resolution SPH

Primary reference for unequal particle masses and split/merge.

Renato Vacondio et al., **Variable Resolution for SPH: A Dynamic Particle Coalescing and Splitting Scheme**, 2013.

Follow with the 3D extension:

**Variable Resolution for SPH in Three Dimensions: Towards Optimal Splitting and Coalescing for Dynamic Adaptivity.**

---

## 5. GPU adaptive particles

Qingang Xiong, Bo Li, Ji Xu, **GPU-Accelerated Adaptive Particle Splitting and Merging in SPH**, 2013.

This is particularly relevant when designing GPU split/merge compaction and scheduling.

---

## 6. Adaptive multiphase resolution

**Adaptive Resolution for Multiphase Smoothed Particle Hydrodynamics**, 2019.

Relevant specifically to interface-driven refinement.

---

## 7. Phantom APR

Use Phantom as a modern practical reference for adaptive particle refinement rules, split/merge lifecycle and known operational constraints.

---

## 8. PB-MPM

Do not use PB-MPM as the initial solver, but keep it as a reference for:

```text
robust game-oriented material simulation
constraint formulation
material behavior
```

Chris Lewin, **A Position Based Material Point Method**, SIGGRAPH 2024.

EA provides an open-source WebGPU implementation; its `SIGGRAPH 2024` branch is specifically described as easier to read than the optimized main branch.

---

# 69. Final architecture summary

The target system is:

```text
                         WORLD
                           |
                           v
                  Unified Material Parcels
                           |
        +------------------+------------------+
        |                  |                  |
       Gas               Liquid            Granular
        |                  |                  |
        +------------------+------------------+
                           |
                   Shared particle state
                           |
       position / velocity / mass / volume / material
                           |
                           v
                 Hierarchical resolution
                           |
                 L0 L1 L2 L3 ...
                           |
              +------------+------------+
              |                         |
           refine                     merge
              |                         |
      interface / gradient       homogeneous bulk
      collision / gameplay       low-gradient region
              |                         |
              +------------+------------+
                           |
                           v
                Hierarchical neighbor grid
                           |
                           v
                Local symmetric constraints
                           |
         +-----------------+----------------+
         |                 |                |
      pressure          viscosity         collision
         |                 |                |
         +-----------------+----------------+
                           |
                           v
                     integration
                           |
             +-------------+-------------+
             |                           |
          thermal                    chemistry
             |
         phase change
                           |
                           v
                    sleep / wake / LOD
```

The central architectural rule is:

> **Particles store conserved matter and material identity. Resolution determines how much matter each particle represents. Local constraints determine material behavior. Refinement follows complexity rather than world size.**

Do not optimize initially for beautiful water.

Optimize for proving these four properties:

```text
1. conservation survives split/merge;

2. particles of different resolutions interact stably;

3. multiple materials coexist naturally without fixed mixture storage;

4. computational cost follows active complexity rather than total simulated volume.
```

If those four properties hold, progressively add gas, temperature, granular matter, solids and chemistry.

If they do not hold, stop at the relevant phase gate and fix the underlying representation before adding additional physics.