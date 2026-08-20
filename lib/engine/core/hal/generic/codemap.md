# lib/engine/core/hal/generic

## Responsibility
The generic stub HAL provides minimal, throw-based implementations for all functional areas as a fallback when no platform-specific HAL is selected. It is intended for development on platforms without a dedicated HAL implementation, or for test environments. Every function either throws `std::bad_alloc`, `std::runtime_error`, or `std::system_error` with "not supported", or is a complete no-op. It implements the pP::hal interface defined in Core.HAL.cppm with the sole purpose of compilation success — real OS calls must be provided by a platform-specific specialization.

## Design
Each functional area is a stub `Core.HAL.generic.<Area>.cpp` file with identical include pattern: `#include "pP/Macros.h"` then `module engine.core; import :hal; import :memory; import std;`. All functions in namespace `pP::hal` are implemented to either throw or no-op. Memory functions either throw `std::bad_alloc` (`pageAlloc`, `pageCommit`) or are no-ops. Io functions either throw `std::system_error` with `errc::operation_not_supported` (`init`, `openFile`, `mapFile`) or are no-ops/return zeros. Process functions throw `std::runtime_error` stating "not implemented for generic platform". Timer uses `std::jthread` for deadline scheduling but still stores data in a `TimerData` struct; `setDeadline` launches a jthread that sleeps then callbacks. Debugger functions are complete no-ops (cast `(void)` args, return defaults). String transcoding is byte-wise memcpy or truncation with no API calls. Directory watching uses a snapshot-based comparison of filesystem modification times via `std::filesystem::directory_iterator`. The ring buffer is not implemented in the generic file set (no definition — declared in the interface, defined only on Windows).

## Flow
A typical call either throws immediately or returns a default value with no side effect. `pageAlloc` throws `std::bad_alloc`. `pageCommit`/`pageProtect`/`pageFree` throw `std::bad_alloc`. `pageDecommit`/`pageOfferToOS`/`pageReclaimFromOS` are no-ops returning false/default. `ringBufferAlloc`/`ringBufferFree` would throw unsupported. `io::init` throws unsupported; `deinit`/`closeFile` are no-ops. `submit`/`poll`/`wait`/`wake`/`cancelIo` return zeros/no-ops. `openFile` throws unsupported; `mapFile` throws unsupported; `unmapFile`/`mapData`/`mapSize` return defaults. `openWatch` creates `WatchHandleData` with a snapshot of file times, `pollWatch` compares current vs snapshot and yields `added`/`modified`/`removed` events, `waitWatch` polls in a 10ms sleep loop. `parseWatchEvents` reads the u8+u32+name format produced by `pollWatch`. `pageAlloc` alignment and size parameters are `(void)`-cast and ignored. All OS-specific handles (`HANDLE`, `HWND`, `HINSTANCE`, `HFILE`, etc.) are absent — the generic stub uses plain `int` dummies or `void*`.

## Integration
Consumed by `engine.core` via the `Core.HAL.cppm` umbrella export `export namespace hal { ... }` when `PPR_HAL_PLATFORM` is set to `generic` (the default/fallback). CMake selects this platform when no `PPR_HAL_PLATFORM` is specified or when the value is `generic`. All engine modules import `engine.core:hal` to use `pP::hal::pageAlloc`, etc., but at runtime this will throw or be a no-op until a platform-specific HAL is selected. The generic HAL allows the codebase to compile and link on any platform, with the expectation that a build configuration will override `PPR_HAL_PLATFORM` to `windows`, `linux`, or `darwin` for the target deployment environment.

## Key Files
- `Core.HAL.generic.Memory.cpp` — stub: all page functions throw bad_alloc or are no-ops
- `Core.HAL.generic.Filesystem.cpp` — stub: HOME/USERPROFILE/WINDIR env vars, current_path fallback
- `Core.HAL.generic.Process.cpp` — stub: throws runtime_error "not implemented for generic platform"
- `Core.HAL.generic.Timer.cpp` — stub: std::jthread-based deadline with cancelDeadline
- `Core.HAL.generic.Debugger.cpp` — stub: complete no-ops, all functions no-op
- `Core.HAL.generic.Strings.cpp` — stub: byte-wise transcoding, no API calls
- `Core.HAL.generic.IoWatch.cpp` — stub: snapshot-based directory watching via filesystem_iterator
- `Core.HAL.generic.Io.cpp` — stub: throws unsupported with errc::operation_not_supported
- `Core.HAL.generic.IoMap.cpp` — stub: throws unsupported with errc::operation_not_supported
- `Core.HAL.generic.System.cpp` — stub: platformName "generic", userName via USER/USERNAME env vars