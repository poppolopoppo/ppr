# Cross-Review: Per-Test Timeout Proposals A, B, C

## Context

All three proposals address two orthogonal concerns:
- **Signal bug**: `poll()` does CAS (`compare_exchange_weak`) to clear a bit from `m_pending`, while `notify()` does `fetch_or` + `semaphore.release()`. The race: if the semaphore count is consumed by `wait()` at exactly the wrong moment between CAS retries, a wakeup is lost (`wait()` blocks forever even though a bit is set).
- **Per-test timeout**: killing a test that runs too long (infinite loop, deadlock).

---

## Proposal A — Aggressive

### Strengths
- **Race-free `Deadline`**: `std::atomic<bool> m_fired` ensures cancel and fire are race-safe. The callback sets `m_fired` under the atomic, and `cancelDeadline` reads it to decide success/failure.
- **`cancelDeadline` returns `bool`**: gives the caller deterministic knowledge of whether the callback ran — valuable for resource cleanup ordering.
- **Completion-based `spawnAndWait`**: `WaitForSingleObject(ms)` on Windows is efficient (single syscall, no polling). POSIX `ualarm` + `waitpid` + `SIGKILL` is a well-understood pattern.
- **`[[noreturn]] terminateProcess` with `TerminateProcess`/`_exit`**: correct choice for process-level kill where atexit handlers should NOT run (avoids deadlocks in dying process).
- **Unit tests for timer + timeout integration**: essential for confidence in cross-platform timer behavior.

### Weaknesses
- **Signal fix via "handoff" in `poll()`**: vaguely described. "`poll()` releases semaphore when clearing the last pending bit" — this adds complexity inside the already-tricky CAS loop and modifies the `Signal` concurrency model. Not clearly scoped and risks introducing new races.
- **Callback is `void(*)(Deadline*)` — stateless function pointer**: limits flexibility. Can't capture context without globals. Forces the callback to extract `RunImpl*` from somewhere else (probably via a global or `this` embedded in the `Deadline` struct itself).
- **`WT_EXECUTEINTIMERTHREAD | WT_EXECUTEINPERSISTENTTHREAD` on Windows**: The persistent thread flag keeps a thread alive for the timer queue. On a test runner with potentially many timeouts, this is fine, but the `INTIMERTHREAD` flag means the callback runs on the timer thread, which is highly constrained (no blocking calls, no stack-heavy operations). The callback sets a flag and logs — this is fine, but a future developer might accidentally add blocking work.
- **`timer_create(SIGEV_THREAD)` on POSIX**: This spawns a new thread per timer (or reuses a process-wide helper thread depending on implementation). It's heavier than necessary and some POSIX systems limit the number of timers. Not portable across all POSIX targets.
- **Changes to `spawnAndWait` signature** (timeout overload): modifies an existing HAL API, which affects all three platforms. The generic/stub platform would need updates too.
- **Signal fix mixed with timeout work**: the proposal bundles two unrelated changes (Signal bugfix + timeout feature). This makes review, testing, and (if needed) revert more difficult.

### Blind Spots / Missing
- **Callback lifetime**: if `Deadline` fires after `cancelDeadline` but before the callback finishes executing, what guarantees that the `RunImpl*` pointer is still valid? The callback captures a raw `RunImpl*` — if `RunImpl` is stack-allocated (it is — inside `UnitTest::run`), the stack frame may be gone by the time the callback runs. The `m_fired` atomic only tells you whether fire *started*, not whether the callback *finished*.
- **Missing macOS timer implementation**: `timer_create` with `SIGEV_THREAD` exists on macOS, but `ualarm` does not (it's deprecated on Linux too). What's the `spawnAndWait` timeout mechanism on macOS?
- **`WT_EXECUTEINTIMERTHREAD`**: the callback must not throw, must not allocate, and must not block. The proposal doesn't mention constraints or `noexcept`.
- **No discussion of timer cleanup ordering** relative to `RunImpl` destruction. If `Deadline` is a member of `RunImpl`, the dtor must cancel before `RunImpl` members are destroyed — but the callback still needs them.
- **CLI arg `--timeout <ms>`**: where is this consumed? In `main.cpp`? Needs to propagate to `Context` and then to `RunImpl`. The proposal doesn't specify the plumbing.

### What to Adopt
- `cancelDeadline` returning `bool` for deterministic cancel-or-fired insight.
- Windows `WaitForSingleObject(ms)` for the completion-based wait (efficient).
- Unit tests for timer lifecycle and timeout integration.
- `[[noreturn]]` + `_exit` (not `exit`) for process termination — correct for child process.

---

## Proposal B — Conservative

### Strengths
- **Minimal Signal fix**: "`try_acquire()` after CAS in `poll()`" is a small, well-scoped change. It's the minimal fix for the semaphore starvation bug described in the Signal analysis.
- **Recognizes and documents known issues**: the 24-byte leak on POSIX `timer_create` cancel, the spurious timeout line race, the `exit()` vs `_exit()` tradeoff. Honest about tradeoffs.
- **No changes to `spawnAndWait` signature**: avoids touching existing HAL API contracts. The POSIX polling loop with `WNOHANG` + 50ms sleep is simple and doesn't require signal handlers.
- **Callback is raw function pointer + `void*` context**: more flexible than Proposal A's `Deadline*` callback — the caller can pass whatever they need.
- **`DeleteTimerQueueTimer(..., INVALID_HANDLE_VALUE)` blocks until callback completes**: safe cancellation on Windows. The callback is guaranteed done before cancel returns.
- **No new module files**: likely adds content to existing files only. Small diff.

### Weaknesses
- **Accepts real bugs for "simplicity"**: the POSIX 24-byte leak and spurious timeout line are accepted, not fixed. In a game engine with rigorous testing (ASAN, leak detection), a known memory leak — however small — will flag in CI. The "spurious timeout" race means flaky CI results, which erodes trust in the test suite.
- **`exit()` on POSIX** (runs atexit handlers): the proposal notes `_exit` would be safer but uses `exit`. If the test has acquired locks or allocated resources with atexit cleanup, `exit()` can deadlock (atexit mutex held by dying thread, etc.) or cause cascading failures in shared state.
- **Polling loop with `WNOHANG` + 50ms sleep**: wastes CPU on each poll (context switch every 50ms). For a 120s timeout this is 2400 wakeups. Not a problem for occasional use, but not elegant. More importantly, if the test produces output during the sleep, the parent doesn't forward it in real time.
- **`TimerHandle = void*` and raw `void(*)(void*)` callback**: no type safety. The callback must be cast back. No RAII — manual cancel required.
- **Global-only callback capture**: the callback uses only globals (`stderr`, `terminateProcess`). This means per-test customization (e.g., different timeout actions per test) is impossible without more globals.

### Blind Spots / Missing
- **No `Deadline` or RAII wrapper**: the proposal has `TimerHandle = void*` with manual creation/cancel. If an exception is thrown between creation and cancel, the timer leaks forever. The test framework catches exceptions, but the timer handle still needs cleanup.
- **50ms polling resolution**: the timeout accuracy is ±50ms. For a 120s timeout this is fine. But if someone sets a 100ms timeout for quick-deadlock detection, it's wildly inaccurate.
- **Doesn't address where in `RunImpl` the timeout is stored/checked**: needs to add a field to `RunImpl` or `Context`, but doesn't detail the plumbing.
- **No test for the POSIX race or leak**: acknowledging a bug without a test to detect it (or to document the expected behavior) means it will be rediscovered.
- **`DeleteTimerQueueTimer` blocking cancel**: what if the callback is stuck (e.g., test is in an infinite loop and the callback is trying to log)? The callback runs on the timer thread, so if it logs to `stderr` which blocks on a full buffer, the cancel hangs too. `WT_EXECUTEINTIMERTHREAD` timers on the timer thread must never block.

### What to Adopt
- Minimal, targeted Signal fix (the `try_acquire()` after CAS). Keep it separate from the timeout feature — two PRs/commits.
- `DeleteTimerQueueTimer(..., INVALID_HANDLE_VALUE)` pattern for safe Windows cancel (blocks until done).
- Honest documentation of known issues and tradeoffs (even if some should be fixed, not just documented).
- No changes to `spawnAndWait` signature — keep the HAL interface stable.

---

## Proposal C — Fresh Perspective

### Strengths
- **Signal fix via `atomic::wait()/notify_one()`**: eliminates the `counting_semaphore` design flaw entirely. `atomic::wait()` is a thread-level wait (not a counter) and integrates naturally with `m_pending` state. No CAS+semaphore mismatch. This is a clean, minimal replacement that fixes the root cause without adding complexity to `poll()`.
- **Single cross-platform timer file**: `Core.HAL.Timer.cpp` using `std::jthread`. No per-platform files, no `CreateTimerQueueTimer` vs `timer_create` divergence. This is *much* simpler to maintain and test.
- **`Deadline` is RAII with `std::jthread`**: destructor joins the thread, which means `cancel` is implicit and safe. No manual cancel calls, no leaks, no dangling timer handles. The `jthread` join in the destructor ensures the callback has completed before `Deadline` (and thus `RunImpl`) is destroyed.
- **No changes to `spawnAndWait`**: the timeout is implemented as a child self-kill. This is architecturally clean — the child process terminates itself via `_Exit()` when the deadline fires. The parent never needs to kill the child. This avoids all the complexity of `SIGKILL`, `TerminateProcess`, polling loops, etc.
- **Auto-enables `fork` when timeout is set**: uses the existing `fork` mechanism. The fork flag already runs the test in a child process. Adding timeout support on top of fork is natural — the child sets a self-destruct timer.
- **Reduces CMake test timeout from 120s to 30s**: this is a proactive improvement. Lower timeouts mean faster CI feedback.

### Weaknesses
- **`std::jthread` availability**: requires C++20 with `<thread>` (available, already used). But `std::jthread::request_stop()` and `stop_token` are used here, which are C++20. The codebase uses C++23, so this is fine.
- **`std::jthread` for deadlines**: spawning a thread per timeout is heavyweight. A thread stack is ~1 MiB (or at least 64 KiB on some platforms). If a test suite has many timeout-enabled tests running in sequence, each test spawns and joins a thread. For a test runner, this is acceptable (tests are rare compared to, say, game frames). But if thousands of tests each got their own thread, it would be wasteful.
- **`atomic::wait()` may not be supported on all platforms**: MSVC, GCC, and Clang all support it on modern versions. The codebase already uses `std::atomic`, so this is likely fine. However, the exact semantics (spurious wakeups, performance) vary by implementation.
- **`--timeout` CLI arg propagated to child process**: the proposal says "timeout is propagated as `--timeout` CLI arg to child process" — this means the timeout is applied *after* the child starts. If the child is stuck in a tight loop without checking the deadline mechanism (which is a separate thread), the jthread will still fire and call `_Exit()`. This is correct, but the proposal should clarify the mechanism.
- **jthread stop_token vs `_Exit()`**: the callback should use `stop_token` to check if it was cancelled, but the proposal says the callback captures `this` (RunImpl*) and calls `_Exit()`. If `Deadline` is destroyed (jthread joins) before the timer fires, the callback never runs — no issue. But what if `Deadline` is destroyed via exception unwind during test failure? The join will wait for the timer to fire (or for the stop_token to be requested). If the timer hasn't fired yet, the destructor blocks until it does. This could deadlock if `_Exit()` is called from the callback but the process doesn't actually exit (e.g., `_Exit()` is called by the callback, but the main thread is stuck waiting in `Deadline::~Deadline()` — or rather, `_Exit()` terminates the process immediately, so the destructor never blocks).

Wait — let me re-examine: if `Deadline` is a member of `RunImpl`, then when `RunImpl` is destroyed (end of scope in `UnitTest::run`), `Deadline::~Deadline()` is called. This requests stop on the jthread and joins. If the timer hasn't fired yet, `request_stop()` sets the stop_token, and the timer callback can check `stop_token.stop_requested()`. But the proposal says the callback captures `this` and calls `_Exit()`. If the test passes quickly (before the timeout fires), the jthread is stopped and joined — the callback never runs. This is only a problem if the callback is already running when the destructor fires. Since the callback calls `_Exit()`, the process terminates before the join completes. Actually, join in destructor would never complete if `_Exit()` is called from the callback — but `_Exit()` terminates the whole process, so the destructor never has to worry about blocking. So this is actually fine.

But there's a subtler issue: what if `std::jthread`'s destructor requests stop, but the sleeping thread hasn't woken up yet? The destructor will call `join()` which blocks until the thread finishes. But `std::jthread`'s destructor calls `request_stop()` first, then `join()`. The thread's sleep (e.g., `sleep_for`) will be interrupted if it uses `stop_token` — but the proposal uses `std::jthread` without specifying how the timer sleep is implemented. If it uses `std::this_thread::sleep_for` without a stop_token-aware sleep, the join will block for the full timeout duration. A proper implementation would use `sleep_until(stop_token, time)` or a condition variable with a timeout.

This is a significant blind spot: **the proposal assumes `std::jthread` destructor magic makes everything safe, but doesn't specify a stop-aware sleep**. Without it, `~Deadline()` blocks for up to the full timeout.

### Blind Spots / Missing
- **Stop-aware sleep**: the `jthread`'s sleep in the deadline thread must use a `stop_token`-compatible wait (e.g., condition variable `wait_for` with `stop_token`). Plain `sleep_for` blocks the destructor's join.
- **Where does `Deadline` live?**: it needs to be a member of `RunImpl` (or wrapped by it). But `RunImpl` is defined inside `UnitTest` and constructed on the stack. Adding a `Deadline` member means `RunImpl` must be movable or have a defined destruction order. The proposal says "Deadline destructor join ensures completion before RunImpl dies" — this is true only if `Deadline` is declared before other members in `RunImpl` (reverse destruction order means it's destroyed last... wait, members are destroyed in reverse declaration order. So `Deadline` should be declared LAST so it's destroyed FIRST, ensuring it cancels before other members are destroyed).
- **Thread overhead for every test**: `std::jthread` per timeout means a thread create/join pair. For a test suite with 1000 tests, that's 1000 threads created and destroyed. On Windows, thread creation is ~microseconds, but it's not free. The CMake test timeout reduction to 30s mostly mitigates concern, but it's worth noting.
- **macOS/Linux behavior of `std::jthread`**: fine on all three targets, but the stop_token interruptible sleep requires explicit support (condition variable, not `sleep_for`).
- **The 1-line Signal fix vs `atomic::wait()` replacement**: Proposal C replaces `counting_semaphore` with `atomic::wait()/notify_one()`. This is a rewrite of the primitives, not a fix. It may introduce different performance characteristics (kernel wakeups vs userspace spinning). Should be benchmarked.
- **CLI arg plumbing details**: "propagated as --timeout CLI arg to child process" — where is this plumbed? In `startInChildProcess_()`? In `RunImpl`? In `Context`? Not specified.

### What to Adopt
- `atomic::wait()/notify_one()` as Signal fix — clean, correct, eliminates the semaphore counter mismatch. **Best of the three Signal fix approaches**.
- RAII `Deadline` with automatic cleanup — best for exception safety.
- No `spawnAndWait` changes — leverages existing `fork` mechanism. **Architecturally cleanest**.
- Self-kill via `_Exit()` in child — avoids parent-side kill complexity entirely.
- CMake test timeout reduction (30s is reasonable).

---

## Cross-Proposal Synthesis

### Signal Fix: Adopt C's Approach

| Aspect | A | B | C |
|--------|---|---|---|
| Fix type | Handoff in `poll()` | `try_acquire()` after CAS | Replace semaphore with `atomic::wait()` |
| Risk | Medium (modifies CAS loop) | Low (1-line add) | Low (proven pattern) |
| Correctness | Unclear | Conservative but proven | Cleanest |

**Verdict**: C's approach is the cleanest. Use `atomic::wait()/notify_one()` — it matches the bitfield state directly and eliminates the semaphore counter entirely. B's `try_acquire()` is a close second (minimal, safe). A's handoff is risky and under-specified.

### Timer API: Adopt C's RAII + B's Conservative HAL Protection

**C's `Deadline` with `std::jthread`** is the best API: RAII, auto-join, no platform divergence. The cross-platform simplicity outweighs the per-thread overhead for a test runner.

**However**: the proposal must implement a **stop_token-aware sleep** (e.g., `std::condition_variable::wait_for` with a `stop_token`) to avoid blocking the destructor join for the full timeout.

If `std::jthread` is deemed too heavy, fall back to B's `TimerHandle` approach but wrap it in an RAII class with proper cleanup — do not expose raw handles.

### Timeout Mechanism: Adopt C's Fork+Self-Kill

**C's approach is architecturally superior**: instead of modifying `spawnAndWait` to support timeout (which complicates the HAL interface), timeout runs as a *child-process self-kill* using the existing `fork` flag. This leverages existing infrastructure:

- `startInChildProcess_()` already handles `fork` + `spawnAndWait`
- Child process sets a `Deadline` that calls `_Exit()` on fire
- Parent process gets the exit code naturally (child killed by `_Exit` = exit code from `_Exit`)
- No changes needed to platform-specific `spawnAndWait` implementations

**B's polling loop** (`WNOHANG` + 50ms sleep) should be rejected — it's wasteful and inaccurate. **A's `ualarm`+SIGKILL** is better but requires signal handling and doesn't work on macOS.

### Termination: Adopt A's `_exit`/`_Exit` (not B's `exit`)

**A is correct**: `_exit` on POSIX / `TerminateProcess` on Windows. **B's `exit()` is wrong** for a timed-out process that may hold locks or have atexit handlers in a broken state. The child process should die immediately without cleanup.

### Thread Safety: Adopt A's `atomic<bool> m_fired`

**A's `std::atomic<bool> m_fired`** in `Deadline` is a good pattern for race-safe cancel-or-fired detection. C's `jthread` approach doesn't need this (destructor join guarantees completion), but for the fallback case (raw timer handles), A's pattern is correct.

### Code Organization

Split into two independent changes:
1. **Signal fix** (C's `atomic::wait()` approach) — commit 1
2. **Per-test timeout** (C's RAII `Deadline` + fork self-kill) — commit 2

Do NOT bundle them as A does. B has the right idea about separation.

### Summary Decision Matrix

| Concern | Take from |
|---------|-----------|
| Signal fix | **C**: `atomic::wait()/notify_one()` |
| Timer API | **C**: `std::jthread`-based RAII `Deadline` (with stop_token sleep!) |
| Timeout mechanism | **C**: fork + child self-kill (`_Exit()`) |
| `spawnAndWait` changes | **None needed** (C) |
| Platform timer impl | **Single file** (C) — `std::jthread` |
| Termination call | **A**: `_exit`/`_Exit`/`TerminateProcess` (`[[noreturn]]`) |
| Cancel safety | **A**: `atomic<bool>` for race detection (or C's jthread join) |
| Windows cancel | **B**: `DeleteTimerQueueTimer(..., INVALID_HANDLE_VALUE)` (if not using jthread) |
| Unit tests | **A**: add timer + timeout tests |
| Known-issue doc | **B**: document tradeoffs honestly |
| CMake timeout | **C**: reduce from 120 to 30 |

### Remaining Concerns for All Proposals

1. **Where does `Deadline` live in `RunImpl`?** All three need to specify: member of `RunImpl`, declared last (destroyed first).
2. **CLI plumbing**: `--timeout` needs to reach `Context`, then `RunImpl`, then `Deadline`. None of the proposals fully specify this.
3. **`fork` + timeout interaction**: if timeout auto-enables fork (C), then tests flagged `fork` already get timeout. But tests without `fork` (simple tests) would be force-forked. Is this acceptable? The cost is a process spawn per test. For non-fork tests, this adds overhead.
4. **macOS**: Proposal A assumes POSIX primitives (`timer_create`, `ualarm`). macOS has `timer_create` but deprecated `ualarm`. Proposal B's polling loop works on macOS. Proposal C's `std::jthread` works on all three. C wins on portability.
5. **Thread-per-timeout overhead**: for 1000 tests with timeout, C creates 1000 threads. On Windows, a thread is ~1 MB of virtual address space (committed stack ~4 KB). This is fine for a test runner but worth noting in the commit message.
