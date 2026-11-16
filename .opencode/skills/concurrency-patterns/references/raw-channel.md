# RawChannel API and lifecycle

Read the [entry skill](../SKILL.md) for ownership, publication, TLS, teardown,
selection, and performance constraints before using this reference.

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
