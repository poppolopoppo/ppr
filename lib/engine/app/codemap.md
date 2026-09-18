# lib/engine/app

## Responsibility

`engine.app` is the application-layer umbrella module: the single compile-time aggregation point (`import engine.app;`)
for application lifecycle, input, player, scene camera, services, window, platform, renderer, and UI.
`pP::Application` (`:application`) is the slim lifecycle base — `ApplicationDomain`, run-loop, `IPlatform`,
`ServicesStore`, shader/RHI/`Renderer` bootstrap, directory resolution. `pP::ApplicationEditor`
(`:application_editor`) is the interactive-client subclass that owns window, viewport, input context, player, camera,
triangle pass, and ImGui service, and implements `IClientService`. `game/main.cpp` drives `run()` on the concrete
application.

## Design

- Umbrella `App.cppm` only `export import`s partitions — `:application` + `:application_editor`, `:input.*` (6),
  `:player` + `:player.graph`, `:scene.camera` + `:scene.camera.controller`, `:service.client` + `:service.input` +
  `:service.player` + `:service.ui` + `:service.window`, `:window.*` (viewport/handle/monitor), `:platform`,
  `:renderer` + `:renderer.triangle_pass` + `:renderer.types`, `:ui.imgui`.
- `Application` (`App.Application.cppm/.cpp`, `module engine.app:application`) — `safe_object` subclass with virtual
  `initialize/update/render/shutdown` returning `std::error_code`. Owns `unique_ptr<Renderer>`, `TimerExplicitClock`
  `m_application_clock`, `optional<TimeDuration> m_target_frame_duration`, `const unique_ptr<IPlatform> m_platform`
  (declared before `ServicesStore m_services` so reverse-destruction releases store observers before platform owners),
  `SharedContext m_lifecycle` + `context::CancelClauseFunc m_request_exit`, single-use `m_has_torn_down` latch,
  `Array<string>` args, `string` name, `const ApplicationDomain m_domain`, `directory_entry` dirs
  (install/content/working resolved; config left default). Ctor wires `IPlatform::create()` plus
  `hal::disableSystemErrorReporting/installDebugAssertHooks`. Forward-declares `IClientService`. Accessors:
  `getName/Arguments/Domain/Platform/Lifecycle` (const ref), `getRenderer/Services` (const + mutable overloads),
  `getTimerClock/TargetFrameDuration`, dir getters; `requestExit(clause = {})`, `setTargetFrameDuration(TimeDuration/nullopt)`,
  `setTargetFrameRate(fps)` (non-positive clears throttle), `run()`.
- `ApplicationDomain` — immutable-after-construction bitfield struct: `m_is_headless` (false), `m_is_interactive`
  (true), `m_needs_presence` (false), `m_needs_rendering` (true), `m_needs_user_interface` (true); `ApplicationEditor`
  fixes these values at construction.
- `ApplicationEditor` (`App.Application.Editor.cppm/.cpp`, `module engine.app:application_editor`) — `public
  Application, protected IClientService`. Owns `unique_ptr<Player>`, `unique_ptr<Camera>`,
  `unique_ptr<ICameraController>`, `unique_ptr<WindowInputContext> m_main_input_context`,
  `unique_ptr<InputMapping> m_camera_input_mapping`, `unique_ptr<IUIService>`, `unique_ptr<WindowViewport>`,
  `unique_ptr<TrianglePass>`. Implements `IClientService` const + mutable `getMainCamera/Player/Viewport/InputContext`
  getters (input getters wrap `&m_main_input_context->m_context`) plus protected `getApplication()` returning `this`.
  Declares private `onMainWindowClosed_(const Window&)` hook with no definition in the matching `.cpp`. Local
  `EInputPriority { ui = 0, camera, player }` orders the input chain.
- `App.TemplateInstantiations.cpp` pins explicit instantiations for `Delegate` (window events incl. key/mouse/char
  overloads) and `BroadcastCallback` (monitor/window/input/player/time) so downstream users don't pay
  implicit-instantiation cost.
- CMakeLists registers `App.cppm`, `App.Application.cppm`, `App.Application.Editor.cppm`, input (6, incl. new `:input.routing`), platform (5),
  player (2), renderer (3, `Types` is `.cppm`-only — no `Types.cpp`), scene camera (2), service (5, incl. new `service/App.Service.Client.cppm`), ui (1), window
  (3) as `FILE_SET CXX_MODULES`; `App.Application.cpp`, `App.Application.Editor.cpp` + per-area `.cpp` files
  privately. Links `engine.core/math/shader/rhi` internal-public, `glfw`/`mango` private, `imgui.base` + `imgui`
  public (so `import imgui;` resolves for importers).

## Flow

1. `Application::run()` (torn-down guard → `operation_not_permitted`) → `initialize()` → `PPR_DEFER shutdown()` →
   loop `while (not m_lifecycle->pollEvent())`: `tickThrottle(now, target)` when throttled else `tick(now)`,
   `update(m_application_clock.m_elapsed)` + `render()` with first-error-wins `PPR_RETAIN_ERROR_ON_FAIL`; exceptions map
   to exit clauses (`system_error→e.code()`, `invalid_argument`, `bad_alloc→not_enough_memory`, `...→state_not_recoverable`)
   via `m_request_exit`, `Log::flush()` per frame; exit retains `m_lifecycle->error()` as the final error.
2. `Application::initialize()` → `m_platform->initialize(*this)` → `PPR_DEFER`-on-failure `shutdown()` rollback
   (`success` flag; service registrations torn down explicitly via `shutdown()`, not the defer pair) → clock
   reset, `content/install` = executable parent (`install` aliases `content`), `working` = `current_path`
   (`config` untouched), `withCancelClause(background())` →
   if `m_domain.m_needs_rendering`: `IShaderService::get()->initialize()` + insert, `IRhiService::get()->initialize(
   DeviceType::Default, shader)` + insert, `make_unique<Renderer>()->initialize(rhi)`. `update()` =
   `TimerManager::mainTimer().tick()` + `m_platform->update(dt)`; `render()` = no-op default.
   `shutdown()` (latch set first, idempotent-success): `m_renderer->shutdown()` + reset → erase + shutdown RHI →
   erase + shutdown shader → `m_platform->shutdown(*this)` last; `PPR_RETAIN_ERROR_ON_FAIL` first-error accumulation.
3. `ApplicationEditor::initialize()` → `Application::initialize()` → `IWindowService::createWindow({title = getName(),
   1280x720})` + ignored-`setMainWindow` → `WindowInputContext(input_service, main_window)` + `WindowViewport(main_window,
   ViewportLayout{})` → `Player(PlayerIdentity{})` + `Camera(perspective)` + `InputMapping("camera_input_mapping")` +
   `FreeCameraController::provideInputActionKeyMappings` → player listener `addInputMapping(camera_mapping,
   camera)` + context `addInputListener(player.listener, player)` → `app_services.inject()` deducing
   `IRhiService/IShaderService` → `TrianglePass::initialize(rhi, shader, getContentDir())` →
   `ui::createImGuiService()->initialize(input_context, rhi, shader, ui_priority)` + `insert_or_assign(ui)`.
4. Per-frame editor: `update()` = `Application::update` → `viewport->updateFromWindow()` →
   `camera->updateModel(dt, controller, viewport->getViewport())` → `triangle_pass->update(dt, camera->getSnapshot())` →
   `ui_service->update(dt, *viewport)`; `render()` = `Application::render()` →
   `renderer.renderAndPresent(viewport->getWindow(), { *m_triangle_pass, *m_ui_service })`. `shutdown()` unwinds UI
   (`PPR_VERIFY erase` + shutdown + reset) → triangle pass → clear player mappings + frame messages (player itself not
   reset) → capture window from input context + `clearInputListeners` + reset context → assert viewport window matches +
   reset viewport → `setMainWindow(nullptr)` + `destroyWindow` → reset mapping/controller/camera →
   `Application::shutdown()`.
5. No runtime logic in `App.cppm` itself — compile-time re-export only.

## Integration

- **Consumers**: `game/main.cpp` (concrete application subclass), `engine.tests.app` (platform tests).
- **Depends on**: `engine.core` (services, context, delegates), `engine.math`, `engine.rhi`, `engine.shader`
  (via renderer/UI pass), external `glfw`/`mango`/`imgui`.
- **Provides**: every `engine.app:*` namespace; `Application` lifecycle + service-store accessors
  (`getServices()`, `getPlatform()`, `getRenderer()`, `getDomain()`, `getTimerClock()`); `ApplicationEditor` client
  ownership via `IClientService`.

## Key Files

- `App.cppm` — umbrella re-export list incl. `:application_editor` + `:service.client` (no runtime code).
- `App.Application.cppm/.cpp` — slim `Application` base: domain/run-loop/platform/services/shader-RHI-renderer bootstrap.
- `App.Application.Editor.cppm/.cpp` — `ApplicationEditor`: window/viewport/input/player/camera/triangle/UI ownership.
- `App.TemplateInstantiations.cpp` — explicit `Delegate`/`BroadcastCallback` instantiations.
- `CMakeLists.txt` — module + source registration, `setup_ppr_project` deps.
