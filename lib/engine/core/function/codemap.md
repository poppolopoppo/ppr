# lib/engine/core/function/

## Responsibility
The function partition provides foundational functional utilities: `function_ref` (non-owning function reference), `overloaded` visitor pattern for variant-like types, and utility aliases for common function signatures. These are designed for zero-overhead abstraction, compatibility with C++23 `std::function_ref`, and use throughout the engine for callbacks, event handlers, and draw commands.

## Design
- **function_ref**: Non-owning reference to a callable with a specific signature. Stores a pointer to the target callable and the target object (for member functions). `constexpr` constructible from lambdas, function pointers, and bind expressions. `operator()` invokes the target via direct call (no virtual dispatch, no heap allocation). `target()` returns `void*` to the callable, `target_type()` returns `type_info`. Debug mode asserts the target is valid (non-null). No small-buffer optimization — must outlive the `function_ref`.
- **overloaded**: Variadic template that creates a callable object dispatching to the correct overload based on the argument type. Used with `std::visit` or `function_ref` to handle variant-like types. `operator()` selected via `if constexpr` on the argument type index. Provides a default overload if no match (via `std::nullptr_t`).
- **function type aliases**: `VoidFn` (`void()`), `BoolFn` (`bool()`), `StepFn` (`void(float delta)`), `DrawFn` (`void(RenderState)`). Used as defaults for `function_ref` parameters in API surfaces.
- **Compatibility**: Designed to be ABI-compatible with `std::function_ref` (C++23); if C++23 is available, `function_ref` may alias `std::function_ref`, otherwise provides a custom implementation with the same interface.

## Flow
- **Input event handling**: Gamepad/button callbacks store `function_ref<void(GamepadEvent)>`; the main loop calls each registered ref if set.
- **Draw command submission**: Renderer records draw lambdas as `function_ref<void(RenderState)>`; submitted per viewport via `Renderer::render(span<const ViewportEntry>)`.
- **Signal handler composition**: `Signal<Events...>` consumers use `overloaded` to handle multiple event types in a single `select()`-filtered loop.
- **Callback system**: `Callback<T>` (defined in Core.Function.cppm or separate partition) uses `function_ref` internally for multi-subscriber notification; `pushInputListener`/`popInputListener` manage listener stack with `function_ref` targets.

## Integration
- **engine.app**: `Renderer::render()` takes `span<const ViewportEntry>` where each entry has a `function_ref` draw callback; these lambdas are captured from UI scripting code.
- **input system**: `IInputService` listener stack uses `function_ref` to store per-listener callback lambdas; `whenKeyPressed`, `whenMouseMoved` etc. accept `function_ref`.
- **engine.shader**: Hot-reload background compile callback uses `function_ref<void(ModuleHandle)>` for progress reporting.
- **EngineCoreTests**: Tests `function_ref` (construct from lambda/function pointer, `target()` validity, `operator()` invocation, debug assert on null), `overloaded` (visit variant, default overload, type dispatch), and function type aliases as `function_ref` template arguments.

## Key Files
- `Core.Function.cppm` — `pP::function_ref<TArgs...>`, `pP::overloaded<Ts...>`, function type aliases (`VoidFn`, `BoolFn`, `StepFn`, `DrawFn`)