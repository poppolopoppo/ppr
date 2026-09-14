# lib/

## Responsibility

Source root for the PPR engine libraries. All engine code lives under `lib/engine/` as C++20 modules
(`engine.core`, `engine.math`, `engine.shader`, `engine.rhi`, `engine.app`); test infrastructure lives in
`lib/engine/tests/` (excluded from this codemap per skill rules).

## Design

- Single library tree: `lib/engine/` holds the five engine modules plus the test suite.
- Module convention: `.cppm` = interface (exports), `.cpp` = implementation; each library registers via its
  own `CMakeLists.txt` (`engine.core`/`math`/`shader`/`rhi`/`app`) with `setup_ppr_project` dependency edges.
- `engine.app` = slim `Application` base (`:application`) + interactive `ApplicationEditor` client subclass
  (`:application_editor`, owns window/viewport/input/player/camera/triangle/UI, implements `:service.client`);
  five service contracts (`service.client/input/player/ui/window`).
- `app.game` (`game/`) links all five engine modules; no engine code lives outside `lib/engine/`.

## Flow

`game/main.cpp` → `import engine.app` → umbrella re-exports `engine.core`, `engine.math`, `engine.shader`,
`engine.rhi` → transitive access to all partitions; concrete `Application`/`ApplicationEditor` owns lifecycle,
`run()` drives platform init → shader/RHI/renderer bootstrap → editor client setup → per-frame update/render.

## Integration

- Consumed by: `game/` (entry point), CMake build system (`cmake/`).
- See [engine/](engine/codemap.md) for the full engine module map.

## Key Files

- `engine/` — all engine module libraries (see [engine/codemap.md](engine/codemap.md)).
