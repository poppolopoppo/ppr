# toml++

Upstream: https://github.com/marzer/tomlplusplus

Version: **3.4.0** — vcpkg manifest mode resolves the port as `3.4.0#1` at the
pinned baseline (`builtin-baseline 544a4c5c297e60e4ac4a5a1810df66748d908869`);
the CPM fallback in
`cmake/external/tomlplusplus.cmake` pins the identical upstream release tag
`v3.4.0`, so the TOML behavior does not depend on which route found the
package first. No override or moving tag is involved.

## License

MIT (Copyright (c) Mark Gillard). Keep the notice with redistributed
software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1094 | 529bc3900a9571e49db285b0df432397e70b881cc3bf48de6667ae74ff4b06d8 |

The preserved file is the installed vcpkg notice
(`share/tomlplusplus/copyright` in the manifest install tree).

## Integration scope

- Consumed only by `engine.core` (EXTERNAL_SYSTEM_PRIVATE_DEPS
  `tomlplusplus::tomlplusplus`): the `:config.file` codec backs `decode`,
  source patching (`patchScalar`/`patchTree`), and `insertRecord`/`removeRecord`
  on the upstream parser (`Core.Config.File.cpp` flattens the parsed tree into
  the engine-owned `Document`; arrays/dates are rejected downstream, and the
  1 MiB document limit is enforced by engine code).
- Private dependency: upstream types never appear in the module interface —
  caller-visible documents, patches, and diagnostics are engine-owned values.
- The CPM fallback targets header-only use (`TOMLPP_BUILD_EXAMPLES/TESTS
  OFF`; unknown upstream option names are inert).
