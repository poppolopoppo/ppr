# lib/engine/core/concurrency/

## Responsibility
Thread-safe core primitives: lock-free MPSC ring buffer (`RawChannel` + typed `Channel<T>`), compile-time event multiplexing (`Signal<Events...>` with `select()` helper), single-shot/broadcast events (`PulseEvent`/`BroadcastEvent`/`NeverEvent`), and Go-style cancellation tree (`IContext`/`SharedContext` + `context::` factories). All cross-thread wakeups in IO and engine shutdown flow through `IEvent`/`ISignal`.

## Design
- **RawChannel** (`Core.Concurrency.Channel.cppm/.cpp`, BPF-ring-inspired, cache-line-aligned, extends `IEvent`): runtime-sized buffer; producer side serialized by `m_producer_mutex`, consumer side lock-free via `m_commit`/`m_read` atomics. Records carry `RecordHeader` flags (`busy`/`discard`/`flush`/`close`). API: `producerReserve`/`producerSubmit`/`producerDiscard`, `consumerAcquire`/`consumerRelease`, `flush`/`close`, `EBackPressure` (`drop`/`wait`/`yield`), `EPolling` (`block_until_available`/`peek_without_blocking`), `EError` (`closed`/`empty`/`full`/`invalid`). Level-aware `pollEvent()` (pulse OR `read != commit`) and re-arming `resetEvent()` (re-emits when backlog remains) keep multi-source `Signal` from losing committed messages after the pending bit is consumed.
- **Typed Channel<T>** (header-only in Channel.cppm): `Channel<T>` over `shared_ptr<RawChannel>` with `reader()`/`writer()` splits (`ChannelReader`/`ChannelWriter`), `send`/`emplace`, `receive`/`peek`, `operator<</>>` chaining via `BasicSendResult`, STL `OutputIterator`/`InputIterator` (sentinel-ended `begin`/`end`), `chan`/`input_chan`/`output_chan` aliases. Destructor closes and asserts no unread non-trivially-destructible messages in debug.
- **Signal** (`Core.Concurrency.Event.cppm/.cpp`): multi-event `Signal<Events...>` (bitmask `m_pending` + `counting_semaphore`, `TagPtr<ISignal>` parent tracking, `poll()` consumes lowest set bit, range-for `iterator` resets each consumed event) plus single-event `Signal<EventT>` specialization (direct `pollEvent`/`reset` loop). `select(events...)` deduces and returns the `Signal`; `Event` is a `variant<EventsT*…>`.
- **Pulse/Broadcast**: `PulseEvent` — single-subscriber coalescing pulse keyed by high-bit tag, `acq_rel` reset ordering; `BroadcastEvent` — multi-subscriber fan-out via mutex + `StableVectorInplace<TagPtr<ISignal>, mem::PMR>`; `NeverEvent` — inert constexpr placeholder.
- **Context** (`Core.Concurrency.Context.cppm/.cpp`): `IContext : IEvent` adds `error()` + `value(user_key)`; `SharedContext = shared_ptr<IContext>`. `context::` factories: `background()`, `withCancel`/`withCancelClause` (+ `CancelFunc`/`CancelClauseFunc` weak handles), `withoutCancel` (sever cancel, keep values), `withAfterFunc` (run on cancel/destroy), `withValue`/`withValues` (opaque data), `withDeadline(Cause)`/`withTimeout(Cause)` defaulting to `TimerManager::mainTimer()`.

## Flow
- **Send/receive**: producer `producerReserve(size, policy)` → placement-new payload → `producerSubmit` (advances commit, pulses); consumer `consumerAcquire(block)` → moves/copies out → `destroy_at` → `consumerRelease`. Full → policy decides (drop/yield/wait); closed → `error_closed`.
- **Multiplexed wait**: `auto sig = select(chan, watcher, ctx); for (auto &ev : sig) { std::visit(overloaded{…}, ev); }` — `notify(tag)` sets bit + releases semaphore once per newly-set bit; iterator blocks on `wait()`, yields one pending event, resets it before advancing.
- **Reset re-arm**: `Signal::reset(event)` → `RawChannel::resetEvent` → clears pulse then re-emits if `read != commit`, so a second committed message stays visible to `poll()` instead of being hidden behind a consumed bit.
- **Cancellation**: `background()` → `withCancel`/`withTimeout` children inherit deadline/values; cancel request or deadline expiry marks tree, wakes `IEvent` waiters, `error()` reports cause; `withoutCancel` forks a value-only branch.

## Integration
- **engine.core IO**: `IoRequest` and `DirectoryWatcher` implement `IEvent` (PulseEvent-backed) so async completions and file changes join the same `select()` loops as channels.
- **engine.core HAL**: channel storage via `hal::ringBufferAlloc`; `hal::cacheline_size_v` alignment; thread identity via `hal::ThreadId`.
- **engine.app**: `Application` lifecycle `SharedContext` cancels subsystems on shutdown; input/UI event paths use `Signal`/`select`.
- **engine.tests.core**: channel send/receive, back-pressure, MPSC safety, close/flush wakeups, `select` single/multi/close/loop plus `select_reset_rearms_pending_commit` regression; `Signal` filtering/iteration; context cancel/deadline/value/`withoutCancel`/`AfterFunc` suites.

## Key Files
- `Core.Concurrency.Channel.cppm` / `.cpp` — `RawChannel`, `Channel<T>`/`Reader`/`Writer`, iterators, `IEvent` impl with re-arm
- `Core.Concurrency.Event.cppm` / `.cpp` — `IEvent`/`ISignal`, `Signal` (multi + single), `select()`, `PulseEvent`, `BroadcastEvent`, `NeverEvent`
- `Core.Concurrency.Context.cppm` / `.cpp` — `IContext`/`SharedContext`, `context::` factories
