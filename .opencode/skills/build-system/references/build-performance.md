# Build-performance workflow

Load when a focused-measurement trigger in [build-system](../SKILL.md#build-performance-workflow)
applies. Execution ownership remains with the validation owner / builder lane.

The build hot-path is edit-to-ready latency for a named edit class, target closure,
and preset, not the slowest TU.

| Change under investigation | Representative workload |
|---|---|
| Implementation-local | Warm implementation edit → affected executable/test |
| Interface/import/dependency | Interface edit → dependent closure; clean build if topology changes |
| Flags, scheduling, source organization | Clean configured-tree build plus representative incremental build |
| CMake/custom commands | Configure/regenerate and no-op behavior |
| Release code generation/LTCG | `msvc-rel`, not extrapolated Debug results |
| Edit-and-Continue | `msvc-live` |

Default to the host Dev preset (`msvc-dev` on Windows, `clang-dev` on Linux);
select other presets by risk. Separate dependency
acquisition and configure from compilation. Distinguish clean outputs from cold
OS cache. Use an isolated tree for cleaning/source-mutation experiments; NEVER
clean the developer's active tree.

Retain artifacts in `out/build-perf/<case>/<baseline-or-candidate>/<run>/`:
`manifest.json`, `build.log`, `ninja.before.log`, `ninja.after.log`, `summary.md`,
and conditional `compiler.json`, `raw.etl`, or `configure.json`. The manifest
records revision/dirty identity, exact commands, working/build directories,
preset/targets/parallelism, toolchain versions, edit/reset and cache preparation,
wall time, exit codes, capture status, and artifact paths. The summary separates
measured from pending evidence and records samples/spread, executed-edge count,
top-five attributable costs, proposed cause, and verdict. Read summaries first.

Isolate the current Ninja invocation; detect log compaction and deduplicate
multi-output edges. Never sum nested compiler spans into wall time or call a long
TU the critical path without dependency evidence. `vcperf` remaps process/thread
IDs for visualization: they are not physical-concurrency evidence. The builder
owns a unique capture session and stops only that session even on build failure;
preserve build failure separately from capture/conversion failure. Unsupported
tools/permissions mean profiling unavailable, not success.

Historical build-performance measurements belong in their retained evidence
records, not in this operational procedure. Cite only artifacts available to the
current review; otherwise mark the historical result unverifiable.

Non-goals: new presets, always-on ETW, GUI automation, telemetry, compiler
upgrades, module-cache enablement, or disabling checks/weakening build semantics.
