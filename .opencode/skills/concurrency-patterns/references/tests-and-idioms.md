# Concurrency tests and composition idioms

Read the [entry skill](../SKILL.md) and the API reference selected there
before applying these examples.

## Quick API Patterns

```cpp
RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};
auto reserved = chan.producerReserve(sizeof(MyData), RawChannel::wait_if_full);
if (reserved.has_value()) {
    new (reserved->data()) MyData{...};
    chan.producerSubmit(*reserved);   // takes RecordHeader&
}

auto signal = select(channel, *ctx);  // Signal over event references
for (auto &event : signal) {          // iterator auto-resets on ++ (signals-contexts.md §5)
    std::visit(pP::overloaded{
        [&](RawChannel *c) { /* drain */ },
        [&](IContext *) { /* cancelled */ },
    }, event);
}

auto [ctx, cancel] = context::withCancel(context::background());
cancel();                             // latches operation_canceled, emits
```

## 11. Test Patterns

Tests use `PPR_UNIT_TEST(name)` in nested namespaces with `_.recurse({...})`
registration (layout details: `unit-test-updater`). Worker bodies never
assert (`PPR_TEST_ASSERT` throws) — they record into atomics and the parent
thread asserts; gates are deterministic (`std::latch`, atomic wait/notify)
with scope-guard release so a throwing assert never strands a worker.

**Channel round-trip** (from `single_threaded_send_receive`):

```cpp
RawChannel chan{static_cast<std::size_t>(hal::page_granularity)};

auto hdr = chan.producerReserve(sizeof(int));
PPR_TEST_ASSERT(hdr.has_value());
*static_cast<int *>(hdr->data()) = 42;
chan.producerSubmit(*hdr);

auto read = chan.consumerAcquire(RawChannel::peek_without_blocking);
PPR_TEST_ASSERT(read.has_value());
PPR_TEST_ASSERT(*static_cast<int *>(read->data()) == 42);
chan.consumerRelease(*read);
```

**Backpressure**: fill with `drop_if_full` until `error_full`; prove
`wait_if_full` blocks with a drainer thread (never assert between spawn and
join); close is covered by `close_idempotent`, `close_record_consumed`,
`select_close_wakes_waiting_thread`. **Select**: `select_one`,
`select_multiple`, `select_reset_rearms_pending_commit` (level-aware re-arm),
`select_concurrent_wakeup_and_drain` (drain everything per wakeup),
`select_notify_before_subscribe` (late-subscriber level check).

**Context**: `deadline_is_set` (far-future date, no waiting),
`parent_cancel_overrides_deadline`, `value_is_retrievable`,
`callback_called_on_destruction`, `clause_fires_done_with_error`,
`empty_clause_is_noop`. **Scheduler**: [§8 links](scheduler-and-io.md#8-parallelscheduler--admission-on-stable-error).
**App exit**: [§9 links](scheduler-and-io.md#9-application-graceful-exit--latch-not-cancellation).

## 13. Common Patterns and Idioms

### 13.1 Select-Based Worker

Subscribe before checking the stable context `error()` latch. A late subscriber
receives no replay of an earlier cancellation ([§5.3](signals-contexts.md#53-event-implementations)); the latch must be checked
before the first wait and again after wakes. Keep the subscription alive across
checks and waits. Do not use the event pulse as persistent cancellation state.

On channel readiness, drain with non-blocking acquires, releasing every acquired
record. Distinguish `error_empty` from `error_closed`; after consuming the close
record, leave the receive loop rather than waiting for another notification.
The select iterator owns reset; do not reset its source inside the body.

Cancellation requests shutdown; it does not close producer channels or join
workers. The owner must stop admission, settle outstanding producer reservations,
and close channels while consumers can still drain them. Join consumers only
after their terminal receive condition. Specify whether cancellation drains or
discards work before implementing the loop; no universal loop serves both policies.

### 13.2 Fan-Out with Multiple Channels

Use one `RawChannel` per consumer. Stop producer admission and settle outstanding
reservations first. If a flush barrier is required, flush while consumers remain
active; then close each channel. Consumers drain to `error_closed`, then the
owner joins them and releases storage. Never wait for consumers to observe close
before issuing close, or flush after consumers have stopped.

### 13.3 Context-Bounded Operation with Timeout

Use the owner's injected, actively advanced `TimerManager` with
`context::withTimeout`. Scheduling a deadline does not start a timer thread:
the owner must continue calling `tick()` while the operation is pending.
A local manager followed by a blocking operation on that same thread cannot
fire the timeout. Tests advance an injected clock and tick deterministically.

Observe the context's stable error result at the operation boundary. Calling
its cancellation function signals cancellation only; explicitly detach callbacks
and drain/join in-flight work before releasing the context tree and timer owner.
