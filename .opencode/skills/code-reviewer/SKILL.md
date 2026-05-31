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
across nine dimensions.

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
| Engine code | `lib/*` | Full — all 9 dimensions |
| Game code | `game/*` | Full — all 9 dimensions |
| Tests | `*Tests*`, `*test*`, `*Test*` | Subset: 1, 3, 8, 9 |
| Build system | `CMakeLists.txt`, `cmake/*.cmake` | Build correctness only |
| Third-party wrappers | `cmake/external/*.cmake` | Minimal — version pin, no engine patches |
| Config / docs | `*.md`, `*.json`, `.gitignore` | Skip |

---

## Step 3 — Review across all 9 dimensions

For each file in the diff, apply the relevant checklists below.

---

### Dimension 1 — C++ standard usage

- Uses `import std;` not `#include <...>` in module files
- Uses `consteval` where compile-time is mandatory (string literals, hash mixing)
- Uses deducing `this` for const/mutable overloads where appropriate
- Prefers `std::expected` / `std::optional` over out-params for fallible results
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

---

### Dimension 4 — Cache behavior

- Hot / cold data separation (frequently accessed fields grouped together)
- Struct-of-arrays preferred over array-of-structs for container internals
- `alignas(hal::cacheline_size_v)` on shared mutable data to prevent false sharing
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

### Dimension 6 — Exception usage

- All engine core functions marked `noexcept`
- `noexcept` not used as "documentation" where code could actually throw
- Exceptions not used for control flow in engine layer
- `PPR_ASSERT` for invariants (compiles away in release), `PPR_VERIFY` for side effects, `PPR_ENSURE` for post-conditions
- `std::nothrow` used with `operator new` in `GPA::allocateRaw`

---

### Dimension 7 — Compile-time impact

- Module partitions minimize what is recompiled on change
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
