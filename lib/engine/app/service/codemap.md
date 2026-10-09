# lib/engine/app/service

## Responsibility

App-level service interfaces plugged into the engine's service-locator pattern (`IService` →
`ServicesStore` with parent-chain fallback, consumed via `safe_ptr<T>`). This directory declares the five contracts
`Application`/`ApplicationEditor` and the GLFW platform backend coordinate: client scene access, input, player,
window, and UI overlay.

## Design

- `IClientService` (`App.Service.Client.cppm`, `:service.client`) — scene-client accessor contract implemented by
  `ApplicationEditor` (protected inheritance): `getApplication()`, plus const + mutable `safe_ptr` pairs for
  `getMainCamera` (`Camera`), `getMainPlayer` (`Player`), `getMainViewport` (`WindowViewport`), `getMainInputContext`
  (`InputContext`). Encapsulates window events and camera handling per scenario; forward-declares
  `Application/Camera/ICameraController/InputContext/Player/WindowViewport` to avoid partition cycles.
- `IInputService` (`App.Service.Input.cppm`, `:service.input`) — keyboard/mouse/gamepad device accessors, device-by-ID
  lookup + enumeration, supported-key enumeration, global/per-device input contexts + listener registration,
  device→context assignment, `pollInputDevices`/`resetInputDevices`, `post*` delegates, device connect/disconnect
  callbacks; plus chain-order enums `EInputListenerPriority { ui = 0, detector = 1, player = 2 }` (detector is the
  deterministic pre-player tier for the background-drag actuator) and `EInputMappingPriority { camera = 1 }`.
- `IPlayerService` (`App.Service.Player.cppm`, `:service.player`, `public virtual IService`) — player identity store:
  `getPlayer(id)` / `enumeratePlayers` (`noexcept`), `getOrCreateKeyboardPlayer` / `addGamepadPlayer(u32)` returning
  `Expected<SharedPlayer>`, `removePlayer(id)` returning `std::error_code`, `whenPlayerAdded/Removed` broadcast
  callbacks; `extern template` for the player callback instantiation (defined in
  `App.TemplateInstantiations.cpp`).
- `IWindowService` (`App.Service.Window.cppm`, `:service.window`) — event pump (`pollEvents`/`waitEvents`), monitor
  enumeration + primary + video modes + gamma, monitor connect/disconnect callbacks, window
   create/destroy/focused/main, resize/focus/close callbacks, manipulation + clipboard. `configureDisplayWindow`
   accepts a nullable monitor and optional exclusive refresh, returns authoritative `WindowDisplayGeometry`, and
   is native-thread-only; `displayWindowGeometry` queries observations. Capability flags default unsupported.
- `IUIService` (`App.Service.UI.cppm`, `:service.ui`) — overlay contract only: `initialize(WindowInputContext &,
  IRhiService &, IShaderService &, int input_listener_priority)`, `shutdown`, `update(TimeSpan, const WindowViewport
  &)`, `render(const DrawContext &)`, `getContext`, and detached frontend/backend rendering. `buildDetachedUiFrame` runs on the client; `renderDetachedUiFrame` encodes owned data on the render owner lane; font-version acknowledgments cross back in frame results. See [UI map](../ui/codemap.md). Implementation lives in `ui/App.UI.ImGui.cpp`, NOT here.
- Linux bring-up: no service-partition source changed — the bring-up contract change is that input callbacks posted
  through `IInputService` now resolve to the conditional `std23::move_only_function` alias from `Core.Function.Ref`
  (see `input/codemap.md`), so service consumers (`GlfwInput`, `ImGuiService`, player listeners) share one callback
  type on Clang and MSVC alike.
- No `service/*.cpp` files exist; all service behavior is implemented by the platform backend, the UI module, and
  `ApplicationEditor` (client contract).

## Flow

1. `Application::initialize()` → platform `initialize(*this)` inserts `IInputService` / `IWindowService` /
   `IPlayerService` into `m_services`; rendering path inserts `IShaderService` + `IRhiService`. `ApplicationEditor::
   initialize()` additionally creates window/viewport/input-context/player/camera and inserts the initialized
   `IUIService` into `m_services` via `insert_or_assign`.
2. Per-frame: platform polling/replay dispatches input through listeners/mappings (`ui(0) < detector(1) < player(2)`, camera mapping priority `1`). The editor updates camera/UI on main and publishes owned grid/triangle/UI payloads. Pass updates, GPU encoding, submission and presentation execute on the render lane (or inline reference), with grid/triangle followed by UI. See [application flow](../codemap.md); service lookup stays at the startup/client boundary rather than in worker callbacks.
3. Lookup rule: a `ServicesStore` first checks its local map, then an explicitly supplied parent. The current UI implementation has no `m_ui_services` child store; it receives dependencies at initialization. Client code reaches camera/player/viewport state through `IClientService` getters.

## Integration

- **Consumers**: `Application` (owns `m_services`), `ApplicationEditor` (implements `IClientService`, owns + inserts
  `IUIService`), GLFW platform (inserts input/window/player services), `ImGuiService` (initialized with
  `WindowInputContext` + RHI/shader services, reads viewport size, renders via `DrawContext`).
- **Depends on**: `engine.core` (`IService`, `ServicesStore`, `safe_ptr`, delegates/callbacks, `Expected`,
  `Collector`), `engine.math` (`int2`/`float2`), `engine.rhi` + `engine.shader` (UI init/render signatures), `std`.
- **Provides**: `engine.app:service.client/input/player/ui/window` interfaces — no behavior, no runtime singletons here.

## Key Files

- `App.Service.Client.cppm` — `IClientService` (main camera/player/viewport/input-context + application accessors).
- `App.Service.Input.cppm` — `IInputService` (devices, contexts, listeners, mappings, action callbacks).
- `App.Service.Player.cppm` — `IPlayerService` (player store + add/remove callbacks).
- `App.Service.UI.cppm` — `IUIService` (overlay lifecycle; impl in `ui/`).
- `App.Service.Window.cppm` — `IWindowService` (monitors, windows, event pump, clipboard).
