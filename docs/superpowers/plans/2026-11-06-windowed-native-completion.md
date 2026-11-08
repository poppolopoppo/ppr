# Windowed native transport completion plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox tracking.

**Goal:** Qualify the approved demand-window native transport without changing its lighting values.

**Architecture:** Extend the existing strip scheduler with an explicit receiver quota. Preserve immutable source identity, native CH overlap, one actual-frame quantum and complete fenced publication. Reuse existing fixtures and runtime receipts.

**Tech Stack:** C++23 modules, Slang, Slang-RHI, Vulkan on Linux, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-11-05-windowed-direct-native-transport-design.md`, execution reconciliation dated 2026-11-06.

## Global Constraints

- 256 midpoint directions, unchanged signed FP32 order-4 CH and transport math.
- Brightness error ≤15%, blocked leakage ≤1%; coefficient control tolerance 1e-5.
- GPU lighting ≤6 ms normal / ≤10 ms stress, update age ≤1 s, peak lighting ≤4 GiB including overlap.
- Full 4096² source/occluder domain; visible plus explicit gameplay receiver demand.
- No shader math, source omission, reduced validation, deadline increases or quota sweeps.
- Preserve pre-existing staged/unstaged work. Work in the current feature checkout; do not commit the user's accumulated implementation or rebuild a HEAD-only worktree that omits it.
- Parent owns builds/tests; delegated authors edit only. No parallel source authors.

## Review Focus

- Partial world-edge pages must neither skip nor duplicate receivers.
- Invalid quota admission must preserve the previous usable generation.
- Changed source/cohort must prevent stale overlap copies.
- Quota changes must not change coefficients or bypass u32 counter safety.
- Copy-only replacement must retain a real fence and complete-publication checks.

### Task 1: Explicit bounded receiver quota

**Files:** `App.Renderer.WorldRadiance.cppm/.cpp` in `lib/engine/app/renderer/`; `lib/engine/tests/app/App.WorldRadiance.Tests.cpp`; renderer codemap.

**Interfaces:** Add `u32 WorldRadianceSettings::m_receiver_rows_per_quantum{256u}` after the receiver window (preserves positional aggregate initializers; verify resulting size/alignment). Direct-window accepts multiples of 32 in [32,1024]; other modes ignore it. No new owning type or shader ABI. Existing `pendingBatches()` exposes completion work.

- [ ] Add interface plus tests first. Extend the existing fixture's `generate` with trailing quota argument default256; select512 in the two physical holdouts. Add a 64×32 sky control requiring four batches at512 (old scheduler yields eight); compare all27 coefficients and status against the existing256 result. Preserve defaults of old controls.
- [ ] Build `engine.tests.app`, run registered ordinary `engine.tests.app.world_radiance_candidate_gpu`, and retain the expected batch-count failure before implementation.
- [ ] Implement quota-derived strip height `min(32, quota / page_width)`, preserving page masks and accumulation order. Replace fixed256 dispatch/segment admission bounds with the same admitted quota capped by page size. Reject invalid quota before mutation. Scheduling-only quota changes do not disqualify a matching donor.
- [ ] Extend existing shared controls for invalid0/31/1056, partial world-edge pages, quota-changed overlap/copy-only byte identity, and source/revision invalidation. Use no new fixture or CTest entry.
- [ ] Run format fix/check, touched-file semantic style, affected build, ordinary then GPUAV on ordinary success. Independent review covers layout, counters, identity, teardown and source grouping.

### Task 2: Actual-frame integration and bounded measurement

**Files:** `lib/engine/app/App.Application.Editor.cpp`, existing runtime diagnostic artifacts; no new public frame transaction.

**Interfaces:** Set the512 experiment quota at direct-only initial and camera-replacement settings creation after Task1 passes (leave single-bounce/Matter settings at256); reuse actual-frame work token, pending-request elapsed values and `WORLD_DIRECT_QUANTUM` receipts.

- [ ] Locate every real direct-window settings creation and preserve legacy/reference settings. Audit age origin, matching publication, retained-picture behavior and coalescing using the existing injected elapsed time.
- [ ] Build affected Game/asset targets and run applicable Editor/lifecycle checks; reuse unaffected evidence only with matching source closure.
- [ ] Run one bounded actual-world static/edit/cold-pan witness with existing harness. Preserve raw child exit, source/build identity, complete frame-joined GPU timing, request-to-matching-publication age and overlap ledger. Do not equate modeled reserve with live VRAM.
- [ ] If measured cost/age fails, stop tuning and record the architectural feasibility blocker. Do not promote the candidate or claim the whole plan complete. If it passes, advance remaining source/media/feedback/gameplay/postprocess gates from the master plan with separate concrete acceptance scopes.

### Task 3: Reconcile delivery evidence

**Files:** approved spec and `docs/plans/hrc-v22-simplification-proposal.md`.

- [ ] Record source review, exact commands, pass/fail/blocked gates and remaining work. Keep old failed evidence and the revised6ms budget explicit.
- [ ] Independent final review; no merge, push or broad staging of pre-existing changes.
