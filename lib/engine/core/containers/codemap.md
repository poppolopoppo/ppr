# lib/engine/core/containers/

## Responsibility
The containers partition provides PPR's core data structures: bounded trivial-type containers (`Stack<T,N>`, `RingBuffer<T,N>`), sparse and stable vectors (`SparseVector<T>`, `StableVector<T>`), associative maps and sets (`HashMap<K,V>`, `HashSet<K>`, `FlatMap<K,V>`), bit manipulation (`Bitmask<T,N>`, `SetBitsRange`), and view abstractions (`ArrayView`, `RelativeView`, `TransformView`) plus relative/tagged pointers (`RelPtr`, `TagPtr`) and index iteration. All containers are designed for performance-critical engine use with optional allocator support, and types supporting `memcpy` (marked `relocatable<T>`) can be used as element types.

## Design
- **Bounded trivial containers**: `Stack<T,N>` (fixed-capacity LIFO) and `RingBuffer<T,N>` (fixed-capacity FIFO) require trivially copyable/movable T and N as compile capacity; no dynamic allocation when N is known at compile time.
- **Sparse & stable vectors**: `SparseVector<T>` maps logical indices to physical slots with a sparse lookup table; supports O(1) insert/erase/lookup without invalidating other entries. `StableVector<T>` maintains stable references/pointers across insertions/erasures via an index-to-generation mapping; erasures leave a tombstone that re-uses slots.
- **Associative containers**: `HashMap<K,V>` robin-hood hash map with out-of-core metadata (default allocator `mem::GPA`); `HashSet<K>` as HashMap with void values. `FlatMap<K,V>` is an alias of `std::flat_map` (sorted vector with binary search) — cache-friendly for small-to-medium sizes, no rehashing.
- **Bitmask**: `Bitmask<T,N>` where T is an integer type and N is the bit count; provides bit set/clear/toggle/test operations; static_assert(N <= sizeof(T)*8).
- **SetBitsRange**: Lightweight range over a contiguous bit range within a Bitmask; used for sparse set bit operations.
- **Views**: `ArrayView<T>` (non-owning span), `RelativeView<T>` (non-owning view using relative pointers — half the size of `ArrayView`), `TransformView<T>` (lazy view with applied transform). All are non-owning, trivially copyable, and support `data()`, `size()`, `operator[]`.
- **RelPtr / TagPtr**: `RelPtr` packs a pointer into a relative offset (copyable, serializable, not movable — used within arenas); `TagPtr` packs a tag into the low bits of an aligned pointer (e.g. event index in `ISignal` subscriptions). Both are `relocatable<T>`-aware and suitable for arena-allocated object graphs.
- **IndexIterator**: compile-time or runtime index sequence generator for unrolled loops over container elements.

## Flow
- **Per-frame entity management**: `SparseVector<Entity>` tracks active entities; `StableVector<Entity>` provides stable handles. New entities get a fresh index + generation; erased entities mark slot as free with incremented generation.
- **HashMap/HashSet usage**: Gameplay systems (e.g. component lookup, tag lookup) use `HashMap<Hash,Component*>` or `HashSet<Tag>` with `pP::hashValue()` for keys. `FlatMap` for small fixed-size lookups (e.g. max 8 lights).
- **Stack/RingBuffer**: Audio buffer queues, command recording rings, temporary scratch buffers. `Stack<Command, 128>` for per-thread command buffers; `RingBuffer<Event, 4096>` for lock-free MPSC between threads.
- **Bitmask usage**: Component bitmask archetype filtering; `Bitmask<ComponentID, 32>` for archetype bitmask checks; `SetBitsRange` for iterating set bits.
- **RelPtr in arenas**: Scene graph nodes, asset references, component pointers stored as `RelPtr<Node>` within a `mem::Arena`; offset computed from arena base.

## Integration
- **engine.math**: `ArrayView<float>` / `ArrayView<float2/3/4>` passed to math functions; `VectorCast` between views and math types.
- **engine.rhi**: Resource heaps and descriptor heaps use `SparseVector<Resource>` for loose resource tracking; `Bitmask<ResourceFlag, 64>` for feature flag bits.
- **engine.app**: ECS-like component arrays use `StableVector<Component>` for stable handles; `HashMap<TypeID,ComponentType>` for component type registry; `Stack<System, N>` for system pipeline ordering.
- **EngineCoreTests**: Tests `Stack`, `RingBuffer`, `SparseVector`, `StableVector`, `HashMap`, `HashSet`, `FlatMap`, `Bitmask`, `ArrayView`, `RelPtr`, `TagPtr`, and `IndexIterator` with unit tests for insert/erase/lookup, stability, and view semantics.

## Key Files
- `Core.Containers.cppm` — umbrella partition: `relocatable<T>` trait, `Collector`, `IndexIterator`, `SetBitsRange`, `Bitmask<T,N>`, `RelPtr`, `TagPtr`, `ArrayView`, `RelativeView`, `TransformView`, `Stack<T,N>`, `RingBuffer<T,N>`
- `Core.Containers.HashMap.cppm` / `.cpp` — `pP::HashMap<K,V>` robin-hood hash map, `HashSet<K>`
- `Core.Containers.SparseVector.cppm` — `pP::SparseVector<T>` sparse logical-to-physical mapping
- `Core.Containers.StableVector.cppm` / `.cpp` — `pP::StableVector<T>` stable references across mutations
- `Core.Containers.STL.cppm` — `pP::FlatMap<K,V>` alias of `std::flat_map`