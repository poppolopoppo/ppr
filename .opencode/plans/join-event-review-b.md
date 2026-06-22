# Cross-Review B: `join()` Implementation Proposals

**Reviewer:** Senior C++ engineer (API design, type safety, codebase consistency)
**Files examined:** `Core.Concurrency.Event.cppm`, `Core.Concurrency.Event.cpp`,
`Core.Concurrency.Event.Tests.cppm`, `Core.Concurrency.Channel.cppm`,
`Core.Concurrency.Context.cppm`, `Core.cppm`, `AGENTS.md`

---

## Review of Proposal A — Aggressive (new partition, `JoinedSignal`)

### Strengths

- **Module organization is correct.** Creating `Core.Concurrency.Join.cppm` exporting `engine.core:concurrency.join` follows the exact pattern used by every other concurrency feature (channel, context, event). No existing file is modified beyond adding an `import` line in `Core.cppm`.
- **Lock-free notify pattern is sound.** The `fetch_or` + final-bit-release design mirrors `Signal::notify()` but with the key difference: release only when all bits are set. No locks, no CAS on the hot path.
- **`poll()` CAS on the full mask** is the right approach for all-or-nothing consumption. Only one consumer thread wins.
- **No iterator.** Correct for batch semantics — join yields one complete tuple per round, not a stream.
- **`kAllMask` is `constexpr`.** Zero runtime cost; folds to a single `cmp` on x86-64.
- **CTAD guide** follows the existing `Signal` pattern.

### Weaknesses

- **`wait()` has a latent race with `poll()`.** If thread A calls `wait()` and thread B calls `poll()` (CAS-es pending to 0) between A's semaphore wakeup and its load-check, A sees `0 != kAllMask`, loops back to `acquire()`, and blocks forever. This same race exists in `Signal::wait()` today, so it's consistent — but it should be documented as a single-consumer constraint. See Blind spots below.
- **Name `JoinedSignal` is awkward.** The codebase uses single-word names (`Signal`, `Channel`, `Context`). `JoinedSignal` is the only past-tense adjective + noun compound. If the class is never exposed directly (users only see `join()`), this is minor. But if users see the type (e.g., in a `auto j = join(a,b)` declaration), `Join` would be cleaner.
- **No `get()` convenience method.** User must always write `wait()` + `poll()` in sequence. The existing `Signal` doesn't have `get()` either, so this is consistent — but `join`'s all-or-nothing semantics make `get()` a natural fit.
- **`reset()` signature differs from `Signal`.** `Signal::reset(const Event &)` takes which event to reset; `JoinedSignal::reset()` is parameterless. This is correct semantically (all-or-nothing) but introduces a third reset pattern beyond `Signal<EventT>::reset()` and `Signal<EventsT...>::reset(const Event &)`. Not a flaw, but worth noting.
- **Missing `BroadcastEvent` in test matrix.** Only `PulseEvent` tests are listed. `BroadcastEvent` exercises the multi-subscriber path which is directly relevant to join (where each constituent event has one subscriber — the JoinedSignal).

### Blind spots

1. **`wait()` / `poll()` race (single-consumer assumption).** The current codebase never documents whether `Signal` supports concurrent `wait()` + `poll()` from different threads. If it does, both `Signal::wait()` and this proposal's `waitedSignal::wait()` can deadlock. If it doesn't, the proposed code is consistent — but this should be documented explicitly, and `poll()` should be called only from the same thread that calls `wait()`, or external synchronization must be used.

2. **`alignas(hal::cacheline_size_v)` padding.** This is placed on the class, ensuring the first member (`m_semaphore`) is cache-aligned. But `m_semaphore` and `m_pending` are on the same cache line — every `notify()` (from any producer thread) touches both. For high-contention joins, this could cause false sharing between the semaphore's internal state and the atomic bitset. Consider splitting: `m_semaphore` first, `// padding here`, then `m_pending`. Alternatively, this matches `Signal` exactly, so it's consistent.

3. **`NeverEvent` with `bit_count_v<std::size_t>` limit.** The constraint says `N <= bit_count_v<std::size_t>`. If one event is `NeverEvent`, a join with `bit_count_v` other non-Never events will never complete. The mask computation is still correct — the bit for `NeverEvent` is simply never set. This is documented by the proposal but the user experience (hanging indefinitely) should probably be a `PPR_ASSERT` on construction if deducible at runtime (it is not compilable since `NeverEvent` is a concrete class, not a template marker).

4. **Over-constraint on `sizeof...(EventsT) > 0`.** Is `join()` with a single event meaningful? Yes — it's a thin wrapper that fires when that event fires. The constraint should allow `N >= 1` (already the case) but the test plan should include the degenerate case.

---

## Review of Proposal B — Conservative (inline in existing module, `JoinSignal`)

### Strengths

- **Zero risk of breaking anything.** All additions are insertions into existing files; no existing code is modified.
- **Thorough test patterns.** The test plan is more detailed than A's — covers partial readiness, full readiness, reset, three events. Well-organized into `JoinSingle` / `JoinMulti` namespaces.
- **All test scenarios include `poll` semantics** (non-blocking), which is the primary use case.
- **No new module means no build system changes.** Minimal diff.

### Weaknesses

- **Architectural mismatch.** `Core.Concurrency.Event.cppm` is for "Event signaling infrastructure" — it contains `IEvent`, `ISignal`, concrete event types, and `Signal` (the multiplexer). Adding `join()` — an **independent** composition primitive — into this file violates separation of concerns. Every other concurrency feature (channel, context) gets its own partition. This proposal would create the only case where two distinct primitives share a module partition.
- **Name `JoinSignal` is inconsistent.** The existing class is `Signal`, not `SelectSignal`. `JoinSignal` would be the only type name with a "Signal" suffix. The type alias pattern from the existing codebase would suggest `using JoinSignal = /* something */` if anything — but even that isn't used elsewhere.
- **`getAllEventsTuple_()` constructs a zero-initialized tuple then assigns.** This is two operations where a direct construction would suffice:
  ```cpp
  // Instead of zero-init + assign:
  std::tuple<EventsT *...> result{};
  static_iota<...>([&](auto... I) { ((std::get<I>(result) = ...), ...); });
  ```
  This is a minor inefficiency (trivial for pointers, but noisy).
- **`wait()` inherits the same race condition as Proposal A** (see A's blind spot #1), but with no discussion.
- **No `BroadcastEvent` tests.** Stated in design rationale as "same IEvent contract as PulseEvent; coverage is equivalent." This is wrong — `BroadcastEvent` has a mutex-protected subscriber list and different `notify()` fan-out. Join's `subscribe` path should be tested against it.
- **Module bloat.** The `Event.cppm` module partition is already 445 lines. Adding `JoinSignal` (another ~120 lines) and `join()` would push it to ~570 lines — the largest partition in the codebase. Future maintainers will struggle to find things.

### Blind spots

Same as Proposal A, plus:

1. **No discussion of module re-export.** By placing JoinSignal in the existing `:concurrency.event` partition, it is implicitly re-exported via `Core.cppm`'s `import :concurrency.event`. This is fine mechanically, but a developer looking for "where is join defined" would naturally look for a `Join.cppm` or similar, not find it, and waste time.

2. **Single-file scaling problem.** If future features (e.g., `Join::get()`, cancellation, iterator) are added to `JoinSignal`, the event module grows further. This is the same partition that also contains `NeverEvent`, `PulseEvent`, `BroadcastEvent`, and `Signal` with its iterator — conceptually distinct things.

---

## Review of Proposal C — Fresh Perspective (`Join` as `IEvent`)

### Strengths

- **`Join` as `IEvent` enables event algebra.** This is a genuinely novel insight. `select(join(a,b), c)` becomes possible because `Join` implements `IEvent` and can be subscribed to by a parent `Signal`. The codebase already has `RawChannel` implementing `IEvent` as precedent for non-leaf types participating in the event system.
- **`get()` convenience method.** `auto result = join(a,b).get()` is the most ergonomic API across all three proposals. It mirrors the blocking semantics users expect from a join.
- **Cancel event parameter.** The Go-style cancellation via an `IEvent* cancel` signal is practical for timeout patterns without adding a deadline parameter to every `join()` call.
- **Iterator follows `Signal::iterator` pattern.** Range-for over join as a repeating barrier is intuitive: each iteration waits for one complete round of all events.
- **`DynamicJoin` with counter-based tracking.** For runtime-known event counts, the counter (not bitset) approach is the correct design.
- **Name `Join`.** Concise, consistent with codebase naming conventions.

### Weaknesses

- **Architectural complexity of dual inheritance.** `Join` inherits both `IEvent` (public) and `ISignal` (private). It must simultaneously:
  - Act as a subscriber (ISignal) to its constituents
  - Act as a subscribable (IEvent) for its parent Signal
  - These two roles have different lifetimes and subscription cascading
  
  When a parent `Signal` subscribes to `Join`, `Join::subscribeEvent()` must subscribe to all its constituent events — O(n) cost. When it unsubscribes, it must unsubscribe all constituents — O(n) cost. This cascading subscription is unlike any other `IEvent` in the codebase and introduces potential for partial-subscription leaks if an exception occurs mid-subscription.

- **`poll()` uses `compare_exchange_strong`** rather than `compare_exchange_weak`. The proposal uses strong CAS which on x86-64 is fine but on ARM can be slower. More importantly, the CAS doesn't handle the case where pending was modified by a concurrent `reset()` — `reset()` does `m_pending.store(0)` which could race with CAS setting it to 0. This isn't fundamentally broken, but it's a subtlety.

- **`notify()` calls parent synchronously inside the hot path.** When the last bit is set, `notify()` both releases the semaphore AND calls `parent_sig->notify(tag)`. If the parent is a `Signal` that does its own `fetch_or` + semaphore release, this chains two atomic operations on the same critical path. For `select(join(a,b), c)`, firing the final bit of `join(a,b)` triggers two semaphore releases (one in Join, one in the parent Signal). Both are correct, but the parent's `notify()` executes in the context of the child event's emitter thread — potentially deep call stacks.

- **`result_type = std::vector<IEvent*>` for `DynamicJoin` discards all type information.** The entire codebase is designed around type-safe event references. Returning `IEvent*` vectors forces the user to downcast or use branches — the opposite of the codebase's type-safe philosophy. If dynamic joins are needed, they should be a separate discussion with a type-erased wrapper, not baked into the core API.

- **`cancel` as a raw `IEvent*` parameter is fragile.** It's a non-nullable pointer that is easy to forget or pass incorrectly. The overload `join(IEvent& cancel, EventsT&... events)` puts cancel first, which breaks the parameter order convention (events are variadic, cancel is separate). Variadic overloads also create ambiguity when the first argument is an `IEvent&`.

- **No separate module partition.** All code goes into `Core.Concurrency.Event.cppm`, which is already the largest partition. Same scaling problem as Proposal B.

- **`requires (sizeof...(EventsT) > 1)`** — the proposal's constraint excludes single-event join. This is inconsistent with `select()` which allows `select(singleEvent)`. A single-event join is trivial but should be allowed for consistency and generic code (template code shouldn't need `if constexpr (count > 1)` branches).

### Blind spots

1. **Lifetime management with cascading subscriptions.** If `Join` is used inside a `select()`:
   ```
   auto s = select(join(a, b), c);
   // s owns a Join<A,B> internally (returned by value from join())
   // The Join subscribes to A and B
   // The parent Signal subscribes to Join
   // When s goes out of scope, ~Signal() unsubscribes from Join
   // Then Join goes out of scope, ~Join() unsubscribes from A and B
   ```
   This works because `join()` returns by value and the parent `Signal` stores a tuple of references to events (including Join). But what if the user creates `join(a,b)` as a temporary in a `select()` call? Let's trace:
   ```
   auto s = select(join(a, b), c);
   ```
   `join(a,b)` returns a temporary `Join<A,B>`. `select` receives a reference to it (stored in `Signal`'s tuple). The temporary lives until the end of the full expression. After `select` returns, the temporary is destroyed — but Signal holds a dangling reference.
   
   This is a **lifetime bug**. The existing `Signal` avoids this because events are typically long-lived (owned by the application). But `Join` as `IEvent` creates a scenario where `Join` is both a temporary (in the select argument list) AND a long-lived participant. Proposals A and B have the same issue — `join()` returns by value and `select()` receives by reference — but they don't claim composition so the user is less likely to do this.

   Actually, Proposals A and B have the same lifetime bug if the user writes `select(join(a,b), c)`. But since `JoinedSignal`/`JoinSignal` isn't an `IEvent`, `select()` would reject it via the `std::is_base_of<IEvent, EventsT>` constraint. So Proposals A/B are **immune** to this by design — you can't compose them.

   Proposal C enables the pattern but introduces the dangling hazard.

2. **`resetEvent()` cascading.** When a parent Signal calls `reset(variant_containing_JoinPtr)`, it calls `Join::resetEvent()`. This calls `resetEvent()` on ALL constituents. If one constituent is a `BroadcastEvent` shared with another subscriber, the other subscriber's view is unexpectedly reset. This is different from `Signal::reset(Event)` which resets only a single event. The cascade is an architectural surprise.

3. **Test plan includes `cancel_aborts_wait` and `poll_is_atomic`** which involve complex threading. The test framework (`PPR_UNIT_TEST`) uses `PPR_ASSERT` which compiles away in release. Thread timing tests under assertion-only conditions are unreliable. The proposal doesn't discuss how to test these deterministically.

4. **`DynamicJoin`'s `JoinSlot` internal signal** uses `std::unique_ptr<JoinSlot[]>` — an array of individually heap-allocated objects. Each `JoinSlot` stores a raw pointer to its owning `DynamicJoin`. If `DynamicJoin` is moved, all `JoinSlot::m_owner` pointers dangle. The class is non-movable, which is fine, but not documented.

---

## Synthesis Recommendations

### Adopt from A:

1. **New module partition `engine.core:concurrency.join`** with files `Core.Concurrency.Join.cppm` and `Core.Concurrency.Join.cpp`. This follows the established pattern (channel → `:concurrency.channel`, context → `:concurrency.context`) and keeps the codebase navigable.

2. **Lock-free notify pattern** — `fetch_or` with final-bit semaphore release. Proven correct, no new concurrency primitives.

3. **`alignas(hal::cacheline_size_v)`** — consistent with `Signal`. Consider splitting semaphore and atomic bitset onto separate cache lines for high-contention scenarios, but that matches the existing (possibly known) trade-off.

### Adopt from B:

1. **Test organization** — `JoinSingle` / `JoinMulti` namespace split with clear, focused test cases. The partial-readiness tests are essential and B tests them best.

2. **No iterator** — correct for batch semantics. The repeating-barrier iterator from C is a nice idea but adds API surface that will almost never be used; the manual `while (auto r = join(a,b).poll()) { ... }` loop is clearer.

3. **Add `BroadcastEvent` tests** to the test plan (B's blind spot). Exercising the multi-subscriber subscription path is critical for join correctness.

### Adopt from C:

1. **Rename the class to `Join`** (not `JoinedSignal`, not `JoinSignal`). Following the precedent of `Signal` (the class returned by `select()`), the class returned by `join()` should be `Join`. The full type name `pP::Join<A, B>` is clean and parallel with `pP::Signal<A, B>`.

2. **`get()` convenience method** — `result_type get() noexcept { wait(); return poll()->value(); }`. This provides the blocking shorthand that makes join ergonomic. (Note: returns by value; the `std::tuple` of pointers is trivially copyable, so this is zero-cost with NRVO.)

3. **Reject `IEvent` inheritance for v1.** The composition use case (`select(join(a,b), c)`) is niche and introduces:
   - Dual-inheritance complexity (ISignal + IEvent)
   - Cascading subscription/unsubscription (O(n) on subscribe)
   - Dangling reference hazard when `join()` is used as a temporary inside `select()`
   - Unexpected cascading reset from parent Signal
   
   Instead, keep `Join` as implementing only `ISignal` (consistent with `Signal`). If event-algebra composition is needed later, a separate `IEvent`-compatible wrapper can be designed with proper lifetime safety.

4. **Reject cancel event parameter for v1.** The `IEvent* cancel` approach is fragile (nullability, dangling). If cancellation is needed, the existing `context` system (`IContext` extending `IEvent`) already provides a composable cancellation mechanism. A future `join_with_context(context, events...)` overload would integrate cleanly without API pollution.

5. **Reject `DynamicJoin` for v1.** The `std::vector<IEvent*>` return type discards all type information, violating the codebase's type-safe design. Dynamic event counting is a separate feature that needs a type-erasure strategy (e.g., `std::any`-like result, or a visitor pattern) that maintains the engine's safety guarantees.

### Recommended hybrid design

```
// Core.Concurrency.Join.cppm
export module engine.core:concurrency.join;
import :concurrency.event;
import std;

export namespace pP {

template<typename... EventsT>
    requires (sizeof...(EventsT) > 0 &&
              sizeof...(EventsT) <= bit_count_v<std::size_t> &&
              std::conjunction_v<std::is_base_of<IEvent, EventsT>...>)
class [[nodiscard]] alignas(hal::cacheline_size_v) Join final : public ISignal {
    // ... same internals as Proposal A's JoinedSignal ...
public:
    using result_type = std::tuple<EventsT *...>;

    explicit Join(EventsT &... events PPR_LIFETIME_BOUND) noexcept;

    [[nodiscard]] std::optional<result_type> poll() noexcept;
    void wait() noexcept override;
    void notify(std::size_t event_index) noexcept override;
    void reset() noexcept;
    
    [[nodiscard]] result_type get() noexcept {
        wait();
        return *poll();
    }
};

template<typename... EventsT>
Join(EventsT &...) -> Join<EventsT...>;

template<typename... EventsT>
[[nodiscard]] Join<EventsT...> join(EventsT &... events) noexcept {
    return Join(events...);
}

} // namespace pP

// With unit tests following B's organization (JoinSingle + JoinMulti),
// including BroadcastEvent subscription tests.
```
