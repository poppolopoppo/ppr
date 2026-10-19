# GLFW

Upstream: https://github.com/glfw/glfw (main website with changelogs: https://www.glfw.org/)

Version: **3.4** — resolved by the vcpkg manifest at the pinned baseline; the
installed port is `glfw3 3.4#1` (`#1` is the vcpkg port revision). There is no
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
