# lib/engine/core/hal/linux

## Responsibility
The Linux HAL wraps POSIX/glibc APIs to provide core OS abstractions: page-based memory management via mmap/munmap/mprotect, directory watching via inotify, process creation via fork+execvp, high-resolution timers via timer_create/timer_settime, debugger integration (SIGTRAP, write to stderr, thread IDs via `syscall(SYS_gettid)`), UTF-8/ASCII transcoding, and memory-mapped file support. It implements the pP::hal interface defined in Core.HAL.cppm for the Linux platform, using sysconf(_SC_PAGESIZE) for page size and file descriptors for all I/O operations.

## Design
Each functional area is partitioned into its own `Core.HAL.linux.<Area>.cpp` file. Namespaces mirror the Core.HAL.cppm structure: `pP::hal`, `pP::hal::io`, `pP::hal::process`, `pP::hal::timer`. File descriptors (`int`) are managed with close-on-exec (`INIT1_IN_CLOEXEC`) and RAII-style cleanup via `::close`. Memory uses `MAP_PRIVATE | MAP_ANONYMOUS` (Linux) / `MAP_ANON` (Darwin) with `mprotect` for protection changes. The ring buffer is not implemented on Linux (absent from this platform's file set). Directory watching uses `inotify_init1` + `inotify_add_watch` with `poll` on the fd. Timers use `timer_create` with `SIGEV_THREAD` for callback invocation on expiry. Debugger uses `::write(STDERR_FILENO, ...)`, `raise(SIGTRAP)`, and `::syscall(SYS_gettid)` for thread IDs. Transcoding is pure C++ with no OS API calls.

## Flow
A typical call flows: `pP::hal::pageAlloc(size, commit, protection)` → `mmap(nullptr, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)` → if `!commit`, `madvise(ptr, size, MADV_DONTNEED)`; `pageProtect` calls `mprotect(ptr, size, prot)`; `pageFree` calls `munmap(ptr, size)`; `ringBufferAlloc` is not implemented on Linux (throws unsupported); IOCP is not implemented — `io::init`/`submit`/`poll`/`wait`/`wake` are no-ops or throw `operation_not_supported`; `openWatch` calls `inotify_init1(IN_CLOEXEC|IN_NONBLOCK)`, `inotify_add_watch` with mask `IN_CREATE|IN_DELETE|IN_MODIFY|...`, then `poll`/`read` on the inotify fd to retrieve `inotify_event` records; `process::spawnAndWait` calls `fork()`, child `execvp` the executable, parent `waitpid`; `timer::setDeadline` calls `timer_create(CLOCK_MONOTONIC, &sev, &timer_id)` + `timer_settime`; debugger `outputDebug` writes to `STDERR_FILENO`; `currentThreadId` calls `::syscall(SYS_gettid)`; thread naming uses `prctl(PR_SET_NAME, ...)`; transcoding loops over bytes manually.

## Integration
Consumed by `engine.core` via the `Core.HAL.cppm` umbrella export `export namespace hal { ... }`. The platform is selected at CMake configure time via `PPR_HAL_PLATFORM` (set to `linux`), which causes CMake to link the appropriate `Core.HAL.linux` module. All engine modules import `engine.core:hal` to use `pP::hal::pageAlloc`, `pP::hal::currentExecutablePath`, `pP::hal::io::poll`, etc. The runtime selects the correct implementation at link time; only the chosen platform's object files are included in the build. Linux HAL files are compiled with `-DPPR_HAL_PLATFORM=linux` and link against libc, libpthread, and libdl.

## Key Files
- `Core.HAL.linux.Memory.cpp` — mmap/munmap/mprotect based page allocation, MADV_DONTNEED/FREE
- `Core.HAL.linux.Filesystem.cpp` — HOME/XDG env vars, /proc / /usr/bin directory resolution
- `Core.HAL.linux.Process.cpp` — fork+execvp, /proc/self/exe for executable path
- `Core.HAL.linux.Timer.cpp` — timer_create/timer_settime with SIGEV_THREAD callbacks
- `Core.HAL.linux.Debugger.cpp` — SIGTRAP, write to stderr, syscall SYS_gettid, prctl PR_SET_NAME
- `Core.HAL.linux.Strings.cpp` — manual UTF-8/ASCII transcoding, no OS API calls
- `Core.HAL.linux.IoWatch.cpp` — inotify_init1 + inotify_add_watch + poll/read for directory events
- `Core.HAL.linux.System.cpp` — platformName "linux", userName via getpwuid/ENV
- `Core.HAL.linux.IoMap.cpp` — mmap + munmap for memory-mapped files
- `Core.HAL.linux.Io.cpp` — stub: io_uring/not-yet-implemented, throws unsupported