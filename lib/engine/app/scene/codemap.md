# lib/engine/app/scene

## Responsibility

The `engine.app:scene.camera` module provides the camera home for the app layer: `Camera` (pose + projection state)
plus its controller, feeding `CameraSnapshot` into the renderer scene layer (`SceneView` in `:renderer.types`).

## Design

- **CameraModel** — pose and projection parameters (`m_origin`, `m_basis`, `m_fov`, `m_z_near`/`m_z_far`,
  `m_camera_mode`).
- **CameraSnapshot** — `CameraModel` plus derived matrices (`m_view`, `m_projection`, `m_view_projection` and
  inverses, jittered variants), basis vectors, and frustums.
- **Camera::updateModel(dt, new_model, viewport)** — derives basis vectors, builds the view matrix via
  `float4x4::lookat` (`App.Scene.Camera.cpp:80`), selects perspective/ortho projection, then composes
  `m_view * m_projection` (`App.Scene.Camera.cpp:103`).
- Degenerate (zero-extent) viewports keep prior state; camera cuts reset revision/velocity.

## Flow

1. Client builds a `CameraModel` → `Camera::updateModel(dt, model, viewport)` snapshots view/projection.
2. Snapshot pairs with a target-local `RenderView` as `SceneView` (`:renderer.types`).
3. `TrianglePass::draw()` uploads the snapshot-fed frame constants; per-frame submission stays in `Renderer`.

## Integration

- **Consumers**: `engine.app:renderer.triangle_pass` (snapshot upload), `SceneView` pairing in `:renderer.types`
- **Depends on**: `engine.core`, `engine.math`, `engine.rhi` (projection helpers), `:window.viewport` (client rect)
- **Provides**: `engine.app:scene.camera`, `engine.app:scene.camera.controller`

## Key Files

- `App.Scene.Camera.cppm` — `Camera`, `CameraModel`, `CameraSnapshot` declarations
- `App.Scene.Camera.cpp` — `updateModel` (lookat view, view*projection composition)
- `App.Scene.Camera.Controller.cppm` — camera controller declaration
- `App.Scene.Camera.Controller.cpp` — controller implementation
