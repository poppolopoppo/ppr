## Async execution and responsiveness

> **Document scope:** Design/proposal record, not a current implementation inventory or repository-wide instruction. Apply its task-specific requirements only when the active task explicitly adopts this design. Verify implementation claims against source/CMake and the current [documentation guide](../README.md); historical paths, counts and phase status may be stale.

**Update:** Architecture §4.2/§8–9; Roadmap P0/MVP-2; UX §2.1.

- **Keep input responsive:** input capture, camera, previews and UI must not wait synchronously for disk I/O, solver jobs or staging completion. Queue tick-stamped intents; expose pending/rejected operations rather than silently dropping them.
- **Async persistence:** establish a coherent, immutable checkpoint at a defined tick boundary. Compression and filesystem I/O run off the tick thread. Workers never read mutable live ECS/grid storage. Bound capture memory and queued saves; measure capture stalls separately from write latency.
- **Measured solver parallelism:** retain single-threaded execution initially. When profiling warrants workers, partition independent chunk work using immutable inputs/halos and worker-owned outputs. Assign cross-partition faces exactly once; merge transfers in stable partition/face order, enforcing aggregate donor limits. Workers never mutate authoritative ECS.
- **Deterministic scheduling:** worker completion order must not determine simulation results or which work advances. Use fixed work-count budgets and persistent carry-over cursors. If required results are late, delay the dependent tick phase—not input handling or deterministic work selection.
- **Double-buffer render staging:** produce into one owned buffer while presentation consumes the other. Reuse only after consumer/GPU completion permits it. If both are busy, retain the last valid presentation and coalesce pending updates; do not block input or discard authoritative changes.
- **Bound everything:** job queues, scratch, checkpoint captures and staging buffers require explicit limits, backpressure and cancellation/teardown ownership.

**Parallelism acceptance gate:** demonstrated performance improvement, no input-latency regression, bounded queue age/RSS, deterministic replay and checkpoint-continuation equality. Switching execution mode must preserve canonical results; otherwise treat it as an explicit compatibility change, not a transparent optimization.
