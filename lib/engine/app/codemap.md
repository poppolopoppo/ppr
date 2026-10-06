# lib/engine/app

## Responsibility

`engine.app` is the application-layer umbrella module: the single compile-time aggregation point (`import engine.app;`)
for application lifecycle, input, player, scene camera, services, window, platform, renderer, and UI.
`pP::Application` (`:application`) is the slim lifecycle base — `ApplicationDomain`, run-loop, `IPlatform`,
`ServicesStore`, shader/RHI/`Renderer` bootstrap, directory resolution. `pP::ApplicationEditor`
(`:application_editor`) is the interactive-client subclass that owns window, viewport, input context, player, camera,
  triangle and grid passes, and ImGui service, and implements `IClientService`. `game/main.cpp` drives `run()` on the concrete
application.

## Design

- Umbrella `App.cppm` only `export import`s partitions — `:application` + `:application_editor`, `:frame.packet`,
  `:input.*` (7, incl. `:input.batch` owned pump batches),
  `:player` + `:player.graph`, `:scene.camera` + `:scene.camera.controller`, `:service.client` + `:service.input` +
  `:service.player` + `:service.ui` + `:service.window`, `:window.*` (viewport/handle/monitor), `:platform`,
  `:renderer` + `:renderer.gpu_caches` + `:renderer.grid_pass` + `:renderer.triangle_pass` + `:renderer.types`,
  `:ui.imgui`.
- `Application` (`App.Application.cppm/.cpp`, `module engine.app:application`) — `safe_object` subclass with virtual
  `initialize/update/render/shutdown` returning `std::error_code`. Owns `unique_ptr<Renderer>`, `TimerManager`
  `m_application_clock`, `optional<TimeSpan> m_target_frame_time` plus `m_has_background_priority` (background 5 fps
  throttle surfaced via `getTargetFrameTime`), `const unique_ptr<IPlatform> m_platform`
  (declared before `ServicesStore m_services` so reverse-destruction releases store observers before platform owners),
  `SharedContext m_lifecycle` + `context::CancelClauseFunc m_request_exit`,
  `Array<string>` args, `string` name, `const ApplicationDomain m_domain`, `directory_entry` dirs
  (install/content/working resolved; config left default). Ctor wires `IPlatform::create()` plus
  `hal::disableSystemErrorReporting/installDebugAssertHooks/setThreadName("Application")` + `main_group` (`App.Application.cpp:31`). Forward-declares `IClientService`. Accessors:
  `getName/Arguments/Domain/Platform/Lifecycle` (const ref), `getRenderer/Services/TimerManager` (const + mutable overloads),
  `getTargetFrameTime`, dir getters; `requestExit(clause = {})` (empty raises only the `m_exit_requested` latch — `run()` returns success; nonzero latches the cause + emits), `isExitRequested()` (latch OR latched cancel error), `setBackgroundPriority(throttle)`,
  `setTargetFrameDuration(TimeDuration/nullopt)`,
  `setTargetFrameRate(fps)` (non-positive clears throttle), `run()`.
- `ApplicationDomain` — immutable-after-construction bitfield struct: `m_is_headless` (false), `m_is_interactive`
  (true), `m_needs_presence` (false), `m_needs_rendering` (true), `m_needs_user_interface` (true); `ApplicationEditor`
  fixes these values at construction.
- `ApplicationEditor` (`App.Application.Editor.cppm/.cpp`, `module engine.app:application_editor`) — `public
  Application, protected IClientService`. Owns `unique_ptr<Player>`, `unique_ptr<Camera>`,
  `unique_ptr<ICameraController>`, `unique_ptr<WindowInputContext> m_main_input_context`,
  `unique_ptr<InputMapping> m_camera_input_mapping`, `unique_ptr<IUIService>`, `unique_ptr<WindowViewport>`,
   `unique_ptr<TrianglePass>` + `unique_ptr<GridPass>`. Implements `IClientService` const + mutable `getMainCamera/Player/Viewport/InputContext`
   getters (input getters wrap `&m_main_input_context->m_context`) plus protected `getApplication()` returning `this`.
   Owns `unique_ptr<InputBackgroundLatch> m_input_background_latch` (registrar == owner; actuator borrows it,
   detach-before-destroy) + device-disconnect handle. Private `onMainWindowFocused_(window, focused)` throttles to
   5 fps via `setBackgroundPriority` and, on focus loss, resets the routing latch + camera-controller motion state.
    Public `getGridPass()` exposes editor-owned tile staging; `replaceMainCameraController(unique_ptr<ICameraController>,
    ECameraProjection)` prepares a fresh mapping, detaches the previous mapping and resets input, then attaches the
    replacement at camera priority (invalid null → `invalid_argument`; absent editor state → `not_connected`; projection + cut `App.Application.Editor.cpp:224-225`).
    Input-chain order comes from `:service.input`: listener `EInputListenerPriority { ui = 0, detector = 1, player = 2 }`
   with mapping `EInputMappingPriority::camera = 1`.
- `App.TemplateInstantiations.cpp` pins explicit instantiations for `Delegate` (window events incl. key/mouse/char
  overloads) and `BroadcastCallback` (monitor/window/input/player/time) so downstream users don't pay
  implicit-instantiation cost. Linux/clang bring-up: the seven `extern template` pins removed
  from `:window.handle` are covered here by explicit `Delegate` window-event instantiations instead, keeping the header
  free of per-TU instantiation pins.
- Linux/clang bring-up summary (detail per area): GLFW native headers gated per OS + complete-type partition imports
  (`platform/glfw`); `[[nodiscard]]`-free friend declarations with namespace-scope attribute declarations (`input.key`);
  `:input.action` imported by the scene controller (`scene`); conditional `std23::move_only_function` callback aliases
  (`input`, `service`); explicit mango dims (`<float, 2u/3u/4u>`, `toFloat<int, 2u>`) at the `int`-vs-`u32` boundary
  (`input`, `scene`, `ui`, `window`); `[[fallthrough]]` + index-loop `Listener` fixes (`input`); `checked_cast` +
  `static_cast<double/u32/u64>` conversions (`input`, `platform/glfw`, `ui`); `.indexBuffer = {}` (`renderer`);
  `[[maybe_unused]]` debug helpers (`ui`).
- CMakeLists registers `App.cppm`, `App.Application.cppm`, `App.Application.Editor.cppm`, input (7, incl. `:input.batch` owned pump batches + `:input.routing`), frame (1, `:frame.packet`), platform (5),
  player (2), renderer (5, `Types` is `.cppm`-only — no `Types.cpp`), scene camera (2 — `App.Scene.World.cppm` exists on
  disk but is unregistered, see `scene/codemap.md`), service (5, incl. new `service/App.Service.Client.cppm`), ui (1), window
  (3) as `FILE_SET CXX_MODULES`; `App.Application.cpp`, `App.Application.Editor.cpp` + per-area `.cpp` files
  privately. Links `engine.core/math/shader/rhi/image/mesh` internal-public, `glfw`/`mango` private, `imgui.base` + `imgui`
  public (so `import imgui;` resolves for importers).
- Linux bring-up conformance (no lifecycle/renderer contract change): call sites spell the same explicit dim
  args math requires (`opaqueValue<float,2u/3u>` on the axis structs — same mango `int`-dim vs engine
  `u32`-dim boundary); `std23::move_only_function` without `const noexcept`, explicit `[[fallthrough]]`,
  index loops over `views::enumerate`, no `[[nodiscard]]` on friend declarations — all Clang/libc++ strictness.

## Flow

1. `Application::run()` → `initialize()` (re-arms the `m_exit_requested` latch) → fused `finishRun_()` on every
   path (first error retained across `shutdown()` + `Log::flush(true)`; loop error read BEFORE `shutdown()` resets the
   lifecycle; exception-only safety-net defer otherwise) → loop `while (not isExitRequested())` — exit latch OR latched
   cancel error, never the consumable event pulse alone: `advanceClock_()` (wakeable `FramePacer` wait on the
   exit/error latch with 2ms quantum, then zero-target `m_application_clock.tick()` — `now()` stays latched) →
   post-wait admission recheck (`break` retires the frame without client work) → `update(dt)` + `render()` with
   first-error-wins `PPR_RETAIN_ERROR_ON_FAIL`; exceptions map to exit clauses (`system_error→e.code()`,
   `invalid_argument`, `bad_alloc→not_enough_memory`, `...→state_not_recoverable`) via `m_request_exit`; no per-frame
   flush — shutdown drain only.
2. `Application::initialize()` → clock reset (`App.Application.cpp:141`) BEFORE platform init (`:151`) → `PPR_DEFER`-on-failure `shutdown()` rollback
   (`success` flag; service registrations torn down explicitly via `shutdown()`, not the defer pair) →
   `content/install` = executable parent (`install` aliases `content`), `working` = `current_path`
   (`config` untouched), `withCancelClause(background())` →
   if `m_domain.m_needs_rendering`: `IShaderService::get()->initialize()` + insert, `IRhiService::get()->initialize(
   DeviceType::Default, shader)` + insert, `make_unique<Renderer>()->initialize(rhi)`. `update()` =
   `not_connected` guard + `m_platform->update(dt)` (`App.Application.cpp:243-250`); `render()` = no-op default.
   `shutdown()` (idempotent-success): `m_renderer->shutdown()` + reset → shutdown RHI then erase
   on success → shutdown shader then erase on success → `m_platform->shutdown(*this)` last;
   `PPR_RETAIN_ERROR_ON_FAIL` first-error accumulation.
3. `ApplicationEditor::initialize()` → `Application::initialize()` → `Player(PlayerIdentity{})` +
   `Camera(perspective)` + `InputMapping("camera_input_mapping")` + `FreeCameraController::lookAt`/`provideInputActionKeyMappings`
   → player listener `addInputMapping(camera_mapping, EInputMappingPriority::camera)` → `WindowInputContext(input_service)`
   → `InputBackgroundLatch(ui, player-listener, player, detector)` + device-disconnect handle (resets latch + controller
   motion) → `app_services.inject()` deducing `IRhiService/IShaderService` → `TrianglePass::initialize(rhi, shader,
    getContentDir())` → `GridPass::initialize(rhi, shader, getContentDir())` →
    `ui::createImGuiService()->initialize(input_context, rhi, shader, latch.m_foreground_priority)` →
   `latch.initialize(context, ui-listener)` → `IWindowService::createWindow({title = getName(), 1280x720})` → refresh-rate
   throttle from the monitor video mode → `WindowViewport(main_window, ViewportLayout{})` →
   `input_context.initialize(main_window)` + ignored-`setMainWindow` + `m_when_focused` subscription → `insert_or_assign(ui)`.
4. Per-frame editor: `update()` = `Application::update` → `viewport->updateFromWindow()` →
   `camera->updateModel(dt, controller, viewport->getViewport())` →
   `triangle_pass->update(dt, packet.m_camera)` + `grid_pass->update(dt, packet.m_camera)` (packet-owned snapshot;
    passes never consult the mutable `Camera` while drawing) + `submitSceneInstances_()` when a scene is loaded →
   `ui_service->update(dt, *viewport)`; the owned `FramePacket` (P0: camera/viewport/window copies) is built right
   after the camera update and before pass staging (same-thread consume); `render()` = `Application::render()` → consume the staged packet (fail closed when absent;
   zero-size/minimized is an explicit non-rendering success) → two explicit surface passes:
    GridPass prepares visible pending cell-atlas uploads before `renderAndPresent` (RHI device from services;
    renderer-owned `waitOnHost` fence on demand). GridPass and TrianglePass encode in the same renderer-owned-depth surface pass,
    then ImGui selects no depth and loads the existing color
   attachment; both render before one present. `shutdown()` starts with `unloadScene()` (`App.Application.Editor.cpp:338-340`) + `destroyWindowSurface` (`:399`), then detaches the
   device-disconnect handle first, resets controller motion, shuts down + resets the latch (detector-first detach),
    erases + shuts down UI, grid pass, triangle pass, clears player mappings + frame messages, shuts down + clears the input context,
   resets viewport, drops the focus subscription, destroys the window, resets mapping/controller/camera, then
   `Application::shutdown()`.
5. Titles (P0 no-op record): ZERO live title producers — `renameWindow` has no callers, so no
   coalescing/rate-limiting exists. `WindowModel::m_title` is copied render metadata only
   (see `FramePacket::m_window_title`).
6. No runtime logic in `App.cppm` itself — compile-time re-export only.

## Integration

- **Consumers**: `game/main.cpp` (concrete application subclass), `engine.tests.app` (platform tests).
- **Depends on**: `engine.core` (services, context, delegates), `engine.math`, `engine.rhi`, `engine.shader`
  (via renderer/UI pass), `engine.image` + `engine.mesh` (public, `CMakeLists.txt:86`; cache upload types), external `glfw`/`mango`/`imgui`.
- **Asset consumer**: pass caches upload `engine.image`/`engine.mesh` CPU types (no sim types at the boundary).
- **Provides**: every `engine.app:*` namespace; `Application` lifecycle + service-store accessors
  (`getServices()`, `getPlatform()`, `getRenderer()`, `getDomain()`, `getTimerManager()`); `ApplicationEditor` client
  ownership via `IClientService`.

## Key Files

- `App.cppm` — umbrella re-export list incl. `:application_editor` + `:service.client` (no runtime code).
- `App.Application.cppm/.cpp` — slim `Application` base: domain/run-loop/platform/services/shader-RHI-renderer bootstrap.
- `App.Application.Editor.cppm/.cpp` — `ApplicationEditor`: window/viewport/input/player/camera/grid/triangle/UI ownership.
- `frame/App.Frame.Packet.cppm/.cpp` — `engine.app:frame.packet`: owned `FramePacket` (camera/viewport/window copies),
  `makeFramePacket` capture, `hasRenderTarget` derived-from-size guard (see `frame/codemap.md`).
- `App.TemplateInstantiations.cpp` — explicit `Delegate`/`BroadcastCallback` instantiations.
- `CMakeLists.txt` — module + source registration, `setup_ppr_project` deps.
