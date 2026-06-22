# Proposal C — Per-Test Timeout with Deadlock Detection

## Thesis

The simplest deployment has the lowest maintenance cost. Avoid platform-specific
timer primitives. Avoid modifying `spawnAndWait`. Fix the Signal bug by removing
the semaphore entirely, not patching around it.

---

## Phase 1 — Fix Signal lost-wakeup (prerequisite)

### Step 1 — Replace `counting_semaphore` with `atomic::wait`

- **Files:** `lib/engine/core/Core.Concurrency.Event.cppm`
- **Operations:** edit

In both `Signal<EventsT...>` (multi-signal) and `Signal<EventT>` (single-signal),
replace the `std::counting_semaphore<> m_semaphore` member with `std::atomic<size_t>`.

**Rationale:** The current design releases a semaphore token per newly-set bit
but never consumes tokens in `poll()`. This creates a mismatch: after N `poll()`
calls, the semaphore count exceeds the pending-bit count. A concurrent `wait()`
that acquires an orphaned token will spin forever (`m_pending == 0` → acquire →
`m_pending == 0` → loop). The iterator `advance_()` deadlocks in this scenario.
Using `atomic::wait()/notify_one()` maps directly to futex (Linux) and
`WaitOnAddress` (Windows) with no token-management problem.

**Multi-signal changes (`Signal<EventsT...>`):**

```cpp
// Remove:
std::counting_semaphore<> m_semaphore{0};

// Add:
// (m_pending<size_t> already exists — it doubles as the wait variable)

// notify:
void notify(const std::size_t event_index) noexcept override {
    if (PPR_ENSURE(event_index < sizeof...(EventsT))) [[likely]] {
        const std::size_t bit = std::size_t{1u} << event_index;
        m_pending.fetch_or(bit, std::memory_order_release);
        m_pending.notify_one();  // wake at most one waiter
    }
}

// wait:
void wait() noexcept override {
    while (m_pending.load(std::memory_order_acquire) == 0u) {
        m_pending.wait(0u, std::memory_order_relaxed);  // blocks until != 0
    }
}

// poll — unchanged, the CAS loop is correct as-is:
std::optional<Event> poll() noexcept {
    // (keep existing CAS implementation — it clears a bit atomically
    //  and never consumes a semaphore, which is now correct)
}
```

**Single-signal changes (`Signal<EventT>`):**

```cpp
// Remove:
std::counting_semaphore<> m_semaphore{0};

// notify:
void notify(const std::size_t event_tag) noexcept override {
    PPR_ASSERT(event_tag == 0u);
    m_event.m_pending.store(1u, std::memory_order_release);
    m_event.m_pending.notify_one();  // wake one waiter
}
```

Wait — `PulseEvent` uses `std::atomic<std::uintptr_t> m_signal` internally,
not a separate `m_pending`. For the single-signal case, `notify()` must set a
flag on the underlying event. But the single-signal `Signal<EventT>` currently
just does `m_semaphore.release()` — it doesn't interact with the event's state.
That's because `wait()` calls `m_event.pollEvent()` in the loop:

```cpp
void wait() noexcept override {
    while (not m_event.pollEvent()) {
        m_semaphore.acquire();
    }
}
```

For the single-signal case, we need a different approach. The event state is in
`m_event` (the underlying `PulseEvent`/etc). The semaphore just provides the
wakeup. We can replace with:

```cpp
void wait() noexcept override {
    while (not m_event.pollEvent()) {
        m_event.waitEvent();  // new: block until event is signaled
    }
}
```

This requires adding `waitEvent()` to `IEvent` as a blocking variant of `pollEvent()`.
But that changes the interface. Simpler: use a dedicated atomic per signal.

**Simplest fix for single-signal:** Replace `m_semaphore` with a `std::atomic<bool>`:

```cpp
std::atomic<bool> m_notified{false};

void notify(const std::size_t) noexcept override {
    m_notified.store(true, std::memory_order_release);
    m_notified.notify_one();
}

void wait() noexcept override {
    while (not m_event.pollEvent()) {
        m_notified.wait(false, std::memory_order_relaxed);
        m_notified.store(false, std::memory_order_relaxed);
    }
}

// poll — unchanged
```

**Note:** For `PulseEvent` and `BroadcastEvent`, add `m_pending` atomic that
`notify_one()` wakes on, shared between the event and the signal. This is an
internal detail — the key is removing the counting semaphore.

---

## Phase 2 — Add `hal::timer::Deadline` (cross-platform, one file)

### Step 1 — Declare `hal::timer` in `Core.HAL.cppm`

- **Files:** `lib/engine/core/Core.HAL.cppm`
- **Operations:** edit

Add after the `process` namespace (line 352):

```cpp
// ------------------------------------------------------------------
// deadline timer (RAII, fires callback once after duration)
// ------------------------------------------------------------------

namespace timer {
    class Deadline {
        std::jthread m_thread;
    public:
        using Callback = std::move_only_function<void() noexcept>;

        explicit Deadline(std::chrono::milliseconds timeout, Callback callback) noexcept;
        ~Deadline() noexcept = default;  // jthread join cancels
        Deadline(const Deadline&) = delete;
        Deadline& operator=(const Deadline&) = delete;
        Deadline(Deadline&&) noexcept = default;
        Deadline& operator=(Deadline&&) noexcept = default;
    };

    [[nodiscard]] Deadline createDeadlineTimer(
        std::chrono::milliseconds timeout,
        Deadline::Callback callback) noexcept;
}
```

**Rationale:** `std::jthread` is C++20, works on all three target platforms,
requires zero platform-specific code. The destructor requests stop and joins,
making cancellation automatic on scope exit. Thread creation is negligible for
a test-timeout use case (seconds-long deadlines).

### Step 2 — Implement `hal::timer` (single cross-platform file)

- **Files:** `lib/engine/core/Core.HAL.Timer.cpp` (new)
- **Operations:** create

```cpp
module;
#include "pP/Macros.h"
module engine.core;
import :hal;
import std;

namespace pP::hal::timer {

Deadline::Deadline(std::chrono::milliseconds timeout, Callback callback) noexcept {
    m_thread = std::jthread([timeout, cb = std::move(callback)](std::stop_token st) mutable {
        if (st.wait_for(timeout) == std::future_status::timeout) {
            cb();
        }
        // else: cancelled before timeout → do nothing
    });
}

Deadline createDeadlineTimer(
    const std::chrono::milliseconds timeout,
    Deadline::Callback callback) noexcept {
    return Deadline(timeout, std::move(callback));
}

}
```

**Why one file instead of four per-platform files:** The `std::jthread`-based
implementation is correct on Windows, Linux, and Darwin. Platform-specific
timer APIs (`CreateThreadpoolTimer`, `timer_create`, `dispatch_source_t`) add
complexity and maintenance burden with no benefit for a deadline timer whose
granularity is milliseconds and whose sole consumer is test timeouts. If a
future use case needs zero-overhead high-frequency timers, add per-platform
files then.

### Step 3 — Register the new file in CMakeLists.txt

- **Files:** `lib/engine/core/CMakeLists.txt`
- **Operations:** edit

Add `Core.HAL.Timer.cpp` to the PRIVATE source list (after the HAL_PLATFORM_SOURCES
section, around line 74):

```cmake
    ${HAL_PLATFORM_SOURCES}
    Core.HAL.Timer.cpp
```

---

## Phase 3 — Add timeout to unit test framework

### Step 1 — Add `m_timeout` field to `Context`

- **Files:** `lib/engine/core/Core.UnitTest.cppm`
- **Operations:** edit

Add to `struct Context` (after line 131):

```cpp
std::optional<std::chrono::milliseconds> m_timeout{};
```

### Step 2 — Propagate timeout to child process

- **Files:** `lib/engine/core/Core.UnitTest.cpp`
- **Operations:** edit

Modify `startInChildProcess_` to forward the timeout argument:

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
        child_args.push_back(std::to_string(run.m_context.m_timeout->count()));
    }

    const int exit_code = hal::process::spawnAndWait(exe_path, child_args);
    return (exit_code == 0);
}
```

### Step 3 — Add timeout guard in `RunImpl::start()` / `~RunImpl()`

- **Files:** `lib/engine/core/Core.UnitTest.cpp` (and potentially `.cppm`)
- **Operations:** edit

Add a `m_deadline` member to `RunImpl`:

```cpp
// In Core.UnitTest.cppm, struct RunImpl (line 175):
std::optional<hal::timer::Deadline> m_deadline{};
```

In `Core.UnitTest.cpp`, modify `RunImpl::start()`:

```cpp
void UnitTest::RunImpl::start() noexcept {
#if PPR_ENABLE_ASSERTIONS
    Assertion::Policy new_policy(std23::nontype<&RunImpl::onAssertFailure>, this);
    m_prev_assert_policy = Assertion::setFailurePolicy(std::move(new_policy));
#endif

    m_start_time = std::chrono::steady_clock::now();

    if (m_context.m_timeout.has_value()) {
        m_deadline.emplace(*m_context.m_timeout, [this]() noexcept {
            // The test hung. The deadline fires on a background thread.
            // If this is a child process, kill ourselves cleanly.
            // In a parent process, this should never fire (parent is waiting).
            std::cerr << "TIMEOUT: test " << currentPath()
                      << " exceeded " << m_context.m_timeout->count() << "ms\n";
            std::fflush(stderr);
            // _Exit avoids static destructors, matching the child-process pattern
            ::_Exit(EXIT_FAILURE);
        });
    }
}
```

The `~RunImpl()` destructor cleans up naturally: when `m_deadline` is destroyed
(the `jthread`'s destructor requests stop and joins), the background thread
will not fire the callback if the test completed on time.

**Important:** The deadline callback captures `this` (the `RunImpl`). This is
safe because `m_deadline` is a member of `RunImpl`, so it is destroyed *before*
`RunImpl`'s other members. The join in `jthread`'s destructor ensures the
callback either fired (during the test) or was cancelled (after `start()`).
By the time `RunImpl` is destroyed, the thread is joined.

### Step 4 — Auto-enable `fork` when timeout is set and test is not forked

- **Files:** `lib/engine/core/Core.UnitTest.cpp`
- **Operations:** edit

In `UnitTest::run(IRun&)`, before the fork check:

```cpp
void UnitTest::run(IRun &run) const noexcept {
    try {
        auto &impl = checked_cast<RunImpl>(run);
        impl.start();

        // Determine effective flags: auto-fork when timeout is set
        EFlags effective_flags = m_flags;
        if ((effective_flags & fork) == none &&
            impl.m_context.m_timeout.has_value()) {
            effective_flags = static_cast<EFlags>(effective_flags | fork);
        }

        if ((effective_flags & fork) == none) [[likely]] {
            // ... existing in-process path ...
        } else {
            // ... existing fork path (unchanged) ...
        }
    }
}
```

**Rationale:** A timeout kill inside the parent process would terminate the
entire test runner. By auto-enabling `fork`, the test runs in a child process
where the deadline's `_Exit()` only kills the child. The parent sees a non-zero
exit code and reports a failure. No `TerminateProcess`/`SIGKILL` needed.

---

## Phase 4 — Add `--timeout` CLI flag

### Step 1 — Parse `--timeout <ms>` in tests/main.cpp

- **Files:** `lib/engine/tests/main.cpp`
- **Operations:** edit

Add after the `--loop` handler (line 44):

```cpp
} else if (arg == "--timeout" && i + 1 < argc) {
    unsigned long ms = std::stoul(argv[++i]);
    context.m_timeout = std::chrono::milliseconds(ms);
```

### Step 2 — Update help text

- **Files:** `lib/engine/tests/main.cpp`
- **Operations:** edit

Update the `--help` output (line 46-57) to include:

```
  --timeout <ms>     Kill test after N milliseconds (auto-enables forking)
```

### Step 3 — Update CMake test properties

- **Files:** `lib/engine/tests/CMakeLists.txt`
- **Operations:** edit

Reduce the global `TIMEOUT` from 120 to 30 (tests should be fast; the per-test
`--timeout` catches individual hangs):

```cmake
set_tests_properties(EngineUnitTests
    PROPERTIES
        TIMEOUT 30
        LABELS "unit;engine"
)
```

---

## Summary of changes

| File | Change |
|------|--------|
| `Core.Concurrency.Event.cppm` | Replace `counting_semaphore` with `atomic::wait/notify` |
| `Core.HAL.cppm` | Declare `hal::timer::Deadline` |
| `Core.HAL.Timer.cpp` | New: `std::jthread`-based deadline |
| `Core.UnitTest.cppm` | Add `m_timeout` to `Context`, `m_deadline` to `RunImpl` |
| `Core.UnitTest.cpp` | Auto-fork on timeout, pass `--timeout` to child, install deadline |
| `tests/main.cpp` | Parse `--timeout <ms>`, update help |
| `tests/CMakeLists.txt` | Reduce global timeout to 30s |
| `CMakeLists.txt` (core) | Register `Core.HAL.Timer.cpp` |

## What this avoids

- **No `TerminateProcess`/`SIGKILL`/platform process-kill APIs** — the child
  kills itself via `_Exit()` when the deadline fires.
- **No modification to `spawnAndWait`** — the timeout is propagated as a CLI
  argument, not as an argument to the spawn function.
- **No per-platform timer files** — `std::jthread` works everywhere.
- **No `void*` opaque handles** — `Deadline` is an RAII class.
- **No `try_acquire` race-window patching** — `atomic::wait` eliminates the
  semaphore-mismatch problem entirely.
