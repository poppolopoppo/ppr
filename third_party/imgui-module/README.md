# imgui-module

Upstream: https://github.com/stripe2933/imgui-module

Generated C++20 module bindings for Dear ImGui. Build route: the two binding
files at the `v1.93.0` tag (`generated/imgui.cppm`,
`generated/imgui_internal.cppm`) are downloaded once by CMake
(`file(DOWNLOAD)` with `TLS_VERIFY ON`, fatal on failure) into
`out/build/imgui_module_bindings` — a directory shared by every CMake preset
(`cmake/external/DearImGui.cmake`), so the filesystem holds a single physical
`export module imgui;` declaration. Multiple per-preset copies make CLion's
C++20 module indexer report "Module 'imgui' is ambiguous". Downloading only
the generated module files (not configuring upstream's CMake) keeps example
backends out.

- The bindings are the "Combined Module" form that supports both `main` and
  `docking` branches (`IMGUI_HAS_DOCK`-guarded docking symbols), matching the
  docking-pinned `imgui.base` built in the DearImGui entry.
- `imgui_internal.cppm` implicitly exports `imgui` and provides the full
  `ImGuiContext` definition the engine needs (e.g. `ErrorCallback`).
- Upstream generator quirk: the generated `imgui.cppm` has an extra `;` after
  a member body that clang promotes under `-Werror`
  (`-Wc++98-compat-extra-semi`); PPR silences exactly that warning via a
  PUBLIC compile option on the `imgui` target so it reaches BMI recompiles.
- PPR does not use the freetype or backend bindings.

## License

MIT (Copyright (c) 2025 LEE KYOUNGHEON). Keep the notice with redistributed
software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE.txt | 1071 | 0c2066ec7d94f0eff06b06c913f26e80902c9ecc7430cb7fd7c1382cd10af438 |

Hash taken from the upstream raw file at the pinned tag (`v1.93.0`,
`LICENSE.txt`). The binding generator also derives from
[cimgui](https://github.com/cimgui/cimgui)/Dear ImGui (MIT); see
[Dear ImGui](../DearImGui/README.md) for the consumed upstream notice.

Bump ImGui and these bindings together; the tag must track the pinned ImGui
tree: `v1.93.0-docking` pairs with bindings `v1.93.0`.
