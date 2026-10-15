# lib/engine/asset/

## Responsibility

`engine.asset` depends only on `engine.core`. It supplies stable qualified names,
versioned asset identity, immutable owned pack indices, checked kind/membership
lookup, pack-relative containment, and reusable typed descriptor importing.
Composition supplies supported textual kind identifiers; this library has no
image/simulation payload registry, processing scheduler, or GPU state.

## Boundaries

- `Asset.Pack.cppm/.cpp` (`:pack`) owns schema-1 manifests with
  `[assets.<local_key>]` and `[requires.<alias>]` tables. Dependencies name a pack
  through `pack_id`; duplicate namespaces, redundant dependency aliases,
  unsupported kinds, undeclared references and escaping paths fail explicitly.
- `PackSet::index` consumes owned manifest sources and freezes sorted lookup
  indices. `resolve` returns observers tied to the value's lifetime.
- `importDescriptor<T>` invokes core tree decoding/resolution once and returns
  the typed value, owned source bytes and owned property provenance.
- Identity encodes the fixed domain/version and three length-prefixed text
  fields with FNV-1a-64. `Asset.Identity.Validation.hpp` is a private collision
  validator shared only with the forced-collision fixture.

`engine.tests.asset` registers CPU-only `asset/packs` tests. No binary image
sources are opened by pack indexing or typed definition importing.
