# Cross-Review: Reviewer B

## Overview

Each proposal addresses the same three sub-problems: (1) fixing the Signal
lost-wakeup, (2) adding a deadline timer mechanism, and (3) wiring it into the
test runner. They diverge sharply on how. My evaluation: **adopt Proposal C's
auto-fork strategy as the architectural core, steal Proposal A's callback type
and exit-code hygiene, and copy Proposal B's failure-mode documentation.**

---

## 1. Proposal A (Aggressive) — Strengths

- **Signals a clear intent that `terminateProcess` is `[[noreturn]]`** (line 210)
  and uses `_exit()` on POSIX (line 249).  This is the correct choice for a
  deadlock timeout: atexit handlers may themselves deadlock.
- **The handoff fix for the multi-event `Signal`** (lines 497–526) is a correct
  analysis of the race and a valid fix.  The discussion of the single-event
  variant (lines 542–607) correctly concludes that one is not affected.
- **Unit-test sketches** in Phase 6 show awareness that the feature needs
  automated verification.
- **Exit code 124** (line 328) gives the parent process a way to distinguish
  "timed out" from a generic failure.  Good hygiene.

## 1. Proposal A — Weaknesses

### Fatal: callback type mismatch (lines 343–351 vs. line 34)

The `OnTimeoutCallback_` is written as a lambda that captures `this` and `path`:
```cpp
m_deadline = hal::timer::setDeadline(..., [this, path](auto *dl) noexcept { ... });
```
But `setDeadline` expects `void(*)(Deadline*) noexcept` — a *stateless* function
pointer.  A capturing lambda is not convertible to a function pointer.  This
will not compile.  This is a **design-level bug**, not an implementation
detail.  The same issue exists for the Windows `TimerCallback_` which is also a
free function that ignores the user callback parameter.

**Root cause:** The API signature `void(*)(Deadline*)` was chosen for "platform
elegance" (line 8), but the actual use case demands a closure (access to
`RunImpl` members for diagnostics).

### SIGALRM in `spawnAndWait` (lines 280–304)

`ualarm` is a legacy BSD function removed from POSIX.1-2008; it does not exist
on macOS ≥ 12.  The `sigaction` handler lambda on line 283 is a C++ lambda
being assigned to `sa_handler` (expects `void(*)(int)`) — again, a lambda with
capture `[]` is valid only if it is captureless, which this one is (`[](int){}`
is fine), but the pattern is fragile and the `ualarm`/`SIGALRM` approach mixes
signal handling with `waitpid` in a way that is notoriously hard to get right
(SIGALRM may interrupt `waitpid` in a `SA_RESTART`-dependent way).

### No auto-fork (blind spot)

Without `fork`, a timeout in the parent process calls `terminateProcess` which
kills the *entire test runner*, not just one test.  The proposal never
discusses this.  If you run `--timeout 100 --run-test slow_test`, a timeout
kills the whole process — all subsequent tests are lost, and the parent never
reports results.  This makes the feature nearly unusable in practice.

### POSIX `cancelDeadline` race (lines 164–173)

`timer_delete` does **not** guarantee that a concurrent `SIGEV_THREAD` callback
has completed.  The proposal returns `!dl.m_fired` immediately after
`timer_delete`, but the callback may be mid-flight writing to freed memory.
Compare Proposal B which explicitly flags this and either ref-counts (line
122–127) or accepts the leak.

---

## 2. Proposal B (Conservative) — Strengths

- **Best failure-mode documentation.**  Every phase lists concrete "what if XYZ
  happens" with risk levels.  This pattern should be adopted project-wide.
- **Callback captures nothing from `RunImpl`** (lines 373–376).  Only globals
  (`std::cerr`, `terminateProcess`).  This eliminates the entire class of
  use-after-free races during cancel.
- **`spawnAndWait` polling loop** (lines 259–280) avoids signals entirely.
  No `SIGALRM`, no `ualarm`, no async-signal-safety worries.  50 ms granularity
  is adequate for a timeout feature.
- **Honest about trade-offs.**  The leak of ~24 bytes on POSIX cancel
  (lines 146–155) and the cosmetic stderr race (lines 383–393) are explicitly
  called out and justified.
- **The nested-timeout edge case** (lines 470–478) is correctly identified,
  though the analysis that "parent's timeout is strictly longer" is only true
  by convention, not enforcement.

## 2. Proposal B — Weaknesses

### `try_acquire` in `poll()` is a band-aid, not a fix

The one-line fix on lines 23–24 consumes one semaphore permit when `poll()`
clears a bit.  But `poll()` can be called N times while `wait()` is called
M times.  Over time, orphaned permits still accumulate on different interleavings:

1. `notify()` → release (count: 0→1)
2. `poll()` → CAS succeeds (count should be: 0→1→try_acquire→0) ✓
3. But if `wait()` acquires *before* `poll()` tries_acquire, the permit is
   consumed by `wait()`, `try_acquire()` fails (count is 0), and the permit is
   "used correctly" this time.

The problem: `try_acquire()` is a *best-effort drain*.  If `wait()` and
`poll()` are called concurrently on different threads and `poll()` always loses
the race (unlikely but possible under scheduler unfairness), the semaphore
count drifts upward.  The fix is *statistically* correct but not
*deterministically* correct.  Proposal C's `atomic::wait()` eliminates the
mismatch entirely.

### `exit()` instead of `_exit()` (lines 200–207)

The proposal acknowledges that `_exit()` is safer (line 204) but recommends
`exit()` anyway for "coverage flushing" (line 203).  This contradicts the
feature's purpose: *deadlock* detection.  If the deadlock holds a mutex
required by an atexit handler, `exit()` hangs too.  `_exit()` is the only safe
choice.  The coverage-flushing argument is a red herring — if you care about
coverage, don't let tests deadlock.

### No auto-fork (same blind spot as Proposal A)

The `spawnAndWait` timeout overload (Phase 5) does protect the parent from
child hangs, but this only applies to tests that already `fork`.  Tests running
in-process (the common case) still kill the whole runner.  This is never
mentioned.

### Polling granularity (line 279)

50 ms sleep means a 1000 ms timeout may take 1049 ms to detect.  For a test
runner this is fine, but the proposal should state the worst-case latency
explicitly: `timeout + 50 ms`.

---

## 3. Proposal C (Fresh perspective) — Strengths

### Auto-enable fork (lines 327–331) — the killer insight

This single design decision eliminates more complexity than anything else:
- No need for `terminateProcess` (the child kills itself with `_Exit()`)
- No need for a timed `spawnAndWait` overload
- No need for `TerminateProcess`/`SIGKILL`/platform process-kill APIs
- The parent sees a clean non-zero exit code and reports failure normally
- No risk of killing the test runner

This is the right architecture. **Should be adopted.**

### `atomic::wait()` instead of `counting_semaphore` (lines 18–126)

This is a root-cause fix rather than a patch.  `atomic::wait()`/`notify_one()`
maps directly to futex (Linux) and `WaitOnAddress` (Windows) with no
token-management problem.  The loop in `wait()` correctly rechecks the
condition (spurious wakeups are handled).  The only downside is changing a core
concurrency primitive, which requires thorough testing.

### Single cross-platform timer file (lines 169–208)

`std::jthread` is C++20, works on all three targets, and the implementation is
~20 lines.  The RAII `Deadline` class with automatic cancellation on scope exit
is elegant and correct.

### No changes to `spawnAndWait` (line 412)

The timeout is propagated as `--timeout <ms>` to the child process.
This is the minimal-edit approach and avoids touching the HAL process layer
entirely.

## 3. Proposal C — Weaknesses

### Thread-per-timer overhead (lines 183–198)

Each timed test creates a `std::jthread`.  For a suite with 1000 tests each
setting a timeout, that's 1000 thread creations and joins.  While a thread
creation is ~microseconds, it's still heavier than an OS timer callback (which
reuses a thread-pool thread).  The proposal should mention when this might
matter (constrained embedded targets, CI with tight resource limits).

### `this` capture in callback (lines 289, 307–311)

The safety argument ("jthread destructor joins, so the callback either fired or
is cancelled before `RunImpl` is destroyed") is *sound* but subtle.  It relies
on:
1. `m_deadline` being declared *after* other members that the callback uses
2. The `jthread` destructor joining (which is guaranteed by the standard)

If someone reorders members in `RunImpl`, this breaks silently.  Compare
Proposal B which captures nothing — zero risk.

### No exit-code distinction

The callback calls `_Exit(EXIT_FAILURE)` (line 297) which is usually exit code
1.  The parent can't distinguish "timed out" from any other child failure.
Proposal A's exit code 124 is better.  Consider using `_Exit(124)`.

### No test plan

Unlike Proposal A (Phase 6), Proposal C has no test coverage for the timer or
timeout integration.

### Cross-platform `std::jthread` risk

While all three targets support `std::jthread` in C++20, the `stop_token`
mechanism has had implementation bugs (notably in libstdc++ < 11).  The
proposal should at minimum document the minimum toolchain versions required.

---

## Cross-Cutting Synthesis: What to adopt from each

### Adopt from Proposal A

| Item | Reason |
|------|--------|
| `[[noreturn]] terminateProcess` with `_exit()` (lines 210, 249) | Correct for deadlock scenarios |
| Exit code 124 (line 328) | Distinguish timeout from other failures |
| Signal handoff analysis (lines 470–607) | Rigorous; single-event analysis is correct |
| Unit-test structure (Phase 6) | Feature needs tests |
| Pass `--timeout` to child processes (lines 448–466) | Necessary for fork isolation |

### Adopt from Proposal B

| Item | Reason |
|------|--------|
| Failure-mode documentation pattern | Every phase should call out "if X fails, Y happens" |
| No-RunImpl-capture callback (lines 373–376) | Eliminates use-after-free class of bugs |
| Polling `spawnAndWait` (lines 259–280) | No signals, works uniformly on POSIX |
| Edge-cases section (lines 468–494) | Puts implicit assumptions in writing |
| Risk-level annotations | Low / Medium / High per phase |

### Adopt from Proposal C

| Item | Reason |
|------|--------|
| **Auto-enable fork on timeout** (lines 327–331) | Eliminates platform-specific process-kill entirely |
| `atomic::wait()` replaces `counting_semaphore` (lines 18–126) | Root-cause Signal fix, no token-mismatch |
| **Single cross-platform `Deadline`** (lines 169–208) | Lowest maintenance, RAII, automatic cancel |
| No changes to `spawnAndWait` (line 412) | Minimises blast radius |
| Reduce CMake timeout 120→30 (lines 380–389) | Tighter global timeout catches actual hangs |

### Recommended hybrid

1. **Signal fix:** Proposal C's `atomic::wait()` — root-cause, architecturally
   clean.
2. **Timer API:** Proposal C's RAII `Deadline` with `std::jthread` — simple,
   cross-platform, RAII.
3. **Process isolation:** Proposal C's auto-fork — kills the child, not the
   runner.
4. **Callback signature:** Proposal A's per-`Deadline*` callback (but with
   `std::move_only_function`, not a raw pointer) *or* Proposal B's "capture
   nothing" approach (simpler).  Vote: B's approach — the callback only needs
   to write a fixed string and exit.  No `RunImpl` reference needed.
5. **Exit code:** Proposal A's 124 instead of `EXIT_FAILURE`.
6. **Signal fix tests:** Proposal A's test structure, adapted for the
   `atomic::wait` implementation.
7. **Documentation:** Proposal B's failure-mode analysis for every phase.

---

## Summary verdict

| Dimension | Winner | Why |
|-----------|--------|-----|
| Signal fix | C (atomic::wait) | Root-cause, not band-aid |
| Timer API | C (jthread) | Cross-platform, RAII, ~20 lines |
| Process isolation | C (auto-fork) | Eliminates killer-process problem |
| Callback type | B (no captures) | Zero use-after-free risk |
| Exit code | A (124) | Distinguishable from generic failure |
| spawnAndWait | C (unchanged) | Complexity not needed with auto-fork |
| Documentation | B (failure-modes) | Concrete, honest, per-phase |
| Tests | A (sketched) | Feature needs automated verification |

The ideal plan: **start from Proposal C, overlay Proposal A's exit-code
hygiene and test structure, and wrap in Proposal B's failure-mode documentation
discipline.**
