# Allocator-family and adapter patterns

Read the [entry skill](../SKILL.md) for failure semantics (§5), TLS (§2),
ownership, arena, poisoning, and teardown constraints. When implementing an
allocator, also read [concept declarations (§4)](concepts.md).

## 6. Arena Patterns

### Slab — Fixed-buffer LIFO

`Slab` wraps an existing buffer (pointer + capacity, `span`, or
`allocation_result`); it owns nothing — the caller owns the bytes.

```cpp
std::byte storage[1024];
mem::Slab slab(storage);
void *p = slab.allocateRaw(64u, max_align_v).ptr;  // throws bad_alloc when full

const void *mark = slab.watermark();
void *p2 = slab.allocateRaw(16u, max_align_v).ptr;
slab.restore(mark);  // p2 is now "freed"; no destructors called
slab.reset();
```

Constraints: only the most recent allocation resizes/deallocates (both
return `false` off the top); `allocateRaw` on a full `Slab` throws
`std::bad_alloc`.

### InSituSlab — Embedded Fixed Buffer

```cpp
mem::InSituSlab<256u> slab;  // alignas(max_align_t) bytes owned by the object
void *p = slab.allocateRaw(32u, max_align_v).ptr;
```

`InSituSlab<CapacityV>` inherits `Slab` over its own embedded storage, so
the slab object itself owns the bytes (owned state, not stateless). Same
LIFO rules and throwing OOM as `Slab`.

### Arena — Growable Slab Chain

```cpp
mem::Arena arena;  // default: block-sized slabs backed by HugePage
mem::Arena<mem::GPA> arena(4096u, mem::GPA{});

void *p = arena.allocateRaw(128u, max_align_v).ptr;  // pushes slabs on demand

const void *mark = arena.watermark();
void *tmp = arena.allocateRaw(64u, max_align_v).ptr;
// ... work ...
arena.restore(mark);  // pops intermediate slabs back to the backing allocator
arena.reset();
```

`Arena` inherits `AllocatorTraits<Arena>` and its backing allocator; it is
move-only (moves transfer the chain, source goes null). The destructor calls
`reset()` then releases every slab — live allocations leak by design (no
per-object destructor calls), so destroy objects before reset. Restoring to
a mark outside the arena terminates (`unreachable`).

### ScopedArena — RAII Watermark Guard

```cpp
void process_frame(mem::Arena<> &frame_arena) {
    mem::ScopedArena scope(frame_arena);  // CTAD from ArenaT&
    void *p = frame_arena.allocateRaw(256u, max_align_v).ptr;
    // ... use p ...
}  // ~ScopedArena restores the watermark; moves transfer the boundary
```

Moves transfer the restore boundary; the moved-from guard goes inert. In
debug builds `ScopedArenaWithDebug` tracks nesting depth and asserts on
mismatched restore ordering at destruction.

### ScratchPad — TLS Transient Arena

```cpp
void *p = mem::ScratchPad::allocateRaw(64u, max_align_v).ptr;

{
    auto scope = mem::ScratchPad::open();  // Allocator<scoped_arena_t>
    void *tmp = mem::ScratchPad::allocateRaw(32u, max_align_v).ptr;
    // ...
}  // scope destructor restores the TLS arena to the watermark

const void *mark = mem::ScratchPad::watermark();
void *buf = mem::ScratchPad::allocateRaw(128u, max_align_v).ptr;
mem::ScratchPad::restore(mark);
```

`ScratchPad` is an empty static facade over a per-thread
`Arena<SmallPage>` (debug: `ScopedArenaWithDebug`-guarded). `deallocateRaw`
and `resizeRaw` return `bool` and apply LIFO-top-only. See
[§2](../SKILL.md#2-thread-local-storage-and-cross-thread-handles) for the
cross-thread rules: never let these handles or their pointers escape the
thread.

---

## 7. Composite Allocator Patterns

### Fallback — Primary then Secondary

Routes to `PrimaryT` first, to `SecondaryT` on `{nullptr, 0}`; deallocation
and resize route via `PrimaryT::owns`, so both sides must be owning
allocators. From `fallback_prefers_primary_then_secondary`
(`ResizeOnlyAllocator` is that file's owning resizable test double — any
owning resizable allocator works the same way):

```cpp
mem::Fallback<mem::InSitu<32u>, ResizeOnlyAllocator> alloc{};

const auto primary = alloc.allocateRaw(16u, max_align_v);    // from InSitu
const auto secondary = alloc.allocateRaw(16u, max_align_v);  // InSitu spent

std::size_t primary_size = 24u;
alloc.resizeRaw(primary.ptr, 16u, primary_size);   // true, same owner
primary_size = 48u;
alloc.resizeRaw(primary.ptr, 24u, primary_size);    // false, InSitu is one-shot

alloc.deallocateRaw(secondary.ptr, 8u, max_align_v);  // routed by owns()
alloc.deallocateRaw(primary.ptr, 24u, max_align_v);
```

### Threshold — Under/Above Size Split

`Threshold<UnderT, SizeThresholdV, AboveT>`: sizes at or under the threshold
go to `UnderT`, larger to `AboveT`. Resize stays within its bucket (same
test double as above):

```cpp
mem::Threshold<mem::InSitu<64u>, 64u, ResizeOnlyAllocator> alloc{};

const auto under = alloc.allocateRaw(64u, max_align_v);  // from InSitu
const auto above = alloc.allocateRaw(80u, max_align_v);  // from fallback

std::size_t under_size = 32u;
alloc.resizeRaw(under.ptr, 64u, under_size);   // true, same bucket
under_size = 96u;
alloc.resizeRaw(under.ptr, 32u, under_size);   // false, crosses threshold
```

### InSitu — Single One-Shot Buffer

`InSitu<SizeV>` embeds `SizeV` bytes plus a `0x7F`-free / `0x7A`-used status
byte: at most one live allocation, then `{nullptr, 0}` until deallocated
(the caller decides). Poisoned on construction, re-poisoned on deallocate,
unpoisoned on destruction so enclosing stack reuse stays addressable. The
buffer is owned state — `InSitu` is stored by reference unless wrapped (see
`Inplace` below). Typical use is as the primary of a composite:

```cpp
using MyAlloc = mem::InSituFallback<256u, mem::GPA>;
// == mem::Inplace<mem::Fallback<mem::InSitu<256u>, mem::GPA>>
using MySplit = mem::InSituThreshold<256u, mem::GPA>;
// == mem::Inplace<mem::Threshold<mem::InSitu<256u>, 256u, mem::GPA>>
```

### LocalCache — MRU Block Cache

Single-threaded ring of recently freed fixed-size blocks; destruction runs
`shrinkToFit()`, returning cached blocks to the backing allocator.
`MaxNumBlocks` defaults to 2. `bytes` must equal `BlockSizeV` (asserted);
`owns()` forwards to the backing allocator when it is an owning allocator.

```cpp
mem::LocalCache<64u, mem::GPA, 4u> cache;

void *a = cache.allocateRaw(64u, max_align_v).ptr;  // from GPA
cache.deallocateRaw(a, 64u, max_align_v);           // parked in the MRU ring
void *b = cache.allocateRaw(64u, max_align_v).ptr;  // hot reuse of the block
```

`HugePage`/`SmallPage` are `LocalCache<block_size_v, Static<&getGlobalPool>[, 2u]>`
over their global pools — the same MRU shape, per thread
([§2](../SKILL.md#2-thread-local-storage-and-cross-thread-handles)).

### Pooling — Lock-free Fixed-Size Pool

`Pooling<BlockSizeV, BlockAllocatorT, MaxNumBlocksV, AlignmentV>` provisions
`num_pools_v` pools of 64 blocks (the pool quantum) from the block
allocator. Hot-path allocate/deallocate are lock-free CAS on per-pool atomic
bitmasks; growing a pool and releasing a fully-unused pool take the pool
mutex. Fully unused pools return to the block allocator when more than one
pool is alive.

**Provision, don't scale.** `num_pools_v = ceil(MaxNumBlocksV / 64)`;
provisioned capacity is `num_pools_v * 64` usable blocks — non-multiples of
64 round *up* to that usable count. The `requires` expression on the class is
advisory: always provision a multiple of 64, size `MaxNumBlocksV` for
worst-case simultaneously-live blocks, and assert the bound in tests.
Exhaustion terminates (`throwBadAlloc` through `noexcept`) — never rely on
`{nullptr, 0}`. Concurrent allocate/deallocate only; never concurrent with
destruction or shrink equivalents. `owns()` accepts an optional pool-index
hint out-param; `HintedPooling<..., PoolingHintT>` threads a
`thread_local u32` hint (`TPoolingHint::value`) through automatically:

```cpp
mem::Pooling<64u, mem::GPA, 256u> pool;  // 4 pools x 64 blocks provisioned

void *a = pool.allocateRaw(64u, max_align_v).ptr;  // CAS hot path
pool.deallocateRaw(a, 64u, max_align_v);

struct MyHint { static thread_local u32 value; };
thread_local u32 MyHint::value{};
mem::HintedPooling<64u, mem::GPA, 256u, MyHint> hpool;
```

### Static — Singleton Accessor

`Static<&GetAllocatorF> = Accessor<...>` forwards every call to a nullary
function returning an allocator reference (typically a global pool or TLS
singleton) — the glue behind `HugePage`/`SmallPage` routing.

---

## 8. AllocatorTraits Usage

`AllocatorTraits<AllocatorT>` uses explicit-`this` methods mixed into
`Allocator<AllocatorT>`, `Slab`, `Arena`, and other allocators, so
`al.template method<T>(...)` works on any of them. From
`allocator_traits_operations`:

```cpp
mem::Allocator wrapped{backend};  // deduction guide; reference semantics

int *ints = wrapped.allocate<int>(2u);
ints[0] = 7; ints[1] = 11;
wrapped.deallocate<int>(ints, 2u);

const auto at_least = wrapped.allocate_at_least<int>(3u);  // count >= 3
wrapped.deallocate<int>(at_least.ptr, 3u);

const auto span = wrapped.span<int>(4u);  // relocatable T only
wrapped.deallocate<int>(span.data(), 4u);

Widget *widget = wrapped.create<Widget>(42);  // allocate + construct_at
wrapped.destroy(widget);                      // destroy_at + deallocate

const auto fresh = wrapped.relocate<int>(nullptr, 0u, 3u);   // fresh 3-block
const auto shrunk = wrapped.relocate<int>(fresh.ptr, 3u, 2u);  // in place
```

`create(al, copy)` / `create(al, move)` return owning `Allocation<T, ...>`
handles; `dup(range)` memcpys a contiguous range into a new span. Relocation
is `memcpy`-based only for relocatable types — mark yours by specializing
the trait struct (not the variable):

```cpp
template<>
struct pP::details::relocatable<MyType> : std::true_type {};
```

`RecordingAllocator` and `Widget` are test doubles in
`Core.Allocator.Tests.cpp`, used by `allocator_traits_operations`.

---

## 10. Inplace — Embedding Allocators by Value

`Inplace<AllocatorT>` inherits the allocator and deletes copies, forcing
by-value embedding. `use_inplace_v<T>` is `is_empty<T>` (plus an explicit
opt-in for `Inplace<>`, `PMR`, `ScopedArena`): empty allocators are stored
inline in `Allocator<>`, everything with owned state is stored by
`reference_wrapper` unless wrapped:

```cpp
mem::Arena arena(4096u);
mem::Allocator al(arena);  // Arena has state -> stored by reference

mem::Allocator<mem::GPA> gpa_al;  // GPA is empty -> stored inline

auto ref = al.forceRef();  // AllocatorForceRef: reference even for inline types
```

`materialize()` reaches the underlying allocator in both modes. `Arena` is
move-only and transfers its chain (source goes null); `Inplace` deletes
copies entirely. `Allocator` itself is copyable and converts implicitly to
`AllocatorT&`.

---

## 11. PMR — Runtime Polymorphic Allocator

`PMR` type-erases any `TAllocator` over a `void*` context plus a static
vtable (`allocateRaw` / `deallocateRaw` / `resizeRaw`). Empty allocators need
no context; stateful ones are referenced (never owned — the PMR must not
outlive them). From `pmr_erasure_and_equality`:

```cpp
const mem::PMR stateful{backend};
const mem::PMR erased{mem::Allocator{backend}};
PPR_TEST_ASSERT(stateful == erased);  // same type erasure compares equal

const mem::PMR stateless{mem::GPA{}};
const auto overaligned = stateless.allocateRaw(32u, std::align_val_t{64u});
stateless.deallocateRaw(overaligned.ptr, overaligned.count, std::align_val_t{64u});
```

---

## 12. Custom Placement new/delete for Arena

Global `operator new` / `operator delete` overloads exist for any
`TArenaAllocator` (defined in the global namespace):

```cpp
mem::Arena<mem::GPA> arena(4096u);

auto *widget = new (arena) Widget(42);  // allocateRaw(size, max_align_v)
// Constructor throw -> matching placement delete releases the block.
// Manual end: widget->~Widget(); then arena.restore(mark).
```

---

## 13. STL Adapter

`STL<T, AllocatorT>` satisfies the C++23 `std::allocator` requirements over
an `Allocator<AllocatorT>` base. `propagate_on_container_move_assignment`
is `true_type` only when the allocator is *stateful*
(`not is_stateless_v`) — stateless adapters never propagate because there is
nothing to transfer:

```cpp
using GpaVec = std::vector<int, mem::STL<int, mem::GPA>>;
GpaVec vec;
vec.push_back(42);

mem::Arena arena(4096u);
mem::STL<int, mem::Arena<mem::GPA>> stl_alloc(arena);
std::vector<int, decltype(stl_alloc)> arena_vec(stl_alloc);
arena_vec.push_back(1);

mem::STL<char, mem::ScratchPad> sp_alloc;  // TLS arena; same-thread use (entry skill §2)
std::basic_string<char, std::char_traits<char>, decltype(sp_alloc)> s(sp_alloc);
```

`allocate(n)` forwards to `allocate<T>(n)`; `deallocate(ptr, n)` to
`deallocate<T>(ptr, n)`; `allocate_at_least` is also provided.
