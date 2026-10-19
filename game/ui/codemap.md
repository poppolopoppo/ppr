# game/ui/

## Responsibility

Game-owned ImGui presentation and its driver adapter. The gameplay host owns
simulation, camera mapping, transactions, cancellation and descriptor I/O. It
supplies synchronous presentation data and applies returned typed actions after
drawing. Engine UI continues to own ImGui lifecycle, input routing, static font
publication and detached GPU rendering.

## Navigation

| Module | Files | Responsibility |
|---|---|---|
| `game.ui` | `Game.UI.cppm/.cpp` | Initially viewport-fitted utilities with optional maximum constraint; ribbon world inset; modal actions; frontend capture; background drawing of projected feedback |
| `game.ui.model` | `Game.UI.Model.cppm/.cpp` | Resolves only queried cells, nearby agent and scalar world report from the driver into reusable `ColonyPresentation` storage |
| `game.colony.panel` | `ColonyPanel.cppm/.cpp` | Top simulation/management ribbon and Menu Quit; compact tool rail with native Build popup; left Colony/selection/tool context; independently visible Performance; native utilities |
| `game.colony.worldgen_panel` | `WorldGenPanel.cppm/.cpp` | Single workspace titled `Authoring Workspace###Worldgen Workshop`: File/Edit/View/Help menu bar, status line and pack toolbar; wide left preview-settings column plus editor and full-width asset strip, narrow Assets/Editor/Preview pane tabs; nested-property search matches keys, paths and values; descriptor-backed numeric controls pair a range slider with a typed exact entry clamped to the schema domain; deferred edits/selection and typed Start/Return/Quit |
| `game.colony.asset_browser_panel` | `AssetBrowserPanel.cppm/.cpp` | Embedded virtualized glyph grid with native multiselect, sorting, box selection and zoom; returns stable AssetKey without descriptor I/O/separate windows |
| `game.tests` | `Game.UI.Tests.cpp`, `AssetBrowserPanel.Tests.cpp` | `game/ui` geometry/capture/independent-window/native-menu/rail intents and workspace resize/edit-to-tile/identity/action contracts; `game/asset_browser` grid/order/zoom contracts |

The retained `game.colony.*` module names keep consumers stable after the file
move. Both executable providers are registered explicitly in
[`../CMakeLists.txt`](../CMakeLists.txt).

The existing [`../colony/Checkpoint.Tests.cpp`](../colony/Checkpoint.Tests.cpp)
restore fixture also tests production-model unresident metadata/nearest-agent
preservation and invalid-coordinate rejection without generating a world.

## Panel chrome

Every decorated panel has exactly one header: its window title bar. The heading
face is pushed only around `ImGui::Begin`/`BeginPopupModal` by
`ui::WindowTitleFont` and restored immediately after, so the body never inherits
it. The colony ribbon, tool rail, cursor Inspector and floating performance
widget use `NoTitleBar`/`NoDecoration`; they render no header and do not
construct the guard. Title bars use the accent-yellow tokens with ink text,
window bodies use the cream `kRaised` surface, borders use ink, the menu-bar
background matches the window body, and container/control rounding are both 2.

`ui::helpMarker(const char *)` in
[`App.UI.ImGui.cppm`](../../lib/engine/app/ui/App.UI.ImGui.cppm) is the shared
Font Awesome `?` affordance. It draws on the icon-merged body face and carries
its explanation in the hover tooltip. Panels use it for control documentation;
data, status, error, section-header and field-label text remain visible.

## Lifetime and cost

`ColonyPresentation` copies scalar and queried-cell values. Its names and the
driver's panel-safe, per-tick `AgentSummary` span are borrowed only for the
following synchronous draw. No tick or action dispatch occurs during that call,
and no borrow enters detached render packets. Feature-name storage and projected
feedback storage retain capacity between frames. The adapter keeps no driver
pointer. Aggregate `Game.UI.Model`, `Game.UI.Draw`, `Game.UI.ProjectFeedback` and
`Game.UI.Feedback` profiler scopes distinguish these phases without per-item
instrumentation.

World work vocabulary is declared independently in
[`../colony/WorldProgress.cppm`](../colony/WorldProgress.cppm), re-exported by the
existing WorldJob provider. Presentation does not import the application host.

The current design follows the warm Site Office palette and standard ImGui
controls. Only the full-width ribbon reserves an entire edge; finite native
windows use existing capture and background-gesture routing. The former right
inspector and colony bottom strip are removed; their supported controls/feedback
live in the left context. Hiding Colony preserves active-tool/error feedback in that
same compact surface. F3 toggles Performance independently. Native close and
Menu Quit use the host-owned dirty transition and asynchronous drain path.
The Performance window embeds the existing engine timing plots through a
synchronous const borrow of the host-owned widget; there is no separate graph
overlay over the colony/tool context. Sampling continues while hidden.

Default typography is Pixelify Sans 400/600, Big Shoulders Display 700 headings
and a 12 px Pixelify binding for the compact Inspector read-out, with Font
Awesome merged into body only. Reacquire face pointers each draw.
Font/backend ownership and deferred dynamic-texture work are recorded
in the [visual refinement plan](../../docs/plans/imgui-ricing-plan.md).
