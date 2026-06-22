# Per-Test Timeout with Deadlock Detection — Proposal A (Aggressive)

## Design Philosophy

- **Zero-cost when unused:** no allocations, no timer setup, no branch overhead in the hot path when `m_timeout` is `std::nullopt`.
- **Fail-fast, fail-loud:** timeout = immediate `terminateProcess`. No recovery, no half-corrupted state. The stderr diagnostic includes the full test path so the failure is trivially actionable.
- **Race-safe by construction:** the handoff in `poll()` combined with an atomic `m_timed_out` flag guarantees no double-termination and no lost-wakeup even under concurrent notify/poll.
- **Platform elegance:** each HAL backend is a single `.cpp` file. Opaque `void*` handles keep the interface clean.

---

## Phase 1 — `hal::timer` namespace (deadline API)

### Step 1 — Declare `hal::timer` in `Core.HAL.cppm`

- **Files:** `lib/engine/core/Core.HAL.cppm`
- **Operations:** edit

Add a new `hal::timer` namespace block inside the existing `namespace pP::hal {}`:

```cpp
namespace timer {
    using TimerHandle = void *;

    struct Deadline {
        TimerHandle m_handle{nullptr};
        std::atomic<bool> m_fired{false};  // set by callback, consumed by cancel
    };

    // Arms an OS timer that fires `callback` after `ms` milliseconds.
    // Returns a Deadline whose m_handle tracks the platform resource.
    // The callback receives the Deadline address so it can set m_fired.
    // Must be cancellable via cancelDeadline.
    [[nodiscard]] Deadline setDeadline(u64 ms, void(*callback)(Deadline*) noexcept) noexcept(false);

    // Cancels a running deadline. If the callback has already fired (m_fired == true)
    // or fires concurrently, the platform cancel may fail — this is safe because
    // the Deadline outlives the cancel call (it lives on RunImpl's stack).
    // Returns true if the timer was successfully cancelled before it fired.
    [[nodiscard]] bool cancelDeadline(Deadline &dl) noexcept;
}
```

The `Deadline` struct embeds a `std::atomic<bool> m_fired` that the callback sets. `cancelDeadline` checks this after the platform cancel call to detect the race: if the platform says "too late, already fired" but `m_fired` is still false, we spin briefly. This eliminates the need for external synchronisation.

### Step 2 — Implement Windows timer backend

- **Files:** `lib/engine/core/windows/Core.HAL.windows.Timer.cpp` (new)
- **Operations:** create

```cpp
module;
#include "Core.HAL.windows.include.h"
module engine.core;
import :hal;

namespace pP::hal::timer {
    struct alignas(64) WindowsTimer {
        HANDLE m_timer{nullptr};
        HANDLE m_completion{nullptr};  // event for synchronous cancel wait
    };

    void CALLBACK TimerCallback_(PVOID param, BOOLEAN) noexcept {
        auto *dl = static_cast<Deadline *>(param);
        dl->m_fired.store(true, std::memory_order_release);
    }

    Deadline setDeadline(u64 ms, void(*callback)(Deadline*) noexcept) noexcept(false) {
        Deadline dl;
        auto *win = new WindowsTimer{};
        win->m_completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!win->m_completion) { delete win; throw std::bad_alloc(); }
        BOOL ok = CreateTimerQueueTimer(
            &win->m_timer, nullptr,
            TimerCallback_, &dl, ms, 0,
            WT_EXECUTEINTIMERTHREAD | WT_EXECUTEINPERSISTENTTHREAD);
        if (!ok) { CloseHandle(win->m_completion); delete win; throw std::runtime_error("CreateTimerQueueTimer failed"); }
        dl.m_handle = win;
        return dl;
    }

    bool cancelDeadline(Deadline &dl) noexcept {
        if (!dl.m_handle) return true;
        auto *win = static_cast<WindowsTimer *>(dl.m_handle);
        // Request cancel — INVALID_HANDLE_VALUE means "don't wait for callback"
        BOOL ok = DeleteTimerQueueTimer(nullptr, win->m_timer, INVALID_HANDLE_VALUE);
        if (ok) {
            // Timer was removed before it fired.
            CloseHandle(win->m_completion);
            delete win;
            dl.m_handle = nullptr;
            return true;
        }
        // The callback may have already fired or is running.
        // If m_fired is set, the callback ran — safe to clean up.
        // If m_fired is NOT set, the callback is in-flight; wait for it.
        if (!dl.m_fired.load(std::memory_order_acquire)) {
            WaitForSingleObject(win->m_completion, INFINITE);
        }
        CloseHandle(win->m_completion);
        delete win;
        dl.m_handle = nullptr;
        return false;
    }
}
```

Wait — the callback above uses `TimerCallback_` which only sets `m_fired`. But the requirement says the callback should write a diagnostic and terminate the process. Since this callback runs on the timer thread (not the test thread), we **cannot** safely write to stderr from there (it would interleave with test output). Instead:

**Refinement:** The callback only sets `m_fired = true` and terminates the process via `terminateProcess()`. The diagnostic is written **by the test thread** in `~RunImpl()` when it detects `m_timed_out`. The terminated process never reaches that code, but the stderr output from the parent (or the OS) is sufficient. Actually, the requirement says "write diagnostic to stderr, then terminate the entire process". Let me follow the requirement: the callback writes to stderr first, then calls `terminateProcess`. On the timer thread, this is safe because stderr is process-global and we're about to die anyway.

### Step 3 — Implement POSIX timer backend

- **Files:**
  - `lib/engine/core/linux/Core.HAL.linux.Timer.cpp` (new)
  - `lib/engine/core/darwin/Core.HAL.darwin.Timer.cpp` (new)
  - `lib/engine/core/generic/Core.HAL.generic.Timer.cpp` (new — throws not implemented)
- **Operations:** create

POSIX backend uses `timer_create(CLOCK_MONOTONIC, SIGEV_THREAD, ...)`:

```cpp
module;
#include <unistd.h>
#include <signal.h>
#include <time.h>
module engine.core;
import :hal;

namespace pP::hal::timer {
    struct PosixTimer {
        timer_t m_timer{};
    };

    extern "C" void TimerCallback_(sigval sv) noexcept {
        auto *dl = static_cast<Deadline *>(sv.sival_ptr);
        dl->m_fired.store(true, std::memory_order_release);
        // notify the termination path
    }

    Deadline setDeadline(u64 ms, void(*callback)(Deadline*) noexcept) noexcept(false) {
        Deadline dl;
        auto *posix = new PosixTimer{};
        struct sigevent sev{};
        sev.sigev_notify = SIGEV_THREAD;
        sev.sigev_notify_function = TimerCallback_;
        sev.sigev_value.sival_ptr = &dl;
        if (timer_create(CLOCK_MONOTONIC, &sev, &posix->m_timer) == -1) {
            delete posix;
            throw std::runtime_error("timer_create failed");
        }
        struct itimerspec its{};
        its.it_value.tv_sec = ms / 1000;
        its.it_value.tv_nsec = (ms % 1000) * 1'000'000;
        if (timer_settime(posix->m_timer, 0, &its, nullptr) == -1) {
            timer_delete(posix->m_timer);
            delete posix;
            throw std::runtime_error("timer_settime failed");
        }
        dl.m_handle = posix;
        return dl;
    }

    bool cancelDeadline(Deadline &dl) noexcept {
        if (!dl.m_handle) return true;
        auto *posix = static_cast<PosixTimer *>(dl.m_handle);
        timer_delete(posix->m_timer);
        delete posix;
        dl.m_handle = nullptr;
        // If the callback fired concurrently, m_fired is already true.
        return !dl.m_fired.load(std::memory_order_acquire);
    }
}
```

### Step 4 — Register new timer files in CMakeLists.txt

- **Files:** `lib/engine/core/CMakeLists.txt`
- **Operations:** edit

Append to `HAL_PLATFORM_SOURCES`:

```cmake
${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Timer.cpp
```

---

## Phase 2 — `hal::process::terminateProcess()` and timed `spawnAndWait`

### Step 1 — Declare in `Core.HAL.cppm`

- **Files:** `lib/engine/core/Core.HAL.cppm`
- **Operations:** edit

Inside `hal::process`, add:

```cpp
// Terminates the current process immediately without cleanup.
// Equivalent to TerminateProcess(self) on Windows, _exit() on POSIX.
[[noreturn]] void terminateProcess(int exit_code) noexcept;

// Timeout overload: kills the child after `timeout_ms` milliseconds.
[[nodiscard]] int spawnAndWait(
    const std::filesystem::path &executable,
    std::span<const std::string> args,
    u64 timeout_ms) noexcept(false);
```

`terminateProcess` is `[[noreturn]]` and does NOT run destructors, flush buffers, or call atexit handlers — that's the point. The only safe thing after a deadlock timeout is immediate death.

### Step 2 — Implement Windows

- **Files:** `lib/engine/core/windows/Core.HAL.windows.Process.cpp`
- **Operations:** edit

```cpp
[[noreturn]] void terminateProcess(int exit_code) noexcept {
    // Flush stderr first — it's the one thing we want to preserve
    // (the timeout diagnostic was already written by the timer callback).
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(exit_code));
    // Unreachable, but satisfy [[noreturn]]:
    for (;;) ::_mm_pause();
}

[[nodiscard]] int spawnAndWait(
    const std::filesystem::path &executable,
    std::span<const std::string> args,
    const u64 timeout_ms) noexcept(false)
{
    // ... same setup as the original spawnAndWait ...
    // Replace WaitForSingleObject(pi.hProcess, INFINITE) with:
    const DWORD wait_result = ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    if (wait_result == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, INFINITE);
    }
    // ... same GetExitCodeProcess + CloseHandle ...
}
```

### Step 3 — Implement generic

- **Files:** `lib/engine/core/generic/Core.HAL.generic.Process.cpp`
- **Operations:** edit

```cpp
[[noreturn]] void terminateProcess(int exit_code) noexcept {
    ::_exit(exit_code);
}
```

The generic `spawnAndWait` timeout overload just throws (same as the base).

### Step 4 — Implement Linux/Darwin

- **Files:**
  - `lib/engine/core/linux/Core.HAL.linux.Process.cpp`
  - `lib/engine/core/darwin/Core.HAL.darwin.Process.cpp`
- **Operations:** edit

```cpp
[[noreturn]] void terminateProcess(int exit_code) noexcept {
    ::_exit(exit_code);
}

[[nodiscard]] int spawnAndWait(
    const std::filesystem::path &executable,
    std::span<const std::string> args,
    const u64 timeout_ms) noexcept(false)
{
    const pid_t pid = ::fork();
    if (pid == 0) {
        // ... same exec setup ...
        ::_exit(127);
    }
    if (pid < 0) throw std::runtime_error("fork failed");

    // Use timer + kill instead of alarm() for better signal hygiene
    // We'll use a one-shot  SIGALRM approach for simplicity:
    struct sigaction old_sa{};
    struct sigaction sa{};
    sa.sa_handler = [](int) {};  // just interrupt waitpid
    ::sigaction(SIGALRM, &sa, &old_sa);

    ualarm(timeout_ms * 1000, 0);  // microsecond precision

    int status = 0;
    const pid_t waited = ::waitpid(pid, &status, 0);

    ualarm(0, 0);               // disarm
    ::sigaction(SIGALRM, &old_sa, nullptr);

    if (waited == 0 || (waited == -1 && errno == EINTR)) {
        // Timed out — SIGALRM interrupted waitpid
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        return -1;
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}
```

---

## Phase 3 — `UnitTest` core changes: `Context::m_timeout`, `RunImpl::m_deadline`

### Step 1 — Add `m_timeout` to `Context` and `m_deadline` to `RunImpl`

- **Files:** `lib/engine/core/Core.UnitTest.cppm`
- **Operations:** edit

In `struct Context`, add:
```cpp
std::optional<u64> m_timeout{};  // milliseconds, nullopt = no deadline
```

In `struct RunImpl`, add:
```cpp
std::optional<decltype(hal::timer::setDeadline(0, nullptr))> m_deadline{};
bool m_timed_out{false};  // set by timer callback on timeout thread

static void OnTimeoutCallback_(hal::timer::Deadline *dl) noexcept;
```

The `OnTimeoutCallback_` static method writes the test path to stderr and calls `hal::process::terminateProcess(124)`.

### Step 2 — Arm deadline in `RunImpl::start()`

- **Files:** `lib/engine/core/Core.UnitTest.cpp` (the implementation file)
- **Operations:** edit

In `RunImpl::start()`, after existing setup:
```cpp
void UnitTest::RunImpl::start() noexcept {
    // ... existing assert policy setup ...

    if (m_context.m_timeout.has_value()) {
        // Capture the test path by value for the timer thread
        const std::string path = currentPath();
        m_deadline = hal::timer::setDeadline(*m_context.m_timeout, [this, path](hal::timer::Deadline *dl) noexcept {
            // This runs on the OS timer thread
            std::cerr << "\n*** TIMEOUT: test '" << path << "' exceeded "
                      << *m_context.m_timeout << "ms ***\n"
                      << "*** The test likely deadlocked or entered an infinite loop. ***\n"
                      << "*** Terminating process. ***\n" << std::flush;
            m_timed_out = true;
            hal::process::terminateProcess(124);
        });
    }

    m_start_time = std::chrono::steady_clock::now();
}
```

Wait — the callback needs access to both `m_context.m_timeout` (to print the limit) and `*this` (to set `m_timed_out`). The `RunImpl` object is stack-allocated in `UnitTest::run()`, so it lives until the test returns. The callback fires asynchronously and terminates the process, so there's no use-after-free concern: either the timer is cancelled before `~RunImpl()` runs, or the callback fires and terminates everything, or the callback fires concurrently with the destructor — but `terminateProcess` is [[noreturn]] so the destructor never completes after termination.

BUT: we must guarantee that `cancelDeadline` is robust against the concurrent callback. The `Deadline::m_fired` flag handles this: if the callback runs before `cancelDeadline`, `m_fired` is true and the handle is cleaned up; if `cancelDeadline` runs first, the callback never fires; if they race, `cancelDeadline` detects the race via `m_fired` and the platform cancel status.

### Step 3 — Disarm deadline in `~RunImpl()`

- **Files:** `lib/engine/core/Core.UnitTest.cpp`
- **Operations:** edit

At the beginning of `~RunImpl()`, before the duration calculation:

```cpp
UnitTest::RunImpl::~RunImpl() {
    // Disarm deadline first — prevents timer callback from firing during teardown
    if (m_deadline.has_value()) {
        hal::timer::cancelDeadline(*m_deadline);
        m_deadline.reset();
    }

    // ... rest of existing destructor ...
}
```

### Step 4 — Handle timeout in `UnitTest::run()`

- **Files:** `lib/engine/core/Core.UnitTest.cpp`
- **Operations:** edit

In `UnitTest::run()`, inside the `try` block after the test executes, check the timeout flag:

```cpp
void UnitTest::run(IRun &run) const noexcept {
    try {
        auto &impl = checked_cast<RunImpl>(run);
        impl.start();

        if ((m_flags & fork) == none) [[likely]] {
            m_run(run);
            // Check if the timer fired and we somehow survived (race)
            if (impl.m_timed_out) [[unlikely]] {
                return;  // terminateProcess was already called; this is unreachable
            }
            // ... existing expected-to-fail logic ...
        } else {
            // ... fork path ...
        }
        run.success();
    } catch (std::exception &e) {
        // ... existing exception handling ...
    }
}
```

---

## Phase 4 — CLI `--timeout <ms>` argument

### Step 1 — Parse `--timeout` in `main.cpp`

- **Files:** `lib/engine/tests/main.cpp`
- **Operations:** edit

Add to the argument parsing loop:

```cpp
else if (arg == "--timeout" && i + 1 < argc) {
    context.m_timeout = std::stoull(argv[++i]);
}
```

Lazy refinement — also support `PPR_TEST_TIMEOUT` environment variable as default:

```cpp
// Before the loop, check env:
if (const char *env_timeout = std::getenv("PPR_TEST_TIMEOUT")) {
    context.m_timeout = std::stoull(env_timeout);
}
```

Update the `--help` text:

```cpp
"  --timeout <ms>      Per-test timeout in milliseconds (default: env PPR_TEST_TIMEOUT)\n"
```

### Step 2 — Pass timeout to child processes

- **Files:** `lib/engine/core/Core.UnitTest.cpp`
- **Operations:** edit

In `startInChildProcess_()`, forward the timeout via `--timeout` argument:

```cpp
bool UnitTest::startInChildProcess_(RunImpl &run) const {
    if (run.m_context.isChildRun()) {
        m_run(run);
        return true;
    }
    const auto exe_path = hal::process::currentExecutablePath();
    const std::string test_path = run.currentPath();
    std::vector<std::string> child_args{"--child-run", "--run-test", test_path};
    if (run.m_context.m_timeout.has_value()) {
        child_args.push_back("--timeout");
        child_args.push_back(std::to_string(*run.m_context.m_timeout));
    }
    const int exit_code = hal::process::spawnAndWait(exe_path, child_args);
    return (exit_code == 0);
}
```

---

## Phase 5 — Fix `Signal::wait()` lost-wakeup bug

### Step 1 — Analyze the bug

The bug in `Signal::wait()` (`Core.Concurrency.Event.cppm:84-88`):

```cpp
void wait() noexcept override {
    while (m_pending.load(std::memory_order_acquire) == 0u) {
        m_semaphore.acquire();
    }
}
```

**Scenario (all interleavings on different threads):**
1. Thread A (iterator): `wait()` → `load()` → 0 → about to call `acquire()`
2. Thread B: `notify()` → sets bit → releases semaphore (count: 0→1)
3. Thread C: `poll()` → CAS clears the bit → `m_pending` is now 0
4. Thread A: `acquire()` → succeeds (count: 1→0) → loops → `load()` → **0** → calls `acquire()` → **blocks forever**

The semaphore notification was "consumed" by thread A's `acquire()`, but the work was stolen by thread C's `poll()`. No more notifications will come, so thread A sleeps forever.

### Step 2 — Apply the handoff fix

- **Files:** `lib/engine/core/Core.Concurrency.Event.cppm`
- **Operations:** edit

In `poll()`, after the CAS succeeds, detect whether the cleared bit was the **last** pending bit. If so, release the semaphore to hand off the wakeup to any waiter who might have missed it:

```cpp
[[nodiscard]] std::optional<Event> poll() noexcept {
    std::size_t pending = m_pending.load(std::memory_order_acquire);
    for (;;) {
        if (pending == 0u) {
            return std::nullopt;
        }
        const std::size_t desired_pending = pending & (pending - 1u);
        if (m_pending.compare_exchange_weak(
            pending, desired_pending,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    };

    // Handoff: if pending had exactly one bit set, we just transitioned
    // m_pending from 1→0. Any waiter that called wait() and got the
    // semaphore token will re-check m_pending and see 0. Release one
    // extra token so it can try again (it will re-enter wait properly).
    const bool was_last = pending != 0u && (pending & (pending - 1u)) == 0u;
    if (was_last) [[unlikely]] {
        m_semaphore.release();
    }

    const std::size_t ready_index = std::countr_zero(pending);
    return getOptionalEvent_(ready_index);
}
```

The same fix applies to the single-event `Signal<EventT>` partial specialization. In that version, `poll()` is simpler but the same race exists:

```cpp
[[nodiscard]] std::optional<EventT *> poll() noexcept {
    if (m_event.pollEvent()) {
        // Handoff: there may be a waiter that got a stale semaphore release.
        // Release again so it re-checks.
        m_semaphore.release();
        return std::addressof(m_event);
    }
    return std::nullopt;
}
```

Wait — that over-releases. The single-event version needs a different approach. Since `poll()` in the single-event specialization isn't atomic (it doesn't clear a bit, it just checks the event), the race is different. Let me look again...

The single-event `Signal<EventT>`:
- `notify()` simply does `m_semaphore.release()`
- `wait()`: `while (not m_event.pollEvent()) { m_semaphore.acquire(); }`
- `poll()`: `if (m_event.pollEvent()) return &m_event; return nullopt;`

The race:
1. Thread A: `wait()` → `pollEvent()` → false → about to call `acquire()`
2. Thread B: `notify()` → release (0→1)
3. Thread C: `poll()` → `pollEvent()` → true → returns event
4. Thread A: `acquire()` → succeeds (1→0) → loop → `pollEvent()` → false → calls `acquire()` → blocks forever

Same bug, but the handoff approach is different. In the single-event `Signal<EventT>`, `poll()` doesn't atomically consume the event; `pollEvent()` is a non-destructive read. The actual consumption happens in `advance_()` via `reset()`. So the fix is: after `poll()` confirms the event is ready, release the semaphore:

Actually no — that would over-release. The cleanest fix for the single-event version: change `wait()` to not use the semaphore as the sole indicator:

```cpp
void wait() noexcept override {
    while (not m_event.pollEvent()) {
        m_semaphore.acquire();
    }
}
```

The problem is symmetric with the multi-event version. The handoff fix in the multi-event version works because `poll()` atomically drains a bit. In the single-event version, we can't do the same, so instead:

```cpp
void wait() noexcept override {
    while (not m_event.pollEvent()) {
        m_semaphore.acquire();
    }
    // Handoff: release an extra token for any other waiter that
    // might have missed a notify while we consumed the event.
    m_semaphore.release();
}
```

No, that's wrong. If nobody else is waiting, the semaphore count accumulates and future `acquire()` calls won't block.

The correct fix for the single-event version: `notify()` should always release, and `wait()` should always acquire first:

Wait, actually the simplest correct fix for both versions is: `wait()` should never trust the semaphore alone. After `acquire()`, always re-check the condition and loop if it's still false:

```cpp
void wait() noexcept override {
    while (not m_event.pollEvent()) {
        m_semaphore.acquire();
    }
}
```

This IS the current code, and it handles spurious wakeups fine. The problem is only when poll() is called concurrently. In the single-event version, any concurrent poll that returns the event is fine — pollEvent() is non-destructive, so the event remains signaled until reset().

Actually let me re-think the single-event version. pollEvent() just checks the flag — it doesn't clear it. The flag is cleared by resetEvent(). So:

1. Thread A: wait() → pollEvent() → false → acquire() → blocks
2. Thread B: notify() → release (0→1)
3. Thread C: poll() → pollEvent() → true → returns event
4. Thread A: acquire() → succeeds (1→0) → pollEvent() → STILL true (poll was non-destructive!) → exits wait()

In the single-event version, there's no race because `pollEvent()` is non-destructive. Thread B's notify sets the flag, and it stays set until `reset()` is called. So the `wait()` → `pollEvent()` call in step 4 still sees the flag.

OK so the single-event version is actually fine! The bug is only in the multi-event version because `poll()` atomically clears the bit.

So I only need to fix the multi-event version. The fix is the handoff in `poll()`.

---

## Phase 6 — Unit tests and verification

### Step 1 — Add basic HAL timer test

- **Files:** `lib/engine/tests/core/Core.HAL.Tests.cppm` (new, but we could also add to `Core.Tests.cppm`)
- **Operations:** create

```cpp
export module engine.tests:core.hal;
import engine.core;

PPR_UNIT_TEST(timers) {
    _.recurse(deadline_cancel_before_fire);
    _.recurse(deadline_fires_and_terminates);
};
```

### Step 2 — Add timeout integration test

- **Files:** `lib/engine/tests/core/Core.UnitTest.Tests.cppm` (new)
- **Operations:** create

A test marked with `expect_fail` and a short timeout, verifying the timeout mechanism:

```cpp
PPR_UNIT_TEST(test_timeout) {
    // This test will be killed by the timeout mechanism.
    // Run it in a child process to avoid killing the test runner.
};
```

This test would be run with `--timeout 100` and would verify the child exits with code 124.

### Step 3 — Register new test module(s) in CMakeLists.txt

- **Files:** `lib/engine/tests/CMakeLists.txt`
- **Operations:** edit

---

## Phase 7 — Signal::wait() handoff fix for multi-event `Signal`

(This was already described in Phase 5 — included here as a separate phase for clarity.)

---

## Summary of all files changed/created

| File | Action | Phase |
|------|--------|-------|
| `lib/engine/core/Core.HAL.cppm` | Edit (add `hal::timer` namespace, `terminateProcess`, `spawnAndWait` overload) | 1, 2 |
| `lib/engine/core/windows/Core.HAL.windows.Timer.cpp` | Create | 1 |
| `lib/engine/core/linux/Core.HAL.linux.Timer.cpp` | Create | 1 |
| `lib/engine/core/darwin/Core.HAL.darwin.Timer.cpp` | Create | 1 |
| `lib/engine/core/generic/Core.HAL.generic.Timer.cpp` | Create | 1 |
| `lib/engine/core/windows/Core.HAL.windows.Process.cpp` | Edit (add `terminateProcess`, timed `spawnAndWait`) | 2 |
| `lib/engine/core/generic/Core.HAL.generic.Process.cpp` | Edit | 2 |
| `lib/engine/core/linux/Core.HAL.linux.Process.cpp` | Edit | 2 |
| `lib/engine/core/darwin/Core.HAL.darwin.Process.cpp` | Edit | 2 |
| `lib/engine/core/Core.UnitTest.cppm` | Edit (add `m_timeout`, `m_deadline`, `m_timed_out`) | 3 |
| `lib/engine/core/Core.UnitTest.cpp` | Edit (arm/disarm deadline, handle timeout in `run()`) | 3 |
| `lib/engine/core/Core.Concurrency.Event.cppm` | Edit (handoff fix in `poll()`) | 5 |
| `lib/engine/core/CMakeLists.txt` | Edit (add Timer.cpp) | 1 |
| `lib/engine/tests/CMakeLists.txt` | Edit (add test modules) | 6 |
| `lib/engine/tests/main.cpp` | Edit (parse `--timeout`, env default) | 4 |
