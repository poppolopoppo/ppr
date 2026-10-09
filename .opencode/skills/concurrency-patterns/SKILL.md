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
- Write the shutdown sequence: first detach or disable callbacks,
  subscriptions, and submissions and prevent new work; then cancel, wake,
  drain, or join in-flight channels, I/O, and workers; then release storage and
  other resources, preserving the first cleanup error.
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

Use these terms the same way in every design note, review, and test. They
match the `memory-allocator` skill exactly.

- **Own**: `SharedContext` / `shared_ptr` (context tree, channel handles via
  `SharedRawChannelPtr`), `unique_ptr` workers and services, and any object
  whose destructor releases the resource. The owner outlives every observer.
- **Observe only — never own**: `safe_ptr` (debug-checked observer, raw
  pointer in release), event subscriptions (`TagPtr<ISignal>` handed to
  `subscribeEvent`), spans and views over channel records, allocator
  references held by `Allocator<>` wrappers, and raw-pointer callback
  captures. None of these extend a lifetime.
- A subscription does not keep its event alive, and an event does not keep
  its subscribers alive: order teardown explicitly (detach subscribers, then
  quiesce emitters, then destroy). **Quiesce** means drain in-flight work to
  idle before releasing storage. A **latch** is sticky state that never clears
  (a recorded cancel error, the exit flag). A **cause** is the `error_code`
  carried by a latch.

---

## 2. Thread-local storage and cross-thread handles

Scratch arenas, page-cache TLS caches, and pooling index hints are
**per-thread**: a handle or cached block obtained on one thread must never be
used or released on another.

- `ScratchPad` is a per-thread `Arena<SmallPage>`; an
  `Allocation<T, ScratchPad>` destroyed on another thread deallocates through
  *that* thread's TLS arena, so keep allocations on their originating thread.
- Channels must not carry pointers into producer scratch whose scope restores
  before the consumer reads them — copy the bytes into the record or extend
  the scope past consumption.
- Async I/O buffers must outlive scratch scopes until OS quiescence: the HAL
  owns the in-flight bytes until completion or cancel acknowledgement, so the
  backing scope may not restore while a request is pending.
- `HugePage` / `SmallPage` route through a TLS `LocalCache`; `Pooling` hints
  (`TPoolingHint::value`) are `thread_local`. These are safe to call from any
  thread precisely because each thread uses its own cache — never share the
  cached result across threads.

---

## 3. Quick Reference — Choosing a Concurrency Primitive

| Pattern | When to Use | Key Classes | Partition |
|---------|------------|-------------|-----------|
| **RawChannel** | Async MPSC message passing between threads. Fixed-capacity, lock-free consumer path. | `RawChannel`, `Channel<T>`, `ChannelWriter<T>`, `ChannelReader<T>` | `engine.core:concurrency.channel` (`Core.Concurrency.Channel.cppm`) |
| **IEvent / Signal** | Wait on one or more event sources (I/O, timers, channels). Compile-time multiplexing with `select()`. | `IEvent`, `PulseEvent`, `BroadcastEvent`, `NeverEvent`, `Signal<EventsT...>` | `engine.core:concurrency.event` (`Core.Concurrency.Event.cppm`) |
| **IContext** | Cancellation tree with deadlines. Propagate a cause to workers. | `IContext`, `SharedContext`, `context::background/withCancel/withCancelClause/...` | `engine.core:concurrency.context` (`Core.Concurrency.Context.cppm`) |
| **ParallelScheduler** | Fixed worker pool with admission control and draining join. | `ParallelScheduler`, `WaitGroup` | `engine.core:concurrency.scheduler`, `:concurrency.thread` |
| **Application exit** | Graceful process shutdown, distinct from cancellation. | `Application::requestExit/isExitRequested/run` | `engine.app:application` (`App.Application.cppm/.cpp`) |

### Quick API Patterns

```cpp
RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
auto reserved = chan.producerReserve(sizeof(MyData), RawChannel::wait_if_full);
if (reserved.has_value()) {
    new (reserved->data()) MyData{...};
    chan.producerSubmit(*reserved);   // takes RecordHeader&
}

auto signal = select(channel, *ctx);  // Signal over event references
for (auto &event : signal) {          // iterator auto-resets on ++ (§5)
    std::visit(pP::overloaded{
        [&](RawChannel *c) { /* drain */ },
        [&](IContext *) { /* cancelled */ },
    }, event);
}

auto [ctx, cancel] = context::withCancel(context::background());
cancel();                             // latches operation_canceled, emits
```

Return to the full guide below for detailed construction, lifecycle,
thread-safety, and integration patterns.

---

## 4. RawChannel — Lock-Free MPSC Ring Buffer

RawChannel is a multi-producer, single-consumer (MPSC) lock-free circular
buffer for fast message passing between threads, inspired by the BPF ring
buffer.

### 4.1 When To Use

Use RawChannel (or its typed wrapper `Channel<T>`) when you need:

- **Async producer-consumer** — one thread (or several) produces data, a
  single dedicated consumer thread processes it.
- **Low-latency** — the hot path (producer submit / consumer acquire) uses
  lock-free atomics; the producer mutex only serializes reserve/commit.
- **Fixed-capacity backpressure** — the caller chooses the buffer size at
  construction and selects the backpressure policy on each reserve.

Do NOT use RawChannel for:

- Multiple consumers on the same channel — it is strictly single-consumer.
  Use multiple channels (one per consumer) with a fan-out strategy instead.
- Heap-allocated variable-length messages without a pool — every record is a
  fixed-size slot (`RecordHeader` + payload) stored inline in the ring.

### 4.2 Construction and Capacity

```cpp
// From raw buffer size (rounded up to a power of two, at least one HAL page)
RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

// Default: one page (hal::page_granularity)
RawChannel chan{};

// Convenience: N elements of element_size each (auto-rounded as above)
RawChannel chan{num_elements, element_size};
```

- Capacity is rounded up with `std::bit_ceil` after page alignment; the
  `(num_elements, element_size)` form accounts per-record overhead via
  `RawChannel::alignSize` (8-byte header + 8-byte-aligned payload).
- Memory is allocated via `hal::ringBufferAlloc`, which maps the same physical
  pages twice (virtual-memory trick) so the ring never copies wrapped data.
- The typed wrapper takes `std::in_place_t` plus capacity **in elements**:
  ```cpp
  auto chan = Channel<int>(std::in_place_t{}, 64u);
  ```
- `SharedRawChannelPtr` (`std::shared_ptr<RawChannel>`) is the owning handle
  shared between `ChannelWriter<T>` and `ChannelReader<T>` for explicit role
  separation. The `shared_ptr` owns; writers, readers, and record spans
  observe.

### 4.3 Producer API — Reserve, Submit, Discard

The producer follows a **reserve / submit** two-phase protocol. `Record` is a
thin handle over `RecordHeader`; submit/discard take the header:

```cpp
// Reserve space — returns std::expected<Record, errc>, default wait_if_full
auto record = chan.producerReserve(sizeof(MyData), RawChannel::wait_if_full);

if (record.has_value()) {
    new (record->data()) MyData{...};  // Record::data() forwards to header
    chan.producerSubmit(*record);      // Record converts to RecordHeader&
}
```

**Backpressure policies** (`EBackPressure`, second argument):

| Policy | Behavior |
|---|---|
| `drop_if_full` | Returns `error_full` immediately if buffer is full |
| `wait_if_full` | Blocks (via `m_read.wait()`) until consumer frees space |
| `yield_if_full` | Unlocks the producer mutex, calls `std::this_thread::yield()`, retries |

**Discard** — release a reserved record without committing it:
```cpp
chan.producerDiscard(*record);
```
The consumer skips discarded records (marked `flag_discard`).

### 4.4 Consumer API — Acquire, Release

```cpp
// Block until data is available (default policy):
auto record = chan.consumerAcquire(RawChannel::block_until_available);

// Non-blocking peek:
auto record = chan.consumerAcquire(RawChannel::peek_without_blocking);

if (record.has_value()) {
    process(record->data());
    chan.consumerRelease(*record);
}
```

**Errors** from `consumerAcquire` (`errc`, convertible to `error_code` via
`make_error_code` + `is_error_code_enum`):
- `error_empty` — no data available (only with `peek_without_blocking`)
- `error_closed` — channel closed and all preceding records drained

**Drain pattern** — consume all available messages without blocking:
```cpp
while (auto record = chan.consumerAcquire(RawChannel::peek_without_blocking)) {
    process(record->data());
    chan.consumerRelease(*record);
}
```

### 4.5 Typed Channel (`Channel<T>`)

```cpp
auto chan = Channel<int>(std::in_place_t{}, 64u);

chan.send(42);                          // error_code; default drop_if_full
chan.emplace(42);                       // default wait_if_full
chan.send(42, RawChannel::wait_if_full);
auto val = chan.receive();              // expected<T, errc> (blocks)
auto peeked = chan.peek();              // optional<T> (non-blocking)
int dst;  chan >> dst;                  // error_code (blocking)

for (const auto &msg : chan) { process(msg); }  // blocking; ends on close+drain
for (auto it = chan.begin(RawChannel::peek_without_blocking);
     it != chan.end(); ++it) { process(*it); }

ChannelWriter<T> writer = chan.writer();  // send-only view (observes)
ChannelReader<T> reader = chan.reader();  // receive-only view (observes)
```

**Requirements on `T`** (enforced by `requires` clauses):
- Always: `std::is_nothrow_destructible_v<T>`.
- `send`: `is_nothrow_constructible<T, LikeT&&>` and
  `is_nothrow_move_constructible<T>`.
- `emplace`: `is_nothrow_constructible<T, ArgsT&&...>`.
- `receive` / `peek`: `is_nothrow_move_constructible<T>`.
- The destructor closes the channel, then asserts no unread
  non-trivially-destructible messages remain (assertions builds only).

### 4.6 Channel Lifecycle and Close

- **`close()`** — transitions `opened → closing → closed` and injects a
  `flag_close` record. The consumer sees `error_closed` after draining all
  preceding records. Idempotent-ish: closing twice reports `error_closed`.
- **`flush()`** — injects a `flag_flush` record and blocks the caller until
  the consumer processes it, proving all previously submitted records were
  seen.
- **Destructor** — `virtual` (safe teardown through the `IEvent` base);
  closes the channel and frees the ring via `hal::ringBufferFree`.

**Close is terminal — provision the shutdown order around it.** Once a
producer observes `error_closed` from `producerReserve`, it must stop
admitting; once `consumerAcquire` returns `error_closed`, the backlog is
empty, so no further *blocking* acquire may follow (it would block
indefinitely — the close record was already consumed). Either use
`peek_without_blocking` after close or track closed state explicitly, and run
exactly one consumer per `RawChannel` unless an additional close barrier is
added.

```cpp
chan.flush();  // prove the consumer has seen everything
chan.close();  // consumer drains, then sees error_closed
```

### 4.7 IEvent Interface (Event Multiplexing)

RawChannel implements `IEvent`, so it composes with `select()`/`Signal`.
`pollEvent()` is level-aware: the commit pulse OR `read != commit`, so a
committed message stays visible even after the pending bit is consumed.
`resetEvent()` clears the pulse and re-emits when backlog remains, so a
multi-source `Signal` never loses a committed message behind a consumed bit:

```cpp
auto signal = select(channel);
if (auto event = signal.poll()) {
    auto record = (*event)->consumerAcquire(RawChannel::peek_without_blocking);
    // ...
    signal.reset();  // single-source Signal: nullary reset consumes the pending bit
}
```

### 4.8 Thread Roles

The MPSC design assumes **one logical consumer**. Multiple producers are
serialized by the internal `m_producer_mutex`; the consumer read path is
lock-free.

| Thread role | Callable methods | Thread-safe? |
|---|---|---|
| Producer(s) | `producerReserve`, `producerSubmit`, `producerDiscard`, `close`, `flush` | Yes (mutex-protected) |
| Consumer (single) | `consumerAcquire`, `consumerRelease` | Yes (single-threaded by design) |
| Any | `isOpened`, `isClosed`, `isClosedOrClosing`, `capacity` | Yes (atomic reads) |

## 5. IEvent / ISignal / Signal — Event Multiplexing

### 5.1 Core Interfaces

```cpp
class ISignal {
    virtual void notify(std::size_t event_tag) noexcept = 0;
    virtual void wait() noexcept = 0;
};

class IEvent {
    virtual TagPtr<ISignal> subscribeEvent(TagPtr<ISignal> signal) noexcept = 0;
    virtual void unsubscribeEvent(TagPtr<ISignal> signal, TagPtr<ISignal> restore) noexcept = 0;
    virtual bool pollEvent() noexcept = 0;
    virtual void resetEvent() noexcept = 0;
};
```

- **`IEvent`** — a level- or edge-triggered source. Implemented by
  `RawChannel`, `PulseEvent`, `BroadcastEvent`, `IContext`, `IoRequest`.
- **`ISignal`** — a subscriber. `Signal<EventsT...>` is the concrete
  implementation; `select()` is its deduction-guide factory.

### 5.2 Non-owning subscribe/unsubscribe — teardown order

Subscriptions never own: never destroy a signal while another thread may be
inside `emitEvent`, and never destroy an event while another thread may call
`notify` on it. Join, close, or drain the emitters first, then unsubscribe,
then destroy. Cross-event notification obeys the same rule:
`CancelContext::notify` emits the child event while the parent emits, so tear
down child-before-parent under the owner's join — the child's subscription to
the parent must be released (child destroyed or unsubscribed) before the
parent event can be destroyed.

### 5.3 Event Implementations

| Class | Behavior |
|---|---|
| `NeverEvent` | `pollEvent()` always `false`; subscribe/unsubscribe are no-ops. Stub event. |
| `PulseEvent` | Edge-triggered: `emitEvent()` sets the bit and notifies at most once until `resetEvent()`. **One subscriber** (single-slot atomic exchange). |
| `BroadcastEvent` | Same pulse semantics, **multiple subscribers** via a mutex-protected subscription list. `emitEvent()` notifies from a snapshot taken under the mutex, so unsubscribing inside `notify()` is safe. |

```cpp
PulseEvent event;
event.emitEvent();
bool fired = event.pollEvent();   // true
event.resetEvent();
bool fired2 = event.pollEvent();  // false
```

**Late subscription is level-checked, not replayed, for broadcast sources.**
`BroadcastEvent::subscribeEvent` after `emitEvent` synthesizes NO `Signal`
notification — the new subscriber simply missed the pulse. After subscribing,
always check `pollEvent()`/`error()` for level state instead of assuming a
notification will arrive. (`PulseEvent`, by contrast, re-fires into a
subscriber that arrives while the bit is set.) `CancelContext` children
created after the parent cancelled observe `error()` through parent
delegation, not `pollEvent()` on their own (never-emitted) flag — and
scheduler admission follows the same rule (§8).

### 5.4 Signal — Multi-Event Composition

```cpp
PulseEvent timer;
IoRequest io_done;

auto signal = select(timer, io_done);  // Signal<PulseEvent, IoRequest>

if (auto event = signal.poll()) {      // non-blocking; variant of pointers
    std::visit(pP::overloaded{
        [](IoRequest *r) { process_io(r); },
        [](PulseEvent *t) { handle_timeout(t); },
    }, *event);
    signal.reset(*event);              // reset the consumed source
}

signal.wait();  // block until some source fires
```

**Constraints**: at least one event type, at most `bit_count_v<std::size_t>`
(64) sources, every source derives from `IEvent`. `Signal<EventT>` is a
specialized single-event form with a direct `pollEvent`/`reset` loop.

**Range-for over `select()` auto-resets the previously yielded event on
`++`.** The iterator resets the consumed source *before* fetching the next
pending event, then blocks in `wait()` when nothing is pending. Never rearm
between observing an event and incrementing the iterator — the increment owns
the reset. Sources that need explicit control use the rearmable sequence
`poll → handle → resetEvent → wait` directly on the event, never range-for:

```cpp
// Rearmable source handled explicitly (no range-for):
for (;;) {
    if (chan.pollEvent()) {
        drain(chan);
        chan.resetEvent();
    }
    if (ctx->error()) break;  // stable latch, not the consumable pulse (§6)
    signal.wait();
}
```

## 6. IContext / SharedContext — Cancellation Tree

`IContext : IEvent` adds `error()` and `value(user_key)`;
`SharedContext = std::shared_ptr<IContext>` is the owning handle. Factories
live in `context::` (`Core.Concurrency.Context.cppm`).

### 6.1 Background Context (Root)

```cpp
SharedContext bg = context::background();
bg->pollEvent();  // always false (NeverEvent)
bg->error();      // std::error_code{} (no error)
bg->value("key"); // std::nullopt
```

### 6.2 WithCancel / WithCancelClause — Manual Cancellation

```cpp
auto [ctx, cancel] = context::withCancel(context::background());
cancel();                 // latches operation_canceled, emits the event
ctx->pollEvent();         // true
ctx->error();             // std::errc::operation_canceled
```

Cancellation propagates **down** (child of a cancelled parent fires) and
never **up**. `cancel()` is idempotent — first cause wins via CAS. Both
`CancelFunc` and `CancelClauseFunc` hold a *weak* handle with an `isValid()`
liveness check; calling through an expired handle is a silent no-op.
`WithCancelClause` accepts an explicit cause:

```cpp
auto [ctx, cancel_] = context::withCancelClause(parent);
cancel_(std::error_code{42, std::generic_category()});
ctx->error().value() == 42;  // true
```

**An empty clause is a no-op: no event, no error.** `cancelCause` returns
before the CAS loop when the cause is empty, so `pollEvent()` stays false and
`error()` stays empty (`Core.Concurrency.Context.Tests.cpp`,
`CancelClause::empty_clause_is_noop`). This is what makes
`Application::requestExit({})` (§9) and `join({})` drain quietly instead of
firing cancellation for no cause.

`CancelContext::error()` delegates to the parent when no local cause is
latched, so a child observes parent cancellation through `error()` even if
its own flag never fired — the same level-state rule as §5.3.

### 6.3 WithoutCancel — Sever the Tree

```cpp
auto [parent, cancel_parent] = context::withCancel(context::background());
auto child = context::withoutCancel(parent);  // values flow, cancel severed
cancel_parent();
child->pollEvent();  // false — deliberately never subscribed to the parent
child->error();      // std::error_code{}
```

### 6.4 WithValue — Attach Data

```cpp
const SharedContext parent = context::background();
const SharedContext ctx = context::withValue(parent, "test_key", opaque::Value{42});
ctx->value("test_key");    // has_value()
ctx->value("other_key");   // std::nullopt
```

Children fall back to parent values (`withValues` attaches a whole
`opaque::Dict`). Values are immutable after construction.

### 6.5 WithDeadline and WithTimeout — Injected Timer

Pass an explicit `TimerManager &timer` — there is no default and no
singleton. `TimerManager timer{ITimerClock::steady()}` is the production
spelling from the tests:

```cpp
TimerManager timer{ITimerClock::steady()};
const TimePoint dl = timer.now() + std::chrono::seconds(60);
const SharedContext ctx = context::withDeadline(parent, dl, timer);
ctx->pollEvent() == false;  // nothing due yet

const SharedContext tctx = context::withTimeout(
    context::background(), std::chrono::milliseconds(150), timer);
// Cause variants: withDeadlineCause(parent, dl, cause, timer),
// withTimeoutCause(parent, delay, cause, timer); default cause is timed_out.
```

`withDeadline`/`withTimeout` derive from `CancelContext` and schedule through
`TimerManager::schedule`; the timer holds a `weak_ptr`, so destroying the
context before the deadline is safe. Parent cancellation overrides the
deadline, and `cancelDeadline` is `PPR_ENSURE`-guarded to fire only at/after
the date. For deterministic tests, assert setup state (as above) rather than
sleeping for a real deadline.

### 6.6 WithAfterFunc — Destruction Callback

```cpp
bool called = false;
{
    const SharedContext ctx = context::withAfterFunc(
        parent, [&called](const IContext &) noexcept { called = true; });
}
called == true;  // callback ran in the destructor
```

`AfterFunc = std23::move_only_function<void(const IContext &)>`; it runs on
cancel/destroy with the context that is going away.

### 6.7 Checking Cancellation

Prefer the stable latch over the consumable pulse: `error()` persists while
`pollEvent()` can be consumed and reset by a competing waiter (§5.4, §8).

```cpp
if (const std::error_code err = ctx->error()) {
    // handle cancellation with its cause
}
```

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

## 8. ParallelScheduler — Admission on Stable Error

`ParallelScheduler(description, num_workers, queue_bytes_per_worker, parent,
thread_priority)` parents its lifecycle to the given context (default
`background()`). `schedule`/`broadcast` take an optional tracking `WaitGroup`
and an `EProducerPolicy` (default `wait_if_full`); void tasks report admission
as `error_code`, value tasks as `expected<future<R>, error_code>`.
`join(cause = {})` drains admitted tasks — a nonzero cause cancels first, an
empty cause drains quietly — and reports the lifecycle error. `error()` is
null-safe; `getContext()` exposes the lifecycle for observation only (it
returns `const IContext&`).

**Admission gates on the persistent `error()`, never on the consumable
`pollEvent()`.** Worker select-iterators consume-and-reset the lifecycle
pulse while draining, but the recorded cause never clears — so a pulse check
would admit work after cancellation was already acknowledged. `shouldCancel_`
checks `m_lifecycle->error()` first and keeps `pollEvent()` only as an
`operation_canceled` fallback for lifecycles that pulse without an error:

```cpp
ParallelScheduler sched{"workers", 2u, queueBytes(), parent};
cancel();  // parent cancelled with operation_canceled
sched.schedule([](const IContext &) {}, std::nullopt,
               ParallelScheduler::drop_if_full);  // == operation_canceled
sched.join();                                     // == operation_canceled
```

Admission failure touches neither the group counter nor its error slot;
success registers exactly once and the worker envelope settles the group via
`done(err)` exactly once (`WaitGroup::add/done/error/wait`, all `const`).
`parallel::forRange/forEach` compose caller-lane, group, then scheduler
error with queue-full fallback to the caller lane.

Regression links: `Core.Concurrency.Scheduler.Tests.cpp`
`cancel_ack_still_rejects_admission` (post-ack admission stays rejected),
`cancel_parent_drains_accepted_rejects_later` (accepted work still drains),
`empty_clause_cancellation_normalized` (empty clause admits and joins clean).

## 9. Application Graceful Exit — Latch, Not Cancellation

`Application::requestExit(clause = {})` separates exit from cancellation.
`m_exit_requested` is a dedicated latch: an empty clause raises only the
latch (no cause recorded, no event fired), so `run()` terminates the loop and
still returns success `{}`. A nonzero clause latches the cause on the
lifecycle and emits, exactly as cancellation always has.
`isExitRequested()` is latch OR latched error; the run loop terminates on
that stable state — never on the consumable pulse alone — and `run()` reports
the same stable error. `initialize()` re-arms the latch for repeated `run()`.

```cpp
app.requestExit();  // graceful: loop exits, run() returns {}
```

Callers (`game/main.cpp` auto-exit and `--profile-csv` paths,
`ColonySmoke` pixel/load100 paths, GLFW main-window close) all pass empty
and observe unchanged clean-exit behavior; `run()` consumers
(`game/main.cpp`, smoke harnesses) see `{}` on clean exit and the cause on
error exit. Regression links: `App.WindowInput.Tests.cpp`
`app_empty_request_exit_signals_exit_without_error` (signal without error),
`app_error_request_exit_latches_cause`,
`app_empty_request_exit_run_returns_success` (frame-3 `ExitAfterFramesApp`
harness), `app_error_request_exit_run_reports_cause`.

## 10. Integration with HAL I/O

`IoRequest` (final, move-disallowed, stable address while in-flight)
implements `IEvent` over an internal pulse with states 0 idle / 1 pending /
2 complete. `IoPort::open` returns `expected<IoFile, error_code>`;
`read`/`write(req, file, span<byte>, offset)` assert idle, bind, mark pending,
reset the pulse, and submit; `pollCompletions()`/`waitForCompletions()`
drain up to 64 completions per call into `complete_()` (CAS 1→2, then emit).
`resetEvent()` returns to idle for reuse; `cancel()` aborts via
`hal::io::cancelIo` and reports whether the request was in flight; the
destructor cancels pending work and verifies quiescence. `hal::io::wake(handle)`
unblocks a thread parked in `hal::io::wait()` so channel messages can be
drained alongside completions:

```cpp
while (running) {
    port.waitForCompletions();  // or pollCompletions() for non-blocking
    while (auto msg = channel.consumerAcquire(RawChannel::peek_without_blocking)) {
        process_channel_message(msg);
        channel.consumerRelease(*msg);
    }
    if (ctx->error()) break;  // stable latch, not the pulse
}
```

`port.read(req, file, buf, 0u)` takes `std::span<std::byte>`; after
completion `req.bytesTransferred()`/`req.error()` assert completed state.
Multiplex completions with other sources via `select(req, timer)` and the
explicit `poll → handle → resetEvent → wait` sequence from §5.4 — never
range-for over a source that must be rearmed manually.

## 11. Test Patterns

Tests use `PPR_UNIT_TEST(name)` in nested namespaces with `_.recurse({...})`
registration (layout details: `unit-test-updater`). Worker bodies never
assert (`PPR_TEST_ASSERT` throws) — they record into atomics and the parent
thread asserts; gates are deterministic (`std::latch`, atomic wait/notify)
with scope-guard release so a throwing assert never strands a worker.

**Channel round-trip** (from `single_threaded_send_receive`):

```cpp
RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

auto hdr = chan.producerReserve(sizeof(int));
PPR_TEST_ASSERT(hdr.has_value());
*static_cast<int *>(hdr->data()) = 42;
chan.producerSubmit(*hdr);

auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
PPR_TEST_ASSERT(read.has_value());
PPR_TEST_ASSERT(*static_cast<int *>(read->data()) == 42);
chan.consumerRelease(*read);
```

**Backpressure**: fill with `drop_if_full` until `error_full`; prove
`wait_if_full` blocks with a drainer thread (never assert between spawn and
join); close is covered by `close_idempotent`, `close_record_consumed`,
`select_close_wakes_waiting_thread`. **Select**: `select_one`,
`select_multiple`, `select_reset_rearms_pending_commit` (level-aware re-arm),
`select_concurrent_wakeup_and_drain` (drain everything per wakeup),
`select_notify_before_subscribe` (late-subscriber level check).

**Context**: `deadline_is_set` (far-future date, no waiting),
`parent_cancel_overrides_deadline`, `value_is_retrievable`,
`callback_called_on_destruction`, `clause_fires_done_with_error`,
`empty_clause_is_noop`. **Scheduler**: §8 links. **App exit**: §9 links.

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

## 13. Common Patterns and Idioms

### 13.1 Select-Based Worker

Subscribe before checking the stable context `error()` latch. A late subscriber
receives no replay of an earlier cancellation (§5.3); the latch must be checked
before the first wait and again after wakes. Keep the subscription alive across
checks and waits. Do not use the event pulse as persistent cancellation state.

On channel readiness, drain with non-blocking acquires, releasing every acquired
record. Distinguish `error_empty` from `error_closed`; after consuming the close
record, leave the receive loop rather than waiting for another notification.
The select iterator owns reset; do not reset its source inside the body.

Cancellation requests shutdown; it does not close producer channels or join
workers. The owner must stop admission, settle outstanding producer reservations,
and close channels while consumers can still drain them. Join consumers only
after their terminal receive condition. Specify whether cancellation drains or
discards work before implementing the loop; no universal loop serves both policies.

### 13.2 Fan-Out with Multiple Channels

Use one `RawChannel` per consumer. Stop producer admission and settle outstanding
reservations first. If a flush barrier is required, flush while consumers remain
active; then close each channel. Consumers drain to `error_closed`, then the
owner joins them and releases storage. Never wait for consumers to observe close
before issuing close, or flush after consumers have stopped.

### 13.3 Context-Bounded Operation with Timeout

Use the owner's injected, actively advanced `TimerManager` with
`context::withTimeout`. Scheduling a deadline does not start a timer thread:
the owner must continue calling `tick()` while the operation is pending.
A local manager followed by a blocking operation on that same thread cannot
fire the timeout. Tests advance an injected clock and tick deterministically.

Observe the context's stable error result at the operation boundary. Calling
its cancellation function signals cancellation only; explicitly detach callbacks
and drain/join in-flight work before releasing the context tree and timer owner.

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
  for many. Late broadcast subscribers get no replay (§5.3).
- `Signal::iterator` is input-only, non-reentrant, single-consumer; it blocks
  in `wait()` when nothing is pending.
- Custom cancel causes must be `error_code`-compatible; an empty clause is a
  no-op by contract (§6.2), never a silent cancel.
- Implementation: `Core.Concurrency.Channel/Event/Context/Scheduler/Thread`
  partitions; tests `Core.Concurrency.{Channel,Event,Context,Scheduler}.Tests.cpp`
  and `App.WindowInput.Tests.cpp` (exit path).
