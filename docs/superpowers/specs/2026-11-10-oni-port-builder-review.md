# Oni port: initial independent builder review

Date: 2026-11-10
Status: consequential gaps identified; **not a design approval**.

Scope: read-only review of the fresh design and relevant source. No production
edits, builds, captures or numerical/visual acceptance were performed. Historical
plans and their gates were not treated as authority.

## Reference-fidelity questions

### 1. Harmonic packing loses coefficient signs

`CircularHarmonics.Streamer.resolve` produces signed AC coefficients in
`docs/oni_v22.slang:6095-6099`. The active fine-probe path stores these using
`Packing.packRadiance9`, whose quantization clamps each scaled coefficient to the
unsigned range in `:6600`; unpacking does not restore a sign.

This is a source-level finding. Its effect on the owner's expected images has not
been evaluated. A sign-preserving format would change prototype behavior and
requires a specific design decision; it is not an authorized optimization.

### 2. Fine connector hit shading uses the raw cascade-zero provider

Coarse passes use `RadianceMultiBounce`, backed by fine `SuperSampling`
(`:9561-9567`). The fine-update entry point instead supplies
`CascadeRadiance<Radiance::Cascade>` (`:9593-9595`). That provider's irradiance
samples `Cascade(0)` (`:6486-6531`), while the ordinary level-zero dispatch is
disabled in the active supersampling configuration (`:9579-9586`).

Inference: in the shown active dispatch path, fine connector hits do not obtain
an updated level-zero indirect contribution. This may be an intentional
direct-only near-hit policy, rather than a bug. Far cascade radiance can still
carry indirect illumination. Do not conclude that all multibounce is absent.

Changing the provider, enabling L0, or sampling a previous fine field changes
feedback dependencies and may require additional history storage. The future
implementer must not choose among these silently.

### 3. Direction lookup uses current global frame, including retained samples

`probeDir(probe, frame, phi)` ignores the passed frame and rotates directions
using global `Time.frame` (`:6205-6216`). Coarse scheduling retains three probes
out of each 2 x 2 group, while fine reconstruction reads their values and calls
that direction function (`:6805-6828`).

Inference: retained values can be evaluated with a direction different from the
one used to trace them. Visual consequences are unmeasured. Preserving literal
behavior versus keeping trace-direction identity is a design choice affecting
scheduling/metadata and connector interpretation.

## Other unresolved contracts

1. Exact PBR scalar material mapping and helper/LUT reuse; visible transparent
   surface composition. The owner requires standard on-screen PBR and minimal
   transport data, but has not approved a new optical/refraction subsystem.
2. Gas presentation scope. Preserve gas simulation; solid/liquid grid projection
   approval does not by itself authorize deleting gas presentation or adding
   volumetric gas transport.
3. Final display inventory and whether Hable has a user-visible off state. One
   algorithm is approved; that alone does not settle enable/disable behavior.
4. Exact map parameters, arbitrary-extent mapping and display/profiling automation
   details already identified in the design must be specified before final handoff.

## Disposition

The coordinator must resolve reference fidelity with the owner before specifying
final field storage and feedback ownership. Do not fix these findings in the
reference file or current engine during this documentation session. The supplied
reference may contain intentional approximations or unfinished experiments; ask
instead of treating source inspection as authority to redesign it.

Retired performance gates stay retired. These questions concern fidelity,
ownership and required behavior, not numeric visual-acceptance thresholds.
