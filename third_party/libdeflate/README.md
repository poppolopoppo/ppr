# libdeflate

Upstream: https://github.com/ebiggers/libdeflate

Version: **1.25** resolved by the vcpkg manifest at the pinned baseline; no
override and no CPM fallback. The port enables the `compression`,
`decompression`, `gzip`, and `zlib` features.

## License

MIT-style license (Copyright 2016 Eric Biggers, Copyright 2024 Google LLC).
Keep the notice with redistributed software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1078 | 4ad69099cb4374836fd27583d9991e2838cd86b6e4666ab26b2d32582c91e73a |

The preserved file is the installed vcpkg notice
(`share/libdeflate/copyright`).

## Integration scope

- PPR code never includes libdeflate. It is a Mango dependency (fast
  deflate/zlib/gzip codec used by Mango's image/container paths), linked
  through the vcpkg-resolved build.
- On the Windows `x64-windows` triplet libdeflate ships as a runtime DLL
  (`deflate.dll` in the dev configuration) staged beside PPR executables, so
  binary redistribution carries this notice.
