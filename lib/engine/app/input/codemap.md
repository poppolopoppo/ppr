# lib/engine/app/input

## Responsibility
The `engine.app:input` module defines the input abstraction layer for the PPR game engine. It provides the type definitions and interfaces for keyboard, mouse, and gamepad devices, along with the action/mapping system, modifier/trigger events, exponentially filtered analog shaping, and the `InputContext` listener-routing tree that higher-level systems (player service, UI, gameplay) use to consume input in a device-agnostic way.

Recent refactor: the per-device `device/` subdirectory (`Gamepad`/`Keyboard`/`Mouse` partitions) was removed and consolidated into the unified `engine.app:input.device` partition; the standalone `App.Input.Mapping` partition was removed and merged into `engine.app:input.action`; `App.Input.Replay` was removed entirely; `App.Input.FilteredAnalog` and `App.Input.Device.cpp` were added.

## Design
- **InputValue** variant type (`digital`, `axis_1d`, `axis_2d`, `axis_3d`) carries per-key state with absolute/relative values
- **InputKey** wraps `InputKeyCode` (variant of keyboard/gamepad/mouse enums) plus `InputValueType` to classify the value kind
- **InputAction** holds a description, value type, flags (`consume_input`, `trigger_when_paused`), optional `ModifierEvent`, and per-transition `TriggerEvent` callbacks (`setStarted`/`setTriggered`/`setCompleted`); static `modulate()` factories include a generic `GetValueT` overload projecting sampled state into the action value
- **InputActionKeyMapping** (key → action binding with optional per-binding modifier/trigger callbacks) and **InputMapping** (`StableVectorInplace` keymap with `mapInputKey`/`unmapInputKey`/`unmapInputAction`/`clearInputMappings`) now live in `engine.app:input.action` (moved from the deleted `:input.mapping` partition)
- **InputModifierEvent** (`TimeSpan dt, InputValue &value`) — modulates an action's value before triggering
- **InputTriggerEvent** (`event, trigger`) — fires on action started/triggered/completed transitions; `EInputTriggerEvent` (`inactive → started → triggered → completed`); `InputActionEvent` carries source action, optional value, elapsed trigger time, and repeat count
- **IInputDevice** (`safe_object` base) — `getInputDeviceID()`, `enumerateSupportedInputKeys()`, `pollInputMessages(dt)`, `resetInputState()`; `InputMessage` carries key, value, device id, and event (`pressed`/`released`/`repeat`/`double_click`/`axis`)
- **InputAxisState<T>** template — accumulates `m_next_raw_absolute`, applies dead-zone/sensitivity shaping, and posts raw + filtered messages via `postInputMessages(dt, context, device_id, key, enable_filtered_inputs)` (extern instantiations for `float`, `float2`)
- **InputDigitalState<ButtonT>** template (constrained to enum/integral) — `FlatSet` pressed-set with per-button `postInputMessages` (extern instantiations for `EKeyboardKey`, `EGamepadButton`, `EMouseButton`)
- **KeyboardDevice** (digital keys + `m_character_inputs` text buffer), **MouseDevice** (button set, cursor-pos `float2` axis, wheel `float` axes, opt-in `m_has_axis_filtering`), **GamepadDevice** (button set, stick `float2` axes, trigger/rumble `float` axes, per-group filtering flags, `isConnected()` via `m_controller_id != none_v`) — all consolidated in `engine.app:input.device` (moved from the deleted `device/` per-device files)
- **FilteredAnalog<T>** (`engine.app:input.filtered_analog`) — exponentially filtered analog accumulator (`m_raw`/`m_delta`/`m_filtered` + `sensitivity`); `add`/`addClamp`/`setRaw` feed raw input, `update(dt)` advances the filter; `Quaternion` gets explicit specialization treatment; extern templates for `float`, `float2`, `float3`, `Quaternion`
- **InputListener** — `PrioritySet` of mappings, `FlatMap<action, InputActionEvent>` live action state, `FlatMultiMap<key, InputBinding>` rebuilt keybindings; action + raw-key callbacks; `EInputMessageResponse` listener mode (`unhandled`/`handled`/`consumed`); `postKeyEvent` dispatches with trigger state transitions
- **InputContext** — priority-ordered listener set with optional parent chain (`getParentContext`); `addInputListener`/`removeInputListener`/`postKeyEvent`; **WindowInputContext** binds an `IInputService` + `Window` and bridges window callbacks (character/key/mouse-button/mouse-moved/scroll) into context posts
- `IInputService` interface (defined in `engine.app:service.input`) provides the device-agnostic posting contract; all types reside in `namespace pP` and use `safe_ptr`, `string_literal`, `Numeric<T>`

## Flow
1. Device front-ends (`GlfwInput` hot state, `WindowInputContext` window callbacks) call `post*` entry points carrying the target `InputContext`
2. Digital state (`InputDigitalState::postInputMessages`) emits pressed/released messages; analog state (`InputAxisState::postInputMessages`) emits axis messages in both raw and (optionally) filtered form after dead-zone/sensitivity shaping
3. `InputContext::postKeyEvent` walks listeners by priority (then parent chain); each `InputListener::postKeyEvent` resolves keybindings → per-action `InputActionEvent` with trigger state transitions (`inactive → started → triggered → completed`)
4. `InputModifierEvent` modulates the event value; `InputTriggerEvent` callbacks fire per transition; `consume_input` actions stop further propagation (`consumed`)
5. Per-frame, `pollInputMessages`/`pollInputDevices(dt)` flush accumulated axis/digital state; `resetInputState` clears transient state (focus loss, device reset)

## Integration
- **Consumers**: `GlfwInput` (hot devices + global/per-device contexts), `GlfwPlayer`/`PlayerGraph` (per-player listeners), `ImGuiService`, `Application` (per-frame poll)
- **Depends on**: `engine.core` (Numeric, safe_ptr/safe_object, FlatSet/FlatMap/PrioritySet, TimeSpan), `engine.math` (float2/float3/Quaternion vector types), `std`
- **Provides**: `engine.app:input.action`, `engine.app:input.device`, `engine.app:input.filtered_analog`, `engine.app:input.key`, `engine.app:input.listener` module namespaces (no `:input.mapping`, `:input.replay`, or `device/` sub-partitions remain)
- **Used by**: `IInputService` (service.locator), `Application.update()` → per-frame device poll, `ImGuiService` for ImGui integration

## Key Files
- `App.Input.Action.cppm` — `InputAction`, `InputModifierEvent`, `InputTriggerEvent`, `EInputActionFlags`, `EInputTriggerEvent`, `InputActionEvent`, `InputActionKeyMapping`, `InputMapping` (mapping types merged here after `App.Input.Mapping` removal)
- `App.Input.Action.cpp` — InputAction/InputActionEvent/InputMapping method implementations
- `App.Input.Device.cppm` — `IInputDevice`, `InputMessage`, `EInputMessageEvent`, `EInputMessageResponse`, `InputAxisState<T>`, `InputDigitalState<ButtonT>`, `KeyboardDevice`, `MouseDevice`, `GamepadDevice` (unified here after `device/` removal)
- `App.Input.Device.cpp` — device/state method implementations (pollInputMessages, resetInputState, post helpers)
- `App.Input.FilteredAnalog.cppm` — `FilteredAnalog<T>` exponentially filtered analog accumulator (added)
- `App.Input.FilteredAnalog.cpp` — FilteredAnalog method implementations + extern template instantiations
- `App.Input.Key.cppm` — `EKeyboardKey`, `EGamepadAxis`, `EGamepadButton`, `EMouseAxis`, `EMouseButton`, `InputKey`, `InputValue`, `InputDigital`, `InputAxis1D/2D/3D`
- `App.Input.Key.cpp` — InputKey/InputValue method implementations (isKeyboard/isGamepad/isMouse, from/enumerate, modulate)
- `App.Input.Listener.cppm` — `InputListener`, `InputContext`, `WindowInputContext`, action/raw-key callbacks
- `App.Input.Listener.cpp` — InputListener/InputContext/WindowInputContext implementations (postKeyEvent, addMapping, rebuildKeybindings, window callback bridges)
- Deleted, not present: `App.Input.Mapping.cpp/.cppm` (merged into Action), `App.Input.Replay.cpp/.cppm` (removed), `device/` subdirectory with per-device Gamepad/Keyboard/Mouse files (consolidated into Device)
