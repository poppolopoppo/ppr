# lib/engine/app/service

## Responsibility

App-level service interfaces plugged into the engine's service-locator pattern (`IService` → `typeUid<T>()` →
`ServicesStore` with parent-chain fallback, consumed via `safe_ptr<T>`). This directory declares the four contracts
`Application` and the GLFW platform backend coordinate: input, player, window, and UI overlay.

## Design

- `IInputService` (`App.Service.Input.cppm`, `:service.input`) — keyboard/mouse/gamepad device accessors, device-by-ID
  lookup + enumeration, supported-key enumeration, input contexts/listeners/mappings, action-event callbacks.
- `IPlayerService` (`App.Service.Player.cppm`, `:service.player`) — player identity store: `getPlayer`,
  `enumeratePlayers`, `getOrCreateKeyboardPlayer` / `addGamepadPlayer` / `removePlayer`, `whenPlayerAdded/Removed`
  broadcast callbacks; static `get()` accessor; `extern template` for the player callback instantiation (defined in
  `App.TemplateInstantiations.cpp`).
- `IWindowService` (`App.Service.Window.cppm`, `:service.window`) — event pump (`pollEvents`/`waitEvents`), monitor
  enumeration + primary + video modes + gamma, monitor connect/disconnect callbacks, window
  create/destroy/focused/main, resize/focus/close callbacks, manipulation + clipboard.
- `IUIService` (`App.Service.UI.cppm`, `:service.ui`) — overlay contract only: `initialize(IRhiService,
  IWindowService, IInputService, Window, Format)`, `shutdown`, `newFrame(dt)`, `renderOverlay(pass, fb_size)`,
  `onResize`, `getContext`. Implementation lives in `ui/App.UI.ImGui.cpp`, NOT here.
- DELETED: `App.Service.UI.cpp` (former empty stub) is gone — only `App.Service.UI.cppm` remains, and CMakeLists no
  longer lists any `service/*.cpp`. There are no other `service/*.cpp` files; all service behavior is implemented by
  the platform backend and the UI module.

## Flow

1. `Application::initialize()` → platform `initialize(*this)` inserts `IInputService` / `IWindowService` /
   `IPlayerService` into `m_services`; `ui::createImGuiService()` result is initialized and visible via
   `m_ui_services` (chained to the root store); RHI + shader services are inserted alongside.
2. Per-frame: cached `IWindowService::pollEvents()` + `IInputService::postInputMessages(dt)` drive listeners/mappings;
   `IPlayerService` callbacks fire on player add/remove; `IUIService::newFrame/renderOverlay` runs inside
   `Application::update/render`.
3. Lookup rule: `m_services.get<T>()` first, else walk to parent store — UI-scoped code sees both `m_ui_services`
   and the root store.

## Integration

- **Consumers**: `Application` (owns `m_services` + child `m_ui_services`), GLFW platform (inserts input/window/player
  services), `ImGuiService` (registers its listener with `IInputService`, reads `IWindowService` framebuffer size,
  uses `IRhiService` device/queue).
- **Depends on**: `engine.core` (`IService`, `ServicesStore`, `safe_ptr`, delegates/callbacks), `engine.math`
  (`int2`/`float2`), `engine.rhi` (UI init/render signatures), `std`.
- **Provides**: `engine.app:service.input/player/ui/window` interfaces — no behavior, no runtime singletons here.

## Key Files

- `App.Service.Input.cppm` — `IInputService` (devices, contexts, listeners, mappings, action callbacks).
- `App.Service.Player.cppm` — `IPlayerService` (player store + add/remove callbacks).
- `App.Service.UI.cppm` — `IUIService` (overlay lifecycle; impl in `ui/`).
- `App.Service.Window.cppm` — `IWindowService` (monitors, windows, event pump, clipboard).
