# lib/engine/core/memory/

## Responsibility
The memory partition of `engine.core` defines the entire allocator hierarchy and page-based memory management used throughout the PPR engine. It provides concepts (`TAllocator`, `TOwningAllocator`, `TBlockAllocator`, `TArenaAllocator`, `TSlabAllocator`), concrete allocator implementations (`mem::GPA`, `mem::OS`, `mem::PagePool`, `mem::HugePage`, `mem::SmallPage`), and composite allocators (`InSitu`, `Fallback`, `Threshold`, `Pooling`, `LocalCache`, `HintedPooling`) that compose smaller allocators into larger, more flexible ones. It also defines `Allocation<T,A>` RAII handle, `Allocator<A>` type erasure, `PMR` polymorphic dispatch, and `STL<A>` std adapter. Poison/ASAN annotations are applied in debug/developer builds.

## Design
- **Concepts tier**: `TAllocator` (minimal interface: `allocateRaw()`/`deallocateRaw()` returning `std::allocation_result`), `TOwningAllocator` (adds `owns(ptr, size)`), `TResizableAllocator` (adds `resizeRaw()`), `TBlockAllocator` (exposes `block_size_v`), `TArenaAllocator` (sequential bump allocation with `watermark()`/`restore(mark)`/`reset()`), `TSlabAllocator` (adds `data()`).
- **OS page allocator**: `mem::OS` wraps platform page allocation (`VirtualAlloc`/`mmap`) via `hal::pageAlloc`, `pageFree`, `pageCommit`, `pageDecommit`, `pageProtect`, `pageOfferToOS`, `pageReclaimFromOS`. Selected via `PPR_HAL_PLATFORM` (windows/linux/darwin/generic).
- **PagePool**: Bitmap-tree managed pool of fixed-size pages (HugePage: 2 MiB, SmallPage: 32 KiB / 64 KiB). Tracks free/used bits per page; supports bulk commit/decommit.
- **HugePage/SmallPage**: Concrete page pools. HugePage (2 MiB) for large allocations; SmallPage (32/64 KiB) for many small allocations. Both use a bitmap tree for O(1) bit tracking.
- **Arena/ScopedArena**: `mem::Arena` provides persistent O(1) bump allocation with `watermark()`/`restore(mark)` for scope-bound workloads. `ScopedArena` is RAII — restores on destruction.
- **ScratchPad**: TLS-local `mem::Arena<mem::SmallPage>` transient scratch space per thread; automatically cleared on thread exit.
- **Composite allocators**: `InSitu<T,N>` (inline storage for small types), `Fallback<A,B>` (try A then B), `Threshold<N,A,B>` (small→A, large→B), `Pooling<N,A>` (pool from A), `LocalCache<N,A,C>` (TLS cache over pool), `HintedPooling` (hint-based pool selection).
- **Allocation<T,A>**: RAII handle owning `T` allocated via `A`; custom deleter ensures proper deallocation. Debug mode tracks allocation size and poison boundaries.
- **Allocator<A>**: Value/reference wrapper (`allocator_ref_t`) that satisfies `TAllocator` given a concrete `A`; `AllocatorForceRef` forces reference semantics. Used by PMR and STL adapters.
- **PMR (Polymorphic Memory Resource)**: `mem::PMR` wraps any `TAllocator` with vtable dispatch (`allocateRaw`/`deallocateRaw`/`resizeRaw`); used where a single allocator object must be passed around (e.g. `BroadcastEvent` subscriptions).
- **STL<A>**: `std::vector`, `std::list`, etc. adapter that uses `Allocator<A>` for all allocations. Zero-overhead in release; debug mode poisons freed memory.
- **Poison annotations**: `poisonReserved` (fill freed memory with 0xAA), `unpoisonUninitialized` (clear on re-use), `poisonDestroyed` (fill on destroy), `annotateContiguousContainer` (debug boundary markers). In ASAN builds, maps to `__asan_*` calls; in debug builds, uses pattern fills; no-op in release.
- **GPA (General Purpose Allocator)**: Wraps `operator new`/`operator delete` (nothrow variants); the default allocator for engine containers and `opaque::Unique`.

## Flow
- **Application startup**: `mem::GPA` is a stateless wrapper over the global `operator new`/`operator delete` — the default allocator for engine containers, not a global replacement.
- **Per-frame / transient allocation**: Code uses `mem::ScopedArena` or `mem::ScratchPad` for short-lived data; `watermark()` saves position, `restore(mark)` discards transient data in O(1).
- **Persistent allocation**: `mem::Arena` with `watermark()`/`restore(mark)` pattern for long-lived data (e.g. asset data, scene graph nodes). Or `mem::HugePage`/`mem::SmallPage` pools for fixed-size object pools.
- **Container allocation**: `HashMap`/`SparseVector`/`StableVector` take an `AllocatorT` template parameter (default `mem::GPA`); `Stack`/`RingBuffer` are fixed-capacity with no allocator; `FlatMap` is a `std::flat_map` alias over `Array<T, AllocatorT>`. Debug builds poison freed memory.
- **STL interop**: `std::vector<float, mem::STL<mem::GPA>>` or `std::pmr::vector<float>` with a PMR resource. In debug, poison on deallocation.

## Integration
- **engine.math**: Uses `mem::GPA` for temporary math scratch buffers; `mem::ScratchPad` in thread-local storage for per-thread math work.
- **engine.rhi**: GPU resource uploads go through `mem::GPA`; upload heaps and command list buffers are arena-allocated per-frame.
- **engine.shader**: Compiled shader bytecode and hot-reload data use `mem::Arena` for persistent storage; scratch buffers use `mem::ScratchPad`.
- **engine.app**: `Application` constructor creates the global GPA; viewport-specific allocators may be derived from it via `Fallback` or `Threshold`.
- **EngineCoreTests**: Tests memory allocation, arena checkpoint/restore, poison behavior, GPA round-trip, PMR interop, and slab allocator bucket correctness.

## Key Files
- `Core.Memory.cppm` — umbrella; `mem::GPA` (operator new), `mem::OS` (page alloc), `mem::PMR` (vtable dispatch), `mem::HugePage` (2 MiB blocks), `mem::SmallPage` (32/64 KiB blocks)
- `Core.Memory.Allocator.cppm` — concepts (`TAllocator`, `TOwningAllocator`, `TResizableAllocator`, `TBlockAllocator`, `TArenaAllocator`, `TSlabAllocator`); `Allocation<T,A>`, `AllocatorTraits`, `Allocator<A>` wrapper; composites (`InSitu`, `Fallback`, `Threshold`, `Pooling`, `LocalCache`, `HintedPooling`, `Static`); `STL<A>` adapter, `Inplace`
- `Core.Memory.Arena.cppm` — `mem::Slab`, `mem::InSituSlab`, `mem::Arena`, `mem::ScopedArena`, `mem::ScratchPad` (TLS `Arena<SmallPage>`)
- `Core.Memory.PagePool.cppm` — `mem::PagePool` bitmap-tree page pool
- `Core.Memory.Pointer.cppm` — `safe_ptr` (namespace `pP`, not `pP::mem`)
- `Core.Memory.Poison.cppm` — `poisonReserved`, `unpoisonUninitialized`, `poisonDestroyed`, `annotateContiguousContainer`