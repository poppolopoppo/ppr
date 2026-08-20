# lib/engine/core/hal/

## Responsibility
The HAL (Hardware Abstraction Layer) partition provides platform-independent primitives for page-level memory management, ring buffer operations, output debugging, debugger detection, and breakpoint insertion. It is the lowest-level foundation in `engine.core`, with platform-specific implementations in `lib/engine/core/hal/windows/`, `lib/engine/core/hal/linux/`, `lib/engine/core/hal/darwin/`, and `lib/engine/core/hal/generic/` (stub). The selected platform is determined by `PPR_HAL_PLATFORM` CMake variable (windows/linux/darwin/generic). All APIs are designed for zero-overhead in release, debug safety in development builds, and integration with the engine's allocator and IO subsystems.

## Design
- **page memory**: `pageAlloc(size, commit, PageProtection, alignment)` / `pageFree(ptr, size)` — allocate/free a page-aligned region. `pageCommit` / `pageDecommit` — commit/decommit physical memory. `pageProtect` — change read/write/execute protection. `pageOfferToOS` / `pageReclaimFromOS` — give back / reacquire memory from the OS.
- **ring buffer**: `ringBufferAlloc(buffer_size)` / `ringBufferFree(ring_buffer, buffer_size)` — a "magic" ring buffer backed by contiguous pages that map the same memory twice, so wrap-around is free (no copy). Used by `RawChannel` for inter-thread message passing.
- **outputDebug**: `outputDebug(msg)` — writes a string to the debugger output pane (OutputDebugString on Windows, stderr on Linux/Darwin). Used for assert messages and runtime diagnostics.
- **isDebuggerPresent**: `isDebuggerPresent()` — returns true if a debugger is attached. `breakpoint()` / `breakpointIfDebugging()` — software breakpoint (`int 3`).
- **process**: `process::currentExecutablePath()` — full path of the running executable. `process::spawnAndWait(executable, args)` — spawn a child process and wait for termination, returning exit code. `process::terminateProcess(exit_code)` — terminate the current process.
- **timer**: `timer::setDeadline(ms, callback)` / `timer::cancelDeadline(handle)` — one-shot deadline timers (used for test timeout enforcement).
- **threads**: `ThreadId` struct, `currentThreadId()`, `setThreadName()` / `getThreadName()` — platform-native thread identifiers and debugger-visible names.
- **native string transcoding**: `transcode(...)` overloads plus `native::ansi` / `native::utf8` / `native::from` helpers convert between ANSI, UTF-8, and the platform-native `wchar_t`/`char` string type.
- **file-system dirs**: `homeDir()`, `systemDir()`, `appDataLocalDir()`, `appDataRoamingDir()` — well-known OS directories.
- **async I/O**: `io::init/deinit`, `io::openFile/closeFile`, `io::submit/poll/wait`, `io::mapFile/unmapFile/mapData/mapSize`, `io::openWatch/closeWatch/pollWatch/waitWatch/parseWatchEvents` — the platform async I/O surface (see `Core.Io` partition for the engine-level wrapper).

## Flow
- **Application startup**: `pP::Application` calls `hal::disableSystemErrorReporting()` and `hal::installDebugAssertHooks()`, resolves the content directory from `hal::process::currentExecutablePath()`, and logs `hal::platformName()`. The `mem::OS` page allocator is backed by `hal::pageAlloc`/`pageFree`.
- **Assert flow**: `PPR_ASSERT(expr)` evaluates `expr`; if false, `outputDebug("Assertion failed: ...")` is called, `breakpoint()` is hit, and the debug assert callback fires. In release builds, `PPR_ASSUME(expr)` acts as `[[assume(expr)]]` / `__built_assume`.
- **Message passing**: `RawChannel` allocates its double-mapped buffer via `hal::ringBufferAlloc`; producers/consumers communicate through it without copies on wrap-around.
- **Shader hot-reload**: `engine.shader` maps shader source files via `hal::io::mapFile` and watches them through the IO partition's `FileWatcher`.
- **Thread debugging**: `setThreadName`/`getThreadName` expose debugger-visible thread names; `ThreadId` values are formattable via `std::format`.

## Integration
- **engine.core memory**: `mem::OS` page allocator delegates to `hal::pageAlloc`/`pageCommit`/`pageFree`; `RawChannel` uses `hal::ringBufferAlloc`.
- **engine.core IO**: `Core.Io` partition wraps `hal::io::submit/poll/wait`, `hal::io::mapFile`, and `hal::io::openWatch` into `IoPort`/`MappedFile`/`FileWatcher`.
- **engine.app**: `Application` startup uses `hal::disableSystemErrorReporting`, `hal::installDebugAssertHooks`, `hal::platformName`, `hal::process::currentExecutablePath`; GLFW input feeds `hal::native::char_t` characters into the keyboard state.
- **engine.shader**: Hot-reload reads shader source via `io::mapFile` (HAL-backed).
- **EngineCoreTests**: Tests `pageAlloc`/`pageFree`, `ringBufferAlloc`/`ringBufferFree`, `outputDebug`, `isDebuggerPresent`, `breakpoint`, `process::spawnAndWait`, `timer::setDeadline`/`cancelDeadline`, and native string transcoding.

## Key Files
- `Core.HAL.cppm` — `pP::hal` namespace: page memory, ring buffer, outputDebug, isDebuggerPresent, breakpoint, process, timer, threads, native transcoding, async I/O, well-known dirs
- `hal/windows/` — Win32 implementation, 13 files (`Core.HAL.windows.<Area>.cpp` + `Core.HAL.windows.include.hpp`): Memory, RingBuffer, Io, IoMap, IoWatch, Process, Timer, Strings, Debugger, Filesystem, System, Random
- `hal/linux/` — POSIX implementation, 10 files (`Core.HAL.linux.<Area>.cpp`): mmap/mprotect, inotify, fork+execvp, timer_create
- `hal/darwin/` — XNU implementation, 10 files (`Core.HAL.darwin.<Area>.cpp`)
- `hal/generic/` — stub implementation, 10 files (`Core.HAL.generic.<Area>.cpp`): no-op/throw fallbacks