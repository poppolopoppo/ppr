# lib/engine/app/input

## Responsibility
The `engine.app:input` module defines the input abstraction layer for the PPR game engine. It provides the type definitions and interfaces for keyboard, mouse, gamepad, and generic input devices, along with the action mapping system, modifier events, and trigger events that higher-level systems (player service, UI, gameplay) use to consume input in a device-agnostic way.

## Design
- **InputValue** variant type (`digital`, `axis_1d`, `axis_2d`, `axis_3d`) carries per-key state with absolute/relative values
- **InputKey** wraps `InputKeyCode` (variant of keyboard/gamepad/mouse enums) plus `InputValueType` to classify the value kind
- **InputAction** holds a description, value type, flags (`consume_input`, `trigger_when_paused`), and optional `ModifierEvent`/`TriggerEvent` callbacks
- **InputMapping** collects `InputActionKeyMapping` entries (key → action bindings) with priority-based keybinding resolution
- **InputModifierEvent** (`TimeSpan dt, InputValue &value`) — called to modulate an action's value before triggering
- **InputTriggerEvent** (`event, trigger`) — called when an action starts/triggered/completes
- `IInputService` interface (defined in `engine.app:service.input`) provides the contract for device-agnostic input posting
- `InputReplay` (in `input/replay`) decorates `IInputService` to support recording/playback of input frames
- All types reside in `namespace pP` and use `safe_ptr`, `string_literal`, `Numeric<T>` for type safety

## Flow
1. Device drivers (GLFW backend) produce `InputMessage` events (pressed/repeated/released/axis) via `IInputDevice::postInputMessages`
2. `InputListener::postKeyEvent` dispatches messages through keybindings → `InputActionEvent` with trigger state transitions (`inactive → started → triggered → completed`)
3. `InputModifierEvent` modulates the event value; `InputTriggerEvent` callbacks fire per-state-transition
4. `InputReplay` can decorate `IInputService` to record live input or replay captured frames
5. `PlayerGraph` maps devices to `Player` instances; each player has its own `InputListener` with per-player keybindings

## Integration
- **Consumers**: `GlfwInput`, `GlfwPlayer`, `ImGuiService`, `PlayerGraph`, `Application` (via `m_cached_input_service`)
- **Depends on**: `engine.core` (Numeric, safe_ptr), `engine.math` (vector types), `engine.rhi`, `engine.shader`
- **Provides**: `engine.app:input.action`, `engine.app:input.key`, `engine.app:input.mapping`, `engine.app:input.device`, `engine.app:input.replay`, `engine.app:input.listener` module namespaces
- **Used by**: `IInputService` (service.locator), `Application.update()` → `m_cached_input_service->postInputMessages(dt)`, `ImGuiService` for ImGui integration

## Key Files
- `App.Input.Action.cppm` — `InputAction`, `InputModifierEvent`, `InputTriggerEvent`, `EInputActionFlags`
- `App.Input.Action.cpp` — InputAction method implementations (modulate)
- `App.Input.Device.cppm` — `IInputDevice`, `InputMessage`, `InputAxisState`, `InputDigitalState`
- `App.Input.Key.cppm` — `EKeyboardKey`, `EGamepadAxis`, `EGamepadButton`, `EMouseAxis`, `EMouseButton`, `InputKey`, `InputValue`, `InputDigital`, `InputAxis1D/2D/3D`
- `App.Input.Key.cpp` — InputKey method implementations (isKeyboard/isGamepad/isMouse, from/enumerate)
- `App.Input.Listener.cppm` — `InputListener` class with keybindings, priority, action callbacks
- `App.Input.Listener.cpp` — InputListener method implementations (postKeyEvent, addMapping, rebuildKeybindings)
- `App.Input.Mapping.cppm` — `InputMapping`, `InputActionKeyMapping`, `InputMappingProvider`
- `App.Input.Mapping.cpp` — InputMapping method implementations (mapKey, unmapKey, clearAllMappings)
- `App.Input.Replay.cppm` — `InputReplay` final class decorating `IInputService` with record/playback
- `App.Input.Replay.cpp` — InputReplay method implementations (inject, routeToOwnListeners, postInputMessages, getKeyboard/mouse/gamepad)
- `App.Input.Gamepad.cppm` — `GamepadState`, `GamepadDevice` (inherits `IInputDevice`)
- `App.Input.Gamepad.cpp` — GamepadDevice method implementations (supportedInputKeys, postInputMessages, resetInputState)
- `App.Input.Keyboard.cppm` — `KeyboardState`, `KeyboardDevice` (inherits `IInputDevice`)
- `App.Input.Keyboard.cpp` — KeyboardDevice method implementations (supportedInputKeys, postInputMessages, resetInputState)
- `App.Input.Mouse.cppm` — `MouseState`, `MouseDevice` (inherits `IInputDevice`)
- `App.Input.Mouse.cpp` — MouseDevice method implementations (supportedInputKeys, postInputMessages, resetInputState)