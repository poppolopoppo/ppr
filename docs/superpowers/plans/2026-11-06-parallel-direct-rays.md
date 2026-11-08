# Parallel direct rays implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox tracking.

**Goal:** Expose concurrent ray work while preserving ordered lighting values.
**Architecture:** One64-thread group per direct-window receiver, four rays per lane, shared RGB/status, ordered lane0 projection; existing pass owns new pipeline.
**Tech Stack:** C++23, Slang, Slang-RHI/Vulkan, CTest, Nsight Systems.
**Spec:** `docs/superpowers/specs/2026-11-06-parallel-direct-rays-design.md`.

## Global Constraints

- Preserve256directions, FP32 transport and coefficient accumulation order; no floating wave reduction.
- New path only direct-window; retained64-thread straight/cascade/bounce controls.
- Preserve quota/masks/source identity/copies/fences/cancellation/teardown; no public tuning or payload ABI.
- Logical ray scratch4096bytes/group plus32bytes of existing counters (4128total); one dispatch group per receiver; no global ray scratch.
- Quality15%brightness/1%leakage, coefficient tolerance1e-5, GPU6/10ms, age1s, lighting4GiB unchanged.
- Existing dirty feature checkout is authoritative; no stage/commit/reset. Author edits only; coordinator owns builds/tests; runtime delegated serially after release.

## Review Focus

- GroupIndex64 vs wave-local indexing must cover each phi exactly once.
- Masked bins explicitly initialize RGB/status, even if no rays are traced.
- All lanes reach barriers; only lane0 writes/reports output and aggregate errors.
- Shared projection extraction preserves every expression/order and reference behavior.
- One-group-per-receiver admission, pipeline and root bindings agree; failed acquisition preserves prior state.

### Task 1: Kernel, integration and parity controls

**Files:** `assets/shaders/world_radiance.slang`, `lib/engine/app/renderer/App.Renderer.WorldRadiance.cpp`, `lib/engine/tests/app/App.WorldRadiance.Tests.cpp`, owning codemaps.
**Interfaces:** new `[numthreads(64,1,1)] refineWorldRadianceParallel(uint3 group:SV_GroupID,uint lane:SV_GroupIndex)`; private program/pipeline for window roots, dispatch group count equals rows. Public API unchanged.

- [x] Tests first: new entry contract, heterogeneous nonblack-sky ordered coefficient/status/work parity; black-sky masked-bin and constant controls through existing fixture, retain partial/overlap/physical cases. Coordinator records missing-entry RED.
- [x] Implement four shared arrays and unique lane+64*k tracing with original helper; ordered projection in lane0. Reuse narrowly extracted projection operations if necessary; keep reference mathematical sequence unchanged. Preserve group counters and uniform barriers.
- [x] Add transactional private pipeline/root lifecycle and window-only group-count admission/encode. Update maps, self-review semantic grouping/layout and failure paths.
- [x] Coordinator format/style/build, exact ordinary `engine.tests.app.world_radiance_candidate_gpu`, then GPUAV twin, applicable asset check and independent source review. No unobserved success claims.

### Task 2: Performance qualification and restoration/promotion decision

**Files:** temporary Editor512selectors, diagnostic artifacts, spec/master/ledger.
**Interfaces:** existing actual-frame quantum and camera A/B hook, same workload and counter settings as previous baseline.

- [x] Freeze tested candidate; temporary direct-only512Editor settings; build app.game. Sole GPU execution owner runs same4K Frontier4096 seed42 A/B with existing watchdogs and metrics.
- [x] Verify query/segment/copy identity, quality receipts, actual-frame producer totals, request age, counter samples and interference. No quota/group sweeps; unprofiled confirmation only if gates support promotion.
- [x] Restore temporary Editor and rebuild. On failure archive candidate and restore exact bounded source delta; on success preserve verified implementation with explicit remaining gates. Independent final evidence review and reconcile master plan; no whole-goal completion claim.

Outcome: retained after independent final review. Profiled and unprofiled512-row witnesses pass the measured direct/discovery and warm-pan component gates. Production Editor quota remains256; default-quota freshness and integrated master-plan acceptance are not established by these512-row witnesses. See `out/diagnostics/windowed-native-20261106/parallel-rays/runtime/report.md`.
