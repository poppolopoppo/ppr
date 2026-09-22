# lib/engine/core/hal/

## Responsibility
Lowest-level platform abstraction (`export module engine.core:hal`) plus shared foundation helpers. `namespace hal` exposes page memory, magic ring buffers, native-string transcoding, debugger hooks, thread identity/naming, process spawn, deadline timers, well-known directories, and the full async-IO surface that `Core.Io` wraps. Platform backends live in `hal/<windows|linux|darwin|generic>/`, selected by `PPR_HAL_PLATFORM`.

## Design
- **Foundation prelude** (in `pP` scope of `Core.HAL.cppm`): `simd_128_t`, `hash::mix`/`combine` (mx3/xmxmx 64-bit, best-xmxmx 32-bit, Boost `combine`), `overloaded` visitor, `Deferred`/`defer` scope guard, `randomNumberGenerator()` (hardware-seeded `mt19937_64`).
- **Identity/dirs**: `platformName()`, cached `userName()`, `Uuid` (`m_data[4]` + `create()`), `homeDir`/`systemDir`/`appDataLocalDir`/`appDataRoamingDir` as `directory_entry` refs.
- **Page memory**: `pageAlloc(size, commit, PageProtection{read,write,execute}, alignment)` → `allocation_result`, `pageCommit`/`pageDecommit`/`pageProtect`/`pageOfferToOS`/`pageReclaimFromOS`/`pageFree`; `page_size`/`page_granularity` externs, `cacheline_size_v` (hardware constant or 64 fallback). Backs `mem::OS`. Linux now matches Windows on over-aligned requests (reserve `aligned_size + align - granularity` window, `mprotect` the aligned slice, `munmap` the head/tail slop); fast path when `align <= granularity`.
- **Ring buffer**: `ringBufferAlloc`/`ringBufferFree` — dual-mapped contiguous window so wrap-around needs no copy. Backs `RawChannel`. Windows uses `CreateFileMapping` + `MapViewOfFile3` over placeholders; Linux uses `memfd_create` + `ftruncate` + `MAP_SHARED|MAP_FIXED` double-map over a `PROT_NONE` reservation.
- **Strings**: `transcode` overloads (ansi↔wide↔utf8, `nullptr,0` = size query, plus the `string_view → char*` identity-copy overload) + `toString` two-step helper (identity overload returns the view, converting overload allocates via size-query). `native::{string,char_t,is_wchar_v,ansi,utf8,from,format}` adapts to the platform `path::string_type`.
- **Debugger**: `outputDebug` (ansi + native), `isDebuggerPresent`, `breakpoint`/`breakpointIfDebugging`, `disableSystemErrorReporting`, `installDebugAssertHooks`; `outputDebugFmt` compiles out in release.
- **Threads**: `ThreadId{m_value}` with ordering/swap, `currentThreadId`, `setThreadName`, buffer-based `getThreadName` (returns required size, truncates at 256 in the string overload), `std::formatter<ThreadId>` rendering the debug name or numeric id.
- **Process/timers**: `process::{currentExecutablePath, spawnAndWait(exe, args), terminateProcess}`; `timer::{DeadlineHandle, setDeadline(ms, move_only_callback), cancelDeadline}` for test timeouts and context deadlines. The callback type is `std23::move_only_function<void()>` (via `:function.ref`) on all backends.
- **Async IO** (`hal::io`): `IoHandle`/`FileHandle`/`MapHandle`/`WatchHandle` opaques; `Opcode{read,write}`; `OpenFlags` bitmask (`read/write/create/truncate`); `SubmitEntry{m_file,m_buffer,m_buffer_size,m_file_offset,m_opcode,m_user_data → IoRequest*,m_overlapped}`; `CompletionEntry{m_user_data,m_bytes_transferred,m_error}`; `overlapped_storage_size_v` = 64 floor for the Windows extension. Lifecycle `init`/`deinit`; files `openFile`/`closeFile`; drain `submit`/`poll`/`wait` + `wake`/`cancelIo`; maps `mapFile`/`unmapFile`/`mapData`/`mapSize`; watches `openWatch`/`closeWatch`/`pollWatch` (non-blocking, `result_out_of_range` on overflow)/`waitWatch`/`parseWatchEvents` (raw bytes → `WatchEvent{Action,m_name_offset}` + concatenated filenames). Linux drives raw syscalls (`io_uring_setup/enter/register` + `eventfd`, SQ/CQ ring mmaps with `SINGLE_MMAP` handling, mutex-guarded SQ alloc/commit, `drainLocked_` CQE→`CompletionEntry` translation, `wake` via `IORING_OP_NOP`, `cancelIo` via `IORING_OP_ASYNC_CANCEL` keyed on the stashed `m_user_data` in the overlapped slot); `deinit` drains in-flight CQEs under lock, then closes fds and unmaps rings before `delete` (noexcept, best effort).

## Flow
- **Boot**: `Application` disables error dialogs, installs assert hooks, resolves content dir from `currentExecutablePath()`, logs `platformName()`; `mem::OS` pages come from `pageAlloc`/`pageFree`.
- **Assert**: `PPR_ASSERT` → `outputDebug` + `breakpoint` + policy hook in debug; `[[assume]]` in release.
- **Messaging**: `RawChannel` ctor sizes via `page_granularity`, storage from `ringBufferAlloc`; producers/consumers rendezvous without wrap copies. Implemented on windows + linux; darwin/generic still throw/omit.
- **File IO**: `Core.Io` submits `SubmitEntry` batches → backend completes (IOCP / raw-syscall io_uring / kqueue / stub) → `CompletionEntry` batch drained into `IoRequest::complete_`. Linux `submit` maps `Opcode` to `IORING_OP_READ/WRITE`, retries once on SQ-full via `io_uring_enter` flush; `poll` drains under lock, `wait` loops drain → `io_uring_enter(GETEVENTS)` without holding the mutex.
- **Watch**: backend fills raw buffer (`ReadDirectoryChangesW` / inotify / FSEvents / stub) → `parseWatchEvents` normalizes to `WatchEvent` + names → `DirectoryWatcher` caches `FileChange`s.

## Integration
- **memory**: `mem::OS` → `pageAlloc`/`pageCommit`/`pageFree`; poison/ASAN annotations wrap mapped and paged memory.
- **concurrency**: `RawChannel` → `ringBufferAlloc`; channel/context/event wakeups ride `PulseEvent`/`Signal`.
- **Core.Io**: thin RAII/event wrapper over `hal::io` (see `io/codemap.md`); `IoRequest` embeds the overlapped storage whose minimum the HAL declares.
- **engine.app / engine.shader**: startup/debug/thread-naming/process APIs;
  shader loading via `mapFile`; watches provide normalized file-change events.
- **engine.tests.core**: page/ring-buffer round-trips, debugger/output probes, `spawnAndWait`, deadline timers, transcoding, IO submit/poll/wait and watch suites.

## Key Files
- `Core.HAL.cppm` — full `hal` interface + foundation prelude (`simd`, `hash::mix`, `overloaded`, `Deferred`, RNG); `transcode(string_view→char*)` overload, split identity/converting `toString`, `std23::move_only_function` deadline callback
- `hal/windows/` — Win32 backend, 12 files (see `hal/windows/codemap.md`)
- `hal/linux/` — POSIX backend, 12 files: mmap/mprotect pageAlloc with Windows-matching alignment, memfd dual-map ring, raw-syscall io_uring, inotify, fork+execvp, timer_create, `random_device`-seeded RNG
- `hal/darwin/` — XNU backend, 10 files
- `hal/generic/` — stub backend, 10 files (no-op/throw fallbacks)
