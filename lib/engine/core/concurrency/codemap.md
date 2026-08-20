# lib/engine/core/concurrency/

## Responsibility
The concurrency partition provides PPR's core thread-safe primitives: lock-free MPSC ring buffer (`RawChannel`), compile-time event multiplexing (`Signal<Events...>` with `select()` Go-style helper), and Go-style cancellation context (`IContext`/`SharedContext`). These primitives are designed for zero-overhead in release, debug safety via assertions in development builds, and integration with the HAL IO subsystem and engine services.

## Design
- **RawChannel** (lock-free MPSC): BPF-ring-buffer-inspired lock-free MPSC circular buffer (`class RawChannel : public IEvent`). Consumer side is lock-free via atomic commit/read indices; the producer side is serialized by an internal mutex, so multiple producers are supported without external synchronization. Buffer size is a runtime constructor parameter (allocated via `hal::ringBufferAlloc`), not a compile-time N. The templated `Channel<T>` wrapper adds typed `send()`/`receive()` with a back-pressure policy (`wait_if_full`) and `ChannelWriter<T>` output iterators. Debug mode asserts on overrun/underrun.
- **Signal<Events...>** (compile-time event multiplexing): Variadic template that composes event types at compile time. `select(events...)` returns a compile-time enabled set; range-for over the result iterates only over signaled events. Internally uses `std::counting_semaphore` (or lightweight event handles) per event type. Enables wait-free event-driven architectures without runtime polymorphism. Used for input handling, UI events, and game event systems.
- **IContext / SharedContext** (cancellation tree): Go-style context with deadline propagation, cancellation, and value carrying. `SharedContext` is reference-counted and tree-structured — child contexts inherit deadline and cancellation from parent; cancellation propagates down the tree. Values set on a context are visible to descendants. Used for request-scoped cancellation (e.g. unload level, terminate thread).
- **Thread ID**: `hal::ThreadId` struct and `hal::currentThreadId()` provide platform-native thread identifiers for channel mapping and context association.

## Flow
- **RawChannel production**: Producer thread calls `channel.send(message)` — serialized by the internal producer mutex, then the commit index is advanced atomically and the consumer is signaled via `PulseEvent`. Consumer calls `channel.receive(message)` — waits on the event, reads the record, advances the read index. If full, the producer blocks per the back-pressure policy (`wait_if_full`).
- **Signal event handling**: Game systems register interest in specific event types (`PulseEvent`, `KeyDownEvent`, `MouseMoveEvent`). `select<PulseEvent, KeyDownEvent>(events...)` enables only those events; the range-for loop processes only triggered events. Zero-cost for uninteresting events (compile-filtered out).
- **Context cancellation**: Root context created at thread start. Sub-contexts created via `context.createChild()` inherit deadline. When `context.requestCancel()` is called, all descendant contexts see cancellation and exit their loops. Used for scoped task groups and safe thread termination.

## Integration
- **engine.core IO**: `IoRequest` and `DirectoryWatcher` are `IEvent`s backed by `PulseEvent` — async completions and file changes are observed via `Signal`/`select`.
- **engine.app**: `Application` holds a `SharedContext` lifecycle context (`m_lifecycle`) used to cancel engine subsystems on shutdown.
- **EngineCoreTests**: Tests `RawChannel` (send/receive, full/empty, MPSC safety), `Signal` (select, range-for iteration, compile-time filtering), and `IContext` (createChild, requestCancel, deadline propagation, value carrying).

## Key Files
- `Core.Concurrency.Channel.cppm` / `.cpp` — `pP::RawChannel` lock-free MPSC buffer, `Channel<T>` typed wrapper, `ChannelWriter<T>`
- `Core.Concurrency.Event.cppm` / `.cpp` — `IEvent`/`ISignal`, `Signal<Events...>` with `select()`, `PulseEvent`, `BroadcastEvent`, `NeverEvent`
- `Core.Concurrency.Context.cppm` / `.cpp` — `pP::IContext` / `pP::SharedContext` Go-style cancellation tree (`withCancel`, `withDeadline`, `withTimeout`, `withValue`)