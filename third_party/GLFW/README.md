# GLFW

Upstream: https://github.com/glfw/glfw (main website with changelogs: https://www.glfw.org/)

Version: **3.4** — resolved by the vcpkg manifest at the pinned baseline; the
local overlay port is `glfw3 3.4#2` (based on baseline revision `#1`). There is no
CPM fallback for GLFW: `cmake/external/GLFW.cmake` only runs
`find_package(glfw3 REQUIRED CONFIG)`, so builds rely on the vcpkg manifest
(or a system config package).

## License

Zlib/libpng-style permissive license (Copyright (c) 2002-2006 Marcus
Geelnard, Copyright (c) 2006-2019 Camilla Löwy, additional GLFW
contributors). Keep the notice with redistributed software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 904 | 149704059b5d0bf551637e50042dd4de9c2cae921021f6636298911e3a5f9462 |

The preserved file is the installed vcpkg notice (`share/glfw3/copyright`).

## Integration scope

- On the Windows `x64-windows` triplet GLFW ships as a runtime DLL staged
  beside PPR executables, so binary redistribution carries its notice.
- `engine.app:platform.glfw` is the only PPR consumer: `GlfwPlatform`
  implements `IPlatform` and publishes `GlfwWindow` (`IWindowService`),
  `GlfwInput` (`IInputService`), and `GlfwPlayer` (`IPlayerService`) into the
  application's service store, domain-gated by `ApplicationDomain`.
- `GLFW.cmake` marks the GLFW include dirs SYSTEM and sets
  `CXX_MODULE_STD OFF` so the external headers cannot disturb the std-module
  routing.
- Engine-side additions: process-lifetime `GLFWallocator` hooks (malloc/realloc/free)
  with an error callback routed to the engine logger; Windows monitor
  identity is resolved through `EnumDisplayDevicesW` against GLFW's native
  handles and persisted as `win32-monitor:<DeviceID>` tokens.
- Native surface handles come from `glfwGetWin32Window` (Win32) and
  `glfwGetCocoaWindow` (Apple) — consumed by the renderer's surface map
  (`WindowHandle` keying) via the platform-owned `Window`.
- Gamepad rumble is not supported (upstream GLFW limitation recorded in the
  GLFW backend map).

## Local X11 fixes

`vcpkg-configuration.json` selects `cmake/ports/glfw3`, a copy of the pinned
baseline port with `x11-refresh-crtc.patch` and
`x11-visibility-timeout.patch`. The source revision, download hash,
features and dependency graph stay the same. The copied port retains its MIT
notice in `cmake/ports/glfw3/LICENSE.vcpkg.txt`.

RandR can reassign the CRTC of a retained output. GLFW 3.4 matches existing
monitors by output ID but leaves their cached CRTC unchanged. Refresh that ID
before the matched-monitor branch returns, so mode/position queries use the
current identity. The patch does not fabricate a disconnect or swallow X errors.

A native GDB probe invalidates only the diagnostic process's cached CRTC, calls
`_glfwPollMonitorsX11`, and checks that it recovers the live output's CRTC. No
desktop topology changes are needed. Baseline and patched results are recorded
in `out/validation/particles-runtime-fixes-20261029/`. The original intermittent
`BadRRCrtc` has not been reproduced on the current desktop; the stale-cache
defect is independently reproduced.

The visibility wait previously decremented its 100 ms allowance only inside
`poll`. An unrelated queued X11 event makes `XPending` return immediately, so
that loop can spin indefinitely when no matching VisibilityNotify arrives.
Measure total elapsed time using GLFW's existing timer for the whole wait,
including the queued-event path. The existing 100 ms limit and event handling
remain unchanged. A second native probe uses an unmapped diagnostic window and
one unrelated queued event; it must return false instead of spinning.
