---
name: memory-allocator
description: >
  Complete guide to the PPR memory allocator hierarchy. Use this skill when
  you need to allocate memory in engine code — selecting the right allocator,
  composing allocators with Fallback/Threshold/InSitu, using Arena allocators
  with ScopedArena or ScratchPad for bulk transient work, building lock-free
  pools with Pooling or LocalCache, wrapping allocators for std:: containers
  via STL<>, applying poison/ASAN annotations, or using AllocatorTraits typed
  helpers and the Allocation RAII handle.
---

# Memory Allocator Guide

## 1. Allocator Hierarchy Overview

All allocators live in `namespace pP::mem` and are modeled as standalone classes
satisfying C++20 concepts. The hierarchy has five tiers:

| Tier | Allocator | Source Module | Key Characteristics |
|------|-----------|---------------|---------------------|
| **General** | `GPA` | `Core.Memory.cppm` | Stateless, thin wrapper over `operator new`/`delete`. |
| **OS** | `OS` | `Core.Memory.cppm` | Stateless, backed by `hal::pageAlloc`/`hal::pageFree`. |
| **Polymorphic** | `PMR` | `Core.Memory.cppm` | Type-erased vtable dispatch over any `TAllocator`. |
| **Page Pool** | `HugePage`, `SmallPage` | `Core.Memory.cppm` | 2 MiB / 32-64 KiB fixed-block pools backed by `PagePool` + `BitmapTree`. |
| **Arena** | `Slab`, `InSituSlab`, `Arena<>`, `ScopedArena`, `ScratchPad` | `Core.Memory.Arena.cppm` | LIFO bump allocators; watermark/restore pattern. |
| **Composite** | `Fallback`, `Threshold`, `InSitu`, `InSituFallback`, `InSituThreshold`, `LocalCache`, `Pooling`, `HintedPooling`, `Static`, `Allocator<>` | `Core.Memory.Allocator.cppm` | Building blocks for combining allocators. |
| **Adapter** | `AllocatorTraits`, `Allocation`, `STL<>`, `PMR` | `Core.Memory.Allocator.cppm` | Typed wrappers, RAII handles, std:: container adapters. |

---

## 2. Allocator Selection Guide

Choose the right base allocator according to these scenarios:

### GPA — small, general-purpose allocations
- **When**: Single objects, small arrays, temporary buffers, anything that would
  use `malloc`/`new` in regular C++.
- **Characteristics**: Stateless; delegates to global `operator new`/`delete`.
  Supports over-aligned allocations (> `max_align_v`) via the aligned overload.
- **Cost**: General-purpose heap — moderate speed, fragmentation possible.
- **Code**: `mem::GPA::allocateRaw(bytes, alignment)`.

### OS — large virtual memory allocations
- **When**: Multi-megabyte buffers, scratch space for file I/O, large transient
  data where page-level granularity (typically 4 KiB) is acceptable.
- **Characteristics**: Stateless; calls `hal::pageAlloc`/`hal::pageFree`.
  Returns whole pages from the OS.
- **Cost**: Expensive — syscall on every allocation. Only for large/rare use.
- **Code**: `mem::OS::allocateRaw(bytes, alignment)`.

### HugePage — pooled 2 MiB pages for backend use
- **When**: Backend for arenas, pools, and large transient slabs. 2 MiB blocks
  reserved from OS in a 16 GiB virtual reservation.
- **Characteristics**: Each thread has a TLS `LocalCache` (MRU-2) backed by a
  global `PagePool` + `BitmapTree` for O(1) allocation tracking. Stateless.
- **Cost**: Very fast after warm-up; TLS cache avoids atomic ops on hot path.
- **Code**: `mem::HugePage::allocateRaw(bytes, alignment)`.

### SmallPage — pooled small pages for transient work
- **When**: Temporary scratch buffers and transient allocations in hot code.
  32 KiB (64-bit) or 64 KiB (32-bit) blocks.
- **Characteristics**: TLS `LocalCache` (MRU-2) backed by `HintedPooling<SmallPage, HugePage, ...>`.
  Uses a thread-local `LocalHint::value` for pool index hints. Stateless.
- **Cost**: Very fast hot path; pool-level lock only on rare expansion/release.
- **Code**: `mem::SmallPage::allocateRaw(bytes, alignment)`.

### Arena<> — persistent batch allocations (LIFO)
- **When**: Long-lived data with infrequent resets; frame allocators; systems
  that allocate many objects then free them all at once (e.g., frame rendering,
  serialization, scene loading).
- **Template parameter**: `Arena<AllocatorT = HugePage>` — defaults to `HugePage`.
- **Characteristics**: Growable linked list of fixed-size slabs. LIFO only.
  `watermark()`/`restore()` for O(1) rollback — no destructors called.
  Supports `resizeRaw`/`deallocateRaw` only for the **most recent** allocation.
- **Cost**: Near-zero per allocation (bump pointer). Slab overhead: one pointer
  + two `u32` per slab.
- **Code**: `mem::Arena arena{initial_capacity};`

### ScratchPad — thread-local transient arena
- **When**: Short-lived temporary work (string formatting, serialization,
  temporary transformations) within a single function or scope.
- **Characteristics**: TLS `Arena<SmallPage>` (auto-created on first access).
  Use `ScratchPad::open()` to get a `ScopedArena` that auto-restores.
  All `ScratchPad` methods are static and forward to the TLS arena.
- **Cost**: Bump-pointer speed; no contention.
- **Code**: `mem::ScratchPad::allocateRaw(bytes, alignment)`.

### Slab / InSituSlab — fixed-buffer LIFO
- **When**: You already have a fixed buffer (stack array, memory-mapped region,
  static storage) and want an arena-style allocator on top of it.
- **Characteristics**: `Slab` wraps an existing buffer; `InSituSlab<CapacityV>`
  embeds the buffer directly as `alignas(std::max_align_t) std::byte[CapacityV]`.
  Neither owns the memory. Throws `std::bad_alloc` on OOM (Slab) or returns
  null (InSitu — caller decides).
- **Cost**: Trivial bump pointer.
- **Code**: `mem::InSituSlab<1024u> slab;`

### InSitu — single-fixed-buffer one-shot
- **When**: You need at most one allocation from a tiny stack-fixed buffer,
  typically as the first choice in a Fallback/Threshold composite.
- **Characteristics**: 7A/7F status byte guards against double-use. Only one
  allocation possible before deallocate. Poisoned on construction/destruction.
- **Cost**: Essentially free.
- **Code**: `mem::InSitu<64u> insitu;`

### Pooling — lock-free fixed-size block pool
- **When**: Many threads repeatedly allocate and free identical fixed-size
  blocks (e.g., network packets, message objects, small nodes).
- **Template params**: `Pooling<BlockSizeV, BlockAllocatorT, MaxNumBlocksV, AlignmentV>`.
  `MaxNumBlocksV` must be a power of two (enforced via bitmask requirement).
- **Characteristics**: Divides the heap into pools of `Bitmask<size_t>::bit_count`
  blocks each. Lock-free operations via `compare_exchange_strong` on atomic
  bitmasks. On overflow, a mutex-protected slow path allocates new pools.
  Fully unused pools are released back to `BlockAllocatorT`.
- **Cost**: Near-zero on hot path (single CAS). Slow path rare.
- **Code**: `mem::Pooling<64u, mem::GPA, 256u> pool;`

### LocalCache — MRU block cache
- **When**: A single thread repeatedly allocates and frees the same fixed-size
  block and wants to avoid hitting the backing allocator.
- **Template params**: `LocalCache<BlockSizeV, AllocatorT, MaxNumBlocks = 2, AlignmentV>`.
- **Characteristics**: Ring buffer of recently freed blocks. On `allocateRaw`,
  pops MRU block; on `deallocateRaw`, pushes to MRU; evicts oldest if full.
  Calls `shrinkToFit()` on destruction.
- **Cost**: Ring buffer push/pop — essentially free.
- **Code**: `mem::LocalCache<64u, mem::GPA, 2u> cache;`

### Composite shorthand aliases

| Alias | Expansion | Use Case |
|-------|-----------|----------|
| `InSituFallback<Size, FallbackT>` | `Inplace<Fallback<InSitu<Size>, FallbackT>>` | First try a fixed stack buffer, then heap. |
| `InSituThreshold<Size, FallbackT>` | `Inplace<Threshold<InSitu<Size>, Size, FallbackT>>` | Small allocs use stack buffer; large go to heap. |
| `Static<&GetAllocatorF>` | `Accessor<AllocatorT, GetAllocatorF>` | Adapter wrapping a singleton allocator accessor function. |

---

## 3. Allocator Concept Requirements

### `TAllocator` (5 lines)
The fundamental concept. Every allocator must provide:

```cpp
// Returns {ptr, count} — aligned block of at least `bytes`.
// On failure, `ptr` is nullptr and `count` is 0.
[[nodiscard]] std::allocation_result<void *>
allocateRaw(std::size_t bytes, std::align_val_t alignment) noexcept;

// Returns the block to the allocator. `ptr` must come from
// a previous `allocateRaw` call; `bytes` and `alignment`
// must match the values passed to that call.
void deallocateRaw(void *ptr, std::size_t bytes, std::align_val_t alignment) noexcept;
```

### `TOwningAllocator` (extends TAllocator)
Adds ownership query, enabling `Fallback` to route deallocation correctly:

```cpp
// Returns true if `ptr` + `size` falls within memory owned by this allocator.
[[nodiscard]] bool owns(const void *ptr, std::size_t size) const noexcept;
```

### `TResizableAllocator` (extends TAllocator)
Adds in-place resizing for arena-style allocators:

```cpp
// Attempts to grow or shrink the block at `ptr` from `old_size` to `new_size`.
// Returns true if successful (ptr remains valid). On false, caller must
// allocate+copy+deallocate.
[[nodiscard]] bool resizeRaw(void *ptr, std::size_t old_size, std::size_t &new_size) noexcept;
```

Note: For `Slab`/`Arena`, resize only works when `ptr` is the **most recent**
allocation (LIFO top). Otherwise returns false.

### `TBlockAllocator` (extends TAllocator)
Statically advertises a fixed block size for pool backends:

```cpp
static constexpr std::size_t block_size_v = ...;
```

Satisfied by `HugePage` (2 MiB), `SmallPage` (32/64 KiB), and `LocalCache`/`Pooling`
(their own `BlockSizeV`).

### `TArenaAllocator` (extends TOwningAllocator + TResizableAllocator)
Arena-style watermark-based allocator:

```cpp
// Returns the current allocation pointer — a checkpoint for restore.
[[nodiscard]] const void *watermark() const noexcept;

// Rewinds to a previous watermark. No destructors called. O(1) for
// the current slab; O(slabs) if the mark is in an older slab.
void restore(const void *mark) noexcept;

// Resets to empty state (rewinds all slabs to their initial position,
// keeping the first slab's allocation).
void reset() noexcept;
```

### Concept usage examples from tests

```cpp
static_assert(mem::details::TAllocator<mem::GPA>);
static_assert(mem::details::TAllocator<mem::OS>);
static_assert(mem::details::TBlockAllocator<mem::HugePage>);
static_assert(mem::details::TBlockAllocator<mem::SmallPage>);
static_assert(mem::details::TArenaAllocator<mem::Arena<mem::GPA>>);
```

---

## 4. Arena Patterns

### Slab — Fixed-buffer LIFO

```cpp
// From an existing buffer
std::byte storage[1024];
mem::Slab slab(storage);
void *p = slab.allocateRaw(64u, max_align_v).ptr;

// As an InSituSlab with embedded storage
mem::InSituSlab<256u> slab;
void *p = slab.allocateRaw(32u, max_align_v).ptr;

// Restore to checkpoint
const void *mark = slab.watermark();
void *p2 = slab.allocateRaw(16u, max_align_v).ptr;
slab.restore(mark); // p2 is now "freed"

// Reset entirely
slab.reset();
```

Constraints:
- Only the **most recent** allocation can be resized or deallocated (LIFO).
- `allocateRaw` on a full `Slab` throws `std::bad_alloc`.
- `InSituSlab` does **not** own its storage; it's embedded in the object.
- `resizeRaw(p, old, new)` returns `false` if `p` is not the top allocation.
- `deallocateRaw(p, bytes, alignment)` returns `false` if `p` is not the top.

### Arena — Growable Slab Chain

```cpp
// Default: 2 MiB slabs backed by HugePage
mem::Arena arena;

// Explicit initial capacity and backing allocator
mem::Arena<mem::GPA> arena(4096u, mem::GPA{});

// Allocate — grows automatically by pushing new slabs
void *p = arena.allocateRaw(128u, max_align_v).ptr;

// Watermark / Restore
const void *mark = arena.watermark();
void *tmp = arena.allocateRaw(64u, max_align_v).ptr;
// ... work ...
arena.restore(mark); // O(1) if mark is in current slab

// Reset to initial state
arena.reset();
```

Constraints:
- Same LIFO restrictions as Slab.
- On `restore` across a slab boundary, intermediate slabs are popped and
  freed back to the backing allocator.
- Destructor calls `reset()` then `popSlab_()` — any allocations not freed
  by the caller will leak memory (no per-object destructor calls).

### ScopedArena — RAII Watermark Guard

```cpp
void process_frame(mem::Arena<> &frame_arena) {
    // Snapshot current position
    mem::ScopedArena scope(frame_arena);
    // Allocations inside this scope are automatically rolled back
    // when `scope` goes out of scope (RAII).
    void *p = frame_arena.allocateRaw(256u, max_align_v).ptr;
    // ... use p ...
    // ~ScopedArena calls frame_arena.restore(scope.m_scope_offset)
}
```

Supports move semantics — moving a `ScopedArena` transfers ownership of the
scope boundary. The moved-from instance becomes inert.

### ScratchPad — TLS Transient Arena

```cpp
// Allocate directly (static methods)
void *p = mem::ScratchPad::allocateRaw(64u, max_align_v).ptr;

// Scoped RAII — auto-restores on scope exit
{
    auto scope = mem::ScratchPad::open(); // returns scoped_arena_t
    void *tmp = mem::ScratchPad::allocateRaw(32u, max_align_v).ptr;
    // ...
} // scope destructor restores the TLS arena to the watermark

// Manual watermark/restore
const void *mark = mem::ScratchPad::watermark();
void *buf = mem::ScratchPad::allocateRaw(128u, max_align_v).ptr;
mem::ScratchPad::restore(mark);
```

In debug builds `ScopedArenaWithDebug` tracks nesting depth to catch
mismatched restore ordering (asserts on destruction).

---

## 5. Composite Allocator Patterns

### Fallback — Primary then Secondary

Routes all allocations to `PrimaryT` first. If `PrimaryT::allocateRaw` returns
nullptr (OOM), tries `SecondaryT`. Deallocation uses `PrimaryT::owns` to
determine which allocator to return to.

```cpp
mem::Fallback<mem::InSitu<64u>, mem::GPA> alloc;
void *p = alloc.allocateRaw(32u, max_align_v).ptr; // from InSitu
void *q = alloc.allocateRaw(128u, max_align_v).ptr; // from GPA (InSitu exhausted)
alloc.deallocateRaw(q, 128u, max_align_v);          // routed to GPA
alloc.deallocateRaw(p, 32u, max_align_v);           // routed back to InSitu
```

### Threshold — Under/Above Size Split

Routes allocations ≤ `SizeThresholdV` to `UnderT`, larger to `AboveT`.
`resizeRaw` will only succeed if the old and new sizes stay within the same bucket.

```cpp
mem::Threshold<mem::InSitu<64u>, 64u, mem::GPA> alloc;
void *small = alloc.allocateRaw(64u, max_align_v).ptr;   // from InSitu
void *large = alloc.allocateRaw(80u, max_align_v).ptr;   // from GPA
// resize from 64 to 32 works (same bucket)
alloc.resizeRaw(small, 64u, 32u);
// resize from 64 to 96 fails (crosses threshold)
alloc.resizeRaw(small, 32u, 96u); // returns false
```

### InSituFallback / InSituThreshold

Convenience aliases that wrap `Fallback<InSitu<N>, FallbackT>` or
`Threshold<InSitu<N>, N, FallbackT>` in `Inplace<>` for direct embedding
(no reference wrapper, no copy allowed).

```cpp
using MyAlloc = mem::InSituFallback<256u, mem::GPA>;
// Equivalent to:
// mem::Inplace<mem::Fallback<mem::InSitu<256u>, mem::GPA>>
```

### LocalCache — MRU Block Cache

Caches recently freed blocks in a ring buffer. On `allocateRaw`, returns the
most recently freed block (hot cache hit). On `deallocateRaw`, pushes to the
ring; if full, evicts the oldest block back to the backing allocator.

```cpp
mem::LocalCache<64u, mem::GPA, 4u> cache;

void *a = cache.allocateRaw(64u, max_align_v).ptr; // from GPA
cache.deallocateRaw(a, 64u, max_align_v);          // cached in MRU ring
void *b = cache.allocateRaw(64u, max_align_v).ptr; // returns a — fast path
PPR_ASSERT(a == b);
```

Constraints:
- `bytes` must equal `BlockSizeV` (asserted).
- Not thread-safe (single-threaded MRU cache).

### Pooling — Lock-free Fixed-Size Pool

Concurrent lock-free pool. Divides blocks into "pools" of
`Bitmask<size_t>::bit_count` blocks each. Each pool is tracked by an atomic
bitmask. Hot path uses lock-free CAS. The slow path (new pool needed / pool
release) uses a mutex.

```cpp
mem::Pooling<64u, mem::GPA, 256u> pool;

// Thread 1
void *a = pool.allocateRaw(64u, max_align_v).ptr;

// Thread 2
void *b = pool.allocateRaw(64u, max_align_v).ptr;

pool.deallocateRaw(a, 64u, max_align_v);
pool.deallocateRaw(b, 64u, max_align_v);
```

Constraints:
- `MaxNumBlocksV` must be a power of two (bitmask alignment).
- `BlockSizeV` must be > 0.
- Blocks are always the same size; `bytes` must equal `BlockSizeV` (asserted).
- Destructor asserts all blocks have been returned.

### HintedPooling — Pooling with TLS Index Hint

Extends `Pooling` with a thread-local `PoolingHintT` that caches the last-used
pool index per thread, reducing pool search on hot path.

```cpp
// Hint type must have a `static thread_local u32 value` member
struct MyHint {
    static thread_local u32 value;
};
thread_local u32 MyHint::value{};

mem::HintedPooling<64u, mem::GPA, 256u, MyHint> pool;
void *p = pool.allocateRaw(64u, max_align_v).ptr;
// Next allocation on the same thread will try MyHint::value first
void *q = pool.allocateRaw(64u, max_align_v).ptr;
```

### Static — Accessor via Callback

Wraps a free function `GetAllocatorF()` returning a reference to a singleton
allocator. Used internally by `HugePage`/`SmallPage` to route through TLS.

```cpp
// Pattern: wrap a global pool accessor
mem::Static<&MyPool::get> pool;
void *p = pool.allocateRaw(4096u, max_align_v).ptr;
```

---

## 6. AllocatorTraits Usage

`AllocatorTraits<AllocatorT>` is mixed into `Allocator<AllocatorT>` and also
into `Slab`, `Arena`, and other allocators via public inheritance. It provides
typed convenience methods accessible with `al.template method<T>(...)`.

```cpp
mem::Allocator<mem::GPA> alloc;

// Typed allocation
int *arr = alloc.allocate<int>(4u);
arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;

// allocate_at_least — may return more than requested
auto [ptr, count] = alloc.allocate_at_least<int>(3u);
// count >= 3

// Typed deallocation
alloc.deallocate<int>(arr, 4u);

// Object construction
struct Widget { int x; };
Widget *w = alloc.create<Widget>(42);
// Equivalent to: allocate<Widget>() + construct_at

// Object destruction
alloc.destroy(w);
// Equivalent to: destroy_at + deallocate<Widget>

// Relocation (requires relocatable type)
auto [new_ptr, new_count] = alloc.relocate<int>(arr, 4u, 8u);
// May resize in-place if the allocator supports it, otherwise
// allocates new + memcpy + deallocate old

// Span allocation (returns std::span<T>)
std::span<int> sp = alloc.span<int>(10u);

// Duplicate a contiguous range
std::vector<int> src = {1, 2, 3};
std::span<int> dup = alloc.dup(src);
// dup now owns a copy of src's data
```

**`pP::details::relocatable<T>`**: Types marked as relocatable support
`memcpy`-based relocation. The built-in trait defaults to true for trivially
copyable types. Specialize `pP::details::is_relocatable_v<T>` for your own
types to enable `relocate` and `span`/`dup`.

```cpp
template<>
inline constexpr bool pP::details::is_relocatable_v<MyType> = true;
```

---

## 7. Allocation<T> RAII Handle

`Allocation<T, AllocatorT, AlignmentV, SizeT>` is a RAII wrapper that owns
a typed allocation and automatically deallocates on destruction (for stateless
allocators; stateful allocators require explicit deallocation).

```cpp
// Stateless allocator (GPA) — auto-deallocates
{
    mem::Allocation<int, mem::GPA> alloc(10u);
    alloc[0] = 42;
    alloc[1] = 99;
    // ... use alloc.view() ...
} // ~Allocation calls GPA::deallocateRaw

// Stateful allocator — must deallocate explicitly
mem::Allocation<int, RecordingAllocator> alloc(4u, backend);
alloc.resize(backend, 8u); // resize if possible
alloc.deallocateAssumeNotEmpty(backend);
// Destructor asserts that deallocation already happened

// Create/destroy non-trivial objects
mem::Allocation<Widget, mem::GPA> walloc;
walloc.create(42); // allocate + construct_at
walloc.destroy();  // destroy_at + deallocate

// Relocate
auto result = alloc.relocate(16u); // may resize or allocate+copy+free

// Discard ownership (leak memory intentionally)
std::span<int> leaked = alloc.discard();
// alloc is now empty; memory NOT freed
```

Key methods:
- `isValid()` — whether the handle owns an allocation.
- `data()` / `count()` / `size_bytes()` — access the allocation.
- `operator[]` — indexed access with bounds assertion.
- `view()` — returns `std::span<T>`.
- `owns(ptr, size)` — overlap test.
- `allocate(n)` / `deallocate()` / `resize(n)` / `relocate(n)`.
- `create(args...)` / `destroy()` — for non-trivial `T`.
- `discard()` — releases ownership without deallocation, returns a span.

Stateless allocators (`GPA`, `OS`, `HugePage`, `SmallPage`, `ScratchPad`)
allow methods without an allocator argument. Stateful allocators require
passing the allocator reference to every method.

---

## 8. STL Adapter

`STL<T, AllocatorT>` satisfies the C++23 `std::allocator` requirements,
letting you pass PPR allocators to `std::vector`, `std::basic_string`, etc.

```cpp
// Vector using GPA
using GpaVec = std::vector<int, mem::STL<int, mem::GPA>>;
GpaVec vec;
vec.push_back(42);

// Vector using an arena
mem::Arena arena(4096u);
mem::STL<int, mem::Arena<mem::GPA>> stl_alloc(arena);
std::vector<int, decltype(stl_alloc)> arena_vec(stl_alloc);
arena_vec.push_back(1);
arena_vec.push_back(2);

// String using ScratchPad
mem::STL<char, mem::ScratchPad> sp_alloc;
std::basic_string<char, std::char_traits<char>, decltype(sp_alloc)> scratch_str(sp_alloc);
scratch_str = "temporary";
```

`STL<T, AllocatorT>` inherits from `Allocator<AllocatorT>` and forwards
`allocate(n)` to `allocate<T>(n)` and `deallocate(ptr, n)` to `deallocate<T>(ptr, n)`.
It sets `propagate_on_container_move_assignment = std::true_type`.

---

## 9. Poison API

The poison subsystem detects use-after-free, use-before-initialization, and
buffer overruns. Controlled by the `PPR_ENABLE_MEMORY_POISONING` macro
(automatically enabled in debug builds or when ASAN is active).

When ASAN is enabled (`PPR_ENABLE_SANITIZER_ADDRESS`), the poison functions
call the ASAN runtime directly. In debug-only mode, they flood memory with
distinct byte patterns:

| Function | Pattern | When to Call |
|----------|---------|--------------|
| `poisonReserved(ptr, size)` | `0xAA` | After reserving memory that is not yet usable (constructor of InSitu, reset/clear of containers). |
| `unpoisonUninitialized(ptr, size)` | `0xCC` | After allocating memory that the caller will write to (before returning from allocateRaw). |
| `poisonDestroyed(ptr, size)` | `0xDD` | After freeing/destructing memory (before returning from deallocateRaw, after destroy_at). |
| `annotateContiguousContainer(storage, capacity, old_live, new_live)` | mixed | When a vector-like container's live count changes — poisons the newly-unused or newly-reserved region. |

**Type-safe overloads** accept typed pointers and a count:

```cpp
int *arr = ...;
mem::poisonReserved(arr, 10);    // treats as sizeof(int) * 10 bytes
mem::unpoisonUninitialized(arr, 10);
mem::poisonDestroyed(arr, 10);

// Contiguous container annotation — typed
mem::annotateContiguousContainer(storage_ptr, capacity, old_size, new_size);
```

**Range overloads** accept contiguous ranges:

```cpp
std::span<int> span = ...;
mem::poisonReserved(span);
mem::unpoisonUninitialized(span);
mem::poisonDestroyed(span);
```

**`UnpoisonUninitializedIterator`** wraps an input iterator to automatically
unpoison memory as elements are constructed:

```cpp
auto it = mem::UnpoisonUninitializedIterator(raw_it);
std::uninitialized_copy(src.begin(), src.end(), it);
```

**Double-ended container annotations** (`annotateDoubleEndedContiguousContainer`) support
containers like `std::deque` or ring buffers that grow from both ends.

**Empty container helpers** (`annotateEmptyContiguousContainer`, `annotateEmptyDoubleEndedContiguousContainer`)
conveniently annotate a fully-empty container in two passes.

### When to call each function (canonical pattern)

```
1. Reserve / allocate storage block:
   → poisonReserved(block, size)

2. Carve out a sub-allocation from the block:
   → unpoisonUninitialized(sub_block, sub_size)

3. User constructs objects into the sub-block:
   → (construct_at / uninitialized_* — already unpoisoned)

4. User destructs objects:
   → destroy_at(ptr)

5. Return sub-block to the pool:
   → poisonDestroyed(sub_block, sub_size)

6. Return whole block / reset:
   → poisonReserved(block, size)   (or just leave ASAN-poisoned)
```

### Implementation notes

- ASAN mode: `poisonDestroyed` skips the first byte (leaves a redzone gap
  before the poisoned region) to catch underflow reads.
- Debug mode: Patterns are XOR-seeded with a per-process salt and the pointer
  address to make pattern detection harder to accidentally match real data.
- The high nibble of every byte is the pattern constant; the low nibble is
  seeded-randomized (`0xAx` for reserved, `0xCx` for uninitialized, `0xDx`
  for destroyed).

---

## 10. safe_ptr Usage

`safe_ptr<T>` provides debug-mode lifetime checking for objects that inherit
from `safe_object`. In debug builds, it maintains a reference count on the
pointee and asserts on destruction/move/copy that no dangling references exist.

```cpp
class MyNode : public pP::safe_object {
    int value;
public:
    explicit MyNode(int v) : value(v) {}
    int get() const { return value; }
};

void example() {
    pP::safe_ptr<MyNode> ptr(new MyNode(42));
    pP::safe_ptr<MyNode> copy = ptr; // increments ref count

    // Access
    int v = ptr->get();
    int v2 = (*ptr).get();

    // Comparisons
    if (ptr == copy) { /* same object */ }

    // Upcast
    pP::safe_ptr<safe_object> base = std::move(ptr).upcast<safe_object>();
    // ptr is now null after the move+upcast
}
```

In release builds (`!PPR_ENABLE_DEBUG`), `safe_ptr<T>` becomes a simple
typedef to `T*` and `safe_object` is an empty base — zero overhead.

Key operations:
- Construction from raw pointer (`safe_ptr<T>(ptr)`) increments ref count.
- Copy: increments ref count.
- Move: transfers ownership without ref count change.
- `operator->`, `operator*`: assert non-null, return raw pointer/reference.
- `get()`: return raw pointer without assertion.
- `isValid()`: check for non-null.
- `upcast<BaseT>() &&`: move-convert to base safe_ptr (rvalue only).
- `checked_cast<DerivedT>(safe)`: downcast with runtime check (dynamic_cast in
  debug, static_cast in release).

**Important**: safe_object asserts on destruction if `m_safe_ref_count != 0`,
and asserts on move/copy if the source is still observed. This catches dangling
pointer bugs at the source of invalidation rather than at use.

---

## 11. Inplace — Embedding Allocators Without Reference Wrapping

`Inplace<AllocatorT>` is a thin wrapper that inherits from the allocator and
deletes copy operations. It is used when an allocator should be stored
**by value** (embedded) rather than by reference. The `use_inplace_v` trait
controls whether `Allocator<AllocatorT>` stores the allocator inline or as a
`std::reference_wrapper`.

All stateless allocators (`GPA`, `OS`, `HugePage`, `SmallPage`, `ScratchPad`,
`InSitu`, `Pooling`, `LocalCache`) are `use_inplace_v = true` — they are
always stored by value and are cheaply default-constructible.

Stateful allocators (e.g., a `RecordingAllocator` from tests, `Arena<>`) are
stored by reference inside `Allocator<>`. Use `Inplace<Arena<GPA>>` if you
want to embed the arena directly (but note: no copy allowed).

`AllocatorForceRef<AllocatorT>` unwraps any `Inplace<>` wrapper to force
reference semantics, useful when you need to share a single allocator instance.

```cpp
// Arena is stateful — Allocator stores a reference
mem::Arena arena(4096u);
mem::Allocator al(arena); // references `arena`

// InSitu is stateless — Allocator stores it inline (empty)
mem::Allocator<mem::InSitu<64u>> al; // InSitu constructed in-place

// Force reference even for inplace types
auto ref = al.forceRef(); // AllocatorForceRef<InSitu<64u>>
```

---

## 12. PMR — Runtime Polymorphic Allocator

`PMR` provides type-erased runtime dispatch for `TAllocator`. It stores a
`void* context` and a static `VTable` with function pointers for allocateRaw,
deallocateRaw, and resizeRaw.

```cpp
// Wrap a stateless allocator (no context needed)
mem::PMR pmr(mem::GPA{});
void *p = pmr.allocateRaw(64u, max_align_v).ptr;

// Wrap a stateful allocator (stores reference as context)
mem::Arena arena(4096u);
mem::PMR pmr(arena); // references arena via context pointer

// Wrap an Allocator<> wrapper
mem::Allocator<mem::GPA> alloc;
mem::PMR pmr(alloc); // unwraps to materialize()

// PMR objects wrapping the same allocator type compare equal
mem::PMR a(mem::GPA{}), b(mem::GPA{});
PPR_ASSERT(a == b);
```

---

## 13. Custom Placement new/delete for Arena

The allocator module exports global placement `operator new` and `operator delete`
overloads for any `TArenaAllocator`:

```cpp
mem::Arena<mem::GPA> arena(4096u);

// Allocates from arena via placement new
auto *widget = new (arena) Widget(42);

// If Widget's constructor throws, operator delete is called automatically
// to release the arena allocation.

// Manual destruction (does NOT release arena memory — use restore/reset):
widget->~Widget();
arena.restore(mark); // releases both widget and its allocation
```

---

## 14. Allocator<AllocatorT> — The Universal Wrapper

`Allocator<AllocatorT>` is the primary way to pass allocators to generic code.
It inherits from `AllocatorTraits<Allocator<AllocatorT>>` and conditionally
stores the allocator:

- If `use_inplace_v<AllocatorT>` is true (stateless or Inplace), the allocator
  is stored directly (empty base optimization).
- Otherwise, it stores a `std::reference_wrapper<AllocatorT>`.

It provides the full public API:

```cpp
// Construction
mem::Allocator<mem::GPA> stateless;              // default ctor
mem::Allocator wrapped(arena);                    // deduction guide
mem::Allocator<mem::Arena<mem::GPA>> stateful(arena);

// AllocatorTraits methods
int *arr = wrapped.allocate<int>(4u);
wrapped.deallocate(arr, 4u);
auto *w = wrapped.create<Widget>(42);
wrapped.destroy(w);

// Materialize the underlying allocator
auto &raw = wrapped.materialize();
const auto &craw = std::as_const(wrapped).materialize();

// Force reference semantics
auto forced = wrapped.forceRef(); // AllocatorForceRef<AllocatorT>
```

---

## 15. Static<F> — Singleton Accessor

`Static<&GetAllocatorF>` is a utility that wraps a nullary function returning
a reference to an allocator (typically a global or TLS singleton). It satisfies
`TAllocator` by forwarding all calls to `GetAllocatorF()`.

```cpp
// Used internally for HugePage's TLS LocalCache:
// static LocalCache<block_size_v, Static<&getGlobalPool>> g_instance_tls{};
```

---

## 16. MSVC ASAN Note

When using the `msvc-dev` CMake preset, ASAN (Address Sanitizer) is
automatically enabled via the `PPR_ENABLE_DEVELOPER_MODE` option. This
activates the ASAN code paths in the poison subsystem:

- `poisonReserved` calls `ASAN_POISON_MEMORY_REGION` (8-byte-aligned).
- `unpoisonUninitialized` calls `ASAN_UNPOISON_MEMORY_REGION`.
- `poisonDestroyed` calls `ASAN_POISON_MEMORY_REGION` with a 1-byte gap at
  the start to catch underflow.
- `annotateContiguousContainer` calls `__sanitizer_annotate_contiguous_container`.
- `annotateDoubleEndedContiguousContainer` calls
  `__sanitizer_annotate_double_ended_contiguous_container`.

The `__asan_default_options` function sets `abort_on_error=1` and `print_stats=1`
for immediate failure with diagnostics.

Many unit tests in `Core.Memory.Tests.cppm` are gated on
`if constexpr (mem::is_asan_enabled_v)` and use `UnitTest::expect_crash` to
verify that accessing poisoned memory correctly triggers ASAN.

In non-ASAN debug builds, the same functions flood memory with debug patterns
but do not trap access — use the ASAN preset for active detection.

---

## 17. Constraints and Best Practices

1. **No raw loops**: Use `AllocatorTraits` typed methods instead of raw
   `allocateRaw`/`deallocateRaw` calls whenever possible.

2. **RAII over manual**: Prefer `Allocation<T, AllocatorT>` or
   `ScopedArena` over manual allocate/deallocate pairs.

3. **LIFO discipline**: Arena allocators (`Slab`, `Arena`, `ScratchPad`) are
   LIFO only. resize/deallocate only works for the most recent allocation.
   Use `watermark()`/`restore()` for batch free; do not rely on per-object
   deallocation.

4. **Thread safety**:
   - `GPA`, `OS`: Thread-safe (delegate to global heap/OS).
   - `HugePage`, `SmallPage`: Thread-safe (TLS + global pool with locks).
   - `Arena`, `Slab`, `InSituSlab`: **Not thread-safe** — single-threaded use only.
   - `Pooling`, `HintedPooling`: Thread-safe (lock-free hot path).
   - `LocalCache`: **Not thread-safe** — thread-local use only.
   - `ScratchPad`: Thread-safe (each thread has its own TLS arena).

5. **Poison all paths**: Every `allocateRaw` should `unpoisonUninitialized`
   the returned block. Every `deallocateRaw` should `poisonDestroyed` it.
   Every reserve/reset should `poisonReserved` the freed region. The built-in
   allocators already follow this; follow the same pattern in custom allocators.

6. **Stateless vs stateful**:
   - Stateless allocators (`GPA`, `OS`, `HugePage`, `SmallPage`, `ScratchPad`,
     `InSitu`, `LocalCache`, `Pooling`) are empty and can be default-constructed
     anywhere. They are `use_inplace_v = true`.
   - Stateful allocators (`Arena<>`, custom allocators with data) must be
     constructed explicitly and passed by reference. Wrap in `Allocator<>` to
     get reference semantics.

7. **Block size alignment**: When using `Pooling` or `LocalCache`, the
   alignment must not exceed block size. The assertion `alignForward(alignment, block_size_v) == block_size_v`
   is enforced.

8. **No exceptions in deallocate**: All `deallocateRaw` implementations are
   `noexcept`. `allocateRaw` should be `noexcept` and signal failure by
   returning `{nullptr, 0}` — except for `Slab` which throws `std::bad_alloc`.

9. **`checked_cast` can be used for `std::size_t` → `u32` conversions** in
   arena offsets and block counts. The `safe_narrowing` tag type asserts
   round-trip fidelity.

10. **Contiguous container annotation** is required for any container that
    manages a live prefix within a larger buffer (like `std::vector` or
    `Arena`'s slab). Call `annotateContiguousContainer(storage, capacity, old_live, new_live)`
    every time the live region boundary moves.
