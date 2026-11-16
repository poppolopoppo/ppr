# lib/engine/app/platform/glfw

## Responsibility and navigation

`engine.app:platform.glfw` implements the platform, window, input and player
services. `../App.Platform.cppm` owns the platform contract;
`../../CMakeLists.txt` owns backend registration.

| Files (`.cppm/.cpp`) | Owner and responsibility |
|---|---|
| `App.Platform.Glfw` | `GlfwPlatform`: factory, domain-gated services, lifecycle, admitted update and native pumping; `.cpp` also owns `GlfwErrorCategory` |
| `App.Platform.Glfw.Window` | `GlfwWindow`: windows/monitors, native callbacks, display geometry, clipboard, waits and surface handles |
| `App.Platform.Glfw.Input` | `GlfwInput`: devices, global/per-device contexts, hotplug, polling and collected input replay |
| `App.Platform.Glfw.Player` | `GlfwPlayer`: `PlayerGraph` adapter, keyboard/mouse player and gamepad player lifecycle |
| `App.Platform.Glfw.include.hpp` | OS-gated GLFW/native includes |

## Service ownership and teardown

`GlfwPlatform` owns the three services; application/service accessors are
non-owning views. Window service creation requires a non-headless domain, input
an interactive domain, and player service presence support. Initialization
publishes each service only after its initialization succeeds and rolls back
partial setup. GLFW allocator hooks have process lifetime and are intentionally
not cleared. GLFW errors go through the engine logger and platform error category.
`GlfwErrorCategory` maps invalid arguments to the standard condition;
`make_error_code(int)` treats `GL_TRUE` as success and other results as failure.

Shutdown is idempotent and releases player, input, then window before GLFW
termination and error-callback removal. It unregisters only successfully
published services, attempts independent cleanup and returns the first error.
`GlfwWindow` and `GlfwInput` use file-static non-owning callback routers, not
public singleton ownership; shutdown detaches native callbacks and routers
before releasing their owned state. Window/monitor destruction notifications
precede native handle/user-pointer release. Input teardown clears batches,
collection mode, timing, device state and registrations.

`GlfwInput` owns keyboard/mouse/gamepads and context assignments; unassigned
gamepads use the global context. `GlfwPlayer` borrows input and delegates player
ownership/events to `PlayerGraph`; keyboard and mouse share a player, existing
device players are reused, and gamepad indices are bounds-checked. GLFW rumble
remains unsupported (the source retains GLFW issue #57 as a TODO).

## Admitted update and callback routing

`GlfwPlatform::update` advances devices once, collects native events, then calls
`GlfwInput::dispatchCollectedBatches` for exactly-once ordered replay. Device
polling precedes native text delivery so transient clearing cannot erase the
frame's characters; gamepad sampling is event-pump independent. Additional
`pumpNativeEvents`/`waitNativeEvents` calls collect owned records without client
advancement. See [input contracts](../../input/codemap.md).

Focus and hotplug split immediate platform bookkeeping from deferred client
broadcasts. Replay interleaves those broadcasts with input in collection order;
focus broadcasts use the captured value, not a newer live flag. State/geometry
events reach window and service sinks; input/drag events reach window sinks
only. The framebuffer callback alone drives resize notifications; logical
window-size changes still update the model. Closing the main window requests
application exit. Key translation drops unmapped keys; character delivery
rejects surrogates and values above Unicode's maximum.

Window/display operations belong on main, coordinated with owner-lane surface
requests. `postEmptyEvent` is the any-thread wake operation and the render lane's
only legal platform call. Positive Windows waits use an owned high-resolution
timer plus the native message queue, preserving posted-event wakeups; other
platforms use GLFW timed waits. Timer creation/wait errors propagate, and timer
release participates in retain-first-error shutdown. Wait bounds are best-effort;
the caller owns the absolute deadline. They are not pure blocked-time measures
because native callbacks may run inside the wait.

## Native platform and compiler constraints

The include helper exposes Win32/Cocoa native APIs only on their platforms;
Linux gets plain `glfw3.h` there. Native-handle/display helpers in
`App.Platform.Glfw.Window.cpp` also contain an X11-conditional branch, usable
only when `GLFW_EXPOSE_NATIVE_X11` is supplied. Wayland does not receive a foreign
XID. `createWindow` rejects an unavailable native handle with `not_supported`
before publication and releases the temporary GLFW owner. Native surface support
must be established from the active build, not inferred from that dormant branch.

Windows monitor identities use `EnumDisplayDevicesW(..., EDD_GET_DEVICE_INTERFACE_NAME)`
matched to GLFW adapter/monitor names and persist as `win32-monitor:<DeviceID>`.
Other backends leave identity empty and support only unambiguous friendly-name
preferences. Failed native identity lookup prevents publication and is logged;
callback exceptions are contained and request application exit.

Linux/Clang bring-up constraints: the error category is `static const`, not
`constexpr` (`std::error_category` is non-literal); partition imports must provide
complete service/device types; joystick indices use checked unsigned conversion.
The earlier map also recorded return-by-value/no-redundant-move monitor handling
for `-Werror=return-std-move`; the current monitor factory returns an owned
expected result, so consult `glfwCreateMonitor_` rather than the old value-return
recipe. These notes are historical compatibility context, not current build evidence.

## Display and renderer integration

`GlfwWindow::createWindow` owns native creation and rolls back failed publication.
`setMainWindow` changes identity without an implicit viewport/focus side effect.
Renderer surfaces consume its `WindowHandle`; the service retains window ownership.

`centerDisplayWindow` centers the decorated outer rectangle in the selected
work area after final size/mode application, with bounded destination-DPI
correction. Oversized windows retain client size and align outer right/top
edges. Native errors propagate and observed geometry refreshes the model.
Startup uses this operation; runtime uses it only for a changed explicit
windowed monitor preference, preserving ordinary resize and rollback positions.

`configureDisplayWindow` supports windowed (null monitor) and exclusive
fullscreen, optional explicit refresh, and error-reporting native apply/rollback.
`setWindowMonitor` consumes the call's GLFW error rather than stale error state.
`displayWindowGeometry` reports logical/framebuffer extents and exclusive
monitor/refresh without guessing windowed observations; monitor enumeration
refreshes current-mode telemetry. See `../../DisplayControls.md`.

The backend depends on core/math, app service/window/input/player partitions and
GLFW. The application owns startup, pumping and teardown; service consumers
include renderer surfaces and UI/gameplay listeners.
