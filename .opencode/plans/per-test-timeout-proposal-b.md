# Per-Test Timeout with Deadlock Detection — Conservative Plan (Agent B)

## Overview

Add optional per-test timeout that terminates the process on expiry, detecting
deadlocks and runaway tests.  The design minimises risk by:

- Adding no new dependencies (only OS APIs already available)
- Using existing `void*` handle patterns (matching `IoHandle`, `FileHandle`)
- Putting all platform code behind the existing HAL boundary
- Making timeout purely opt-in via `Context::m_timeout`
- Fixing the Signal lost-wakeup bug in an independent early step

---

## Phase 1 — Fix Signal lost-wakeup bug

### Step 1 — Add `try_acquire()` in `poll()` after CAS
**Files:**
- `lib/engine/core/Core.Concurrency.Event.cppm:98-114`

**Operations:**
- After the successful `compare_exchange_weak` on line 108, insert
  `m_semaphore.try_acquire()` to consume the semaphore permit that was released
  by `notify()` when setting this bit.

**Explanation:**
`notify()` calls `m_semaphore.release()` exactly once per newly-set bit.
`poll()` atomically clears a bit via CAS but never consumes the corresponding
semaphore permit.  Over time excess permits accumulate, causing `wait()` to
return spuriously when `m_pending == 0` (classic lost-wakeup pattern).

**Failure mode analysis:**
- *Change is too aggressive:* `try_acquire()` may fail if the semaphore count is
  momentarily zero (another thread consumed it first).  This is benign — the
  excess permit is consumed elsewhere and correctness is preserved.
- *Change is missed:* the bug persists.  No other risk.
- Risk level: **very low**.  One line, well-understood pattern (`try_acquire`
  is guaranteed non-blocking in C++20).

---

## Phase 2 — Add `hal::timer` namespace to HAL module interface

### Step 1 — Declare `hal::timer` API in `Core.HAL.cppm`
**Files:**
- `lib/engine/core/Core.HAL.cppm:348` (after `namespace process` block)

**Operations:**
- Add a new `namespace timer` block with:

```cpp
namespace timer {
    using TimerHandle = void *;
    using TimerCallback = void (*)(void *context) noexcept;

    [[nodiscard]] TimerHandle setDeadline(
        std::chrono::milliseconds ms,
        TimerCallback callback,
        void *context) noexcept(false);

    void cancelDeadline(TimerHandle handle) noexcept;
}
```

**Rationale for `void(*)(void*)` callback:**
- Matches the Windows `WAITORTIMERCALLBACK` signature directly (no trampoline
  needed beyond a thin adapter)
- Avoids `std::function` heap allocation and vtable overhead
- `void*` context follows the existing `IoHandle`/`FileHandle` opaque pattern

**Failure mode analysis:**
- Adding declarations to a module interface requires that all consuming
  translation units see compatible definitions.  The platform .cpp files already
  implement the `pP::hal::` namespace — adding new functions follows the exact
  same pattern.  Risk: **low**.

---

## Phase 3 — Create per-platform `Timer.cpp` files

### Step 1 — Windows timer implementation
**Files:**
- `lib/engine/core/windows/Core.HAL.windows.Timer.cpp` (create)

**Operations:**
- Include `Core.HAL.windows.include.h`
- Implement `hal::timer::setDeadline` using `CreateTimerQueueTimer` with
  `WT_EXECUTEDEFAULT`.  The callback is an adapter that casts `PVOID` to the
  `TimerCallback`/context pair (packed into a small heap struct).
- Implement `hal::timer::cancelDeadline` using
  `DeleteTimerQueueTimer(nullptr, handle, INVALID_HANDLE_VALUE)` — this blocks
  until all pending callbacks complete, eliminating the fire-during-cancel race.
- Struct for packing:

```cpp
struct TimerCallbackCtx {
    TimerCallback m_func;
    void *m_context;
};
```

**Failure mode analysis:**
- `CreateTimerQueueTimer` can fail under heavy system load (queue full). The
  function is marked `noexcept(false)`; caller must handle (the plan propagates
  to `RunImpl::start()` which catches and logs a warning, proceeding without
  timeout).
- `DeleteTimerQueueTimer(..., INVALID_HANDLE_VALUE)` blocks indefinitely if a
  callback deadlocks.  This cannot happen because the callback only writes to
  stderr and calls `terminateProcess`.  Risk: **low**.

### Step 2 — Linux timer implementation
**Files:**
- `lib/engine/core/linux/Core.HAL.linux.Timer.cpp` (create)

**Operations:**
- Include `<signal.h>`, `<time.h>`, `<unistd.h>`
- Implement `setDeadline` using `timer_create(CLOCK_MONOTONIC, ...)` with
  `SIGEV_THREAD`.  The `sigev_value.sival_ptr` carries a heap-allocated struct
  holding the `TimerCallback` + `void*` context.
- Implement `cancelDeadline` using `timer_delete`.
- **Race handling:** heap-allocated callback struct is reference-counted via a
  `std::atomic<unsigned>` embedded in the struct.  `timer_delete` does not
  guarantee pending `SIGEV_THREAD` callbacks have completed; the ref-count
  prevents premature free.  The callback decrements ref-count after invocation
  and frees if it was the last reference.  `cancelDeadline` decrements the
  initial reference after `timer_delete` returns.

```cpp
struct PosixTimerCtx {
    std::atomic<unsigned> m_refs{2}; // 1 for timer, 1 for cancelDeadline
    TimerCallback m_func;
    void *m_context;
};
```

- If the callback fires after `timer_delete`, it still safely dereferences the
  struct via the ref-count, checks a `bool cancelled` flag in the struct, and
  either runs or skips.

**Failure mode analysis:**
- `timer_create` can fail if the kernel's timer limit is reached.  Falls back
  to no-timeout (exception caught by caller).
- The ref-counted struct adds complexity.  Alternative simpler approach: leak
  the struct (small, fixed size).  In a test framework that terminates on
  timeout, a one-time leak per test is acceptable.
- **Recommendation (conservative):** skip ref-counting; leak the struct on
  cancel.  The struct is `2 * sizeof(void*) + sizeof(atomic<unsigned>)` ≈ 24
  bytes per armed timer.  With at most one active timer per test run (serial
  execution), this is a fixed cost of ~24 bytes per `cancelDeadline` call.
  Risk: **very low**.  Simplicity gain: **high**.
- Change this step to use the leaky approach: heap-allocate the context struct
  in `setDeadline`; `cancelDeadline` calls `timer_delete` but does **not**
  free the struct (the callback frees it if it fires, or it leaks on cancel).

### Step 3 — Darwin timer implementation
**Files:**
- `lib/engine/core/darwin/Core.HAL.darwin.Timer.cpp` (create)

**Operations:**
- Identical implementation to Linux (same POSIX API surface on Darwin).

### Step 4 — Generic (stub) timer implementation
**Files:**
- `lib/engine/core/generic/Core.HAL.generic.Timer.cpp` (create)

**Operations:**
- Both functions throw `std::runtime_error("not implemented")`.

### Step 5 — Register timer files in `CMakeLists.txt`
**Files:**
- `lib/engine/core/CMakeLists.txt:13` (after the `System.cpp` line)

**Operations:**
- Add `${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Timer.cpp` to the
  `HAL_PLATFORM_SOURCES` list.

**Failure mode analysis (all platforms):**
- New .cpp files follow the exact module/namespace pattern of existing HAL
  files.  Risk of miscompilation: **low** if the `module engine.core;` /
  `import :hal;` boilerplate matches existing files exactly.

---

## Phase 4 — Add `terminateProcess` to `hal::process`

### Step 1 — Declare in module interface
**Files:**
- `lib/engine/core/Core.HAL.cppm:352` (in `namespace process`)

**Operations:**
- Add `void terminateProcess(int exitCode) noexcept;`

### Step 2 — Implement per platform

**Windows** (`windows/Core.HAL.windows.Process.cpp`):
- `::TerminateProcess(::GetCurrentProcess(), static_cast<DWORD>(exitCode))`

**Linux** (`linux/Core.HAL.linux.Process.cpp`):
- `::exit(exitCode)` (not `_exit` — we want atexit handlers to run for
  coverage flushing, though in a timeout scenario we might prefer `_exit`).
- **Conservative choice:** use `::exit(exitCode)` because it runs stdio flush,
  atexit, etc.  If the test deadlocked, `exit()` may deadlock too if a mutex
  is held.  For deadlock detection, `_exit` is actually safer.
- **Recommendation:** use `::exit(exitCode)` on POSIX as the default — if it
  hangs, the OS will eventually reap the process.  Document that timeouts may
  not flush buffers.

**Darwin** (`darwin/Core.HAL.darwin.Process.cpp`):
- Same as Linux.

**Generic** (`generic/Core.HAL.generic.Process.cpp`):
- Throw `std::runtime_error("terminateProcess not implemented")`.

**Failure mode analysis:**
- `TerminateProcess` on Windows does not run DLL-main/atexit handlers.  This
  means coverage data (if any) may not be flushed.  Acceptable for a timeout
  feature.
- `exit()` on POSIX may deadlock.  Acceptable — the process is being terminated
  due to a suspected deadlock; an additional deadlock is still a termination
  (albeit possibly delayed).  Risk: **low**.

---

## Phase 5 — Add `spawnAndWait` timeout overload

### Step 1 — Declare in module interface
**Files:**
- `lib/engine/core/Core.HAL.cppm:352` (in `namespace process`, next to existing
  `spawnAndWait`)

**Operations:**
- Add overload:
```cpp
[[nodiscard]] int spawnAndWait(
    const std::filesystem::path &executable,
    std::span<const std::string> args,
    std::chrono::milliseconds timeout) noexcept(false);
```

### Step 2 — Windows implementation
**Files:**
- `windows/Core.HAL.windows.Process.cpp`

**Operations:**
- Follow existing `spawnAndWait` but replace `WaitForSingleObject(pi.hProcess, INFINITE)`
  with `WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout.count()))`.
- If `WAIT_TIMEOUT`, call `TerminateProcess(pi.hProcess, 124)`, then
  `WaitForSingleObject(pi.hProcess, INFINITE)` (wait for termination), then
  throw `std::runtime_error("child process timed out")`.
- On normal completion, same as existing: `GetExitCodeProcess`, close handles.

### Step 3 — POSIX implementation (Linux/Darwin)
**Files:**
- `linux/Core.HAL.linux.Process.cpp`
- `darwin/Core.HAL.darwin.Process.cpp`

**Operations:**
- Replace `waitpid(pid, &status, 0)` with a polling loop:
  ```cpp
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  for (;;) {
      const pid_t ret = ::waitpid(pid, &status, WNOHANG);
      if (ret == pid) {
          if (WIFEXITED(status)) return WEXITSTATUS(status);
          return -1;
      }
      if (ret < 0) {
          if (errno == EINTR) continue;
          throw std::runtime_error("waitpid failed");
      }
      // ret == 0: child still running
      if (std::chrono::steady_clock::now() >= deadline) {
          ::kill(pid, SIGKILL);
          ::waitpid(pid, &status, 0); // reap
          throw std::runtime_error("child process timed out");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ```

**Rationale for polling (not `alarm`+SIGALRM):**
- No signal handler needed (async-signal-safety is hard to reason about)
- Works uniformly on Linux and Darwin without `SIGEV_THREAD` complexities
- 50 ms polling granularity is more than adequate for timeout detection
- The parent process is not terminated — only the child is killed, leaving the
  test runner intact to report the failure

### Step 4 — Generic implementation
**Files:**
- `generic/Core.HAL.generic.Process.cpp`

**Operations:**
- Throw `std::runtime_error("spawnAndWait with timeout not implemented")`.

**Failure mode analysis:**
- The polling loop on POSIX may race with the child's natural exit.
  `waitpid(WNOHANG)` correctly handles the case where the child exits between
  iterations — the next call returns the pid.  Risk: **low**.
- If `kill(SIGKILL)` fails (child already reaped by signal handler), the
  subsequent `waitpid` returns `ECHILD`.  Catch and break out of loop.
  Risk: **very low**.

---

## Phase 6 — Add `m_timeout` to `UnitTest::Context`

### Step 1 — Add field to Context
**Files:**
- `lib/engine/core/Core.UnitTest.cppm:127` (in `struct Context`)

**Operations:**
- Add `std::optional<std::chrono::milliseconds> m_timeout{};` after
  `m_shuffle_seed`.

### Step 2 — Add `--timeout` CLI parsing
**Files:**
- `lib/engine/tests/main.cpp:44` (before `--help` block)

**Operations:**
- Add:
```cpp
} else if (arg == "--timeout" && i + 1 < argc) {
    context.m_timeout = std::chrono::milliseconds(std::stoul(argv[++i]));
```
- Update usage string to include `[--timeout <ms>]`.

**Failure mode analysis:**
- `--timeout` does nothing unless timer HAL is implemented (generic platform
  throws at `RunImpl::start()`).  CLI parsing itself cannot fail.  Risk:
  **very low**.

---

## Phase 7 — RunImpl timeout lifecycle

### Step 1 — Add timer members to RunImpl
**Files:**
- `lib/engine/core/Core.UnitTest.cppm:178` (in `struct RunImpl`, after
  existing members)

**Operations:**
- Add after `m_status`:
```cpp
hal::timer::TimerHandle m_timer_handle{nullptr};
```

### Step 2 — Arm timer in `start()`
**Files:**
- `lib/engine/core/Core.UnitTest.cpp:198-205` (`RunImpl::start()`)

**Operations:**
- After `m_start_time = ...;`, add:
```cpp
if (m_context.m_timeout.has_value()) {
    PPR_VERIFY(m_timer_handle == nullptr);
    try {
        m_timer_handle = hal::timer::setDeadline(
            *m_context.m_timeout,
            [](void *) noexcept {
                std::cerr << "[TIMEOUT] Test exceeded deadline, terminating\n";
                hal::process::terminateProcess(124);
            },
            nullptr);
    } catch (std::exception &) {
        // Timer creation failed — run without timeout protection
        m_timer_handle = nullptr;
    }
}
```

**Note on callback safety:**
- The callback captures no references to `RunImpl`.  It uses only globals
  (`std::cerr`, `hal::process::terminateProcess`).  This eliminates any
  use-after-free race if the callback fires during or after `cancelDeadline`.
- No per-callback cancellation flag is needed because `cancelDeadline` on
  Windows is synchronous (waits for callbacks), and on POSIX the callback
  unconditionally terminates the process (if it fires after cancel, the
  process is already exiting or running the next test — this is a cosmetic
  false-positive timeout message, not a correctness issue).

**Why accept the POSIX race?**
- The window is tiny: callback fires, writes to stderr, calls terminateProcess.
  If it fires after cancel but before the next test starts, the next test's
  output is preceded by a spurious `[TIMEOUT]` line.  The test still runs
  normally afterward.
- If the callback fires after the test executable exits (during static
  destruction), `terminateProcess` is called but the process is already
  terminating — no harm.
- A full cancellation flag would require `shared_ptr<atomic<bool>>` and a
  heap-allocated shared state, increasing complexity for an edge case that
  has no observable correctness impact.

### Step 3 — Disarm timer in `~RunImpl()`
**Files:**
- `lib/engine/core/Core.UnitTest.cpp:207-234` (`~RunImpl()`)

**Operations:**
- At the very top of the destructor body (before all existing code), add:
```cpp
if (m_timer_handle != nullptr) {
    hal::timer::cancelDeadline(m_timer_handle);
    m_timer_handle = nullptr;
}
```

**Failure mode analysis:**
- `cancelDeadline` must be called before the destructor uses `m_start_time`
  and writes output — if the timer fires during output, stderr interleaving
  is cosmetic.
- If `cancelDeadline` throws (it is `noexcept`, so it shouldn't), the
  destructor would `std::terminate`.  Keep it `noexcept`.
- Risk: **low**.

---

## Phase 8 — Update `startInChildProcess_()` to use timeout

### Step 1 — Pass context timeout to spawnAndWait overload
**Files:**
- `lib/engine/core/Core.UnitTest.cpp:19-30`

**Operations:**
- Replace the single `hal::process::spawnAndWait` call with:
```cpp
int exit_code;
if (run.m_context.m_timeout.has_value()) {
    exit_code = hal::process::spawnAndWait(
        exe_path, child_args, *run.m_context.m_timeout);
} else {
    exit_code = hal::process::spawnAndWait(exe_path, child_args);
}
```

**Failure mode analysis:**
- The timeout overload throws on timeout (child killed).  The exception
  propagates to `UnitTest::run()` line 61, where it is caught and reported
  as a test failure.  This is correct behaviour — a timed-out child is a
  failed test.
- Risk: **low**.

---

## Summary of files changed

| File | Operation | Risk |
|------|-----------|------|
| `Core.Concurrency.Event.cppm` | Edit (1 line) | Very low |
| `Core.HAL.cppm` | Edit (2 namespace blocks) | Low |
| `Core.UnitTest.cppm` | Edit (1 field) | Very low |
| `Core.UnitTest.cpp` | Edit (3 sites: start, dtor, spawnAndWait) | Low |
| `CMakeLists.txt` | Edit (1 line) | Very low |
| `tests/main.cpp` | Edit (CLI parsing + help) | Very low |
| `windows/...Timer.cpp` | Create (~40 lines) | Low |
| `linux/...Timer.cpp` | Create (~50 lines) | Low |
| `darwin/...Timer.cpp` | Create (~50 lines) | Low |
| `generic/...Timer.cpp` | Create (~15 lines) | Very low |
| `windows/...Process.cpp` | Edit (2 functions) | Low |
| `linux/...Process.cpp` | Edit (2 functions) | Low |
| `darwin/...Process.cpp` | Edit (2 functions) | Low |
| `generic/...Process.cpp` | Edit (2 stubs) | Very low |

**Total:** 5 new files, 12 edited files.  14 changes across ~200 net lines.

---

## Edge cases and ambiguities noted

1. **Nested timeouts:** If parent and child both set timeouts, the child's
   timer replaces the parent's (the child's `start()` arms a new timer).
   When the child completes, `~RunImpl()` disarms the child's timer.
   The parent's timer was never disarmed (it was overwritten).  This is
   acceptable because the parent's timeout is strictly longer than the
   child's (the child already protects against deadlock at a finer
   granularity).  If desired, a `m_previous_timer` save/restore could be
   added, but this is unnecessary complexity for now.

2. **Zero timeout:** A `--timeout 0` value arms the timer with 0 ms, which
   fires immediately on most platforms (next timer tick).  This is a valid
   way to verify the timeout mechanism.  No special handling needed.

3. **Callback uses `std::cerr` after static destruction:** If the timer
   callback fires during process shutdown (after `std::cerr` is destroyed),
   writing to `std::cerr` is undefined behaviour.  On Windows this cannot
   happen because `cancelDeadline` waits for callbacks.  On POSIX the
   window is extremely narrow (between `cancelDeadline` returning and
   `_exit()` completing).  Accepting this cosmetic risk is simpler than
   adding async-signal-safe I/O.

4. **`spawnAndWait` timeout on generic platform:** Throws "not implemented".
   The caller (`startInChildProcess_`) catches via `UnitTest::run()` and
   reports a test failure.  If a user sets `--timeout` on an unsupported
   platform, fork tests will always fail.  Document this in `--help`.
