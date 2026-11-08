# Parallel face moments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Track steps below.

**Goal:** Reduce the measured warm32–34ms reflected-light face-moment cost without changing ray math or ordered sums.
**Architecture:** Retained serial control plus64-thread group-per-face entry using existing shared ray arrays, selected by pass-owned pipeline. Existing immutable lifecycle and uploaded ABI unchanged.
**Tech Stack:** C++23 modules, Slang2026.17.1, pinned Slang-RHI, Vulkan/RTX2080; Windows/D3D12 supported but unavailable here.
**Spec:** docs/superpowers/specs/2026-11-06-parallel-face-moments-design.md

## Global constraints

Preserve current dirty checkout and all unrelated edits. No commits/staging/reset/new HEAD-only worktree. Orchestrator owns builds/execution; author edit-only. No quota/scheduling/traversal/ABI changes. Keep6ms/1s/quality/resource gates. All barriers uniform, original phi summation order and exact integer counters. Reuse existing source/pipeline/fixture facilities.

## Review focus

- All four normals and skipped inward directions; no stale shared bins.
- Non-multiple face counts, firstRecord offsets and device group-count bounds.
- Exactly one output row per face, exact aggregate status/work counters.
- Ordered original weighting and accumulation; no FP reduction reassociation.
- Existing serial path retained solely as qualification control; owned pipeline teardown remains correct.

## Task1: Parallel face entry and correctness

Files: assets/shaders/world_radiance.slang; lib/engine/app/renderer/App.Renderer.WorldRadiance.cpp and only necessary existing internal declaration seam; lib/engine/tests/app/App.WorldRadiance.Tests.cpp; relevant codemaps if entry inventory changes.

- [ ] Author focused entry/reflection or parity regression first; root runs meaningful failing check before production implementation.
- [ ] Add parallel entry and pass-owned pipeline/dispatch selection, retaining serial control without new public tuning option. If existing fixture cannot access serial control, report the narrow test seam to coordinator before changing public API.
- [ ] Compare serial/parallel moments/final output within1e-5 and exact counters on normals/tail/sky/media/partial-Matter fixtures. Preserve existing quality controls.
- [ ] Root format/style, affected Dev app test build, registered ordinary/GPUAV world checks. Independent source/lifetime review; resolve findings without redesign.

## Task2: Optimized integrated qualification

Baseline frozen at out/build/clang-rel/game/app.game.integrated-20261106 with full manifest in out/diagnostics/windowed-native-20261106/optimized-integrated/.

- [ ] Build Release candidate and freeze matching shader/source identity.
- [ ] Repeat same4096²/4K/32-cell-pan integrated Matter/one-bounce/temporal capture; performance validation-off, correctness separately. Preserve Auto power mode.
- [ ] Compare warm moments and frame-joined producer timing, first-Matter age, capture history and clean shutdown. No automatic quota change on age failure.
- [ ] Retain only with correctness and material measured benefit; record remaining full-goal gates. Restore only this candidate if rejected.
