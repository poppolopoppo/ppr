# Signals and cancellation contexts

Read the [entry skill](../SKILL.md) for ownership, publication, TLS, and
teardown constraints before using this reference.

## 5. IEvent / ISignal / Signal — Event Multiplexing

### 5.1 Core Interfaces

```cpp
class ISignal {
    virtual void notify(std::size_t event_tag) noexcept = 0;
    virtual void wait() noexcept = 0;
};

class IEvent {
    virtual TagPtr<ISignal> subscribeEvent(TagPtr<ISignal> signal) noexcept = 0;
    virtual void unsubscribeEvent(TagPtr<ISignal> signal, TagPtr<ISignal> restore) noexcept = 0;
    virtual bool pollEvent() noexcept = 0;
    virtual void resetEvent() noexcept = 0;
};
```

- **`IEvent`** — a level- or edge-triggered source. Implemented by
  `RawChannel`, `PulseEvent`, `BroadcastEvent`, `IContext`, `IoRequest`.
- **`ISignal`** — a subscriber. `Signal<EventsT...>` is the concrete
  implementation; `select()` is its deduction-guide factory.

### 5.2 Non-owning subscribe/unsubscribe — teardown order

Subscriptions never own: never destroy a signal while another thread may be
inside `emitEvent`, and never destroy an event while another thread may call
`notify` on it. Join, close, or drain the emitters first, then unsubscribe,
then destroy. Cross-event notification obeys the same rule:
`CancelContext::notify` emits the child event while the parent emits, so tear
down child-before-parent under the owner's join — the child's subscription to
the parent must be released (child destroyed or unsubscribed) before the
parent event can be destroyed.

### 5.3 Event Implementations

| Class | Behavior |
|---|---|
| `NeverEvent` | `pollEvent()` always `false`; subscribe/unsubscribe are no-ops. Stub event. |
| `PulseEvent` | Edge-triggered: `emitEvent()` sets the bit and notifies at most once until `resetEvent()`. **One subscriber** (single-slot atomic exchange). |
| `BroadcastEvent` | Same pulse semantics, **multiple subscribers** via a mutex-protected subscription list. `emitEvent()` notifies from a snapshot taken under the mutex, so unsubscribing inside `notify()` is safe. |

```cpp
PulseEvent event;
event.emitEvent();
bool fired = event.pollEvent();   // true
event.resetEvent();
bool fired2 = event.pollEvent();  // false
```

**Late subscription is level-checked, not replayed, for broadcast sources.**
`BroadcastEvent::subscribeEvent` after `emitEvent` synthesizes NO `Signal`
notification — the new subscriber simply missed the pulse. After subscribing,
always check `pollEvent()`/`error()` for level state instead of assuming a
notification will arrive. (`PulseEvent`, by contrast, re-fires into a
subscriber that arrives while the bit is set.) `CancelContext` children
created after the parent cancelled observe `error()` through parent
delegation, not `pollEvent()` on their own (never-emitted) flag — and
scheduler admission follows the same rule ([§8](scheduler-and-io.md#8-parallelscheduler--admission-on-stable-error)).

### 5.4 Signal — Multi-Event Composition

```cpp
PulseEvent timer;
IoRequest io_done;

auto signal = select(timer, io_done);  // Signal<PulseEvent, IoRequest>

if (auto event = signal.poll()) {      // non-blocking; variant of pointers
    std::visit(pP::overloaded{
        [](IoRequest *r) { process_io(r); },
        [](PulseEvent *t) { handle_timeout(t); },
    }, *event);
    signal.reset(*event);              // reset the consumed source
}

signal.wait();  // block until some source fires
```

**Constraints**: at least one event type, at most `bit_count_v<std::size_t>`
(64) sources, every source derives from `IEvent`. `Signal<EventT>` is a
specialized single-event form with a direct `pollEvent`/`reset` loop.

**Range-for over `select()` auto-resets the previously yielded event on
`++`.** The iterator resets the consumed source *before* fetching the next
pending event, then blocks in `wait()` when nothing is pending. Never rearm
between observing an event and incrementing the iterator — the increment owns
the reset. Sources that need explicit control use the rearmable sequence
`poll → handle → resetEvent → wait` directly on the event, never range-for:

```cpp
// Rearmable source handled explicitly (no range-for):
for (;;) {
    if (chan.pollEvent()) {
        drain(chan);
        chan.resetEvent();
    }
    if (ctx->error()) break;  // stable latch, not the consumable pulse (§6)
    signal.wait();
}
```

## 6. IContext / SharedContext — Cancellation Tree

`IContext : IEvent` adds `error()` and `value(user_key)`;
`SharedContext = std::shared_ptr<IContext>` is the owning handle. Factories
live in `context::` (`Core.Concurrency.Context.cppm`).

### 6.1 Background Context (Root)

```cpp
SharedContext bg = context::background();
bg->pollEvent();  // always false (NeverEvent)
bg->error();      // std::error_code{} (no error)
bg->value("key"); // std::nullopt
```

### 6.2 WithCancel / WithCancelClause — Manual Cancellation

```cpp
auto [ctx, cancel] = context::withCancel(context::background());
cancel();                 // latches operation_canceled, emits the event
ctx->pollEvent();         // true
ctx->error();             // std::errc::operation_canceled
```

Cancellation propagates **down** (child of a cancelled parent fires) and
never **up**. `cancel()` is idempotent — first cause wins via CAS. Both
`CancelFunc` and `CancelClauseFunc` hold a *weak* handle with an `isValid()`
liveness check; calling through an expired handle is a silent no-op.
`WithCancelClause` accepts an explicit cause:

```cpp
auto [ctx, cancel_] = context::withCancelClause(parent);
cancel_(std::error_code{42, std::generic_category()});
ctx->error().value() == 42;  // true
```

**An empty clause is a no-op: no event, no error.** `cancelCause` returns
before the CAS loop when the cause is empty, so `pollEvent()` stays false and
`error()` stays empty (`Core.Concurrency.Context.Tests.cpp`,
`CancelClause::empty_clause_is_noop`). This is what makes
`Application::requestExit({})` ([§9](scheduler-and-io.md#9-application-graceful-exit--latch-not-cancellation)) and `join({})` drain quietly instead of
firing cancellation for no cause.

`CancelContext::error()` delegates to the parent when no local cause is
latched, so a child observes parent cancellation through `error()` even if
its own flag never fired — the same level-state rule as §5.3.

### 6.3 WithoutCancel — Sever the Tree

```cpp
auto [parent, cancel_parent] = context::withCancel(context::background());
auto child = context::withoutCancel(parent);  // values flow, cancel severed
cancel_parent();
child->pollEvent();  // false — deliberately never subscribed to the parent
child->error();      // std::error_code{}
```

### 6.4 WithValue — Attach Data

```cpp
const SharedContext parent = context::background();
const SharedContext ctx = context::withValue(parent, "test_key", opaque::Value{42});
ctx->value("test_key");    // has_value()
ctx->value("other_key");   // std::nullopt
```

Children fall back to parent values (`withValues` attaches a whole
`opaque::Dict`). Values are immutable after construction.

### 6.5 WithDeadline and WithTimeout — Injected Timer

Pass an explicit `TimerManager &timer` — there is no default and no
singleton. `TimerManager timer{ITimerClock::steady()}` is the production
spelling from the tests:

```cpp
TimerManager timer{ITimerClock::steady()};
const TimePoint dl = timer.now() + std::chrono::seconds(60);
const SharedContext ctx = context::withDeadline(parent, dl, timer);
ctx->pollEvent() == false;  // nothing due yet

const SharedContext tctx = context::withTimeout(
    context::background(), std::chrono::milliseconds(150), timer);
// Cause variants: withDeadlineCause(parent, dl, cause, timer),
// withTimeoutCause(parent, delay, cause, timer); default cause is timed_out.
```

`withDeadline`/`withTimeout` derive from `CancelContext` and schedule through
`TimerManager::schedule`; the timer holds a `weak_ptr`, so destroying the
context before the deadline is safe. Parent cancellation overrides the
deadline, and `cancelDeadline` is `PPR_ENSURE`-guarded to fire only at/after
the date. For deterministic tests, assert setup state (as above) rather than
sleeping for a real deadline.

### 6.6 WithAfterFunc — Destruction Callback

```cpp
bool called = false;
{
    const SharedContext ctx = context::withAfterFunc(
        parent, [&called](const IContext &) noexcept { called = true; });
}
called == true;  // callback ran in the destructor
```

`AfterFunc = std23::move_only_function<void(const IContext &)>`; it runs on
cancel/destroy with the context that is going away.

### 6.7 Checking Cancellation

Prefer the stable latch over the consumable pulse: `error()` persists while
`pollEvent()` can be consumed and reset by a competing waiter (§5.4,
[§8](scheduler-and-io.md#8-parallelscheduler--admission-on-stable-error)).

```cpp
if (const std::error_code err = ctx->error()) {
    // handle cancellation with its cause
}
```
