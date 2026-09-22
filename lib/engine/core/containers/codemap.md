# lib/engine/core/containers/

## Responsibility
The containers partition provides PPR's core data structures: bounded trivial-type containers (`Stack<T,N>`, `RingBuffer<T,N>`), sparse and stable vectors (`SparseVector<T>`, `StableVector<T>`), associative maps and sets (`HashMap<K,V>`, `HashSet<K>`, `FlatMap`/`FlatSet`/`FlatMultiMap`/`FlatMultiSet`/`PrioritySet` aliases over `STL<A>`-backed std containers), bit manipulation (`Bitmask<T>`/`BitmaskRef<T>`, `SetBitsRange`), and view abstractions (`ArrayView`, `RelativeView`, `TransformView`) plus relative/tagged pointers (`RelPtr`, `TagPtr`), index iteration, `relocate` memcpy-or-move helpers, and the `Collector` push-back abstraction. All containers are designed for performance-critical engine use with optional allocator support, and types supporting `memcpy` (marked `relocatable<T>`) can be moved by bitwise copy.

## Design
- **Bounded trivial containers**: `Stack<T,N>` (fixed-capacity LIFO) and `RingBuffer<T,N>` (fixed-capacity FIFO) require trivially copyable/movable T and N as compile capacity; no dynamic allocation when N is known at compile time.
- **Sparse & stable vectors**: `SparseVector<T>` is a never-shrinking free-list vector keyed by `SparseKeyId` — each slot carries a `SparseVectorPayload` (skip + seed generation) so stale keys fail validation; wraps `StableVector` storage. `StableVector<T>` grows exponentially in slices without invalidating storage, with a slice-cached random-access iterator; erasures leave reusable slots.
- **Associative containers**: `HashMap<K,V>` naive robin-hood hash map with out-of-core metadata (key comparator `TEqualTo`, hasher `hash::DefaultHash`, default allocator `mem::GPA`); internal storage uses non-const keys for swap/move while iterators present `pair<const K,V>`. `HashSet<K>` is a `HashMap` with `void` value. Key-only `initializer_list`/`(list, alloc)` deduction guides target `HashMap<K, void, …>` (not `HashSet`, which is an alias and cannot be deduced). `FlatMap`/`FlatSet` (plus multi and `PrioritySet` variants) are `std::flat_*` aliases over `Array<T, AllocatorT>` — cache-friendly for small-to-medium sizes, no rehashing.
- **STL aliases**: `Array<T,A>` (`std::vector` + `mem::STL`), `Deque<T,A>` (`std::deque` + `mem::STL`), `FlatSet`/`FlatMap`/`FlatMultiSet`/`FlatMultiMap` over `Array`, and `PrioritySet<T>` (priority-keyed `FlatSet` of `PriorityPair`). All take an `AllocatorT` (default `mem::GPA`).
- **Bitmask**: `Bitmask<T>` where T is an integer word type, plus non-owning `BitmaskRef<T>` used by `BitmapTree`; provides bit set/clear/toggle/test operations with a `bit_count_v` trait.
- **SetBitsRange**: Lightweight range over a contiguous bit range within a Bitmask; used for sparse set bit operations.
- **relocate helpers**: `relocate<T>(src)` / `relocateUninitialized<T>(src, dst)` move by `memcpy` when `is_relocatable_v<T>`, otherwise by move-construct + destroy — the single choke point for storage compaction.
- **Collector**: Generic push-back sink (`Collector<Args...>` derives from `std23::function_ref<std::error_code(...)>`) with `append(first, last)` and tuple support — lets producers fill any container through one callable type. Range-`append` constraints use the unexported `details::range_const_reference_t` polyfill (equivalent const-iterating formulation from C++20/23 parts — C++26 `std::ranges::range_const_reference_t` is unavailable in C++23 mode).
- **Views**: `ArrayView<T>` (non-owning span), `RelativeView<T>` (non-owning view using relative pointers — half the size of `ArrayView`), `TransformView<T>` (lazy view with applied transform). All are non-owning, trivially copyable, and support `data()`, `size()`, `operator[]`. `void` setters (`RelPtr::setData`, `TagPtr::setData`, `ArrayView::reset`) carry no `lifetimebound` attribute (removed for Clang acceptability — the annotation is only meaningful on reference-returning accessors); index arithmetic goes through `checked_cast` under Clang `-Werror`.
- **RelPtr / TagPtr**: `RelPtr` packs a pointer into a relative offset (copyable, serializable, not movable — used within arenas); `TagPtr` packs a tag into the low bits of an aligned pointer (e.g. event index in `ISignal` subscriptions). Both are `relocatable<T>`-aware and suitable for arena-allocated object graphs.
- **IndexIterator**: compile-time or runtime index sequence generator for unrolled loops over container elements.

## Flow
- **Per-frame entity management**: `SparseVector<Entity>` tracks active entities via `SparseKeyId`; `add()` returns a key, `erase(key)` pushes the slot to the free-list with a bumped seed so stale keys are rejected. `StableVector<Entity>` provides stable storage underneath without invalidation on growth.
- **HashMap/HashSet usage**: Gameplay systems (e.g. component lookup, tag lookup) use `HashMap<Hash,Component*>` or `HashSet<Tag>` with `hash::DefaultHash` for keys. `FlatMap` for small fixed-size lookups (e.g. max 8 lights).
- **Stack/RingBuffer**: Audio buffer queues, command recording rings, temporary scratch buffers. `Stack<Command, 128>` for per-thread command buffers; `RingBuffer<Event, 4096>` for lock-free MPSC between threads.
- **Bitmask usage**: Component bitmask archetype filtering; page-pool allocation tracking via `BitmapTree` over `BitmaskRef` words; `SetBitsRange` for iterating set bits.
- **RelPtr in arenas**: Scene graph nodes, asset references, component pointers stored as `RelPtr<Node>` within a `mem::Arena`; offset computed from arena base.
- **Callback subscriber store**: `BroadcastCallback` keeps its `function_ref` subscribers in a `SparseVectorInplace`, keyed by `SparseKeyId` handles; `safe_ptr` referencer registry (when enabled) is also `SparseVector`-backed.

## Integration
- **memory partition**: Every allocator-parameterized container defaults to `mem::GPA`; `Array`/`Deque`/flat aliases route through `mem::STL<A>`. `BitmapTree` consumes `BitmaskRef` words; `safe_object` tracking keys off `SparseKeyId`.
- **function partition**: `Collector` and callback subscriber types are `std23::function_ref` aliases — containers expose callable sinks without owning them.
- **engine.math**: `ArrayView<float>` / `ArrayView<float2/3/4>` passed to math functions; `VectorCast` between views and math types.
- **engine.rhi**: Resource heaps and descriptor heaps use `SparseVector<Resource>` for loose resource tracking; `Bitmask<ResourceFlag, 64>` for feature flag bits.
- **engine.app**: ECS-like component arrays use `StableVector<Component>` for stable handles; `HashMap<TypeID,ComponentType>` for component type registry; `Stack<System, N>` for system pipeline ordering.
- **engine.tests.core**: Tests `Stack`, `RingBuffer`, `SparseVector`, `StableVector`, `HashMap`, `HashSet`, `FlatMap`, `Bitmask`, `ArrayView`, `RelPtr`, `TagPtr`, and `IndexIterator` with unit tests for insert/erase/lookup, stability, and view semantics.

## Key Files
- `Core.Containers.cppm` — umbrella partition: `relocatable<T>` trait + `relocate`/`relocateUninitialized`, `Collector`, `IndexIterator`, `SetBitsRange`, `Bitmask<T>`/`BitmaskRef<T>`, `RelPtr`, `TagPtr`, `ArrayView`, `RelativeView`, `TransformView`, `Stack<T,N>`, `RingBuffer<T,N>`
- `Core.Containers.HashMap.cppm` / `.cpp` — `pP::HashMap<K,V>` robin-hood hash map, `HashSet<K>`
- `Core.Containers.SparseVector.cppm` — `pP::SparseVector<T>` free-list vector over `StableVector`, `SparseKeyId` keys, `SparseVectorPayload` generations
- `Core.Containers.StableVector.cppm` / `.cpp` — `pP::StableVector<T>` slice-grown storage with stable references
- `Core.Containers.STL.cppm` — `pP::Array`/`Deque` plus `FlatSet`/`FlatMap`/`FlatMultiSet`/`FlatMultiMap`/`PrioritySet` aliases over `mem::STL`
