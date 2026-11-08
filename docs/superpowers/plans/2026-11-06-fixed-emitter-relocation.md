# Fixed Emitter Relocation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Move the existing showcase amber lamp reversibly between two fixed slots through the owned edit/publication transaction.

**Architecture:** Extend the existing Driver service-bay transaction and its Game/UI caller. Candidate cells, optical labels, collider cohort and committed position share the same revision and publication boundary.

**Tech Stack:** C++23 modules, existing engine containers/Expected/error macros, Slang-RHI publication, native colony UI.

**Spec:** docs/superpowers/specs/2026-11-06-fixed-emitter-relocation-design.md (user approved).

## Global Constraints

- Slots x30..31,y16..17 and x37..38,y16..17; preserve four-cell material/temperature, on/off and source strength.
- Preserve atomic candidate publication, known/unknown submission fate, cancellation, stale acknowledgement and partial-failure teardown.
- No direct live-grid mutation, separate transaction framework, quota/shader changes, arbitrary placement or art changes.
- Keep the dirty checkout intact. Use bounded before-snapshots; do not commit/stage/reset or create a HEAD-only worktree.
- Author is edit-only; coordinator owns builds/tests and records RED before production implementation. No nested agents.
- Review changed type layouts and logical paragraphs; root/area codemaps and specialist skills apply.

## Review Focus

- Moving while switched off must remain off; a later switch acts on the new slot.
- An occupied destination or actor overlap rejects without changing committed cells/state.
- Cancellation or stale activation/present acknowledgements cannot commit or undo a different revision.
- Duplicate/misplaced amber source cells remain rejected by presentation admission.
- Forward and reverse moves preserve displaced vacuum values and lamp temperatures, including after intervening door edits.

### Task 1: Atomic two-slot relocation with playable controls

**Files:**
- Modify game/colony/ColonyDriver.cppm and ColonyDriver.Edits.cpp for typed position/query/request, candidate/commit state and cell swap.
- Modify game/colony/ColonyDriver.cpp only where existing initialization/reset/copy paths require the new state.
- Modify game/colony/SimFramePacket.cpp for revision-matched source-shape validation at selected position.
- Modify game/Game.Application.cpp, game/ui/ColonyPanel.cppm and ColonyPanel.cpp for the move intent, queued request state, revision acknowledgement, UI destination label and existing diagnostic capture flow.
- Tests: game/colony/WorldGen.Preview.Tests.cpp, game/colony/Checkpoint.Gpu.Tests.cpp; game/ui/Game.UI.Tests.cpp only for affected existing adapter contracts.
- Update owning codemaps only for changed current behavior; no file split or new target required.

**Interfaces:**
- Add ColonyDriver::EServiceLampPosition : u8 { original, alternate }.
- Add Expected<u64> requestServiceLampPosition(EServiceLampPosition position, u64 expected_revision).
- Add EServiceLampPosition serviceLampPosition() const noexcept and carry the same typed value in ServiceDoorPublication.
- Add EServiceBayTarget::amber_lamp_position and EColonyIntent::move_service_lamp. Keep existing public door/lamp APIs and transaction methods unchanged.
- Existing private requestServiceBay_ may retain its bool target-value representation with validated typed conversion at the new public boundary; reject invalid enum values.

- [x] Read the spec, current transaction/callers and owning maps/skills. Save bounded snapshots of all touched files and inventory applicable reset/commit/cancel paths.
- [x] Extend existing Driver/source tests before production edits: assert old grid remains unchanged while candidate removes old four/adds new four; source labels match candidate slot; commit updates position only after matching activation. Test reverse move, unequal source/vacuum temperatures, off-state move then on, intervening door edit, occupied destination and actor overlap, invalid enum/current-position/stale-revision/busy rejection, duplicate-source rejection, cancel/stale ACK and revision nonreuse.
- [x] Stop for coordinator RED build/test. Missing new API compile failure is acceptable first RED; retain exact command and diagnostic. Do not implement until the coordinator releases this step.
- [x] Implement the exact typed API and transaction extension, preserving current error and lifecycle machinery. Reuse source validation and collider preparation, change all hard-coded lamp-position checks to the correct candidate/committed slot. Add no silent fallback.
- [x] Wire the existing Game service-bay workflow and panel Move lamp action. Preserve on/off, disable pending controls and expose existing Cancel. Extend the existing opt-in playable diagnostic with forward/reverse move receipts, preserving old switch/door cases and capturing matched source revision/present before success.
- [x] Author self-review: layouts, initialization/destruction dependencies, all lamp target branches, candidate/committed source projection, logical paragraphs and errors. Hand off exact changed-file inventory, tests, runtime invocation and diff; no builds/commits.
- [x] Coordinator: format fix/check, semantic checks; build app.game/game.tests and any affected app/asset target closure. Discover exact registered leaves and run focused CPU relocation/optical-source and GPU transaction tests serially. Preserve failure evidence and fix only within scope.
- [x] Run actual playable forward/reverse relocation with existing watchdog and documented sanitizer policy. Verify four-cell source replacement, retained old picture until matching publication, correct final on/off and request-to-matching-present age. Report actual quality/cost limits and unrun gates; do not equate CPU fixtures with world-scale acceptance.
- [x] Independent bounded spec/quality review; resolve findings, reconcile execution evidence and update master plan. This task closes only fixed-showcase movement, not the entire lighting goal.

## Validation ownership and execution

Use out/build/clang-dev-lighting-recovery-20261102. Coordinator selects current exact test entries from CTest/CMake; no competing GPU workloads. The cadence512-lifecycle capture currently owns Game.Application.cpp temporarily; no production edits until its wrapper confirms exact restoration. Start with read-only exploration and test drafting only after coordinator release. Accepted user instruction is approve-and-implement using economical Sol subagents; no additional execution-method confirmation is required.

## Execution result

Functional increment implemented and verified2026-11-06. Source/spec reviewPASS, app.game/game.tests buildPASS, format/stylePASS, focusedCPU relocation12.0997s andGPU transaction16.31sPASS, finalwholeGame suite133.796sPASS after preserving andcorrecting the stale shippedpalette assertion. Actualplayable5captures/revisions1..5 complete with rawexit0/212.589s andexistingNVIDIA-onlysanitizersuppressions.

Performance acceptance remains OPEN: forward1069.432112ms/reverse1084.606385ms FAIL1s underdocumenteddiagnosticpolicy. No wholeworld, physicalquality, memory/cadence orWindows/release claim. Checkboxes record performed implementation/verification steps, notpassedlatency. Nextwork is evidence-ledsharededitlatency investigation; fullmastergoal remainsactive.

Evidence: out/diagnostics/windowed-native-20261106/fixed-emitter-relocation/runtime/report.md; .superpowers/sdd/2026-11-06-fixed-emitter-relocation/progress.md andtask-1-review.md.

## Latency follow-up

The bounded makeMatterTerrain resident-view reuse now passes the unchanged1s gate in the matched showcase: forward564.881ms, reverse558.285ms. Five captures complete, rawexit0, no sanitizer/Vulkan error lines under existing policy, exact launch hashes match. Full-output contour parity, attached Matter GPU regression, independent source review, and wholeGame140.786s pass. Earlier latency failures remain historical evidence. This closes the measured fixed-showcase gate only. Evidence: out/diagnostics/windowed-native-20261106/fixed-emitter-relocation/terrain-lookup/report.md.
