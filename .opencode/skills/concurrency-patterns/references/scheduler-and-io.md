# Scheduler, application exit, and HAL I/O

Read the [entry skill](../SKILL.md) for ownership, publication, TLS, and
teardown constraints. Event/context integration also requires
[signals and contexts](signals-contexts.md).

## 8. ParallelScheduler — Admission on Stable Error

`ParallelScheduler(description, num_workers, queue_bytes_per_worker, parent,
thread_priority)` parents its lifecycle to the given context (default
`background()`). `schedule`/`broadcast` take an optional tracking `WaitGroup`
and an `EProducerPolicy` (default `wait_if_full`); void tasks report admission
as `error_code`, value tasks as `expected<future<R>, error_code>`.
`join(cause = {})` drains admitted tasks — a nonzero cause cancels first, an
empty cause drains quietly — and reports the lifecycle error. `error()` is
null-safe; `getContext()` exposes the lifecycle for observation only (it
returns `const IContext&`).

**Admission gates on the persistent `error()`, never on the consumable
`pollEvent()`.** Worker select-iterators consume-and-reset the lifecycle
pulse while draining, but the recorded cause never clears — so a pulse check
would admit work after cancellation was already acknowledged. `shouldCancel_`
checks `m_lifecycle->error()` first and keeps `pollEvent()` only as an
`operation_canceled` fallback for lifecycles that pulse without an error:

```cpp
ParallelScheduler sched{"workers", 2u, queueBytes(), parent};
cancel();  // parent cancelled with operation_canceled
sched.schedule([](const IContext &) {}, std::nullopt,
               ParallelScheduler::drop_if_full);  // == operation_canceled
sched.join();                                     // == operation_canceled
```

Admission failure touches neither the group counter nor its error slot;
success registers exactly once and the worker envelope settles the group via
`done(err)` exactly once (`WaitGroup::add/done/error/wait`, all `const`).
`parallel::forRange/forEach` compose caller-lane, group, then scheduler
error with queue-full fallback to the caller lane.

On a worker of the same scheduler, `forRange` executes inline rather than
dispatching and waiting on that pool. The synchronous helper also drains all
accepted tasks if submission unwinds with an exception, before borrowed caller
state is destroyed. Fallible jobs that need ordered outcomes or fatal-exception
priority must retain private results until that drain; the scheduler safety net
is not a substitute for their error policy. Keep profiling scopes local to the
thread executing each phase.

Regression links: `Core.Concurrency.Scheduler.Tests.cpp`
`cancel_ack_still_rejects_admission` (post-ack admission stays rejected),
`cancel_parent_drains_accepted_rejects_later` (accepted work still drains),
`empty_clause_cancellation_normalized` (empty clause admits and joins clean).

## 9. Application Graceful Exit — Latch, Not Cancellation

`Application::requestExit(clause = {})` separates exit from cancellation.
`m_exit_requested` is a dedicated latch: an empty clause raises only the
latch (no cause recorded, no event fired), so `run()` terminates the loop and
still returns success `{}`. A nonzero clause latches the cause on the
lifecycle and emits, exactly as cancellation always has.
`isExitRequested()` is latch OR latched error; the run loop terminates on
that stable state — never on the consumable pulse alone — and `run()` reports
the same stable error. `initialize()` re-arms the latch for repeated `run()`.

```cpp
app.requestExit();  // graceful: loop exits, run() returns {}
```

Callers (`game/main.cpp` auto-exit and `--profile-csv` paths,
`ColonySmoke` pixel/load100 paths, GLFW main-window close) all pass empty
and observe unchanged clean-exit behavior; `run()` consumers
(`game/main.cpp`, smoke harnesses) see `{}` on clean exit and the cause on
error exit. Regression links: `App.WindowInput.Tests.cpp`
`app_empty_request_exit_signals_exit_without_error` (signal without error),
`app_error_request_exit_latches_cause`,
`app_empty_request_exit_run_returns_success` (frame-3 `ExitAfterFramesApp`
harness), `app_error_request_exit_run_reports_cause`.

## 10. Integration with HAL I/O

`IoRequest` (final, move-disallowed, stable address while in-flight)
implements `IEvent` over an internal pulse with states 0 idle / 1 pending /
2 complete. `IoPort::open` returns `expected<IoFile, error_code>`;
`read`/`write(req, file, span<byte>, offset)` assert idle, bind, mark pending,
reset the pulse, and submit; `pollCompletions()`/`waitForCompletions()`
drain up to 64 completions per call into `complete_()` (CAS 1→2, then emit).
`resetEvent()` returns to idle for reuse; `cancel()` aborts via
`hal::io::cancelIo` and reports whether the request was in flight; the
destructor cancels pending work and verifies quiescence. `hal::io::wake(handle)`
unblocks a thread parked in `hal::io::wait()` so channel messages can be
drained alongside completions:

```cpp
while (running) {
    port.waitForCompletions();  // or pollCompletions() for non-blocking
    while (auto msg = channel.consumerAcquire(RawChannel::peek_without_blocking)) {
        process_channel_message(msg);
        channel.consumerRelease(*msg);
    }
    if (ctx->error()) break;  // stable latch, not the pulse
}
```

`port.read(req, file, buf, 0u)` takes `std::span<std::byte>`; after
completion `req.bytesTransferred()`/`req.error()` assert completed state.
Multiplex completions with other sources via `select(req, timer)` and the
explicit `poll → handle → resetEvent → wait` sequence from
[§5.4](signals-contexts.md#54-signal--multi-event-composition) — never
range-for over a source that must be rearmed manually.
