# Synthesis: `join()` — All-Events Synchronization

**Source files:** 3 proposals + 3 cross-reviews

---

## High Confidence — Areas Where All/Majority Agreed

### 1. `poll()` must use CAS for atomic consumption

**Consensus: Mandatory.** Proposal B's non-atomic `poll()` was flagged as a **critical correctness bug** by all three reviewers. The `compare_exchange_weak` pattern from Proposal A is the correct approach:

- Load `m_pending`
- If `!= kAllMask`, return `nullopt`
- CAS `kAllMask → 0`; on failure, retry (typical ABA-safe loop)
- On success, return `std::tuple<EventsT*...>`

### 2. Separate module partition

**Consensus: Yes (2 of 3 reviewers).** Create `Core.Concurrency.Join.cppm` exporting `engine.core:concurrency.join`.

- Pros: Follows existing pattern (`Channel` → `:concurrency.channel`, `Context` → `:concurrency.context`); keeps `Event.cppm` from growing past 600+ lines
- Cons: Requires `.cppm` + `.cpp` + `CMakeLists.txt` + `Core.cppm` edits (4 files)
- The dissenting reviewer (C) suggested keeping it in `Event.cppm`, but the majority favors isolation

### 3. Reject IEvent composition for V1

**Consensus: All 3 reviewers.** Proposal C's `Join`-as-`IEvent` approach is the right long-term vision but introduces:
- Cascading subscription/unsubscription (O(n) cost)
- Dangling-reference hazard when used as temporary inside `select()`
- Dual-inheritance lifecycle complexity (`IEvent` + `ISignal`)
- Synchronous notification propagation through nested trees

Defer to V2 with proper lifetime safety design.

### 4. Reject `DynamicJoin` for V1

**Consensus: All 3 reviewers.** The type-erased `std::vector<IEvent*>` return violates the codebase's type-safe philosophy. Heap-allocated slot array (`unique_ptr<JoinSlot[]>`) misaligns with allocator conventions. If runtime-variable event counts are needed, a type-erasure strategy must be designed alongside the engine's existing patterns.

### 5. Reject cancel event parameter for V1

**Consensus: All 3 reviewers.**

- The `IEvent*` pointer is nullable, dangles easily
- The overload `join(cancel, a, b)` creates ambiguity when first arg is an `IEvent`
- The existing `IContext` system already provides composable cancellation
- Can be added post-V1 via a wrapper or partial specialization

### 6. Class name: `Join` (not `JoinedSignal` or `JoinSignal`)

**Consensus: 2 of 3 reviewers (B and C preferred `Join`).** Parallels `Signal` (returned by `select()` — class `Signal`). The class returned by `join()` should be `Join`. Full type: `pP::Join<A, B>`.

---

## Needs Judgment — Areas Where Reviewers Disagreed

### Iterator support

| Position | Reviewers |
|----------|-----------|
| No iterator — batch is different from stream | A, B |
| Add iterator with repeating-barrier semantics (fix the `wait()`/`poll()` race) | C |

**Trade-off:** Adding an iterator is 40-50 lines of code and makes `join()` usable in range-for loops, consistent with `Signal`. But it introduces a subtle race: `wait()` returns when `m_pending == kAllMask`, then `poll()` (CAS) may fail if another thread consumed. Without iterator, the user writes `while (auto r = join(a,b).poll()) { ... }`.

**Verdict: No iterator for V1.** The manual `while` loop is clearer for batch semantics. The iterator race is a real concern that needs design work. Add in a follow-up.

### Single-event specialization

**Proposal:** Mirror `Signal<EventT>` which returns `EventT*` instead of `std::tuple<EventT*>`.

| Position | Reviewers |
|----------|-----------|
| Yes, add specialization | C |
| Not mentioned | A, B |

**Trade-off:** A single-event join is a valid degenerate case (e.g., generic code). Returning `std::tuple<PulseEvent*>` is slightly awkward but consistent. Adding a specialization adds ~30 lines of code and a separate test suite.

**Verdict: Add generic (tuple-based) version only for V1.** The specialization can be added if a concrete need arises. The tuple return is always correct; the specialization is an optimization.

### `static_assert` rejecting `NeverEvent`

| Position | Reviewers |
|----------|-----------|
| Add `static_assert` to catch at compile time | C |
| "Document it" — let it block forever | A, B |

**Trade-off:** `join(neverEvent, anyEvent)` can never complete. A `static_assert` with a clear message (`"join with NeverEvent will never fire by design"`) is cheap and catches real bugs before runtime. However, rejecting it prevents users from intentionally using `NeverEvent` as a "never" sentinel.

**Verdict: Add `static_assert` with a clear message.** Add a fold expression `(... && not std::is_same_v<NeverEvent, EventsT>)` in the requires-clause or as a `static_assert` inside the class body. Provide a `NeverPlaceholderEvent` if users need a never-firing event that composes with `join()`.

### `reset()` does not drain the semaphore

| Position | Reviewers |
|----------|-----------|
| Add `while (m_semaphore.try_acquire()) {}` after `poll()` | C |
| Not mentioned (semaphore desync is benign) | A, B |

**Trade-off:** After `poll()` clears `m_pending`, the semaphore count is still 1 (from `notify()` release). Next `wait()` acquires immediately, sees `m_pending == 0`, and re-acquires — one spurious wakeup per cycle. `try_acquire()` drain eliminates this. However, drain itself is not free (exactly N `try_acquire()` calls where N = number of semaphore releases).

**Verdict: Add drainage.** The spurious wakeup is consistent with `Signal`'s existing behavior (same issue exists), but `Join` should be cleaner since this is new code. Add `while (m_semaphore.try_acquire()) {}` inside `poll()` after successful CAS.

### `kAllMask` shift UB at `N == bit_count_v<std::size_t>`

**Discovered by:** Reviewer A.

`std::size_t{1u} << sizeof...(EventsT)` is UB when `sizeof...(EventsT) == bit_count_v<std::size_t>`. The requires-clause permits `<= bit_count_v`, but the shift requires `< bit_count_v`.

**Fix:** Use `(std::size_t{1u} << (sizeof...(EventsT) - 1u)) * 2u - 1u` or guard with `N == bit_count_v ? ~std::size_t{0} : (std::size_t{1u} << N) - 1u`.

**Verdict: Fix with the guard pattern.** This is a correctness requirement.

### Test coverage — `BroadcastEvent` subscription

| Position | Reviewers |
|----------|-----------|
| Must test with `BroadcastEvent` | A, B |
| Not a priority | C's default |

**Verdict: Add `BroadcastEvent` tests.** `BroadcastEvent` has a mutex-protected subscriber list and different notify fan-out. Testing the multi-subscriber path is critical for join correctness.

### Concurrent `poll()` test

**All reviewers** agree this should be tested but none provide concrete test code. The `PPR_UNIT_TEST` framework uses `PPR_ASSERT` (assertions only in debug). Thread timing tests are unreliable under assertion-only conditions.

**Verdict: Add a best-effort concurrent test** that spawns two threads racing on `poll()` and verifies exactly one wins. Mark as `[runnable_multiple_times]` since it depends on scheduling.

---

## Final Plan

### Phase 1 — Create `Core.Concurrency.Join.cppm` module interface

- **Files:** `lib/engine/core/Core.Concurrency.Join.cppm` (create)
- **Operations:** create

```cpp
module;
#include "pP/Macros.h"
export module engine.core:concurrency.join;

import :concurrency.event;
import :hal;

import std;

export namespace pP {

template<typename... EventsT>
    requires (sizeof...(EventsT) > 0 &&
              sizeof...(EventsT) <= bit_count_v<std::size_t> &&
              std::conjunction_v<std::is_base_of<IEvent, EventsT>...> &&
              (... && not std::is_same_v<NeverEvent, EventsT>))
class [[nodiscard]] alignas(hal::cacheline_size_v) Join final : public ISignal {
    std::counting_semaphore<> m_semaphore{0};
    std::atomic<std::size_t> m_pending{0};
    static constexpr std::size_t kAllMask_ =
        sizeof...(EventsT) == bit_count_v<std::size_t>
            ? ~std::size_t{0}
            : (std::size_t{1u} << sizeof...(EventsT)) - 1u;

    std::tuple<EventsT &...> m_events;
    std::array<TagPtr<ISignal>, sizeof...(EventsT)> m_parents{};

public:
    using result_type = std::tuple<EventsT *...>;

    explicit Join(EventsT &... events PPR_LIFETIME_BOUND) noexcept
        : m_events(events...) {
        static_iota<sizeof...(EventsT)>([&](auto... event_index) noexcept {
            ((m_parents[event_index] = std::get<event_index>(m_events).subscribeEvent(
                  TagPtr<ISignal>{this, event_index})), ...);
        });
    }

    ~Join() noexcept {
        static_iota<sizeof...(EventsT)>([&](auto... event_index) noexcept {
            ((std::get<event_index>(m_events).unsubscribeEvent(
                  TagPtr<ISignal>{this, event_index},
                  m_parents[event_index])), ...);
        });
    }

    void notify(const std::size_t event_index) noexcept override {
        const std::size_t bit = std::size_t{1u} << event_index;
        const std::size_t prev = m_pending.fetch_or(bit, std::memory_order_release);
        if ((prev & bit) == 0u) [[likely]] {
            const std::size_t current = prev | bit;
            if (current == kAllMask_) {
                m_semaphore.release();
            }
        }
    }

    void wait() noexcept override {
        while (m_pending.load(std::memory_order_acquire) != kAllMask_) {
            m_semaphore.acquire();
        }
    }

    [[nodiscard]] std::optional<result_type> poll() noexcept {
        std::size_t pending = m_pending.load(std::memory_order_acquire);
        for (;;) {
            if (pending != kAllMask_) {
                return std::nullopt;
            }
            if (m_pending.compare_exchange_weak(
                    pending, std::size_t{0},
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) [[likely]] {
                break;
            }
        }
        drainSemaphore_();
        return getAllEvents_();
    }

    void reset() noexcept {
        static_iota<sizeof...(EventsT)>([&](auto... event_index) noexcept {
            (std::get<event_index>(m_events).resetEvent(), ...);
        });
        m_pending.store(0, std::memory_order_release);
    }

private:
    [[nodiscard]] result_type getAllEvents_() const noexcept {
        result_type result{};
        static_iota<sizeof...(EventsT)>([&](auto... event_index) noexcept {
            ((std::get<event_index>(result) =
              std::addressof(std::get<event_index>(m_events))), ...);
        });
        return result;
    }

    void drainSemaphore_() noexcept {
        while (m_semaphore.try_acquire()) {}
    }
};

template<typename... EventsT>
Join(EventsT &...) -> Join<EventsT...>;

template<typename... EventsT>
    requires (sizeof...(EventsT) > 0 &&
              sizeof...(EventsT) <= bit_count_v<std::size_t> &&
              std::conjunction_v<std::is_base_of<IEvent, EventsT>...> &&
              (... && not std::is_same_v<NeverEvent, EventsT>))
[[nodiscard]] Join<EventsT...> join(EventsT &... events) noexcept {
    return Join(events...);
}

} // namespace pP
```

### Phase 2 — Create `Core.Concurrency.Join.cpp` implementation partition

- **Files:** `lib/engine/core/Core.Concurrency.Join.cpp` (create)
- **Operations:** create

```cpp
module;
#include "pP/Macros.h"
module engine.core;
import :concurrency.join;
```

### Phase 3 — Register in build system

- **Files:** `lib/engine/core/CMakeLists.txt`
- **Operations:** edit

Add `Core.Concurrency.Join.cppm` to `FILE_SET CXX_MODULES FILES` (alphabetically after `Core.Concurrency.Event.cppm`):
```
Core.Concurrency.Join.cppm
```

Add `Core.Concurrency.Join.cpp` to PRIVATE sources (alphabetically after `Core.Concurrency.Event.cpp`):
```
Core.Concurrency.Join.cpp
```

### Phase 4 — Re-export from `Core.cppm`

- **Files:** `lib/engine/core/Core.cppm`
- **Operations:** edit

Add after `import :concurrency.event;`:
```
export import :concurrency.join;
```

### Phase 5 — Unit tests

- **Files:** `lib/engine/tests/core/Core.Concurrency.Event.Tests.cppm`
- **Operations:** edit

Add `JoinSingle` and `JoinMulti` test namespaces (see Proposal B's test structure), including:

| Test | Description |
|------|-------------|
| `poll_empty_returns_nullopt` | No events emitted → nullopt |
| `poll_one_of_two_not_ready` | Only one event emitted → nullopt |
| `poll_all_ready` | Both emitted → tuple with correct pointers |
| `poll_broadcast_all_ready` | Mixed PulseEvent + BroadcastEvent |
| `poll_three_events_all_ready` | Three events, all emitted → poll succeeds |
| `reset_clears_all` | Poll → reset → poll returns nullopt |
| `reset_then_repoll_needs_all` | Reset only one constituent → re-emit → poll still null |
| `poll_wait_poll_cycle` | Full wait/poll/reset/poll cycle |
| `static_assert_rejects_never` | `join(neverEvent, pulseEvent)` fails to compile |
| `concurrent_poll_atomicity` | Two threads racing poll, exactly one wins |

Wire into test hierarchy:
```cpp
PPR_UNIT_TEST(join) {
    _.recurse({
        JoinSingle::poll_empty_returns_nullopt,
        JoinSingle::poll_after_emit_returns_tuple,
        JoinSingle::reset_clears_all,
    });
};

PPR_UNIT_TEST(join_multi) {
    _.recurse({
        JoinMulti::poll_one_of_two_not_ready,
        JoinMulti::poll_all_ready,
        JoinMulti::poll_broadcast_all_ready,
        JoinMulti::poll_three_events_all_ready,
        JoinMulti::reset_then_repoll_needs_all,
        JoinMulti::concurrent_poll_atomicity,
    });
};
```

Add `Events::join` and `Events::join_multi` to the `event` root test.

### Phase 6 — Build verification

Run tests via CLion `EngineTests` configuration or CLI fallback.

---

## Summary of Sources

| Element | Source |
|---------|--------|
| CAS-based `poll()` | Proposal A (all reviewers confirmed) |
| `kAllMask` with UB guard | Reviewer A (blind spot) |
| Define guard for `NeverEvent` | Reviewer C (endorsed by all) |
| Semaphore drain after `poll()` | Reviewer C |
| `alignas(hal::cacheline_size_v)` | Proposal A, all reviewers |
| Separate module partition | Proposals A, Reviewers A+B |
| Class name `Join` | Proposal C, Reviewers B+C |
| `get()` convenience method | Deferred (Reviewer B favored, not majority) |
| Iterator | Deferred (disagreement, add in V2) |
| Cancel parameter | Deferred (all reviewers, use IContext instead) |
| `DynamicJoin` | Deferred (all reviewers, type-safety concern) |
| Test structure `JoinSingle`/`JoinMulti` | Proposal B |
| `BroadcastEvent` integration test | Reviewers A+B (blind spot in all proposals) |
