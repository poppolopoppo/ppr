# include/

## Responsibility

Public header root for the PPR engine. Holds the single non-module header `pP/Macros.h` — the engine-wide
preprocessor vocabulary (build-mode + memory-poisoning detection, pointer-size selection, macro helpers,
compiler-attribute portability, wide-char literals, assertions, logging, error-propagation returns, RAII
helpers) included by every module partition and the game entry point.

## Design

- Only one shipped file: `pP/Macros.h`. It is NOT a module — preprocessor-only, `#include`d in the global
  fragment (`module; #include "pP/Macros.h"`) before any module scanning.
- Build-mode detection: `PPR_ENABLE_DEBUG` (`_DEBUG`/`!NDEBUG`) with `PPR_DECL_IF_DEBUG`/`PPR_EXPR_IF_DEBUG`;
  `PPR_ENABLE_MEMORY_POISONING`/`PPR_ENABLE_SAFE_OBJECT_TRACKING` when ASAN or debug; `PPR_ENABLE_ASSERTIONS`
  follows `PPR_ENABLE_DEBUG`; `PPR_ENABLE_LOGGING` is fixed `1`.
- Pointer size: `PPR_64BIT`/`PPR_32BIT` + `PPR_32BIT_OR_64BIT` selector (errors on unknown pointer size).
- Macro helpers: `PPR_EXPAND`/`PPR_EXPAND_VA`, `PPR_COMMA` chain + `PPR_COMMA_PROTECT`, `PPR_STRINGIZE`,
  `PPR_CONCAT`/`PPR_CONCAT3`; plus line-unique `PPR_ANONYMIZE` underpinning `PPR_DEFER` (scope-exit via
  `pP::Deferred`) and the assertion/error macros.
- Compiler attributes (MSVC / Clang+GCC / fallback): `PPR_ASSUME`, `PPR_FORCE_INLINE`/`PPR_NO_INLINE`/
  `PPR_FLATTEN`, `PPR_EMPTY_BASES`, `PPR_LIFETIME_BOUND`, `PPR_OFFSETOF`, `PPR_ATTRIBUTE_CODE_SEGMENT`
  (`.ppr_dbg` for assertion paths), `PPR_COMPILER_READWRITE_BARRIER`, `PPR_PRAGMA_WARNING_PUSH`/`POP`,
  `PPR_PRAGMA_WARNING_DISABLE_MSVC`/`DISABLE_GCC_CLANG`, `PPR_PRAGMA_SYSTEM_HEADER`.
- Wide-char handling: `TEXT` (MSVC `L##quote`, otherwise identity) and char-generic consteval
  `PPR_LITERAL_FOR` (`char`/`wchar_t`/`char8_t`, `std::unreachable` otherwise).
- Assertions: debug `PPR_DETAILS_ASSERTION_IMPL` evaluates once, honors `consteval` via `PPR_ASSUME`, else
  routes `Assertion::onFailure` (require/verify) from `.ppr_dbg` with `source_location`; `PPR_ASSERT`/
  `PPR_VERIFY` plus boolean `PPR_ENSURE`; release lowers to `PPR_ASSUME` (+ evaluate for `VERIFY`/`ENSURE`).
- Logging: `PPR_DECLARE`/`PPR_DEFINE_LOG_CATEGORY`, `PPR_LOG`/`PPR_LOG_RAW` (emitter + message + attributes),
  `PPR_FLUSH_LOG`.
- Error returns (log-and-return, `[[unlikely]]`): generic `PPR_RETURN_ON_FAIL` (`hasFailed` check, returns the
  failed value — covers `std::error_code` and `rhi::Result`); `make_error_code(__VA_ARGS__)` variants
  `PPR_RETURN_ERROR_ON_FAIL` (return code), `PPR_RETAIN_ERROR_ON_FAIL` (retain first failure),
  `PPR_RETURN_UNEXPECTED_ON_FAIL` (return `std::unexpected`), `PPR_LOG_WARNING_ON_FAIL` (log only).
- Test-only macros (`PPR_UNIT_TEST`, `PPR_TEST_ASSERT`) live separately in
  `lib/engine/tests/include/pP/UnitTest.h` and are NOT shipped here.

## Flow

Every `.cppm`/`.cpp` starts with `module; #include "pP/Macros.h"` (plus `game/main.cpp`); macros expand at
preprocessor time before module scanning and are available uniformly across all partitions — build flags feed
assertion/logging/error paths, attributes and helpers feed declaration-site annotations.

## Integration

- Consumed by: every engine module partition, `game/main.cpp`, all test targets.
- Provides: the assertion/logging/error-handling vocabulary used by `Core.Assert`, `Core.Logger`, and all
  `PPR_RETURN_*` / `PPR_RETAIN_*` / `PPR_LOG_WARNING_*` call sites (shader, RHI, app services).
- See [pP/Macros.h codemap](pP/codemap.md) for the full macro catalog.

## Key Files

- `pP/Macros.h` — engine-wide preprocessor macros (see [pP/codemap.md](pP/codemap.md)).
