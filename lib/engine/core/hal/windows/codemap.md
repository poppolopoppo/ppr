# lib/engine/core/hal/windows

## Responsibility
Win32 backend for `pP::hal`: page memory, dual-view ring buffers, IOCP async IO, mapped files, `ReadDirectoryChangesW` watching, process spawn, timer-queue deadlines, debugger/CRT hooks, thread naming, `MultiByte` transcoding, known-folder resolution, UUID/RNG. Primary platform implementation linked when `PPR_HAL_PLATFORM=windows`.

## Design
- **Layout**: 12 files — shared `Core.HAL.windows.include.hpp` (Win32 typedefs, `#pragma comment(lib, mincore/bcrypt)`) + per-area `Core.HAL.windows.<Area>.cpp` in `pP::hal` / `pP::hal::io` / `pP::hal::process` / `pP::hal::timer` namespaces, mirroring `Core.HAL.cppm`. `Memory` maps `PageProtection` to `PAGE_*`; `RingBuffer` builds the magic window with `CreateFileMapping` + `MapViewOfFile3` over `MEM_RESERVE_PLACEHOLDER`s; `Io` drives IOCP (`init` = completion port, `submit` = overlapped `ReadFile`/`WriteFile`, `poll`/`wait` = `GetQueuedCompletionStatusEx`, plus `wake`/`cancelIo`); `IoMap` wraps file-mapping objects; `IoWatch` issues overlapped `ReadDirectoryChangesW` on `CreateFileW(BACKUP_SEMANTICS|OVERLAPPED)` handles.
- **System** (`Core.HAL.windows.System.cpp`): `platformName()` → `"windows"`; `userName()` → cached `GetUserNameW` result transcoded via `native::ansi` (`"unknown_user"` fallback); `Uuid::create()` → `BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)` over `m_data` with `safe_narrowing` size and `PPR_VERIFY` on status.
- **Process/Timer/Debugger/Strings/Filesystem/Random**: `CreateProcessW` + `GetModuleFileNameW`; `CreateTimerQueueTimer` one-shots with `std23::move_only_function<void()>` callback (via `:function.ref`); `OutputDebugStringA/W`, `IsDebuggerPresent`, `__debugbreak`, CRT report hooks, debugger-visible thread names; `MultiByteToWideChar`/`WideCharToMultiByte` (`CP_ACP`/`CP_UTF8`) plus the shared `string_view → char*` memcpy overload; known folders via environment/shell resolution; `Random` seeds `mt19937_64` from `random_device` (separate from the BCrypt UUID path; Linux now mirrors this shape).

## Flow
- **Alloc**: `pageAlloc` → `alignedVirtualAlloc_` (`VirtualAlloc2`, placeholder fallback) → flag translation → ASAN `unpoisonUninitialized`; `pageProtect` → `VirtualProtect`; `pageFree` → `VirtualFree(MEM_RELEASE)`; `ringBufferAlloc` maps two adjacent views `buffer_size` apart. Linux `pageAlloc` now matches this alignment contract (reserve/mprotect/trim-slop); Linux `ringBufferAlloc` is the memfd dual-map counterpart.
- **IO**: `Io::init` → port; `openFile` → `CreateFileW(OVERLAPPED)`; `submit(SubmitEntry)` posts overlapped ops with the caller's embedded storage; `poll`/`wait` harvest `CompletionEntry`s; `cancelIo` aborts by overlapped pointer; `wake` unblocks waiters.
- **Watch/map**: `openWatch` → handle + first `ReadDirectoryChangesW`; `pollWatch`/`waitWatch` return raw bytes, `parseWatchEvents` splits `FILE_NOTIFY_INFORMATION` into `WatchEvent` + names; `mapFile` → mapping object, `mapData`/`mapSize` expose the view, `unmapFile` releases.
- **Identity**: `Uuid::create` draws 16 BCrypt random bytes per call; `userName` resolves once into a function-static `std::string`.

## Integration
- Consumed only through `engine.core:hal` (`pP::hal::pageAlloc`, `ringBufferAlloc`, `io::init/submit/poll/…`); CMake selects this backend at configure time and links only its objects. Other engines (`memory`, `concurrency`, `io`, `app`, `shader`) never touch Win32 directly. Non-Windows builds substitute the linux/darwin/generic backends behind the identical interface.

## Key Files
- `Core.HAL.windows.include.hpp` — shared Win32 typedefs and lib pragmas
- `Core.HAL.windows.Memory.cpp` — VirtualAlloc2 pages, protect/decommit/offer/reclaim
- `Core.HAL.windows.RingBuffer.cpp` — dual-view magic ring buffer (windows-only)
- `Core.HAL.windows.Io.cpp` — IOCP submit/poll/wait/wake/cancelIo
- `Core.HAL.windows.IoMap.cpp` — file-mapping backed `mapFile` family
- `Core.HAL.windows.IoWatch.cpp` — overlapped `ReadDirectoryChangesW` watches
- `Core.HAL.windows.System.cpp` — `platformName`, cached `userName`, BCrypt `Uuid::create`
- `Core.HAL.windows.Random.cpp` — `random_device`-seeded `mt19937_64` (windows-only)
- `Core.HAL.windows.Process.cpp` — `CreateProcessW` spawn, executable path
- `Core.HAL.windows.Timer.cpp` — timer-queue deadline timers
- `Core.HAL.windows.Debugger.cpp` — OutputDebugString, debugger check, CRT hooks, thread names
- `Core.HAL.windows.Strings.cpp` — `MultiByte`/`WideChar` transcoding
- `Core.HAL.windows.Filesystem.cpp` — known-folder resolution
