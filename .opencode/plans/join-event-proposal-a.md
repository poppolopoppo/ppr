# Proposal A — Aggressive: `join()` — All-Events Synchronization

Architect: Aggressive
Theme: Lock-free batch coordination with zero-cost abstraction

## Overview

Introduce `join(events...)` — the dual of `select()`. Where `select()` fires on *any* event,
`join()` fires when *all* subscribed events are ready. Returns a `std::tuple<EventsT*...>`
guaranteeing every event is ready. Semaphore releases exactly once — when the last bit
completes the mask. No redundant wakeups. Lock-free hot path.

---

## Phase 1 — `JoinedSignal` class template (new module partition file)

### Step 1 — Create `Core.Concurrency.Join.cppm` with `JoinedSignal<EventsT...>`

- **Files:** `lib/engine/core/Core.Concurrency.Join.cppm` (create)
- **Operations:** create

New module partition `engine.core:concurrency.join` that exports:

```cpp
export module engine.core:concurrency.join;
import :concurrency.event;
import std;

export namespace pP {

template<typename... EventsT>
    requires (sizeof...(EventsT) > 0 &&
              sizeof...(EventsT) <= bit_count_v<std::size_t> &&
              std::conjunction_v<std::is_base_of<IEvent, EventsT>...>)
class [[nodiscard]] alignas(hal::cacheline_size_v) JoinedSignal final : public ISignal {
    std::counting_semaphore<> m_semaphore{0};
    std::atomic<std::size_t> m_pending{0};
    static constexpr std::size_t kAllMask =
        (std::size_t{1u} << sizeof...(EventsT)) - 1u;

    std::tuple<EventsT &...> m_events;
    std::array<TagPtr<ISignal>, sizeof...(EventsT)> m_parents{};

public:
    using Result = std::tuple<EventsT *...>;

    explicit JoinedSignal(EventsT &... events PPR_LIFETIME_BOUND) noexcept
        : m_events(events...) { /* subscribe all */ }

    ~JoinedSignal() noexcept { /* unsubscribe all */ }

    void notify(std::size_t event_index) noexcept override;

    void wait() noexcept override {
        while (m_pending.load(std::memory_order_acquire) != kAllMask) {
            m_semaphore.acquire();
        }
    }

    [[nodiscard]] std::optional<Result> poll() noexcept;

    void reset() noexcept {
        static_iota<sizeof...(EventsT)>([&](auto... I) noexcept {
            ((std::get<I>(m_events).resetEvent()), ...);
        });
        m_pending.store(0, std::memory_order_release);
    }

private:
    [[nodiscard]] Result getAllEvents_() const noexcept {
        return Result{std::addressof(std::get<I>(m_events))...};
    }
};

// CTAD guide
template<typename... EventsT>
JoinedSignal(EventsT &...) -> JoinedSignal<EventsT...>;

} // namespace pP
```

**Key design — `notify()` (lock-free all-or-nothing):**

```cpp
void notify(const std::size_t event_index) noexcept override {
    const std::size_t bit = std::size_t{1u} << event_index;
    const std::size_t prev = m_pending.fetch_or(bit, std::memory_order_release);

    if ((prev & bit) == 0u) [[likely]] {          // was this bit newly set?
        const std::size_t current = prev | bit;
        if (current == kAllMask) {                 // are we the last bit?
            m_semaphore.release();                 // release exactly once
        }
    }
}
```

**Key design — `poll()` (atomic clear of entire mask):**

```cpp
[[nodiscard]] std::optional<Result> poll() noexcept {
    std::size_t pending = m_pending.load(std::memory_order_acquire);
    for (;;) {
        if (pending != kAllMask) {
            return std::nullopt;
        }
        if (m_pending.compare_exchange_weak(
                pending, std::size_t{0},
                std::memory_order_acq_rel,
                std::memory_order_acquire)) [[likely]] {
            break;
        }
    }
    return getAllEvents_();
}
```

### Step 2 — Create `Core.Concurrency.Join.cpp` (implementation partition)

- **Files:** `lib/engine/core/Core.Concurrency.Join.cpp` (create)
- **Operations:** create

Empty for now — all logic is in the header-like `.cppm`. Create as a placeholder module
implementation file so the build system has a matching `.cpp`:

```cpp
module;
#include "pP/Macros.h"
module engine.core;
import :concurrency.join;
```

(This follows the pattern where `.cpp` files exist even when all logic is in `.cppm`.)

---

## Phase 2 — Free function `join()`

### Step 1 — Add `join()` to `Core.Concurrency.Join.cppm`

- **Files:** `lib/engine/core/Core.Concurrency.Join.cppm`
- **Operations:** edit

Add to the same module partition, after `JoinedSignal`:

```cpp
template<typename... EventsT>
    requires (sizeof...(EventsT) > 0 &&
              sizeof...(EventsT) <= bit_count_v<std::size_t> &&
              std::conjunction_v<std::is_base_of<IEvent, EventsT>...>)
[[nodiscard]] JoinedSignal<EventsT...> join(EventsT &... events) noexcept {
    return JoinedSignal(events...);
}
```

---

## Phase 3 — Register new module in build system

### Step 1 — Register `.cppm` and `.cpp` in `lib/engine/core/CMakeLists.txt`

- **Files:** `lib/engine/core/CMakeLists.txt`
- **Operations:** edit

Add to PUBLIC `FILE_SET CXX_MODULES FILES`:

```
Core.Concurrency.Join.cppm
```

Add to PRIVATE sources:

```
Core.Concurrency.Join.cpp
```

### Step 2 — Re-export from `Core.cppm`

- **Files:** `lib/engine/core/Core.cppm`
- **Operations:** edit

Add after `import :concurrency.event;`:

```
import :concurrency.join;
```

---

## Phase 4 — Unit tests

### Step 1 — Extend `Core.Concurrency.Event.Tests.cppm` with `Join` test suite

- **Files:** `lib/engine/tests/core/Core.Concurrency.Event.Tests.cppm`
- **Operations:** edit

Add a new `Join` namespace with test cases:

| Test | Description |
|------|-------------|
| `join_two_pulses_both_ready` | Emit both events, poll returns tuple |
| `join_two_pulses_one_ready` | Emit only one, poll returns nullopt |
| `join_two_pulses_sequential` | Emit A, emit B — poll returns after B |
| `join_pulse_and_broadcast` | Mixed event types |
| `join_single_event` | Degenerate case: join(one) acts like select(one) |
| `join_with_never_never_fires` | join(never, pulse) — poll never returns after single emit |
| `join_reset_resets_all` | Both emitted, poll consumed, reset, re-wait |
| `join_wait_poll_cycle` | Full wait + poll + process + re-wait cycle |
| `join_three_events` | Verify all-mask correctness with 3 events |

Also wire into the test suite parent:

```cpp
PPR_UNIT_TEST(event) {
    _.recurse({
        Events::never_event,
        Events::pulse_event,
        Events::broadcast_event,
        Events::signal_single,
        Events::signal_multi,
        Events::join,                  // NEW
    });
};
```

---

## Phase 5 — Design Documentation in AGENTS.md (optional follow-up)

### Step 1 — Document `join()` semantics

- **Files:** `AGENTS.md`
- **Operations:** edit

Add entry under Core Abstractions:

- `pP::JoinedSignal<EventsT...>` / `pP::join(events...)`: All-events barrier. Fires when all
  subscribed events are ready. Returns `std::tuple<EventsT*...>`. Lock-free hot path.
  `join(neverEvent, ...)` never fires by design.

---

## Design Rationale

### Return type: `std::tuple<EventsT*...>` over `std::variant`

All events are guaranteed ready — a variant would hide indices behind a visitor,
forcing `std::visit` boilerplate. A tuple lets the user destructure directly:

```cpp
auto [pA, pB, pC] = *join(a, b, c).poll();  // clean
```

### Separate class over modifying `Signal`

- `Signal::poll()` pops one bit at a time using `pending & (pending - 1)`.
- `JoinedSignal::poll()` must atomically compare against `kAllMask`.
- These are fundamentally different state machines. Merging them adds branches,
  runtime checks, and complexity to `Signal` for no benefit.

### Semaphore: exactly one release per completion

Only the thread that sets the final bit releases the semaphore. No spurious wakeups.
`wait()` loops on `m_pending != kAllMask` (not just `!= 0`).

### `NeverEvent` interaction

`join(neverEvent, pulseEvent)` can never complete — `neverEvent` never sets its bit.
This is correct and documented. Equivalent to an infinite wait.

### Iterator support deliberately omitted

`select()` iterates because it yields one event at a time from a sequence.
`join()` yields a *batch* — iterating over batches of tuples is an unnatural fit.
If batch iteration is desired, add `for(;;) { auto result = join(a,b).poll(); ... }`.

### Lock-free guarantee

The hot path (`notify()`) uses a single `fetch_or` — no locks, no CAS retry.
`poll()` uses `compare_exchange_weak` with a single retry window; the high-contention
window is the same as `select()`'s `poll()`.

---

## Future Extensions (not in scope, but designed for)

### `join_n(dynamic_container)` — runtime event count

```cpp
class DynamicJoinedSignal : public ISignal {
    std::atomic<std::size_t> m_remaining;  // counter, not mask
    // notify() does fetch_sub(1), releases semaphore when 0
};
```

### `join_with_timeout(duration, events...)`

A `TimeoutEvent` that fires after a deadline, composed with `join()`:

```cpp
auto result = join(timeoutEvent, workEvent).poll();
if (holds_timeout(result)) ...
```

### `then(callback)` on `JoinedSignal`

Functional chaining:

```cpp
join(a, b).then([](auto a_ptr, auto b_ptr) { ... });
```

Implemented as a wrapper that spawns a worker thread waiting on the join.

### Zero-cost `kAllMask` computation

`kAllMask` is `constexpr` per instantiation — computed at compile time, not runtime.
On x86-64 this folds to a single `cmp` instruction in the `notify` hot path.
