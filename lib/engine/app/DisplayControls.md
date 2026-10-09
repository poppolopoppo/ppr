# Display switching interface (panel integration)

All calls below are application/main-native-thread calls. The panel owns drafts,
confirmation state and injected-time countdown; it never calls window services
or the renderer. No borderless mode, present-mode enum or image-count control is
advertised.

## Schema

`DisplaySettings::describe()` is the authoritative metadata/CLI visitor. New
properties are runtime-editable, allowed from all configuration sources and
persistable:

| Canonical key | C++ member/type | Default / interpretation |
|---|---|---|
| `display.window_mode` | `EDisplayWindowMode m_window_mode` | `windowed`; domain is `windowed`, `fullscreen` (exclusive) |
| `display.monitor` | `std::string m_monitor` | `"auto"`; otherwise an exact, unambiguous enumerated name |
| `display.width` | `u32 m_width` | `0` |
| `display.height` | `u32 m_height` | `0` |
| `display.refresh_hz` | `u32 m_refresh_hz` | `0` (platform default, **not** known refresh) |

Dimensions must both be zero or both positive and fit native signed integers.
Windowed mode cannot request refresh. Zero dimensions select the monitor's current
exclusive resolution, or retain windowed geometry; returning from fullscreen
restores the last windowed geometry when available. Explicit exclusive
resolution/refresh must match an enumerated mode. Refresh absence in a capability
or observation is unknown and never proves support for an explicit Hz request.

Monitor names are preferences, not stable hardware identifiers. Duplicate names
are rejected as ambiguous; `auto` is reserved for primary-monitor selection.
Preferences are revalidated at request time and again immediately before native
apply/rollback. An absent preference fails visibly; it is not silently substituted.
No native monitor handle appears in the schema or serialized document.

## Panel-facing API

```cpp
Expected<DisplayCapabilities> Application::displayCapabilities() const;
Expected<WindowDisplayGeometry> Application::displayWindowGeometry() const;
std::error_code Application::requestDisplaySettings(
    config::Resolved<DisplaySettings> candidate, config::Diagnostics &diagnostics);
const DisplayController &Application::displayController() const noexcept;
std::error_code DisplaySettings::validate(
    const DisplaySettings &settings, config::Diagnostics &diagnostics);
std::error_code validateDisplaySettings(
    const DisplaySettings &settings, const DisplayCapabilities &capabilities,
    config::Diagnostics &diagnostics);
std::error_code DisplayController::begin(
    config::Resolved<DisplaySettings> candidate, const DisplayCapabilities &capabilities,
    config::Diagnostics &diagnostics);
```

The existing one-argument request/begin overloads remain compatibility adapters.
Use the diagnostic overload for user-facing rejection details. The pure
controller never enumerates services; inject the owned capability snapshot.

`DisplayCapabilities` owns `m_monitors` (name, primary flag, current mode and
available modes), `m_window_changes` and `m_exclusive_refresh`. Each
`DisplayModeCapability` contains `u32 m_width`, `u32 m_height` and
`std::optional<u32> m_refresh_hz`. Capability enumeration failure is an error,
not an empty list presented as successful enumeration. With no window service,
the successful view has no monitors and both support flags false.

`WindowDisplayGeometry` owns position, logical window size, framebuffer size,
optional observed exclusive refresh, `m_fullscreen` and an observed monitor name.
Windowed monitor/refresh observations are unavailable (empty name / nullopt),
not guesses about which physical monitor overlaps the window. Geometry may
already reflect a pending OS change; only the controller's acknowledged revision
is committed configuration. Query failures remain errors.

## Apply, Keep and Revert

1. Capture the previous `controller.resolved()` before applying a disruptive draft.
2. Resolve runtime assignments through the configuration visitor, then submit
   the owned candidate with `requestDisplaySettings(candidate, diagnostics)`.
3. Show pending state until the matching acknowledgement publishes a new
   `resolved().m_revision` and the transaction is idle. Queuing is not success.
4. Start the caller-owned injected-time confirmation countdown **after** this
   successful acknowledgement. Keep only dismisses that confirmation; no second
   engine operation is needed.
5. Revert/timeout submits the captured prior values through the same request API
   (or a newly resolved runtime layer with those values). It is another
   acknowledged transaction, not a game-side `setWindowMonitor` call. Do not
   overlap it with an outstanding transaction. A failed revert remains an error.
6. Save only acknowledged committed values, using persistability metadata. Never
   save native geometry telemetry or a monitor handle as configuration.

The executable's combined startup/draft schema must project **all nine** display
descriptors and values; `displayCliOptions()` itself automatically visits all
nine. Game/panel projection and controls are a separate integration lane.

## Coordination and service contracts

After suspending admission and draining the existing reservation ledger,
Application applies native window intent once per transaction on the main thread.
The returned **framebuffer** extent and vsync travel together in the existing
`RenderSurfaceRequest`; only the owner lane reconfigures the surface. Full control
queues leave the candidate visibly unsubmitted and do not repeat native apply.
Matching transaction ID, generation and window lifetime guard acknowledgements;
only success publishes configuration/provenance/revision. The existing suspended
completion pump remains responsible for progress without another client frame.

Failure restores the captured actual prior native mode/geometry, then sends the
restored extent and committed vsync through the same lane path. A failed native
restore reports an unavailable/degraded observation and its restoration error;
it cannot be mislabeled as a usable committed candidate. Shutdown cannot simply
discard a candidate after native mutation: cancellation schedules restoration,
and existing owner-lane handoffs attempt to settle it before backend quarantine.
Unsettled/failed shutdown restoration is retained as an operational error.
Recreated windows are never targeted with an old transaction's restoration.

At quiescent editor startup, native intent is validated/applied before the first
surface exists. Its resulting geometry feeds first surface creation on the owner
lane. Failed initial apply follows ordinary initialization rollback.

The service seam is:

```cpp
bool IWindowService::supportsDisplayWindowChanges() const noexcept;
bool IWindowService::supportsExclusiveRefresh() const noexcept;
Expected<WindowDisplayGeometry> IWindowService::displayWindowGeometry(const Window &) const;
Expected<WindowDisplayGeometry> IWindowService::configureDisplayWindow(
    const Window &, const Monitor *, const int2 &position, const int2 &size,
    std::optional<u32> refresh_hz);
```

Null monitor means windowed; nullopt refresh means native default. Unsupported
services return `operation_not_supported` and advertise neither capability.
GLFW implements both modes and optional exclusive refresh with
`glfwSetWindowMonitor`, then queries authoritative geometry/refresh. The original
error-reporting `setWindowMonitor` API remains compatible but is **not** the
panel's switching API.
