---
name: code-reviewer
description: >
  Reviews local git modifications against AGENTS.md conventions,
  modern C++ best practices for real-time applications (games/engines),
  and AI prompt engineering principles. Use this skill whenever the
  user says "review my changes", "check my code", "does this follow
  the conventions", or "code review please".
---

# Code Reviewer

Analyze unstaged and staged changes in tracked files, then produce a
structured review organized by zone (engine core, game, tests, build)
across ten dimensions.

---

## Step 1 — Gather context

```bash
git diff HEAD                  # all unstaged + staged changes
git diff --cached              # staged-only
git log --oneline -8           # recent history for tone reference
```

Load `AGENTS.md`. Follow any `@include` references found in touched files.

**Scope**: only files tracked by git. Untracked files, `build/`, and
vcpkg install paths are excluded automatically.

---

## Step 2 — Classify each changed file by zone

| Zone | Path | Review depth |
|------|------|-------------|
| Engine code | `lib/*` | Full — all 10 dimensions |
| Game code | `game/*` | Full — all 10 dimensions |
| Tests | `*Tests*`, `*test*`, `*Test*` | Subset: 1, 3, 8, 9, 10 |
| Build system | `CMakeLists.txt`, `cmake/*.cmake` | Build correctness only |
| Third-party wrappers | `cmake/external/*.cmake` | Minimal — version pin, no engine patches |
| Config / docs | `*.md`, `*.json`, `.gitignore` | Skip |

---

## Step 3 — Review across all 10 dimensions

For each file in the diff, apply the relevant checklists below.

---

### Dimension 1 — C++ standard usage

- Uses `import std;` not `#include <...>` in module files
- Uses `consteval` where compile-time is mandatory (string literals, hash mixing)
- Uses deducing `this` for const/mutable overloads where appropriate
- Prefers `std::expected` / `std::optional` over out-params for fallible results
- `.value()` called only after `.has_value()` guard (or use `value_or()`)
- `.and_then()`, `.or_else()`, `.transform()` preferred over manual `if/else` branches for chaining
- `std::expected::error()` accessed only when `!has_value()` is proven
- `std::optional` for "maybe" values; `std::expected` for error reporting (with distinct error type)
- No `std::optional<T&>` — use `T*` or `std::optional<std::reference_wrapper<T>>`
- Uses `std::span<T>` not `T* + size_t` for array views
- Uses `std::bit_cast` not `reinterpret_cast` / `memcpy` for type punning
- C-style casts are forbidden; uses `checked_cast<>` for narrowing/widening
- Prefers `std::size_t` / `std::ptrdiff_t` for sizes and indices, not `int`
- Uses C++20 concepts for template validation, not `std::enable_if` / SFINAE

---

### Dimension 2 — Template complexity

- Concept definitions are concise — one `requires` clause, not nested
- Template parameter count ≤ 3 (flag deeply parameterized types)
- `static_assert` with clear messages for constraint violations
- No `auto` template parameters where a constrained concept would do
- No recursive template instantiation beyond reasonable depth

---

### Dimension 3 — Memory patterns

- Hot-path code uses pool / arena allocators, not `operator new` per-frame
- `poisonAllocated` / `poisonDestroyed` called correctly in custom allocators
- No raw `new` / `delete` outside allocator implementations
- `Allocation<T>` RAII wrapper used instead of manual pairings
- Every `allocateRaw` is paired with `deallocateRaw`
- Alignment respected: `alignof_v<T>` passed through correctly
- Appropriate allocator tier chosen (`GPA` / `OS` / `HugePage` / `SmallPage`)
- Relocatable types marked via `pP::details::relocatable<T>`
- **Member alignment & padding:**
  - Fields ordered from largest alignment to smallest to minimize padding
  - `alignas` used deliberately (not reflexively) with a documented rationale
  - No unnecessary `#pragma pack` — if required for wire format, `static_assert` the layout
  - `offsetof` verified where ABI-sensitive layout is assumed
  - Bit-fields checked for platform-dependent layout and storage-unit boundaries

---

### Dimension 4 — Cache behavior

- Hot / cold data separation (frequently accessed fields grouped together)
- Struct-of-arrays preferred over array-of-structs for container internals
- `alignas(hal::cacheline_size_v)` on shared mutable data to prevent false sharing
- Read-mostly data separated from read-write data across cache-line boundaries
- Hot struct fits within a single cache line (total size ≤ `hal::cacheline_size_v`)
- Standalone mutable atomics / counters that are write-contended have `alignas(hal::cacheline_size_v)`
- Intentional cache-line isolation uses separator padding (`[[no_unique_address]]` + unused byte array)
- `thread_local` for thread-private data to avoid contention
- No pointer-chasing in hot paths (linked lists, deep indirection)
- Branch order favors hot path with `[[likely]]`

---

### Dimension 5 — Threading model

- Shared mutable state protected by atomics or explicit synchronization
- `thread_local` variables are POD or have trivial destructors (MSVC module issue)
- Lock-free algorithms use explicit `memory_order`, not `seq_cst` everywhere
- No data races detectable by thread sanitizer
- Per-thread caching (`LocalCache`) handles ownership and lifetime correctly
- Spin loops contain a compiler barrier (`PPR_COMPILER_READWRITE_BARRIER`)

---

### Dimension 6 — Exception safety & noexcept

- All engine core functions marked `noexcept`
- `noexcept` not used as "documentation" where code could actually throw (must be provably non-throwing)
- Move constructors, move assignment operators, and `swap()` must be `noexcept`
- Destructors are implicitly `noexcept` — flag any that could actually throw
- Non-trivial destructors explicitly marked `noexcept` for clarity
- No throwing from destructors during stack unwinding (calls `std::terminate`)
- Exceptions not used for control flow in engine layer
- `PPR_ASSERT` for invariants (compiles away in release), `PPR_VERIFY` for side effects, `PPR_ENSURE` for post-conditions
- `std::nothrow` used with `operator new` in `GPA::allocateRaw`
- **Exception safety guarantees:** each function's guarantee is deliberate:
  - **No‑throw:** `noexcept` functions provide the no‑throw guarantee
  - **Strong:** state changes are committed only after all operations succeed (commit‐or‑rollback via RAII or scope guard)
  - **Basic:** no resource leaks on exception; invariants remain valid
  - No functions leave objects in an indeterminate state on exception
- RAII wrappers used for all resource ownership to prevent leaks during unwinding
- Exception-neutral code propagates exceptions correctly through wrappers (`std::nested_exception` or transparent forwarding)
- No throwing in hot paths or non‑critical paths where error codes suffice
- No throwing in constructors of types allocated in bulk (prefer two‑phase init or factory functions)
- Code paths that call `std::terminate` are guarded by a clear precondition check

---

### Dimension 7 — Compile-time impact

- Module partitions minimize what is recompiled on change
- Module partition names (after `:`) match the source file name suffix (e.g. `:containers` → `Core.Containers.cppm`)
- Module partition hierarchy mirrors subdirectory layout (e.g. `lib/engine/core/strings/` → `engine.core:strings`)
- Every `.cppm` with non-trivial definitions has a corresponding `.cpp` implementation file
- No `export import` of entire partitions where a more selective `export { ... }` would suffice
- Module partitions imported by parent module only, not by unrelated consumers
- Template-heavy code isolated in dedicated `.cppm` partitions
- No unnecessary `import std;` in files that don't use standard types
- `.cppm` files kept minimal (declarations only), definitions in `.cpp`
- Unity build compatibility considered (`PPR_ANONYMIZE` usage)
- Avoid pulling large headers into module interfaces

---

### Dimension 8 — Undefined behavior

- `reinterpret_cast` is forbidden; use `std::bit_cast` for type-punning
- Signed integer overflow is UB — use `checked_cast<>` or saturating arithmetic
- Pointer arithmetic beyond array bounds is forbidden — use `std::span` or iterators
- Shifting by ≥ bit width of the type is UB — all shifts must be range-checked
- `std::memcpy` from uninitialized storage is UB — poison/zero before reading
- Dangling reference / iterator after container mutation is strictly flagged
- Violating strict aliasing rules is forbidden unless isolated and gated by `#if __has_attribute(may_alias)` with a documented rationale
- `std::unreachable()` / `PPR_ASSUME` only after a guard that proves the path is dead
- Any unavoidable UB must be:
  - Enclosed in a narrow, scoped block
  - Documented with why it is safe on the *target platform/compiler*
  - Gated by a compiler-specific macro (`#ifdef _MSC_VER`, `#if defined(__clang__)`, etc.)
  - Preceded by a `static_assert` or `PPR_ASSERT` validating the precondition

---

### Dimension 9 — Design decisions

- Every exported symbol has a clear rationale
- No commented-out code
- Namespace choice reflects ownership tier (`pP::mem::` vs `pP::details::`)
- Public API symmetry (`allocate` / `deallocate`, `acquire` / `release`)
- `static_assert` with error messages validate design assumptions
- ABI impact called out for exported class layout changes
- No raw loops — prefer algorithms and ranges
- No comments — code should be self-documenting
- `constexpr` everywhere — prefer compile-time evaluation
- `[[nodiscard]]` on functions returning values
- `PPR_FORCE_INLINE` on hot-path functions
- Macros are forbidden outside `Macros.h`

---

### Dimension 10 — safe_ptr lifetime correctness

- All `safe_ptr` instances pointing to an object must be released before the object is destroyed (all copies set to `nullptr` or go out of scope)
- `safe_ptr` is NOT a shared ownership pointer — it is a debug-only lifetime checker; treat as `T*` for ownership semantics
- `safe_ptr` acquired from `unique_ptr::get()` or `safe_ptr` `get()` requires the caller to ensure the source outlives the copy
- Local variables holding `safe_ptr` to a service-owned object must be non-`const` and nulled before removing the object from the service
- When storing `safe_ptr` as a class member, document the lifetime contract (who owns the source and how destruction ordering is enforced)
- Nested function calls that create temporary `safe_ptr` copies are safe as long as the pointed-to object lives until the return of the outermost call
- `addGamepadPlayer` / `getOrCreateKeyboardPlayer` return `safe_ptr` that must be released before the corresponding `removePlayer` call

---

## Step 4 — Generate per-zone report

Each finding uses this structure:

```
## Zone: lib — Core.Foo.cppm:42

### Severity: ⚠️ Warning

**Dimension**: Memory patterns

**Issue**: `allocateRaw` does not call `poisonAllocated` after allocation

**Current**:
```cpp
auto ptr = OS::allocateRaw(bytes, alignment);
return {ptr, bytes};
```

**Recommended**:
```cpp
auto [ptr, size] = OS::allocateRaw(bytes, alignment);
poisonAllocated(ptr, size);
return {ptr, size};
```

**Rationale**: AGENTS.md §6.2 — base allocators must poison on allocate
```

---

## Step 5 — Summary table

```
## Summary

| Zone | ❌ Error | ⚠️ Warning | 💡 Suggestion |
|------|---------|-----------|--------------|
| lib/ | 2 | 5 | 8 |
| game/ | 0 | 1 | 3 |
| cmake/ | 0 | 0 | 1 |

**Most critical**: Memory leak in lib/Core.Foo.cppm:156 (Error)
```

---

## Constraints

- Only files tracked by git are reviewed; `build/` and vcpkg paths excluded
- Third-party source (outside tracked paths) is never reviewed
- `cmake/external/*.cmake` checks only version pinning and that no engine code is patched
- Every finding must cite a specific rule from AGENTS.md or a named C++ best practice
- Engine code (`lib/`) findings take priority over `game/`
- For diffs with >10 files, ask user which zones to focus on
