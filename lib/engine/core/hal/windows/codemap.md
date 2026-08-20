# lib/engine/core/hal/windows

## Responsibility
The Windows HAL wraps Win32 API surface to provide core OS abstractions: page-based memory management via VirtualAlloc2/VirtualFree, IOCP-driven asynchronous I/O, directory watching via ReadDirectoryChangesW, process creation/spawning, high-resolution timers, debugger integration (OutputDebugString, IsDebuggerPresent, CRT hooks), UTF-8/ASCII/Wide char transcoding, and memory-mapped file support. It is the primary platform-specific implementation behind the pP::hal interface defined in Core.HAL.cppm.

## Design
Each functional area is partitioned into its own `Core.HAL.windows.<Area>.cpp` file, all including `Core.HAL.windows.include.hpp` for platform typedefs; individual files link `mincore.lib`/`bcrypt.lib` via `#pragma comment(lib, ...)`. Namespaces mirror the Core.HAL.cppm structure: `pP::hal`, `pP::hal::io`, `pP::hal::process`, `pP::hal::timer`. Types use Win32 handles (`HANDLE`, `HMODULE`) with RAII-style cleanup via `::CloseHandle`. Page protection flags map `PageProtection::read/write/execute` to `PAGE_*` constants. The ring buffer uses `CreateFileMapping` + `MapViewOfFile3` with `MEM_RESERVE_PLACEHOLDER` for lock-free dual-view access.

## Flow
A typical call flows: `pP::hal::pageAlloc(size, commit, protection)` → `alignedVirtualAlloc_` → `VirtualAlloc2` (or fallback `VirtualAlloc` with alignment placeholders) → `pageProtectionFlags_` translates `PageProtection` to `DWORD` flags → on success, `mem::unpoisonUninitialized` annotates the region; `pageProtect` calls `VirtualProtect`; `pageFree` calls `VirtualFree(MemRelease)`; `ringBufferAlloc` creates a file mapping and maps two views at offset `buffer_size` apart; IOCP init creates a completion port, `submit` posts `ReadFile`/`WriteFile` with `FILE_FLAG_OVERLAPPED`, `poll`/`wait` call `GetQueuedCompletionStatusEx`; `openWatch` calls `CreateFileW` with `FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED` then `ReadDirectoryChangesW`; process spawning uses `CreateProcessW`; timer uses `CreateTimerQueueTimer`; debugger calls `OutputDebugStringA/W`; transcoding uses `MultiByteToWideChar`/`WideCharToMultiByte` with `CP_ACP` or `CP_UTF8`.

## Integration
Consumed by `engine.core` via the `Core.HAL.cppm` umbrella export `export namespace hal { ... }`. The platform is selected at CMake configure time via `PPR_HAL_PLATFORM` (set to `windows`), which causes CMake to link the appropriate `Core.HAL.<platform>` module. All other engine modules import `engine.core:hal` to use `pP::hal::pageAlloc`, `pP::hal::ringBufferAlloc`, `pP::hal::io::init`, etc. The runtime selects the correct implementation at link time; only the chosen platform's object files are included in the build.

## Key Files
- `Core.HAL.windows.Memory.cpp` — page-based allocation via VirtualAlloc2, page protection, decommit, offer/reclaim
- `Core.HAL.windows.RingBuffer.cpp` — contiguous ring buffer via CreateFileMapping + MapViewOfFile3 placeholders
- `Core.HAL.windows.Io.cpp` — IOCP (I/O Completion Ports) async file submit/poll/wait
- `Core.HAL.windows.Filesystem.cpp` — known folder paths, environment-variable-based directory resolution
- `Core.HAL.windows.Process.cpp` — CreateProcessW, GetModuleFileNameW, spawnAndWait
- `Core.HAL.windows.Timer.cpp` — CreateTimerQueueTimer for deadline timers
- `Core.HAL.windows.Debugger.cpp` — OutputDebugString, IsDebuggerPresent, __debugbreak, CRT report hooks, thread naming
- `Core.HAL.windows.Strings.cpp` — MultiByteToWideChar, WideCharToMultiByte, UTF-8 transcoding
- `Core.HAL.windows.IoWatch.cpp` — ReadDirectoryChangesW for directory watching with OVERLAPPED
- `Core.HAL.windows.System.cpp` — BCryptGenRandom, platformName, userName, UUID v4
- `Core.HAL.windows.IoMap.cpp` — CreateFileMapping + MapViewOfFile for memory-mapped files
- `Core.HAL.windows.Random.cpp` — std::mt19937_64 seeded by random_device