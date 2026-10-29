# Dear ImGui

Upstream: https://github.com/ocornut/imgui — the pinned tag is on the
repository's `docking` branch

Build route: CPM source, `cmake/external/DearImGui.cmake`:

- `GIT_TAG v1.93.0-docking` — an immutable tag pointing at the docking
  branch release; the same revision supplies the vendored
  `misc/freetype/imgui_freetype.cpp` wrapper (see
  [FreeType](../FreeType/README.md)), so no second pin can drift.
- `imgui.base`: static library over `imgui.cpp`, `imgui_demo.cpp`,
  `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`; SYSTEM includes;
  `CXX_MODULE_STD OFF` (CMake 4.4 root-scope `@cmake_cxx_std` synth-target
  workaround shared with the `imgui` module target).
- The `IMGUI_ENABLE_FREETYPE` swap path compiles the wrapper TU and defines it
  PUBLIC so module BMIs and `engine.app` consumers see one consistent
  imgui.h/loader view; only the `msvc-dev-freetype` preset enables it.

## License

MIT (Copyright (c) 2014-2026 Omar Cornut). Keep the notice with redistributed
software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE.txt | 1104 | f20418b409e53c8c9f4e90917ff395554a60320d4dfbf833da89b339cad8628a |

## Integration scope

- `engine.app` links `imgui.base` (EXTERNAL_SYSTEM_PUBLIC_DEPS) and the
  [module bindings](../imgui-module/README.md) target `imgui` PUBLIC; engine
  code writes `import imgui;` and never `#include <imgui.h>`.
- `engine.app.ui` owns the ImGui integration, including the overlay shader
  embedded in `App.UI.ImGui.cpp`; `game/ui` composes the game-facing panels
  natively with ImGui calls and owns no shader.
- The bindings' access to `ImGuiContext::ErrorCallback` flows through the
  internal-partition binding (`imgui_internal.cppm` implicitly exports the
  `imgui` module).
- PPR renders the UI in gamma space; see the FreeType entry for the linear
  blending caveat when evaluating that backend.

Version bumps of ImGui change the module binding contract too — keep the
`imgui-module` version in lockstep (both currently `1.93.0`).
