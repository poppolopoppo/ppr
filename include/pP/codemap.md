# include/pP/

## Responsibility

Holds the engine's single public header `Macros.h` — the only non-module header in the codebase. Defines the
cross-cutting preprocessor vocabulary every module shares: build-mode detection, macro helpers, compiler-attribute
portability, assertions, logging, error-propagation returns, and RAII helpers.

## Design

- **Build-mode detection**: `PPR_ENABLE_DEBUG` (`_DEBUG`/`!NDEBUG`) with `PPR_DECL_IF_DEBUG`/`PPR_EXPR_IF_DEBUG`;
  `PPR_ENABLE_MEMORY_POISONING`/`PPR_ENABLE_SAFE_OBJECT_TRACKING` when ASAN or debug; `PPR_ENABLE_ASSERTIONS`
  = `PPR_ENABLE_DEBUG`; `PPR_ENABLE_LOGGING` fixed `1`.
- **Pointer size**: `PPR_64BIT`/`PPR_32BIT` + `PPR_32BIT_OR_64BIT` selector (errors on unknown pointer size).
- **Macro helpers**: `PPR_EXPAND`, `PPR_COMMA`/`PPR_COMMA_PROTECT`, `PPR_STRINGIZE`, `PPR_CONCAT`/`PPR_CONCAT3`.
- **Compiler attributes** (MSVC / Clang+GCC / fallback): `PPR_ASSUME`, `PPR_FORCE_INLINE`/`PPR_NO_INLINE`/
  `PPR_FLATTEN`, `PPR_EMPTY_BASES`, `PPR_LIFETIME_BOUND`, `PPR_OFFSETOF`, `PPR_ATTRIBUTE_CODE_SEGMENT`,
  `PPR_COMPILER_READWRITE_BARRIER`, `PPR_PRAGMA_WARNING_PUSH/POP`, `PPR_PRAGMA_WARNING_DISABLE_MSVC`/
  `DISABLE_GCC_CLANG`, `PPR_PRAGMA_SYSTEM_HEADER`; plus `TEXT` and char-generic `PPR_LITERAL_FOR`.
- **Assertions**: debug `PPR_DETAILS_ASSERTION_IMPL` evaluates once, honors `consteval` (`PPR_ASSUME`), else
  routes failures to `Assertion::onFailure` from a `.ppr_dbg` code segment with `source_location`;
  `PPR_ASSERT`/`PPR_VERIFY` (require/verify), boolean `PPR_ENSURE`; release lowers to `PPR_ASSUME` (+ evaluate).
- **Logging**: `PPR_DECLARE/DEFINE_LOG_CATEGORY`, `PPR_LOG`/`PPR_LOG_RAW` (emitter + message + attribute list),
  `PPR_FLUSH_LOG`.
- **Error returns** (log-and-return on `pP::hasFailed`, `[[unlikely]]`): `PPR_RETURN_ON_FAIL` (returns the failed
  value), `PPR_RETURN_ERROR_ON_FAIL` (via `make_error_code`, logs category/value/message),
  `PPR_LOG_WARNING_ON_FAIL` (log only), `PPR_RETURN_UNEXPECTED_ON_FAIL` (`std::unexpected`), and
  `RHI_RETURN_ERROR_ON_FAIL` (`pP::rhi::result(...)` shorthand).
- **RAII**: `PPR_ANONYMIZE` (line-unique name), `PPR_DEFER` (scope-exit via `pP::Deferred`).

## Flow

Included in the global fragment of every module (`module; #include "pP/Macros.h"`) and `game/main.cpp`;
expands at preprocessor time before module scanning, so all partitions share one vocabulary.

## Integration

- Consumed by: every engine module partition and the game entry point.
- Provides: the assertion/logging/error-handling vocabulary used by `Core.Assert`, `Core.Logger`, and all
  `PPR_RETURN_*` / `RHI_RETURN_*` call sites (shader, RHI, app services).
- Note: test-only macros (`PPR_UNIT_TEST`, `PPR_TEST_ASSERT`) live in
  `lib/engine/tests/include/pP/UnitTest.h`, NOT here.

## Key Files

- `Macros.h` — all engine-wide preprocessor macros (assertions, logging, attributes, error handling, RAII).
