# include/pP/

## Responsibility
Holds the engine's single public header `Macros.h` — the only non-module header in the codebase. It defines the cross-cutting preprocessor macros used throughout every module: debug-build detection, assertion/verification family, logging categories, RAII `PPR_DEFER`, compiler-attribute portability shaders, and error-propagation helpers.

## Design
- **Build-mode detection**: `PPR_ENABLE_DEBUG` derived from `_DEBUG`/`NDEBUG`; `PPR_ENABLE_MEMORY_POISONING` and `PPR_ENABLE_SAFE_OBJECT_TRACKING` are enabled when ASAN (`PPR_ENABLE_SANITIZER_ADDRESS`) or debug builds are active.
- **Pointer-size detection**: `PPR_64BIT`/`PPR_32BIT` with `PPR_32BIT_OR_64BIT(...)` selector.
- **Compiler-attribute portability**: `PPR_FORCE_INLINE`, `PPR_NO_INLINE`, `PPR_FLATTEN`, `PPR_EMPTY_BASES`, `PPR_LIFETIME_BOUND`, `PPR_ASSUME`, `PPR_OFFSETOF`, warning-push/pop pragmas — each defined per MSVC / Clang / GCC, with a no-op fallback.
- **Assertions**: `PPR_ASSERT`/`PPR_VERIFY`/`PPR_ENSURE` compile to `PPR_ASSUME` in release and to `Assertion::onFailure` with `std::source_location` in debug.
- **Logging**: `PPR_DEFINE_LOG_CATEGORY` / `PPR_DECLARE_LOG_CATEGORY` / `PPR_LOG` / `PPR_LOG_RAW` / `PPR_FLUSH_LOG`.
- **Error propagation**: `PPR_RETURN_ON_FAIL`, `PPR_RETURN_ERROR_ON_FAIL`, `PPR_RETURN_UNEXPECTED_ON_FAIL`, `RHI_RETURN_ERROR_ON_FAIL` — log-and-return on failure.
- **RAII**: `PPR_DEFER` (scope-exit via `pP::Deferred`), `PPR_ANONYMIZE` (line-unique identifier), `PPR_LITERAL_FOR` (char-type literal selector).

## Flow
Included as the very first line of every module (`module; #include "pP/Macros.h"`) and of `game/main.cpp`. Macros are expanded at preprocessor time before module scanning, so they are available to all partitions uniformly.

## Integration
- Consumed by: every engine module partition and the game entry point.
- Provides: the assertion/logging/error-handling vocabulary used by `Core.Logger.cppm`, `Core.Assertion`, and all `PPR_RETURN_*_ON_FAIL` call sites.
- Note: test-only macros (`PPR_UNIT_TEST`, `PPR_TEST_ASSERT`) live in `lib/engine/tests/include/pP/UnitTest.h`, NOT here.

## Key Files
- `Macros.h` — all engine-wide preprocessor macros (assertions, logging, attributes, error handling, RAII).
