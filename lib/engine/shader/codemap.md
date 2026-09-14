# lib/engine/shader/

## Responsibility

`engine.shader` wraps Slang compilation into `namespace pP::shader`: the `errc`/error-category vocabulary shared
with `engine.rhi`, the `IShaderService` session-lifecycle singleton, synchronous module loading from file or
source string, and the `SharedModule` borrowed-view handle. Single module — `Shader.cppm` + `Shader.cpp`.
Row-major matrix layout is fixed at session creation for cross-API portability.

## Design

- `errc` mirrors `Slang::Result` values with `std::is_error_code_enum` + `error_category()`/`make_error_code`/
  `result()`; `SlangErrorCategory` maps invalid-arg/OOM/not-found/timeout/not-implemented/buffer-too-small to
  `std::errc` conditions. `Diagnose` is an RAII `IBlob` holder that logs diagnostics via `PPR_LOG_RAW` on scope
  exit; `diagnoseIfNeeded` no-ops on null blobs.
- `SharedModule` is a non-owning `slang::IModule*` view (session owns lifetime until `shutdown()`; callers must
  not release — `writeRef()` exists so load functions can fill it in).
- `IShaderService : IService` (`pP::`): `initialize()`, `shutdown()`, `setTargetFormat(SlangCompileTarget)`
  (pre-load only — post-load calls return `invalid_arg`; recreates the session), `getGlobalSession()`,
  `loadModuleFromFile(path, name, out)` (via `io::mapFile` + ref-counted `MappedFileBlob : IBlob`), 
  `loadModuleFromSource(name, path, source, out)` (via `loadModuleFromSourceString`). Both set
  `m_modules_loaded = true` on success. `initialize()`/`setTargetFormat()` build the session with
  `defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR`.
- Loading is synchronous, with no file watching or background compile thread;
  diagnostics flow through `Diagnose` → `PPR_LOG`.

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
