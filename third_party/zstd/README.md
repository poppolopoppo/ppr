# zstd

Upstream: https://github.com/facebook/zstd (home: https://facebook.github.io/zstd/)

Version: **1.5.7** resolved by the vcpkg manifest at the pinned baseline; no
override and no CPM fallback.

## License

Dual-licensed under BSD and GPLv2 — choose BSD when redistributing. The BSD
text (Copyright (c) 2016-present, Facebook, Inc.) is inside the preserved
file; keep it with redistributed software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 20086 | 434dca949c6da7c500413aef694539fe37f867dd1a94d83d4ed1d260194e2660 |

The preserved file is the installed vcpkg notice (`share/zstd/copyright`),
which carries the dual-license statement plus both full texts.

## Integration scope

- PPR code never includes zstd. It is a Mango dependency (zstd compression
  inside Mango's image/container codecs), linked through the vcpkg-resolved
  build.
- On the Windows `x64-windows` triplet zstd ships as a runtime DLL staged
  beside PPR executables, so binary redistribution carries this notice. Mango's own
  bundled zstd notice is also present in
  [Mango/THIRD-PARTY-LICENSES.txt](../Mango/THIRD-PARTY-LICENSES.txt); when
  Mango links the vcpkg zstd instead of its bundled copy, this notice applies.
