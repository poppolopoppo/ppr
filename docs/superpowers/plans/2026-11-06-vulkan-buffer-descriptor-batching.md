# Vulkan buffer descriptor batching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Track steps below.

**Goal:** Reduce repeated native descriptor update overhead while preserving results and establish optimized performance independently of validation overhead.

**Architecture:** Hash-checked private Vulkan source overlay, local 32-write raw-buffer batches, unchanged immediate other-descriptor paths. Preserve the current dirty particles checkout; no commits, staging or HEAD-only checkout. Orchestrator owns all builds and executions.

**Tech Stack:** C++23 engine, pinned Slang-RHI a78e208, Slang 2026.17.1, CMake, Linux Clang/libc++, Vulkan RTX2080.

**Spec:** docs/superpowers/specs/2026-11-06-vulkan-buffer-descriptor-batching-design.md

## Global constraints

- Preserve both validation layers for correctness; measure optimized validation-off and validation-on configurations separately.
- Preserve shaders, descriptor payload/order, resource lifetime, dispatches, barriers, quotas and GPU Auto policy.
- No CPM-cache edits, upstream pin change, RHI migration or new global/TLS state.
- Do not discard existing staged/unstaged work. Baseline is current worktree, not HEAD.

## Review focus

- Data pointers remain valid at native call, including 32/33/96 writes.
- Mixed descriptor types preserve order; pending buffers flush before recursion.
- Early returns preserve previously issued writes; empty flush makes no API call.
- No allocation per descriptor; helper is noncopyable with verified bounded size.
- Dependency hash/registration guards reject drift and preserve other overlays.

## Task 1: Patch and focused verification

Files: new cmake/external/PatchSlangRhiDescriptorBatching.cmake; targeted integration in SlangRHI.cmake; third_party/SlangRHI/README.md and cmake/external/codemap.md; diagnostic verification harness under out/diagnostics/windowed-native-20261106/descriptor-batching/.

- [x] Author hash-checked private vk-shader-object.cpp overlay; initially leave integration inactive while baseline is built/captured.
- [x] Verify local batch behavior with captured native-call payloads at empty,1,32,33,96, mixed-type, recursion and early-exit boundaries. Test original generated helper, not a reimplementation.
- [x] Independently review patch for source-registration, ownership/order and failure handling.
- [x] Activate overlay only after baseline source/binary identity is frozen; configure twice to verify idempotence, build affected app/asset closure.
- [x] Run selected Matter, ordinary/GPUAV world and asset checks. Matter and both world checks pass; ordinary asset passes, asset GPUAV times out at300s. This is an unmet gate, not a pass. Exact helper/traversal tests supply batch boundary evidence.

## Task 2: Optimized baseline and paired measurement

Artifacts: out/diagnostics/windowed-native-20261106/descriptor-batching/; reuse current 128-cell pan diagnostic and same workload/telemetry watchdog, with exact restoration snapshots.

- [x] Configure the existing clang-rel preset against current source; verify Release optimization and sanitizers off from actual compiler commands. Preserve separate Dev sanitizer evidence.
- [x] Freeze unpatched optimized executable and assets; run normal validation-off baseline and separate core-validation-on observation with identical workload, power policy and output settings. GPU-assisted correctness is separate from timing.
- [x] Build patched optimized executable; compare matching cases, native update call counts, GPU work counts and observable output. Include 4K/4096 demand readiness, matching presentation and shutdown.
- [x] Apply retention criterion: withdraw the patch because no material end-to-end benefit was demonstrated; preserve the experiment and unmet asset GPUAV timeout. Record variance and open full-plan gates. No physical cadence/live VRAM claim.
