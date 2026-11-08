# Fixed-position emitter relocation

Status: approved by the user on2026-11-06 with explicit instruction to implement.

## Intent and scope

Implement the master plan's smallest playable emitter-movement experiment in the existing lighting showcase. The user selected two fixed positions first instead of arbitrary valid placement controls. Add one reversible Move lamp action alongside the current door/lamp controls; its label identifies the destination. Preserve lamp on/off state, four-cell shape, material, temperature and authored source strength. Other emitters remain unchanged.

The proposed slots are the current amber lamp at x30..31,y16..17 and the vacant slot at x37..38,y16..17. The second slot is the coordinator's implementation choice, grounded in the current showcase recipe: right of the door lintel and left of the pipe atx39. This moves the light across the door-side geometry without altering the scene's walls or dressing.

## Approaches

The user chose two fixed positions: small UI and validation surface, reversible, exercises real source relocation through the existing transaction. Arbitrary valid placement would require placement interaction, broader collision/occupancy rules and additional supported-world admission; it is deferred. Both must use the Driver transaction; direct grid mutation is incompatible with the existing publication contract.

## Owned state and transaction

Extend the existing service-bay target and committed/candidate lamp state with a typed two-position choice. Use the same pending edit slot, monotonic revision allocation, frozen candidate chunk, inactive colliders, optional Matter source/target binding, graphics activation acknowledgement, Driver commit and matching-present release. No new transaction framework or Renderer-owned scene state.

Validate current revision, transaction availability, exact four-cell source at the committed position, absence of duplicate amber sources, destination vacancy and actor overlap before exposing a candidate. Reject invalid/occupied destinations or concurrent work through the existing error contract. Stage removal of all four old cells and insertion of all four new cells in the same candidate; transfer each lamp cell including its temperature and preserve the displaced vacuum cell values at the old location. Committed grid/source/position stay unchanged until the existing commit boundary. Keep on/off independent from position so switching after a move and moving while off both work.

Update the presentation/source builder's current hard-coded amber-coordinate invariant to the candidate or committed position belonging to the exact revision. Do not weaken source-shape or duplicate-source validation. Grid uploads and immutable optical source must describe the same old/new occupancy. Rebuild the candidate collider cohort through the existing path.

Retain current cancellation, stale acknowledgement, rejected/unknown submission and teardown behavior. Cancellation before commit restores the original position and cells; reverse movement is a subsequent normal transaction. No additional clocks, GPU resource ownership or asynchronous callbacks. Review layout for any changed state types and preserve destruction dependencies.

## UI and verification

Expose one move action in the existing colony panel; disable it while an edit is pending and keep the existing Cancel operation. Route the intent through the same Game edit workflow and record request-to-matching-present age using its existing explicit clock.

Before product edits, add focused tests for forward/reverse movement, off-state movement and switching at the new slot, exact source count/temperature preservation, destination/actor rejection, revision/busy rejection, cancellation and stale acknowledgements. Extend existing Driver/presentation fixtures; test candidate grid and optical labels together. Reuse unchanged unknown-fate controls where applicable; add a relocation-specific assertion where the new state crosses their boundary.

Build affected game/app tests, apply format/style gates, run the relevant existing CPU/GPU transaction and source checks, then exercise actual playable forward/reverse movement with matching revision/present evidence. Retain the unchanged15% brightness/1% blocked-leakage limits and report edit latency against1s; test success or a small scene does not establish full-world cost/age. An independent bounded review closes this increment.

## Remaining full-plan requirements

This increment does not supply arbitrary generated-world source edits, recursive diffuse feedback, scattering, gameplay mechanics or the complete4K60/live-memory qualification. Those remain explicit work, not excluded final goals. No shader math, quota change, new art, source-strength tuning or general placement system is part of this bounded change.
