# Logging and error propagation

[AGENTS.md](../AGENTS.md#errors-and-operational-boundaries) defines mandatory
usage. [Macros.h](../include/pP/Macros.h) defines the public macro surface;
[Core.Logger.cppm](../lib/engine/core/Core.Logger.cppm) and
[Core.Logger.cpp](../lib/engine/core/Core.Logger.cpp) define the custom logger.
The [header map](../include/pP/codemap.md) covers the other macro families:
build detection, compiler attributes, assertions, text literals, and RAII.

## Categories and structured messages

Include `pP/Macros.h` in a module's global fragment and import `engine.core`
where its types are needed. Declare a shared category with
`PPR_DECLARE_LOG_CATEGORY(Assets)` in the appropriate shared module scope;
define it once with `PPR_DEFINE_LOG_CATEGORY(Assets, debug, none)` in the same
enclosing namespace. Definitions need no trailing semicolon. Category macros
create a `details::log::Assets()` accessor; call sites must resolve that namespace.

```cpp
PPR_DEFINE_LOG_CATEGORY(Assets, debug, none)

// In a function:
PPR_LOG(Assets, info, "asset loaded", {{"path", path}, {"bytes", byte_count}});
PPR_LOG_RAW(Assets, warning, dynamic_message, {{"path", path}});
```

`PPR_LOG(category, level, message, optional_fields)` takes a literal message
(`string_literal`) and optional `opaque::Dict` structured fields, rather than a
format string with positional arguments. `PPR_LOG_RAW` copies a runtime
`std::string_view` message into the queue. Fields are serialized into the queued
entry. Levels are bare lowercase tokens, ordered `debug`, `verbose`, `info`,
`emphasis`, `warning`, `error`, `fatal`. Logging at `fatal` does not itself exit
or throw.

The category's minimum verbosity and the global minimum set by
`Log::setMinimumVerboseLevel` both filter runtime visibility. The setter returns
the previous minimum. Category flags are `none`, `immediate`, `break_on_error`,
`break_on_warning`, and `all`. Break flags apply at the named severity and above
and can trigger a debugger breakpoint even when runtime visibility rejects an
entry. `immediate` flushes stdout when the consumer writes the entry; it does
not make the producer synchronous.

## Filtering, queueing, and flushing

CMake's `PPR_LOGGING_LEVEL` supplies numeric `PPR_LOG_LEVEL_THRESHOLD`: `DEBUG`
through `FATAL` map to 0 through 6; `OFF` is 7. The header defaults to `WARNING`
when no threshold is supplied. Below-threshold macro calls erase their message
and field expressions without evaluating them. At `OFF`, category declarations,
definitions, logging, and `PPR_FLUSH_LOG` are stubs. Error propagation still runs.
Do not put required side effects in logging arguments.

Compiled-in calls check runtime visibility before evaluating the message and
fields, except when a category requests a debugger break. Direct `Log::log`
and `Log::logRaw` calls do not provide the macro's compile-time erasure or
caller-side argument gate.

`PPR_LOG_ONCE(category, level, message, fields)` claims a process-wide file/line
key. `PPR_LOG_ONCE_KEY(category, level, key, message, fields)` uses an explicit
`u64` key; `Log::Once::combine` combines keys. Compiled-out calls erase the claim
too. Compiled-in calls claim before runtime filtering, so a filtered first call
still consumes the claim. This is first occurrence suppression, not a rate limiter.

The logger drains a bounded `RawChannel` on a background thread. On saturation,
entries below `error` may be dropped; `error` and `fatal` wait for queue space.
The default writer prints to stdout with elapsed time, producer thread,
severity, category, message, and fields; `error` and above add source location.
There is no built-in file sink.

`Log::flush(false)` queues a flush marker without waiting and can fail when the
queue is full. `Log::flush(true)` waits for queue space and draining; both return
`std::error_code`. Check that result when flushing is required for correctness.
`PPR_FLUSH_LOG(...)` discards the result and is appropriate only for best-effort
flushing; it does nothing at `OFF`.

`Log::setWriterPolicy` installs a non-owning `function_ref` callback and returns
the previous policy. Callbacks run on the consumer thread; entry views are valid
only during the callback. Keep the callback target alive until its policy has
been replaced, and drain pending entries before replacing a policy when they
must reach that writer. Avoid recursive logging or synchronous flushes in a
writer: waiting for the same consumer can deadlock.

## Mandatory error macros

The implemented name is `PPR_RETURN_ERROR_ON_FAIL`, not
`PPR_RETURN_ERROR_IF_FAILED`. Use these macros in error-returning functions as
required by the engineering contract:

| Macro | Failure behavior |
| --- | --- |
| `PPR_RETURN_ERROR_ON_FAIL(category, expression)` | Converts with unqualified `make_error_code`, logs error details, returns the nonzero `std::error_code`. |
| `PPR_RETURN_UNEXPECTED_ON_FAIL(category, expression)` | Same conversion and logging, returns `std::unexpected{code}` for an expected result. |
| `PPR_RETURN_ON_FAIL(category, expression)` | Tests with unqualified `hasFailed`, logs the failed expression, returns the original result type. |
| `PPR_RETAIN_ERROR_ON_FAIL(category, retained, expression)` | Converts and logs each failure, assigns only if `retained` is still successful, and continues. |
| `PPR_LOG_WARNING_ON_FAIL(category, expression)` | Converts and logs a warning, then continues; use only for deliberately nonfatal failures. |

Each macro evaluates the operation once, including when logging is disabled.
The conversion helpers must be visible or available through argument-dependent
lookup. The generic variant uses `hasFailed`, not `failed`, and does not serialize
the category/value/message fields supplied by the error-code variants. The
error-code variants log the stringized expression plus those three fields.
They accept convertible error enums as well as supported error-code/result types.
[Core.Utility.cppm](../lib/engine/core/Core.Utility.cppm) provides conversions
for `std::error_code`, `std::unexpected<std::error_code>`, and
`std::expected<T, std::error_code>` (success converts to a zero code). Passing
an expected result directly checks its error without extracting its value;
testing it explicitly and passing `.error()` is also valid.

```cpp
// In a function returning std::error_code:
PPR_RETURN_ERROR_ON_FAIL(Assets, initialize());
return {};

// In a function returning std::expected<Value, std::error_code>:
const auto value = load();
if (not value) {
    PPR_RETURN_UNEXPECTED_ON_FAIL(Assets, value.error());
}
return *value;

// In best-effort shutdown returning std::error_code:
std::error_code first_error;
PPR_RETAIN_ERROR_ON_FAIL(Assets, first_error, detach());
PPR_RETAIN_ERROR_ON_FAIL(Assets, first_error, drain());
PPR_RETAIN_ERROR_ON_FAIL(Assets, first_error, release());
return first_error;
```

These are statement macros that expand to an `if` with a line-unique local;
use braces around surrounding control flow and put separate invocations on
separate lines. Retained-code arguments must be stable modifiable lvalues.
Choose the retaining variant when an early return would skip required cleanup.
Recovery, expected absence, and translation need explicit handling before final
propagation; never turn a real failure into success merely to avoid a macro.

`PPR_THROW(category, exception)` logs a `std::exception`-derived exception's
`what` and dynamic type, then throws it. `PPR_RETHROW(category)` belongs inside a
catch handler; it logs standard or foreign exceptions and rethrows. At `OFF`
they reduce to throw/rethrow. They do not replace error-code results for
recoverable operational failures.
