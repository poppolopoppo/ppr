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

## Contract

This skill is a reference guide for PPR memory allocation. It **does not**
edit allocator code, apply poison annotations, or modify engine state. Its
output is a structured guide for allocator selection, composition, and usage
patterns.

## Mandatory pre-implementation / review checklist

- Name the allocation owner, every non-owning observer, and the exact teardown
  point using the vocabulary in §1; an arena reset/restore never destroys
  objects.
- State boundary failure semantics from the table in §3: `{nullptr, 0}` for
  fallible `noexcept` allocation, the documented `Slab` exception, and
  no-throw deallocation.
- Prove shutdown order: stop users, destroy live objects, release observers,
  then reset/free backing storage; no callback may retain storage past this
  point.
- For `safe_ptr`, retain a real owner (`unique_ptr`, value member, service, or
  allocation handle); it is only an observer and becomes raw-pointer-equivalent
  in release, so debug assertions are not a lifetime mechanism.
- Add behavior-contract tests for success, OOM/overflow where exposed,
  LIFO/watermark boundaries, and teardown. For test file layout — private
  group `.cpp` files, `detail::` leaves, TU-local groups + accessors — see
  `unit-test-updater`.

## Execution handoff

This skill defines allocator-selection and lifecycle procedure only. The
active OMO preset is the sole authority for actor selection, permissions,
tool access, parallel work, and session reuse. Apply the procedure only
through the active configuration; this document grants or routes none of them.

---

## 1. Ownership and lifetime vocabulary

Use these terms the same way in every design note, review, and test. They
match the `concurrency-patterns` skill exactly.

- **Own**: `SharedContext` / `shared_ptr` handles, `Allocation` RAII handles
  (own raw storage until `deallocate`/`destroy`/`discard`), `unique_ptr`
  owners, arenas and slabs (own their backing slabs once pushed), and pool
  objects (own provisioned pools until destruction). The owner outlives every
  observer, and only the owner releases.
- **Observe only — never own**: `safe_ptr` (debug-checked observer, raw
  pointer in release), event subscriptions, spans and views over allocations,
  allocator references held inside `Allocator<>` wrappers, and raw-pointer
  callback captures. None of these extend a lifetime.
- Inline allocator storage is **owned state**, never "stateless": an object
  that embeds bytes (`InSitu` status + buffer, `LocalCache` MRU ring,
  `Pooling` bitmask arrays, `Arena` slab chain) owns that state and must be
  destroyed before its storage is reused. Only truly empty allocators
  (`GPA`, `OS`, page caches, `ScratchPad`, `PMR`) are stateless.
- **Quiesce** means drain in-flight work to idle before releasing storage.
  A **latch** is sticky state that never clears. A **cause** is the
  `error_code` carried by a latch. **Provisioned capacity** is pre-sized at
  construction (pools, slabs, rings) and never grows elastically — size it
  for the worst case and prove it in tests.

---

## 2. Thread-local storage and cross-thread handles

Page-cache TLS arenas, `ScratchPad`, and pooling index hints are
**per-thread**: a handle or cached block obtained on one thread must never be
used or released on another.

- `ScratchPad` is a per-thread `Arena<SmallPage>`; an
  `Allocation<T, ScratchPad>` destroyed on another thread deallocates through
  *that* thread's TLS arena, so keep allocations on their originating thread.
- Channels must not carry pointers into producer scratch whose scope restores
  before the consumer reads them — copy the bytes into the record or extend
  the scope past consumption.
- Async I/O buffers must outlive scratch scopes until OS quiescence: the HAL
  owns the in-flight bytes until completion or cancel acknowledgement, so the
  backing scope may not restore while a request is pending.
- `HugePage` / `SmallPage` route through a TLS `LocalCache`; `Pooling` hints
  (`TPoolingHint::value`) are `thread_local`. These entry points are safe to
  call from any thread precisely because each thread uses its own cache —
  never share the cached result across threads.

---

## 3. Quick Reference — Choosing an Allocator

All allocators live in `namespace pP::mem`; concepts in `mem::details`.

| Scenario | Recommended Allocator | Key API |
|----------|----------------------|---------|
| Single object, small array, temp buffer | `GPA` | `GPA::allocateRaw(bytes, alignment)` |
| Multi-MB buffer, file I/O scratch | `OS` | `OS::allocateRaw(bytes, alignment)` |
| Backend for arenas/pools | `HugePage` (2 MiB blocks) | `HugePage::allocateRaw(bytes, alignment)` |
| Transient scratch in hot code | `SmallPage` (32 KiB on 64-bit, 64 KiB on 32-bit) | `SmallPage::allocateRaw(bytes, alignment)` |
| Persistent batch LIFO (frame data) | `Arena<>` (defaults to `HugePage`) | `arena.allocateRaw(bytes, alignment)` |
| Short-lived TLS temp work | `ScratchPad` | `ScratchPad::allocateRaw(bytes, alignment)` |
| Fixed buffer arena (stack/mmap) | `Slab` / `InSituSlab<Cap>` | `slab.allocateRaw(bytes, alignment)` |
| Composable: try A then B | `Fallback<A, B>` | see §6 |
| Composable: small → A, large → B | `Threshold<Under, N, Above>` | see §6 |
| Pool + TLS cache | `Pooling` / `LocalCache` | see §6 |
| std:: container adapter | `STL<T, A>` | `STL<T, A>{allocator}` wraps any `TAllocator` |

Return to the full guide below for detailed semantics, composition patterns,
and poison/ASAN usage.

---

## 4. Allocator Concept Requirements

### `TAllocator`

```cpp
// Returns {ptr, count} — aligned block of at least `bytes`.
// On failure, `ptr` is nullptr and `count` is 0.
[[nodiscard]] std::allocation_result<void *>
allocateRaw(std::size_t bytes, std::align_val_t alignment) noexcept;

// Returns the block. `ptr` must come from a previous `allocateRaw`;
// `bytes` and `alignment` must match that call.
void deallocateRaw(void *ptr, std::size_t bytes, std::align_val_t alignment) noexcept;
```

### `TOwningAllocator` (extends `TAllocator`)

```cpp
// True when `ptr` + `size` falls within memory owned by this allocator.
// Used for deallocation routing (Fallback) — not a safety net for
// foreign pointers.
[[nodiscard]] bool owns(const void *ptr, std::size_t size) const noexcept;
```

### `TResizableAllocator` (extends `TAllocator`)

```cpp
// Attempts to grow or shrink `ptr` from `old_size`; `new_size` is in/out and
// may be adjusted. True keeps `ptr` valid; false means allocate+copy+free.
[[nodiscard]] bool resizeRaw(void *ptr, std::size_t old_size, std::size_t &new_size) noexcept;
```

For `Slab`/`Arena`, resize only succeeds on the **most recent** allocation
(LIFO top). Some composites take the size by value; always pass a named
variable so in/out adjustment can apply.

### `TBlockAllocator` (extends `TAllocator`)

```cpp
static constexpr std::size_t block_size_v = ...;
```

Satisfied by `HugePage` (2 MiB), `SmallPage` (32/64 KiB), and
`LocalCache`/`Pooling` (their own `BlockSizeV`).

### `TArenaAllocator` (extends `TOwningAllocator` + `TResizableAllocator`)

```cpp
[[nodiscard]] const void *watermark() const noexcept;  // checkpoint
void restore(const void *mark) noexcept;  // rewind; no destructors; O(1) in-slab
void reset() noexcept;                    // rewind everything, keep storage
```

### `TSlabAllocator` (extends `TArenaAllocator`)

```cpp
// The raw backing of the current slab (zero-copy iteration).
[[nodiscard]] std::allocation_result<void *> data() noexcept;
```

Satisfied by `Slab`, `InSituSlab`, and `Arena<T>`. `mem::overlap()`
helpers test pointer/range containment for ownership checks.

```cpp
static_assert(mem::details::TAllocator<mem::GPA>);
static_assert(mem::details::TAllocator<mem::OS>);
static_assert(mem::details::TBlockAllocator<mem::HugePage>);
static_assert(mem::details::TBlockAllocator<mem::SmallPage>);
static_assert(mem::details::TArenaAllocator<mem::Arena<mem::GPA>>);
```

---

## 5. Per-API Failure Table

Terminate-by-design until callers are null-safe: provision for the worst
case and assert it in tests rather than relying on `{nullptr, 0}` where the
API cannot produce it.

| Allocator | OOM / overflow behavior |
|---|---|
| Fallible `noexcept allocateRaw` (`GPA`, `OS`, page caches, `InSitu`, `LocalCache` cold path) | Returns `{nullptr, 0}` — the caller decides (fallback, error, retry). |
| `Slab::allocateRaw` (`noexcept(false)`) | Throws `std::bad_alloc` when the fixed buffer is exhausted. |
| `Pooling::allocateRaw` (marked `noexcept`) | Exhaustion calls `throwBadAlloc()` through `noexcept` — terminates. Never rely on `{nullptr, 0}` here: size `MaxNumBlocksV` for worst-case live blocks and assert it in tests. |
| `Arena` backing failure | `pushSlab_` asserts the backing allocation succeeded (`PPR_ASSERT(al.ptr)`). |
| All `deallocateRaw` | `noexcept`, no failure mode. |
| `resizeRaw` off-LIFO-top | Returns `false` — allocate + copy + deallocate. |

`Pooling` further asserts `bytes == block_size_v` and
`alignForward(alignment, block_size_v) == block_size_v` on both paths, and
its destructor asserts every block was returned
("Destroying a Pooling allocator with live allocations"). Concurrent
`allocate`/`deallocate` is supported; concurrent use with destruction or
shrink equivalents is not. `owns()` is deallocation routing, not a safety
net — a foreign pointer that happens to overlap still terminates in
`deallocateRaw` (`PPR_ASSERT(false)` + `unreachable`).

---

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
and `resizeRaw` return `bool` and apply LIFO-top-only. See §2 for the
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
over their global pools — the same MRU shape, per thread (§2).

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

(`RecordingAllocator`/`Widget` below are the doubles in
`Core.Allocator.Tests.cpp`, test `allocator_traits_operations`):

---

## 9. Allocation<T> RAII Handle

`Allocation<T, AllocatorT, AlignmentV, SizeT>` owns raw storage (not
constructed objects) until explicitly released. The destructor deallocates
**without `destroy_at`** for stateless allocators, and asserts prior release
for stateful ones — always pair `create` with `destroy`, or `allocate` with
`deallocate`, before destruction:

```cpp
{
    mem::Allocation<int, mem::GPA> alloc(10u);  // stateless: no allocator arg
    alloc[0] = 42;                              // bounds-asserted operator[]
    // ... use alloc.view() ...
}  // ~Allocation deallocates the raw storage (no destroy_at)

// RecordingAllocator is the stateful test double in Core.Allocator.Tests.cpp:
mem::Allocation<int, RecordingAllocator> alloc(4u, backend);  // pass it
alloc.resize(backend, 8u);
alloc.deallocateAssumeNotEmpty(backend);  // dtor then asserts empty
```

Key methods: `isValid()`, `data()` / `count()` / `size_bytes()`,
`operator[]`, `view()` (span, asserts valid), `owns(ptr, size)` (overlap
test), `allocate(al, n)` / `allocate(n)` (stateless only),
`resize(al, n)` / `relocate(al, n)`, `deallocate(al)` /
`deallocateAssumeNotEmpty(al)`, `create(al, args...)` + `destroy(al)` for
non-trivial `T`. **`discard()` returns `std::allocation_result<T*>`** and
empties the handle — the memory is intentionally leaked, never freed:

```cpp
mem::Allocation<Widget, mem::GPA> walloc;
walloc.create(42);   // allocate + construct_at
walloc.destroy();    // destroy_at + deallocate
```

Stateless allocators (`GPA`, `OS`, `HugePage`, `SmallPage`, `ScratchPad`)
allow the no-allocator-argument forms; stateful ones require the allocator
reference on every call.

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

mem::STL<char, mem::ScratchPad> sp_alloc;  // TLS arena; same-thread use (§2)
std::basic_string<char, std::char_traits<char>, decltype(sp_alloc)> s(sp_alloc);
```

`allocate(n)` forwards to `allocate<T>(n)`; `deallocate(ptr, n)` to
`deallocate<T>(ptr, n)`; `allocate_at_least` is also provided.

---

## 14. Poison API

Poisoning is **debug-gated**: `PPR_ENABLE_MEMORY_POISONING` equals
`PPR_ENABLE_DEBUG`; outside it the calls compile to no-op `constexpr`s, and
under `PPR_ENABLE_SANITIZER_ADDRESS` they drive the ASAN runtime instead of
flooding. Never route genuine OS page mappings (`pageAlloc` / `pageDecommit`
/ `pageFree` / `pageOfferToOS`) through poisoning — those pages may be
decommitted or read-only; heap-backed page-aligned blocks (arenas, slabs,
pools) are always safe. Callers skip OS mappings themselves.

| Function | Debug pattern | Canonical call site |
|---|---|---|
| `poisonReserved(ptr, size)` | `0xAA…` | Reserving not-yet-usable memory (InSitu ctor, container reserve/reset). |
| `unpoisonUninitialized(ptr, size)` | `0xCC…` | Allocated memory the caller will write, before returning it. |
| `poisonDestroyed(ptr, size)` | `0xDD…` | Freed/destructed memory, before returning it. |
| `annotateContiguousContainer(beg, end, old_mid, new_mid)` | mixed | Live-prefix boundary moves (vector-like growth/shrink). |

Rounding is an **8-byte granule**: debug flooding aligns inward to `u64`
(high nibble holds the pattern, low nibble is salted); ASAN rounds
reserve/unpoison *outward* to 8 and `poisonDestroyed` *inward*, leaving the
edges addressable. Typed overloads take `(T*, n)`; range overloads take
spans; `annotateDoubleEndedContiguousContainer` and the
`annotateEmpty…` helpers cover deque-style and fully-empty containers;
`UnpoisonUninitializedIterator` unpoisons through iterator-based
construction. The built-in allocators already poison every path — custom
allocators must follow the reserve → unpoison-on-alloc →
poison-on-free sequence.

---

## 15. safe_ptr Usage

`safe_ptr<T>` observes an object owned elsewhere (it requires a real owner:
`unique_ptr`, value member, service, or allocation handle). Debug builds
track referencers and assert liveness; release builds are raw pointers at
zero overhead — identical API in both, so never branch call sites per config:

```cpp
class MyNode : public pP::safe_object {
    int value;
public:
    explicit MyNode(int v) : value(v) {}
    int get() const { return value; }
};

void example() {
    auto owner = std::make_unique<MyNode>(42);  // the owner
    pP::safe_ptr<MyNode> ptr(owner.get());      // observer only
    pP::safe_ptr<MyNode> copy = ptr;

    int v = ptr->get();
    if (ptr == copy) { /* same object */ }

    pP::safe_ptr<safe_object> base = std::move(ptr).upcast<safe_object>();
    // Debug moves clear their source for diagnosability; release moves are
    // bit-copies — never rely on moved-from state portably.
    copy = nullptr;
    base = nullptr;  // observers detach before `owner` destroys MyNode
}
```

Key operations: implicit construction (incl. `unique_ptr`-observing),
`operator->` / `operator*`, `get()` (no assert), explicit `operator bool`,
`isValid()`, `upcast<BaseT>() &&` (rvalue only), and
`checked_cast<DerivedT>(safe)` downcasts. `safe_object` destruction asserts
zero referencers in debug; moves/copies require the source to be unobserved.
`details::relocatable<safe_ptr<T>>` is `false_type` in debug (never
mem-copied) and `true_type` in release.

---

## 16. Constraints and Best Practices

1. **RAII over manual**: prefer `Allocation<T, AllocatorT>` or `ScopedArena`
   over manual allocate/deallocate pairs.
2. **LIFO discipline**: `Slab`, `Arena`, `ScratchPad` resize/deallocate only
   the most recent allocation. Use `watermark()`/`restore()` for batch free;
   never rely on per-object deallocation, and never expect `restore` to run
   destructors.
3. **Thread safety**:
   - `GPA`, `OS`: thread-safe (global heap / OS pages).
   - `HugePage`, `SmallPage`: thread-safe via TLS cache + locked global pool.
   - `Pooling`, `HintedPooling`: thread-safe for concurrent
     allocate/deallocate (lock-free hot path, mutex growth); never concurrent
     with destruction.
   - `Arena`, `Slab`, `InSituSlab`, `InSitu`, `LocalCache`: **not
     thread-safe** — single-threaded (or TLS-confined) use only.
   - `ScratchPad`: per-thread — safe to call anywhere, bound to the thread
     (§2).
4. **Poison all paths**: every `allocateRaw` unpoisons what it returns; every
   `deallocateRaw` poisons what it takes back; every reserve/reset poisons the
   freed region.
5. **Stateless vs stateful**:
   - Stateless (empty, `use_inplace_v`, default-construct anywhere): `GPA`,
     `OS`, `HugePage`, `SmallPage`, `ScratchPad`, `PMR`.
   - Stateful (owned inline storage, stored by reference unless wrapped in
     `Inplace<>`): `Arena<>`, `Slab`, `InSituSlab`, `InSitu`, `LocalCache`,
     `Pooling`, `HintedPooling`.
6. **Block size alignment**: for `Pooling`/`LocalCache`,
   `alignForward(alignment, block_size_v) == block_size_v` is asserted —
   alignment must fit inside the block.
7. **No exceptions in deallocate**: every `deallocateRaw` is `noexcept`;
   fallible `allocateRaw` returns `{nullptr, 0}` — except `Slab` (throws)
   and `Pooling` (terminates), per §5.
8. **`checked_cast` / `safe_narrowing`** for `size_t` → `u32` conversions in
   arena offsets and block counts; round-trip fidelity is asserted.
9. **Contiguous container annotation** is required for any container managing
   a live prefix in a larger buffer: call
   `annotateContiguousContainer(beg, end, old_mid, new_mid)` every time the
   boundary moves.

Implementation: `Core.Memory / .Arena / .Allocator / .Poison / .Pointer`
partitions; tests `Core.Allocator / .Memory.Slab / .Memory.Arena /
.Memory.PagePool / .Poisoning / .SafePtr / .Buffer.Tests.cpp`.
