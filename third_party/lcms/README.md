# lcms2

Upstream: https://github.com/mm2/Little-CMS (home: https://littlecms.com/)

Version: **2.17** resolved by the vcpkg manifest at the pinned baseline
(port name `lcms`, package `lcms2`); no override and no CPM fallback.

## License

MIT (Copyright (c) 2023 Marti Maria Saguer). Keep the notice with
redistributed software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1080 | 6dbd60437f8ef91d8de1f08ad75882547fd4931bfcc3566a0735f28db1484d31 |

The preserved file is the installed vcpkg notice (`share/lcms/copyright`).

## Integration scope

- PPR code never includes lcms2. It is an optional Mango dependency (ICC
  color space conversion in Mango's image paths), present in the manifest so
  Mango's build finds the vcpkg-installed color engine; Mango's own
  consolidated notices are in
  [Mango/THIRD-PARTY-LICENSES.txt](../Mango/THIRD-PARTY-LICENSES.txt).
- On the Windows `x64-windows` triplet lcms2 ships as a runtime DLL
  (`lcms2-2.dll` in the dev configuration) staged beside PPR executables, so
  binary redistribution carries this notice.
- Color interpretation for engine image loads is decided by Mango header
  flags (`!header.linear`, `EImageUsage::data` forcing linear) — see
  `lib/engine/image/codemap.md`; PPR does not manage ICC profiles itself.
