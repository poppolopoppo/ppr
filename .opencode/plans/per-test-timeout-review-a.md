# Cross-Review: Proposals A, B, C — Per-Test Timeout Facility

**Reviewer:** A
**Date:** 2026-06-21

---

## Executive Summary

Each proposal takes a fundamentally different trade-off axis:

| Axis | A (Aggressive) | B (Conservative) | C (Fresh) |
|------|---------------|-----------------|-----------|
| Signal fix approach | Handoff in `poll()` | `try_acquire()` after CAS | Replace semaphore w/ `atomic::wait` |
| Timer backend | Per-platform OS APIs | Per-platform OS APIs | `std::jthread` (cross-platform) |
| Timeout enforcement | `terminateProcess` in-process | `terminateProcess` in-process | Auto-fork + child self-kill |
| `spawnAndWait` changes | Yes (timeout overload) | Yes (timeout overload) | None |
| Platform timer files | 4 new files | 4 new files | 1 new file |
| Risk profile | Medium-high | Low-medium | Medium |

---

## Proposal A — Strengths

1. **Handoff in `poll()` (Phase 5):** The analysis of the lost-wakeup bug is the most thorough of all three proposals. The handoff (lines 514–521) is a minimal, correct patch for the multi-event case. It avoids replacing the entire synchronization primitive.

2. **`m_fired` atomic in `Deadline` (line 27):** Embedding an atomic flag in the deadline struct provides a clear, portable way to detect whether the callback fired, without shared_ptr or ref-counting overhead.

3. **Environment variable support (lines 431–434):** `PPR_TEST_TIMEOUT` env var is useful for CI without command-line changes. Neither B nor C includes this.

4. **`[[noreturn]]` on `terminateProcess` (lines 201–208):** Correctly annotated. The infinite spin loop after `TerminateProcess` (line 223) is a good belt-and-suspenders for MSVC's `__declspec(noreturn)`.

---

## Proposal A — Weaknesses

### 🔴 CRITICAL: Raw function pointer signature vs. capturing lambda (lines 34 vs. 343)

This is a **fatal design contradiction**. The API declares:

```cpp
[[nodiscard]] Deadline setDeadline(u64 ms, void(*callback)(Deadline*) noexcept) noexcept(false);
//                                          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//                                          raw function pointer, no captures
```

But Phase 3 Step 2 (line 343) passes a **capturing lambda**:

```cpp
m_deadline = hal::timer::setDeadline(*m_context.m_timeout, [this, path](hal::timer::Deadline *dl) noexcept { ... });
//                                                          ^^^^^^^^^^^^^^^^
//                                                          captures this and path — does not decay to raw fn ptr
```

A capturing lambda cannot decay to a `void(*)(Deadline*)`. This code will not compile. The proposal needs either `std::function`/`move_only_function`, a `void*` context parameter (like B's approach), or a different mechanism to pass `RunImpl*` to the callback.

### 🔴 User callback is never invoked (Windows backend, lines 63–80)

The Windows `TimerCallback_` hardcodes `m_fired.store(true)` and ignores the user-provided `callback` parameter entirely. The `Deadline` struct has no field to store the callback. So the per-test diagnostic and `terminateProcess` call from Phase 3 Step 2's lambda will **never execute** when using the Windows timer backend. The timer effectively does nothing except set a flag that is never checked (the `m_timed_out` check in Phase 3 Step 4 is unreachable because `terminateProcess` was supposed to have been called).

This is a gap between the API design and the implementation — the Windows backend doesn't store or invoke the callback it receives.

### 🔴 `ualarm` is not portable (lines 286, 291)

`ualarm()` is a legacy BSD function. It exists on Linux but:
- Not available on Darwin/macOS (since macOS 10.12 it's been deprecated, and the implementation may be incomplete)
- Not in POSIX.1-2008 (withdrawn)
- Interferes with `alarm()`/`setitimer()` — if any other code uses these, behavior is undefined

The POSIX `spawnAndWait` timeout (Phase 2 Step 4) should use a different approach (timerfd on Linux, kqueue on Darwin, or the polling loop from Proposal B).

### Signal fix: correct for multi-event, wrong for single-event (lines 570–593)

The handoff fix for the multi-event `Signal<EventsT...>` is correct. But the analysis of the single-event `Signal<EventT>` (lines 542–604) goes through several wrong proposals before concluding "the single-event version is actually fine!" — and then still includes a broken `m_semaphore.release()` in the fix block (lines 576–578) before dismissing it. This is confusing and suggests the analysis wasn't fully resolved at time of writing.

In fact, the single-event `Signal<EventT>` **does** have the same bug: `notify()` calls `m_semaphore.release()`, `poll()` checks `m_event.pollEvent()` non-destructively, but `poll()` never consumes the semaphore token. After N `poll()` calls there are N excess permits. The claim at line 603–604 that "there's no race because pollEvent() is non-destructive" is incorrect — the non-destructive read means the *event* remains signaled, but the *semaphore* accumulates surplus permits, causing `wait()` to spuriously wake when nothing is pending.

### `WT_EXECUTEINTIMERTHREAD | WT_EXECUTEINPERSISTENTTHREAD` (line 76)

This flag combination creates a persistent thread in the thread pool that never recycles. For an occasional per-test timer this is wasteful. The flags are designed for high-frequency periodic timers. `WT_EXECUTEDEFAULT` (as used by Proposal B) is more appropriate.

### `m_deadline` declared with complex type in header (line 322)

```cpp
std::optional<decltype(hal::timer::setDeadline(0, nullptr))> m_deadline{};
```

This is fragile: it depends on the return type of `setDeadline`, which is platform-specific. If the return type ever changes, this line silently adjusts. Worse, the initializer `(0, nullptr)` is evaluated for `decltype` but passing `nullptr` as a function pointer where `void(*)(Deadline*)` is expected... won't compile. `nullptr` can't convert to a function pointer type in this context. Actually `nullptr` can convert to any pointer type including function pointers (`nullptr_t` → `void(*)(Deadline*)`), so it does compile, but it's obscure.

More importantly, `decltype(setDeadline(...))` returns a `Deadline` by value. So `m_deadline` is `std::optional<Deadline>`. But the `WindowsTimer` struct is heap-allocated inside `setDeadline` and stored via `Deadline::m_handle`. If `Deadline` is moved (into the optional), the handle still points to the heap allocation. This is fine for move, but the code doesn't define move semantics for `Deadline` explicitly — it relies on implicit memberwise move, which would copy the `atomic<bool>` (atomic is not movable). `std::atomic` has deleted copy/move constructors, so this won't compile.

### `cancelDeadline` blocks waiting for completion event (line 98)

On Windows, if the callback fires and sets `m_fired` before `WaitForSingleObject` is called, we wait forever on an event that was never set. The `m_completion` event is created but never signaled in any code path — it's `CreateEventW(nullptr, TRUE, FALSE, nullptr)` (manual-reset, initially non-signaled) and `TimerCallback_` never calls `SetEvent`. So if `m_fired` is true (callback already ran), we skip the wait (good). But if the callback fired AND terminated the process, `WaitForSingleObject` is never reached. If the race goes the other way (the callback fires between the `m_fired` check and the `WaitForSingleObject` call), the callback runs, sets `m_fired`, calls `terminateProcess` — and the process dies, so no hang. Actually this is safe because `terminateProcess` kills the process. But if `terminateProcess` were not `[[noreturn]]` (e.g., on a platform where it couldn't be), this would be a deadlock. The `m_completion` event is dead code and should be removed.

---

## Proposal B — Strengths

1. **`try_acquire()` Signal fix (Phase 1):** The simplest possible fix. One line, well-understood, non-blocking. Risk is very low.

2. **`void(*)(void*)` callback signature (line 55):** Matches Windows `WAITORTIMERCALLBACK` directly, no trampoline needed. Works with stateless lambdas or function pointers. Avoids all the capture issues of Proposal A.

3. **Explicit failure mode analysis per step:** Every Phase includes a "Failure mode analysis" section. This is extremely valuable for reviewing the correctness of the design.

4. **Synchronous cancel on Windows (Phase 3 Step 1):** `DeleteTimerQueueTimer(..., INVALID_HANDLE_VALUE)` blocks until all pending callbacks complete. This eliminates the fire-during-cancel race entirely on Windows.

5. **Polling loop for POSIX `spawnAndWait` (Phase 5 Step 3):** No signal handlers needed. Works uniformly on Linux and Darwin. Correctly handles `EINTR` and `ECHILD`.

6. **Callback doesn't capture `RunImpl*` (Phase 7 Step 2):** Using only globals (`std::cerr`, `terminateProcess`) eliminates use-after-free races entirely. The POSIX race is acknowledged and accepted.

7. **Edge cases explicitly documented (Section "Edge cases and ambiguities noted", lines 468–494):** Covers nested timeouts, zero timeout, callback after static destruction, and generic platform behavior. This shows careful thought.

---

## Proposal B — Weaknesses

### ⚠️ Uses `exit()` instead of `_exit()` on POSIX (Phase 4 Step 2, line 200)

The proposal acknowledges this is wrong ("if the test deadlocked, `exit()` may deadlock too if a mutex is held") but recommends `exit()` anyway because "if it hangs, the OS will eventually reap the process." This is contradictory:

- The entire purpose of the timeout facility is to **recover** from deadlocks and hangs. Using `exit()` (which calls `atexit` handlers, static destructors, flushes stdio buffers) can re-enter the same deadlock that triggered the timeout.
- `_exit()` skips all of this and terminates immediately. It's the correct choice for a deadlock-recovery mechanism.
- The only argument for `exit()` is coverage flushing — but coverage is meaningless if the process deadlocks trying to flush it.

The proposal says "Recommendation: use `::exit(exitCode)`" but this recommendation is incorrect for the stated use case.

### ⚠️ `timer_create(SIGEV_THREAD)` known quirk not addressed (Phase 3 Step 2)

On Linux, `timer_create(CLOCK_MONOTONIC, SIGEV_THREAD, ...)` has a documented issue: if the kernel cannot create a new thread (e.g., resource exhaustion), the notification **silently falls back to `SIGEV_SIGNAL`**, which delivers `SIGALRM` to the process. If the process has no handler for `SIGALRM`, the default action is termination — which would kill the test runner without diagnostic. This should at least be documented and possibly handled (install a no-op `SIGALRM` handler before creating timers).

The proposal accepts a 24-byte leak on cancel (which is fine) but doesn't mention this signal fallback behavior, which is a correctness hazard.

### ⚠️ Callback doesn't print test path or timeout duration (Phase 7 Step 2, line 362)

```cpp
[](void *) noexcept {
    std::cerr << "[TIMEOUT] Test exceeded deadline, terminating\n";
    hal::process::terminateProcess(124);
}
```

The diagnostic doesn't include which test timed out or what the timeout duration was. For a test suite with hundreds of tests, a bare "[TIMEOUT]" message is nearly useless for debugging. Since the callback intentionally avoids capturing `RunImpl*` (for safety), there's no way to access the test path — but the proposal doesn't address passing this information via the `void*` context parameter, which is the whole reason `void*` context exists.

### ⚠️ `spawnAndWait` timeout throws exception (Phase 5 Step 2)

The timeout overload throws `std::runtime_error`. But the fork path in `UnitTest::run()` uses `startInChildProcess_()` which returns a `bool`. The exception must be caught in `UnitTest::run()`. The proposal should verify the catch site exists and handles this correctly. In Proposal A's equivalent, the timeout returns `-1` instead of throwing, which is more consistent with the non-throwing return-value pattern.

### Polling loop granularity (Phase 5 Step 3, line 279)

50ms sleep per iteration is acceptable for timeout purposes, but:
- Wastes CPU (wake every 50ms even for a 30-second timeout = 600 wakeups)
- No mention of using `sigtimedwait` or `ppoll` as alternatives that block without polling
- No mention of `pidfd_open` (Linux 5.3+) as a more efficient approach

---

## Proposal C — Strengths

1. **`atomic::wait`/`notify_one` Signal fix (Phase 1):** This is the most elegant fix. It eliminates the semaphore token-mismatch problem at the root rather than patching around it. No handoff, no `try_acquire`, no edge cases to reason about. The design is correct by construction.

2. **Single cross-platform timer file (Phase 2):** `std::jthread` works on all three target platforms. Zero platform-specific code means zero platform-specific bugs. For a milliseconds-granularity deadline whose sole consumer is test timeouts, the overhead is negligible.

3. **RAII `Deadline` class (Phase 2, lines 145–156):** Clean ownership semantics. Destruction→join→auto-cancel. No manual `cancelDeadline` calls needed (though `~RunImpl` still destroys the optional, which triggers the join). Much harder to misuse than a `void*` handle.

4. **Auto-fork on timeout (Phase 3 Step 4):** This is the most important design insight in all three proposals. Instead of trying to kill the parent process safely (which requires `TerminateProcess`/`_exit` and all their platform-specific concerns), just let the child kill itself. The parent never needs to do anything dangerous.

5. **No `spawnAndWait` changes:** Fewer API changes, less risk of breaking existing code. The timeout is purely a CLI/child-process concern.

6. **`_Exit()` not `exit()` (Phase 3 Step 3, line 297):** Correctly uses `_Exit()` which skips static destructors and is safe to call from a signal handler or timer thread.

7. **`std::move_only_function` (Phase 2, line 148):** Modern, type-erased, no-alloc callable. Fits the use case well.

---

## Proposal C — Weaknesses

### ⚠️ Auto-fork changes test execution semantics (Phase 3 Step 4, lines 326–340)

Enabling `fork` mode for ALL timed-out tests is a behavioral change that may surprise users:

- **Windows has no `fork()`:** The fork path uses `spawnAndWait` which creates a child process via `CreateProcess`. This works but is significantly more expensive (process creation) and behaves differently (no shared memory, different PID, etc.).
- **Tests that disable fork explicitly:** The check `if ((effective_flags & fork) == none)` is correct, but auto-overriding user intent is questionable. A user who explicitly removed the `fork` flag presumably had a reason.
- **Non-fork tests with timeouts behave fundamentally differently:** A test that normally runs in-process (and thus has access to all process state, file descriptors, etc.) suddenly runs in a child process. This could mask or introduce bugs.
- **Debugging:** A forked test that times out loses the ability to attach a debugger to the hung process.

The proposal should at least log a warning when auto-fork is triggered, or provide a way to opt out.

### ⚠️ Thread-per-timer overhead (Phase 2, lines 183–190)

`std::jthread` creates a new OS thread for each deadline. With serial execution (one test at a time), there's at most one extra thread. But:
- If tests run in parallel in the future, each would get its own thread
- Thread creation overhead (~8µs for the thread, plus stack allocation) is incurred even for tests that finish normally and don't time out
- For a test suite with thousands of tests, this is thousands of thread create/join cycles

This is acceptable for now, but the proposal should note that `std::jthread` is a placeholder that could be replaced by a platform-specific timer if the overhead becomes an issue.

### ⚠️ `atomic::wait`/`notify_one` requirements

`std::atomic::wait()` / `notify_one()` were added in C++20 and are widely available, but:
- On Linux they map to `futex` (fine)
- On Windows they map to `WaitOnAddress` which requires **Windows 8+** (NT 6.2). The project's minimum supported Windows version should be documented.
- On older MSVC toolchains, `atomic::wait` may not be available or may have bugs. The proposal should verify that the MSVC version in the toolchain (VS 2026) supports it fully.

### ⚠️ No cross-process timeout for the parent (Phase 3 Step 4)

By relying entirely on the child process's self-kill via `_Exit()`, the parent `spawnAndWait` has no timeout of its own. If the child process hangs and the deadline thread doesn't fire (thread starvation, scheduler issues), the parent waits forever. The CMake-level timeout (reduced from 120s to 30s in Phase 4 Step 3) provides a safety net, but that kills the entire test process, not just the hung test.

Proposals A and B add a `spawnAndWait` timeout overload that provides this safety net at the HAL level. Proposal C should additionally add a watchdog to `spawnAndWait` when running in fork mode.

### ⚠️ `atomic::wait` for single-signal — additional state needed (Phase 1, lines 104–122)

The multi-signal case maps cleanly: `m_pending` (already a `std::atomic<size_t>`) doubles as the wait variable. But the single-signal case needs a **new** `m_notified` atomic member. This is additional state per signal that wasn't needed before. The proposal acknowledges this but doesn't analyze the memory overhead (one atomic per `Signal<EventT>`).

### ⚠️ Single-signal `notify()` and `wait()` correctness (lines 109–121)

```cpp
void wait() noexcept override {
    while (not m_event.pollEvent()) {
        m_notified.wait(false, std::memory_order_relaxed);
        m_notified.store(false, std::memory_order_relaxed);
    }
}
```

If `notify()` is called while `wait()` is between `pollEvent()` (returned false) and `m_notified.wait(false, ...)`:
1. `notify()`: `m_notified.store(true, release); m_notified.notify_one();`
2. `wait()`: `m_notified.wait(false, ...)` — **misses the notification** because `m_notified` is already `true`, so `wait()` with `false` blocks

Actually, no — `wait(false)` checks `m_notified.load() == false` before blocking. If `m_notified` is `true`, `wait()` returns immediately. So step 2 sees `m_notified == true` and returns immediately. Then `store(false)` resets it. Then the while loop re-checks `pollEvent()`.

But there's a race in the reset: if `notify()` is called between `wait()` returning and `store(false)`, the notification is lost. The next `pollEvent()` hasn't been called yet (the `store(false)` happens inside the `while` body, then the loop re-evaluates `pollEvent()` which may now return false because the event was consumed by `poll()` in between).

This is exactly the same semaphore-mismatch bug, reproduced with a different primitive. The problem is not the waiting mechanism — it's that `poll()` consumes the event without coordinating with `wait()`. The `atomic::wait` approach doesn't fix this on its own; it still needs some form of handoff or tracking of consumers vs. notifiers.

In the multi-signal case, this doesn't happen because `poll()` atomically clears a specific bit, and the bit can be re-checked. In the single-signal case, `poll()` doesn't atomically consume — it just reads. The fix for the single-signal case is more subtle and Proposal C doesn't fully address it.

---

## What to adopt from each proposal

### From Proposal A:
1. **The handoff fix for multi-event `Signal`** (Phase 5, lines 514–521). It's correct, minimal, and doesn't change the synchronization primitive. Should be combined with B's `try_acquire()` for symmetry.
2. **Environment variable `PPR_TEST_TIMEOUT`** (Phase 4, lines 431–434). Useful for CI. Neither B nor C includes this.
3. **`terminateProcess` is `[[noreturn]]` with `_exit()` on POSIX** (Phase 2, line 249). Correct choice for deadlock recovery.
4. **Return value from `cancelDeadline`** (line 40). B's cancel is `void`; returning `bool` (did we cancel before it fired?) is more informative.

**But do NOT adopt:** the raw function pointer signature with capturing lambda (lines 34 vs. 343), the `ualarm` usage (lines 286/291), the `WT_EXECUTEINTIMERTHREAD` flags (line 76), `decltype(setDeadline(0, nullptr))` (line 322).

### From Proposal B:
1. **`try_acquire()` Signal fix** (Phase 1, line 24). Simplest possible fix. Can complement A's handoff approach.
2. **Explicit failure mode analysis per step.** This should be a standard part of every design proposal.
3. **Synchronous cancel on Windows** (`DeleteTimerQueueTimer(..., INVALID_HANDLE_VALUE)`, Phase 3 Step 1). Eliminates fire-during-cancel race.
4. **Callback with `void*` context** (line 55). Correct way to pass state to a C-style callback without `std::function` overhead.
5. **Polling loop for POSIX `spawnAndWait`** (Phase 5 Step 3). Portable, no signal handlers needed.
6. **Edge case documentation** (lines 468–494). Nested timeouts, zero timeout, static destruction safety — all three proposals should document these.
7. **Graceful fallback on timer creation failure** (Phase 7, lines 366–369). Catching the exception and running without timeout protection. Better than crashing.

**But do NOT adopt:** `exit()` instead of `_exit()` on POSIX (Phase 4, line 200). `_exit()` is the correct choice.

### From Proposal C:
1. **`atomic::wait`/`notify_one` for multi-signal `Signal`** (Phase 1). The most elegant Signal fix. Eliminates the semaphore token-mismatch entirely.
2. **Auto-fork on timeout** (Phase 3 Step 4). The key insight: let the child kill itself. This eliminates most of the platform-specific complexity around `terminateProcess`.
3. **`std::jthread`-based `Deadline`** (Phase 2). Single cross-platform file, RAII semantics, no platform-specific timer code. Acceptable overhead for the test framework use case.
4. **`_Exit()` not `exit()`** (Phase 3 Step 3, line 297). Correct for timeout termination.
5. **CLI propagation of timeout** (Phase 3 Step 2). Avoiding `spawnAndWait` modifications is cleaner.
6. **`std::move_only_function`** (Phase 2, line 148). Modern, type-safe callback. Better than raw function pointer + `void*` for this use case.

**But do NOT adopt:** the single-signal `atomic::wait` solution as written (Phase 1, lines 109–121) — it has a race; the `m_notified` reset loses notifications. The multi-signal solution is clean; the single-signal case needs more thought.

---

## Recommended hybrid approach

Combine the key ideas:

1. **Signal fix:** Use `atomic::wait`/`notify_one` for the multi-signal `Signal` (from C). For the single-signal `Signal`, apply B's `try_acquire()` fix (simpler and correct for the single-bit case).

2. **Timer backend:** Single cross-platform `std::jthread`-based `Deadline` (from C). No per-platform files. RAII semantics.

3. **Timeout strategy:** Auto-fork on timeout (from C — child self-kill via `_Exit()`). **Plus** add a `spawnAndWait` timeout overload for the parent watchdog (from A/B — belt and suspenders). The parent's timeout should be slightly longer than the test's deadline to avoid false positives.

4. **CLI:** `--timeout <ms>` with env var `PPR_TEST_TIMEOUT` fallback (from A). Propagate to child via CLI args (from C).

5. **terminateProcess:** `[[noreturn]]` with `_exit()` on POSIX (from A). Keep it for non-fork mode.

6. **Callback:** `std::move_only_function<void() noexcept>` (from C). Captures test path and RunImpl* safely via `jthread` join semantics.

7. **Edge cases:** Document all from B's edge-case list (lines 468–494). Add warnings for auto-fork.

---

## Comparison: How each proposal handles the Signal fix

| Criterion | A (Handoff) | B (try_acquire) | C (atomic::wait) |
|-----------|-------------|-----------------|-------------------|
| Lines changed | ~15 (multi) + analysis | 1 line | ~15 (multi) + new member for single |
| Correctness | ✅ Multi; ❌ Single (confused analysis) | ✅ Both (simple) | ✅ Multi; ❌ Single (race in reset) |
| Performance | 1 extra branch + rare `release()` | 1 `try_acquire()` per `poll()` | Replaces semaphore ops with futex/WA |
| Risk | Medium | Very low | Medium (new primitive dependency) |

**Recommendation:** B's `try_acquire()` for the single-signal case; either A's handoff or C's `atomic::wait` for the multi-signal case. C's approach is architecturally cleaner for multi-signal.

---

## Comparison: How each proposal handles timer ownership

| Criterion | A (Opaque handle) | B (Opaque handle) | C (RAII Deadline) |
|-----------|-------------------|-------------------|-------------------|
| Ownership model | Manual (cancelDeadline in dtor) | Manual (cancelDeadline in dtor) | Automatic (jthread join in dtor) |
| Move semantics | Implicit (broken — atomic not movable) | Trivial (void* copy) | Explicit (deleted copy, default move) |
| Resource leak risk | Medium (heap-alloc WindowsTimer) | Low (void* handle) | Low (jthread stack-owned) |
| Platform complexity | 4 files | 4 files | 1 file |

**Recommendation:** C's RAII approach is cleaner. If per-platform timers are needed later, the RAII wrapper can be retrofitted with a platform-specific backend.

---

## Comparison: How each proposal handles the SIGALRM/Signal issue

| Criterion | A | B | C |
|-----------|---|---|---|
| Mechanism | `ualarm` interrupts `waitpid` | Polling loop with `WNOHANG` | Child self-kill via `_Exit()` (no parent signal needed) |
| Signal required | `SIGALRM` handler | None | None |
| Portability | ❌ `ualarm` not on macOS | ✅ Pure POSIX | ✅ C++20 standard |
| Parent safety | ❌ Parent receives SIGALRM | ✅ Parent never interrupted | ✅ Parent never interrupted |
| Precision | Microsecond | 50ms | Depends on jthread scheduling |

**Recommendation:** C's approach is the cleanest (no parent signal handling needed). Use B's polling loop as a fallback for the parent-side watchdog.

---

## Summary of cross-cutting issues

| Issue | A | B | C |
|-------|---|---|---|
| Signal fix correctness (multi) | ✅ | ✅ | ✅ |
| Signal fix correctness (single) | ❌ | ✅ | ❌ |
| Timer callback type | ❌ Mismatch | ✅ void(void*) | ✅ move_only_function |
| Timer callback capture safety | ❌ this captured | ✅ globals only | ✅ jthread join guarantees |
| Portability (macOS) | ❌ ualarm | ✅ Polling | ✅ jthread |
| Parent watchdog timeout | ❌ None in fork path | ❌ None in fork path | ❌ None (relies on CMake) |
| Diagnostic includes test path | ✅ | ❌ No | ✅ |
| Env var support | ✅ | ❌ | ❌ |
| Edge case documentation | Partial | ✅ Complete | Partial |
| Implementation risk | Medium-high | Low-medium | Medium |
