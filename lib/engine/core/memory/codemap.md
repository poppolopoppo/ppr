# lib/engine/core/memory/

## Responsibility
The memory partition of `engine.core` defines the entire allocator hierarchy and page-based memory management used throughout the PPR engine. It provides concepts (`TAllocator`, `TOwningAllocator`, `TResizableAllocator`, `TBlockAllocator`, `TArenaAllocator`, `TSlabAllocator`), concrete allocator implementations (`mem::GPA`, `mem::OS`, `mem::PagePool`, `mem::HugePage`, `mem::SmallPage`), and composite allocators (`InSitu` + `InSituFallback`/`InSituThreshold` aliases, `Fallback`, `Threshold`, `Pooling`, `LocalCache`, `HintedPooling`, `Static`) that compose smaller allocators into larger, more flexible ones. It also defines `Allocation<T,A>` RAII handle, `Allocator<A>` type erasure, `PMR` polymorphic dispatch, and `STL<A>` std adapter. `safe_object`/`safe_ptr` lifetime tracking lives here (`memory.pointer`, namespace `pP`), keyed by `SparseKeyId` from containers. Poison/ASAN annotations are applied in debug/developer builds.

## Design
- **Concepts tier**: `TAllocator` (minimal interface: `allocateRaw()`/`deallocateRaw()` returning `std::allocation_result`), `TOwningAllocator` (adds `owns(ptr, size)`), `TResizableAllocator` (adds `resizeRaw()`), `TBlockAllocator` (exposes `block_size_v`), `TArenaAllocator` (sequential bump allocation with `watermark()`/`restore(mark)`/`reset()`), `TSlabAllocator` (adds `data()`). `mem::overlap()` helpers test pointer/range containment for ownership checks.
- **AllocatorTraits / Inplace / Static**: `AllocatorTraits<Slab>` is the CRTP-style base for slab types; `Inplace<A>` derives from stateless allocators so they can be stored by value, with `use_inplace_v` / `unwrap_inplace_t` / `allocator_ref_t` / `AllocatorForceRef` controlling value-vs-reference semantics. `Static<GetAllocatorF>` exposes a global allocator instance through a function accessor without owning it.
- **OS page allocator**: `mem::OS` wraps platform page allocation (`VirtualAlloc`/`mmap`) via `hal::pageAlloc`, `pageFree`, `pageCommit`, `pageDecommit`, `pageProtect`, `pageOfferToOS`, `pageReclaimFromOS`. Selected via `PPR_HAL_PLATFORM` (windows/linux/darwin/generic).
- **PagePool**: `BitmapTree` (O(1) bit alloc/dealloc with high cache locality, `BuildInfos` sizing) backs `PagePool`, which hands out fixed-size pages in full/partial bundles with bulk commit/decommit.
- **HugePage/SmallPage**: Concrete page pools over `PagePool` (HugePage: 2 MiB blocks; SmallPage: 32/64 KiB blocks). Each exposes a process-wide pool plus a `LocalHint`/`HintedPooling` global and a TLS `LocalCache` block cache for fast thread-local reuse.
- **Arena/ScopedArena**: `mem::Slab` (non-owning fixed chunk) → `mem::InSituSlab` (inline storage) → `mem::Arena<PageT>` (persistent O(1) bump allocation with `watermark()`/`restore(mark)`, `extern template` for HugePage/SmallPage). `ScopedArena` is RAII — restores the watermark on destruction.
- **ScratchPad**: TLS-local transient scratch space (`ScopedArena<Arena<SmallPage>>` with a debug variant); automatically cleared on thread exit.
- **Composite allocators**: `InSitu<T,N>` (inline storage for small types) with `InSituFallback`/`InSituThreshold` aliases, `Fallback<A,B>` (try A then B), `Threshold<N,A,B>` (small→A, large→B), `Pooling<N,A>` (pool from A), `LocalCache<N,A,C>` (TLS cache over pool), `HintedPooling` (hint-based pool selection).
- **Allocation<T,A>**: RAII handle owning `T` allocated via `A`; custom deleter ensures proper deallocation. Debug mode tracks allocation size and poison boundaries.
- **Allocator<A>**: Value/reference wrapper (`allocator_ref_t`) that satisfies `TAllocator` given a concrete `A`; `AllocatorForceRef` forces reference semantics. Used by PMR and STL adapters.
- **PMR (Polymorphic Memory Resource)**: `mem::PMR` wraps any `TAllocator` with vtable dispatch (`allocateRaw`/`deallocateRaw`/`resizeRaw`); stateless allocators convert implicitly. Used where a single allocator object must be passed around.
- **STL<A>**: `std::vector`, `std::deque`, etc. adapter that uses `Allocator<A>` for all allocations (`propagate_on_container_move_assignment` follows statelessness). Zero-overhead in release; debug mode poisons freed memory.
- **safe_object / safe_ptr** (`namespace pP`, in `memory.pointer`): debug-mode lifetime checker — `safe_object` tracks referencers by `SparseKeyId` (`incSafeRef`/`decSafeRef`, move/copy transfer or forbid observation), `safe_ptr` asserts no copy outlives the object; release mode is a zero-overhead raw pointer. Enabled via `PPR_ENABLE_SAFE_OBJECT_TRACKING` (SparseVector-backed registry).
- **Poison annotations**: `poisonReserved` (fill freed memory with 0xAA), `unpoisonUninitialized` (clear on re-use), `poisonDestroyed` (fill on destroy), `annotateContiguousContainer` (debug boundary markers, incl. range overloads). In ASAN builds, maps to `__asan_*` calls; in debug builds, uses pattern fills; no-op in release.
- **GPA (General Purpose Allocator)**: Wraps `operator new`/`operator delete` (nothrow variants); the default allocator for engine containers and `opaque::Unique`.

## Flow
- **Application startup**: `mem::GPA` is a stateless wrapper over the global `operator new`/`operator delete` — the default allocator for engine containers, not a global replacement.
- **Per-frame / transient allocation**: Code uses `mem::ScopedArena` or `mem::ScratchPad` for short-lived data; `watermark()` saves position, `restore(mark)` discards transient data in O(1).
- **Persistent allocation**: `mem::Arena` with `watermark()`/`restore(mark)` pattern for long-lived data (e.g. asset data, scene graph nodes). Or `mem::HugePage`/`mem::SmallPage` pools (via `HintedPooling` global + TLS `LocalCache`) for fixed-size object pools.
- **Container allocation**: `HashMap`/`SparseVector`/`StableVector` take an `AllocatorT` template parameter (default `mem::GPA`); `Stack`/`RingBuffer` are fixed-capacity with no allocator; `Array`/`Deque`/`FlatMap`/`FlatSet` are `STL<A>`-backed std aliases. Debug builds poison freed memory.
- **Lifetime-checked references**: `safe_object`-derived types hand out `safe_ptr` copies tracked by `SparseKeyId`; destruction with outstanding references asserts in debug. `CallbackSink` forwards `safe_ptr` args so callbacks observe liveness.
- **STL interop**: `std::vector<float, mem::STL<mem::GPA>>` or `std::pmr::vector<float>` with a PMR resource. In debug, poison on deallocation.

## Integration
- **engine.math**: Uses `mem::GPA` for temporary math scratch buffers; `mem::ScratchPad` in thread-local storage for per-thread math work.
- **engine.rhi**: GPU resource uploads go through `mem::GPA`; upload heaps and command list buffers are arena-allocated per-frame.
- **engine.shader**: Compiled shader bytecode and hot-reload data use `mem::Arena` for persistent storage; scratch buffers use `mem::ScratchPad`.
- **engine.app**: `Application` constructor creates the global GPA; viewport-specific allocators may be derived from it via `Fallback` or `Threshold`.
- **function partition**: `BroadcastCallback`/`CallbackSink` default to `mem::GPA` for their `SparseVector` subscriber store; `safe_ptr` forwarding protects callback arguments.
- **engine.tests.core**: Tests memory allocation, arena checkpoint/restore, poison behavior, GPA round-trip, PMR interop, and slab allocator bucket correctness.

## Key Files
- `Core.Memory.cppm` — umbrella; `mem::GPA` (operator new), `mem::OS` (page alloc), `mem::PMR` (vtable dispatch), `mem::HugePage` (2 MiB blocks, hinted global + TLS cache), `mem::SmallPage` (32/64 KiB blocks, hinted global + TLS cache)
- `Core.Memory.Allocator.cppm` — `overlap()` helpers; concepts (`TAllocator`, `TOwningAllocator`, `TResizableAllocator`, `TBlockAllocator`, `TArenaAllocator`, `TSlabAllocator`); `Allocation<T,A>`, `AllocatorTraits`, `Allocator<A>` wrapper, `Inplace`/`use_inplace`/`unwrap_inplace`/`allocator_ref`/`AllocatorForceRef`; composites (`InSitu`, `InSituFallback`/`InSituThreshold` aliases, `Fallback`, `Threshold`, `Pooling`, `LocalCache`, `HintedPooling`, `Static`, `Accessor`); `STL<A>` adapter
- `Core.Memory.Arena.cppm` — `mem::Slab`, `mem::InSituSlab`, `mem::Arena` (`extern template` HugePage/SmallPage), `mem::ScopedArena`, `mem::ScratchPad` (TLS arena)
- `Core.Memory.PagePool.cppm` — `mem::BitmapTree` bit-tree plus `mem::PagePool` bundle-based page pool
- `Core.Memory.Pointer.cppm` — `safe_object` / `safe_ptr` (namespace `pP`, not `pP::mem`), `SparseKeyId`-keyed referencer tracking
- `Core.Memory.Poison.cppm` — `poisonReserved`, `unpoisonUninitialized`, `poisonDestroyed`, `annotateContiguousContainer`
