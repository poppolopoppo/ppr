# Away Team — Gameplay Loop + Mechanics + Inventory (Clone Spec)

> Title correction: **Away Team** (Steam 2168390), Klei. Not "Away-Team" / "AwayTeam".
> Genre: 2D side-scroller, single directly-controlled duplicant platformer.
> Closed playtest: Apr 2026, ~50-cycle cap [INFERRED-PT] (proof #1).
> Source of truth: playtest footage + ONI baseline adapted.
>
> **Evidence legend (unified, used throughout this doc):**
> - `[OBSERVED]` = seen in footage or store page.
> - `[INFERRED-PT]` = playtest inference (partial footage, needs proof-checklist item).
> - `[TUNING-UNKNOWN]` = threshold / ratio / rate unconfirmed; value is a clone placeholder.
> - `[DECISION]` = clone choice (not observed; decided for MVP).
> - `[ONI-REF]` = reused ONI baseline (not re-derived here).
> - `[PPR-FACT]` = verified in PPR source / codemaps.

---

## 1. Premise / Scope

| Field | Value |
|---|---|
| Title | Away Team `[OBSERVED]` |
| Steam ID | 2168390 `[OBSERVED]` |
| Genre | 2D side-scroller platformer-survival colony-lite `[OBSERVED]` |
| Control model | Single directly-controlled duplicant (jump/climb/ladders), pause + free cam `[OBSERVED]` |
| Core sim | Full gas/liquid/heat tile sim (ONI-derived), single-element-per-cell displacement `[OBSERVED]` sim exists / `[DECISION]` cell model |
| Session scope | Land and survey → survive → automate → imperatives; playtest cap ~50 cycles `[INFERRED-PT]` (proof #1) |
| Win/fail | Mission imperatives = scored quotas (kill/collect/sustain counts against a cycle deadline) `[DECISION]`; imperative UI not yet implemented in playtest `[INFERRED-PT]` (proof #7) |
| Multi-dupe mgmt | Absent — one avatar only; no printing, no jobs board, no multi-select `[DECISION]` (see §6 Cut List) |
| Char select | Ren selectable skin; cosmetic-only (no stat difference) `[OBSERVED]` skin present / `[INFERRED-PT]` cosmetic-only (proof #9) |
| Single-avatar loop (canonical MVP) | Research + farm/cook + kcal + battery-to-hub power are ALL required for MVP loop closure `[DECISION]` (reconciles Architecture MVP-1/MVP-2 phasing: MVP-1 "Breathe" and MVP-2 "Plumb and Burn" are delivery phases; gameplay MVP requires the closed loop across both) |

### Scope boundary

| In scope (clone MVP) | Out of scope (see §6 Cut List, no stubs in MVP tables) |
|---|---|
| 1 avatar, platformer controller | Multiple dupes, printing pod, jobs/skills tiers |
| Vacuum-gun gather, dig, build-preview | Blueprints scheduling by others, sweep/mop errands |
| Hand-wheel → battery → circuit hub power | Watt/brownout grid at ONI scale (simplified circuit hub instead) |
| Pipes/wires/electrolysis/turbine automation | Full ONI automation gates/sensors suite (subset only) |
| Farm/cook, science lab/lubricant tier | Cooking quality scale, ranch stables, rockets |
| Slime poison gas, cold damage, O2/thin-air, stamina tax, bladder [INFERRED-PT] | Germs, morale/stress, decor/rooms, overcrowding |
| Biomes + geysers, critters (ambient) | Critter morphs/ranching, space/biomes beyond surface |
| Research tech tree | Skill XP formulas, attributes system |
| Temp tolerance band `[TUNING-UNKNOWN]` (proof #4) | Full suit tiers (subset: basic thermal handling) |

---

## 2. Full Gameplay Loop (Phased)

Single-avatar loop is canonical for MVP: every run must pass through research, farm/cook (kcal positive), and battery-to-hub power `[DECISION]`.

| Phase | Name | Player actions | Sim pressure | Unlocks / exit criteria |
|---|---|---|---|---|
| 0 | Land and Survey | Land pod, pause + free cam recon, build-preview survey, pick dig axis | O2 bar, starting O2 pocket finite | Surveyed start biome, placed first ladder/brick plan |
| 1 | Gather and Survive | Vacuum-gun pickup (loose solids + small liquid packets `[INFERRED-PT]`, proof #10), platformer traverse (jump/climb/ladders), eat starting rations | O2 drain + thin-air rule `[TUNING-UNKNOWN]` (proof #2), stamina tax on mining | Stockpile > X kcal + O2 stable |
| 2 | Manual Power | Crank hand-wheel → charge battery → circuit hub distributes to first consumers (lab light, pump) | Battery leakage/round-trip `[TUNING-UNKNOWN]`, hub capacity cap `[TUNING-UNKNOWN]` | Sustained powered lab |
| 3 | Dig and Explore | Dig into adjacent biomes, brick over infra (not ladders), manage slime poison gas, cold damage zones | Gas displacement, pressure, heat leak, poison exposure | Exposed slime + cold biome, sealed with bricks |
| 4 | Research | Staff science lab (manual + powered), unlock tech tree tiers | Power + water + time cost, lubricant tier gate `[INFERRED-PT]` (proof #6) | Pipes/wires/electrolysis/turbine unlocked |
| 5 | Automate | Lay pipes/wires, electrolysis O2, turbine power recovery, vent/pump loops | Fluid/thermal/fire/reactions, brownout-lite `[TUNING-UNKNOWN]`, pipe throughput `[TUNING-UNKNOWN]` | Closed O2 + power loop |
| 6 | Farm and Cook | Farm plots (Mealwood-class 3-cycle baseline `[ONI-REF]` / `[INFERRED-PT]` timing, proof #11), cook station, store food | Water/dirtemp/fertility, spoilage-lite `[INFERRED-PT]`, bladder `[INFERRED-PT]` (proof #3) | Positive kcal/day (MVP-required `[DECISION]`) |
| 7 | Deep Tech | Lubricant tier `[INFERRED-PT]` (proof #6), advanced materials, heat management, geyser tap | High-temp, phase change, pressure damage | Geyser-coupled base |
| 8 | Imperatives | Mission imperative scored quotas `[DECISION]`; ~50-cycle playtest cap `[INFERRED-PT]` (proof #1) | Scored objectives, cycle deadline | Win screen / playtest end |

### Moment-to-moment loop (60-second)

| Step | Action | System |
|---|---|---|
| 1 | Traverse (run/jump/climb) to task site | Platforming |
| 2 | Vacuum or mine resource | Tools / stamina |
| 3 | Check O2 bar + temp readout | Vitals |
| 4 | Crank wheel if battery low | Power |
| 5 | Deposit to stockpile / pipe inlet | Inventory/build |
| 6 | Re-survey in pause + free cam | Camera |

### Cycle-to-cycle loop

| Cycle budget | Sinks |
|---|---|
| O2 consumption | Breathing + thin-air leak |
| kcal consumption | Farming/cooking must exceed (single kcal pool lives on buildings/food, not duplicated in vitals — see S12) |
| Water consumption | Electrolysis + farm + research |
| Power consumption | Lab + pumps + turbine loop efficiency `[TUNING-UNKNOWN]` (proof #5) |
| Heat accumulation | Machines + biomes + geysers |

---

## 3. Mechanics per System

### 3.1 Vitals / Needs (S12)

S12 owns O2/stamina/bladder/warmth and HP. kcal accounting lives in S13 buildings/food stores; S12 reads but never duplicates it `[DECISION]`.

| Mechanic | Rule | Tag |
|---|---|---|
| O2 bar | Depletes on breath tick; refill in breathable pressure/mass zone | `[OBSERVED]` |
| Thin-air rule | Below min gas mass/pressure → O2 bar still drains, suffocation timer; exact kPa/g threshold unconfirmed | `[TUNING-UNKNOWN]` (proof #2) |
| Stamina tax on mining | Each mine swing costs stamina; exhausted = slower mine/move | `[OBSERVED]` |
| Bladder % | Rises over cycle; optional relief interaction; no stress cascade (stress is cut, §6) | `[INFERRED-PT]` (proof #3) |
| Temp tolerance / warmth | Comfort band placeholder; outside = damage-over-time via S15 single damage path (no double-count with S12); exact -10..60 band unconfirmed | `[TUNING-UNKNOWN]` (proof #4) |
| HP / death | Single damage path only (S15 applies, S12 integrates); death = run fail (no respawn in playtest) | `[INFERRED-PT]` (proof #12) |
| Food/kcal | Single kcal pool on stocks/buildings (S13); starvation drains HP via S15 path | `[ONI-REF]` adapted |

### 3.2 Platforming / Tools (S01–S02)

| Mechanic | Rule | Tag |
|---|---|---|
| Move set | Run, jump (variable height), climb walls, mount/dismount ladders | `[OBSERVED]` |
| Ladders | Climbable, but bricks preferred over infra (ladders not built over pipes/wires) | `[OBSERVED]` |
| Vacuum gun | Ranged gather: loose solids + small liquid packets into inventory; no tile damage (resolves "liquids?" note: solids primary, small liquid packets secondary) | `[OBSERVED]` solids / `[INFERRED-PT]` liquid packets (proof #10) |
| Dig tool | Melee mine on solid tiles; stamina-taxed; yields debris | `[OBSERVED]` |
| Build-preview | Ghost placement in pause or live; survey-first workflow | `[OBSERVED]` |
| Bricks over infra | Solid bricks may be built enclosing pipes/wires (seal/protect); ladders may not | `[OBSERVED]` |
| Pause + free cam | Pause sim, pan/zoom camera for survey/planning | `[OBSERVED]` |

| Tool params (clone baseline) | Value |
|---|---|
| Vacuum range | ~4–6 tiles cone `[TUNING-UNKNOWN]` |
| Vacuum rate | Stack-limited per second `[TUNING-UNKNOWN]` |
| Mine hardness tiers | Sand < dirt < rock < metal ore `[ONI-REF]` |
| Fall damage threshold | Present, generous (platformer-friendly) `[INFERRED-PT]` |

### 3.3 Power Chain (S09; manual → hub)

MVP requires battery-to-hub closure `[DECISION]`.

| Stage | Entity | Behavior | Tag |
|---|---|---|---|
| Generation | Hand-wheel (manual crank) | Player channels to inject wattage; no fuel | `[OBSERVED]` |
| Storage | Battery | Stores joules, small leakage; single hub-linked; capacity `[TUNING-UNKNOWN]` | `[OBSERVED]` / `[TUNING-UNKNOWN]` rate |
| Distribution | Circuit hub | Hub capacity cap; consumers prioritized/first-come; brownout-lite (no ONI wire overload) | `[OBSERVED]` hub / `[TUNING-UNKNOWN]` brownout |
| Recovery | Turbine | Converts waste heat/steam/flow back to power (deep tech); input/output ratio unconfirmed | `[OBSERVED]` exists / `[TUNING-UNKNOWN]` ratio (proof #5) |
| Consumers | Lab, pumps, electrolysis, cook station, heaters | Draw while powered; stall without power | `[OBSERVED]` |

| Power params | Clone rule |
|---|---|
| Wheel output | Fixed W while cranking `[TUNING-UNKNOWN]` |
| Battery capacity | Single tier early, upgradeable via research `[INFERRED-PT]` |
| Hub limit | Sum concurrent draw ≤ hub cap or newest stalls `[TUNING-UNKNOWN]` |
| No ONI wire wattage/overload | Simplified — no wire burn `[DECISION]` |

### 3.4 Fluids / Thermal / Fire / Reactions (S04–S08)

Cell model (canonical, `[DECISION]`): single-element-per-cell displacement.

```cpp
struct Cell {
  uint16_t element; // element id, vacuum = 0
  uint16_t flags;   // solid|liquid|gas|vacuum, insulated, on-fire, debris-bit
  float    temp_K;  // kelvin, finite-validated
  float    mass_kg; // kg in cell; pressure derived via element compressibility
};
```

- Pressure derived via element compressibility (zmax-style) `[ONI-REF]`; vapor displaces O2 by density `[OBSERVED]`.
- No secondary liquid / debris quantities in MVP cells `[DECISION]`; flooding films and debris are entities/pickups, not second payloads.
- Conservation: all transfers mass/energy-conservative, capped per-tick, documented per system `[DECISION]`.
- PPR baseline today is content-free `Cell{m_element: u16, m_temperature: float}` with no mass/pressure/phase `[PPR-FACT]`; MVP extends payload to the 4-field form above.

| Subsystem | Rule | Tag |
|---|---|---|
| Tile sim | Per-cell single element + mass, temp, derived pressure; displacement by density | `[OBSERVED]` sim / `[DECISION]` model / `[ONI-REF]` formula |
| Phase model | 3 K hysteresis, 1.5 K rebound, flaking, 80% mass rule | `[ONI-REF]` |
| Elements | ~30+ gases/liquids/solids w/ SHC/TC (subset spawned: O2, CO2, H2, Cl2/poison, water, polluted water, slime, ice/snow, rock/metal ores) | `[ONI-REF]` subset / `[INFERRED-PT]` spawned set |
| Pressure damage | zmax-formula tiles take damage above threshold | `[ONI-REF]` |
| Gas reactions | O2 + H2 via electrolysis; H2 burned/vented; CO2 sinks; poison off-gasses from slime | `[OBSERVED]` electrolysis / `[INFERRED-PT]` rest |
| Thermal | Conduction per TC/SHC, machines emit heat, cold biome drains HP via S15 | `[OBSERVED]` |
| Fire | Flammable gas + heat + O2 → fire tile, damages avatar/buildings; spread tuning unconfirmed | `[INFERRED-PT]` event / `[TUNING-UNKNOWN]` spread (proof #8) |
| Plumbing rates | Pump/pipe throughput fixed kg/s; vent/intake symmetric; exact rates unconfirmed | `[TUNING-UNKNOWN]` |
| Ventilation | Gas pump + filter + vent loop; thin-air stalls flow | `[ONI-REF]` |
| Electrolysis | Water → O2 + H2 at fixed ratio + heat; ratio unconfirmed | `[OBSERVED]` / `[TUNING-UNKNOWN]` ratio |
| Turbine | Steam/heat → power + water reclaim; ratio unconfirmed | `[OBSERVED]` / `[TUNING-UNKNOWN]` ratio (proof #5) |

### 3.5 Building (S03 + S13)

| Mechanic | Rule | Tag |
|---|---|---|
| Ghost preview | Place/delete ghost (S03); materials reserved from inventory | `[OBSERVED]` |
| Bricks | Solid tiles; seal gas/liquid/heat; buildable over pipes/wires | `[OBSERVED]` |
| Ladders | Climb aids; cannot enclose infra | `[OBSERVED]` |
| Pipes/wires | Path-laid; connect ports (input/output/vent); no grid-lock | `[OBSERVED]` |
| Buildings | Lab, battery, hub, wheel, pump, vent, electrolysis, turbine, farm plot, cook station, storage | `[OBSERVED]` |
| Deconstruct | Refund partial/full materials as debris | `[INFERRED-PT]` |

### 3.6 Research Tech Tree (S16)

| Tier | Unlocks | Cost | Tag |
|---|---|---|---|
| T0 Survival | Ladders, bricks, storage, wheel+battery+hub | Free/start | `[INFERRED-PT]` |
| T1 Plumbing | Pipes, pump, vent, basic farm | Science points (lab time + power) | `[INFERRED-PT]` |
| T2 Power/Atmo | Wires upgrade, electrolysis, gas filter | Science + water | `[INFERRED-PT]` |
| T3 Heat/Industry | Turbine, lubricant, insulated brick, cook station+ | Science + lubricant | `[OBSERVED]` lab present / `[INFERRED-PT]` lubricant gate (proof #6) |
| T4 Deep (geyser) | High-temp pump, steam handling, advanced storage | Geyser materials | `[INFERRED-PT]` |

| Research params | Rule |
|---|---|
| Staffing | Avatar channels at lab while powered |
| Science resource | Points into tree; per-tier cost escalation `[TUNING-UNKNOWN]` |
| Lubricant gate | Refinable consumable gating T3+ `[INFERRED-PT]` (proof #6) |

### 3.7 Farming / Cooking (S13)

MVP requires kcal-positive farm/cook closure `[DECISION]`.

| Mechanic | Rule | Tag |
|---|---|---|
| Farm plot | Requires water + light/power + temp band; Mealwood-class growth baseline | `[OBSERVED]` plot / `[ONI-REF]` Mealwood ref / `[TUNING-UNKNOWN]` timing (proof #11) |
| Harvest | Yields kcal solids; replant cost | `[INFERRED-PT]` |
| Cook station | Raw → cooked kcal multiplier; powered + water | `[OBSERVED]` station / `[TUNING-UNKNOWN]` multiplier |
| Spoilage | Lite (long timers) or absent in playtest | `[INFERRED-PT]` |

### 3.8 Critters (S14)

| Mechanic | Rule | Tag |
|---|---|---|
| Presence | Ambient fauna in biomes | `[OBSERVED]` |
| Behavior | Wander/flee; no taming/ranching | `[INFERRED-PT]` |
| Drops | Optional meat/egg (unconfirmed) | `[INFERRED-PT]` |
| Ren cameo | Selectable avatar skin, cosmetic-only | `[OBSERVED]` skin / `[INFERRED-PT]` cosmetic-only (proof #9) |

### 3.9 Biomes / Geysers / Hazards (S15 context)

| Biome | Hazards / resources | Tag |
|---|---|---|
| Start / temperate | O2 pocket, dirt, water, food | `[INFERRED-PT]` |
| Slime | Poison gas off-gas, damage (no germs; germs cut, §6) | `[OBSERVED]` poison gas |
| Cold | Sub-zero damage, ice water; exact band `[TUNING-UNKNOWN]` | `[OBSERVED]` cold / `[TUNING-UNKNOWN]` band (proof #4) |
| Mineral/ore | Metal ore, rock hardness up | `[INFERRED-PT]` |
| Heat/geyser | Steam/hot water emission, power + water source | `[INFERRED-PT]` |
| Glow flora (glow-fern/goo) | Lighting flora reported; lighting contribution unconfirmed | `[INFERRED-PT]` (proof #13) |

| Geyser params | Rule |
|---|---|
| Emission | Periodic kg/s + temp + element `[TUNING-UNKNOWN]` |
| Tap | Pump + insulated pipe required `[INFERRED-PT]` |
| Dormancy | Active/dormant cycle `[INFERRED-PT]` |

---

## 4. Inventory Tables

### 4.1 Entities (27, MVP only)

| # | Entity | Kind | Description | Tag |
|---|---|---|---|---|
| E01 | Avatar (Ren + alts) | Character | Single controlled duplicant; vitals, inventory, tools | `[OBSERVED]` |
| E02 | Landing pod | Structure | Spawn, starting storage | `[INFERRED-PT]` |
| E03 | Loose debris | Pickup | Ore, dirt, food, science/lubricant mats; vacuumable | `[OBSERVED]` |
| E04 | Solid tile (natural) | Tile | Diggable biome rock/ore/ice/slime | `[OBSERVED]` |
| E05 | Built brick | Tile/structure | Player solid; seals infra | `[OBSERVED]` |
| E06 | Ladder | Structure | Climb aid | `[OBSERVED]` |
| E07 | Storage bin | Building | Holds solids | `[INFERRED-PT]` |
| E08 | Hand-wheel | Building | Manual generator | `[OBSERVED]` |
| E09 | Battery | Building | Energy store | `[OBSERVED]` |
| E10 | Circuit hub | Building | Power distributor | `[OBSERVED]` |
| E11 | Science lab | Building | Research station (powered) | `[OBSERVED]` |
| E12 | Gas/liquid pump | Building | Intake from world cell | `[OBSERVED]` |
| E13 | Vent / outflow | Building | Emit to world cell | `[OBSERVED]` |
| E14 | Pipe segment | Infra | Fluid transport | `[OBSERVED]` |
| E15 | Wire segment | Infra | Power transport (hub-linked) | `[OBSERVED]` |
| E16 | Gas filter | Building | Stream split/select | `[INFERRED-PT]` |
| E17 | Electrolyzer | Building | Water → O2+H2 | `[OBSERVED]` |
| E18 | Turbine/generator | Building | Heat/steam → power; ratio `[TUNING-UNKNOWN]` (proof #5) | `[OBSERVED]` / `[TUNING-UNKNOWN]` |
| E19 | Farm plot | Building | Crop bed | `[OBSERVED]` |
| E20 | Crop (Mealwood-class) | Plant | 3-cycle growable baseline `[ONI-REF]`; timing `[TUNING-UNKNOWN]` (proof #11) | `[ONI-REF]` / `[TUNING-UNKNOWN]` |
| E21 | Cook station | Building | Raw → cooked | `[OBSERVED]` |
| E22 | Geyser | POI | Periodic emitter | `[INFERRED-PT]` |
| E23 | Critter (ambient) | Creature | Wander/flee fauna | `[OBSERVED]` |
| E24 | Poison gas cloud | Hazard tile | Slime off-gas damage zone | `[OBSERVED]` |
| E25 | Fire tile | Hazard | Combustion event; spread `[TUNING-UNKNOWN]` (proof #8) | `[INFERRED-PT]` / `[TUNING-UNKNOWN]` |
| E26 | Lubricant depot/refinery | Building | T3 gate material; recipe unconfirmed | `[INFERRED-PT]` (proof #6) |
| E27 | Imperative board/flag | UI/POI | Scored-quota tracker `[DECISION]`; UI unimplemented in playtest `[INFERRED-PT]` (proof #7) | `[DECISION]` / `[INFERRED-PT]` |

### 4.2 Components (ECS-style, MVP only)

`TileCell` matches the canonical §3.4 `Cell` exactly. No DLC-reserved components in this table (see §6 Cut List).

| Component | Fields | On entities | Tag |
|---|---|---|---|
| `Avatar` | moveSpeed, jumpV, climbSpeed, stamina, o2, bladderPct, warmth, hp | E01 | `[OBSERVED]` fields / `[INFERRED-PT]` bladder (proof #3) |
| `Inventory` | slots, mass caps, kcal store | E01, E07 | `[INFERRED-PT]` |
| `ToolVacuum` | range, rate, filter tags | E01 | `[OBSERVED]` |
| `ToolDig` | power tier, staminaCost/swing | E01 | `[OBSERVED]` |
| `TileCell` | element u16, flags u16, temp_K float, mass_kg float (pressure derived) | E04–E05, world grid | `[DECISION]` model / `[PPR-FACT]` PPR baseline |
| `Health` | hp, maxHp, damageSources[] (S15 single path only) | E01, E23 | `[INFERRED-PT]` |
| `BatteryStore` | joules, cap, leakRate | E09 | `[OBSERVED]` / `[TUNING-UNKNOWN]` rate |
| `GeneratorManual` | wattsWhileCranked, channelTime | E08 | `[OBSERVED]` / `[TUNING-UNKNOWN]` watts |
| `CircuitHub` | capW, consumerList, priority | E10 | `[OBSERVED]` / `[TUNING-UNKNOWN]` cap |
| `Consumer` | demandW, stalled bool | E11–E13, E17–E18, E21 | `[OBSERVED]` |
| `PipeNode` | port(dir,in/out), contents kg, throughput | E12–E16, pipes | `[OBSERVED]` / `[TUNING-UNKNOWN]` rate |
| `Researchable` | tier, scienceCost, prereqs[] | tech defs | `[INFERRED-PT]` |
| `Lab` | progress, ratePerSec, powerRequired | E11 | `[OBSERVED]` |
| `FarmPlot` | water, light, tempBand, growth 0..1 | E19–E20 | `[INFERRED-PT]` / `[TUNING-UNKNOWN]` timing (proof #11) |
| `Cooker` | recipe in→out, time, power/water | E21 | `[OBSERVED]` / `[TUNING-UNKNOWN]` multiplier |
| `Emitter` | element, rate, temp, period/dormancy | E22 | `[INFERRED-PT]` |
| `CritterAI` | state(wander/flee), speed, drops? | E23 | `[INFERRED-PT]` |
| `Hazard` | type(poison/cold/heat/fire), dps (S15 only; S12 never double-counts) | E24–E25, biomes | `[OBSERVED]` poison/cold / `[INFERRED-PT]` fire (proof #8) |
| `Buildable` | ghost, cost[], blocksGas, allowsInfraOverlap | E05–E21 | `[OBSERVED]` |
| `Imperative` | goal type, quota, deadline, reward (scored quotas) | E27 | `[DECISION]` / `[INFERRED-PT]` UI (proof #7) |

### 4.3 Systems (canonical tick order)

Canonical tick, same in all docs `[DECISION]`. Input is captured per-frame and consumed on the 60 Hz tick. Slow systems run on integer divisors of the tick: vitals/hazards/buildings at 1 Hz, critters at 10 Hz. Power (S09) runs before plumbing (S10); no invented per-system Hz beyond the divisors stated here.

| Order | System | Inputs → Outputs | Tag |
|---|---|---|---|
| frame | Input capture | Per-frame input → command buffer (consumed on tick) | `[DECISION]` |
| S00 | Pause/camera | Input → freeze S01–S16, free cam | `[OBSERVED]` |
| S01 | Controller | Commands (consumed on tick) → velocity, climb/ladder state, crank channel | `[OBSERVED]` |
| S02 | Tools (vacuum/dig) | Aim + stamina → debris/inventory, tile damage | `[OBSERVED]` |
| S03 | Build | Ghosts + inventory → placed/removed structures | `[OBSERVED]` |
| S04 | Liquid flow | Pressure/mass → liquid mass exchange (capped/tick, conservative) | `[OBSERVED]` sim / `[TUNING-UNKNOWN]` rates |
| S05 | Gas flow | Pressure/mass → gas mass exchange; vapor displaces O2 by density | `[OBSERVED]` / `[ONI-REF]` formula |
| S06 | Thermal | TC/SHC conduction + convection advection | `[OBSERVED]` / `[ONI-REF]` |
| S07 | Phase | 3 K hysteresis / 1.5 K rebound, flaking, 80% mass rule | `[ONI-REF]` |
| S08 | Reactions/fire | Element pairs + heat → outputs/damage; spread `[TUNING-UNKNOWN]` (proof #8) | `[INFERRED-PT]` / `[TUNING-UNKNOWN]` |
| S09 | Power | Crank + storage via hub → consumer stall/run | `[OBSERVED]` / `[TUNING-UNKNOWN]` caps |
| S10 | Plumbing | Pump → pipe → vent/filter mass move (fixed kg/s `[TUNING-UNKNOWN]`) | `[OBSERVED]` / `[TUNING-UNKNOWN]` |
| S11 | Automation | Sensor/logic subset → gates/valves | `[INFERRED-PT]` subset |
| S12 | Vitals/needs | O2 mass + temp + food → o2/stamina/bladder/warmth, HP (no kcal duplication; kcal in S13) | `[OBSERVED]` O2/stamina / `[INFERRED-PT]` bladder (proof #3) |
| S13 | Buildings (lab/farm/cook/electro/turbine) | Power+water+time → science/kcal/O2/power; turbine/lubricant ratios `[TUNING-UNKNOWN]` (proofs #5, #6, #11) | `[OBSERVED]` / `[TUNING-UNKNOWN]` |
| S14 | Critters (10 Hz divisor) | AI wander/flee | `[OBSERVED]` present / `[INFERRED-PT]` behavior |
| S15 | Hazards (cold/poison/fire; single damage path, no double-count with S12) | Zone + exposure → dps | `[OBSERVED]` cold/poison / `[INFERRED-PT]` fire (proof #8) |
| S16 | Research/mission | Points → unlocks; scored quotas → win/fail | `[INFERRED-PT]` tree / `[DECISION]` quotas (proof #7) |

Slow-system divisors: S12/S13/S15 at 1 Hz; S14 at 10 Hz; all divisors of the 60 Hz fixed tick `[DECISION]`.

### 4.4 Assets

| Category | Asset | Notes | Tag |
|---|---|---|---|
| Tiles/materials | Dirt, sand, rock, metal ore, slime, ice/snow, water, polluted water, O2, CO2, H2, poison gas, steam | SHC/TC per ONI subset | `[ONI-REF]` / `[INFERRED-PT]` spawned set |
| Buildings | Pod, wheel, battery, hub, lab, pump, vent, pipe, wire, filter, electrolyzer, turbine, farm plot, cooker, storage, ladder, brick (+insulated), lubricant station | Ghost + built + stalled states; lubricant recipe `[INFERRED-PT]` (proof #6) | `[OBSERVED]` core |
| Avatar | Base suit + Ren skin, run/jump/climb/crank/mine/vacuum anims | Single rig, palette swap; Ren cosmetic-only `[INFERRED-PT]` (proof #9) | `[OBSERVED]` / `[INFERRED-PT]` |
| Critters | 2–3 ambient species sprites + wander/flee anims | No morphs | `[INFERRED-PT]` |
| VFX | Dig particles, vacuum beam/suction, splash/vent puffs, steam, fire, cold breath, poison haze, spark/power flow, research glow, harvest pop | Tile-cell aligned; glow-fern/goo lighting unconfirmed | `[INFERRED-PT]` / glow `[INFERRED-PT]` (proof #13) |
| Audio | Jump/land, mine ticks, vacuum loop, crank loop, build thunk, pipe flow, vent hiss, electrolysis hum, turbine spin, farm/cooking cues, hazard cough/freeze, UI clicks, ambient biome beds | Full catalogue inferred; loops for crank/vacuum/flow | `[INFERRED-PT]` catalogue (proof #14) |
| UI | O2 bar, stamina pips, kcal, bladder% (toggle) `[INFERRED-PT]` (proof #3), temp readout, battery/hub panel, build-preview palette, tech tree, farm/cooker panels, imperative tracker (placeholder) `[DECISION]`/`[INFERRED-PT]` (proof #7), pause/free-cam hint, char select (Ren) | Pause menu + tooltips | `[OBSERVED]` bars/palette/tree/char-select |
| Overlays (lite) | Power, plumbing, thermal, gas pressure | Subset of ONI overlays | `[INFERRED-PT]` |

---

## 5. Observed vs Inferred vs Tuning-Unknown

| Status | Items |
|---|---|
| `[OBSERVED]` (footage/store) | Side-scroller single-dupe platformer; jump/climb/ladders; vacuum gun solids gather; build-preview; hand-wheel→battery→circuit hub; bricks over infra not ladders; dig/explore; slime poison gas; cold damage; research tech tree; pipes/wires/electrolysis/turbine existence; farm plot/cook station existence; science lab tier; full gas/liquid/heat tile sim; vapor displaces O2 by density; O2 bar; stamina tax on mining; critters present; Ren skin in char select; pause+free cam |
| `[INFERRED-PT]` (inference, needs proof) | ~50-cycle cap (#1); bladder% presence/effect (#3); fire event (#8); farm 3-cycle timing (#11); geysers + dormancy; lubricant gate/recipe (#6); imperative UI absent-impl (#7); Ren cosmetic-only (#9); vacuum small liquid packets (#10); glow-fern/goo lighting (#13); audio/VFX specifics and full sound catalogue (#14); fall damage tuning; death/fail handling (#12) |
| `[TUNING-UNKNOWN]` (placeholder until measured) | Thin-air kPa/g threshold (#2); -10..60 tolerance band (#4); turbine in/out ratio (#5); electrolysis ratio; pipe/vent/pump kg/s rates; wheel watts; battery cap/leak; hub cap/brownout point; research costs; farm/cook multipliers and growth time (#11); fire spread (#8) |
| `[ONI-REF]` (reuse baseline) | Phase model (3K/1.5K/flaking/80%), SHC/TC table, zmax-style pressure formula, plumbing/ventilation structure, Mealwood 3-cycle reference |
| `[DECISION]` (clone choice) | Single-element-per-cell `Cell{element u16, flags u16, temp_K float, mass_kg float}` with derived pressure; no secondary liquid/debris in MVP cells; conservative capped transfers; single-avatar canonical loop (research + farm/cook + kcal + battery→hub all MVP-required); imperatives as scored quotas; canonical 17-stage tick with per-frame capture and 1 Hz / 10 Hz divisors; single S15 damage path |
| `[PPR-FACT]` (PPR source) | PPR `Cell{m_element: u16, m_temperature: float}` baseline (no mass/pressure/phase); ChunkGrid/ECS/tick/snapshot ground truth per Architecture §1 |

### Proof checklist (to promote [INFERRED-PT] / [TUNING-UNKNOWN] → observed)

| # | Claim | Proof needed |
|---|---|---|
| 1 | 50-cycle cap | Footage of cycle counter / end screen |
| 2 | Thin-air kPa/g threshold | HUD tooltip or debug readout |
| 3 | Bladder% effect | UI tick or interaction prompt |
| 4 | -10..60 tolerance band | Tooltip stringify or damage log |
| 5 | Turbine input/output ratio | Building panel numbers |
| 6 | Lubricant recipe | Refinery panel |
| 7 | Imperatives implemented | Goal UI with quota/progress |
| 8 | Fire propagation/spread | Ignition footage |
| 9 | Ren cosmetic-only | Char select stats or dev note |
| 10 | Vacuum liquid packets | Footage of liquid pickup into inventory |
| 11 | Farm timing/multiplier | Farm/cooker panel numbers over cycles |
| 12 | Death/fail handling | Death footage or fail screen |
| 13 | Glow-fern/goo lighting | Footage of flora light contribution |
| 14 | Sound catalogue | Captured audio stems or asset list |

---

## 6. Cut List (only place cut systems are listed; no MVP stubs)

Morale, stress, skills (XP/attributes/jobs), printing (pod/dupe generation), germs (all types, diffusion/contraction), ranching (stables/morphs/comfort/taming), decor (items/rooms/overlays), rockets (space/multi-planet), ribbons (4-bit bundled networks — automation is 1-bit subset only in MVP), scheduling (cycle-block schedule UI — 600 s / 24×25 s schedule hangs off MVP-2, not MVP loop). No component or system IDs are reserved for these in MVP tables `[DECISION]`.

---

*End of AwayTeam-Gameplay-Inventory.md — one-file clone spec; no other writes.*
