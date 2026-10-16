# lib/

## Responsibility

Engine source lives in `engine/`: ten production C++20 module libraries plus the shared test infrastructure and engine test executables. The game client lives separately in `../game/`.

## Navigation

- [engine/codemap.md](engine/codemap.md) records module responsibilities and dependencies.
- [engine/app/codemap.md](engine/app/codemap.md) records application lifecycle and interactive ownership.
- [engine/app/frame/codemap.md](engine/app/frame/codemap.md) records owned frame transport and the render worker.
- [../codemap.md](../codemap.md) records test scheduling and repository entry points.

`lib/CMakeLists.txt` adds `engine/`. Each subsystem's CMake file registers its module interfaces, implementation units, and dependencies. Use those registrations over old file inventories or proposed architectures.
