# Prepare required discovery pipeline before field execution

Evidence: triangle-readiness/clock-observation runtime log records discoverWorldFaces compilation25261.9ms between completed direct field and discovery; discovery then fails6ms at15.120384ms, clock window P8/300–630MHz (approximate alignment). This is a startup preparation gap, not proven shader throughput regression. Preserve failed captures.

Coordinator ruling: explicitly prepare the discovery program only when the already-selected Editor route requires discovery (single bounce OR existing direct diagnostic discovery predicate). Perform it after successful field admission but before field encoding/state publication. Direct-only initialization remains lazy. No power configuration, fake GPU warmup, gate relaxation, shader/kernel/quantum/math change, or unrequested program sweep. This moves required work; performance effect must be measured.

## Global constraints

Preserve dirty particles checkout, exact before snapshots, no commits/staging/reset. Author edit-only, root builds/tests; no nested agents. Reuse immutable program/pipeline pair slot2 and existing create/commit logic. Owner-thread/known-state guards; cache-hit idempotence. Do not mutate batches, fences, publication/epoch, buffer admission or submission fate. Preserve prepareDiscovery memory admission before its lazy fallback. No stored members/layout change. Root/area AGENTS and shader/module/test authorities apply; errors through macros, logical paragraphs/section headers. Do not touch Game.Application.cpp (runtime wrapper later owns temporary hook).

### Task 1

Bounded files: WorldRadiance.cpp/.cppm, Editor.cpp, existing App.WorldRadiance.Tests.cpp and owning codemaps.

- [x] Snapshot exact files; test-first extend existing discovery fixture/checks to call new `WorldRadiancePass::prepareDiscoveryPipeline()` before the direct generation (and twice for idempotence), preserving discovery keys/statistics and existing fence/lifecycle assertions. Add appropriate unavailable/foreign-owner/unknown-state guards using existing meaningful fixtures, without broad duplicated cases. Stop for root RED missing-API build.
- [x] Extract the existing optional slot2 creation block into the fallible public operation. Validate initialized/current owner/known state consistently; successful no-work on prepared pair. Atomic local program/pipeline creation then commit both; existing profiling/log coverage retained. No batch/resource/fence action. `prepareDiscovery` calls it at its original post-admission point as lazy fallback, preserving non-Editor callers.
- [x] Editor computes existing discovery-enabled predicate once and prepares only for that selected route after successful direct-field admission, before state commit/field submission. Reuse predicate for state.m_discovery_enabled; preserve ordinary direct-only routes. Check existing setup failure/cancellation cleanup before author handoff; no changed owner or new recovery framework.
- [x] Root scoped format/style, affected app/game tests build, existing relevant discovery/failure tests and bounded independent review.
- [x] One unchanged4096²/128-cell camera run with read-only GPU telemetry; record compile order, direct/discovery GPU timings, freshness and raw exit. Restore hook/rebuild. Retain prior failures and report performance unverified if budget still fails; do not repeatedly retry.

Execution complete but acceptance FAILED: initial discovery4.092ms passes, warm discovery6.400ms and2446.642ms fail; noB. See out/diagnostics/windowed-native-20261106/discovery-readiness/report.md. A user-approved power-policy diagnostic is pending; no setting changed. Full goal active.
