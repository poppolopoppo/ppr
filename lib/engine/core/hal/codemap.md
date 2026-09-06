# lib/engine/core/hal/

## Responsibility
Lowest-level platform abstraction (`export module engine.core:hal`) plus shared foundation helpers. `namespace hal` exposes page memory, magic ring buffers, native-string transcoding, debugger hooks, thread identity/naming, process spawn, deadline timers, well-known directories, and the full async-IO surface that `Core.Io` wraps. Platform backends live in `hal/<windows|linux|darwin|generic>/`, selected by `PPR_HAL_PLATFORM`.

## Design
- **Foundation prelude** (in `pP` scope of `Core.HAL.cppm`): `simd_128_t`, `hash::mix`/`combine` (mx3/xmxmx 64-bit, best-xmxmx 32-bit, Boost `combine`), `overloaded` visitor, `Deferred`/`defer` scope guard, `randomNumberGenerator()` (hardware-seeded `mt19937_64`).
- **Identity/dirs**: `platformName()`, cached `userName()`, `Uuid` (`m_data[4]` + `create()`), `homeDir`/`systemDir`/`appDataLocalDir`/`appDataRoamingDir` as `directory_entry` refs.
- **Page memory**: `pageAlloc(size, commit, PageProtection{read,write,execute}, alignment)` → `allocation_result`, `pageCommit`/`pageDecommit`/`pageProtect`/`pageOfferToOS`/`pageReclaimFromOS`/`pageFree`; `page_size`/`page_granularity` externs, `cacheline_size_v` (hardware constant or 64 fallback). Backs `mem::OS`.
- **Ring buffer**: `ringBufferAlloc`/`ringBufferFree` — dual-mapped contiguous window so wrap-around needs no copy. Backs `RawChannel`.
- **Strings**: `transcode` overloads (ansi↔wide↔utf8, `nullptr,0` = size query) + `toString` two-step helper; `native::{string,char_t,is_wchar_v,ansi,utf8,from,format}` adapts to the platform `path::string_type`.
- **Debugger**: `outputDebug` (ansi + native), `isDebuggerPresent`, `breakpoint`/`breakpointIfDebugging`, `disableSystemErrorReporting`, `installDebugAssertHooks`; `outputDebugFmt` compiles out in release.
- **Threads**: `ThreadId{m_value}` with ordering/swap, `currentThreadId`, `setThreadName`, buffer-based `getThreadName` (returns required size, truncates at 256 in the string overload), `std::formatter<ThreadId>` rendering the debug name or numeric id.
- **Process/timers**: `process::{currentExecutablePath, spawnAndWait(exe, args), terminateProcess}`; `timer::{DeadlineHandle, setDeadline(ms, move_only_callback), cancelDeadline}` for test timeouts and context deadlines.
- **Async IO** (`hal::io`): `IoHandle`/`FileHandle`/`MapHandle`/`WatchHandle` opaques; `Opcode{read,write}`; `OpenFlags` bitmask (`read/write/create/truncate`); `SubmitEntry{m_file,m_buffer,m_buffer_size,m_file_offset,m_opcode,m_user_data → IoRequest*,m_overlapped}`; `CompletionEntry{m_user_data,m_bytes_transferred,m_error}`; `overlapped_storage_size_v` = 64 floor for the Windows extension. Lifecycle `init`/`deinit`; files `openFile`/`closeFile`; drain `submit`/`poll`/`wait` + `wake`/`cancelIo`; maps `mapFile`/`unmapFile`/`mapData`/`mapSize`; watches `openWatch`/`closeWatch`/`pollWatch` (non-blocking, `result_out_of_range` on overflow)/`waitWatch`/`parseWatchEvents` (raw bytes → `WatchEvent{Action,m_name_offset}` + concatenated filenames).

## Flow
- **Boot**: `Application` disables error dialogs, installs assert hooks, resolves content dir from `currentExecutablePath()`, logs `platformName()`; `mem::OS` pages come from `pageAlloc`/`pageFree`.
- **Assert**: `PPR_ASSERT` → `outputDebug` + `breakpoint` + policy hook in debug; `[[assume]]` in release.
- **Messaging**: `RawChannel` ctor sizes via `page_granularity`, storage from `ringBufferAlloc`; producers/consumers rendezvous without wrap copies.
- **File IO**: `Core.Io` submits `SubmitEntry` batches → backend completes (IOCP / io_uring-equivalent / kqueue / stub) → `CompletionEntry` batch drained into `IoRequest::complete_`.
- **Watch**: backend fills raw buffer (`ReadDirectoryChangesW` / inotify / FSEvents / stub) → `parseWatchEvents` normalizes to `WatchEvent` + names → `DirectoryWatcher` caches `FileChange`s.

## Integration
- **memory**: `mem::OS` → `pageAlloc`/`pageCommit`/`pageFree`; poison/ASAN annotations wrap mapped and paged memory.
- **concurrency**: `RawChannel` → `ringBufferAlloc`; channel/context/event wakeups ride `PulseEvent`/`Signal`.
- **Core.Io**: thin RAII/event wrapper over `hal::io` (see `io/codemap.md`); `IoRequest` embeds the overlapped storage whose minimum the HAL declares.
- **engine.app / engine.shader**: startup/debug/thread-naming/process APIs; shader loading via `mapFile`, hot-reload via watches.
- **engine.tests.core**: page/ring-buffer round-trips, debugger/output probes, `spawnAndWait`, deadline timers, transcoding, IO submit/poll/wait and watch suites.

## Key Files
- `Core.HAL.cppm` — full `hal` interface + foundation prelude (`simd`, `hash::mix`, `overloaded`, `Deferred`, RNG)
- `hal/windows/` — Win32 backend, 12 files (see `hal/windows/codemap.md`)
- `hal/linux/` — POSIX backend, 10 files (mmap/mprotect, inotify, fork+execvp, timer_create)
- `hal/darwin/` — XNU backend, 10 files
- `hal/generic/` — stub backend, 10 files (no-op/throw fallbacks)
