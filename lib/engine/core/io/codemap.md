# lib/engine/core/io/

## Responsibility
Engine-level async file IO, memory-mapped files, and directory watching on top of `hal::io`. No background threads — the caller drives completion via explicit `poll`/`wait` drains — with every completion and file change exposed as an `IEvent` so it composes with `Signal`/`select`.

## Design
- **IoFile** (`Core.Io.cppm/.cpp`, `namespace pP`): move-only RAII `{IoHandle port, FileHandle file}` pair. Nulling-`exchange` move assign/ctor plus null-guarded `close_()` prevent double-close of the raw HAL handle.
- **IoRequest** (`Core.Io.cppm/.cpp`, `final : IEvent`, move-disallowed): per-operation event with embedded `m_overlapped_storage` (`overlapped_storage_size_v` = 64, ≥ Windows `OverlappedExt`), 3-state `m_state` (0 idle / 1 pending / 2 complete), `PulseEvent m_completed`, byte count + `error_code`. `complete_()` CAS(1→2) then emits; `resetEvent()` returns to idle; `cancel()` aborts via `hal::io::cancelIo`; destructor auto-cancels pending ops and verifies quiescence. Delegates `subscribe/unsubscribe/poll/reset` to the inner pulse; `bytesTransferred()`/`error()` assert completed state.
- **IoPort** (`export namespace pP::io`, moved there so the exported `createPort()` factory returns an exported type): owns `hal::io::init()` handle, teardown-guarded destructor, nulling-`exchange` moves. `open()` returns `expected<IoFile, error_code>` (maps `system_error`/`invalid_argument`/`bad_alloc`); `read`/`write(req, file, span, offset)` assert the request is idle, bind `m_active_file`, point the HAL entry at the embedded overlapped storage, mark pending, reset the pulse, then `submit`; `pollCompletions()`/`waitForCompletions()` drain up to 64 completions per call into `IoRequest::complete_`.
- **MappedFile** (`Core.Io.MappedFile.cppm/.cpp`, `namespace pP` + `pP::io::mapFile`): move-only RAII over `hal::io::MapHandle` with nulling moves; `c_str()`, const + mutable `span()`, `size()` (null → empty); `relocatable<MappedFile>`; ASAN-aware unpoison on map (no flooding of read-only views). `io::mapFile(path, flags)` returns `expected<MappedFile, error_code>`.
- **DirectoryWatcher** (`Core.Io.FileWatcher.cppm/.cpp`, `final : IEvent`, move-deleted): `PulseEvent m_changed` + fixed inline buffers (256 `WatchEvent`s, 16 KiB names, 64 KiB raw). `poll(ec)`/`wait(ec)` pull raw bytes via `hal::io::pollWatch`/`waitWatch`, normalize with `parseWatchEvents` into cached `FileChange{Action, filename}` records (`added`/`removed`/`modified`/`renamed_old`/`renamed_new`); `changes()`, `hadOverflow()`, `hadError()`, `isOpen()`, `root()` report health.

## Flow
- **Async read/write**: `IoPort port = createPort(); auto f = port.open(path).value(); IoRequest req; port.read(req, f, buf, off); … port.pollCompletions(); co_await select(req)` → `req.bytesTransferred()`/`req.error()`; reuse requires the drain to complete the request first.
- **Mapped load**: `auto m = io::mapFile(shaderPath).value(); session->loadModuleFromSource(m.c_str(), m.size())` — zero-copy source view, unmapped on scope exit.
- **Watch**: `DirectoryWatcher w(dir, recursive); w.poll(ec); for (auto &c : w.changes()) { … }` or block in `select(w)` alongside channels/requests; overflow surfaces via `hadOverflow()` rather than silent loss.

## Integration
- **hal::io**: `init`/`deinit`, `openFile`/`closeFile`, `submit`/`poll`/`wait`/`wake`/`cancelIo`, `mapFile`/`unmapFile`/`mapData`/`mapSize`, `openWatch`/`closeWatch`/`pollWatch`/`waitWatch`/`parseWatchEvents`; `SubmitEntry::{m_user_data → IoRequest*, m_overlapped → embedded storage}` and `CompletionEntry` bridge the layers.
- **concurrency**: `IoRequest`/`DirectoryWatcher` are `IEvent`s — completions and file changes are observed via `Signal`/`select` next to `RawChannel` and contexts.
- **engine.shader**: shader sources mapped via `io::mapFile`; shader
  directories may be observed via `DirectoryWatcher` for file-change events.
- **engine.tests.core**: `IoPort` open/read/write/submit/poll/wait cycles, error paths, `Port::move_semantics` (no double-close) coverage; `MappedFile` map/span/size; watcher start/stop/event-delivery/`select` filtering suites.

## Key Files
- `Core.Io.cppm` / `.cpp` — `IoFile`, `IoRequest`, `pP::io::IoPort`, `createPort()`
- `Core.Io.MappedFile.cppm` / `.cpp` — `MappedFile`, `io::mapFile()`
- `Core.Io.FileWatcher.cppm` / `.cpp` — `DirectoryWatcher`, `FileChange`
