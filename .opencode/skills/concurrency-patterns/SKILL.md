---
name: concurrency-patterns
description: >
  Use this skill whenever you need to implement or reason about concurrent
  message passing, cancellation, event multiplexing, or async I/O in the PPR
  game engine. It covers the three core concurrency abstractions — RawChannel
  (lock-free MPSC ring buffer), IEvent/ISignal/Signal (compile-time event
  multiplexing), and IContext/SharedContext (cancellation tree) — along with
  their thread-safety model, HAL I/O integration, testing strategies, and
  performance-critical design decisions.
---

# Concurrency Patterns Guide

## Quick Reference — Choosing a Concurrency Primitive

| Pattern | When to Use | Key Classes | Header/Partition |
|---------|------------|-------------|------------------|
| **RawChannel** | Async MPSC message passing between threads. Fixed-capacity, lock-free producer path. | `RawChannel`, `Channel<T>`, `ChannelWriter<T>`, `ChannelReader<T>` | `Core.Concurrency.cppm` |
| **IEvent / Signal** | Wait on one or more event sources (I/O, timers, channels). Compile-time multiplexing with `select()`. | `IEvent`, `PulseEvent`, `BroadcastEvent`, `NeverEvent`, `Signal<EventsT...>` | `Core.Concurrency.Event.cppm` |
| **IContext** | Go-style cancellation tree with deadlines. Propagate cancellation to goroutines/workers. | `IContext`, `SharedContext`, `Background`, `WithCancel`, `WithDeadline`, `WithTimeout` | `Core.Concurrency.Context.cppm` |

### Quick API Patterns

```
RawChannel:  producerReserve(size, policy) → producerSubmit(record) / producerDiscard(record)
             consumerAcquire() → consumerRelease(record)
Signal:      Signal<Event1, Event2> sig{event1, event2}; for (auto& e : select(sig)) { ... }
Context:     auto ctx = IContext::WithCancel(IContext::Background());
             ctx->cancel();  // propagates to all derived contexts
```

Return to the full guide below for detailed construction, lifecycle, thread-safety, and integration patterns.

---

## 1. RawChannel — Lock-Free MPSC Ring Buffer

RawChannel is a multi-producer, single-consumer (MPSC) lock-free circular buffer
designed for fast message passing between threads. It is inspired by the BPF
ring buffer (`kernel.org/doc/html/latest/bpf/ringbuf.html`).

### 1.1 When To Use

Use RawChannel (or its typed wrapper `Channel<T>`) when you need:

- **Async producer-consumer** — one thread (or multiple threads) produces data,
  a single dedicated consumer thread processes it.
- **Low-latency** — the hot path (producer submit / consumer acquire) uses
  lock-free atomics; the producer mutex only protects the reserve/commit
  serialization and is typically uncontested.
- **Fixed-capacity backpressure** — the caller chooses the buffer size at
  construction and selects the backpressure policy on each reserve.

Do NOT use RawChannel for:

- Multiple consumers on the same channel — it is strictly single-consumer.
  Use multiple channels (one per consumer) with a fan-out strategy instead.
- Heap-allocated variable-length messages without a pool — every record is
  a fixed-size slot aligned to `RecordHeader + payload`, allocated inline in
  the ring buffer.

### 1.2 Construction and Capacity

```cpp
// From raw buffer size (must be power of 2, aligned to page granularity)
RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

// Default: one page (hal::page_granularity)
RawChannel chan{};

// Convenience: N elements of element_size each (auto-rounded to power-of-2 page)
RawChannel chan{num_elements, element_size};
```

- The buffer size **must** be a power of two (enforced by `PPR_ASSERT`).
- Memory is allocated via `hal::ringBufferAlloc`, which maps the same physical
  pages twice (virtual-memory trick) so the ring buffer never needs to copy
  wrapped data — offsets are computed with `offset & (capacity - 1)`.
- The typed wrapper `Channel<T>` accepts `std::in_place_t` plus capacity in
  elements:
  ```cpp
  auto chan = Channel<int>(std::in_place_t{}, 64u);
  ```
- `SharedRawChannelPtr` (`std::shared_ptr<RawChannel>`) can be shared between
  `ChannelWriter<T>` and `ChannelReader<T>` for explicit role separation.

### 1.3 Producer API — Reserve, Submit, Discard

The producer follows a **reserve / submit** two-phase protocol:

```cpp
// Reserve space — returns std::expected<Record, EError>
auto record = chan.producerReserve(sizeof(MyData), RawChannel::wait_if_full);

if (record.has_value()) {
    // Write data into the reserved slot
    new (record->data()) MyData{...};

    // Commit — makes data visible to consumer
    chan.producerSubmit(*record);
}
```

**Backpressure policies** (third argument to `producerReserve`):

| Policy | Behavior |
|---|---|
| `drop_if_full` | Returns `error_full` immediately if buffer is full |
| `wait_if_full` | Blocks (via `m_read.wait()`) until consumer frees space |
| `yield_if_full` | Unlocks the producer mutex, calls `std::this_thread::yield()`, retries |

**Discard** — release a reserved record without committing it:
```cpp
chan.producerDiscard(*record);
```
The consumer will skip discarded records (they are marked `flag_discard`).

**Batch operations** — because the producer mutex serializes all producer
threads, you can reserve, write, and submit multiple records in a single
critical section for higher throughput.

### 1.4 Consumer API — Acquire, Release

The consumer follows an **acquire / release** two-phase protocol:

```cpp
// Block until data is available:
auto record = chan.consumerAcquire(RawChannel::block_until_available);

// Non-blocking peek:
auto record = chan.consumerAcquire(RawChannel::peek_without_blocking);

if (record.has_value()) {
    // Read data
    auto *data = static_cast<const MyData *>(record->data());
    process(*data);

    // Release — makes space available for producers
    chan.consumerRelease(*record);
}
```

**Errors** returned from `consumerAcquire`:
- `error_empty` — no data available (only with `peek_without_blocking`)
- `error_closed` — channel has been closed (all remaining data has been consumed)

**Drain pattern** — consume all available messages without blocking:
```cpp
while (auto record = chan.consumerAcquire(RawChannel::peek_without_blocking)) {
    process(record->data());
    chan.consumerRelease(*record);
}
```

### 1.5 Typed Channel (<code>Channel&lt;T&gt;</code>)

`Channel<T>` is a type-safe wrapper around `SharedRawChannelPtr`:

```cpp
auto chan = Channel<int>(std::in_place_t{}, 64u);

// Send
chan.send(42);                          // returns expected<void, EError>
chan.emplace(42);                       // perfect-forwarding constructor
chan << 1 << 2 << 3;                    // stream-style (SendResult chains)

// Receive
auto val = chan.receive();              // expected<T, EError> (blocks)
auto val = chan.peek();                 // optional<T> (non-blocking)
int dst;  chan >> dst;                  // expected<void, EError> (blocking)

// Range-for (blocking: stops when channel closes)
for (const auto &msg : chan) { process(msg); }

// Non-blocking iteration
for (auto it = chan.begin(RawChannel::peek_without_blocking);
     it != chan.end(); ++it) { process(*it); }
```

**Requirements on `T`**:
- Must be `std::is_nothrow_destructible_v<T>`.
- For `send(LikeT&&)`: `is_nothrow_constructible<T, LikeT&&>` and `is_nothrow_move_constructible<T>`.
- For `emplace(ArgsT&&...)`: `is_nothrow_constructible<T, ArgsT&&...>`.
- For `receive()` and `peek()`: `is_nothrow_move_constructible<T>`.
- The destructor asserts no unread non-trivially-destructible messages remain.

**Role-separated views**:

```cpp
ChannelWriter<T> writer = chan.writer();  // send-only
ChannelReader<T> reader = chan.reader();  // receive-only
```

Both are lightweight wrappers around the same `SharedRawChannelPtr`.

### 1.6 Channel Lifecycle

- **`close()`** — atomically transitions the channel from `opened` → `closing`
  → `closed`. Injects a `flag_close` record into the ring buffer. The consumer
  sees `error_closed` after draining all preceding records.
- **`flush()`** — injects a `flag_flush` record with an atomic flag and blocks
  the caller until the consumer has processed it. Guarantees all previously
  submitted records are visible to the consumer.
- **Destructor** — automatically closes the channel and frees the ring buffer
  via `hal::ringBufferFree`.

```cpp
// Close gracefully: consumer drains then sees error_closed
chan.close();

// Flush before close to ensure consumer has seen everything
chan.flush();
chan.close();
```

### 1.7 IEvent Interface (Event Multiplexing)

RawChannel implements `IEvent`, allowing it to be used with `select()` and
`Signal` for event-driven dispatch:

```cpp
auto signal = select(channel);
auto event = signal.poll();  // optional<RawChannel*>
if (event.has_value()) {
    // Channel has data or is closed
    auto record = (*event)->consumerAcquire(RawChannel::peek_without_blocking);
    // ...
}
```

The internal `PulseEvent m_on_produced` fires whenever the commit position
advances (new data available) or the channel closes.

### 1.8 Producer Role Clarity

The MPSC design assumes **one logical consumer** (the thread calling
`consumerAcquire`). Multiple producers are serialized by the internal
`m_producer_mutex`. The consumer is single-threaded and lock-free on the
read path.

| Thread role | Callable methods | Thread-safe? |
|---|---|---|
| Producer(s) | `producerReserve`, `producerSubmit`, `producerDiscard`, `close`, `flush` | Yes (mutex-protected) |
| Consumer (single) | `consumerAcquire`, `consumerRelease` | Yes (single-threaded by design) |
| Any | `isOpened`, `isClosed`, `isClosedOrClosing`, `capacity` | Yes (atomic reads) |

## 2. IEvent / ISignal / Signal — Event Multiplexing

The event infrastructure provides a compile-time-safe, composable system for
waiting on multiple event sources.

### 2.1 Core Interfaces

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

- **`IEvent`** — an event source that can be polled, subscribed to, and reset.
  All concurrency primitives (RawChannel, PulseEvent, BroadcastEvent,
  IContext, IoRequest) implement IEvent.
- **`ISignal`** — a subscriber that receives notifications from one or more
  IEvent sources. `Signal<EventsT...>` is the concrete implementation.

### 2.2 Event Implementations

| Class | Behavior |
|---|---|
| `NeverEvent` | `pollEvent()` always returns `false`. `subscribeEvent` returns a null sentinel. Used as a stub / no-op event. |
| `PulseEvent` | Edge-triggered: `emitEvent()` sets a flag; `pollEvent()` returns `true` once until `resetEvent()` is called. Supports **one subscriber** at a time (single-slot atomic exchange). |
| `BroadcastEvent` | Level-triggered like PulseEvent, but supports **multiple subscribers** via a mutex-protected `StableVectorInplace`. Each subscriber's `notify()` is called when the event fires. |

```cpp
PulseEvent event;
event.emitEvent();
bool fired = event.pollEvent();   // true
event.resetEvent();
bool fired2 = event.pollEvent();  // false
```

**PulseEvent implementation detail**: `subscribeEvent` atomically exchanges
the subscriber into `m_signal`, returning the previous value. If the event
had already fired before subscription, the `signal_bit_v` flag is set and
`emitEvent()` is called immediately. This ensures subscribers never miss
events that fired before they subscribed.

**BroadcastEvent implementation detail**: `emitEvent()` takes a snapshot of
the subscription list under the mutex before iterating, allowing
unsubscription from within a `notify()` callback.

### 2.3 Signal — Multi-Event Composition

`Signal<EventsT...>` subscribes to multiple `IEvent` sources and provides a
unified interface for polling and waiting:

```cpp
PulseEvent timer;
IoRequest io_done;

// Compose events
auto signal = select(timer, io_done);

// Poll (non-blocking)
auto event = signal.poll();
if (event.has_value()) {
    // event is a std::variant<IoRequest*, PulseEvent*>
    std::visit(overloaded{
        [](IoRequest *r) { process_io(r); },
        [](PulseEvent *t) { handle_timeout(t); },
    }, *event);
    signal.reset(*event);  // reset the specific source
}

// Wait (blocking)
signal.wait();
auto event = signal.poll();
// ... handle and reset

// Range-for (blocking iterator)
for (auto &event : signal) {
    std::visit(overloaded{
        [](IoRequest *r) { process_io(r); },
        [](PulseEvent *t) { handle_timeout(t); },
    }, event);
}
```

**Constraints**:
- At least one event type (`sizeof...(EventsT) > 0`).
- At most `bit_count_v<std::size_t>` events (typically 64).
- Every event type must derive from `IEvent`.

**Specialization**: `Signal<EventT>` (single event) is optimized — it does
not use a bitmask and directly delegates to the single event's poll/subscribe
interface.

**`select()` helper** — a deduction-guide factory:
```cpp
auto signal = select(event_a, event_b);  // Signal<decltype(event_a), decltype(event_b)>
```

### 2.4 Event Type Requirements

For an event type to work with `Signal` and `select()`:

1. Must publicly inherit from `IEvent`.
2. Must implement all four `IEvent` virtual methods.
3. `subscribeEvent` must return the previously subscribed signal (for chaining
   / restore on unsubscribe).
4. `pollEvent()` must be non-blocking and return `true` if the event has fired.
5. `resetEvent()` must clear the fired state.

## 3. IContext / SharedContext — Cancellation Tree

The context system provides Go-inspired cancellation propagation through a
hierarchy of contexts.

### 3.1 Core Types

```cpp
class IContext : public IEvent {
    virtual std::error_code error() const noexcept = 0;
    virtual std::optional<const opaque::Block::Value*> value(string_literal user_key) const noexcept = 0;
};

using SharedContext = std::shared_ptr<IContext>;
```

`IContext` extends `IEvent` — cancellation is signaled through the event
interface. `pollEvent()` returns `true` when the context is cancelled.

### 3.2 Background Context (Root)

`context::background()` creates a root context that is never cancelled and
carries no values:

```cpp
SharedContext bg = context::background();
bg->pollEvent();                // always false (NeverEvent)
bg->error();                    // std::error_code{} (no error)
bg->value("key");               // nullopt
```

Use `background()` as the parent for top-level application contexts.

### 3.3 WithCancel — Manual Cancellation

```cpp
auto [ctx, cancel] = context::withCancel(context::background());
// ctx is a SharedContext, cancel is a callable

cancel();                       // triggers ctx->pollEvent() == true
ctx->error();                   // std::errc::operation_canceled
```

**Cancellation propagates down** the tree:

```cpp
auto [parent, cancel_parent] = context::withCancel(context::background());
auto [child, cancel_child] = context::withCancel(parent);

cancel_parent();                // child->pollEvent() == true
```

**Cancellation does NOT propagate up**:

```cpp
cancel_child();                 // parent->pollEvent() == false (unaffected)
```

**Idempotency**: Calling `cancel()` multiple times is safe — the error code
is stored with a `compare_exchange_strong` and only the first call wins.

**WithCancelClause** — same as `withCancel` but the cancel function accepts
a custom `std::error_code`:

```cpp
auto [ctx, cancel_with_code] = context::withCancelClause(parent);
cancel_with_code(std::error_code{42, std::generic_category()});
ctx->error().value() == 42;    // true
```

### 3.4 WithoutCancel — Sever the Tree

`context::withoutCancel()` creates a child context that **ignores** parent
cancellation while still propagating parent values:

```cpp
auto [parent, cancel_parent] = context::withCancel(context::background());
auto child = context::withoutCancel(parent);

cancel_parent();
child->pollEvent();             // false — parent cancellation severed
child->error();                 // std::error_code{} (no error)
```

### 3.5 WithValue — Attach Data

```cpp
auto ctx = context::withValue(parent, "user_id", opaque::Value{42});
auto val = ctx->value("user_id");
// val->as<int>() == 42
```

- Values are looked up by `string_literal` key.
- Child contexts fall back to parent values (chain of `shared_ptr`s).
- `context::withValues(parent, dict)` attaches multiple values at once.

### 3.6 WithDeadline and WithTimeout

```cpp
// Cancel at a specific time
auto deadline = TimerManager::mainTimer().now() + std::chrono::seconds(30);
auto ctx = context::withDeadline(parent, deadline);
// ctx->pollEvent() == true when deadline passes

// Cancel after a duration
auto ctx = context::withTimeout(parent, std::chrono::milliseconds(150));

// Custom error code on deadline
auto ctx = context::withDeadlineCause(parent, deadline, my_error_code);

// Custom error code on timeout
auto ctx = context::withTimeoutCause(parent, delay, my_error_code);
```

- `withDeadline` and `withTimeout` accept an optional `TimerManager &timer` parameter (defaults to `TimerManager::mainTimer()`).
- `withDeadline` and `withTimeout` both derive from `CancelContext`.
- They schedule a timer callback via `TimerManager::schedule()`.
- The timer uses a `weak_ptr` to the deadline context, so it is safe if the
  context is destroyed before the deadline fires.
- Parent cancellation overrides the deadline (immediate propagation).

### 3.7 WithAfterFunc — Cleanup Callback

```cpp
auto ctx = context::withAfterFunc(parent, [](const IContext &c) noexcept {
    cleanup_resources();
});
```

The callback is invoked when the context is **destroyed** (in the destructor),
which happens after cancellation propagation. Use this for RAII-style cleanup
that must run when a scope ends.

### 3.8 Error Code Semantics

- Default cancel: `std::errc::operation_canceled`
- Default timeout: `std::errc::timed_out`
- Custom errors must use `std::generic_category()` (enforced by
  `PPR_ASSERT(err.category() == std::generic_category())` in `CancelContext::cancelCause`).

### 3.9 Checking Cancellation

```cpp
// Poll-based (non-blocking)
if (ctx->pollEvent()) {
    auto err = ctx->error();
    // handle cancellation
}

// Subscriber-based (event-driven)
auto signal = select(*ctx, other_events);
for (auto &event : signal) {
    if (std::addressof(event) == ctx.get()) {
        // context cancelled
    }
}
```

## 4. Thread Safety Model

### 4.1 RawChannel

| Operation | Thread Safety | Mechanism |
|---|---|---|
| `producerReserve` / `producerSubmit` / `producerDiscard` | **Thread-safe** (multiple producers) | `m_producer_mutex` (std::mutex) |
| `consumerAcquire` / `consumerRelease` | **Single-threaded only** | No locking — assumes one consumer |
| `close` / `flush` | **Thread-safe** (call from producer side) | Uses `compare_exchange_strong` on `m_status` + producer mutex |
| `isOpened` / `isClosed` / `isClosedOrClosing` / `capacity` | **Thread-safe** | Atomic loads with `memory_order_acquire` |
| `subscribeEvent` / `unsubscribeEvent` / `pollEvent` / `resetEvent` | **Thread-safe** (delegates to PulseEvent) | Atomic operations on `PulseEvent` |

The consumer side is intentionally single-threaded for performance — there is
no contention on `m_read` and no mutex on the hot path. If you need multiple
consumers, use multiple channels and a distribution strategy (e.g., round-robin
or work-stealing via atomic counter).

### 4.2 IEvent / ISignal / Signal

| Class | Thread Safety | Notes |
|---|---|---|
| `PulseEvent` | **Thread-safe** — `emitEvent`/`pollEvent`/`resetEvent`/`subscribeEvent` all use atomics | Single-subscriber only |
| `BroadcastEvent` | **Thread-safe** — `emitEvent` uses `test_and_set` + mutex-protected snapshot | Multiple subscribers; snapshot avoids deadlock on re-entrant notify |
| `Signal<EventsT...>` | **Consumer: single-threaded** — `poll()`/`wait()`/`begin()` are not re-entrant | `notify()` is called from producer threads (cross-thread) |
| `NeverEvent` | **Thread-safe** — all methods are constexpr no-ops | |

### 4.3 IContext / SharedContext

| Operation | Thread Safety | Mechanism |
|---|---|---|
| `cancel()` / `cancelCause()` | **Thread-safe** | `compare_exchange_strong` on `m_error` |
| `error()` | **Thread-safe** | Atomic load of `m_error` + parent delegation |
| `pollEvent()` / `subscribeEvent()` / etc. | **Thread-safe** | Delegates to `BroadcastEvent` (`m_done`) |
| `value(key)` | **Thread-safe** for reads | Immutable `opaque::Unique` per context; parent chain is const |
| Context construction | **Not thread-safe** | Must be sequenced (parent must outlive children) |

### 4.4 What Is NOT Thread-Safe

- **Arena / Slab allocators** — not thread-safe; use thread-local arenas or
  external synchronization.
- **ScopedArena** — RAII watermark allocator, not thread-safe.
- **ScratchPad** — TLS arena, implicitly thread-safe per-thread but not
  cross-thread.
- **Channel::InputIterator / Signal::iterator** — not re-entrant; single
  consumer only.

## 5. Integration with HAL I/O

### 5.1 IoPort and IoRequest

The HAL provides an asynchronous I/O driver (`IoPort`) that uses `IoRequest`
(which implements `IEvent`) for per-operation completion signaling:

```cpp
auto port = io::createPort();
auto file = port.open(path);

IoRequest req;
std::array<std::byte, 64> buf{};

port.read(req, file, buf, 0u);
// req is now "in-flight" (isPending() == true)

// Poll completions (non-blocking)
port.pollCompletions();

// Wait for completions (blocking)
port.waitForCompletions();

// After completion, req.pollEvent() == true
auto signal = select(req);
```

**Completion flow**:
1. `IoPort::read/write` sets `m_state` to `1` (pending), resets the internal
   `PulseEvent`, and submits to the HAL via `hal::io::submit()`.
2. The HAL calls back through `hal::io::poll()` or `hal::io::wait()`.
3. `IoPort::pollCompletions()` / `waitForCompletions()` iterates completion
   entries and calls `IoRequest::complete_()`, which atomically transitions
   `m_state` to `2` (done) and calls `m_completed.emitEvent()`.
4. The `IoRequest` (as IEvent) is now fired and visible to `Signal::poll()`.

### 5.2 IoRequest as IEvent

`IoRequest` implements `IEvent` via an internal `PulseEvent`:

```cpp
IoRequest req;
auto signal = select(req);      // subscribe to completion

// The req can also be used with select() alongside other events:
auto signal = select(req, timer);
```

**Lifecycle**:
- Default-constructed: `m_state == 0`, `pollEvent() == false`.
- After `port.read()`: `m_state == 1` (pending), `pollEvent() == false`.
- After completion: `m_state == 2`, `pollEvent() == true`.
- After `resetEvent()`: `m_state == 0`, ready for reuse.

**Cancellation**:
```cpp
bool was_inflight = req.cancel();
if (was_inflight) {
    // HAL cancel submitted; req will not complete
}
```

### 5.3 Wake Mechanism

`hal::io::wake()` is a cross-platform mechanism to unblock a thread blocked in
`hal::io::wait()`. This is how the I/O event loop integrates with RawChannel:

```cpp
// In the I/O thread:
while (running) {
    port.waitForCompletions();   // blocks until I/O completes OR wake() is called

    // Process completed I/O
    process_completions();

    // Process channel messages (non-blocking)
    while (auto msg = channel.consumerAcquire(RawChannel::peek_without_blocking)) {
        process_channel_message(msg);
        channel.consumerRelease(*msg);
    }

    // Check context cancellation
    if (ctx->pollEvent()) {
        break;
    }
}

// In a producer thread:
channel.producerSubmit(record);
hal::io::wake(port_handle);      // unblock the I/O thread
```

### 5.4 Event-Driven I/O with select()

The `select()` helper enables multiplexing I/O completions with other event
sources:

```cpp
IoRequest req;
PulseEvent shutdown_event;

port.read(req, file, buf, 0u);

for (auto &event : select(req, shutdown_event)) {
    std::visit(overloaded{
        [&](IoRequest *r) {
            process_data(r->bytesTransferred());
            // Re-arm for next read
            r->resetEvent();
            port.read(*r, file, buf, 0u);
        },
        [&](PulseEvent *) {
            cleanup_and_exit();
            return;
        },
    }, event);
}
```

## 6. Test Patterns

### 6.1 Unit Test Structure

Tests use `PPR_UNIT_TEST(name)` macros and are organized in nested namespaces
with `_.recurse({...})` for hierarchical test registration:

```cpp
export namespace pP::tests {
    namespace MyFeature {
        PPR_UNIT_TEST(test_name) {
            // test body
            PPR_ASSERT(condition);
        };
    }

    PPR_UNIT_TEST(my_feature) {
        _.recurse({
            MyFeature::test_name,
        });
    };
}
```

### 6.2 Testing RawChannel

**Single-threaded round-trip**:
```cpp
PPR_UNIT_TEST(single_threaded_send_receive) {
    RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

    auto hdr = chan.producerReserve(sizeof(int));
    PPR_ASSERT(hdr.has_value());
    *static_cast<int *>(hdr->data()) = 42;
    chan.producerSubmit(*hdr);

    auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
    PPR_ASSERT(read.has_value());
    PPR_ASSERT(*static_cast<int *>(read->data()) == 42);
    chan.consumerRelease(*read);
};
```

**Ring buffer wrap-around** — fill the buffer, drain the front, then write
more (the second batch wraps in the virtual-memory ring):
```cpp
// Write batch_size records, consume them all, write again — the second
// batch's offset wraps around in the ring buffer.
```

**Concurrent SPSC** — one producer thread, consumer on main thread:
```cpp
std::jthread producer([&chan] {
    for (int i = 0; i < num_messages; ++i) {
        auto hdr = chan.producerReserve(sizeof(int), RawChannel::wait_if_full);
        PPR_ASSERT(hdr.has_value());
        *static_cast<int *>(hdr->data()) = i;
        chan.producerSubmit(*hdr);
    }
});

for (int i = 0; i < num_messages; ++i) {
    auto hdr = chan.consumerAcquire();
    PPR_ASSERT(hdr.has_value());
    PPR_ASSERT(*static_cast<int *>(hdr->data()) == i);
    chan.consumerRelease(*hdr);
}
```

**Concurrent MPSC** — multiple producer threads, one consumer:
- Use `std::barrier` with a completion function that calls `chan.close()`.
- Accumulate checksums (`seed_send` vs `seed_recv`) to verify data integrity.
- Consumer drains until `error_closed`.

**Concurrent MPMC (fan-out)** — multiple producers distributing across
multiple single-consumer channels via atomic counter:
```cpp
std::array<RawChannel, num_consumers> channels;
std::atomic<int> channels_fan_out{0};

// Producer: channels[channels_fan_out.fetch_add(1) % num_consumers]
// Each consumer: drains one channel until error_closed
// After all producers join: flush + close each channel
```

**Backpressure testing**:
- `drop_if_full`: fill until it fails, count how many fit.
- `wait_if_full`: spawn a consumer thread that drains, then producer resumes.
- `yield_if_full`: same pattern but producer yields instead of blocking.

**Flush round-trip** — verify that `flush()` blocks until consumer catches up,
then consumer sees `error_closed`:
```cpp
// Producer submits data, consumer reads it, then consumer blocks.
// Producer calls flush() — returns when consumer has seen all data.
// Producer calls close() — consumer wakes and sees error_closed.
```

### 6.3 Testing Events

**PulseEvent**:
```cpp
PulseEvent event;
PPR_ASSERT(!event.pollEvent());
event.emitEvent();
PPR_ASSERT(event.pollEvent());
event.resetEvent();
PPR_ASSERT(!event.pollEvent());
```

**Subscribe-then-emit** — the `subscribeEvent` path that detects already-fired
events (the `signal_bit_v` mechanism):
```cpp
PulseEvent event;
event.emitEvent();
// Subscribe after emit — subscriber is notified immediately
auto signal = select(event);
auto result = signal.poll();
PPR_ASSERT(result.has_value());
```

**Signal multi-event** — verify correct `variant` index:
```cpp
PulseEvent a, b;
auto signal = select(a, b);
a.emitEvent();
auto result = signal.poll();
PPR_ASSERT(result->index() == 0u);   // a is index 0
signal.reset(*result);
```

### 6.4 Testing Context

**Cancellation propagation**:
```cpp
auto [parent, cancel_parent] = context::withCancel(context::background());
auto [child, cancel_child] = context::withCancel(parent);

cancel_parent();
PPR_ASSERT(child->pollEvent());                      // propagates down
PPR_ASSERT(!parent->pollEvent());                    // child cancel does not affect parent
```

**WithoutCancel**:
```cpp
auto [parent, cancel_parent] = context::withCancel(context::background());
auto child = context::withoutCancel(parent);
cancel_parent();
PPR_ASSERT(!child->pollEvent());                     // severed
PPR_ASSERT(!child->error());
```

**Timeout** — requires ticking the timer:
```cpp
auto ctx = context::withTimeout(context::background(), std::chrono::milliseconds(150));
while (not ctx->pollEvent()) {
    std::this_thread::yield();
    TimerManager::mainTimer().tick();
}
```

### 6.5 Testing I/O Integration

**Async read completion**:
```cpp
auto port = io::createPort();
auto file = port.open(path);

IoRequest req;
std::array<std::byte, 64> buf{};
port.read(req, file, buf, 0u);
port.pollCompletions();

auto signal = select(req);
auto result = signal.poll();
PPR_ASSERT(result.has_value());
PPR_ASSERT(req.bytesTransferred() == kContent.size());
```

**select() with timer**:
```cpp
PulseEvent timer;
auto signal = select(req, timer);
auto result = signal.poll();
PPR_ASSERT(result->index() == 0u);   // I/O completed, not timer
```

**Cancel in-flight**:
```cpp
port.read(req, file, buf, 0u);
bool was = req.cancel();
port.pollCompletions();
if (was) {
    PPR_ASSERT(!req.pollEvent());    // cancelled, no completion
}
```

## 7. Performance Considerations

### 7.1 Cache Line Alignment

- `RawChannel` itself is `alignas(hal::cacheline_size_v)` (typically 64 bytes).
- The hot atomic variables are padded to separate cache lines:
  - `m_commit` — written by producer, read by consumer.
  - `m_read` — written by consumer, read by producer.
  - The producer mutex `m_producer_mutex` and `m_write` are in the first cache
    line (producer-hot data).
- `Signal<EventsT...>` is `alignas(hal::cacheline_size_v)`.
- `RecordHeader` is `alignas(u64)` — 8-byte aligned for atomic-friendly access.
- The `flush_signal` atomic flag in `RawChannel::flush()` is also
  `alignas(hal::cacheline_size_v)` to avoid false sharing with adjacent data.

**Rule of thumb**: Any atomic variable accessed from different threads should
be on its own cache line to prevent false sharing. The codebase uses
`alignas(hal::cacheline_size_v)` between `m_commit` and `m_read`, and between
the producer-hot block and `m_commit`.

### 7.2 Batch Operations for Throughput

- **Producer batch**: Reserve multiple records inside a single lock scope
  (the producer mutex is held for the duration of `producerSubmit`, but
  `advanceCommit_()` processes all ready records at once). For maximum
  throughput, pack multiple messages into a single larger record.
- **Consumer batch**: On each wakeup from `m_commit.wait()`, drain ALL
  available records using a `while (consumerAcquire(peek_without_blocking))`
  loop before returning to wait. This is the pattern used in all select-based
  tests (`select_concurrent_wakeup_and_drain`, `select_multiple_channels_concurrent`).
- **I/O completion batch**: `IoPort::pollCompletions()` and
  `waitForCompletions()` process up to `kMaxCompletionBatch` (64) completions
  per call. In a hot loop, process all pending completions before re-waiting.

```cpp
// Efficient consumer loop — drain before blocking again
for (auto &event : select(channel)) {
    // Drain ALL messages in the channel on each wakeup
    while (true) {
        auto recv = event.consumerAcquire(RawChannel::peek_without_blocking);
        if (not recv.has_value()) break;
        process(recv);
        event.consumerRelease(*recv);
    }
}
```

### 7.3 Memory Ordering

The channel uses these memory orderings:

| Operation | Ordering | Rationale |
|---|---|---|
| `m_status` store (close) | `memory_order_release` | Ensures all prior producer writes are visible before close is visible |
| `m_status` load (isOpened) | `memory_order_acquire` | Paired with release store above |
| `m_status` CAS (close) | `memory_order_acq_rel` | Atomic state transition |
| `m_write` (reserve) | Under mutex | Serialized by lock |
| `m_commit` store (advanceCommit_) | `memory_order_release` | Ensures record writes are visible to consumer |
| `m_commit` load (consumerAcquire) | `memory_order_acquire` | Paired with release store above |
| `m_read` load (producerReserve) | `memory_order_acquire` | Sees consumer's release of space |
| `m_read` fetch_add (consumerRelease) | `memory_order_release` | Makes freed space visible to producer |
| `m_read.wait()` | `memory_order_acquire` | Blocks until producer notifies |
| `m_commit.wait()` | `memory_order_acquire` | Blocks until consumer sees new commit |
| `m_read.notify_all()` | — | Wakes producer waiting on `m_read.wait()` |
| `m_commit.notify_one()` | — | Wakes consumer waiting on `m_commit.wait()` |
| `PulseEvent::m_signal` | `memory_order_acq_rel` | Atomic exchange on subscribe/emit |
| `CancelContext::m_error` | `memory_order_acq_rel` | CAS for cancellation |

### 7.4 Record Size and Fragmentation

- Every record includes a `RecordHeader` (16 bytes: `EFlags` + `u32` size).
- `alignSize(size_bytes)` rounds up payload to `alignof(RecordHeader)` (8 bytes).
- The total space consumed per message is `sizeof(RecordHeader) + aligned_payload`.
- To minimize fragmentation, prefer uniform message sizes. If you need
  variable sizes, batch small messages or pad to a common size.
- The ring buffer is pre-allocated once; there is no dynamic allocation on the
  send/receive hot path (except the producer mutex if contended).

### 7.5 Signal / select() Overhead

- `Signal::poll()` uses a `compare_exchange_weak` loop on the pending bitmask
  (single atomic RMW per poll). This is fast in the uncontended case.
- `Signal::wait()` blocks on a `std::counting_semaphore`; each producer
  `notify()`-to-consumer `wait()` handoff is a single semaphore release/acquire.
- `select()` creates a `Signal` that subscribes to each event at construction
  and unsubscribes at destruction. For long-lived event loops, create the
  `Signal` once and reuse it.
- `Signal::reset(Event)` and `Signal::reset()` clear the pending state after
  consumption. Always reset after handling to avoid spinning on the same event.

### 7.6 Producer Mutex Contention

The `m_producer_mutex` serializes all producer operations (reserve, submit,
discard, advanceCommit_). In high-throughput multi-producer scenarios:

- Consider batching: each thread reserves and submits multiple records per
  lock acquisition.
- Consider sharding: use multiple channels and route producers by thread ID
  or data key to reduce contention.
- If the consumer cannot keep up, backpressure (`wait_if_full`) causes
  producers to block, which holds the mutex longer — use `drop_if_full` for
  latency-sensitive producers.

## 8. Common Patterns and Idioms

### 8.1 Worker Thread with Channel and Context

```cpp
void worker_thread(SharedRawChannelPtr channel, SharedContext ctx) {
    while (not ctx->pollEvent()) {
        auto record = channel->consumerAcquire();
        if (not record.has_value()) {
            if (record.error() == RawChannel::error_closed) break;
            continue;
        }
        process(record);
        channel->consumerRelease(*record);
    }
}
```

### 8.2 Event Loop with select()

```cpp
void event_loop(IoPort &port, RawChannel &channel, SharedContext ctx) {
    PulseEvent wakeup;
    for (auto &event : select(channel, *ctx, wakeup)) {
        std::visit(overloaded{
            [&](RawChannel *c) {
                while (auto recv = c->consumerAcquire(RawChannel::peek_without_blocking)) {
                    dispatch(*static_cast<const Message*>(recv->data()));
                    c->consumerRelease(*recv);
                }
            },
            [&](IContext *) {
                cleanup();
                return;
            },
            [&](PulseEvent *) {
                // Timer tick or external wakeup
            },
        }, event);
    }
}

// In producer thread:
channel.producerSubmit(record);
hal::io::wake(port_handle);  // wake the I/O thread
```

### 8.3 Context-Bounded Operation with Timeout

```cpp
auto [ctx, cancel] = context::withCancel(context::background());
auto deadline = context::withTimeout(ctx, std::chrono::seconds(5));

auto result = do_work_with_timeout(deadline);
if (deadline->pollEvent()) {
    // operation timed out or was cancelled
    auto err = deadline->error();  // timed_out or operation_canceled
}
cancel();  // clean up
```

### 8.4 Fan-Out with Multiple Channels

When you need MPMC semantics, create one RawChannel per consumer and
distribute work with an atomic counter:

```cpp
std::array<RawChannel, num_consumers> channels;
std::atomic<unsigned> distributor{0};

// Producer routes to a channel
auto &chan = channels[distributor.fetch_add(1) % num_consumers];
auto record = chan.producerReserve(sizeof(Task));
// ...

// Each consumer drains its dedicated channel
void consumer(size_t index) {
    auto &chan = channels[index];
    while (auto record = chan.consumerAcquire()) {
        process(record);
        chan.consumerRelease(*record);
    }
}
```

## 9. Constraints and Pitfalls

- **Call `producerSubmit` exactly once per reserved record.** Leaking a
  reserved record causes `advanceCommit_()` to stall on the `flag_busy` header,
  eventually blocking the channel.
- **Always call `consumerRelease` after `consumerAcquire`.** Failure to release
  leaks space; the producer will eventually be unable to reserve.
- **Do not use `select()` on a `RawChannel` that has multiple producers and
  a single consumer where both sides are on the same thread.** The
  `PulseEvent::emitEvent()` call from `advanceCommit_()` triggers `notify()`
  synchronously, which could re-enter the consumer.
- **IoRequest is move-disallowed and non-copyable.** It must stay at a stable
  address while in-flight. Use `std::unique_ptr<IoRequest>` if you need to
  move it.
- **Context values are immutable after construction.** Attach all needed values
  when creating a context scope. Use `withAfterFunc` for dynamic side effects.
- **`PulseEvent` supports exactly one subscriber.** If you need multiple
  subscribers, use `BroadcastEvent` (which trades a mutex for multi-subscriber
  support).
- **`Signal::iterator` is input-only.** It does not support decrement or
  random access. It blocks on `wait()` when no events are pending.
- **Do not reset an event source while a `Signal` is subscribed.** Always
  unsubscribe or destroy the `Signal` first.
- **`std::error_code` values in context cancellation must use
  `std::generic_category()`.** The `CancelContext` asserts this in debug builds.
