# lib/engine/app/ui/

## Responsibility

ImGui frontend and GPU overlay backend behind `IUIService`. `ui::createImGuiService()` returns an owning `unique_ptr<IUIService>`; `ImGuiService` is implemented privately in `App.UI.ImGui.cpp`. The editor owns it; worker execution does not transfer ownership.

## Thread and data boundary

- Main/client owns the live ImGui context, input routing, DPI/frame setup and UI construction. `update(dt, viewport)` begins frontend work. `buildDetachedUiFrame` completes `ImGui::Render()` and deep-copies vertices, indices and commands into `DetachedUiDrawData`.
- Detached commands carry baked framebuffer clip/scale data and opaque texture identities. Unknown texture identities are rejected; arbitrary client callbacks do not cross the lane. A known reset-render-state command is represented explicitly.
- The render owner lane calls `renderDetachedUiFrame(detached, draw_context)`. It uses detached inputs and lane-confined GPU state, never live ImGui IO/context/GetDrawData. The inline reference consumes the same payload shape.
- Font rebuilds publish owned pixels, dimensions and a version. The worker installs the atlas and reports its version; results acknowledge the publication. A dropped frame must not lose an unacknowledged font update.
- `IFenceAwareUiService::notifyFrameSubmitted` associates upload/resource use with the accepted submit's fence value. Buffer ring capacity is not completion proof. The editor stamps accepted work even when presentation subsequently fails.

The embedded `kImGuiShader` is compiled from source text; there is no separate ImGui `.slang` asset. UI is encoded after grid/triangle work, loading the surface color attachment without attaching scene depth.

## Lifecycle and navigation

Initialize with input context, shader/RHI references and listener priority. Frontend input/capture behavior uses the routing latch described in [input](../input/codemap.md). Editor teardown first stops submissions and establishes GPU quiescence; GPU cleanup stays on the owner lane and live context/native work stays on main. See [application flow](../codemap.md) for complete partial-init and shutdown ordering.

- `App.UI.ImGui.cppm`: factory, detached UI vocabulary, fence-awareness seam and conversion helpers.
- `App.UI.ImGui.cpp`: frontend/backend implementation, embedded shader, upload/resource management, DPI and font handling.
- `../service/App.Service.UI.cppm`: lifecycle/frontend/detached rendering contract.
- [frame map](../frame/codemap.md): request/result transport and font acknowledgment.
- `../../tests/app/App.ImGuiRouting.Tests.cpp`, `App.ImguiDpi.Tests.cpp`, and asset render gates cover relevant behavior; owning test CMake files establish registration.
