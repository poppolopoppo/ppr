# Cross-Review C: `join()` Implementation Proposals — Production Readiness Assessment

## Review of Proposal A — "Aggressive: Lock-Free Batch Coordination"

### Strengths
- CAS-based `poll()` with `compare_exchange_weak` — atomic consumption, only one thread wins. Race-free unlike B.
- Comprehensive test table covering edge cases: `NeverEvent` interaction, reset cycle, single-event degenerate case, mixed event types.
- Separate module partition (`Core.Concurrency.Join.cppm`) keeps concerns clean; no risk of bloating `Event.cppm`.
- `kAllMask` is `constexpr` per instantiation — folds to a single `cmp` instruction on x86-64.
- `alignas(hal::cacheline_size_v)` prevents false sharing between cores.
- `notify()` only releases semaphore on the final bit — no redundant wakeups.
- The `wait()` loop rechecks `m_pending` after `acquire()`, correct for spurious wakeups.

### Weaknesses
- **Semaphore count desynchronization after `poll()`**: When `poll()` atomically clears `m_pending` to 0, the semaphore's internal count remains at 1 (from the `release()` in `notify()`). The next `wait()` call will acquire the semaphore immediately, see `m_pending == 0`, and loop back to `acquire()`. This causes exactly one spurious wakeup per poll–wait cycle. With redundant notifications (multiple `emitEvent()` on a `BroadcastEvent` that has been reset between), the desync worsens.
- New module partition requires: new `.cppm`, new `.cpp` (nearly empty), `CMakeLists.txt` changes, and `Core.cppm` re-export. Marginal but nonzero build overhead.
- No single-event specialization — `JoinedSignal<PulseEvent>` works but misses the perf optimization `Signal<EventT>` gets.
- `reset()` stores `m_pending = 0` with `memory_order_release` but nothing drains the semaphore. If `notify()` fired between `reset()` and a re-poll, the old semaphore release remains.

### Blind Spots
1. **`reset()` / `notify()` race**: `reset()` stores 0 to `m_pending`, then resets constituents. If a constituent's `notify()` fires between `m_pending.store(0)` and the constituent reset, the bit gets re-set and the semaphore released. The `wait()` that follows would see the bit and wake up. Not a correctness bug (the user gets a spurious but valid wakeup) but surprising — documented as "reset is not thread-safe with concurrent emit."
2. **`NeverEvent` — runtime infinite wait, no compile-time guard**: `join(neverEvent, pulseEvent)` compiles fine and blocks forever. `static_assert` with `std::negation_v<std::is_same_v<NeverEvent, EventsT>>...` would catch this at compile time with a clear message. Proposal A says "document it" — adequate for V1, but a `static_assert` is cheap and would prevent real bugs.
3. **Move semantics**: `join()` returns `JoinedSignal` by value. `std::counting_semaphore` is non-copyable. This relies on guaranteed copy elision (C++17). If elision fails, the moved-from-object has dangling references. All three proposals share this blind spot.
4. **No concurrency/TSan tests**: No test with two threads racing `poll()` or racing `notify()` vs `poll()`. The CAS in `poll()` makes single-consumer safe, but a TSan test would validate the memory ordering.
5. **No `BroadcastEvent` multi-subscriber edge case**: If a `BroadcastEvent` is shared between two `JoinedSignal` instances, its list of subscribers is correctly managed, but the join's `notify()` could fire concurrently on both instances. This is correct but untested.
6. **`poll()` CAS failure fallback**: On CAS failure, the loop retries with the updated `pending` value. If the value is still `kAllMask`, it retries the CAS. If another thread consumed it in between, `pending` becomes 0 and the loop returns `nullopt`. This is correct.

---

## Review of Proposal B — "Conservative"

### Strengths
- **Zero new files**: Everything is an insertion into existing `Core.Concurrency.Event.cppm`. No build system changes, no module re-exports.
- **Lowest implementation risk**: Literally copy-paste `Signal` with one condition change in `notify()`.
- Follows the exact same lock-free pattern (`fetch_or` + semaphore) as the proven `Signal`.
- Simpler to review and validate than A or C.

### Weaknesses
- **CRITICAL: `poll()` is not atomic and does not consume.** The pseudocode shows:
  ```
  if m_pending != all_mask → return nullopt
  return tuple of pointers
  ```
  This is a load-and-check with no CAS and no clearing. Two threads calling `poll()` simultaneously both get the result. Repeated calls keep returning the tuple indefinitely. This is not an edge case — it's a fundamentally broken consumer model.
- **No `poll()` clearing means `reset()` is the only way to reset state**, breaking the `poll()->process()->reset()` pattern that `select()` users rely on.
- Minimal test coverage (4 multi-event tests vs A's 9, C's 11+). No `NeverEvent` test, no mixed event types, no three-event test (wait — there *is* a three-event test, but only one).
- Single-event `JoinSignal` returns `std::tuple<PulseEvent*>` — awkward compared to `EventT*` in `Signal<EventT>`.
- Bloats `Core.Concurrency.Event.cppm` further (already 445 lines) — the module is a growing monolith.

### Blind Spots
1. **All of A's blind spots apply**, plus:
2. **No `poll()` consumption means stale semaphore releases** — every `notify()` for a bit that's already set still calls `release()`. The semaphore count grows unboundedly. The `if ((prev & bit) == 0u)` check in the pseudocode prevents this (same as A), but without `poll()` clearing the bits, repeated `wait()`/`poll()` cycles become undefined.
3. **`NeverEvent` handling**: "Document it" — no test, no compile-time check, no runtime detection.
4. **No `BroadcastEvent` or mixed-type tests**: "Same IEvent contract as PulseEvent; coverage is equivalent" is complacent. `BroadcastEvent` differs — it supports multiple subscribers and uses `atomic_flag` instead of `atomic<size_t>`. The notify chain is `BroadcastEvent::emitEvent()` → iterates all subscribers → calls each `ISignal::notify()`. This needs testing at least at the integration level.

---

## Review of Proposal C — "Fresh Perspective: First-Class Event Composition"

### Strengths
- **Most innovative**: `Join<EventsT...>` implements `IEvent`, enabling `select(join(a,b), join(c,d))` — full event algebra. This is genuinely powerful for complex synchronization graphs.
- `poll()` uses `compare_exchange_strong` — atomic consumption, race-free.
- **Cancel event** parameter (`IEvent* cancel`) provides Go-style context cancellation. A concrete use case: `join(timeoutEvent, workEvent, cancelEvent)`.
- Iterator support consistent with `Signal` — repeating barrier semantics.
- `DynamicJoin` for runtime-known event counts (spans, plugin systems).
- No new files — all in existing `Event.cppm` + `.cpp`.
- `pollEvent()` (IEvent) returns `m_pending == all_mask` — trivially correct for the IEvent interface.

### Weaknesses
- **CRITICAL: subscribe/unsubscribe lifecycle complexity.** `Join` implements both `IEvent` (public) and `ISignal` (private). When subscribed *to*, it must subscribe *to* all constituents. When unsubscribed *from*, it must unsubscribe from constituents and restore previous state. The pseudocode glosses over this. Re-parenting (someone subscribes to a Join that already has a parent) would leak constituent subscriptions from the first parent. The `unsubscribeEvent` "restore" logic is ambiguous — does it restore the Join's parent, or the constituents' previous parents? These are different things.
- **`pollEvent()` vs `poll()` ambiguity**: `pollEvent()` (IEvent, non-consuming check) exposes the Join's internal state publicly. A user who calls `if (join.pollEvent()) { auto r = join.poll(); }` has a TOCTOU race — another thread could consume between the two calls. This is inherent in exposing both interfaces.
- **Cancel event: `poll()` returns `nullopt` for both "cancelled" and "not ready"**. The user has no way to distinguish. They must check `cancel.pollEvent()` separately. The `wait()` uses cancel to avoid blocking forever, which is useful, but `poll()` consumers get no benefit.
- **`DynamicJoin`**: heap-allocated slot array (`std::unique_ptr<JoinSlot[]>`), type-erased return (`std::vector<IEvent*>`), counter-based tracking instead of bitset. This is significant complexity for a niche use case. For a V1, this is over-engineering — the template approach covers 95% of use cases.
- **Most code changes of any proposal**: new `Join` template, `DynamicJoin`, iterator, cancel logic, slot forwarding.

### Blind Spots
1. **All of A's blind spots apply** (substitute `Join` for `JoinedSignal`), plus:
2. **Composition with `select()` returns awkward types**: `select(join(a,b), join(c,d)).poll()` returns `std::variant<Join<A,B>*, Join<C,D>*>`. The user must `std::visit` and then potentially extract from the Join. Compare to A/B where a single `join(a,b,c,d).poll()` returns `std::tuple<A*,B*,C*,D*>` with structured bindings. Composition is powerful but the ergonomics degrade with depth.
3. **`Join` as `IEvent` cannot be stored in a container**: Each instantiation is a different type (`Join<PulseEvent, BroadcastEvent>` vs `Join<PulseEvent, PulseEvent>`). The `DynamicJoin` is the container-friendly version, which adds heap allocation.
4. **`unsubscribeEvent` on a `Join` requires constituent unsubscription + restoration** — but during destruction of a composed tree (e.g., outer `Signal` destructor unsubscribing from inner `Join`), the order of operations must be precise. If the inner `Join` is destroyed before the outer `Signal` finishes unsubscribing, dangling pointer UB.
5. **`DynamicJoin::JoinSlot::wait()` is a no-op** (returns `{}`). But if the `DynamicJoin` is itself used as an event in a `select()`, its slots' `wait()` is never called — `notify()` is the only path. This is correct but fragile: any future code path that calls `wait()` on a slot would silently do nothing.

---

## Synthesis Recommendations

### Adopt from Proposal A
- **CAS-based `poll()`** with `compare_exchange_weak`/`compare_exchange_strong`. Atomic consumption is non-negotiable — without it (as in B), the consumer model is broken.
- **Test coverage template**: test `NeverEvent`, reset cycle (poll → reset → re-poll → re-emit), single-event degenerate case, mixed event types, three-event mask correctness. A's test table is the most complete.
- **`kAllMask` compile-time computation** and `alignas(hal::cacheline_size_v)` — zero-cost and prevents false sharing.
- **Class name `JoinedSignal`** (or `JoinSignal`) — distinguishes it from the IEvent hierarchy, avoiding A's `pollEvent`/`poll` ambiguity.

### Adopt from Proposal B
- **Keep in existing module partition** (`Core.Concurrency.Event.cppm`). Adding a new partition for a ~100-line template class is unnecessary overhead. The existing module at 445 lines can absorb 100-150 more lines without becoming unwieldy. If the event module grows beyond ~600 lines, *then* refactor into partitions.
- **Same lock-free pattern**: `fetch_or` + `std::counting_semaphore` + atomic bitset. Proven, simple, correct.
- **Wait for a concrete use case before adding `DynamicJoin` or IEvent composition**.

### Adopt from Proposal C — selectively, for V2
- **Cancel event parameter** as an optional extension *only if* the lifecycle complexity is resolved. For V1, skip it. Design `JoinSignal` so that `m_cancel` can be added later without breaking ABI.
- **IEvent composition** (`select(join(a,b), join(c,d))`) is the right long-term direction, but defer to V2. The `subscribeEvent`/`unsubscribeEvent` lifecycle for `Join`-as-`IEvent` needs careful design review and dedicated tests. Ship the flat version (A/B style) first.

### Additional recommendations not in any proposal
1. **Add a `static_assert` rejecting `NeverEvent`** in the `join()` requires clause:
   ```cpp
   && (... && not std::is_same_v<NeverEvent, EventsT>)
   ```
   Provide a compile-time error with a clear message pointing to the documented workaround. "Document it" is insufficient when the alternative is a silent infinite wait.

2. **Drain the semaphore after `poll()` consumes** to prevent the desync wakeup:
   ```cpp
   while (m_semaphore.try_acquire()) {}
   ```
   This eliminates the spurious wakeup after poll–wait cycles.

3. **Add thread-sanitizer (TSan) tests** for:
   - Two threads racing `poll()` — only one should succeed
   - `notify()` concurrent with `reset()`
   - Memory ordering validation (release/acquire chain on `m_pending`)
   - `BroadcastEvent` notify racing with `JoinSignal` poll

4. **Document the `reset()` safety contract**: "Must not be called concurrently with constituent `emitEvent()` calls targeting this join." Same constraint as `Signal::reset()`.

5. **Consider a single-event specialization** (`JoinSignal<EventT>`) that returns `EventT*` instead of `std::tuple<EventT*>`, mirroring `Signal<EventT>`.

### Comparative verdict

| Dimension | A | B | C |
|-----------|---|---|---|
| Correctness (poll atomicity) | ✅ | ❌ broken | ✅ |
| Build complexity | ⚠️ new partition | ✅ none | ✅ none |
| Composition power | ❌ | ❌ | ✅ full algebra |
| Lifecycle safety | ✅ simple | ✅ simple | ❌ complex |
| Test coverage | ✅ good | ⚠️ minimal | ✅ good |
| Cancel/abort | ❌ | ❌ | ✅ partial |
| Move safety risk | ⚠️ | ⚠️ | ❌ dual interface |
| Overall V1 readiness | ✅ high | ❌ poll bug | ⚠️ needs lifecycle review |

**Recommended path**: Implement Proposal A's design (CAS poll, full tests, alignas, separate class) **inside Proposal B's module** (no new partition). Add the `static_assert` for `NeverEvent` and the semaphore drain. Defer cancel events, IEvent composition, and DynamicJoin to a well-specified V2.
