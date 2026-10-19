# Vulkan-Headers

Upstream: https://github.com/KhronosGroup/Vulkan-Headers (Vulkan registry
headers; specification: https://docs.vulkan.org/)

Version: **1.4.328.0** (port `vulkan-headers`) at the pinned baseline.

## License

Mixed: files are Apache-2.0, MIT, or Apache-2.0 OR MIT (SPDX dual). The
preserved file is the repository's per-file summary notice; composition of a
binary that drops in Vulkan-ICD-loader territory does not remove the
per-file SPDX obligations — keep this notice alongside the referenced texts
when distributing derived content from these headers.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 458 | ac24e5ea920e4318e4d02c4086ae51f53cfb03feed06c18df1019e7ada1ec7bc |

The preserved file is the installed vcpkg notice
(`share/vulkan-headers/copyright`).

## Integration scope

- Declared in the manifest since the initial commit; **no PPR source file
  includes Vulkan today** (checked across `lib/`, `game/`, `cmake/`, and
  `include/`; the only Vulkan reference is Mango's `BUILD_VULKAN OFF`).
- The Slang-RHI build does **not** resolve this port: it always fetches its
  own pinned Vulkan-Headers (`v1.4.347`) at configure time when
  `SLANG_RHI_ENABLE_VULKAN` is on (`cmake/external/SlangRHI.cmake` +
  [Slang-RHI entry](SlangRHI/README.md)). The vcpkg entry is therefore
  currently unused by the default graph.
- Removing the unused declaration (or wiring slang-rhi to a shared
  `VulkanHeaders` config package instead of its private fetch) are both
  deliberate dependency-graph changes and need their own reviewed change;
  this entry records the status quo with the survey evidence.

Name note: the vcpkg port is `vulkan-headers`; the upstream project styles
itself Vulkan-Headers.
