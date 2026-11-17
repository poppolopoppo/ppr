# Element binding and calibration (T1; corrected S1)

## Representation and reachability

Terrain-represented solid elements remain static terrain cells, outside MPM.
Destroying/removing a solid through the terrain-edit adapter produces loose MPM
particles. The particle tool still requires `representation = particle` and phase
`solid` or `liquid`; terrain solids and gases return `invalid_argument`. Capacity,
ID, count and other admission checks are retained. The temporary spawn relaxation
was reverted. **There is no bedrock spawn allowance.** Destruction still obeys the
owning tool's eligibility rules (for example, protected/non-diggable terrain is not
made diggable by a constitutive binding).

Bindings use the selected catalog material key. The following table covers the
shipped real materials; grouped names have identical family/spawnability policy.
“Yes” denotes material eligibility, not a bypass of occupancy/capacity checks.

| Element/material keys | MPM family on conversion | Tool spawnable | Route |
|---|---|---|---|
| water, polluted_water, brine, crude_oil, magma | liquid | Yes | Liquid inventory/seeding or tool |
| sand, regolith, ash, pebbles | sand | Yes | Loose inventory/tool |
| sandstone, basalt, granite, limestone, coal, dirt, salt | sand | No | Destroy terrain |
| copper_ore, iron_ore, uranium_ore, wolframite, autunite, azurite | sand | No | Destroy terrain |
| glass, obsidian, clear_quartz, crystal, diamond, emerald, ruby, ice, polluted_ice | elastic | No | Destroy terrain |
| slime | visco | No | Destroy terrain |
| clay | cohesive | No | Destroy terrain |
| snow | compacting | Yes | Loose inventory/tool; terrain-edit conversion also supported |
| pumice | compacting | No | Destroy terrain |
| algae, aluminum, anhydrite, bedrock, breccia, calcite, chalk, conglomerate, copper, dolomite, fluorite, gneiss, gypsum, iron, laterite, masonry, phosphorite, rust, shale, sulfur, tuff | sand (previous fallback) | No | Destroy eligible terrain; protected bedrock stays protected |
| carbon_dioxide, chlorine, hydrogen, natural_gas, oxygen, polluted_oxygen, steam | Unsimulated gas; no MPM row | No | Existing gas inventory only |

Additional bindings: `wet_sand` → cohesive; `gravel` → sand; `oil` → liquid.
These keys are not present in the shipped default palette. Their spawnability
depends on an authored particle representation and solid/liquid phase; they are
not silently added to a catalog. `oil` retains previous coefficient defaults;
the tuned shipped oil is `crude_oil`.

Lighting-lab palette audit (`assets/worldgen/default/worlds/asset-lighting_lab.toml`):
`lab_magma` is the only `lab_*` liquid. It explicitly resolves to magma calibration,
with its own substance ID and catalog density (2800 kg/m³). The comparison liquid
is ordinary `water` (1000 kg/m³); there is no `lab_water` alias. Both are spawnable
liquids. `lab_glass`, `lab_hot_metal`, `lab_wall`, `lab_red`, `lab_green`,
`lab_emitter`, `lab_pbr_0`–`lab_pbr_3` are unmatched terrain material keys and retain
the previous sand/default binding. The unmatched `showcase_*` terrain materials
also retain the previous sand/default binding. No generic `lab_` prefix stripping
is performed.

Every MPM family is reachable: liquid via water, sand via destroyed sandstone,
elastic via destroyed obsidian, visco via destroyed slime, cohesive via destroyed
clay, compacting via destroyed pumice (and loose snow). Liquids are not turned
into dug solids: the existing terrain-edit adapter only emits solid terrain.

## Coefficients

**All numbers below are calibration, not measured physical constants.** Friction
is in degrees; other constitutive coefficients are dimensionless. Density always
comes from the catalog, stored as `rho / 1000 kg/m³`; particle mass follows the
catalog density and emitted volume. No density multiplier is added to pressure.

| Liquid | Viscosity | Relaxation | Catalog density kg/m³ |
|---|---:|---:|---:|
| water | 0.01 | 1.0 | 1000 |
| polluted_water | 0.02 | 1.0 | 1030 |
| brine | 0.03 | 1.0 | 1200 |
| crude_oil | 0.25 | 0.8 | 850 |
| magma, lab_magma | 2.0 | 0.5 | 2800 |

| Sand binding | Friction degrees | Yield ratio |
|---|---:|---:|
| sand | 30 | 1.0 |
| sandstone | 34 | 1.1 |
| basalt, granite | 36 | 1.15 |
| limestone | 33 | 1.05 |
| coal | 28 | 0.9 |
| dirt | 32 | 1.0 |
| regolith | 30 | 1.0 |
| ash | 26 | 0.85 |
| pebbles | 38 | 1.25 |
| salt | 30 | 1.0 |
| copper_ore, iron_ore, uranium_ore, wolframite, autunite, azurite | 34 | 1.1 |
| gravel (retained prior special case; absent from shipped palette) | 38 | 1.25 |

| Family / elements | Overrides |
|---|---|
| elastic: glass, obsidian, clear_quartz, crystal, diamond, emerald, ruby, ice, polluted_ice | Elastic blend 1.0; elastic relaxation 1.5 |
| visco: slime | Elastic blend 1.0; elastic relaxation 1.5; plasticity 0.9 |
| cohesive: clay | Cohesion 0.12; cap depth 0.5; cohesion hardening 0.4 |
| cohesive: wet_sand (absent from shipped palette) | Cohesion 0.06; cap depth 0.4; cohesion hardening 0.6 |
| compacting: snow | Initial solid fraction 0.3 |
| compacting: pumice | Initial solid fraction 0.4 |

“Hardening” in the cohesive ruling binds to `m_cohesion_hardening`;
`m_cap_hardening` remains the previous 0.5. Remaining cohesive fields are
**placeholder calibration**: tension 0.02, cap hardening 0.5, initial solid fraction
0.3, compaction rate 0.5, solid exponent 2.0, dense epsilon 0.01.
Remaining compacting fields are **placeholder calibration**: cohesion 0.05,
cap depth 0.2, tension 0.02, cohesion hardening 0.2, cap hardening 0.5,
compaction rate 0.5, solid exponent 2.0, dense epsilon 0.01.

All other fields retain the previous global-profile values: elastic blend 1.0,
elastic relaxation 1.5, liquid relaxation 1.0, liquid viscosity 0.01, friction 30,
plasticity 0, sand yield ratio equal to profile elastic blend (normally 1.0).
These are **placeholder/default calibration**, not newly calibrated values.
For a nondefault world profile, untouched fields preserve that profile's actual
values. Unmatched materials retain all prior defaults. Existing saved rows and
v6/v7 migration defaults are not retuned. Fresh conversion/seeding/tool particles
use the same row factory, including pumice's initial fraction; restored/evolved
history is never reinitialized.

Admission inspection: friction lies in [0,90); every requested scalar is finite;
cohesion/hardening are nonnegative; cap depths are positive; phi0 is in
(0,1-epsilon] with epsilon 0.01. Viscosity/relaxation admission requires finiteness,
not a [0,1] bound, so magma viscosity 2.0 is admitted. **No requested coefficient
was found outside the existing admission ranges.** Runtime admission is still
checked by the existing material-table validator.

## Expected lighting-lab observation and evidence status

On a new world/newly constructed row, water and lab magma now have distinct
viscosity, relaxation and inventory density. Under matching disturbance/load,
the intended calibration is quicker-moving water and more sluggish magma, with
different compression/late pool shape and a denser magma inventory. Oil should
also move differently from water. This is an expectation to verify, not a claimed
visual observation. An already saved table keeps its saved coefficients.

Authored tests (not executed in this edit-only task):

| Test | Evidence it will collect/assert |
|---|---|
| `game/checkpoint/destroyed_solids_use_calibrated_families_and_initial_history` | Actual terrain-edit conversion of sandstone, obsidian, slime, clay, snow and pumice; family, tuned fields, density, initial logJp/compaction/phi; snapshot reseeding including pumice phi=0.4 |
| `game/checkpoint_gpu/particle_tool_initializes_material_families_and_preserves_restored_history` | Real tool liquid/loose spawn initialization; saved-history preservation; terrain obsidian/slime/pumice/bedrock and gas rejected with `invalid_argument`, no ID consumption |
| `game/checkpoint_gpu/shipped_liquids_have_distinct_loaded_gpu_responses` | Shipped lighting-lab catalog; lab_magma and magma rows identical except substance ID, distinct from water; GPU water/refined-water/magma/oil under identical 8×8 initial geometry, gravity and K5 schedule |

The GPU response test records late mean height and speed (last 30 of 240 ticks),
early mean speed (first 30), normalized volume/compression state J (the historical
`liquidDensity` field), and density proxy rho0/J. Each tick has eight substeps.
It asserts magma's early motion is slower, late height/speed and compression
differ, and its density proxy is higher; oil must differ in early motion and
compression. Differences must exceed max(1e-4, eight times the water transfer
precision sensitivity) per observable. The second water run doubles the
fixed-point multiplier from 10,000,000 to 20,000,000 with identical load and
coefficients; a deterministic same-precision repeat would not expose transfer
quantization. The 1e-4 floor is an **authored provisional noise tolerance**;
the precision-sensitivity measurements have not yet been collected. The late window is an
authored settling observation window, not a demonstrated equilibrium. Failed
direction/threshold or stability assertions must be investigated, not reported
as successful measurements or silently weakened. Numerical results, actual
settling, visual confirmation, and GPU timing remain pending validation.
