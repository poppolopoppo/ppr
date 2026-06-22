# Proposal B — Conservative `join()` Implementation

## Core idea

Introduce a new `JoinSignal<EventsT...>` class that reuses the exact same lock-free patterns as `Signal`, but only fires the semaphore when *all* event bits are set. No changes to existing `Signal`/`select()` code.

---

## Phase 1 — Add `JoinSignal` class and `join()` free function

### Step 1 — Add `JoinSignal` template after `Signal<EventsT...>` in the module interface

- Files: `lib/engine/core/Core.Concurrency.Event.cppm`
- Operations: edit (insert after line 228, before the `PPR_PRAGMA_WARNING_POP()` that closes the multi-event `Signal`)

Insert a new `JoinSignal<EventsT...>` class with:

**Member layout** (identical to `Signal`):
```
std::counting_semaphore<> m_semaphore{0};
std::atomic<std::size_t> m_pending{0};
std::tuple<EventsT &...> m_events;
std::array<TagPtr<ISignal>, sizeof...(EventsT)> m_parents{};
```

**Constructor** (identical to `Signal`):
- Subscribe to all events via `static_iota`, storing parent tags in `m_parents`.

**Destructor** (identical to `Signal`):
- Unsubscribe all events with stored parent tags.

**`notify(std::size_t event_index) noexcept`** — the key behavioral difference:
```
set bit via fetch_or
if bit was newly set:
    compute new_val = prev | bit
    if new_val == all_mask:
        m_semaphore.release()
```
Where `all_mask = (1 << N) - 1`. Only release the semaphore when *every* bit is set.

**`wait() noexcept`**:
```
while m_pending != all_mask:
    m_semaphore.acquire()
```

**`poll() noexcept`** — returns `std::optional<std::tuple<EventsT *...>>`:
```
if m_pending != all_mask → return nullopt
return tuple of pointers (all guaranteed ready)
```
Helper `getAllEventsTuple_()` uses `static_iota` + fold over `std::tuple` construction.

**`reset() noexcept`** — clears ALL state:
```
m_pending.store(0)
fold over: std::get<I>(m_events).resetEvent()
```
No per-event reset parameter needed — join is all-or-nothing.

**No iterator** — join yields all events at once, not a stream.

**`getAllEventsTuple_()` private helper:**
```cpp
[[nodiscard]] std::tuple<EventsT *...> getAllEventsTuple_() const noexcept {
    std::tuple<EventsT *...> result;
    static_iota<sizeof...(EventsT)>([&](auto... event_index) noexcept {
        ((std::get<event_index>(result) =
          std::addressof(std::get<event_index>(m_events))), ...);
    });
    return result;
}
```

### Step 2 — Add `join()` free function after `select()`

- Files: `lib/engine/core/Core.Concurrency.Event.cppm`
- Operations: edit (insert after line 378, after `select()` definition)

```cpp
template<typename... EventsT>
    requires (sizeof...(EventsT) > 0 &&
              sizeof...(EventsT) <= bit_count_v<std::size_t> &&
              std::conjunction_v<std::is_base_of<IEvent, EventsT>...>)
[[nodiscard]] JoinSignal<EventsT...> join(EventsT &... events) noexcept {
    return JoinSignal(events...);
}
```

### Step 3 — No changes to `.cpp` file

- Files: `lib/engine/core/Core.Concurrency.Event.cpp`
- Operations: none

`JoinSignal` is fully defined in the module interface (same pattern as `Signal`).

---

## Phase 2 — Unit tests

### Step 1 — Add `JoinSingle` test namespace

- Files: `lib/engine/tests/core/Core.Concurrency.Event.Tests.cppm`
- Operations: edit

```cpp
namespace JoinSingle {
    PPR_UNIT_TEST(poll_empty_returns_nullopt) {
        PulseEvent event;
        auto signal = join(event);
        const auto result = signal.poll();
        PPR_ASSERT(!result.has_value());
    };

    PPR_UNIT_TEST(poll_after_emit_returns_tuple) {
        PulseEvent event;
        auto signal = join(event);
        event.emitEvent();
        const auto result = signal.poll();
        PPR_ASSERT(result.has_value());
        // Single-event join returns tuple<Event*>
        PPR_ASSERT(std::get<0>(*result) == std::addressof(event));
    };

    PPR_UNIT_TEST(reset_clears_all) {
        PulseEvent event;
        auto signal = join(event);
        event.emitEvent();
        PPR_ASSERT(signal.poll().has_value());
        signal.reset();
        PPR_ASSERT(!signal.poll().has_value());
    };
}
```

### Step 2 — Add `JoinMulti` test namespace

- Files: `lib/engine/tests/core/Core.Concurrency.Event.Tests.cppm`
- Operations: edit

```cpp
namespace JoinMulti {
    PPR_UNIT_TEST(poll_partial_not_ready) {
        PulseEvent a;
        PulseEvent b;
        auto signal = join(a, b);
        a.emitEvent();
        const auto result = signal.poll();
        PPR_ASSERT(!result.has_value());  // B not ready
    };

    PPR_UNIT_TEST(poll_all_ready) {
        PulseEvent a;
        PulseEvent b;
        auto signal = join(a, b);
        a.emitEvent();
        b.emitEvent();
        const auto result = signal.poll();
        PPR_ASSERT(result.has_value());
        PPR_ASSERT(std::get<0>(*result) == std::addressof(a));
        PPR_ASSERT(std::get<1>(*result) == std::addressof(b));
    };

    PPR_UNIT_TEST(reset_then_repoll_needs_all) {
        PulseEvent a;
        PulseEvent b;
        auto signal = join(a, b);
        a.emitEvent();
        b.emitEvent();
        PPR_ASSERT(signal.poll().has_value());
        signal.reset();
        a.emitEvent();
        PPR_ASSERT(!signal.poll().has_value());  // only A ready
    };

    PPR_UNIT_TEST(three_events_all_ready) {
        PulseEvent a;
        PulseEvent b;
        PulseEvent c;
        auto signal = join(a, b, c);
        a.emitEvent();
        b.emitEvent();
        c.emitEvent();
        const auto result = signal.poll();
        PPR_ASSERT(result.has_value());
    };
}
```

### Step 3 — Wire into test hierarchy

- Files: `lib/engine/tests/core/Core.Concurrency.Event.Tests.cppm`
- Operations: edit (add `join_single`, `join_multi` aggregate tests, add to `event` root)

```cpp
PPR_UNIT_TEST(join_single) {
    _.recurse({
        JoinSingle::poll_empty_returns_nullopt,
        JoinSingle::poll_after_emit_returns_tuple,
        JoinSingle::reset_clears_all,
    });
};

PPR_UNIT_TEST(join_multi) {
    _.recurse({
        JoinMulti::poll_partial_not_ready,
        JoinMulti::poll_all_ready,
        JoinMulti::reset_then_repoll_needs_all,
        JoinMulti::three_events_all_ready,
    });
};
```

Then add both to the `event` root aggregate:
```cpp
Events::join_single,
Events::join_multi,
```

---

## Design rationale

| Decision | Rationale |
|----------|-----------|
| New class, not template specialization | Zero risk of breaking existing `select()` behavior; cleaner semantics |
| Same atomic bitset + semaphore pattern | Proven, no new lock-free primitives |
| `all_mask` computed at compile time | No runtime overhead |
| No iterator | Join yields one batch, not a stream — iterator would be misleading |
| `reset()` clears all events | All-or-nothing semantics match the all-or-nothing trigger |
| `std::optional<std::tuple<...>>` from `poll()` | Caller can choose non-blocking check or blocking `wait()` |
| No `BroadcastEvent` variant tests | Same IEvent contract as `PulseEvent`; coverage is equivalent |

## NeverEvent interaction

If any event in the join set is a `NeverEvent`, `all_mask` can never be reached — `join()` will block forever. This is correct and consistent; document it. Users are responsible for not including `NeverEvent` in `join()`.

## Risk assessment

- **Low.** All patterns are copy-paste from existing `Signal` with a single condition change in `notify()` and a different return type from `poll()`.
- **No changes to existing files** outside of adding new code (insertions only).
- **No new concurrency primitives.** Same `counting_semaphore`, same `atomic<size_t>`, same `fetch_or`.
- **Test coverage** mirrors `SignalSingle`/`SignalMulti` with additional partial-readiness tests.
