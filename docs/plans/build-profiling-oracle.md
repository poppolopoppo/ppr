# Build profiling autonomy — oracle design (handoff)

> **Document scope:** Design/proposal record, not a current implementation inventory or repository-wide instruction. Apply its task-specific requirements only when the active task explicitly adopts this design. Verify implementation claims against source/CMake and the current [documentation guide](../README.md); historical paths, counts and phase status may be stale.

Date: 2026-10-09. Source: oracle ora-4 (`ses_ee1524769ffeUzkUOcCQjxpLho`), reconciled.
Evidence: librarian lib-1 (`ses_ee15e347bffeWp1s7n0eQtH55h`), explorer exp-2 (`ses_ee15e3479ffetAbvVDyomzk9KF`).
Constraints: AGENTS.md @ `dd8b8dd` (smallest-complete, no speculative generality, reuse-first, triggered-risk measurement, token sweet-spot; procedures live in skills). No AwayTeam refs.
Starting doc: https://learn.microsoft.com/en-us/cpp/build-insights/get-started-with-cpp-build-insights?view=msvc-170

Status: DESIGN ONLY, unrun. Pilot gate explicitly unrun. No files edited, no MCP built.

## 0. Prior findings (do not re-research without cause)

- No MSVC C++ Build Insights MCP server or skill exists (docs TOC, GitHub `vcperf` = 2 repos only, npm zero, awesome-mcp zero). VS Copilot build-performance (VS 2026 18.3 Insiders 4+ / MSVC 14.50+, paid) has no MCP surface — unusable for OpenCode.
- Closest agent-native alternative is BinlogInsights (MSBuild `.binlog`, MIT, `dotnet tool install -g BinlogInsights.Mcp`, 26 tools) — wrong backend for PPR single-config Ninja builds.
- Local repo has NO build-timing infra. `msvc-profile` / `PPR_ENABLE_PROFILING` / `USE_PIX` / `run_*timing.bat` / `--profile-csv` are all runtime profiling. No `vcperf`, `/Bt`, `/d2cgsummary`, `ninja -d stats`, `msbuild /bl`, CMake profiling, or `.etl` handling (`CMakePresets.json:117-127`, `cmake/compiler/MSVC.cmake:89-95`, `build-system SKILL.md:136`, `pix-profiling SKILL.md:10,18-25`).
- PPR = CMake + C++20 modules + MSVC presets (`msvc-dev` default, `msvc-rel`, `msvc-live`). Build Insights `/MP`/PCH advice from MSBuild samples does NOT transfer: Ninja already schedules compiler jobs.

## 1. Decision

Start with a skill-driven workflow using Ninja evidence plus `vcperf` JSON. Do NOT build an MCP server yet.

`vcperf` already exports machine-readable Chrome trace JSON — no WPA interaction, no custom SDK consumer needed initially:

```bat
vcperf /start /noadmin /level2 PPR_Build_<id>
cmake --build out/build/msvc-dev --target engine.tests.core --parallel <N>
vcperf /stop PPR_Build_<id> /timetrace <artifact-dir>\compiler.json
```

Deferred conversion (per Microsoft command reference):

```bat
vcperf /stopnoanalyze PPR_Build_<id> <artifact-dir>\raw.etl
vcperf /analyze <artifact-dir>\raw.etl /timetrace <artifact-dir>\compiler.json
```

Verify installed-tool support and permissions first; `/noadmin` may require approved one-time permission setup.

### Tool fit

| Tool | Appropriate use |
|---|---|
| Build wall time + Ninja `.ninja_log` | Default evidence: elapsed time, executed edges, expensive compile/scan/link/custom steps |
| Ninja `-d explain` | Unexpected rebuilding and invalidation diagnosis |
| Ninja `-d stats` | Ninja's own bookkeeping overhead — NOT compiler-phase profiling |
| CMake `--profiling-format=google-trace --profiling-output=…` | Slow configure/generate work — NOT compilation (`chrometrace` is not the option value) |
| `vcperf /timetrace` | MSVC compiler/linker internals, independent of CMake vs MSBuild. Appropriate for PPR |
| SDK-backed analyzer (`Microsoft.CppBuildInsights` NuGet; samples `BottleneckCompileFinder`, `FunctionBottlenecks`, `CAToolsetPerfDataCollector`) | Only after a demonstrated gap in JSON attribution or required event coverage. Prefer a CLI before an MCP wrapper |
| BinlogInsights | Wrong primary backend for PPR Ninja builds |

For modules, combine Ninja scheduling/invalidation evidence with MSVC phase evidence. Explicitly check coverage of module scanning and IFC production/consumption rather than assuming dedicated module events exist.

## 2. Step 1 — prove instrumentation before adding infrastructure (UNRUN pilot)

Run one bounded pilot through the existing persistent Insiders vcvars shell:

1. Record actual compiler, linker, CMake, Ninja and `vcperf` paths/versions; fixed parallelism; preset and target.
2. Use `msvc-dev` / `engine.tests.core` — exercises `import std`, engine modules and linking without the application stack.
3. Build to readiness, then measure: no-op build; controlled implementation-unit rebuild; controlled module-interface rebuild with its dependent closure.
4. Capture one genuinely dirty build with `/level2`, export JSON, verify it identifies the expected compiler invocations, source paths and front-end/back-end activities.
5. Compare against Ninja records; check module scan/IFC coverage (see above).
6. Confirm capture teardown also works when the build fails. Stop only the session this workflow owns; preserve build failure separately from capture/conversion failure.

Rules: isolated build tree for cleaning/source-mutation experiments. NEVER clean the developer's active tree. Permission/tool incompatibility means "profiling unavailable", not success.

Investment gate: proceed only if this pilot produces actionable, bounded summaries. If JSON lacks one necessary attribution, request that specific capability — not a general profiling service.

## 3. Step 2 — extend `build-system` (build-system stays authoritative)

Insertion points in `.opencode/skills/build-system/SKILL.md`:

- Lines 3–5: add build-performance diagnosis to the description.
- After "Current project facts", lines 124–138: add "Build-performance workflow".
- Line 136: clarify `msvc-profile` profiles runtime, not build time.
- Delegation table, lines 217–224: builder captures/measures; fixer edits; reviewer interprets evidence once.

Two levels:

- Always-on, cheap: for already-required builds, retain command, preset/targets, exit code, wall time, log path; record whether configure/regeneration occurred. No extra builds or ETW captures.
- Conditional — activate focused measurement for: explicit build-speed work or claimed improvement; broad import/re-export/header dependency changes; module/source splitting, target-graph changes, compiler/linker flags, custom commands or scheduling changes; observed unexpected fan-out, repeated regeneration, significant unexplained slowdown.

Build hot-path definition: edit-to-ready latency for a named edit class, target closure and preset — NOT the slowest TU.

| Change under investigation | Required representative workload |
|---|---|
| Implementation-local optimization | Warm-cache implementation edit → affected executable/test |
| Interface/import/dependency change | Interface edit → dependent closure; clean build if topology changes |
| Compiler flags, scheduling, source organization | Clean configured-tree build plus representative incremental build |
| CMake/custom-command behavior | Configure/regenerate and no-op behavior |
| Release code generation/LTCG | `msvc-rel`, not extrapolated Debug results |
| Edit-and-Continue behavior | `msvc-live` |

Defaults: `msvc-dev`; other presets risk-driven. Keep dependency acquisition and configure separate from compilation; distinguish "clean outputs" from "cold OS cache".

## 4. Step 3 — triggered validation gate

In `.opencode/skills/validation/SKILL.md`:

- Extend execution step 6, lines 62–67, with a reference to the build-performance workflow.
- Add the trigger rule after Scope rules, lines 90–94.
- Extend Report, lines 216–223, with an evidence-directory reference and measurement verdict.

For optimization claims: compare baseline vs candidate with identical workload, toolchain, parallelism and cache policy. Repeat paired runs to expose noise; report samples and spread, not one percentage. Use UNINSTRUMENTED timing for the improvement claim; tracing explains it. Preserve correctness tests, module dependencies, sanitizers and requested output semantics. No invented global percentage threshold. Missing required evidence leaves the claim unverified.

## 5. Step 4 — artifacts (small in context, complete on disk)

Ignored location:

```text
out/build-perf/<case>/<baseline-or-candidate>/<run>/
  manifest.json
  build.log
  ninja.before.log
  ninja.after.log
  compiler.json          # conditional
  raw.etl                # conditional
  configure.json         # conditional
  summary.md
```

Minimum `manifest.json`: revision + dirty-patch identity; exact commands, working/build dirs, preset, targets, parallelism; toolchain versions; workload/edit/reset recipe; cache/dependency preparation; wall time, exit codes, capture status, artifact paths.

`summary.md`: workload, before/after samples, median/spread, executed-edge count, top five attributable costs, proposed cause, verdict.

Analysis safeguards: isolate the current Ninja invocation; detect log compaction; deduplicate multi-output edges. Do NOT sum nested compiler spans into wall time. Do NOT call a long TU the critical path without dependency evidence. `vcperf` remaps trace process/thread IDs for visualization — NOT reliable physical-concurrency evidence.

Agents read summaries first; inspect only relevant raw slices.

## 6. Step 5 — automate only demonstrated repetition

Initially: documented commands + bounded PowerShell JSON extraction. After the pilot, add a small capture/summary script only if repeated manual handling warrants it.

Non-goals: new profiling presets, always-on ETW, GUI automation, telemetry service, automatic compiler upgrades, module-cache enablement, "optimizations" that disable checks or weaken build semantics.

## 7. References

- SDK overview: https://learn.microsoft.com/en-us/cpp/build-insights/reference/sdk/overview?view=msvc-170
- vcperf commands: https://learn.microsoft.com/en-us/cpp/build-insights/reference/vcperf-commands?view=msvc-170
- Samples: https://github.com/microsoft/cpp-build-insights-samples
- vcperf: https://github.com/microsoft/vcperf
- OpenCode MCP config: https://opencode.ai/docs/mcp-servers/

## 8. Addendum 2026-10-09 — live cl.exe blowup on App.Frame.Packet.cpp (CRITICAL TU)

Live snapshot (while the build was running; process may since have exited):

- PID 56708, parent 71808, started 2026-10-09 16:34:15, ~5m41s CPU at capture.
- WorkingSet 37.33 GB (40266362880 B), PageFile 56.13 GB. Kernel 71s / User 271s.
- Toolchain: VS 18 Insiders MSVC 14.51.36231 `Hostx64\x64\cl.exe`.
- TU: `E:\Code\ppr\lib\engine\app\frame\App.Frame.Packet.cpp` → `out/build/msvc-dev`, target `engine.app`, preset `msvc-dev`.
- Full command line (verbatim, 70 args): Debug + ASan + modules — `/Od /Ob0 /MDd /RTC1 /Z7 /W4 /WX /fsanitize=address /std:c++latest /bigobj /permissive- /EHsc /utf-8`, defines `FMT_SHARED GLFW_DLL LIBDEFLATE_DLL PPR_ENABLE_SANITIZER_ADDRESS PPR_LOG_LEVEL_THRESHOLD=0 SIMDJSON_THREADS_ENABLED=1 SIMDJSON_USING_WINDOWS_DYNAMIC_LIBRARY=1 SLANG_USER_CONFIG="slang-user-config.h" UNICODE _MBCS __AVX2__ __AVX__ __SSE2__ __SSE4_1__ __SSE4_2__ _ANNOTATE_STL`, includes Agility SDK + slang/slang-rhi + vcpkg + mango + imgui + `include` (`-external:W0`), plus `@App.Frame.Packet.cpp.obj.modmap` (i.e. `import std` / named-module BMI search paths on the command line), `/showIncludes /FS -c`.

Why this TU is the critical pilot case (replaces the generic `engine.tests.core` starter):

1. It reproduces the worst known pathology in-tree: ~40 GB compiler RAM on a single TU. If the pilot proves anything, it must explain THIS invocation first — frontend (template/module instantiation, `/showIncludes` fan-out via the `.modmap`) vs backend/codegen (`/Od` + AVX2 codegen over a large packet TU).
2. Suspect amplifiers, re-ranked after the msvc-live repro (see below): (a) `import std` + module BMI graph pulled through the response-file `.modmap`; (b) header fan-out still reachable despite modules — verify with `/showIncludes` output already on the command line; (c) Debug codegen at `/Od` holding full IR for a big TU (`/Ob0 /RTC1 /Z7`); (d) ASan instrumentation — now DEMOTED: msvc-live reproduces without it.
3. The pilot's three-step ladder (no-op → impl-unit rebuild → interface rebuild) should be run WITH THIS TU as the dirty-rebuild case: touch `App.Frame.Packet.cpp` only, rebuild `engine.app`, capture with `vcperf /level2` + `/timetrace`, and diff Ninja edge cost vs `compiler.json` frontend/backend split.

Second repro — `msvc-live`, same TU, NO ASan (2026-10-09 ~17:04, PID 64172, 42.67 GB WS):

- Same `cl.exe` 14.51.36231, same TU `lib/engine/app/frame/App.Frame.Packet.cpp` → `out/build/msvc-live`, target `engine.app`.
- Delta vs msvc-dev invocation: NO `PPR_ENABLE_SANITIZER_ADDRESS`, NO `/fsanitize=address`, NO `/WX`, NO `/D_ANNOTATE_STL`; ADDS `/DWIN32 /D_WINDOWS` and `-ZI` (Edit-and-Continue). Rest identical (`/Od /Ob0 /MDd /RTC1 /Z7`, `std:c++latest`, `.modmap`, `/showIncludes`).
- Consequence: ASan is RULED OUT as the primary cause — 42.67 GB without it exceeds the 37.33 GB with it. Prime suspects are now the module/import graph (`.modmap` + `import std`) and/or `/showIncludes` header fan-out and/or `/Od+RTC+Z7` Debug IR retention. `-ZI` (Edit-and-Continue) joins the suspect list for the live preset specifically.
- Both presets red for `engine.app` iteration until this TU is fixed.

Gating rule — this TU fix IS the evidence for the new process/skill:

- Fixing `App.Frame.Packet.cpp` is CRITICAL and blocking: no build-performance skill text is accepted as proven until the workflow in §§2–4 has been executed end-to-end against this TU and produced (a) a `compiler.json` trace attributing the RAM to a phase, (b) a before/after uninstrumented wall-time + peak-RAM comparison for the fix, and (c) the `out/build-perf/packet/` artifact set from §5.
- The skill's first worked example MUST be this TU (both presets where applicable). Generic `engine.tests.core` pilot data alone does not qualify.

Immediate next-session actions:

1. Confirm whether PIDs 56708 (`msvc-dev`, 37.33 GB) and 64172 (`msvc-live`, 42.67 GB) finished or OOM-killed; record exit code, wall time, peak RAM, and emitted `.obj` size each. Both presets are red for `engine.app` iteration until this TU is fixed.
2. Reproduce in the isolated way the plan demands: `vcperf /start /noadmin /level2 PPR_Packet_<id>` → `cmake --build out/build/msvc-dev --target engine.app` (warm, single-TU dirty) → `/stop … /timetrace out/build-perf/packet/<run>/compiler.json`. Also stash `ninja.before.log` / `ninja.after.log` and the `.modmap` response file content. Repeat for `msvc-live` if the dev trace does not explain both.
3. Bisect inputs cheaply before touching code, in this order: (a) `import std` vs narrowed imports (if the TU uses it), (b) `/showIncludes` fan-out top-20, (c) `-ZI` off on the live preset, (d) `/RTC1`+`/Z7` contribution. ASan already ruled out by the live repro — do not re-bisect it. One variable at a time, uninstrumented wall-time + peak-RAM comparison; tracing explains, per §4.
4. Do NOT "fix" by disabling ASan/WX/checks globally or by splitting the TU blindly — those are listed non-goals until the trace attributes the memory to a phase. If the trace shows frontend instantiation blowup, the fix belongs to imports/includes and the module interface boundary; if backend, to codegen inputs (flags/TU size/`-ZI`).
