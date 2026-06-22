# Synthesis: Per-Test Timeout with Deadlock Detection

*Generated from 3 proposals (A-aggressive, B-conservative, C-fresh) and 3 cross-reviews*

---

## High Confidence — Consensus Across All Reviews

| Decision | Consensus | Source |
|----------|-----------|--------|
| Signal fix as **separate first commit** | ✅ All agree | B Phase 1, C Phase 1 |
| **`_exit()`/`_Exit()` not `exit()`** for timeout termination | ✅ All agree `exit()` is wrong | A line 249, B reviewers flag |
| **Exit code 124** for timeout (distinguishable from generic failure) | ✅ All agree | A line 328 |
| **Per-test lifecycle**: timer in `start()` / cancel in `~RunImpl()` | ✅ All agree | A/B/C all converge |
| **Propagate `--timeout`** to child process via CLI args | ✅ All agree | A lines 448-466, C line 254 |
| **Failure-mode documentation** per step (B's pattern) | ✅ All agree | B edge-cases section |

---

## Needs Judgment — Where Reviewers Disagreed

### 1. Timer backend: OS platform APIs vs `std::jthread`

- **Reviewers strongly prefer** C's single-file `std::jthread` approach (cross-platform, RAII, no per-platform files)
- **However, the user explicitly chose Option C** (OS-level timers abstracted through HAL, process termination on timeout)
- **Synthesis**: Use per-platform OS timer files (Windows `CreateTimerQueueTimer`, POSIX `timer_create`+`SIGEV_THREAD`) as the user requested, BUT add a `std::jthread`-based fallback in the generic platform implementation for non-major platforms. This respects the user's architectural choice while providing graceful fallback.

**Why OS timers are still valid despite reviewer preference for `jthread`:**
- The user specifically chose this approach for consistency with the existing HAL pattern
- `CreateTimerQueueTimer` uses a shared thread pool (no thread-per-timer overhead)
- `timer_create(SIGEV_THREAD)` on POSIX also uses a system-managed helper thread
- The callback runs on a timer thread that `cancelDeadline` can synchronously wait for
- OS timers can interrupt the process even if the test is in a tight loop that prevents `jthread` scheduling

### 2. Signal fix: `atomic::wait()` vs `try_acquire()` vs handoff

- **Reviewers A and B prefer** C's `atomic::wait()/notify_one()` for the multi-signal case
- **Reviewer A flags** that C's single-signal fix (with `m_notified`) has a race
- **Reviewer C recommends** C's `atomic::wait()` as "cleanest"
- **Synthesis**: Use C's `atomic::wait()/notify_one()` for the **multi-signal** `Signal<EventsT...>` — the existing `m_pending` atomic doubles as the wait variable, no token mismatch. For the **single-signal** `Signal<EventT>`, use B's simpler `try_acquire()` approach (1-line add, no new state). This hybrid is the safest path.

### 3. Callback capture: safety vs diagnostic quality

- **B's callback** captures nothing (globals only) → zero use-after-free risk → but can't print test path
- **A/C's callback** captures `RunImpl*` or test path → better diagnostics → needs careful ordering guarantees
- **Synthesis**: Capture the **test path string by value** (not `RunImpl*`). This lets the callback print the path without touching `RunImpl` after the test completes. The exit code 124 already distinguishes timeout from other failures. The callback also captures the timeout duration for the diagnostic message.

### 4. Auto-fork semantic change

- **C proposes** auto-enabling `fork` when timeout is set (child self-kill via `_Exit()`)
- **All reviewers agree** this is architecturally clean but acknowledge behavioral change
- **Synthesis**: Auto-enable fork (C's approach). This is the most important safety improvement — it prevents the timeout from killing the entire test runner. Document in `--help` that `--timeout` implicitly enables child-process isolation.

### 5. Parent-side watchdog

- **A and B** add `spawnAndWait` timeout overload (parent safety net)
- **C** relies on CMake-level timeout
- **Synthesis**: Add `spawnAndWait` timeout overload (A/B approach) as a belt-and-suspenders safety net. The parent should have a timeout slightly longer than the child's self-kill deadline. This catches cases where the child's deadline thread fails to fire (scheduler starvation, etc.).

---

## Final Plan

### Signal fix (independent commit)

| Step | Description | Files | Operation |
|------|-------------|-------|-----------|
| 1 | Multi-signal `Signal<EventsT...>`: replace `counting_semaphore` with `atomic::wait()/notify_one()` on `m_pending` | `Core.Concurrency.Event.cppm` | Edit |
| 2 | Single-signal `Signal<EventT>`: add `m_semaphore.try_acquire()` after successful `pollEvent()` | `Core.Concurrency.Event.cppm` | Edit (1 line) |

### Core changes

| Step | Description | Files | Operation |
|------|-------------|-------|-----------|
| 3 | Declare `hal::timer::setDeadline(ms, callback)`, `cancelDeadline()`, and `hal::process::terminateProcess()` | `Core.HAL.cppm` | Edit |
| 4 | Windows timer: `CreateTimerQueueTimer` with `WT_EXECUTEDEFAULT` | `windows/Core.HAL.windows.Timer.cpp` | **Create** |
| 5 | Linux timer: `timer_create(CLOCK_MONOTONIC, SIGEV_THREAD)` | `linux/Core.HAL.linux.Timer.cpp` | **Create** |
| 6 | Darwin timer: same as Linux (POSIX `timer_create`) | `darwin/Core.HAL.darwin.Timer.cpp` | **Create** |
| 7 | Generic timer: `std::jthread`-based fallback (throws if create fails) | `generic/Core.HAL.generic.Timer.cpp` | **Create** |
| 8 | Windows: add `terminateProcess(TerminateProcess)`, `spawnAndWait` timeout overload | `windows/Core.HAL.windows.Process.cpp` | Edit |
| 9 | Linux: add `terminateProcess(_exit)`, `spawnAndWait` timeout overload (polling loop with `WNOHANG`) | `linux/Core.HAL.linux.Process.cpp` | Edit |
| 10 | Darwin: same as Linux | `darwin/Core.HAL.darwin.Process.cpp` | Edit |
| 11 | Generic: add stubs for terminateProcess and spawnAndWait timeout | `generic/Core.HAL.generic.Process.cpp` | Edit |
| 12 | Register Timer.cpp in HAL_PLATFORM_SOURCES | `CMakeLists.txt` (core) | Edit |

### Unit test framework integration

| Step | Description | Files | Operation |
|------|-------------|-------|-----------|
| 13 | Add `m_timeout` (optional ms) to `Context` | `Core.UnitTest.cppm` | Edit |
| 14 | Add `m_deadline` (timer handle) to `RunImpl` | `Core.UnitTest.cppm` | Edit |
| 15 | Arm timer in `RunImpl::start()` with callback that writes test path + calls `terminateProcess(124)` | `Core.UnitTest.cpp` | Edit |
| 16 | Disarm timer in `~RunImpl()` via `cancelDeadline()` | `Core.UnitTest.cpp` | Edit |
| 17 | Auto-enable `fork` in `UnitTest::run()` when timeout is set and test is not already forked | `Core.UnitTest.cpp` | Edit |
| 18 | Forward `--timeout` to child process in `startInChildProcess_()` | `Core.UnitTest.cpp` | Edit |
| 19 | Use `spawnAndWait` timeout overload for forked tests | `Core.UnitTest.cpp` | Edit |

### CLI

| Step | Description | Files | Operation |
|------|-------------|-------|-----------|
| 20 | Parse `--timeout <ms>`, support `PPR_TEST_TIMEOUT` env var | `tests/main.cpp` | Edit |
| 21 | Update help text | `tests/main.cpp` | Edit |

### Verification

| Step | Description | Files | Operation |
|------|-------------|-------|-----------|
| 22 | Deadline timer lifecycle test (arm/cancel before fire) | `tests/core/Core.HAL.Tests.cppm` | **Create** |
| 23 | Timeout integration test (fork test with short timeout, expect exit code 124) | `tests/core/Core.UnitTest.Tests.cppm` | **Create** |

---

## Key Design Details

### Timer callback (runs on OS timer thread)
```cpp
// Captures test_path and timeout_ms by value (no RunImpl* capture)
[test_path = currentPath(), timeout_ms = *m_context.m_timeout]() noexcept {
    std::cerr << "\n*** TIMEOUT: test '" << test_path
              << "' exceeded " << timeout_ms.count() << "ms ***\n"
              << "*** Terminating process. ***\n" << std::flush;
    hal::process::terminateProcess(124);
}
```

### CancelDeadline race safety
- **Windows**: `DeleteTimerQueueTimer(..., INVALID_HANDLE_VALUE)` blocks until pending callbacks complete → safe
- **POSIX**: `timer_delete()` disarms the timer. The `SIGEV_THREAD` callback has already started or will never start. A `std::atomic<bool> m_fired` in the callback context struct handles the race: if fired → callback owns deletion; if not fired → cancelDeadline deletes.
- **Generic**: `std::jthread` join in destructor is the cancel mechanism.

### Auto-fork rule
```cpp
EFlags effective_flags = m_flags;
if ((effective_flags & fork) == none && m_context.m_timeout.has_value()) {
    effective_flags = static_cast<EFlags>(effective_flags | fork);
}
```

### spawnAndWait timeout (parent watchdog)
- Set to `*m_context.m_timeout + std::chrono::seconds(5)` — parent gives the child a 5s grace period beyond its self-kill deadline
- Windows: `WaitForSingleObject(ms)` → `TerminateProcess` on timeout
- POSIX: Polling loop with `WNOHANG` + 50ms sleep → `kill(SIGKILL)` on timeout

---

## Summary of All Files

| File | Action |
|------|--------|
| `Core.Concurrency.Event.cppm` | Edit (multi-signal: atomic::wait; single-signal: try_acquire) |
| `Core.HAL.cppm` | Edit (declare hal::timer + termininateProcess + spawnAndWait overload) |
| `windows/Core.HAL.windows.Timer.cpp` | **Create** |
| `linux/Core.HAL.linux.Timer.cpp` | **Create** |
| `darwin/Core.HAL.darwin.Timer.cpp` | **Create** |
| `generic/Core.HAL.generic.Timer.cpp` | **Create** |
| `windows/Core.HAL.windows.Process.cpp` | Edit |
| `linux/Core.HAL.linux.Process.cpp` | Edit |
| `darwin/Core.HAL.darwin.Process.cpp` | Edit |
| `generic/Core.HAL.generic.Process.cpp` | Edit |
| `CMakeLists.txt` (core) | Edit |
| `Core.UnitTest.cppm` | Edit |
| `Core.UnitTest.cpp` | Edit |
| `tests/main.cpp` | Edit |
| `tests/core/Core.HAL.Tests.cppm` | **Create** |
| `tests/core/Core.UnitTest.Tests.cppm` | **Create** |

**Totals:** 6 new files, 11 edited files. ~350 lines net.
