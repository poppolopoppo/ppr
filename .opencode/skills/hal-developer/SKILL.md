---
name: hal-developer
description: Implement, debug, or review PPR HAL backends, platform source selection, native resource lifetimes, and unsupported-platform behavior.
---

# HAL Developer

Use for changes under `lib/engine/core/hal/`, `cmake/HAL.cmake`, or HAL source
registration in `lib/engine/core/CMakeLists.txt`. `AGENTS.md` owns shared C++
invariants; this skill supplies the HAL investigation and validation procedure.
Actor and tool permissions come from the active harness configuration.

## Establish the current contract

1. Read the root, core, HAL, and affected platform `codemap.md` files. Use
   CodeGraph when available to locate the relevant declarations, backend, and
   caller. Read `Core.HAL.cppm` and the actual implementation before describing
   an API or copying a signature.
2. Check `cmake/HAL.cmake` and core's `CMakeLists.txt` for platform selection and
   registered sources. Platform identity belongs to CMake source selection;
   do not add redundant platform-detection macros inside a selected backend.
   Use value-based conditions for PPR feature flags.
3. Inspect the owning wrapper in core memory, concurrency, or I/O and its tests.
   Distinguish the raw HAL contract from wrapper guarantees. Record existing
   compatibility behavior separately from the intended change.

## Platform scope

| Backend | Implementation scope to verify in source |
|---|---|
| Windows | Win32 page memory, dual-mapped rings, IOCP, mapping/watching, process/timer/debug facilities and PIX markers |
| Linux | Aligned page allocation, memfd dual-mapped rings, raw-syscall io_uring, inotify, process/timer/debug facilities |
| Darwin | Backend files exist; async I/O and other facilities have unsupported paths. Inspect the requested operation before claiming support. |
| Generic | Fallback/stub behavior is operation-specific; inspect its returned error, exception, or no-op contract. |

Random and ring-buffer definitions are selected for Windows and Linux. Their
interface declarations alone do not prove Darwin or Generic link support.
Only Windows registers the platform `include.hpp`. Avoid fixed file counts or
copied API inventories; the CMake source list and current source are authoritative.

## Implement a complete native-resource change

- Identify each handle, mapping, callback context, and owner. Trace acquisition,
  partial-initialization cleanup, cancellation, completion delivery, and release.
- Check OS alignment/granularity requirements, sentinel values, count/size units,
  overflow, and native-error translation at the boundary being changed.
- For async I/O, trace submit through completion and cancellation. Keep request
  storage alive until completion is settled; review wake/drain behavior and
  locking around native queues. Do not invoke user callbacks under internal locks.
- Check shutdown against concurrent submitters, waiters, callbacks, and partially
  initialized state. Follow the ownership/error rules in `AGENTS.md`; existing
  throwing acquisition APIs are compatibility surface, not a template for new APIs.
- Preserve explicit unsupported behavior on other backends. A no-op is suitable
  only where the operation's contract permits it, such as optional markers.
- Register each new source in the same change. Use `module-architect` for module
  boundaries and `build-system` for target/dependency changes.

## Validate

Use `unit-test-updater` to locate tests in `engine.tests.core` and the relevant
owning wrapper suite. Exercise public behavior, error translation, alignment,
partial acquisition, cancellation, and teardown as applicable. Use injected time
for higher-level deadline tests; distinguish OS timers from `TimerManager`, which
requires explicit ticking.

Use `build-system` and `validation` for execution. Validate on the affected native
platform when available; a Windows build does not validate a Linux implementation.
Report unavailable platform checks explicitly. Do not invent platform presets.

## Add a platform

First establish the supported subset and unsupported contracts. Add platform
selection, backend sources, and CMake registration together. Implement declarations
needed by that subset, including cross-cutting foundation functions such as RNG
and ring buffers where required. Check the wrappers' link/runtime needs rather
than assuming ten copied filenames form a complete backend. Add focused native
checks and document the supported subset in the platform codemap.
