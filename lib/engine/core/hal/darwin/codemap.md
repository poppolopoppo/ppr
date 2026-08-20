# lib/engine/core/hal/darwin

## Responsibility
The Darwin/macOS HAL wraps macOS kernel APIs (XNU) to provide core OS abstractions: page-based memory management via mmap with MAP_ANON and mprotect, directory watching (not yet implemented), process creation via fork+execvp, high-resolution timers via timer_create/timer_settime, debugger integration via mach sysctl (KERN_PROC p_flag tracing), thread naming via pthread_setname_np, and memory-mapped file support. It implements the pP::hal interface defined in Core.HAL.cppm for the Darwin platform, using Mach-style primitives where POSIX falls short and pure-C++ transcoding for character set conversion.

## Design
Each functional area is partitioned into its own `Core.HAL.darwin.<Area>.cpp` file. Namespaces mirror the Core.HAL.cppm structure: `pP::hal`, `pP::hal::io`, `pP::hal::process`, `pP::hal::timer`. Memory uses `mmap(nullptr, size, prot, MAP_PRIVATE|MAP_ANON, -1, 0)` with `mprotect` for protection changes — on Darwin, `MAP_ANON` (not `MAP_ANONYMOUS` as on Linux). The ring buffer is not implemented on Darwin (absent from this platform's file set). Directory watching throws `operation_not_supported`. Timers use `timer_create` with `SIGEV_THREAD` for callback invocation on expiry, identical to the Linux implementation. Debugger uses Mach sysctl (`KERN_PROC`, `KERN_PROC_PID`) with `P_TRACED` flag to detect a running debugger, `__builtin_trap()` for breakpoint, and `pthread_threadid_np`/`pthread_setname_np` for thread identification. Thread names are retrieved via `task_threads` + `pthread_from_mach_thread_np`. Transcoding is pure C++ loop-based conversion with no OS API calls.

## Flow
A typical call flows: `pP::hal::pageAlloc(size, commit, protection)` → `mmap(nullptr, size, prot, MAP_PRIVATE|MAP_ANON, -1, 0)` → the `commit` parameter is ignored (pages are always mapped committed); `pageProtect` calls `mprotect(ptr, size, prot)`; `pageFree` calls `munmap(ptr, size)`; `ringBufferAlloc`/`ringBufferFree` are not implemented on Darwin (no definition — declared in the interface, defined only on Windows); IOCP is not implemented — `io::init`/`submit`/`poll`/`wait`/`wake` are no-ops or throw `operation_not_supported`; `openWatch` throws unsupported; `process::spawnAndWait` calls `fork()`, child `execvp` the executable, parent `waitpid`; `timer::setDeadline` calls `timer_create(CLOCK_MONOTONIC, &sev, &timer_id)` + `timer_settime`; debugger `isDebuggerPresent` uses Mach sysctl `sysctl(mib, 4, &info, &size, nullptr, 0)` checking `info.kp_proc.p_flag & P_TRACED`; `breakpoint` calls `__builtin_trap()`; `currentThreadId` calls `::pthread_threadid_np(nullptr, &tid)`; thread naming uses `pthread_setname_np`; transcoding loops over bytes manually.

## Integration
Consumed by `engine.core` via the `Core.HAL.cppm` umbrella export `export namespace hal { ... }`. The platform is selected at CMake configure time via `PPR_HAL_PLATFORM` (set to `darwin`), which causes CMake to link the appropriate `Core.HAL.darwin` module. All engine modules import `engine.core:hal` to use `pP::hal::pageAlloc`, `pP::hal::currentExecutablePath`, `pP::hal::io::poll`, etc. The runtime selects the correct implementation at link time; only the chosen platform's object files are included in the build. Darwin HAL files are compiled with `target=darwin` and link against libSystem, which provides both POSIX and Mach APIs.

## Key Files
- `Core.HAL.darwin.Memory.cpp` — mmap with MAP_ANON, mprotect, MADV_FREE-based page allocation
- `Core.HAL.darwin.Filesystem.cpp` — HOME, Library/Application Support dirs via env vars
- `Core.HAL.darwin.Process.cpp` — fork+execvp, _NSGetExecutablePath for executable path
- `Core.HAL.darwin.Timer.cpp` — timer_create/timer_settime with SIGEV_THREAD callbacks
- `Core.HAL.darwin.Debugger.cpp` — Mach sysctl for debugger detection, __builtin_trap, pthread thread ID
- `Core.HAL.darwin.Strings.cpp` — manual UTF-8/ASCII transcoding, no OS API calls
- `Core.HAL.darwin.IoWatch.cpp` — stub: directory watching not yet implemented, throws unsupported
- `Core.HAL.darwin.System.cpp` — platformName "darwin", userName via getpwuid/ENV
- `Core.HAL.darwin.IoMap.cpp` — mmap + munmap for memory-mapped files
- `Core.HAL.darwin.Io.cpp` — stub: kqueue/not-yet-implemented, throws unsupported