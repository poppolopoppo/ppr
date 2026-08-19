---
name: hal-developer
description: Authoritative reference for implementing, modifying, debugging, or testing the PPR Hardware Abstraction Layer (HAL) — 10 functional areas across Windows, Linux, Darwin, and the Generic stub, including module structure, API surface, platform-by-platform implementation, stub conventions, testing, and the adding-a-platform checklist.
---

# HAL Developer

The HAL provides a uniform C++ API across Windows, Linux, Darwin (macOS), and a
Generic stub platform for page-level memory management, ring buffers, debugging,
process spawning, deadline timers, asynchronous I/O (IOCP / io_uring / kqueue),
memory-mapped files, directory watching, native string transcoding, and system
queries (platform name, user name, directories). It lives in module partition
`engine.core:hal`, re-exported via `engine.core`, with per-platform source files
under `lib/engine/core/hal/<platform>/`.

Activate this skill whenever a task touches `lib/engine/core/hal/Core.HAL.cppm`, any
`Core.HAL.<platform>.*.cpp`, `cmake/HAL.cmake`, or the HAL source selection in
`lib/engine/core/CMakeLists.txt`.

## Contract

This skill is the authoritative reference for all HAL-related work in the PPR
codebase. It documents the 10 functional areas, 4 platform implementations,
module structure, API surface, platform detection, source selection, stub
conventions, and the checklist for adding a new platform. The orchestrator
consults this skill, then delegates platform implementation to `@fixer`/`@oracle`
and recon to `@explorer`. It never writes HAL `.cpp` files directly.

## Constraints

- **No platform-detection macros in platform code.** Platform selection is
  build-system-only (via `PPR_HAL_PLATFORM` in `cmake/HAL.cmake`). No platform
  may use `#ifdef _WIN32`, `#ifdef __linux__`, or other platform-detection
  macros inside its platform-specific source
  (`lib/engine/core/hal/<platform>/`). The compiler only builds the selected
  platform's files, so such macros are unnecessary and must not leak into
  source.

## 1. Architecture Overview

The HAL is organised into **10 functional areas**, each implemented across **4
platforms** plus a **generic stub**. All files live under `lib/engine/core/`.

### File naming convention

```
Core.HAL.<platform>.<Area>.cpp
```

| Area | Filename suffix | Exported from |
|------|----------------|--------------|
| System | `System` | `pP::hal` |
| Memory | `Memory` | `pP::hal` |
| Ring buffer | `RingBuffer` (Windows only) | `pP::hal` |
| Random | `Random` (Windows only) | `pP` (free function `randomNumberGenerator()`) |
| Debugger | `Debugger` | `pP::hal` |
| Strings | `Strings` | `pP::hal` |
| Process | `Process` | `pP::hal::process` |
| Timer | `Timer` | `pP::hal::timer` |
| I/O | `Io` | `pP::hal::io` |
| I/O Mapped files | `IoMap` | `pP::hal::io` |
| I/O Directory watching | `IoWatch` | `pP::hal::io` |
| Filesystem paths | `Filesystem` | `pP::hal` |

### Platform directories

| Platform | Directory | Identifier (`PPR_HAL_PLATFORM`) | Files |
|----------|-----------|----------------------------------|-------|
| Windows | `lib/engine/core/hal/windows/` | `windows` | 13 (10 standard + `Random`, `RingBuffer`, `include.hpp`) |
| Linux | `lib/engine/core/hal/linux/` | `linux` | 10 |
| Darwin (macOS) | `lib/engine/core/hal/darwin/` | `darwin` | 10 |
| Generic (stub) | `lib/engine/core/hal/generic/` | `generic` | 10 |

### Platform detection (`cmake/HAL.cmake`)

```cmake
if (WIN32)
    set(PPR_HAL_PLATFORM windows)
elseif(APPLE)
    set(PPR_HAL_PLATFORM darwin)
elseif(UNIX AND NOT APPLE)
    set(PPR_HAL_PLATFORM linux)
else()
    set(PPR_HAL_PLATFORM generic)
endif()
```

### Source selection (`lib/engine/core/CMakeLists.txt`)

The 10 standard areas are always compiled via a pattern:

```cmake
set(HAL_PLATFORM_SOURCES
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.include.hpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Debugger.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Filesystem.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Io.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.IoMap.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.IoWatch.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Memory.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Process.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Strings.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.System.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Timer.cpp
)
```

Two **Windows-only** files are appended when `PPR_HAL_PLATFORM == "windows"`:

```cmake
if(PPR_HAL_PLATFORM STREQUAL "windows")
    list(APPEND HAL_PLATFORM_SOURCES
        hal/windows/Core.HAL.windows.Random.cpp
        hal/windows/Core.HAL.windows.RingBuffer.cpp
    )
endif()
```

### Module interface file

The public API is declared in `lib/engine/core/hal/Core.HAL.cppm` (module
partition `engine.core:hal`). It is re-exported from the umbrella module
`engine.core`:

```cpp
// Core.cppm
export import :hal;
```

## 2. Module Structure

### Interface file pattern (`Core.HAL.cppm`)

```cpp
module;

#include "pP/Macros.h"

export module engine.core:hal;

import :types;
export import :types;
import std;
export import :utility;

export namespace pP {
    // ... all exported declarations ...
}
```

Key points:
- `#include "pP/Macros.h"` goes in the **global module fragment** (before `export module`).
- The partition **exports** type dependencies (`:types`, `:utility`) so consumers do not need to import them separately.
- All HAL declarations live in `namespace pP::hal` (or sub-namespaces `process`, `timer`, `io`, `native`).
- Helper types (`simd_128_t`, `overloaded`, `Deferred`, `randomNumberGenerator`) are in `namespace pP` directly.

### Implementation file pattern (e.g. `Core.HAL.windows.Memory.cpp`)

```cpp
module;

#include "Core.HAL.windows.include.hpp"  // <-- platform-specific preamble
// ... additional platform headers ...

module engine.core;

import :assert;
import :hal;
import :memory;
// ... other partition imports ...

import std;

namespace pP::hal {
    // ... definitions ...
}
```

Key points:
- **Global module fragment** includes platform SDK headers before the module declaration.
- The implementation file uses `module engine.core;` (not the partition name).
- It explicitly `import :hal;` to bring the partition declarations into scope.
- Definitions use `namespace pP::hal` or `namespace pP::hal::process` etc.

### Windows-specific preamble (`Core.HAL.windows.include.hpp`)

Every Windows HAL `.cpp` file includes this in its global module fragment:

```cpp
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif

#define NOGDICAPMASKS
#define NOATOM
#define NODRAWTEXT
#define NOKERNEL
#define NOMEMMGR
#define NOMETAFILE
#define NOOPENFILE
#define NOSCROLL
#define NOSERVICE
#define NOSOUND
#define NOCOMM
#define NOKANJI
#define NOHELP
#ifdef NDEBUG
#   define NOPROFILER
#endif
#define NODEFERWINDOWPOS
#define NOMCX
#define NOCRYPT
#define NOTAPE
#define NOIMAGE
#define NOPROXYSTUB
#define NORPC

#include <Windows.h>

// Undo problematic macros that clash with C++ names
#undef CreateProcess
#undef CreateSemaphore
#undef CreateWindow
#undef MemoryBarrier
#undef MoveFile
#undef RegisterClass
#undef RemoveDirectory
#undef Yield
#undef small
#undef min
#undef max

#include "pP/Macros.h"
```

## 3. Required API Surface

Below is the **complete** public API that every platform implementation must
provide. All declarations are in `namespace pP::hal` unless otherwise noted.

### 3.1 Core system queries

```cpp
// Returns the platform name as a string literal: "windows", "linux", "darwin", "generic"
[[nodiscard]] std::string_view platformName() noexcept;

// Returns the current user name (falls back to "unknown_user")
[[nodiscard]] std::string_view userName();
```

### 3.2 Filesystem directories

All return a static singleton `std::filesystem::directory_entry` reference:

```cpp
[[nodiscard]] const std::filesystem::directory_entry &homeDir();
[[nodiscard]] const std::filesystem::directory_entry &systemDir();
[[nodiscard]] const std::filesystem::directory_entry &appDataLocalDir();
[[nodiscard]] const std::filesystem::directory_entry &appDataRoamingDir();
```

### 3.3 Cache line and page constants

```cpp
// Hardware cache line size (std::hardware_destructive_interference_size, or 64u fallback)
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t cacheline_size_v = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t cacheline_size_v = 64u; // conservative fallback for older compilers
#endif

// Page protection flags
struct PageProtection {
    bool read: 1 = true;
    bool write: 1 = true;
    bool execute: 1 = false;
};

// System page size (e.g. 4096 on Linux, queried via GetSystemInfo on Windows)
extern const std::size_t page_size;

// Allocation granularity (same as page_size on POSIX, 64 KiB on Windows)
extern const std::align_val_t page_granularity;
```

### 3.4 Page memory operations

```cpp
// Allocate one or more pages. commit=false means reserve-only (no physical backing).
// Throws std::bad_alloc on failure.
[[nodiscard]] std::allocation_result<void *> pageAlloc(
    std::size_t size,
    bool commit = true,
    PageProtection allowed = {},
    std::align_val_t alignment = page_granularity) noexcept(false);

// Commit reserved pages (make physical)
void pageCommit(void *ptr, std::size_t size, PageProtection allowed = {}) noexcept(false);

// Decommit pages (release physical backing, keep reservation)
void pageDecommit(void *ptr, std::size_t size) noexcept(false);

// Change page protection flags
void pageProtect(void *ptr, std::size_t size, PageProtection allowed) noexcept(false);

// Offer pages to the OS for reuse (memory pressure hint)
void pageOfferToOS(void *ptr, std::size_t size) noexcept(false);

// Reclaim previously offered pages. Returns true if content is preserved.
[[nodiscard]] bool pageReclaimFromOS(const void *ptr, std::size_t size) noexcept;

// Free pages entirely (release + decommit)
void pageFree(void *ptr, std::size_t size) noexcept(false);
```

**Poisoning rule (all platforms):** `mem::unpoisonUninitialized` is called on
newly committed/allocated/reclaimed memory. Do **not** poison before
`pageDecommit`/`pageOfferToOS`/`pageFree` — the region may contain
already-decommitted pages, so writing poison patterns would fault. The OS-level
unmap/decommit/offer itself makes the memory inaccessible, catching use-after-free.

### 3.5 Ring buffer (magic mirrored mapping)

A contiguous virtual buffer that maps the same physical pages twice, so reading
past the end wraps around automatically. `buffer_size` must be
page-granularity-aligned.

```cpp
[[nodiscard]] void *ringBufferAlloc(std::size_t buffer_size) noexcept(false);
void ringBufferFree(const void *ring_buffer, std::size_t buffer_size) noexcept(false);
```

### 3.6 Debugger integration

```cpp
// Write a message to the debugger output (OutputDebugString on Windows, stderr on POSIX)
void outputDebug(const char *ansi_msg) noexcept;
void outputDebug(const native::char_t *native_msg) noexcept;

// Formatted debug output (only active in debug builds)
template<typename... ArgsT>
void outputDebugFmt(const std::format_string<ArgsT...> &fmt, ArgsT &&... args) noexcept;
template<typename... ArgsT>
void outputDebugFmt(const native::format_string<ArgsT...> &fmt, ArgsT &&... args) noexcept;

[[nodiscard]] bool isDebuggerPresent() noexcept;
void breakpoint() noexcept;
void breakpointIfDebugging() noexcept;

// Suppress OS error dialogs (set before fork/child-run tests)
void disableSystemErrorReporting() noexcept;

// Install CRT/abort hooks that route failures to outputDebug + breakpoint
void installDebugAssertHooks() noexcept;
```

`outputDebugFmt` is defined inline in `Core.HAL.cppm` and calls `outputDebug`.
It is a no-op in release builds.

### 3.7 Thread names (visible to debuggers)

```cpp
struct ThreadId {
    u64 m_value{0u};
    // operator==, operator<=>, swap
};

[[nodiscard]] ThreadId currentThreadId() noexcept;
void setThreadName(std::string_view name) noexcept;

// Buffer-based query; returns chars written. `nullptr, 0` returns the full
// required size (caller detects truncation). Never allocates.
[[nodiscard]] std::size_t getThreadName(ThreadId thread_id, char *out_buffer, std::size_t capacity) noexcept;

// Allocating convenience wrapper (capped at 256 bytes)
[[nodiscard]] inline std::string getThreadName(ThreadId thread_id);
```

`std::formatter<pP::hal::ThreadId, CharT>` is specialized in `Core.HAL.cppm` so
`std::format("{}", tid)` renders the thread debug name (falling back to the
numeric id for unnamed threads).

### 3.8 String transcoding

All transcode functions accept a source view and a destination buffer with
capacity, and return the number of characters written (not including null
terminator). They perform lossy or lossless conversion depending on encoding.

```cpp
[[nodiscard]] std::size_t transcode(std::string_view ansi, char8_t *p_dst, std::size_t capacity) noexcept;
[[nodiscard]] std::size_t transcode(std::string_view ansi, wchar_t *p_dst, std::size_t capacity) noexcept;
[[nodiscard]] std::size_t transcode(std::wstring_view wide, char8_t *p_dst, std::size_t capacity) noexcept;
[[nodiscard]] std::size_t transcode(std::u8string_view utf8, wchar_t *p_dst, std::size_t capacity) noexcept;
[[nodiscard]] std::size_t transcode(std::wstring_view wide, char *p_dst, std::size_t capacity) noexcept;
[[nodiscard]] std::size_t transcode(std::u8string_view utf8, char *p_dst, std::size_t capacity) noexcept;
```

The `toString<DstCharT>(src)` template allocates a new string:

```cpp
template<details::TChar DstCharT, details::TChar SrcCharT, typename AllocatorT = ...>
[[nodiscard]] decltype(auto) toString(const std::basic_string_view<SrcCharT> src, AllocatorT &&alloc = {}) noexcept(...);
```

### 3.9 Native string helpers (`pP::hal::native`)

```cpp
namespace native {
    using string = std::filesystem::path::string_type;     // std::wstring on Windows, std::string on POSIX
    using char_t = string::value_type;                     // wchar_t on Windows, char on POSIX
    using string_view = std::basic_string_view<char_t>;

    inline constexpr bool is_wchar_v = std::is_same_v<char_t, wchar_t>;

    template<typename... ArgsT>
    using format_string = std::conditional_t<is_wchar_v, std::wformat_string<ArgsT...>, std::format_string<ArgsT...>>;

    // Buffer-based conversion (returns chars written)
    [[nodiscard]] std::size_t ansi(const string_view &native_str, char *out_buffer, std::size_t buffer_size) noexcept;
    [[nodiscard]] std::size_t utf8(const string_view &native_str, char8_t *out_buffer, std::size_t buffer_size) noexcept;
    [[nodiscard]] std::size_t from(const std::string_view &ansi_str, char_t *out_buffer, std::size_t buffer_size) noexcept;
    [[nodiscard]] std::size_t from(const std::u8string_view &utf8_str, char_t *out_buffer, std::size_t buffer_size) noexcept;

    // Allocating conversion (returns new string)
    [[nodiscard]] decltype(auto) ansi(const string_view &native_str);
    [[nodiscard]] decltype(auto) utf8(const string_view &native_str);
    [[nodiscard]] decltype(auto) from(const std::string_view &ansi_str);
    [[nodiscard]] decltype(auto) from(const std::u8string_view &utf8_str);

    // Format-style conversion (identity if same char type, utf8 otherwise)
    template<details::TChar CharT>
    [[nodiscard]] decltype(auto) format(const string_view &native_str) noexcept(...);
}
```

### 3.10 Process management (`pP::hal::process`)

```cpp
namespace process {
    // Full path to the current executable
    [[nodiscard]] std::filesystem::path currentExecutablePath() noexcept(false);

    // Spawn a child process and wait for it to complete; returns exit code
    [[nodiscard]] int spawnAndWait(const std::filesystem::path &executable, std::span<const std::string> args) noexcept(false);

    // Terminate the current process immediately
    [[noreturn]] void terminateProcess(int exit_code) noexcept;
}
```

Note: `terminateProcess` is declared in the interface but **not yet defined on
any platform** — a latent link error if referenced. Implement it when a platform
needs it.

### 3.11 Deadline timers (`pP::hal::timer`)

```cpp
namespace timer {
    struct DeadlineHandle {
        void *m_data{nullptr};
    };

    // Set a one-shot timer; callback is invoked after `ms` milliseconds on a dedicated thread
    [[nodiscard]] DeadlineHandle setDeadline(std::chrono::milliseconds ms, std::move_only_function<void()> callback) noexcept(false);

    // Cancel a pending deadline; the callback will not be invoked
    void cancelDeadline(DeadlineHandle &handle) noexcept;
}
```

### 3.12 Asynchronous I/O (`pP::hal::io`)

#### Types and constants

```cpp
namespace io {
    using IoHandle = void *;
    using FileHandle = void *;
    using MapHandle = void *;
    using WatchHandle = void *;

    enum class Opcode : u8 { read, write };

    // Minimum storage for platform-specific OVERLAPPED extension (64 bytes on x64)
    inline constexpr std::size_t overlapped_storage_size_v = 64u;

    struct OpenFlags {
        enum : u32 { read = 1u << 0, write = 1u << 1, create = 1u << 2, truncate = 1u << 3 };
        u32 m_bits{read};
        // operator|, operator|=, operator==
    };

    struct SubmitEntry {
        FileHandle  m_file;
        void       *m_buffer;
        u64         m_buffer_size;
        u64         m_file_offset;
        Opcode      m_opcode;
        void       *m_user_data;     // points to IoRequest
        void       *m_overlapped;    // points to embedded storage or heap fallback
    };

    struct CompletionEntry {
        void          *m_user_data;
        u64            m_bytes_transferred;
        std::error_code m_error;
    };

    struct WatchEvent {
        enum class Action : u8 { added, removed, modified, renamed_old, renamed_new };
        Action m_action;
        u32    m_name_offset{0u};
    };
}
```

#### Lifecycle

```cpp
[[nodiscard]] IoHandle init() noexcept(false);
void deinit(IoHandle handle) noexcept;
```

#### File operations

```cpp
[[nodiscard]] FileHandle openFile(IoHandle io, const std::filesystem::path &path, OpenFlags flags) noexcept(false);
void closeFile(IoHandle io, FileHandle file) noexcept;
```

#### Submit and drain

```cpp
// Submit a batch of I/O operations; returns number successfully submitted
[[nodiscard]] std::size_t submit(IoHandle io, std::span<SubmitEntry> entries) noexcept;

// Non-blocking drain of completed operations
[[nodiscard]] std::size_t poll(IoHandle io, std::span<CompletionEntry> entries) noexcept;

// Blocking drain (waits for at least one completion)
[[nodiscard]] std::size_t wait(IoHandle io, std::span<CompletionEntry> entries) noexcept;

// Wake up a thread blocked in wait()
void wake(IoHandle io) noexcept;

// Cancel a specific in-flight I/O operation
void cancelIo(FileHandle file, void *overlapped) noexcept;
```

Note: `cancelIo` is defined only on Windows. Linux/Darwin/generic declare it in
the interface but do not define it — a latent link error if referenced.

#### Memory-mapped files

```cpp
[[nodiscard]] MapHandle mapFile(const std::filesystem::path &path, OpenFlags flags) noexcept(false);
void unmapFile(MapHandle map) noexcept;
[[nodiscard]] void *mapData(MapHandle map) noexcept;
[[nodiscard]] std::size_t mapSize(MapHandle map) noexcept;
```

#### Directory watching

```cpp
[[nodiscard]] WatchHandle openWatch(const std::filesystem::path &dir, bool recursive) noexcept(false);
void closeWatch(WatchHandle watch) noexcept;

// Non-blocking read of raw platform events; ec = result_out_of_range on overflow
[[nodiscard]] std::size_t pollWatch(WatchHandle watch, std::span<std::byte> buffer, std::error_code &ec) noexcept;

// Blocking variant
[[nodiscard]] std::size_t waitWatch(WatchHandle watch, std::span<std::byte> buffer, std::error_code &ec) noexcept;

// Parse raw platform event data into normalized WatchEvent records
[[nodiscard]] std::size_t parseWatchEvents(std::span<const std::byte> raw, std::span<WatchEvent> out_events, std::span<char> out_names) noexcept;
```

## 4. Platform-by-Platform Implementation Guide

### 4.1 Windows

#### Memory (`Core.HAL.windows.Memory.cpp`)

- **`page_size` / `page_granularity`**: Query via `::GetSystemInfo(&sys_info)` → `dwPageSize` / `dwAllocationGranularity` (usually 4 KiB / 64 KiB).
- **`pageAlloc`**: Use `VirtualAlloc2` (Win10+) or a fallback that calls `VirtualAlloc` in a loop to achieve the requested alignment. Pass `MEM_RESERVE | (commit ? MEM_COMMIT : 0)` with `MEM_64K_PAGES`. Throws `std::bad_alloc` on failure. Unpoison on commit.
- **`pageCommit`**: `::VirtualAlloc(ptr, size, MEM_COMMIT, prot)`. Throws `std::bad_alloc`. Unpoison.
- **`pageDecommit`**: `::VirtualFree(ptr, size, MEM_DECOMMIT)`. **No poison** (see §3.4 rule). Throws `std::bad_alloc`.
- **`pageProtect`**: `::VirtualProtect(ptr, size, prot, &old)`. Throws `std::bad_alloc`.
- **`pageOfferToOS`**: `::OfferVirtualMemory(ptr, size, VmOfferPriorityNormal)`. **No poison**. Throws `std::bad_alloc`.
- **`pageReclaimFromOS`**: `::ReclaimVirtualMemory(ptr, size)`. Returns `true` on `ERROR_SUCCESS` or `ERROR_BUSY`. Unpoison on success.
- **`pageFree`**: `::VirtualFree(ptr, 0, MEM_RELEASE)`. **No poison**. Throws `std::bad_alloc`.
- **Page protection mapping** helper:

```cpp
static constexpr DWORD pageProtectionFlags_(PageProtection p) noexcept {
    if (p.read) {
        if (p.write) return p.execute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        return p.execute ? PAGE_EXECUTE_READ : PAGE_READONLY;
    }
    if (p.write) return p.execute ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
    return p.execute ? PAGE_EXECUTE : PAGE_NOACCESS;
}
```

#### Ring buffer (`Core.HAL.windows.RingBuffer.cpp`)

The Windows ring buffer uses the **mirrored page mapping** technique:

1. Reserve a **2x buffer_size** placeholder with `VirtualAlloc2(..., MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS)`.
2. Split the placeholder by releasing the first half with `VirtualFree(placeholder1, buffer_size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)`, leaving a second placeholder at `placeholder1 + buffer_size`.
3. Create a `PAGE_READWRITE` section (backed by the paging file) via `CreateFileMapping(INVALID_HANDLE_VALUE, ..., PAGE_READWRITE, ..., buffer_size)`.
4. Map the section into **both** placeholders using `MapViewOfFile3` with `MEM_REPLACE_PLACEHOLDER`.
5. The result is that virtual addresses `[view1, view1 + 2*buffer_size)` all access the same physical memory, providing automatic wrap-around.

On free, unmap both views with `UnmapViewOfFile`.

This file also defines two internal helper types in `pP::hal` (not part of the
public API surface): `Win32LastError` (wraps a `DWORD` error code, `format()` /
`message()` via `::FormatMessageA`) and `Win32Exception` (extends
`std::runtime_error` with a `Win32LastError`).

#### Debugger (`Core.HAL.windows.Debugger.cpp`)

- **`outputDebug`**: `::OutputDebugStringA` / `::OutputDebugStringW` (debug builds only; no-op in release).
- **`isDebuggerPresent`**: `::IsDebuggerPresent()` (debug builds only).
- **`breakpoint`**: `__debugbreak()` (debug builds only).
- **`breakpointIfDebugging`**: Test `::IsDebuggerPresent()` then `__debugbreak()`.
- **`disableSystemErrorReporting`**: `::_set_error_mode(_OUT_TO_STDERR)`, `::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)`, `::_CrtSetReportMode` for all CRT channels, `::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX)`, `::WerSetFlags(WER_FAULT_REPORTING_FLAG_QUEUE)`.
- **`installDebugAssertHooks`**: `::_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &crtReportHook)` (suppresses the Abort/Retry/Ignore box, routes to `outputDebug` + `breakpointIfDebugging`), `::_set_invalid_parameter_handler`, `::_set_purecall_handler`, `std::set_terminate(&terminateHandler)` (logs + `std::_Exit(3)`). All guarded by `PPR_ENABLE_ASSERTIONS`.
- **Thread names**: `currentThreadId` → `::GetCurrentThreadId()`. `setThreadName` → `SetThreadDescription` (dynamically resolved from kernel32), with a legacy `RaiseException(0x406D1388, ...)` fallback when a debugger is attached. `getThreadName` → `GetThreadDescription` (dynamic), `::OpenThread(THREAD_QUERY_LIMITED_INFORMATION)`, transcode wide→UTF-8.

#### Filesystem (`Core.HAL.windows.Filesystem.cpp`)

- **`homeDir`**: `::GetEnvironmentVariableW(L"USERPROFILE", ...)`, fallback `HOMEDRIVE` + `HOMEPATH`.
- **`systemDir`**: `::GetSystemDirectoryW(buffer, MAX_PATH)`.
- **`appDataLocalDir`**: `::SHGetKnownFolderPath(FOLDERID_LocalAppData, ...)`, free with `::CoTaskMemFree`.
- **`appDataRoamingDir`**: `::SHGetKnownFolderPath(FOLDERID_RoamingAppData, ...)`, free with `::CoTaskMemFree`.

Headers needed: `<knownfolders.h>`, `<shlobj.h>`.

#### I/O — IOCP (`Core.HAL.windows.Io.cpp`)

- **`init`**: Create an I/O Completion Port via `::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0)`. Wrap in `IoHandleData { HANDLE m_port }`.
- **`openFile`**: Open with `::CreateFileW(... FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN)`, then associate with the IOCP via `::CreateIoCompletionPort(file, port, key, 0)`. Enable `FILE_SKIP_SET_EVENT_ON_HANDLE`. Wrap in `FileHandleData { HANDLE m_file }`.
- **`submit`**: For each entry, placement-new an `OverlappedExt` (extends `OVERLAPPED` with `m_user_data`) into the provided storage, set `Offset`/`OffsetHigh`, call `::ReadFile` or `::WriteFile`. If the call fails with something other than `ERROR_IO_PENDING`, post a failure completion via `::PostQueuedCompletionStatus`.
- **`poll` / `wait`**: Call `::GetQueuedCompletionStatusEx` with `timeout_ms = 0` (poll) or `INFINITE` (wait). Drain into `CompletionEntry` array. Extract `m_user_data` from `OverlappedExt`. Recover error via `::GetOverlappedResult`.
- **`wake`**: `::PostQueuedCompletionStatus(port, 0, 0, nullptr)`.
- **`cancelIo`**: `::CancelIoEx(file, overlapped)`.
- **`closeFile`**: `::CancelIoEx` then `::CloseHandle`.

`OverlappedExt` layout:

```cpp
struct OverlappedExt : public OVERLAPPED {
    void *m_user_data{nullptr};
};
static_assert(sizeof(OverlappedExt) <= overlapped_storage_size_v);  // 64 bytes
```

#### I/O Mapped files (`Core.HAL.windows.IoMap.cpp`)

- **`mapFile`**: Open with `::CreateFileW`, query size with `::GetFileSizeEx`, create file mapping with `::CreateFileMappingW`, map view with `::MapViewOfFile` using `FILE_MAP_READ | FILE_MAP_WRITE`. Wrap in `MapHandleData { HANDLE m_mapping, void *m_data, size_t m_size }`. Empty files return a valid handle with `m_data == nullptr`, `m_size == 0`.
- **`unmapFile`**: `::UnmapViewOfFile` then `::CloseHandle` on the mapping.
- **`mapData` / `mapSize`**: Return the cached pointer/size from `MapHandleData`.

#### I/O Directory watching — `ReadDirectoryChangesW` (`Core.HAL.windows.IoWatch.cpp`)

- **`openWatch`**: Open directory with `::CreateFileW(FILE_LIST_DIRECTORY, FILE_SHARE_READ|WRITE|DELETE, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED)`. Create a manual-reset event with `::CreateEventW`. Start the first read with `::ReadDirectoryChangesW`. Wrap in `WatchHandleData { HANDLE m_dir, HANDLE m_event, byte m_buffer[65536], OVERLAPPED m_overlapped, bool m_pending, bool m_recursive }`.
- **`startWatch_`**: Reset `m_overlapped`, call `::ReadDirectoryChangesW` with filter `FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE`.
- **`pollWatch` / `waitWatch`**: `::WaitForSingleObject(m_event, timeout_ms)`. On success, `::GetOverlappedResult` then `::ResetEvent`. Copy up to `buffer.size()` bytes of raw `FILE_NOTIFY_INFORMATION` records into the output buffer. Restart the watch immediately. `bytes_returned == 0` → `ec = result_out_of_range`.
- **`closeWatch`**: `::CancelIoEx`, `::WaitForSingleObject`, close event handle and directory handle.
- **`parseWatchEvents`**: Walk the `FILE_NOTIFY_INFORMATION` linked list. Map `FILE_ACTION_*` constants to `WatchEvent::Action`. Convert wide filenames to UTF-8 via `::WideCharToMultiByte(CP_UTF8, ...)`. Return the number of events parsed.

#### Strings (`Core.HAL.windows.Strings.cpp`)

Uses the Windows API conversion functions:

| From | To | Function |
|------|----|----------|
| `ansi` (CP_ACP) | `char8_t` | Direct `memcpy` (same size) |
| `ansi` (CP_ACP) | `wchar_t` | `::MultiByteToWideChar(CP_ACP, ...)` |
| `utf8` | `wchar_t` | `::MultiByteToWideChar(CP_UTF8, ...)` |
| `wchar_t` | `utf8` | `::WideCharToMultiByte(CP_UTF8, ...)` |
| `wchar_t` | `ansi` (CP_ACP) | `::WideCharToMultiByte(CP_ACP, ...)` |
| `utf8` | `ansi` | Convert via `wchar_t` intermediate then to ANSI |

#### System (`Core.HAL.windows.System.cpp`)

- **`platformName`**: Returns `"windows"`.
- **`userName`**: `::GetUserNameW(buffer, &size)`, convert via `native::ansi`.
- **`Uuid`**: `::BCryptGenRandom(nullptr, ..., BCRYPT_USE_SYSTEM_PREFERRED_RNG)` (links `bcrypt.lib`).

#### Process (`Core.HAL.windows.Process.cpp`)

- **`currentExecutablePath`**: `::GetModuleFileNameW(nullptr, buffer, MAX_PATH)`.
- **`spawnAndWait`**: Build command line with quoted arguments, call `::CreateProcessW` with `CREATE_NO_WINDOW | CREATE_DEFAULT_ERROR_MODE`, `STARTF_USESHOWWINDOW` with `SW_HIDE`. Wait with `::WaitForSingleObject(hProcess, INFINITE)`. Get exit code with `::GetExitCodeProcess`.
- **`terminateProcess`**: Declared in the interface; not yet defined.

#### Timer (`Core.HAL.windows.Timer.cpp`)

- **`setDeadline`**: `::CreateTimerQueueTimer` with a callback lambda that atomically checks `m_fired` and invokes the user callback. Wrap in `TimerData { move_only_function, atomic<bool>, HANDLE h_timer }`.
- **`cancelDeadline`**: Set `m_fired = true`, `::DeleteTimerQueueTimer(nullptr, h_timer, INVALID_HANDLE_VALUE)`, null out `handle.m_data`.

#### Random (`Core.HAL.windows.Random.cpp`)

This is a free function in `namespace pP` (not `pP::hal`):

```cpp
// In Core.HAL.cppm:
// [[nodiscard]] std::mt19937_64 randomNumberGenerator() noexcept;

// In Core.HAL.windows.Random.cpp:
std::mt19937_64 randomNumberGenerator() noexcept {
    std::array<std::uint32_t, 8> seed_data{};
    std::random_device rd;
    for (auto &x: seed_data) { x = rd(); }
    std::seed_seq seq(seed_data.begin(), seed_data.end());
    return std::mt19937_64(seq);
}
```

On MSVC, `std::random_device` is backed by the OS RNG. This file does NOT
include `Core.HAL.windows.include.hpp`; it only includes `"pP/Macros.h"`.

### 4.2 Linux

#### Memory (`Core.HAL.linux.Memory.cpp`)

- **`page_size` / `page_granularity`**: `::sysconf(_SC_PAGESIZE)` (typically 4096).
- **`pageAlloc`**: `::mmap(nullptr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)`. If `commit==false`, call `::madvise(ptr, size, MADV_DONTNEED)` after mapping to zero-fill and release pages. Throws `std::bad_alloc`.
- **`pageCommit`**: `::mprotect(ptr, size, prot)`. Throws `std::bad_alloc`.
- **`pageDecommit`**: `::madvise(ptr, size, MADV_DONTNEED)`. Throws `std::bad_alloc`.
- **`pageProtect`**: `::mprotect(ptr, size, prot)`. Throws `std::bad_alloc`.
- **`pageOfferToOS`**: Prefer `MADV_FREE` if available, else `MADV_DONTNEED`. Throws `std::bad_alloc`.
- **`pageReclaimFromOS`**: Always returns `true` (Linux does not track reclaimed pages at user level).
- **`pageFree`**: `::munmap(ptr, size)`. Throws `std::bad_alloc`.
- **Note**: Linux does not support custom alignment for `mmap` beyond page size. The `alignment` parameter must equal `page_granularity`.

#### Debugger (`Core.HAL.linux.Debugger.cpp`)

- **`outputDebug`**: `::write(STDERR_FILENO, ...)` (debug builds only).
- **`outputDebug(native)`**: Convert via `toString<char>` first, then write to stderr.
- **`isDebuggerPresent`**: Returns `false` (no portable Linux API; can be enhanced with `/proc/self/status` TracerPid check).
- **`breakpoint`**: `::raise(SIGTRAP)` (debug builds only).
- **`breakpointIfDebugging`**: Check `isDebuggerPresent()` first (currently always false).
- **`disableSystemErrorReporting`**: No-op (empty body).
- **`installDebugAssertHooks`**: `std::signal(SIGABRT, ...)` → write "SIGABRT received" to stderr + `::_Exit(3)`. Guarded by `PPR_ENABLE_ASSERTIONS`.
- **Thread names**: `currentThreadId` → `::syscall(SYS_gettid)`. `setThreadName` → `::prctl(PR_SET_NAME, ...)` (15-char `comm` limit). `getThreadName` → read `/proc/<tid>/comm` (strip trailing newline).

#### Filesystem (`Core.HAL.linux.Filesystem.cpp`)

- **`homeDir`**: `std::getenv("HOME")`, fallback `::getpwuid(::getuid())->pw_dir`.
- **`systemDir`**: Returns `"/usr/bin"`.
- **`appDataLocalDir`**: `$XDG_DATA_HOME`, fallback `$HOME/.local/share`.
- **`appDataRoamingDir`**: `$XDG_CONFIG_HOME`, fallback `$HOME/.config`.

#### I/O — io_uring (`Core.HAL.linux.Io.cpp`)

**Currently a stub** — `init`/`openFile` throw `std::system_error(operation_not_supported)`; the rest return 0/no-op. The structs `IoHandleData` (with `int m_ring_fd`) and `FileHandleData` (with `int m_fd`) are defined ready for implementation.

#### I/O Mapped files (`Core.HAL.linux.IoMap.cpp`)

- **`mapFile`**: `::open(path.c_str(), oflags)`, `::fstat(fd, &st)`, `::mmap(nullptr, st.st_size, prot, MAP_SHARED, fd, 0)`. Wrap in `MapHandleData { void *m_data, size_t m_size }`. Empty files return a valid handle with `m_data == nullptr`.
- **`unmapFile`**: `::munmap(data->m_data, data->m_size)` then `delete`.
- **`mapData` / `mapSize`**: Return cached values.

#### I/O Directory watching — inotify (`Core.HAL.linux.IoWatch.cpp`)

- **`openWatch`**: `::inotify_init1(IN_CLOEXEC | IN_NONBLOCK)`, `::inotify_add_watch(fd, dir.c_str(), IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_ONLYDIR | IN_EXCL_UNLINK)`. For recursive mode, traverse subdirectories and add watches with relative paths stored in `m_wd_to_relpath`.
- **`pollWatch`**: `::read(m_inotify_fd, ...)` into a local 16 KiB buffer, then convert each `inotify_event` into a binary output format: `u8 action` + `u32 name_len` + `char name[name_len]`. Recursively add new subdirectory watches on `IN_CREATE | IN_ISDIR`. `IN_Q_OVERFLOW` → `ec = result_out_of_range`.
- **`waitWatch`**: `::poll(&pfd, 1, -1)` then `pollWatch`.
- **`closeWatch`**: `::inotify_rm_watch` for each watch descriptor, `::close(m_inotify_fd)`.
- **`parseWatchEvents`**: Parse the same binary format: read `u8 action`, `u32 name_len`, `char name[name_len]`, populate `WatchEvent` and `out_names`.

#### Strings (`Core.HAL.linux.Strings.cpp`)

All implemented directly (no OS calls):

| From | To | Method |
|------|----|--------|
| `ansi` | `char8_t` | `memcpy` (same size) |
| `ansi` | `wchar_t` | Per-character zero-extend |
| `utf8` | `wchar_t` | Manual UTF-8 decoding to `wchar_t` (handles 1-4 byte sequences) |
| `wchar_t` | `char8_t` | Manual UTF-8 encoding from `wchar_t` (handles 1-3 byte sequences) |
| `wchar_t` | `char` | ASCII subset only; non-ASCII replaced with `'?'` |
| `utf8` | `char` | `memcpy` (same size) |

#### System (`Core.HAL.linux.System.cpp`)

- **`platformName`**: Returns `"linux"`.
- **`userName`**: `std::getenv("USER")`, fallback `::getpwuid(::getuid())->pw_name`.

#### Process (`Core.HAL.linux.Process.cpp`)

- **`currentExecutablePath`**: `std::filesystem::read_symlink("/proc/self/exe")`.
- **`spawnAndWait`**: `::fork()`, child calls `::execvp` (exit 127 on failure), parent calls `::waitpid`. Returns `WEXITSTATUS(status)`.
- **`terminateProcess`**: Declared in the interface; not yet defined.

#### Timer (`Core.HAL.linux.Timer.cpp`)

- **`setDeadline`**: `::timer_create(CLOCK_MONOTONIC, &sev, &timer_id)` with `SIGEV_THREAD`, `::timer_settime`. Wrap in `TimerData { move_only_function, atomic<bool>, timer_t }`.
- **`cancelDeadline`**: `::timer_delete`, atomic flag check, `delete` data.

### 4.3 Darwin (macOS)

#### Memory (`Core.HAL.darwin.Memory.cpp`)

- **`page_size` / `page_granularity`**: `::sysconf(_SC_PAGESIZE)` (typically 16384 on Apple Silicon, 4096 on Intel).
- **`pageAlloc`**: `::mmap(nullptr, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0)`. Throws `std::bad_alloc`. Always commits (the `commit` parameter is ignored on Darwin since `mmap` always maps physical pages lazily). Unpoison.
- **`pageCommit`**: `::mprotect(ptr, size, prot)`. Throws `std::bad_alloc`. Unpoison.
- **`pageDecommit`**: `::madvise(ptr, size, MADV_FREE)`. **No poison** (see §3.4 rule). Throws `std::bad_alloc`.
- **`pageProtect`**: `::mprotect(ptr, size, prot)`. Throws `std::bad_alloc`.
- **`pageOfferToOS`**: `::madvise(ptr, size, MADV_FREE)`. **No poison**. Throws `std::bad_alloc`.
- **`pageReclaimFromOS`**: Unpoisons memory, returns `true`.
- **`pageFree`**: `::munmap(ptr, size)`. **No poison**. Throws `std::bad_alloc`.
- **Note**: Darwin does not support custom alignment for `mmap`; `alignment` must equal `page_granularity`.

#### Debugger (`Core.HAL.darwin.Debugger.cpp`)

- **`outputDebug`**: `::write(STDERR_FILENO, ...)` (debug builds only).
- **`outputDebug(native)`**: Convert via `toString<char>` first, then write to stderr.
- **`isDebuggerPresent`**: `::sysctl` with `CTL_KERN`, `KERN_PROC`, `KERN_PROC_PID`, `getpid()` → check `kp_proc.p_flag & P_TRACED`. Debug builds only.
- **`breakpoint`**: `__builtin_trap()` (debug builds only).
- **`breakpointIfDebugging`**: Check `isDebuggerPresent()` first.
- **`disableSystemErrorReporting`**: No-op (empty body).
- **`installDebugAssertHooks`**: `std::signal(SIGABRT, ...)` → write "SIGABRT received" to stderr + `::_Exit(3)`. Guarded by `PPR_ENABLE_ASSERTIONS`.
- **Thread names**: `currentThreadId` → `::pthread_threadid_np(nullptr, &tid)`. `setThreadName` → `::pthread_setname_np` (63-char `MAXTHREADNAMESIZE` limit). `getThreadName` → `::task_threads` + `::pthread_from_mach_thread_np` + `::pthread_getname_np` (deprecation warning suppressed via `PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(-Wdeprecated-declarations)`).

#### Filesystem (`Core.HAL.darwin.Filesystem.cpp`)

- **`homeDir`**: `std::getenv("HOME")`, fallback `::getpwuid(::getuid())->pw_dir`.
- **`systemDir`**: Returns `"/Applications"`.
- **`appDataLocalDir`**: `$HOME/Library/Application Support`.
- **`appDataRoamingDir`**: Same as `appDataLocalDir` (no roaming concept on macOS).

#### I/O — kqueue (`Core.HAL.darwin.Io.cpp`)

**Currently a stub** — `init`/`openFile` throw `std::system_error(operation_not_supported)`; the rest return 0/no-op. The structs `IoHandleData` (with `int m_kq`) and `FileHandleData` (with `int m_fd`) are defined ready for implementation.

#### I/O Mapped files (`Core.HAL.darwin.IoMap.cpp`)

Identical to Linux: `::open`, `::fstat`, `::mmap(MAP_SHARED)`. Same `MapHandleData` structure.

#### I/O Directory watching (`Core.HAL.darwin.IoWatch.cpp`)

**Currently a stub** — `openWatch` throws `operation_not_supported`; all other functions return 0 or no-op.

#### Strings (`Core.HAL.darwin.Strings.cpp`)

Identical implementation to Linux — manual UTF-8 encoding/decoding, no OS conversion calls.

#### System (`Core.HAL.darwin.System.cpp`)

- **`platformName`**: Returns `"darwin"`.
- **`userName`**: `std::getenv("USER")`, fallback `::getpwuid(::getuid())->pw_name`.

#### Process (`Core.HAL.darwin.Process.cpp`)

- **`currentExecutablePath`**: `::_NSGetExecutablePath(nullptr, &buf_size)`, then `::_NSGetExecutablePath(path_buf.data(), &buf_size)`. Requires `<crt_externs.h>`.
- **`spawnAndWait`**: `::fork()`, child `::execvp`, parent `::waitpid`. Same pattern as Linux.
- **`terminateProcess`**: Declared in the interface; not yet defined.

#### Timer (`Core.HAL.darwin.Timer.cpp`)

- **`setDeadline`**: `::timer_create(CLOCK_MONOTONIC, SIGEV_THREAD)`, `::timer_settime`. Identical to Linux implementation.
- **`cancelDeadline`**: `::timer_delete`, atomic flag check.

### 4.4 Generic (Stub)

The generic platform provides minimal stub implementations that compile
everywhere but do not perform real I/O or memory management. It is used when the
platform is unknown/unsupported.

#### System (`Core.HAL.generic.System.cpp`)

- **`platformName`**: Returns `"generic"`.
- **`userName`**: `std::getenv("USER")`, fallback `std::getenv("USERNAME")`, fallback `"unknown_user"`.

#### Memory (`Core.HAL.generic.Memory.cpp`)

- **`page_size`**: Hardcoded `4096u`.
- **`page_granularity`**: Hardcoded `{4096u}`.
- **`pageAlloc`**, **`pageCommit`**: Throw `std::bad_alloc`.
- **`pageDecommit`**, **`pageProtect`**, **`pageOfferToOS`**, **`pageFree`**: No-ops. The generic platform never actually allocates pages.
- **`pageReclaimFromOS`**: Returns `false`.
- There is no `ringBufferAlloc`/`ringBufferFree` (these are Windows-only).

#### Debugger (`Core.HAL.generic.Debugger.cpp`)

- **`outputDebug`**: No-ops.
- **`isDebuggerPresent`**: Returns `false`.
- **`breakpoint`**, **`breakpointIfDebugging`**: No-ops.
- **`disableSystemErrorReporting`**, **`installDebugAssertHooks`**: No-ops.
- **Thread names**: `currentThreadId` returns `ThreadId{0u}`; `setThreadName` no-op; `getThreadName` returns `0u`.

#### Filesystem (`Core.HAL.generic.Filesystem.cpp`)

- **`homeDir`**: Tries `HOME`, `USERPROFILE`, `HOMEDRIVE`+`HOMEPATH`, falls back to `std::filesystem::current_path()`.
- **`systemDir`**: Tries `WINDIR`, falls back to `"/"`.
- **`appDataLocalDir`** / **`appDataRoamingDir`**: Return `homeDir()`.

#### I/O (`Core.HAL.generic.Io.cpp`)

- **`init`**: Throws `std::system_error(errc::operation_not_supported)`.
- **`deinit`**, **`closeFile`**: No-ops.
- **`openFile`**: Throws `operation_not_supported`.
- **`submit`**, **`poll`**, **`wait`**: Return `0u`.
- **`wake`**: No-op.
- **`cancelIo`**: Not defined (declared in the interface only).

#### I/O Mapped files (`Core.HAL.generic.IoMap.cpp`)

- **`mapFile`**: Throws `operation_not_supported`.
- **`unmapFile`**: No-op.
- **`mapData`**: Returns `nullptr`.
- **`mapSize`**: Returns `0u`.

#### I/O Directory watching (`Core.HAL.generic.IoWatch.cpp`)

Uses a **polling fallback** — takes a snapshot of directory contents on
`openWatch`, then compares with a new snapshot on each `pollWatch` to detect
added/removed/modified files.

- **`openWatch`**: Stores directory path, builds initial snapshot via `directory_iterator`/`recursive_directory_iterator`, recording `last_write_time` per file.
- **`pollWatch`**: Builds a new snapshot, compares with stored snapshot. Writes `(action: u8) + (name_len: u32) + (name: char[])` records into the output buffer.
- **`waitWatch`**: Polls in a loop with 10 ms sleep.
- **`parseWatchEvents`**: Reads the same binary format: `u8 action`, `u32 name_len`, `char name[name_len]`.

#### Strings (`Core.HAL.generic.Strings.cpp`)

Simple per-character conversions — no proper UTF-8 handling, lossy at the byte level:

| From | To | Method |
|------|----|--------|
| `ansi` | `char8_t` | `memcpy` |
| `ansi` | `wchar_t` | Zero-extend each byte |
| `utf8` | `wchar_t` | Zero-extend each byte (lossy for multi-byte) |
| `wchar_t` | `char8_t` | Truncate to 8 bits |
| `wchar_t` | `char` | Truncate to 8 bits |
| `utf8` | `char` | `memcpy` |

#### Process (`Core.HAL.generic.Process.cpp`)

- **`currentExecutablePath`**: Throws `std::runtime_error`.
- **`spawnAndWait`**: Throws `std::runtime_error`.

#### Timer (`Core.HAL.generic.Timer.cpp`)

- **`setDeadline`**: Creates a `std::jthread` that sleeps for `ms`, then invokes the callback. Uses `std::stop_token` for cancellation. Wrap in `TimerData { move_only_function, atomic<bool>, std::jthread }`.
- **`cancelDeadline`**: Sets `m_fired = true`, calls `request_stop()` and `join()` on the thread.

## 5. Stub Conventions

When implementing the generic (stub) platform or stubbing a not-yet-implemented
function on a real platform, follow these rules:

| Category | Convention |
|----------|-----------|
| **Memory allocation** (`pageAlloc`, `pageCommit`) | Throw `std::bad_alloc` |
| **Memory deallocation** (`pageDecommit`, `pageFree`, `pageOfferToOS`, `pageProtect`) | No-op (silently succeed for operations that release resources) |
| **Memory query** (`pageReclaimFromOS`) | Return `false` |
| **Process spawn/query** (`currentExecutablePath`, `spawnAndWait`) | Throw `std::runtime_error` with a descriptive message |
| **Process termination** (`terminateProcess`) | `[[noreturn]]` — call `std::abort()` or a platform equivalent |
| **I/O lifecycle** (`init`) | Throw `std::system_error(errc::operation_not_supported, ...)` |
| **I/O operations** (`openFile`, `submit`, `poll`, `wait`) | `openFile` throws; others return 0/no-op |
| **Mapped files** (`mapFile`) | Throw `std::system_error(errc::operation_not_supported, ...)` |
| **Directory watching** (`openWatch`) | Stub may throw or provide a polling fallback. Generic provides a polling implementation. |
| **Debugger** (`outputDebug`, `breakpoint`, `isDebuggerPresent`) | No-op or return `false` |
| **Timer** (`setDeadline`) | Implement with `std::jthread` + `sleep_for` when platform lacks `timer_create`/`CreateTimerQueueTimer` |

### Common error message patterns

```cpp
throw std::system_error(
    std::make_error_code(std::errc::operation_not_supported),
    "pP::io: <operation> not supported on this platform");
```

```cpp
throw std::runtime_error("<operation> not implemented for <platform> platform");
```

## 6. Testing HAL Implementations

### Test structure

Unit tests live in `lib/engine/tests/core/` as module partitions, e.g.
`Core.HAL.Tests.cppm` (`export module engine.tests.core:hal;`) and
`Core.Io.Tests.cppm` (`export module engine.tests.core:io;`). Test files include
the test-only header `"pP/UnitTest.h"` and use `PPR_UNIT_TEST` / `PPR_TEST_ASSERT`
— never `PPR_ASSERT`/`PPR_VERIFY`, which compile to `[[assume]]` in release:

```cpp
module;
#include "pP/UnitTest.h"

export module engine.tests.core:hal;

import engine.core;
import std;

export namespace pP::tests {
    namespace HALTests {
        PPR_UNIT_TEST(thread_id) {
            const auto tid = hal::currentThreadId();
            PPR_TEST_ASSERT(tid == hal::currentThreadId());
            if (hal::platformName() != "generic") {
                PPR_TEST_ASSERT(tid.m_value != 0u);
            }
        };
    }

    PPR_UNIT_TEST(hal) {
        _.recurse({
            HALTests::thread_id,
            HALTests::set_get_name_roundtrip,
            HALTests::buffer_truncation,
            HALTests::worker_thread_name,
        });
    };
}
```

Groups are registered in `Core.Tests.cppm` via `_.recurse({ ... })`. The
existing `Core.HAL.Tests.cppm` covers: `thread_id`, `set_get_name_roundtrip`,
`buffer_truncation`, `worker_thread_name` — all guarded by
`hal::platformName() != "generic"` where the generic stub cannot satisfy them.

### What to test for each platform

#### Page allocation round-trip

```cpp
PPR_UNIT_TEST(page_alloc_free) {
    const std::size_t size = hal::page_size;
    auto [ptr, actual] = hal::pageAlloc(size);
    PPR_TEST_ASSERT(ptr != nullptr);
    PPR_TEST_ASSERT(actual >= size);
    PPR_TEST_ASSERT(reinterpret_cast<std::uintptr_t>(ptr) % hal::page_size == 0u);

    // Write to every page
    std::memset(ptr, 0xAB, size);

    hal::pageFree(ptr, actual);
};
```

#### Page protection

```cpp
PPR_UNIT_TEST(page_protect_readonly) {
    auto [ptr, size] = hal::pageAlloc(hal::page_size, true, {.read = true, .write = true});
    PPR_TEST_ASSERT(ptr != nullptr);

    // Write, then switch to read-only
    static_cast<std::byte *>(ptr)[0] = std::byte{0x42};
    hal::pageProtect(ptr, hal::page_size, {.read = true, .write = false});

    // Read should succeed
    volatile auto val = static_cast<std::byte *>(ptr)[0];
    (void)val;

    hal::pageFree(ptr, size);
};
```

#### Commit/Decommit round-trip

```cpp
PPR_UNIT_TEST(page_commit_decommit) {
    auto [ptr, size] = hal::pageAlloc(hal::page_size, false);
    PPR_TEST_ASSERT(ptr != nullptr);

    hal::pageCommit(ptr, hal::page_size);
    std::memset(ptr, 0xCD, hal::page_size);

    hal::pageDecommit(ptr, hal::page_size);
    // After decommit, memory should be inaccessible or zero-filled

    hal::pageFree(ptr, size);
};
```

#### Ring buffer identity (Windows only)

```cpp
PPR_UNIT_TEST(ring_buffer_wraparound) {
    const std::size_t buf_size = hal::page_size * 4u;
    auto *buf = static_cast<std::byte *>(hal::ringBufferAlloc(buf_size));
    PPR_TEST_ASSERT(buf != nullptr);
    PPR_DEFER { hal::ringBufferFree(buf, buf_size); };

    // Write at offset 0
    buf[0] = std::byte{0xAA};
    // Read at offset + buf_size (should map to same physical page)
    PPR_TEST_ASSERT(buf[buf_size] == std::byte{0xAA});
};
```

#### Debugger detection

```cpp
PPR_UNIT_TEST(debugger_initial_state) {
    // Should not crash, should return a deterministic value
    const bool present = hal::isDebuggerPresent();
    // Typically false when not debugging
    PPR_TEST_ASSERT(present == false || present == true);
};
```

#### Debug output

```cpp
PPR_UNIT_TEST(debug_output_no_crash) {
    hal::outputDebug("HAL test: debug output\n");
    hal::outputDebugFmt("HAL test: formatted {} + {}\n", 42, "hello");
};
```

#### Timer basic

```cpp
PPR_UNIT_TEST(timer_set_and_cancel) {
    std::atomic<bool> fired{false};
    auto handle = hal::timer::setDeadline(
        std::chrono::milliseconds(100),
        [&fired] { fired = true; });
    hal::timer::cancelDeadline(handle);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    PPR_TEST_ASSERT(fired == false);
};
```

#### Timer fires

```cpp
PPR_UNIT_TEST(timer_fires) {
    std::atomic<bool> fired{false};
    auto handle = hal::timer::setDeadline(
        std::chrono::milliseconds(10),
        [&fired] { fired = true; });
    PPR_DEFER { hal::timer::cancelDeadline(handle); };
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    PPR_TEST_ASSERT(fired == true);
};
```

#### Process spawn

```cpp
PPR_UNIT_TEST(spawn_and_wait) {
    // On Windows:
    int code = hal::process::spawnAndWait("cmd.exe", {"/c", "exit 42"});
    PPR_TEST_ASSERT(code == 42);

    // On Linux/Darwin:
    // int code = hal::process::spawnAndWait("/bin/sh", {"-c", "exit 42"});
    // PPR_TEST_ASSERT(code == 42);
};
```

#### String transcoding round-trip

```cpp
PPR_UNIT_TEST(transcode_roundtrip) {
    const std::string_view original = "Hello, HAL!";
    const auto wide = hal::toString<wchar_t>(original);
    const auto back = hal::toString<char>(std::wstring_view(wide));
    PPR_TEST_ASSERT(back == original);
};
```

#### Platform name

```cpp
PPR_UNIT_TEST(platform_name_not_empty) {
    const auto name = hal::platformName();
    PPR_TEST_ASSERT(!name.empty());
    PPR_TEST_ASSERT(name == "windows" || name == "linux" || name == "darwin" || name == "generic");
};
```

#### Filesystem directories

```cpp
PPR_UNIT_TEST(directories_not_empty) {
    PPR_TEST_ASSERT(!hal::homeDir().path().empty());
    PPR_TEST_ASSERT(!hal::systemDir().path().empty());
    // Local/roaming may be empty on some platforms; check existence instead
    PPR_TEST_ASSERT(std::filesystem::exists(hal::homeDir().path()));
};
```

#### Mapped file I/O

```cpp
PPR_UNIT_TEST(mapped_file_read) {
    const auto path = std::filesystem::temp_directory_path() / "ppr_hal_mmap_test.bin";
    PPR_DEFER { std::error_code ec; std::filesystem::remove(path, ec); };
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs.write("HAL content", 11);
    }

    auto map = hal::io::mapFile(path, {});
    PPR_TEST_ASSERT(map != nullptr);
    PPR_DEFER { hal::io::unmapFile(map); };

    PPR_TEST_ASSERT(hal::io::mapSize(map) == 11);
    auto *data = static_cast<const char *>(hal::io::mapData(map));
    PPR_TEST_ASSERT(data != nullptr);
    PPR_TEST_ASSERT(std::string_view(data, 11) == "HAL content");
};
```

### Test execution

```bash
# Build and run (EngineCoreTests is GLFW-free; EngineAppTests links GLFW)
cmake --build out/build/msvc-dev --target EngineCoreTests

# Run a specific test (paths use '/' separators)
out/build/msvc-dev/bin/EngineCoreTests --run-test core/hal/thread_id

# Full suite with shuffle/loop
out/build/msvc-dev/bin/EngineCoreTests --shuffle --loop 10
```

The aggregate target `run-engine-tests` runs both `EngineCoreTests` and
`EngineAppTests`. Fork/crash tests spawn child processes via
`hal::process::spawnAndWait`; assertions are intercepted by the test framework
(converted to failures, not terminations).

## 7. Adding a New Platform

Follow this checklist to add HAL support for a new platform. Use the source tree
and build system in this skill as a reference.

### Step-by-step checklist

1. **Register the platform in `cmake/HAL.cmake`**
   - Add a new `elseif()` branch that sets `PPR_HAL_PLATFORM` to your platform identifier (e.g. `"freebsd"`).

   ```cmake
   elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
       set(PPR_HAL_PLATFORM freebsd)
   ```

2. **Create the platform directory**
   ```
   lib/engine/core/hal/freebsd/
   ```

3. **Create the 10 required implementation files**
   Each file follows the pattern:
   ```
   hal/freebsd/Core.HAL.freebsd.Memory.cpp
   hal/freebsd/Core.HAL.freebsd.Debugger.cpp
   hal/freebsd/Core.HAL.freebsd.Filesystem.cpp
   hal/freebsd/Core.HAL.freebsd.Io.cpp
   hal/freebsd/Core.HAL.freebsd.IoMap.cpp
   hal/freebsd/Core.HAL.freebsd.IoWatch.cpp
   hal/freebsd/Core.HAL.freebsd.Process.cpp
   hal/freebsd/Core.HAL.freebsd.Strings.cpp
   hal/freebsd/Core.HAL.freebsd.System.cpp
   hal/freebsd/Core.HAL.freebsd.Timer.cpp
   ```

4. **For each file, implement the module structure**
   ```cpp
   module;

   #include "pP/Macros.h"  // or a platform-specific preamble header

   module engine.core;

   import :assert;
   import :hal;
   import :memory;

   import std;

   namespace pP::hal {
       // ... implementations ...
   }
   ```

5. **Implement each API function from Section 3**
   - Start by copying the `generic` stubs.
   - Replace stubs with real system calls one function at a time.
   - Run the corresponding test after each function is implemented.
   - For complex APIs (I/O, directory watching), you may leave stubs that throw `operation_not_supported`.

6. **If the platform uses wide native strings** (like Windows):
   - Override `native::string` / `native::char_t` / `native::string_view` in `Core.HAL.cppm` (currently these are an alias for `std::filesystem::path::string_type`, which is platform-dependent automatically).
   - Implement `transcode` functions using the platform's wide-string conversion APIs.

7. **If extra platform-specific source files are needed** (like Windows `Random.cpp` and `RingBuffer.cpp`):
   - Add them to `lib/engine/core/CMakeLists.txt` inside a guard:

   ```cmake
   if(PPR_HAL_PLATFORM STREQUAL "freebsd")
       list(APPEND HAL_PLATFORM_SOURCES
           freebsd/Core.HAL.freebsd.Random.cpp
       )
   endif()
   ```

8. **Update cmake presets** (optional):
   - Add a new preset pair (`freebsd-dev`, `freebsd-rel`) in `CMakePresets.json` if the platform supports CMake presets.

9. **Add at least one test per area** (see Section 6).

10. **Verify the full API compiles and links**:
    ```bash
    cmake --preset freebsd-dev
    cmake --build --preset freebsd-dev --target EngineCore
    ```

### Sources of inspiration

- Windows: `VirtualAlloc2`/`MapViewOfFile3` (ring buffer), IOCP (`CreateIoCompletionPort`), `ReadDirectoryChangesW`, `SetThreadDescription`.
- Linux: `mmap`/`madvise`, inotify, `timer_create`, `prctl(PR_SET_NAME)`, `/proc/self/exe`.
- Darwin: `mmap`/`madvise`, `_NSGetExecutablePath`, `pthread_setname_np`, `task_threads`.
- Generic: `std::jthread` timers, polling directory snapshots, `std::filesystem` directory iteration.

## Subagent routing
| Step | Delegate to | Why |
|------|-------------|-----|
| Locate existing HAL patterns / APIs | `@explorer` | Template discovery |
| Implement a new platform / area | `@fixer` + `@oracle` | Bounded impl + architecture |
| Generate `PPR_UNIT_TEST` bodies | `@fixer` | Test scaffolding |
| Compile + run HAL tests | background build subagent | Reuse validation lane |

## OMO feature wiring
- **Per-agent `skills`/`mcps` allow-lists** — `@fixer` `skills: []`; restrict HAL edits to `lib/engine/core/hal/<platform>/` + `cmake/HAL.cmake` via allow-list.
- **Background orchestration** — run HAL tests as a background subagent in parallel with impl; orchestrator waits on the Job Board, not polling.
- **Session reuse** — reuse a specialist session only when its session key matches `(agent-type, hal-<platform>-<area>, lib/engine/core/hal/<platform>/Core.HAL.<platform>.<Area>.cpp)`; MRU is a tiebreaker only. Invalidate sessions older than the threshold or whose key no longer matches. Never reuse mutating/debug sessions — prefer fresh for impl; read-only recon sessions (`@explorer`) are safe to reuse.
- **Custom agent** — optionally a `hal-impl` custom agent that scaffolds the 10 platform `.cpp` files from this skill's checklist.
- **`orchestratorPrompt` routing** — trigger on 'add HAL platform', 'implement <area> for <platform>', 'HAL test for…'.