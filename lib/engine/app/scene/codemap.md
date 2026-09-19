# lib/engine/app/scene

## Responsibility

The `engine.app:scene.camera` module provides the camera home for the app layer: `Camera` (pose + projection state,
velocities, jitter, frusta) plus its controller hierarchy, feeding `CameraSnapshot` into the renderer scene layer
(`SceneView` in `:renderer.types`). Moved here from `lib/engine/app/camera/` (`App.Camera.cpp/.cppm` deleted); the old
location no longer exists.

## Design

- **CameraModel** — pose and projection parameters (`m_origin`, `m_basis`, `m_fov`, `m_z_near`/`m_z_far`,
  `m_camera_mode`, `m_has_camera_cut` teleport flag).
- **CameraSnapshot** — `CameraModel` plus derived matrices (`m_view`, `m_projection`, `m_view_projection` and
  inverses, jittered variants), basis vectors (`m_right`/`m_up`/`m_forward`), viewport size/aspect, `m_jitter` NDC
  offset, `m_frustum`/`m_ray_frustum`, and `m_revision`.
- **Camera::updateModel(dt, new_model, viewport)** — asserts normalized basis and valid fov/depth range; returns early
  on degenerate (zero-extent) viewports keeping prior state; shifts previous snapshot, bumps/resets `m_revision` (first
  frame and any cut reset to 0), derives basis vectors via `quaternionTransform`, builds the view matrix via
  `float4x4::lookat` (`App.Scene.Camera.cpp:83`), selects perspective (`rhi::getPerspectiveMatrix`) / ortho
  (`rhi::getOrthoMatrix`) projection, then composes `m_view * m_projection` with inverses; converts the active jitter
  pixel offset to NDC (`pixel / (size / 2)`) and composes `projection * makeJitterMatrix`; pins frusta to the unjittered
  D3D `[0,1]`-remapped VP (`makeZeroToOneFrustum`, `RayFrustum` over the same corrected matrix); zeroes velocities on
  cuts/near-zero dt, else derives translational (`Δorigin/dt`) and angular (`angularVelocity`) velocities.
- **Camera modes/extras** — `setCameraMode` and `setJitterSamples` (non-owning `TransformView<float2>` view, empty
  resets) arm `m_has_camera_cut_next_frame`; `signalCameraCutNextFrame()` exposes the same; controller overload
  `updateModel(dt, ICameraController&, viewport)` copies current state, runs `controller.updateCameraModel`, then
  snapshots; full getter surface (pose, basis vectors, matrices, frusta, velocities, jitter).
- **ICameraController** (`safe_object`) — `provideInputActionKeyMappings(out_mapping)` (const, fills bindings) +
  `updateCameraModel(dt, model)` + `resetInputState()` (noop default; `Basic` clears rate maps, deltas, impulses, and
  mouse-look for focus-loss/device-reset); `DummyCameraController` is the noop placeholder.
- **BasicCameraController** (`details::`) — owns five `unique_ptr<InputAction>` (`CameraMove` axis_3d, `CameraRotate`
  axis_2d, `CameraSpeed`/`CameraFov` axis_1d, `CameraLook` digital) wired in the ctor: translate accumulates
  `m_delta_position`, rotate accumulates quaternion deltas (absolute for keys/sticks, relative only when RMB
  `m_has_mouse_look` for `mouse_2d`), speed/fov `addClamp` into `FilteredAnalog<float>` ranges, look started/completed
  toggles mouse-look; `FilteredAnalog` pose state (rotation `Quaternion`, position `float3`, fov, speed multiplier) plus
   sensitivity/speed tuning (`m_mouse_sensitivity`, `m_gamepad_sensitivity`, translate/rotate speeds, fov/speed ranges);
   convergence rates follow the `FilteredAnalog` first-order-lag contract (`alpha = 1−exp(−λ·dt)`): position/rotation
   `8.0` (~125 ms), fov `0.8`, speed `2.5`;
   `updateCameraModel` ticks fov/speed filters, calls virtual `updateCameraPose_`, publishes `m_fov`/`m_has_camera_cut`,
  then clears deltas/teleport; base `updateCameraPose_` blends rotation deltas, world-transforms position deltas by
  speed × orientation, and publishes filtered origin/basis; base `provideInputActionKeyMappings` binds RMB look,
  Shift/Ctrl + shoulder speed modifiers and `+/-` fov modifiers via `InputAction::modulate` range-scaled samplers.
- **FreeCameraController** — free-flight: `lookAt(eye,target,up)` / `lookAt(eye,heading,pitch)` with teleport
  (`reset`) vs smooth (`setRaw`) paths; binds WASD/arrows/`PgUp`/`PgDn`/D-pad translate, left-stick 2D→3D, Q/E +
  mouse-2D + right-stick rotate, wheel fov on top of the base bindings.
- **PanCameraController** — plane-parallel pan: `setParallelPlane(normal,up | basis)` + `translate(eye)` with
  teleport/smooth paths; rebinds translate to plane axes (WASD/arrows/D-pad/Q/E), mouse-2D `{x,y}→{x,y,0}` and wheel
  `{x}→{0,x}` lifts, left stick planar, right stick forward-only (`{0,0,y}` mask).
- **OrbitCameraController** — orbit-around-target: `lookAt(eye,target)` / `setOrbitTarget` / `setOrbitRadius`
  (epsilon-clamped radius, pole-safe `safeOrbitRight_`, `orbitOrigin_ = target − forward×radius`); overrides
  `updateCameraPose_` so translate-Z dollies `m_radius_analog` and the origin is recomputed from basis/target/radius
  each frame; binds W/S + arrows/D-pad dolly, wheel dolly, A/D + arrows + D-pad + sticks rotate.

## Flow

1. Client builds a `CameraModel` directly, or owns an `ICameraController` whose actions are published via
   `provideInputActionKeyMappings` into an `InputMapping` consumed by an `InputListener`.
2. Per-frame input callbacks accumulate `m_delta_position`/`m_delta_rotation` (speed/fov filters update inline);
   `Camera::updateModel(dt, controller, viewport)` runs `controller.updateCameraModel` → `updateCameraPose_` then
   snapshots view/projection/jitter/frusta/velocities (degenerate viewports skip in place).
3. Snapshot pairs with a target-local `RenderView` as `SceneView` (`:renderer.types`).
4. `TrianglePass::draw()` uploads the snapshot-fed frame constants; per-frame submission stays in `Renderer`.

## Integration

- **Consumers**: `engine.app:renderer.triangle_pass` (snapshot upload), `SceneView` pairing in `:renderer.types`
- **Depends on**: `engine.core` (safe_object, TimeSpan), `engine.math` (float/quaternion/matrix, frusta, angular
  velocity), `engine.rhi` (projection helpers), `:window.viewport` (client rect), `:input.action` + `:input.filtered_analog` + `:service.input` (controller actions/bindings)
- **Provides**: `engine.app:scene.camera`, `engine.app:scene.camera.controller`

## Key Files

- `App.Scene.Camera.cppm` — `Camera`, `CameraModel`, `CameraSnapshot`, `ECameraProjection`, `CameraJitterSamples` declarations
- `App.Scene.Camera.cpp` — `updateModel` (lookat view, view*projection composition, jitter, frusta, velocities)
- `App.Scene.Camera.Controller.cppm` — `ICameraController`, `DummyCameraController`, `Basic/Free/Pan/OrbitCameraController` declarations
- `App.Scene.Camera.Controller.cpp` — controller action wiring, pose updates, per-controller key mappings
