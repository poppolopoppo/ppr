# lib/engine/core/function/

## Responsibility

The function partition provides the engine's callable primitives: `std23::function_ref` (non-owning type-erased
function reference, custom C++20 implementation waiting on C++26) plus the subscriber machinery built on it —
`Delegate` (single optional subscriber), `BroadcastCallback` (multi-subscriber, `SparseVector`-backed), and
`CallbackSink` (deferred-dispatch wrapper with `safe_ptr` argument forwarding). These are designed for zero-overhead
abstraction (no heap allocation, no virtual dispatch on call) and are used throughout the engine for callbacks, event
handlers, and draw commands. (`overloaded` lives in `hal`, not here.)

## Design

- **function_ref** (`std23::function_ref`, in `:function.ref`): non-owning reference to a callable with a specific
  signature. A `callable_object` trait layer normalizes free functions, member-function pointers (const/noexcept
  variants), and functor `operator()` into one signature/dispatch shape; `static_function_t`/`nontype<F>` wrap
  compile-time-known targets, and `function_ptr` storage holds the erased callable plus its dispatch pointer.
  `constexpr`-constructible from lambdas, function pointers, and nontype wrappers; `operator()` invokes via direct
  dispatch (no heap allocation). Convertible across compatible signatures; comparable for equality.
- **move_only_function** (in `:function.ref`, `std23::move_only_function` conditional alias): owning callable used by
  `TimerManager::Callback` and `context::AfterFunc`. MSVC aliases `std::move_only_function`; other toolchains
  (Clang libc++ without the C++23 owning wrapper) fall back to `std::function` — restores the build where the
  standard owning type is unavailable without changing call sites.
- **Delegate** (in `:function.callback`): single-subscriber callback over `details::DelegateImpl<FunctionT, params_tuple>` partial specialization. Holds `std::optional<function_ref<F>>`; `subscribe()` exchanges and returns the previous subscriber, with `nontype<F>` overloads (pointer/object) constrained by `requires` on `function_ref` constructibility; nullary `operator()` returns `default_value_v` for
  non-void signatures when empty; `reset()` clears.
- **BroadcastCallback** (in `:function.callback`): multi-subscriber callback for `std::error_code`-returning
  signatures (`TFunctionReturning<std::error_code>`), default allocator `mem::GPA`, implemented as
  `details::BroadcastCallbackImpl<FunctionT, AllocatorT, params_tuple>` partial specialization. Subscribers live in a mutable
  `SparseVectorInplace<Event, AllocatorT>` so `add()`/`remove()` are `const` (subscription through a const reference)
  while `clear()`/`operator()` stay non-const; dispatch short-circuits on the first error. `Handle` is a move-only
  RAII token holding the callback pointer, `SparseKeyId`, and a shared atomic liveness flag (`shared_ptr<atomic<bool>>`
  set false on callback destruction, checked with acquire-load; move-assign unsubscribes first) — destroying the handle
  unsubscribes, and destroying the callback flips the flag so late handle destruction never dangles (single-threaded
  contract: the callback object itself is not ref-counted, so callers keep it alive past handles or drop handles first).
  Removing during dispatch invalidates iteration and must be deferred by callers.
- **CallbackSink** (in `:function.callback`): deferred-dispatch wrapper over `BroadcastCallback` via
  `details::CallbackSinkImpl<FunctionT, AllocatorT, params_tuple>` — `operator()(args…)` latches only the first
  argument set into `m_deferred_params` (later calls ignored until drained), `sink()` applies it via
  `std::apply` + `exchange` and clears. `ForwardAsLValue` rewrites `safe_object`-derived (`TSafeObject`) arguments to
  `safe_ptr` so callbacks observe liveness (currently dormant — the impl stores the raw params tuple).
- **FunctionTraits** (`details::FunctionTraits`/`TFunction`/`TFunctionReturning`): compile-time signature
  introspection (return type, params tuple, noexcept) constraining `Delegate`/`BroadcastCallback`/`CallbackSink`.

## Flow

- **Single-handler slots**: `Delegate<F>` fields store at most one callback (e.g. an optional override); subscribe
  exchanges, invoke no-ops to `default_value_v` when empty.
- **Multi-handler events**: producers call `add(event)` to get a `Handle`; consumers keep the handle alive for the
  subscription lifetime and drop it to unsubscribe. `operator()(args…)` fans out in key order until the first
  `std::error_code` failure.
- **Deferred dispatch**: `CallbackSink` latches event args during an unsafe phase (e.g. inside a dispatch loop where
  removal is forbidden) and `sink()` replays them later from a safe point.
- **Draw command submission**: `Application` builds stack `DrawSubmission`s (named lambdas + borrowed draw callbacks);
  the renderer invokes those callbacks per submission via `renderAndPresent`/`submitToTexture`, retaining nothing.
- **Signal handler composition**: `Signal<Events...>` consumers combine handlers over `function_ref` targets in a
  single `select()`-filtered loop.

## Integration

- **containers**: subscriber storage is `SparseVectorInplace<Event>` keyed by `SparseKeyId`; `Collector` is itself a
  `function_ref`-derived push-back sink.
- **memory**: `BroadcastCallback`/`CallbackSink` allocate subscriber storage via `AllocatorT` (default `mem::GPA`);
  `CallbackSink` forwards `safe_object` arguments as `safe_ptr` for lifetime-checked callbacks.
- **engine.app**: `Renderer::renderAndPresent`/`submitToTexture` take `span<const DrawSubmission>`; each submission
  pairs a `RenderView` with a borrowed draw callback.
- **input system**: `IInputService` listener stack stores per-listener callback lambdas as `function_ref` targets;
  `whenKeyPressed`, `whenMouseMoved` etc. accept `function_ref`.
- **engine.tests.core**: Tests `function_ref` (construct from lambda/function pointer/member function/nontype,
  invocation, equality, cross-signature conversion), `Delegate` (subscribe-exchange/reset/empty-call default), and
  `BroadcastCallback`/`CallbackSink` (add/remove via `Handle`, error short-circuit, deferred `sink()` replay).

## Key Files

- `Core.Function.Ref.cppm` — `pP::std23::function_ref`, `callable_object` traits, `static_function_t`,
  `nontype<F>`, `TCallable`, `FunctionTraits`, `std23::move_only_function` conditional alias (MSVC owning type vs.
  `std::function` fallback)
- `Core.Function.Callback.cppm` — `pP::Delegate<F>`, `pP::BroadcastCallback<F, A>`,
  `pP::CallbackSink<F, A>` (+ `details::ForwardAsLValue`)
