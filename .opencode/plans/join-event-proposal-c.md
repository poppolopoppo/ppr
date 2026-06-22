# Proposal C — `join()`: First-Class Event Composition Primitive

## Core Idea

**`Join<EventsT...>` implements `IEvent`** — it is a first-class event, not a signal. This enables
a **full event algebra**: `select()` is OR-composition, `join()` is AND-composition. Both produce
`IEvent` instances that can nest arbitrarily:

```
select(join(a, b), join(c, d))   // (A AND B) OR (C AND D)
join(select(a, b), c)             // (A OR B) AND C
```

This is the key insight distinguishing this proposal: `join()` returns an *event*, so it participates
in the existing subscription/polling/reset protocol and composes with `select()` naturally.

---

## Phase 1 — `Join<EventsT...>` class template + `join()` free function

### Step 1 — Add `Join` class template and `join()` to the event module interface

- **Files:** `Core.Concurrency.Event.cppm`
- **Operations:** Insert after the `select()` free function (line 378), before the `NeverEvent` section

#### Design

```cpp
template<typename... EventsT>
    requires (sizeof...(EventsT) > 1 &&
              sizeof...(EventsT) <= bit_count_v<std::size_t> &&
              std::conjunction_v<std::is_base_of<IEvent, EventsT>...>)
class [[nodiscard]] Join final : public IEvent, private ISignal {
```

`Join` implements **both** interfaces:
- **`IEvent`** — so it can be subscribed to by a parent `Signal` (or another `Join`)
- **`ISignal`** (private) — so it can receive notifications from its constituent events

#### Internal state

| Member | Purpose |
|--------|---------|
| `std::tuple<EventsT&...> m_events` | References to constituent events |
| `std::atomic<std::size_t> m_pending{0}` | Bitset tracking which constituents are ready |
| `std::counting_semaphore<> m_semaphore{0}` | Blocking primitive for `wait()` |
| `TagPtr<ISignal> m_parent{}` | The subscriber (set during `subscribeEvent`) |
| `IEvent* m_cancel{nullptr}` | Optional cancel event for early abort |

Constant: `all_bits_mask_v = (size_t{1} << N) - 1`

#### ISignal implementation (private)

```cpp
void notify(std::size_t tag) noexcept override {
    size_t bit = size_t{1} << tag;
    size_t prev = m_pending.fetch_or(bit, std::memory_order_release);
    size_t current = prev | bit;
    if (current == all_bits_mask_v) {
        m_semaphore.release();
        if (m_parent.isValid()) {
            auto [sig, t] = m_parent.unpack();
            sig->notify(t);
        }
    }
}

void wait() noexcept override {
    for (;;) {
        size_t cur = m_pending.load(std::memory_order_acquire);
        if (cur == all_bits_mask_v) return;
        if (m_cancel && m_cancel->pollEvent()) return;  // abort
        m_semaphore.acquire();
    }
}
```

Differences from `Signal::notify()`:
- Releases semaphore only when **all** bits are set (not on any bit)
- Propagates to parent only on all-bits condition
- `wait()` checks for `== all_bits_mask_v` instead of `!= 0`

#### IEvent implementation

| Method | Behavior |
|--------|----------|
| `subscribeEvent` | Store parent signal; subscribe to all constituents via `subscribeEvent(TagPtr<ISignal>{this, idx})`; return old parent |
| `unsubscribeEvent` | Unsubscribe from all constituents; restore parent |
| `pollEvent` | `return m_pending.load() == all_bits_mask_v` |
| `resetEvent` | `m_pending.store(0)` + reset all constituents via fold-expression |

The `subscribeEvent`/`unsubscribeEvent` pattern mirrors `Signal`'s constructor/destructor but is
driven by the parent's subscription lifecycle — essential for composition.

#### Public API

```cpp
using result_type = std::tuple<EventsT*...>;

explicit Join(EventsT&... events, IEvent* cancel = nullptr) noexcept;

// Non-blocking: atomically check-and-consume
[[nodiscard]] std::optional<result_type> poll() noexcept;

// Blocking: wait until all ready, then consume
result_type get() noexcept {
    wait();
    return collect_();
}

// Iterator support — yields result_type tuples
class iterator { ... };
iterator begin() noexcept;
static constexpr auto end() noexcept -> std::default_sentinel_t;
```

`poll()` uses a CAS on `m_pending`:

```cpp
std::optional<result_type> poll() noexcept {
    size_t expected = all_bits_mask_v;
    if (m_pending.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
        result_type result;
        static_iota<sizeof...(EventsT)>([&](auto... idx) {
            ((std::get<idx>(result) = std::addressof(std::get<idx>(m_events))), ...);
        });
        return result;
    }
    return std::nullopt;  // includes cancel case (bits != all)
}
```

The CAS makes `poll()` atomic and safe against concurrent consumers — only one thread succeeds.

#### Iterator semantics

The iterator follows the same pattern as `Signal::iterator` but yields `result_type` tuples:

```
wait() → collect_() → reset() → wait() → collect_() → reset() → ...
```

This means a range-for loop over `join(a, b)` blocks until all are ready, yields the tuple,
resets all events, and loops. This is a repeating barrier: each iteration waits for the *next*
round of all events being ready.

#### `join()` free function

```cpp
template<typename... EventsT>
    requires (sizeof...(EventsT) > 0 &&
              sizeof...(EventsT) <= bit_count_v<std::size_t> &&
              std::conjunction_v<std::is_base_of<IEvent, EventsT>...>)
[[nodiscard]] Join<EventsT...> join(EventsT&... events) noexcept {
    return Join(events...);
}

// Overload with cancellation
template<typename... EventsT>
[[nodiscard]] Join<EventsT...> join(IEvent& cancel, EventsT&... events) noexcept {
    return Join(events..., &cancel);
}
```

### Step 2 — Add `DynamicJoin` for runtime-known event count

- **Files:** `Core.Concurrency.Event.cppm` (interface), `Core.Concurrency.Event.cpp` (implementation)
- **Operations:** Add class declaration + `join()` overload taking a span

For cases where the event count is only known at runtime:

```cpp
class [[nodiscard]] DynamicJoin final : public IEvent {
    std::span<IEvent*> m_events;
    std::atomic<std::size_t> m_ready_count{0};
    std::counting_semaphore<> m_semaphore{0};
    TagPtr<ISignal> m_parent{};
    IEvent* m_cancel{nullptr};

    // Internal ISignal implementation via a Signal-like forwarding
    struct JoinSlot : ISignal {
        DynamicJoin* m_owner;
        std::size_t m_index;
        void notify(std::size_t) noexcept override;
        void wait() noexcept override {}
    };
    std::unique_ptr<JoinSlot[]> m_slots;

public:
    using result_type = std::vector<IEvent*>;

    DynamicJoin(std::span<IEvent*> events, IEvent* cancel = nullptr);
    ~DynamicJoin() noexcept;

    // IEvent
    TagPtr<ISignal> subscribeEvent(TagPtr<ISignal> signal) noexcept override;
    void unsubscribeEvent(...) noexcept override;
    bool pollEvent() noexcept override;
    void resetEvent() noexcept override;

    // Public API
    std::optional<result_type> poll() noexcept;
    void wait() noexcept;
};
```

Uses counter-based tracking instead of bitset: `m_ready_count` reaches `m_events.size()` when all ready.

### Step 3 — Register new module sources in CMakeLists (if needed)

- **Files:** `lib/engine/core/CMakeLists.txt`
- **Operations:** No changes needed — all code goes into existing files `Core.Concurrency.Event.cppm` and `Core.Concurrency.Event.cpp`

---

## Phase 2 — Unit tests

### Step 1 — Add `Join` test suite

- **Files:** `Core.Concurrency.Event.Tests.cppm`
- **Operations:** Append test cases within the `Events` namespace

#### Test scenarios

| Test | Description |
|------|-------------|
| `poll_empty_returns_nullopt` | Two events, neither emitted → `poll()` is `nullopt` |
| `poll_one_of_two_returns_nullopt` | Only one event emitted → `poll()` is `nullopt` |
| `poll_both_ready_returns_tuple` | Both emitted → `poll()` returns populated tuple with correct pointers |
| `poll_is_atomic` | Two threads racing on `poll()`, only one wins |
| `get_blocks_until_all_ready` | `get()` blocks until second event fires |
| `reset_clears_all` | After successful poll + reset, `poll()` is `nullopt` again |
| `iteration_yields_tuples` | Range-for over join yields at least one tuple after both emit |
| `cancel_aborts_wait` | Join with cancel event: firing cancel aborts the wait |
| `cancel_poll_returns_nullopt` | After cancel fires, `poll()` returns `nullopt` |
| `composition_with_select` | `select(join(a, b), c)` works and extracts join result |
| `three_events_join` | Three events, all must be ready |

### Step 2 — Add `DynamicJoin` test suite

| Test | Description |
|------|-------------|
| `dynamic_two_events` | DynamicJoin with span size 2, both emitted → poll succeeds |
| `dynamic_cancel` | DynamicJoin with cancel, cancel fired → poll fails |

### Step 3 — Register tests in the test hierarchy

```cpp
PPR_UNIT_TEST(join) {
    _.recurse({
        Join::poll_empty_returns_nullopt,
        Join::poll_one_of_two_returns_nullopt,
        Join::poll_both_ready_returns_tuple,
        // ...
    });
};

PPR_UNIT_TEST(dynamic_join) {
    _.recurse({
        DynamicJoin::dynamic_two_events,
        DynamicJoin::dynamic_cancel,
    });
};
```

Then add `Events::join` and `Events::dynamic_join` to the `event` parent test.

---

## Phase 3 — Build verification

### Step 1 — Compile and run tests

- Use the CLion `EngineTests` run configuration or the CMake fallback
- Verify the existing test suite still passes (no regressions)
- Verify new `Join` and `DynamicJoin` tests pass

---

## Summary of design decisions

| Decision | Rationale |
|----------|-----------|
| `Join` is `IEvent`, not a separate signal type | Enables composition with `select()` — full event algebra |
| `Join` inherits `ISignal` privately | Receives constituent notifications without exposing the interface publicly |
| Atomic CAS in `poll()` | Race-free all-or-nothing consumption |
| Optional cancel event parameter | Go-style context cancellation for early abort |
| `std::tuple<EventsT*...>` return type | Zero-cost, structured-binding friendly |
| Separate `DynamicJoin` class | Runtime-variable event count cannot use bitset; different internals are cleaner than shoehorning |
| Repeating-barrier iterator | Consistent with `select()` iterator semantics — each loop iteration consumes one full round |
