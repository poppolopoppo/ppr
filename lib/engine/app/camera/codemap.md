# lib/engine/app/camera

## Responsibility
The `engine.app:camera` module provides camera abstractions used by the renderer to compute view-projection matrices and pass per-frame constants to shaders. The camera type is lightweight and designed to be stored and updated per-frame, with its state consumed by the renderer's draw callbacks to set the `g_frame` constant buffer. Cameras are registered in the scene services store (`Application.m_scene_services`) so they can be looked up via the service locator pattern.

## Design
- **Camera** class — provides core camera operations: `view()`, `projection()`, `viewProjection()`, `inverseViewProjection()`, `position()`, `viewportSize()`, `cameraVersion()`. The camera version is a `u64` that increments whenever the camera is modified, allowing the renderer to cache computed frame constants and only re-upload when the camera actually changes.
- Camera matrices follow the engine's row-major convention with column-major HLSL compatibility — the shader expects `mul(float4, float4x4)` (row-vector convention). The camera computes its matrices in row-major order matching the engine's storage convention.
- `view()` — computes the view matrix from camera position/orientation; typically a look-at matrix.
- `projection()` — computes the projection matrix; perspective projection with engine conventions (depth range matching the selected RHI projection convention).
- `viewProjection()` — `projection() * view()` (matrix multiply in row-major order).
- `inverseViewProjection()` — inverse of the view-projection matrix, used for view-space position reconstruction.
- `position()` — returns the camera's world-space position as `float4`.
- `velocity()` / `setVelocity()` — optional camera motion tracking (not fully utilized in current code but part of the `FrameConstants` struct).
- `viewportSize()` — returns the current framebuffer size as `float2`.
- Cameras are not owned by the renderer — the renderer receives a `const Camera*` from the draw callback's captured environment, and looks up the camera from the scene services store (`m_scene_services.tryGet<ICameraService>()`).
- `ICameraService` is registered in `Application::initialize()` via `m_scene_services.insert(safe_ptr<ICameraService>{&m_camera_service})`, and the camera service (`CameraService`) is a member of `Application` that holds the active camera state.

## Flow
1. Camera service initialization: `Application::initialize()` → `m_camera_service.initialize(*m_services.get<IInputService>())` — sets up the initial camera based on input replay or free-cam controller
2. Per-frame update: `Application::update()` → `m_camera_service.update(dt)` — processes input (WASD/arrow keys, mouse movement) to update camera position/orientation and increments `cameraVersion()`
3. Render pass: In `Application::render()`, the scene draw callback captures `m_scene_services.tryGet<ICameraService>()` — if valid, calls `cam_svc->camera()` to get the `Camera` reference, then passes it to `renderer->drawTriangle(pass, cam_svc->camera(), viewport, scissor)`
4. Renderer uses the camera to fill `FrameConstants`:
   - `frame.m_view = camera.view()`
   - `frame.m_projection = camera.projection()`
   - `frame.m_view_projection = camera.viewProjection()`
   - `frame.m_camera_position = float4{camera.position(), 0.0f}`
   - These are uploaded via `m_frame_cursor.setData(&frame, sizeof(FrameConstants))` only if the camera version has changed since the last frame
5. Camera shutdown: `Application::shutdown()` → `m_camera_service.deactivateController()` — deactivates the free-cam controller

## Integration
- **Consumers**: `Application` (holds `CameraService m_camera_service`, registers `ICameraService` in `m_scene_services`, calls `m_camera_service.update(dt)` per frame), `Renderer` (receives `const Camera*` from draw callback, uses it to fill `FrameConstants`), gameplay systems that may want to create custom camera types
- **Depends on**: `engine.core` (safe_ptr, safe_object), `engine.math` (float4x4, float4, float2, int2, vector operations, dot, normalize, lerp), `engine.rhi` (for projection convention integration), `std` (chrono for dt)
- **Provides**: `engine.app:camera` module namespace with `Camera` class (forward declaration; full definition in `App.Camera.cppm`/`.cpp`)
- **Used by**: `Application::render()` (scene_draw lambda captures `m_scene_services` and calls `cam_svc->camera()` to get Camera), `Renderer::drawTriangle()` (uses Camera to compute FrameConstants and upload via m_frame_cursor), `CameraService` (member of Application, implements camera update logic)

## Key Files
- `App.Camera.cppm` — `Camera` class declaration (view, projection, viewProjection, inverseViewProjection, position, viewportSize, cameraVersion)
- `App.Camera.cpp` — Camera method implementations (not yet read in detail, but contains the matrix computation logic)
- `App.Camera.cppm` also likely defines `CameraService` or the camera service is in a related partition