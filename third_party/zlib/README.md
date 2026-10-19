# zlib

Upstream: https://github.com/madler/zlib (home: https://zlib.net/)

Version: **1.3.1** resolved by the vcpkg manifest at the pinned baseline; no
override and no CPM fallback.

## License

Classic zlib license (Copyright (c) 1995-2022 Jean-loup Gailly and Mark
Adler): free use/alteration/redistribution; altered source versions must be
plainly marked; the notice must not be removed from source distributions.
Keep the notice with redistributed software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1002 | 845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243 |

The preserved file is the installed vcpkg notice (`share/zlib/copyright`).

## Integration scope

- PPR code never includes zlib. It is a Mango dependency (deflate handling in
  Mango's image/container paths), linked through the vcpkg-resolved build.
- On the Windows `x64-windows` triplet zlib ships as a runtime DLL
  (`zlibd1.dll` in the dev configuration) staged beside PPR executables, so
  binary redistribution carries zlib's notice.
- Engine-side container/compression formats are engine-defined; asset loading
  consumes Mango's codec set rather than zlib directly.
