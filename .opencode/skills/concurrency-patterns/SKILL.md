---
name: concurrency-patterns
description: >
  Use this skill whenever you need to implement or reason about concurrent
  message passing, cancellation, event multiplexing, or async I/O in the PPR
  game engine. It covers the core concurrency abstractions — RawChannel
  (lock-free MPSC ring buffer), IEvent/ISignal/Signal (compile-time event
  multiplexing), IContext/SharedContext (cancellation tree),
  ParallelScheduler admission, and the Application exit latch — along with
  their thread-safety model, HAL I/O integration, testing strategies, and
  performance-critical design decisions.
---

# Concurrency Patterns Guide

## Contract

This skill is a reference guide for the PPR concurrency abstractions —
RawChannel (lock-free MPSC ring buffer), IEvent/ISignal/Signal (compile-time
event multiplexing), IContext/SharedContext (cancellation tree),
ParallelScheduler admission, and the Application graceful-exit latch — along
with their thread-safety model, HAL I/O integration, testing strategies, and
performance-critical design decisions. It **does not** edit concurrency code
or modify engine state. Its output is a structured guide for choosing and
composing the right primitive for a given scenario.

## Mandatory pre-implementation / review checklist

- Assign ownership and lifetime for every channel, event, context, request,
  worker, and callback capture using the vocabulary in §1; stable-address
  in-flight objects outlive completion or cancellation acknowledgement.
- Specify boundary outcomes (`full`, `empty`, `closed`, cancellation, timeout,
  and OS error) and the caller's drain/retry/propagation rule.
- Inject a clock, deadline source, or tick input into time-dependent logic and
  tests; do not make a worker, timeout policy, or test depend on an implicit
  wall clock.
- Write the shutdown sequence under
  [AGENTS.md's teardown contract](../../../AGENTS.md#ownership-lifetime-and-teardown),
  applying the event-specific quiesce-before-unsubscribe rule in §1. Preserve
  the first cleanup error.
- Document each cross-thread publication with its release/acquire (or stronger)
  pairing; preserve the established ordering unless a proof and tests justify
  a change.
- Never invoke an externally controlled callback while holding an internal
  lock. Define unsubscribe/detach ownership and test cancellation-versus-notify,
  close-versus-send, and destruction races.
- Add behavior-contract tests for the outcomes above. For test file layout —
  private group `.cpp` files, `detail::` leaves, TU-local groups + accessors —
  see `unit-test-updater`.

## Execution handoff

This skill defines concurrency and teardown procedure only. The active OMO
preset is the sole authority for actor selection, permissions, tool access,
parallel work, and session reuse. Apply the procedure only through the active
configuration; this document grants or routes none of them.

---

## 1. Ownership and lifetime vocabulary

Apply [AGENTS.md's ownership and lifetime contract](../../../AGENTS.md#ownership-lifetime-and-teardown).
Concurrency-specific owners are `SharedContext` and `SharedRawChannelPtr`; workers
and services are `unique_ptr`-owned. Event subscriptions (`TagPtr<ISignal>` handed
to `subscribeEvent`), spans and views over channel records, and raw-pointer
callback captures never own and never extend a lifetime.
- A subscription does not keep its event alive, and an event does not keep its
  subscribers alive. Disable new work, quiesce in-flight emitters, then
  unsubscribe and destroy subscribers before destroying their event sources.
  **Quiesce** means drain in-flight work to idle before releasing storage. A
  **latch** is sticky state that never clears (a recorded cancel error, the exit
  flag). A **cause** is the `error_code` carried by a latch.

---

## 2. Thread-local storage and cross-thread handles

Apply [memory-allocator's TLS and cross-thread handle contract](../memory-allocator/SKILL.md#2-thread-local-storage-and-cross-thread-handles).
For channel and I/O integration, prove that borrowed buffers remain valid through
consumption or OS completion/cancellation acknowledgement.

---

## 3. Quick Reference — Choosing a Concurrency Primitive

| Pattern | When to Use | Key Classes | Partition |
|---------|------------|-------------|-----------|
| **RawChannel** | Async MPSC message passing between threads. Fixed-capacity, lock-free consumer path. | `RawChannel`, `Channel<T>`, `ChannelWriter<T>`, `ChannelReader<T>` | `engine.core:concurrency.channel` (`Core.Concurrency.Channel.cppm`) |
| **IEvent / Signal** | Wait on one or more event sources (I/O, timers, channels). Compile-time multiplexing with `select()`. | `IEvent`, `PulseEvent`, `BroadcastEvent`, `NeverEvent`, `Signal<EventsT...>` | `engine.core:concurrency.event` (`Core.Concurrency.Event.cppm`) |
| **IContext** | Cancellation tree with deadlines. Propagate a cause to workers. | `IContext`, `SharedContext`, `context::background/withCancel/withCancelClause/...` | `engine.core:concurrency.context` (`Core.Concurrency.Context.cppm`) |
| **ParallelScheduler** | Fixed worker pool with admission control and draining join. | `ParallelScheduler`, `WaitGroup` | `engine.core:concurrency.scheduler`, `:concurrency.thread` |
| **Application exit** | Graceful process shutdown, distinct from cancellation. | `Application::requestExit/isExitRequested/run` | `engine.app:application` (`App.Application.cppm/.cpp`) |

### Required task-selected references

Read the matching references before implementing or reviewing that task; read
all matching rows when composing primitives. Section numbers are retained so
existing procedural references remain recognizable.

| Task | Required detail |
|---|---|
| Channel construction, capacity, backpressure, typed API, close/flush, event integration | [RawChannel (§4)](references/raw-channel.md) |
| Event subscription, Signal reset/rearm, cancellation factories, values, deadlines, callbacks | [Signals and contexts (§5–6)](references/signals-contexts.md) |
| Scheduler admission/WaitGroup, application exit, HAL I/O request lifecycle | [Scheduler and I/O (§8–10)](references/scheduler-and-io.md) |
| Tests, API examples, select workers, fan-out shutdown, timeout composition | [Tests and idioms (§11, §13)](references/tests-and-idioms.md) |

### Which abstraction and lifecycle

- Use `RawChannel` / `Channel<T>` for asynchronous fixed-capacity MPSC transfer
  with explicit backpressure and one dedicated consumer. Do not use one channel
  for multiple consumers; fan out over one channel per consumer. Do not use
  heap-allocated variable-length messages without a pool: records are inline
  `RecordHeader` + payload slots.
- Use `Signal` for waiting on event sources, not ownership or persistent
  cancellation state. Use `IContext::error()` for the stable cancellation cause;
  use the application exit latch for graceful exit without cancellation.
- Use `ParallelScheduler` for fixed-pool admission and draining join. Admission
  gates on persistent `error()`, not a consumable pulse; the reference retains
  the pulse-without-error fallback and exact group/error-settlement policy.
- Close is terminal. After `producerReserve` reports `error_closed`, stop
  admission. After `consumerAcquire` reports `error_closed`, do not perform
  another blocking acquire: the close record is already consumed. Track closed
  state or use nonblocking peek. Keep exactly one consumer per channel unless
  an additional close barrier is added.
- Cancellation does not close channels or join workers. Stop admission, settle
  producer reservations, flush if needed while consumers are active, then
  close; consumers drain to `error_closed`, then the owner joins and frees
  storage. Specify drain-versus-discard policy. Never flush stopped consumers
  or wait for close observation before issuing close.
- Subscriptions never own. Never destroy a signal during another thread's
  `emitEvent` or an event while another thread may call `notify` on it. Join,
  close, or drain emitters before unsubscribe/destruction; tear down context
  children before their parent under the owner's join.
- Subscribe before checking the stable error latch and before the first wait;
  keep the subscription through checks/waits and check again after wakes.
  Late broadcast subscription does not replay cancellation. A context child
  observes a cancelled parent through `error()`, even when its own pulse never
  fired.
- Range-for over `select()` owns reset on increment; never rearm/reset inside
  its body. Manually rearmed sources use `poll → handle → resetEvent → wait`
  directly on the event instead.
- Deadlines use an injected, actively ticked `TimerManager`; scheduling does
  not start a timer thread. Blocking the same thread without ticking cannot
  fire the timeout. Tests advance an injected clock and tick deterministically.
  Detach callbacks and drain/join before releasing contexts and the timer owner.

## 7. Thread Safety Model

| Operation | Thread Safety | Mechanism |
|---|---|---|
| Channel producer ops / `close` / `flush` | Yes (multi-producer) | `m_producer_mutex` + CAS on status |
| Channel consumer ops | Single consumer only | Lock-free `m_commit`/`m_read`, no locking |
| `isOpened` / `isClosed` / `capacity` | Yes | Acquire loads |
| `PulseEvent` all ops | Yes | Atomics; single subscriber |
| `BroadcastEvent` all ops | Yes | `test_and_set` + mutex-protected snapshot |
| `Signal::poll/wait/begin` | Single consumer | `notify()` is cross-thread |
| `cancel()` / `cancelCause()` | Yes | CAS on the error slot; first cause wins |
| `error()` / `value()` | Yes for reads | Atomic load + immutable parent chain |
| Context construction | Sequenced only | Parent must outlive children |

Allocator thread safety (`GPA`/`OS` global, page-cache TLS, `Pooling`
lock-free, arenas single-threaded, `ScratchPad` per-thread) is specified in
`memory-allocator` §Constraints and not restated here.

## 12. Performance Considerations

- `RawChannel` is cache-line aligned; `m_commit` and `m_read` sit on separate
  lines (producer-written vs consumer-written). `Signal<EventsT...>` is
  cache-line aligned. `RecordHeader` is 8-byte aligned (`alignas(u64)`).
- Drain everything per wakeup: on each select wakeup, loop
  `consumerAcquire(peek_without_blocking)` until empty before waiting again.
  I/O completion drains process up to 64 per call — finish the batch before
  re-waiting.
- `Signal::poll()` is one atomic RMW per poll in the uncontended case;
  `wait()` parks on a counting semaphore. For long-lived loops, build the
  `Signal` once and reuse it; construction subscribes and destruction
  unsubscribes each source.
- `m_producer_mutex` serializes all producers: batch reserves per lock
  acquisition, or shard across channels (one consumer each, atomic-counter
  fan-out) when contended. `wait_if_full` under a slow consumer holds
  producers — use `drop_if_full` for latency-sensitive paths.
- Per-message cost is `sizeof(RecordHeader)` + 8-byte-aligned payload;
  uniform message sizes minimize waste. The ring pre-allocates once — no
  allocation on the send/receive hot path.

## 14. Constraints and Pitfalls

- Call `producerSubmit` (or `producerDiscard`) exactly once per reserved
  record. Leaking a reservation stalls `advanceCommit_()` on the busy header
  and eventually blocks the channel.
- Always `consumerRelease` after a successful `consumerAcquire`; otherwise
  producers eventually cannot reserve.
- `IoRequest` is move-disallowed and non-copyable — keep it at a stable
  address while in-flight (`unique_ptr` if it must move).
- Context values are immutable after construction; attach everything the scope
  needs up front.
- `PulseEvent` has exactly one subscriber; `BroadcastEvent` trades a mutex
  for many. Late broadcast subscribers get no replay
  ([§5.3](references/signals-contexts.md#53-event-implementations)).
- `Signal::iterator` is input-only, non-reentrant, single-consumer; it blocks
  in `wait()` when nothing is pending.
- Custom cancel causes must be `error_code`-compatible; an empty clause is a
  no-op by contract ([§6.2](references/signals-contexts.md#62-withcancel--withcancelclause--manual-cancellation)), never a silent cancel.
- Worker test bodies never assert (`PPR_TEST_ASSERT` throws): record in atomics
  and assert in the parent. Use deterministic latch/atomic gates and scope-guard
  release so a throwing assertion cannot strand a worker.
- Implementation: `Core.Concurrency.Channel/Event/Context/Scheduler/Thread`
  partitions; tests `Core.Concurrency.{Channel,Event,Context,Scheduler}.Tests.cpp`
  and `App.WindowInput.Tests.cpp` (exit path).
