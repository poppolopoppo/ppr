# Box2D

Upstream: https://github.com/erincatto/box2d (documentation: https://box2d.org/documentation/)

Version: **3.1.1**, enforced by the manifest `overrides` entry in
`vcpkg.json`, so the baseline cannot silently drift the physics runtime onto
a different 3.x. The engine's physics contracts assume this specific 3.x
behavior — for example, Box2D 3.1.1's opt-in requirement for non-sensor
contact visitors, recorded in `lib/engine/physics/codemap.md` and written
into `Physics.Scene.cpp`. The physics plan under `docs/plans/` (Unified
Matter Simulation v2) carries the "Box2D 3.1.1" runtime assumption. Bumping
the override is a reviewed change that revalidates contact/sensor semantics.

## License

MIT (Copyright (c) 2022 Erin Catto). Keep the notice with redistributed
software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1067 | 68a3e676d7e94093b102d5cba0d4e04af812040d6f230c3db67a6664574e43d2 |

The preserved file is the installed vcpkg notice (`share/box2d/copyright`).

## Integration scope

- `engine.physics` is the only consumer and the boundary: box2d types never
  appear past it (`EXTERNAL_SYSTEM_PRIVATE_DEPS box2d::box2d`).
  - `:scene` owns the RAII world (`Physics.Scene.cpp` includes
    `box2d/box2d.h`), bodies, sensors, contact/sensor event draining, sleep
    control, and the serial-keyed static-body chain API.
  - `:chunk_colliders` builds caller-owned terrain chains with indexed,
    diffed, budget-bounded rebuilds.
  - `:coupling` publishes sorted events and body states into caller buffers;
    the two reserved force/velocity seams return `function_not_supported`.
- On the Windows `x64-windows` triplet Box2D ships as a runtime DLL staged
  beside PPR executables, so binary redistribution carries its notice.
- Dependency direction is load-bearing: `engine.physics` links PRIVATE
  `box2d::box2d`, `game` gets its driver-owned `Scene`/`ChunkColliders`
  through PRIVATE `engine.physics`, and `engine.app` links neither. Only
  `lib/engine/tests/sim` links `engine.physics` privately for its tests.
