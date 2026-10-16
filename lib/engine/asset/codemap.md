# lib/engine/asset/

## Responsibility

`engine.asset` depends only on `engine.core`. It supplies stable qualified names,
versioned asset identity, immutable owned pack indices, checked kind/membership
lookup, pack-relative containment, reusable typed descriptor importing, and a
shared typed asynchronous preparation request. Composition supplies supported
textual kind identifiers. The library uses the existing core scheduler and owns
no image/simulation payload registry, separate thread pool, or GPU state.

## Boundaries

- `Asset.Pack.cppm/.cpp` (`:pack`) owns schema-1 manifests with
  `[assets.<local_key>]` and `[requires.<alias>]` tables. Dependencies name a pack
  through `pack_id`; duplicate namespaces, redundant dependency aliases,
  unsupported kinds, undeclared references and escaping paths fail explicitly.
- `PackSet::index` consumes owned manifest sources and freezes sorted lookup
  indices. `resolve` returns observers tied to the value's lifetime.
- `importDescriptor<T>` invokes core tree decoding/resolution once and returns
  the typed value, owned source bytes and owned property provenance.
  The synchronous `DescriptorReader` overload imports edited owned bytes through
  the same path after checked lookup. It does not retain the callback or fall
  back to disk when a supplied source is absent.
- Identity encodes the fixed domain/version and three length-prefixed text
  fields with FNV-1a-64. `Asset.Identity.Validation.hpp` is a private collision
  validator shared only with the forced-collision fixture.
- `Asset.Preparation.cppm/.cpp` (`:preparation`) owns the common
  `PreparationRequest<Processor>` lifecycle: bounded admission, per-item
  continuations, polling, cancellation, ordered outcomes, capture settlement,
  draining, and stamped owning results. A processor supplies metadata preflight,
  worker stages, owner-side continuation decisions, and its result type. The
  injected scheduler is non-owning and must outlive the request and accepted
  captures. Domain factories select the scheduler at their boundary.
  Worker stages return fresh `Outcome` values through scheduler-provided
  `std::future<Outcome>` objects; cumulative outcomes remain owner-held.
  Request-local capture settlement is required in addition
  to future readiness. Non-image, move-only/nonresident result fixtures live in
  `../tests/asset/Asset.Preparation.Tests.cpp`.

`engine.tests.asset` registers CPU-only `asset/packs` tests. No binary image
sources are opened by pack indexing or typed definition importing.
