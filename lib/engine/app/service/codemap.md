# lib/engine/app/service

## Responsibility
The `engine.app:service` module provides app-level service interfaces that integrate the various subsystem services (input, player, UI, window) into the engine's service locator pattern. Services are registered in the application's `ServicesStore` at compile-time via `typeUid<T>()` keys, and retrieved at runtime via `safe_ptr<T>` with parent-chain fallback. This module defines the service interfaces that `IService` descendants implement, enabling implicit dependency injection throughout the application.

## Design
- **IService** base class (defined in `engine.core`) provides the compile-time type identity mechanism via `typeUid<T>()` — each service type gets a unique `hash_t` key
- **ServicesStore** (`engine.core:services`) — `FlatMap<hash_t, safe_ptr<IService>>` with parent-chain fallback: when a service is not found in the current store, it walks the parent chain to find it
- **ServiceInjector** — implicit DI pattern: code calls `m_services.get<IInputService>()` and gets a `safe_ptr` that is valid as long as the service member object outlives the pointer
- Service categories in this module:
  - `IInputService` — keyboard/mouse/gamepad input, listener stack, action mappings, callbacks (defined in `engine.app:service.input`)
  - `IPlayerService` — player identity management, graph-based state machine, device-to-player mapping (defined in `engine.app:service.player`)
  - `IUIService` — ImGui-based overlay rendering, newFrame/renderOverlay/onResize, getContext (defined in `engine.app:service.ui`)
  - `IWindowService` — window creation/lifecycle, monitor enumeration, event callbacks, clipboard, manipulation (defined in `engine.app:service.window`)
- Each interface inherits from `IService` virtually, enabling `safe_ptr<IService>` base-class upcasting
- `Application` owns all services (`m_services`, `m_ui_services`, `m_scene_services`) and manages their lifecycle (initialize/shutdown in `run()` / `shutdown()`)

## Flow
1. `Application::initialize()` — creates platform services (via `m_platform->initialize(*this)`) which insert `IInputService`, `IWindowService`, `IPlayerService` into `m_services`; creates `IUIService` (ImGui) and inserts into `m_ui_services`; inserts `IShaderService` and `IRhiService` into `m_services`
2. Per-frame `Application::update()` — retrieves services via `m_cached_window_service->pollEvents()`, `m_cached_input_service->postInputMessages(dt)`, and other service lookups via `m_services.get<T>()`
3. Per-frame `Application::render()` — retrieves services for draw callbacks, passes to `m_renderer->render(viewports)`
4. `Application::shutdown()` — erases services in reverse initialization order: `IInputService`, `ICameraService`, `m_ui_service.reset()`, `m_renderer.reset()`, `m_main_window`, `m_cached_input_service`, `m_cached_window_service`; then `IRhiService`, `IShaderService`, `IUIService`, platform shutdown

## Integration
- **Consumers**: `Application` (primary — manages service store lifecycle), all subsystems that retrieve services via `m_services.get<T>()` or `m_ui_services.get<T>()`
- **Depends on**: `engine.core` (IService, typeUid, ServicesStore, safe_ptr, hash_t), `engine.app:service.input`, `engine.app:service.player`, `engine.app:service.ui`, `engine.app:service.window`, `engine.rhi` (IShaderService, IRHIService used in IUIService::initialize), `engine.shader` (IShaderService)
- **Provides**: `engine.app:service.input`, `engine.app:service.player`, `engine.app:service.ui`, `engine.app:service.window` module namespaces with their respective `I*Service` interfaces
- **Used by**: `Application` (m_services, m_ui_services, m_scene_services members, getServices(), getUiServices()), all platform backends (GlfwPlatform inserts services into store), ImGuiService (pushes listener to input service via m_input_service)

## Key Files
- `App.Service.Input.cppm` — `IInputService` interface (devices, events, listeners, mappings, callbacks)
- `App.Service.Player.cppm` — `IPlayerService` interface (player management, callbacks)
- `App.Service.UI.cpp` — empty stub (implementation in `App.UI.ImGui.cpp`)
- `App.Service.UI.cppm` — `IUIService` interface (initialize, shutdown, newFrame, renderOverlay, onResize, getContext)
- `App.Service.Window.cppm` — `IWindowService` interface (monitors, windows, callbacks, manipulation, clipboard)