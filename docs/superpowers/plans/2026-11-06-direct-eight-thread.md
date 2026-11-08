# Eight-thread direct-window implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox tracking.

**Goal:** Test the measured parallelism limit with eight-thread direct-window groups while preserving lighting values.
**Architecture:** Add a pass-owned compute entry point invoking unchanged `worldRefine`; select it only for windowed direct transport. Keep existing 64-thread controls and lifecycle.
**Tech Stack:** C++23, Slang, Slang-RHI/Vulkan, CMake/CTest, NVIDIA Nsight Systems.
**Spec:** `docs/superpowers/specs/2026-11-06-direct-eight-thread-design.md` (approved).

## Global Constraints

- Eight threads only in direct-window refinement; unchanged 256 directions, FP32 operations and per-receiver accumulation order.
- Preserve quotas, page masks, source identity, overlap, complete fenced publication and error/teardown behavior.
- No public group-size setting, shader payload ABI change, group-size sweep or quota sweep.
- Coefficient tolerance1e-5; brightness15%, leakage1%; normal GPU6ms/stress10ms, age1s, lighting4GiB.
- Current dirty feature checkout contains required user work; preserve it. No commit/stage/reset or HEAD-only checkout. Coordinator owns builds/tests; author edits only.

## Review Focus

- Partial last group reaches all shared-counter barriers and writes each valid receiver once.
- All eight counter slots initialize/flush; extra groups preserve totals and u32 bounds.
- Root shader objects use the selected program, including reused pass state and cancellation.
- Device dispatch admission uses the same group width as encoding.
- Legacy straight/cascade and bounce retain their existing entry points and output.

### Task 1: Shader, pass binding and focused regression

**Files:** `assets/shaders/world_radiance.slang`; `lib/engine/app/renderer/App.Renderer.WorldRadiance.cpp`; `lib/engine/tests/app/App.WorldRadiance.Tests.cpp`; owning codemaps as necessary.
**Interfaces:** new `refineWorldRadianceWindow(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex)` with `[numthreads(8,1,1)]`, forwarding to `worldRefine(tid,lane,0u,false)`. Private pass pipeline/program storage selects the new entry for windowed generation only. Public settings/layouts unchanged.

- [x] Extend the existing shared fixture with entry-point/group-size contract and windowed/control parity for coefficients, status and query/segment totals. Use an all-directions sky control so page culling cannot invalidate equal-work expectations; retain existing narrow6x33 and overlap tests.
- [x] Coordinator builds `engine.tests.app` and runs exact ordinary `engine.tests.app.world_radiance_candidate_gpu`; record expected missing-entry/group-contract failure before production edits.
- [x] Add forwarding entry, matching pipeline/root selection, eight-thread admission and dispatch ceiling. Preserve transactional pipeline creation/cleanup. Do not edit transport math or counters.
- [x] Run formatter/style, affected build, exact ordinary leaf then `engine.tests.app.world_radiance_candidate_gpu_validation`; both must pass with unchanged physical gates. Independently review isolated delta and test evidence.

### Task 2: Same-world performance decision and evidence

**Files:** temporary `lib/engine/app/App.Application.Editor.cpp` experiment selectors only; diagnostic artifacts, approved spec, master plan and ledger.
**Interfaces:** existing 512-receiver A/B hook and raw `WORLD_DIRECT_QUANTUM` receipts; existing Nsight launch wrapper adapted to fresh output/source hashes.

- [x] Freeze candidate after correctness; build `app.game` with the same two direct-only512 Editor selections used previously. Preserve baseline Editor bytes.
- [x] Sole execution owner runs Frontier4096 seed42 at3840×2160, same camera A/B, core validation enabled, GPUAV disabled. Use existing bounded watchdogs and capture hardware metrics to test SM coverage; retain run/trace/source identity and raw exit.
- [x] Compare frame-joined producer cost, cold/warm age and counter sampling/context evidence. Do not infer full-pipeline performance or improvement from clocks/one pair alone; unprofiled confirmation follows only if candidate supports promotion.
- [x] Restore Editor bytes and rebuild default256. Reconcile result and next decision in spec/master/ledger, with independent final review. No promotion on failure; no whole-goal completion from this component.

## Outcome

Completed as a rejected experiment, not a production optimization. Warm11.702336ms fails6ms; original dispatch restored, baseline build passed, independent final evidence/restoration review approved. See the approved spec result and eight-thread/runtime/report.md. The full master plan remains incomplete.
