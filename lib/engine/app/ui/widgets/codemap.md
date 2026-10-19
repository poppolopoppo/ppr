# lib/engine/app/ui/widgets/

Reusable ImGui widgets in the public `engine.app` module. Clients own state and
supply explicit values or borrowed views. Widgets do not resolve services,
submit display transactions, read clocks, or own game simulation state.

| Partition | Files | Responsibility |
|---|---|---|
| `:ui.widgets.performance` | `App.UI.Widgets.Performance.cppm/.cpp` | Bounded `RingBuffer` timing history, average/percentiles and CPU frame/render and completed GPU frame plots |
| `:ui.widgets.display` | `App.UI.Widgets.Display.cppm/.cpp` | Display draft controls, capability choices, requested/device telemetry and confirmation presentation |

The display widget borrows editable draft fields through `DisplayWidgetDraft` and
returns one `EDisplayWidgetAction`. Its caller supplies validation/dirty state,
switching availability and trial status; the caller implements synchronization,
validation, submission, confirmation timing, persistence and error handling.
`auto` remains a monitor preference and resolves to the reported primary monitor
for available mode choices. Capability/geometry pointers and spans must remain
alive for the draw call.

Both partitions are exported from `../../App.cppm` and registered in
`../../CMakeLists.txt`. The colony panel adapts game display policy to the neutral
widget seam; the game consumes the timing widget. See the
[UI map](../codemap.md) for frontend/render ownership.

GPU samples have their own bounded history and completed-submission identity.
Repeated or older observations are ignored, so a delayed GPU result is not
counted again on every CPU frame. Missing initial samples show waiting/unavailable.

`PerformanceWidget::drawContents` draws those existing plots in the caller's
current window, sizing plots to available content width. The game embeds it in
its independent Performance window; there is no overlapping standalone graph
overlay. `draw(bottom_inset)` remains the standalone native-window wrapper for
other clients. Sampling and history ownership remain client-owned regardless of
window visibility; synchronous draws borrow the widget without retaining it.
