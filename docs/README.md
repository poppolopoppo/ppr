# Documentation guide

## Choose the right authority

1. [AGENTS.md](../AGENTS.md) defines enduring engineering rules. The active task and runtime permissions determine what work is authorized.
2. Current source, module exports and `CMakeLists.txt` establish implemented behavior and built APIs. A file on disk is not necessarily registered. `CMakePresets.json` establishes checked-in preset values; a local build cache can differ.
3. [Repository map](../codemap.md) and subsystem `codemap.md` files locate implementation. Read the owning map, then verify the relevant symbols. CodeGraph is the first code-navigation tool when `.codegraph/` exists; it returns current source, while graph coverage depends on indexing.
4. [.opencode/skills](../.opencode/skills/) own specialized procedures. Historical tool probes in a procedure are evidence from that session, not proof a tool is available in the current session; discover current capabilities before invoking them.

## Current entry points

- [Engine dependency map](../lib/engine/codemap.md)
- [Application lifecycle/ownership](../lib/engine/app/codemap.md)
- [Render worker and payload boundary](../lib/engine/app/frame/codemap.md)
- [Simulation](../lib/engine/sim/codemap.md) and [physics](../lib/engine/physics/codemap.md)
- [Game integration](../game/codemap.md)
- [Display control interface](../lib/engine/app/DisplayControls.md)
- [Build configuration](../cmake/codemap.md) and [test scheduling](../codemap.md#test-infrastructure)

## Design and historical material

`plans/`, `clone/` and `physics/` contain designs and proposals, including partially implemented designs. Their imperatives, phase gates and proposed modules apply only to work that adopts that design. They do not establish current functionality or authorize expanding an unrelated task.

`diagrams/` contains multiple architecture snapshots and proposed topologies. Check each gallery's source/report and scope; a diagram is not evidence that a lane, API, or phase ships. `.slim/deepwork/` and handoffs are historical session records, not current operational instructions. Recorded timing, green runs, tool registries and debugging observations apply to the captured revision/environment unless revalidated.

## Keep documentation useful

Update the owning map when changing public boundaries, entry points or registration. Prefer symbol/file links over brittle source line ranges and copied inventories. Put operational policy in its owning skill and link it instead of repeating it. Clearly distinguish implemented behavior, intended contract, proposal and measured evidence. Do not turn an obsolete TODO into a new task for the next agent.

The October 2026 documentation audit corrected application/render handoff, demo behavior, test registration/loop defaults and key navigation guidance against source/CMake. It did not re-run runtime/performance gates or certify every historical design claim.
