# lib/engine/app/input

## Responsibility

The `engine.app:input.*` partitions own device-agnostic keys, values, actions,
mapping, analog shaping and listener/context routing. Player, UI and gameplay
code own the actions and mappings consumed through non-owning `safe_ptr` views.

## Navigation

| Files (`.cppm/.cpp`) | Symbols and responsibility |
|---|---|
| `App.Input.Key` | `InputKey`, `InputValue`, device enums: canonical identities, value modulation, hashing and opaque formatting |
| `App.Input.Action` | `InputAction`, `InputMapping`, `InputActionKeyMapping`, modifier/trigger events: bindings and transition vocabulary |
| `App.Input.Device` | `IInputDevice`, `InputMessage`, `InputAxisState`, `InputDigitalState`, keyboard/mouse/gamepad devices: state and message production |
| `App.Input.FilteredAnalog` | `FilteredAnalog`: first-order-lag scalar/vector/quaternion accumulation |
| `App.Input.Listener` | `InputListener`, `InputContext`, `WindowInputContext`: action dispatch, priority/parent routing and window delegates |
| `App.Input.Routing` | `InputBackgroundLatch`: background-origin left/middle drag routing |
| `App.Input.Batch` | `PendingInputBatch`: owned native input/control collection and ordered replay |

`../CMakeLists.txt` owns registration. The former `device/` partitions were
consolidated into `:input.device`; Mapping moved into `:input.action`; Replay
was removed. Do not look for standalone Mapping/Replay files.

## Identity and dispatch contracts

- `InputKey` equality, ordering and hashing use the code only; canonical aliases
  are the bindable identities. Unsupported device-enum mappings return `nullopt`.
  `InputValue` owns absolute/relative digital or axis values; use its `modulate`
  overloads for rank projection and explicit-time scaling, not a second conversion path.
- `InputAction` requires a nonempty description and defaults to consuming input.
  Disabled actions are skipped; pause-trigger intent is part of the action flags.
  `InputMapping` deduplicates by action/key identity; adding a modifier to an
  existing pair updates that binding. Unmapping an action removes all its bindings.
- `InputListener::postKeyEvent` owns the trigger state machine. Modifiers run
  action-first, binding-second; transition callbacks run binding-first,
  action-second, followed by the listener callback. Press starts then triggers;
  repeats accumulate elapsed time/count; release completes and clears the count.
  Axis/double-click triggering resets elapsed time to the supplied delta.
- Listeners default to consumed mode. Unhandled mode suppresses dispatch; a
  handled/consumed raw-key hook intercepts before action lookup. Any-key identities
  are binding selectors, not posted device events. Unmatched keys stay unhandled.
- `rebuildKeybindings_` preserves surviving action state and expands AnyKey by
  value type. All compatible bindings remain candidates: consumption is checked
  against live action flags at dispatch, so toggling it needs no rebuild.
- `InputContext` visits listeners by priority, then its parent chain. Consumed
  stops propagation; handled accumulates without stopping. Actions and mappings
  remain client-owned even when callbacks are invoked through const views.

## Device state and frame admission

`InputDigitalState` owns pressed/repeat/release classification; unsupported keys
and redundant releases do not post. Device wrappers own unbound-key diagnostics.
`InputAxisState` owns dead-zone/sensitivity shaping and raw/filtered selection;
mouse cursor/wheel and gamepad sticks/triggers emit a final inactive zero once
an active signal settles. Reset synchronizes raw, filtered and staged state.

Keyboard polling clears text before native events append the frame's characters;
mouse polling is a no-op; gamepad polling advances rumble state. Mouse axis
filtering is opt-in. Focus loss/device reset clears transient digital, analog,
rumble and text state. `FilteredAnalog` is the separate convergence-rate filter
(sensitivity in seconds⁻¹, 150 ms hitch clamp); unprimed output is raw, reset/clear
drop priming, and quaternion operations preserve normalized rotations with an
identity fallback. See the implementation for filter equations and instantiations.

`PendingInputBatch` collects character/key/mouse/focus/device records while main
waits or admission is busy, without advancing devices or dispatching clients.
The next admitted GLFW update replays records once in order and clears them;
extra native pumps do not advance the client. See [platform flow](../platform/glfw/codemap.md).

## Window and background-drag lifetime

`WindowInputContext` registers its context with `IInputService` and parents it to
the global context. `initialize` connects window delegates; `shutdown` disconnects
them before destruction, which requires a detached window and unregisters the
context. Already-connected, invalid-window and not-connected errors remain
explicit. Cursor positions stay in GLFW client space for ImGui.

`ApplicationEditor` owns `InputBackgroundLatch`. Priority must satisfy
foreground < detector < background (UI, detector, player); a background listener
is required. The detector always returns unhandled and ignores repeats, preserving
background-origin left/middle holds until release. Focus loss/disconnect resets
both latch and camera motion. Initialization registers detector/background
listeners; shutdown removes both with retain-first-error cleanup and clears the
foreground view. Destruction detaches its callback before destroying state.

## Compiler constraints

Linux/Clang bring-up constraints are retained in the owning declarations and
implementations: namespace-scope hash/opaque declarations keep `[[nodiscard]]`
off non-defining friends; explicit Mango opaque dimensions and a double `pow`
exponent avoid overload/narrowing ambiguity. Listener keybinding indices use
manual counters rather than unavailable `views::enumerate`. Modifier, trigger
and listener callbacks use `std23::move_only_function` from `Core.Function.Ref`
(MSVC move-only implementation, `std::function` elsewhere), without a
`const noexcept` signature. These are compatibility constraints, not fresh
cross-compiler validation evidence.

## Integration

`IInputService` in `:service.input` owns the posting/service boundary. GLFW owns
hot devices and global/per-device contexts; player graphs own player listeners;
ImGui consumes foreground input and its pending-deselect raw-key hook; camera
controllers provide action mappings. Application drives the admitted update.
Dependencies are core containers/lifetimes/functions/time and math values, plus
the input service and window delegate contracts. Types live in `namespace pP`;
`Input` and `InputDevice` are the transition/unbound-key log categories.
