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
- State boundary failure semantics from the table in §5: `{nullptr, 0}` for
  fallible `noexcept` allocation, the documented `Slab` exception, and
  no-throw deallocation.
- Prove shutdown order: stop and quiesce users, detach callbacks and release
  observers, destroy live objects, then reset/free backing storage; no callback
  may retain storage past this point.
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

Use [AGENTS.md's ownership, lifetime, and teardown contract](../../../AGENTS.md#ownership-lifetime-and-teardown)
for owners, observers, `safe_ptr`, and callback lifetimes. Only the owner
releases storage. Allocator-specific consequences:

- `Allocation` owns raw storage until `deallocate`/`destroy`/`discard`;
  arenas and slabs own their backing slabs once pushed, and pool objects own
  provisioned pools until destruction. The fixed-buffer `Slab` wrapper itself
  borrows its caller's bytes (see the family reference).
- Spans/views over allocations and allocator references held inside
  `Allocator<>` wrappers observe only; they do not extend a lifetime.
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
| Composable: try A then B | `Fallback<A, B>` | [§7](references/allocator-families.md#7-composite-allocator-patterns) |
| Composable: small → A, large → B | `Threshold<Under, N, Above>` | [§7](references/allocator-families.md#7-composite-allocator-patterns) |
| Pool + TLS cache | `Pooling` / `LocalCache` | [§7](references/allocator-families.md#7-composite-allocator-patterns) |
| std:: container adapter | `STL<T, A>` | `STL<T, A>{allocator}` wraps any `TAllocator` |

### Required task-selected references

Read the matching detail before implementing or reviewing that task; read all
matching rows for compositions. Original section numbers are retained.

| Task | Required detail |
|---|---|
| Implement/check allocator concepts, ownership routing, resize, block or arena API | [Concept declarations (§4)](references/concepts.md) |
| Slab, InSituSlab, Arena, ScopedArena, ScratchPad | [Arena patterns (§6)](references/allocator-families.md#6-arena-patterns) |
| Fallback, Threshold, InSitu, LocalCache, Pooling, HintedPooling, Static | [Composite families (§7)](references/allocator-families.md#7-composite-allocator-patterns) |
| Typed allocation, create/destroy, duplication, relocation traits | [AllocatorTraits (§8)](references/allocator-families.md#8-allocatortraits-usage) |
| Value/reference embedding, Inplace, forceRef, materialize | [Embedding (§10)](references/allocator-families.md#10-inplace--embedding-allocators-by-value) |
| Runtime type erasure and referenced allocator lifetime | [PMR (§11)](references/allocator-families.md#11-pmr--runtime-polymorphic-allocator) |
| Arena placement construction and constructor-failure release | [Placement new/delete (§12)](references/allocator-families.md#12-custom-placement-newdelete-for-arena) |
| Standard containers and propagation behavior | [STL adapter (§13)](references/allocator-families.md#13-stl-adapter) |
| Allocation handle, poison/ASAN, safe_ptr mechanics | Entry §9, §14, §15 below |

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

## Arena lifetime invariants

`Slab`, `Arena`, and `ScratchPad` resize/deallocate only at LIFO top. Destroy
live objects before reset/restore: rewinding storage runs no destructors.
`Arena` restores by popping intermediate slabs and terminates on a foreign
watermark; its destructor resets then releases every slab. Moves transfer
the chain and leave the source null. `ScopedArena` moves transfer the restore
boundary and leave the old guard inert; debug guards assert correct nesting.
These mechanics and examples are in [§6](references/allocator-families.md#6-arena-patterns).

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

Apply [AGENTS.md's ownership contract](../../../AGENTS.md#ownership-lifetime-and-teardown).
Debug builds track referencers and assert liveness; release builds are raw
pointers at zero overhead — identical API in both, so never branch call sites
per config:

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
