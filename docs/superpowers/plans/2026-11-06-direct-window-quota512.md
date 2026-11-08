# Direct-window512 Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans. Steps use checkbox tracking.

**Goal:** Persist the user-approved512 direct-only receiver quota.
**Architecture:** Two existing Editor settings sites select512 for direct-only and256 for bounce. Public default and other paths unchanged.
**Tech Stack:** C++23, CMake, existing Vulkan capture harness.
**Spec:** docs/superpowers/specs/2026-11-06-direct-window-quota512-design.md

## Constraints and review focus

Preserve dirty checkout; no commit/reset or HEAD-only worktree. Review initial admission and camera replacement selections; bounce/Matter and public default remain unchanged. No new ownership or data layout. Reuse prior identical512 source/runtime evidence and ordinary/GPUAV/asset results; no mirror-of-assignment unit test.

## Task1

- [x] Snapshot Editor; add `command.m_single_bounce ?256u :512u` in beginTerrainLighting_ direct-window branch and `state.m_bounce_enabled ?256u :512u` in updateTerrainLightingView_ settings.
- [x] Format/style Editor and build app.game; verify identical Editor bytes to successful512 witness.
- [x] Run existing Frontier4096/4K overlapping-pan harness with persistent512 and documented sanitizer policy,600s app/720s external bounds; retain hashes, exact env, timing/work/age/suppression evidence. Require exit0, matching workload, measured producer<=6ms and age<=1s.
- [x] Independent bounded delta/evidence review and update master; keep broader acceptance gaps explicit.
