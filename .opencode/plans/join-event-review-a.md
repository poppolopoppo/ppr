## Review of Proposal A — Aggressive: Lock-free batch coordination

### Strengths

1. **Correct lock-free `notify()`** — Single `fetch_or`, release semaphore exactly once when `kAllMask` is reached. No spurious wakeups. No races in the hot path.
2. **`poll()` uses CAS for atomic consumption** — `compare_exchange_weak` ensures only one consumer claims the result. This is mandatory for correctness (Proposal B misses this entirely).
3. **Clean module isolation** — A new `Core.Concurrency.Join.cppm` partition follows the existing decomposition pattern and keeps the already-large `Core.Concurrency.Event.cppm` from growing further.
4. **CTAD guide provided** — Consistent with existing `Signal(EventsT&...) -> Signal<EventsT...>`.
5. **Comprehensive test table** — Covers edge cases (`NeverEvent`, `join_single_event`, `join_reset_resets_all`, `join_three_events`) that the other proposals gloss over. The `join_with_never_never_fires` test explicitly documents the infinite-wait semantic.
6. **Design rationale documented** — Explains `std::tuple` vs `std::variant`, why a separate class is necessary, and why iterators are omitted. The rationale shows architectural maturity.
7. **Zero-cost `kAllMask`** — Compile-time `constexpr`, folds to a single `cmp` in the hot path.

### Weaknesses

1. **Duplicates `Signal` internals** — The `JoinedSignal` class is structurally identical to `Signal` (same member layout, same subscription pattern in constructor/destructor). The only behavioral differences are in `notify()` (all-bits vs any-bit), `poll()` (full mask CAS vs pop-least-significant-bit CAS), and `reset()` (all vs specific event). This is ~80 lines of copy-paste code that must be maintained in parallel.
2. **New module partition adds build friction** — Requires creating two new files (`.cppm` + `.cpp`), editing `CMakeLists.txt`, and editing `Core.cppm` to import the new partition. While not onerous, it's more surface area than an insertion into the existing file.
3. **No iterator support** — `Signal` has iterators, `JoinedSignal` doesn't. This inconsistency means users who want a `wait-poll-process-reset` loop must hand-roll it. The proposal explicitly argues this is intentional ("join yields a batch, not a stream"), but the lack of a repeating-barrier API means `join()` cannot be used in range-for loops, which is a discoverability hit.
4. **`poll()` returns `std::optional<Result>` without consuming from constituent events** — After `poll()` succeeds, the constituent events' flags are not cleared (only `m_pending` is cleared). The caller must still call `reset()` per event. This is technically correct but means the `poll()` CAS only protects against concurrent `poll()` calls on the same `JoinedSignal`, not concurrent consumption of the constituent events. Though this is inherent to the design (events are shared state), it should be documented.

### Blind spots

1. **`poll()` / `wait()` interaction with concurrent `reset()`** — If thread A is blocked in `wait()` and thread B calls `reset()` concurrently, thread A may wake spuriously (semaphore release from a prior completion), find `m_pending != kAllMask`, and re-enter `acquire()`. This is correct *by accident*, but the proposal doesn't discuss or test it. A synchronization edge case involving `reset()` racing with `wait()` could consume a stale semaphore token and deadlock. The analysis should prove liveness.

2. **`kAllMask` shift on `unsigned long long` vs `std::size_t`** — `std::size_t{1u} << sizeof...(EventsT)` when `sizeof...(EventsT) == bit_count_v<std::size_t>` is undefined behavior (shift by bit width). The requires clause limits to `<= bit_count_v<std::size_t>`, but when `==`, the shift overflows. Fix: either require `< bit_count_v` or use a guard `(N == bit_width ? 0 : (1 << N))` pattern. This is the same bug that exists in `Signal`'s code, but the proposal inherits it.

3. **`alignas(hal::cacheline_size_v)` on `JoinedSignal`** — The proposal adds this annotation (same as `Signal`), but doesn't verify that the member layout won't have false-sharing between `m_semaphore`, `m_pending`, and `m_events`. False sharing between `m_pending` (written by `fetch_or` in `notify()`) and `m_events` (read by `getAllEvents_()`) could degrade performance. This is a pre-existing concern with `Signal` too.

4. **Constructor/destructor exception safety** — If the `JoinedSignal` constructor fails midway through subscribing (e.g., event 3 of 5 fails to subscribe), it doesn't unwind the first 2 subscriptions. The destructor won't run because construction didn't complete. This is the same pattern as `Signal`, so it's consistent, but still brittle for a reliable game engine.

5. **No `RawChannel` or `IContext` tests** — The test plan lists `PulseEvent` and `BroadcastEvent`, but `RawChannel` (which also implements `IEvent`) could have different subscription semantics that interact poorly with `join()`. Proposal A should at least mention this.

## Review of Proposal B — Conservative

### Strengths

1. **Zero build system changes** — Everything lives in the existing file, with insertions only. This is the lowest-risk, lowest-effort path.
2. **Pattern-matched from existing `Signal`** — Easy to review, easy to verify correctness by diffing against the known-working `Signal`. The code is trivially structured and familiar.
3. **Sensibly scoped** — No iterator, no DynamicJoin, no cancel event. The proposal acknowledges these as out-of-scope, keeping the initial implementation focused.
4. **Good basic test coverage** — Single-event and multi-event cases cover the essential scenarios (partial readiness, all ready, reset cycle, three events).

### Weaknesses

1. **`poll()` is NOT atomic — CRITICAL RACE CONDITION** — The pseudo-code shows:
   ```
   if m_pending != all_mask → return nullopt
   return tuple of pointers (all guaranteed ready)
   ```
   This loads `m_pending`, checks it without atomic claim, then returns. Two threads can both see `m_pending == all_mask` and both return a "successful" result. They will race to process the same events, and both will attempt to `reset()` on overlapping sets. This is a **correctness bug**. The CAS loop from Proposal A is mandatory.

2. **Poll-less `wait()`** — The `wait()` loads `m_pending` and checks `!= all_mask`, then calls `acquire()`. But without a CAS in the matching `poll()`, the semaphore token might be consumed by one thread in `wait()` while another thread sneaks through `poll()`, reducing the semaphore count below zero (undefined behavior for `counting_semaphore`). Actually, `counting_semaphore::acquire()` decrements — if count is 0, it blocks. The issue is the double-consumption of the *logical* completion, not the semaphore.

3. **No discussion of concurrent `poll()`** — Even if the pseudo-code were fixed with CAS, the proposal doesn't discuss concurrent `poll()` behavior, which is essential for multi-threaded usage.

4. **Clutters `Core.Concurrency.Event.cppm`** — This file already has 445 lines and 7 types (IEvent, ISignal, Signal<...>, Signal<T>, NeverEvent, PulseEvent, BroadcastEvent). Adding JoinSignal (another ~80 lines) makes it the second-largest class in the file and violates the existing modularity pattern.

### Blind spots

1. **`getAllEventsTuple_()` fold expression is slightly off** — The code uses a fold over `static_iota`, but the lambda captures `auto... event_index` and then uses a comma-fold inside: `((std::get<event_index>(result) = std::addressof(...)), ...)`. This works but is unnecessary: `static_iota` already expands the lambda for each index, so a simple `std::get` call inside the lambda body (without inner fold) would suffice:
   ```cpp
   static_iota<sizeof...(EventsT)>([&](auto... idx) noexcept {
       ((std::get<idx>(result) = std::addressof(std::get<idx>(m_events))), ...);
   });
   ```
   Actually this is fine — the inner fold is the standard pattern for expanding parameter packs inside a lambda. It matches the existing `getOptionalEvent_` pattern in `Signal`.

2. **No `NeverEvent` test** — The rationale mentions `NeverEvent` makes `join()` block forever, but there's no test verifying this behavior. Adding a test with timeout would document the expected semantic.

3. **`reset()` doesn't drain the semaphore** — After `reset()`, if a stale semaphore token exists from a previous completion, the semaphore count could be positive. A subsequent `wait()` would acquire immediately without checking `m_pending`. The `wait()` loop does re-check, so this is benign (spurious wakeup handled), but the proposal doesn't acknowledge this.

4. **Missing `BroadcastEvent` test** — The rationale dismisses this ("same IEvent contract"), but `BroadcastEvent` has different subscription/unsubscription behavior (mutex-locked, StableVector) and could have distinct interleaving hazards with `join()`.

## Review of Proposal C — Fresh Perspective

### Strengths

1. **`Join` implements `IEvent` — enables full event algebra** — `select(join(a, b), join(c, d))` works naturally. This is the only proposal that enables event algebra. In a game engine, this enables powerful patterns: "(animation AND audio) OR (timeout)".
2. **Cancel event parameter** — Practical for real-world use cases where you want to abort a join (e.g., user disconnects while waiting for resources). Validated by Go's context pattern.
3. **`DynamicJoin`** — Handles the important use case of runtime-variable event count (e.g., loading variable-length asset lists). A and B are constrained to compile-time event counts.
4. **Iterator with repeating-barrier semantics** — `wait → poll → reset → wait...` matches the mental model of "wait for all, process, repeat". Consistent with `Signal`'s iterator pattern.
5. **CAS in `poll()`** — Correct atomic consumption.

### Weaknesses

1. **Dual inheritance (`IEvent` + `ISignal`) is architecturally complex** — `Join` is both a signal receiver (from constituents) and an event emitter (to its parent). The private `ISignal` base is unusual and creates confusion about the object's role. A `Signal` subscribing to a `Join` triggers O(N) subscriptions (Join subscribes to all its constituents). This nested subscription chain is fragile.
2. **Synchronous notification propagation** — When the last constituent fires, `Join::notify()` calls the parent's `notify()` synchronously within the constituent's `emitEvent()` call stack. This creates deep, potentially unbounded synchronous call stacks with nested `select(join(...), join(...))` patterns. In a game engine where `emitEvent()` might be called from a render thread or network callback, this could introduce latency spikes.
3. **`cancel` overload inconsistency** — The free function `join(cancel, events...)` puts cancel first, but the constructor `Join(events..., cancel)` puts it last. Users switching between `join(a, b, cancel)` and `Join(a, b, cancel)` would encounter a compilation error for the former and the wrong overload resolution for the latter.
4. **`get()` method is non-standard** — `Signal` has no `get()` method. Introducing a blocking wait-and-consume with a different name from the existing API (`wait()` + `poll()`) creates API surface inconsistency. Users familiar with `select()` would expect `auto result = join(a, b).poll();` not `join(a, b).get();`.
5. **`DynamicJoin` uses `std::unique_ptr<JoinSlot[]>`** — This violates the engine's allocator conventions documented in AGENTS.md. Should use `mem::GPA` or `mem::Allocator<T>`. The raw `new[]/delete[]` usage is inconsistent with the rest of the codebase.
6. **Module file bloat** — All code (Join, DynamicJoin, iterator, +3 tests) goes into `Core.Concurrency.Event.cppm`, making a 445-line file significantly larger. This conflicts with the separation pattern used for other concerns.
7. **No CTAD guide shown** — The proposal defines `Join<EventsT...>` but doesn't show a CTAD deduction guide. Without it, `join(a, b)` won't deduce the template parameters from the constructor arguments.

### Blind spots

1. **Iterator `wait() ÷ poll()` race** — The iterator calls `wait()` (which returns when `m_pending` *was* `kAllMask`), then calls `poll()` (which CAS to claim). Between these calls, another thread could claim via `poll()`, making the iterator's `poll()` return `nullopt`. Then the iterator is stuck: `wait()` already returned, but `poll()` got nothing. Re-entering `wait()` would block forever (no new notifications). This is a **correctness bug** in the iterator design. Fixing it requires either: (a) embedding the CAS inside `wait()`, (b) looping `poll()` in a retry, or (c) documenting that `Join` is single-consumer only.

2. **`pollEvent()` vs `poll()` mismatch** — `pollEvent()` (IEvent contract) checks `m_pending == all_mask` non-destructively. `poll()` (public API) consumes via CAS. If `pollEvent()` returns true but `poll()` fails to CAS, `pollEvent()` returns false on subsequent calls. The `IEvent` contract tests (e.g., `Signal<EventT>::wait()` which loops on `pollEvent()`) might behave unexpectedly if a `Join` is used as a single constituent of a `select()`. Specifically, `Signal<EventT>::wait()` loops `while (not m_event.pollEvent()) { m_semaphore.acquire(); }`. If a `Join` completes, `pollEvent()` returns true, the `Signal`'s `wait()` returns, then `poll()` CAS fails (another thread grabbed it), the user can proceed with a stale result.

3. **`DynamicJoin` counter over-count hazard with `BroadcastEvent`** — If a `BroadcastEvent` fires, it notifies *all* subscribers. If `DynamicJoin` subscribes via `JoinSlot`, the slot receives exactly one `notify()` per `emitEvent()` (first call sets the atomic_flag, subsequent calls are no-ops). So the counter correctly counts to `N`. But if the user explicitly calls `resetEvent()` on a constituent while `DynamicJoin` is pending, it could re-arm the event and cause another `notify()` after reset, incrementing the counter past `N`. This is a documented hazard but the proposal doesn't mention it.

4. **`Join` as an `IEvent` means it can be subscribed to multiple times** — Unlike `PulseEvent` (single-subscriber atomically swapped), `Join` stores one parent. If subscribed a second time, `subscribeEvent` overwrites the old parent reference. The old parent loses future notifications. The proposal doesn't address or guard against this.

5. **No `alignas(hal::cacheline_size_v)`** — `Join` is not cache-line aligned (unlike `Signal`). `m_pending` (high-traffic atomic) shares a cache line with `m_events` and `m_cancel`, causing false sharing.

## Synthesis Recommendations

### Adopt from A

1. **CAS-based `poll()`** — Mandatory. Without it, concurrent `poll()` is a data race. Use `compare_exchange_weak` on `m_pending` with a `kAllMask` → 0 transition, exactly as Proposal A shows.
2. **Separate module partition** — Keep `Core.Concurrency.Join.cppm` separate. `Core.Concurrency.Event.cppm` already has 7 types and 445 lines. Adding `JoinedSignal` (~80 lines), iterator (~40 lines), and test registration would push it past maintainability. The partition boundary communicates "this is a different composition primitive."
3. **Compile-time `kAllMask`** — The `constexpr (1 << N) - 1` pattern is correct (with the UB fix noted below) and consistent with the zero-cost philosophy.
4. **Comprehensive test coverage** — Include the edge cases from A's test table: `join_with_never_never_fires`, `join_reset_resets_all`, `join_wait_poll_cycle`, `join_three_events`. These are the tests most likely to catch regressions.
5. **NeverEvent documentation** — The explicit "`join(never, ...)` never fires by design" language must be in both code comments and AGENTS.md.

### Adopt from B

1. **Low-friction integration** — Two files (`.cppm` + `.cpp`) is reasonable. No changes to `Core.Concurrency.Event.cppm` or its `.cpp`. Only `Core.cppm` and `CMakeLists.txt` need edits. This is the right balance of isolation vs. overhead.
2. **`reset()` clears all events** — The all-or-nothing `reset()` matches the all-or-nothing trigger semantic. No per-event selectors needed.
3. **Test `reset_then_repoll_needs_all`** — This is an important edge case (reset only one constituent, then check join is still broken). Proposal B's test for this is good.
4. **The "same lock-free primitives, different condition" philosophy** — Keep the implementation simple. Don't introduce new concurrency primitives. The `fetch_or` + `release()` pattern is battle-tested in `Signal`.

### Adopt from C

1. **Iterator with repeating-barrier semantics** — But fix the `wait()` ÷ `poll()` race. The iterator should use a fused `waitAndPoll()` that atomically claims the result upon wakeup, or a `try_poll` loop after `wait()` returns. The repeating-barrier pattern (`wait for all → process → reset → repeat`) is valuable for game loops and matches `Signal`'s iterator API.
2. **Cancel event as a *separate* template parameter** — Not the inconsistent overload in C, but a first-class template parameter:
   ```cpp
   template<typename... EventsT>
   class JoinedSignal;  // no cancel

   template<typename CancelT, typename... EventsT>
       requires std::is_base_of_v<IEvent, CancelT>
   class JoinedSignal<CancelT, EventsT...>;  // partial specialization with cancel
   ```
   This avoids overload ambiguity and makes cancel detectable at compile time.
3. **The composition vision** — Event algebra (`select(join(a, b), c)`) is the right long-term goal. But it should be implemented incrementally: land `JoinedSignal` as `ISignal` first (A's approach), then evolve `Join` into an `IEvent`-implementing composable in a follow-up when the nesting use cases are concretely demonstrated. Premature composition syntax adds risk without proven demand.

### Final recommendation

**Merge A's structure + A's CAS poll + B's low-friction integration + C's iterator (with race fix) + C's cancel (as a partial specialization).**

- Use A's separate partition for clean modularity.
- Use A's `fetch_or` notify + CAS poll (correct lock-free design).
- Use B's "insertions only, no changes to existing logic" approach within the new partition.
- Add C's repeating-barrier iterator, but fix the `wait()/poll()` race by embedding the CAS inside the claim.
- Add cancel as a partial specialization or tag-parameter, not as a positional overload.
- Fix the `kAllMask` undefined behavior for `N == bit_count_v`.
- Add cache-line alignment to prevent false sharing.
- Add a specific test for concurrent `poll()` from two threads.

**Risk rating:** Low-to-medium for the core (A-based). Medium for the iterator (needs race fix). Low for cancel (if deferred to follow-up).
