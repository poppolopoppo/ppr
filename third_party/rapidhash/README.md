# rapidhash

Upstream: https://github.com/Nicoshev/rapidhash

Build route: CPM source, `GIT_TAG master` — a **moving branch, not an
immutable pin**. Resolved revision consumed by the local CPM cache when this
survey was written:

- commit: `1ae7842f59f25929a338e5a76e3fa350673e48e1` (2026-08-19)

`rapidhash.cmake` also creates an INTERFACE IMPORTED target with the source
dir as SYSTEM include when CPM's sub-build did not define one, so consumer
headers resolve the same path in both cases.

## License

MIT (Copyright 2025 Nicolas De Carli). Keep the notice with redistributed
software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 1075 | bba53b7618085310a0b6cdd3774d5f7a0e4063cb3f1b90715c092559909b92f8 |

## Integration scope

- Consumed only by `engine.core` (PRIVATE dep): `Core.Hashing.cppm` includes
  `"rapidhash.h"` and exposes `hash_t` with `hashValue`/small/`trivial` hash
  entry points backed by `rapidhash`/`rapidhashMicro`/`rapidhashNano`.
- The `rapid_secret_v` constant is duplicated locally inside `engine.core`
  (`Core.Hashing.cppm` comment) so the internal hash calls avoid multiplexed
  module/TU linking issues (`hash_t` ABI and algorithm behave identically to
  upstream); worldgen's fixed-block chaining (`Sim.WorldGen.Definition.cpp`)
  is specified against this exact hash behavior, so a different rapidhash
  revision is a worldgen-compatibility consideration: recorded worlds'
  canonical chaining relies on the same block hashing.
- No other module links or includes rapidhash; hash results passing external
  boundaries are engine-owned values, not upstream types.
