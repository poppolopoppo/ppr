# {fmt}

Upstream: https://github.com/fmtlib/fmt

Version: **12.1.0** resolved by the vcpkg manifest at the pinned baseline; no
override and no CPM fallback.

## License

MIT (Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors).
Keep the notice with redistributed software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1431 | 07580f2a3b35709ce703d523f447b242f6dfec7582a8c0df102c7fa2849375f8 |

The preserved file is the installed vcpkg notice (`share/fmt/copyright`).

## Integration scope

- PPR code never includes fmt. The declared dependency exists because Mango
  formats through it (see `cmake/external/Mango.cmake`); Mango finds it in the
  resolved prefix, which is the vcpkg install in PPR's presets. No fmt type or
  include appears in a PPR module interface.
- On the Windows `x64-windows` triplet the vcpkg fmt library ships as a
  runtime DLL staged beside PPR executables, so binary redistribution carries
  fmt's notice even though no PPR source file names it.
- Engine diagnostics format with `std::format` and the `pP/Macros.h` logging
  facilities; adding a direct fmt consumer would be a new dependency decision,
  not a change to this entry.
