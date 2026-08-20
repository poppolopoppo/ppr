# lib/engine/core/io/

## Responsibility
The IO partition provides async I/O primitives, file watching, and memory-mapped file support built on top of the HAL `hal::io` layer. It enables non-blocking file operations, directory monitoring, and mapped memory access for shader data and asset loading — all integrated with the engine's allocator hierarchy and event multiplexing system.

## Design
- **IoPort**: Async I/O driver with no background thread — explicit drain. `open(path, flags)` returns an RAII `IoFile` (move-only); `read(IoRequest&, file, buffer, offset)` / `write(...)` submit operations; `pollCompletions()` / `waitForCompletions()` drain completed operations. Internally uses `hal::io::submit()` / `poll()` / `wait()`.
- **IoRequest**: Per-operation async I/O event (`IEvent`, move-disallowed) with `bytesTransferred()`, `error()`, `isPending()`, `cancel()`; completion signaled via `PulseEvent`.
- **MappedFile**: RAII move-only memory-mapped file via `pP::io::mapFile(path, flags)` (wraps `hal::io::mapFile`); exposes `c_str()`, `span()`, `size()`. Used by `engine.shader` for shader source loading.
- **DirectoryWatcher**: Monitors a directory (optionally recursive) for file changes. Wraps `hal::io::openWatch`/`pollWatch`/`parseWatchEvents` (ReadDirectoryChangesW on Windows, inotify on Linux/Darwin). It is an `IEvent` (PulseEvent-backed); `poll()`/`wait()` drain raw platform events into `FileChange` records (`WatchEvent::Action` + filename), exposed via `changes()`.
- **FileChange**: `{ hal::io::WatchEvent::Action, std::string_view filename }` — actions are `added` / `removed` / `modified` / `renamed_old` / `renamed_new`. Consumers observe via `Signal`/`select` on the watcher's `IEvent` interface.

## Flow
- **Shader compilation hot-reload**: `IShaderService::loadModuleFromFile` maps the source via `pP::io::mapFile`, wraps it in a `MappedFileBlob`, and compiles through `ISession::loadModuleFromSource`. File changes are detected via `DirectoryWatcher` (HAL watch) and trigger recompilation.
- **Async file read**: `IoPort::open()` → `read(IoRequest&, ...)` → next drain cycle `pollCompletions()` returns completed requests with bytes transferred / error code.
- **Directory monitoring**: `DirectoryWatcher::poll()` drains events into the `FileChange` cache; `changes()` returns the accumulated span; `hadOverflow()`/`hadError()` report watch health.

## Integration
- **engine.shader**: Hot-reload uses `pP::io::mapFile` (`MappedFile`) to read shader source; `DirectoryWatcher` monitors shader directories for changes.
- **engine.core concurrency**: `IoRequest` and `DirectoryWatcher` are `IEvent`s — completions and file changes are observed via `Signal`/`select`.
- **EngineCoreTests**: Tests `MappedFile` (map/unmap, span access), `DirectoryWatcher` (start/stop, event delivery, select filtering), and `IoPort` (open/read/write, submit/poll/wait cycle, error handling).

## Key Files
- `Core.Io.cppm` / `.cpp` — `pP::IoPort`, `pP::IoFile`, `pP::IoRequest`, `pP::io::createPort()`
- `Core.Io.MappedFile.cppm` / `.cpp` — `pP::MappedFile`, `pP::io::mapFile()`
- `Core.Io.FileWatcher.cppm` / `.cpp` — `pP::DirectoryWatcher`, `pP::FileChange`