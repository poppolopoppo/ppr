# lib/engine/app/input/device

## Responsibility
The `engine.app:input.device` module provides concrete input device implementations (`IInputDevice`) for keyboard, mouse, and gamepad. These classes encapsulate per-device state (key/button/stick state, dead zones, filtering), handle analog axis filtering and dead zone application, and produce `InputMessage` events that the input listener system dispatches to action mappings. The device layer is the bridge between low-level platform callbacks (GLFW) and the engine's higher-level input abstraction.

## Design
- **IInputDevice** abstract base class with pure virtual methods: `getInputDeviceID()`, `supportedInputKeys()`, `postInputMessages()`, `resetInputState()`
- **InputAxisState<T>** template — manages raw/filtered analog state with dead zone, sensitivity, and low-pass filtering. `update(elapsed)` shifts raw state into filtered state based on elapsed time and sensitivity parameter.
- **InputDigitalState<ButtonT>** — manages a `FlatSet` of button state (pressed/repeated/released) with `update()` that transitions queued→down, pressed→up, and swaps queues. `postInputMessages()` posts pressed/repeated/released messages for all down/pressed buttons.
- **KeyboardDevice** — wraps `KeyboardState` (holds `EKeyboardKey` digital state + character queue). `postInputMessages()` calls `KeyboardState::postInputMessages()` which updates internal state and posts `InputMessage` for each pressed key.
- **MouseDevice** — wraps `MouseState` (holds `EMouseButton` digital state + cursor position + wheel axes). `postInputMessages()` updates cursor position state and posts axis/button messages.
- **GamepadDevice** — wraps `GamepadState` (holds `EGamepadButton` digital state + dual sticks + left/right triggers, rumble, connection flags). `postInputMessages()` calls `GamepadState::update(dt)` then posts axis and button messages for all connected state.
- All devices use `InputDeviceID` (Numeric<u32, IInputDevice>) to identify the device; the GLFW backend assigns IDs 0=keyboard, 1=mouse, 2-5=gamepads 0-3.

## Flow
1. Platform (GLFW) → `GlfwInput::onKey/onMouseButton/onCursorPos/onScroll` → updates hot key/button state → calls `postInputMessages(dt)` on the relevant device
2. Device `postInputMessages()` updates internal state → produces `InputMessage` entries (pressed/repeated/released/axis) via the `Collector<InputMessage>` callback
3. Messages flow through `InputListener::postKeyEvent()` → keybinding resolution → `InputActionEvent` trigger state transitions
4. `resetInputState()` clears all per-frame state (called at startup/shutdown or when reinitializing)

## Integration
- **Consumers**: `GlfwInput` (platform/glfw), `Application` (via `m_cached_input_service`), `InputReplay` (decorates `IInputService`)
- **Depends on**: `engine.core` (Numeric, safe_ptr, IInputDevice), `engine.math` (int2, float2, float3), `std` (variant, monostate)
- **Provides**: `engine.app:input.device` module namespace with `IInputDevice`, `KeyboardDevice`, `MouseDevice`, `GamepadDevice`, `InputAxisState`, `InputDigitalState`
- **Used by**: `GlfwPlatform::initialize()` → inserts `IInputService` into app services; `Application.update()` → `m_cached_input_service->postInputMessages(dt)`; `InputReplay` routes messages through its capture listener

## Key Files
- `App.Input.Device.cppm` — `IInputDevice` interface, `InputMessage`, `InputAxisState<T>`, `InputDigitalState<ButtonT>`
- `App.Input.Device.cpp` — (not a separate .cpp; implementations inline in Device.cppm or in keyboard/mouse/gamepad .cpp files)
- `App.Input.Keyboard.cppm` — `KeyboardState`, `KeyboardDevice` inherits `IInputDevice`
- `App.Input.Keyboard.cpp` — KeyboardDevice method implementations
- `App.Input.Mouse.cppm` — `MouseState`, `MouseDevice` inherits `IInputDevice`
- `App.Input.Mouse.cpp` — MouseDevice method implementations
- `App.Input.Gamepad.cppm` — `GamepadState`, `GamepadDevice` inherits `IInputDevice`
- `App.Input.Gamepad.cpp` — GamepadDevice method implementations