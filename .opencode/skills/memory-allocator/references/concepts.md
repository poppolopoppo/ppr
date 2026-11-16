# Allocator concepts

Read the [entry skill](../SKILL.md) for the per-API failure table, ownership,
TLS, arena, and teardown constraints before using these declarations.

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
