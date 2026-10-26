# PB-MPM

Upstream: https://github.com/electronicarts/pbmpm

Pinned revision: `5a2f438b7115741c41ed429a2a0360d0e5f4b5a9` (v1.0.0). This is a
reference-source pin, not a build dependency: PPR does not fetch, build, or link
the WebGPU implementation. The pin records the exact source revision whose
WGSL solver is directly ported to Slang by `engine.matter`.

Reference: Chris Lewin, *A Position Based Material Point Method*, ACM SIGGRAPH
2024, DOI `10.1145/3641233.3664323`.

`LICENSE` preserves the upstream BSD-3-Clause notice at this revision
(1504 bytes, LF; SHA-256
`7bb46d23e3920a130d94a23a5a134a870ba6087b76802e91a9fd61ee673fd5f3`). Keep this
notice with redistributed software.

`engine.matter` now directly ports the pinned two-dimensional PB-MPM compute
stages into `assets/shaders/pbmpm/`: all source liquid, elastic, sand and visco
branches, F/SVD/plasticity, integer transfer, volume feedback, rotated box/circle
contact and integration safeguards. Original copyright and full BSD-3-Clause
terms are retained beside those redistributed assets. Engine scalar ABI,
checked integer operations and explicit empty-mass handling are documented
adaptations, without a GPU bitwise parity claim. Browser-only mouse, emitter
and drain controls are excluded from the admitted external worldgen/dig runtime.
The prior CPU reference is preserved in task evidence and removed from compiled
registration; it is not a second backend. The game owns separately documented
terrain/body/unit adapters; no upstream type crosses a PPR module boundary.
