# lib/engine/app/frame

## Responsibility

`engine.app:frame.packet` is the P0 serial-explicit owned frame packet: values and owned storage only, built on the
client thread after the camera update and consumed on the same thread by render. It carries the frame id, latched
timing, the copied `CameraSnapshot` (revision included), the copied `Viewport` plus viewport revision, copied
window metadata (title string, framebuffer size), and the generational surface claim (`m_surface_identity` +
`m_surface_generation`, P1). No producer `ScratchPad`, borrowed upload spans,
initializer-list backing, or `function_ref` escapes into or out of the packet.

## Design

- `FramePacket` — plain struct of owned copies: `m_frame_id`, `m_delta_time`/`m_total_elapsed` (manager-latched),
  `m_camera`, `m_viewport` + `m_viewport_revision`, `m_window_title`, `m_framebuffer_size`,
  `m_surface_identity` (from the live handle at capture) + `m_surface_generation` (stamped by the producer from
  `Renderer::surfaceGenerationFor`; `kSurfaceGenerationNone` when no record exists yet). The render-target verdict
  is derived from `m_framebuffer_size` alone (`hasRenderTarget`): zero-size/minimized is an explicit non-rendering
  success, and no stored flag can desync from the size.
- `makeFramePacket(...)` copies every field at capture time; `hasRenderTarget(...)` is pure. Both are defined in
  `App.Frame.Packet.cpp` (non-inline, non-template definitions live with the matching `.cpp`; the `.cppm` exports
  declarations only).
- Pass staging consumes the packet snapshot (`update(dt, packet.m_camera)`), never the live `Camera`, so a
  post-capture producer mutation cannot alter render preparation. `render()` consumes the staged packet single-use
  (fail closed when absent) and retires it the same frame.

## Flow

1. `ApplicationEditor::update()` → camera `updateModel` → `makeFramePacket(...)` → pass `update(dt, packet.m_camera)`
   → scene/UI work.
2. `ApplicationEditor::render()` → take + clear the staged packet → `renderPacket_(packet)` → `hasRenderTarget`
   guard → generational obsolescence guard (stale packets retire before any backend work) → uploads → encode → present → fence-value settlement into every pass/UI backend.

## Key Files

- `App.Frame.Packet.cppm` — `engine.app:frame.packet`: `FramePacket`, `hasRenderTarget`, `makeFramePacket`.
- `App.Frame.Packet.cpp` — non-inline definitions for the above.
