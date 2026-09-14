# lib/engine/shader/

## Responsibility

`engine.shader` wraps Slang compilation into `namespace pP::shader`: the `errc`/error-category vocabulary shared
with `engine.rhi`, the `IShaderService` session-lifecycle singleton, synchronous module loading from file or
source string, and the `SharedModule` borrowed-view handle. Single module — `Shader.cppm` + `Shader.cpp`.
Row-major matrix layout is fixed at session creation for cross-API portability.

## Design

- `errc` mirrors `Slang::Result` values (`ok` through `time_out`) with `std::is_error_code_enum` +
  `error_category()`/`make_error_code(Result|errc)`/`result()`; file-local `SlangErrorCategory`
  (`name() "slang"`, full `message()` switch with `std::format` unknown-result fallback,
  `default_error_condition` mapping invalid-arg→`invalid_argument`, OOM→`not_enough_memory`,
  not-found→`no_such_file_or_directory`, timeout→`timed_out`, not-implemented→`function_not_supported`,
  buffer-too-small→`result_out_of_range` via static `slang_error_condition`); `make_error_code(Result)`
  returns success on `SLANG_SUCCEEDED` else `{result, g_slang_error_category}` (`constexpr` instance).
  Slang vocabulary: `ComPtr`/`Result`, `IBlob`/`IComponentType`/`IEntryPoint`/`IGlobalSession`/`IModule`/
  `ISession` plus `using namespace Slang; using namespace slang;`. `export namespace Slang` defines the
  `PPR_RETURN_*_ON_FAIL` ADL predicate `hasFailed(Result)` (`SLANG_FAILED`, `constexpr`) and re-exports
  `pP::shader::make_error_code`. `Diagnose` is an RAII `IBlob` holder (`writeRef()` for the Slang out-param)
  that logs diagnostics via `PPR_LOG_RAW(Shader, error, …)` on scope exit; `diagnoseIfNeeded` no-ops on
  null blobs (message built from `getBufferPointer()`/`getBufferSize()` via `safe_narrowing`).
- `SharedModule` is a non-owning `slang::IModule*` view (session owns lifetime until `shutdown()`; callers must
  not release — `writeRef()` exists so load functions can fill it in).
- `IShaderService : IService` (`pP::`, `safe_ptr` singleton via `get()` → file-local `ShaderService`
  with `ComPtr<IGlobalSession> m_global_session`, `ComPtr<ISession> m_session`,
  `SlangCompileTarget m_target_format = SLANG_DXBC`, `bool m_modules_loaded`): `initialize()` is idempotent
  (both sessions set → warning + `errc::ok`; exactly one set → error + `errc::internal_fail`; else
  `createGlobalSession` → single-target session with
  `defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR`); `setTargetFormat(target)` requires prior
  `initialize()` (`uninitialized` otherwise), rejects post-load changes (`invalid_arg`), no-ops on the
  current target (`default_value_v`), else stores the target, nulls `m_session`, and recreates it with the
  same row-major single-target desc; `shutdown()` warns when `m_modules_loaded` (modules are session-owned
  and die with it), then clears the flag and nulls session before global session; `getGlobalSession()`
  exposes the global session for `engine.rhi` `DeviceDesc` wiring.
- `loadModuleFromFile(path, name, out)` maps the file via `io::mapFile` (propagates its `error_code` on
  failure), wraps the `MappedFile` in ref-counted `MappedFileBlob : IBlob` (`queryInterface` for
  `ISlangUnknown`/`ISlangBlob`, atomic-`u32` `addRef`/`release` with self-delete at zero,
  `getBufferPointer() = m_file.c_str()`, `getBufferSize() = m_file.size()`), then calls
  `m_session->loadModuleFromSource(module_name, generic_path, blob, Diagnose::writeRef())`.
  `loadModuleFromSource(name, path, source, out)` calls `m_session->loadModuleFromSourceString` with a
  `Diagnose` blob. Both are synchronous (no watch/background thread), set `m_modules_loaded = true` only
  on non-null-module success, and on null-module failure log name+path and return
  `make_error_code(errc::invalid_arg)` (Slang diagnostics already emitted via `Diagnose` → `PPR_LOG`).

## Flow

`IShaderService::get()->initialize()` (global session + row-major session) → `engine.rhi` calls
`setTargetFormat(toSlangCompileTarget_(deviceType))` before device creation → `loadModuleFromFile/Source`
compiles into the session (owned until `shutdown()`) → `shutdown()` drops session then global session.

## Integration

- Depends on: `engine.core` (public — `IService`, `safe_ptr`, `io::mapFile`, `MappedFile`, logging,
  `safe_narrowing`), `slang` (private system dep).
- Consumed by: `engine.rhi` (session handle in, target format + module loads), `engine.app` transitively.
  `rhi::errc` is an alias of `shader::errc` — one shared Slang error vocabulary.
- Build: `Shader.cppm` in `FILE_SET CXX_MODULES`, `Shader.cpp` private;
  `setup_ppr_project(engine.shader INTERNAL_PUBLIC_DEPS engine.core EXTERNAL_SYSTEM_PRIVATE_DEPS slang)`.

## Key Files

- `Shader.cppm` — `errc`, error API, Slang aliases, `Diagnose`, `SharedModule`, `IShaderService`.
- `Shader.cpp` — `SlangErrorCategory`, `MappedFileBlob`, `ShaderService`, log category.
