---
name: code-reviewer
description: >
  Reviews local git modifications against AGENTS.md conventions,
  modern C++ best practices for real-time applications (games/engines),
  and AI prompt engineering principles. Uses CLion MCP tools for
  index-backed evidence gathering (search_symbol, search_text, read_file,
  get_file_problems) and gates completion on zero remaining IDE errors/
  warnings on changed files (fixed or oracle-approved-suppressed).
  Use this skill whenever the user says "review my changes", "check my
  code", "audit my changes", "inspect the diff", "analyze the code",
  "does this follow the conventions", or "code review please".
---

# Code Reviewer

Analyze unstaged and staged changes in tracked files, then produce a
structured review organized by zone (engine core, game, tests, build)
across ten dimensions.

## Contract

This skill performs static code review across 11 dimensions of C++ quality
and engine conventions. It does **not** edit code inline, auto-fix issues
itself, or modify any files. Its output is a validated, reconciled report
grouped by zone and severity. All findings undergo mandatory per-item
validation against actual source code before presentation. The skill is
triggered by the orchestrator on commands such as "review", "check my code",
or "audit changes". The orchestrator drives the skill; diff retrieval is
delegated to `@explorer`, dimension reviews and per-finding validation run
as background `oracle` subagents, and the main lane only aggregates
validated results. Remediation of any finding (human-found or IDE-found)
happens exclusively through delegated subagents (`@fixer` for bounded edits,
`@oracle` for false-positive adjudication); the review is not complete
until every Error/Warning on changed files is either fixed or
oracle-approved-suppressed.

## Subagent routing

| Step | Delegate to | Why |
|------|-------------|-----|
| Diff context retrieval (`git diff HEAD`, `git diff --cached`, `git log`) | `@explorer` | Isolated read-only shell; keeps main lane free |
| Changed-file enumeration | `@explorer` | `clion_git_status` + `clion_get_repositories` for project-aware listing |
| Zone classification of changed files | orchestrator | Cheap; needs the full diff context |
| Dimension reviews (Step 3) | background `oracle` subagent per dimension | Parallelizable; each reviewer loads this skill and uses CLion MCP for evidence |
| IDE inspection sweep (Step 3.5) | orchestrator (main flow, `clion` MCP) | `clion_get_file_problems` per changed file; produces `[IDE]`-tagged findings |
| Per-finding validation (Step 4) | background `oracle` subagent per finding | Mandatory parallel fact-check against actual source; `[IDE]` findings exempt |
| Fix application | `@fixer` | Bounded edits via `clion_apply_patch` / `clion_create_new_file` |
| False-positive adjudication | `@oracle` | Confirms or refutes `@fixer` FP claims on warnings |
| Verdict reconciliation + report (Steps 5–6) | orchestrator | Aggregation; only validated findings are presented |

## OMO feature wiring

- **Per-agent `skills`/`mcps` allow-lists** — orchestrator has `skills: ["*"]`
  and `mcps: ["*","!context7"]`; `@oracle` needs `skills: ["code-reviewer",
  "simplify"]` (explicit grant in `~/.config/opencode/oh-my-opencode-slim.json`
  or project-local override — `code-reviewer` is not in `oracle`'s default
  allow-list); `@oracle` subagents also benefit from `clion` MCP search tools
  for evidence gathering; `@explorer` stays `skills: []` (read-only recon
  only, no skill activation).
- **Background orchestration** — launch the dimension reviews (Step 3) as
  parallel background `oracle` subagents, then the per-finding validation
  (Step 4) as one background subagent per finding; reconcile all verdicts on
  the Background Job Board before the summary is presented.
- **Session reuse** — reuse one read-only `@explorer` session to cache the
  diff context; re-run dimension/validation subagents only on newly added or
  changed files (invalidate a session when its `(agent-type, target area,
  file-glob)` key no longer matches the current diff).
- **`orchestratorPrompt` routing** — trigger on "review", "check my code",
  "audit my changes", "inspect my code", "analyze the code", "does this
  follow the conventions", or "code review please".

## Review Process

### Step 1 — Gather context

```bash
git diff HEAD                  # all unstaged + staged changes
git diff --cached              # staged-only
git log --oneline -8           # recent history for tone reference
```

For project-aware changed-file enumeration, prefer:
```
clion_get_repositories(projectPath="E:/Code/ppr")
clion_git_status(includeUntracked=true, projectPath="E:/Code/ppr")
```

Load `AGENTS.md`. Follow any `@include` references found in touched files.

**Scope**: only files tracked by git. Untracked files, `build/`, and
vcpkg install paths are excluded automatically.

### Step 2 — Classify each changed file by zone

| Zone | Path | Review depth |
|------|------|-------------|
| Engine code | `lib/*` | Full — all 11 dimensions |
| Game code | `game/*` | Full — all 11 dimensions |
| Tests | `*Tests*`, `*test*`, `*Test*` | Subset: 1, 3, 8, 9, 10, 11 |
| Build system | `CMakeLists.txt`, `cmake/*.cmake` | Build correctness only |
| Third-party wrappers | `cmake/external/*.cmake` | Minimal — version pin, no engine patches |
| Config / docs | `*.md`, `*.json`, `.gitignore` | Skip |

### Step 3 — Review across all 11 dimensions

For each file in the diff, apply the relevant checklists below.

**Evidence tooling:** reviewers cite via CLion MCP index-backed tools rather
than raw grep/read — this directly attacks the documented wrong-line-number
and false-positive failure mode. Use:
- `clion_search_symbol(q=..., projectPath="E:/Code/ppr")` for definitions/usages
- `clion_search_text(q=..., paths=[...], projectPath="E:/Code/ppr")` for literal evidence (forbidden casts, raw loops, `PPR_ASSERT` misuse in tests)
- `clion_search_regex(q=..., paths=[...], projectPath="E:/Code/ppr")` for pattern evidence
- `clion_read_file(file_path=..., offset=..., limit=..., projectPath="E:/Code/ppr")` for targeted reads (matches AGENTS.md targeted-reads rule)
- `clion_list_directory_tree(directoryPath=..., maxDepth=..., projectPath="E:/Code/ppr")` for module-layout checks (Dimension 7)
- `clion_get_compiler_info(filePath=..., projectPath="E:/Code/ppr")` for module flags / standard verification (Dimensions 1, 7)

See `clion-tools` SKILL.md §1 for exact signatures.

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

### Dimension 2 — Template complexity

- Concept definitions are concise — one `requires` clause, not nested
- Template parameter count ≤ 3 (flag deeply parameterized types)
- `static_assert` with clear messages for constraint violations
- No `auto` template parameters where a constrained concept would do
- No recursive template instantiation beyond reasonable depth

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

### Dimension 5 — Threading model

- Shared mutable state protected by atomics or explicit synchronization
- `thread_local` variables are POD or have trivial destructors (MSVC module issue)
- Lock-free algorithms use explicit `memory_order`, not `seq_cst` everywhere
- No data races detectable by thread sanitizer
- Per-thread caching (`LocalCache`) handles ownership and lifetime correctly
- Spin loops contain a compiler barrier (`PPR_COMPILER_READWRITE_BARRIER`)

### Dimension 6 — Exception safety & noexcept

- All engine core functions marked `noexcept`
- `noexcept` not used as "documentation" where code could actually throw (must be provably non-throwing)
- Move constructors, move assignment operators, and `swap()` must be `noexcept`
- Destructors are implicitly `noexcept` — flag any that could actually throw
- Non-trivial destructors explicitly marked `noexcept` for clarity
- No throwing from destructors during stack unwinding (calls `std::terminate`)
- Exceptions not used for control flow in engine layer
- `PPR_ASSERT` for invariants (compiles away in release), `PPR_VERIFY` for side effects, `PPR_ENSURE` for post-conditions
- **Test files only:** assert with `PPR_TEST_ASSERT` (from `"pP/UnitTest.h"`, functional in release) — flag any `PPR_ASSERT`/`PPR_VERIFY` inside `PPR_UNIT_TEST` bodies (they compile to `[[assume]]` in release and the test would silently pass)
- `std::nothrow` used with `operator new` in `GPA::allocateRaw`
- **Exception safety guarantees:** each function's guarantee is deliberate:
  - **No‑throw:** `noexcept` functions provide the no‑throw guarantee
  - **Strong:** state changes are committed only after all operations succeed (commit‐or‐rollback via RAII or scope guard)
  - **Basic:** no resource leaks on exception; invariants remain valid
  - No functions leave objects in an indeterminate state on exception
- RAII wrappers used for all resource ownership to prevent leaks during unwinding
- Exception-neutral code propagates exceptions correctly through wrappers (`std::nested_exception` or transparent forwarding)
- No throwing in hot paths or non‑critical paths where error codes suffice
- No throwing in constructors of types allocated in bulk (prefer two‑phase init or factory functions)
- Code paths that call `std::terminate` are guarded by a clear precondition check

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

### Dimension 9 — Design decisions

- Every exported symbol has a clear rationale
- No commented-out code
- Namespace choice reflects ownership tier (`pP::mem::` vs `pP::details::`)
- Public API symmetry (`allocate` / `deallocate`, `acquire` / `release`)
- `static_assert` with error messages validate design assumptions
- ABI impact called out for exported class layout changes
- No raw loops — prefer algorithms and ranges
- No comments on obvious code — comments should be exceptional, only for genuinely surprising or non-obvious code that cannot be clarified through naming or structure alone
- `constexpr` everywhere — prefer compile-time evaluation
- `[[nodiscard]]` on functions returning values
- `PPR_FORCE_INLINE` on hot-path functions
- Macros are forbidden outside `Macros.h`

### Dimension 10 — safe_ptr lifetime correctness

- All `safe_ptr` instances pointing to an object must be released before the object is destroyed (all copies set to `nullptr` or go out of scope)
- `safe_ptr` is NOT a shared ownership pointer — it is a debug-only lifetime checker; treat as `T*` for ownership semantics
- `safe_ptr` acquired from `unique_ptr::get()` or `safe_ptr` `get()` requires the caller to ensure the source outlives the copy
- Local variables holding `safe_ptr` to a service-owned object must be non-`const` and nulled before removing the object from the service
- When storing `safe_ptr` as a class member, document the lifetime contract (who owns the source and how destruction ordering is enforced)
- Nested function calls that create temporary `safe_ptr` copies are safe as long as the pointed-to object lives until the return of the outermost call
- `addGamepadPlayer` / `getOrCreateKeyboardPlayer` return `safe_ptr` that must be released before the corresponding `removePlayer` call

### Dimension 11 — Function design (honesty & abstraction)

Per AGENTS.md §Function Design Principles:

- No hidden global/environment access outside the signature (global PRNG,
  clock reads, static mutable locals, service lookups deep in leaf logic)
- Dishonesty injected at the lowest possible level — I/O, logging side
  effects, RNG seeding belong at call sites/top-level callers, not core logic
- No wallet anti-pattern: functions take the fields they need, not whole
  aggregates holding them
- Parameters not over-constrained: `span<T>` / views accepted where only
  iteration or contiguous access is needed; concrete containers not demanded
- Ordering/invariant contracts encoded in types where they exist (receipt
  parameters, invariant wrapper types like `Numeric<T, TagT>`)
- One abstraction level per function body: no section-labeling comments, no
  non-trivial raw-loop bodies, no zoom-in/zoom-out within a single function
- Framework hooks (`Application::initialize/update/render`, `main`, input/
  window callbacks) stay thin delegators
- Ad-hoc data structures maintained manually across sibling functions are
  flagged for encapsulation

---

### Step 3.5 — IDE inspection sweep

After dimension reviews, run a CLion inspection sweep on every changed file.
This catches problems the dimension checklists miss (unused includes,
deprecated APIs, module-partition naming, etc.) and feeds the resolution gate.

**Procedure:**
1. Enumerate changed files (same list as Step 2).
2. Batch-call `clion_get_file_problems(filePath=<f>, errorsOnly=false,
   projectPath="E:/Code/ppr")` per file — ~8 concurrent calls per message.
   See `clion-tools` SKILL.md §6 for the exact signature.
3. Each returned problem becomes a finding tagged **`[IDE]`** with
   provenance (inspection ID, severity, file:line). These findings enter
   Steps 4–6 like any other finding but are **exempt from Step 4's
   per-finding subagent fact-check** — IDE output is ground truth.

---

### Step 4 — Validate each finding (mandatory parallel fact-check)

After the global review pass (Step 3) and IDE inspection sweep (Step 3.5)
produce candidate findings, EVERY non-`[IDE]` finding must be individually
validated against the actual source BEFORE any result is presented to the
user. `[IDE]` findings are exempt (IDE = ground truth). This step is
mandatory — never skip it and never present unvalidated findings.

**Procedure:**
1. Collect the complete list of candidate findings (all zones, all severities)
   from Steps 3 and 3.5, excluding `[IDE]`-tagged findings.
2. Spawn ONE parallel subagent per finding (background, `oracle` type, each
   loading this `code-reviewer` skill). Each subagent receives only its single
   finding and is instructed to:
   - Read the ACTUAL cited source file(s) at the cited location — **never the
     full diff**. (Reading the whole diff previously caused context exhaustion
     and wrong line numbers.)
   - Trace the real code path to confirm or refute the claim.
   - Return `VERDICT: Confirmed | Partially correct | Incorrect | Cannot
     determine`, with `Evidence` (file:line + key snippet) and an `Assessment`
     of whether the cited severity is over/under-stated.
3. Reconcile all verdicts. Drop or downgrade any finding rated `Incorrect`;
   keep `Partially correct` only with its stated nuance. Present ONLY the
   validated, reconciled results to the user, grouped by severity, and flag
   any finding corroborated by ≥2 reviewers as high confidence.

**Why:** The global pass alone produced false positives — including two
fabricated ❌ Errors and several wrong line numbers. Per-item validation
against source catches misreadings before they reach the user.

---

### Step 5 — Resolution gate (delegated, not inline)

Before the final report issues, every Error and Warning finding (human-found
or `[IDE]`-tagged) must be resolved. The reviewer never edits inline;
remediation happens exclusively through delegation.

**Loop** (max 3 rounds):
1. Dispatch `@fixer` per finding/batch to fix (bounded edits via
   `clion_apply_patch` / `clion_create_new_file`).
2. Re-run `clion_get_file_problems` on touched files only.
3. **False-positive path (warnings only — errors are never suppressible):**
   if `@fixer` reports a warning as a false positive, spawn `@oracle` to
   adjudicate against the actual source.
   - `@oracle` confirms false positive → `@fixer` inserts a suppression
     comment (`//noinspection <InspectionId>` / `// NOLINT`) citing the
     inspection ID and rationale.
   - `@oracle` refutes → dispatch a NEW `@fixer` with explicit confirmation
     that the issue is real.
4. Suppressed warnings count as resolved but are listed in the report with
   their justification.

**Exit:** zero errors AND zero warnings (fixed or oracle-approved-suppressed)
→ gate green. Leftovers after 3 rounds → blocking ❌ entries in the final
report.

**Suppression-comment policy:** suppression comments are exceptional and
reconcile with Dimension 9's no-comments rule by requiring oracle
confirmation + a cited rationale. Errors are never suppressible.

---

### Step 6 — Generate per-zone report

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

### Step 7 — Summary table

```
## Summary

| Zone | ❌ Error | ⚠️ Warning | 💡 Suggestion | IDE (fixed/suppressed/open) |
|------|---------|-----------|--------------|----------------------------|
| lib/ | 2 | 5 | 8 | 3 / 1 / 0 |
| game/ | 0 | 1 | 3 | 0 / 0 / 0 |
| cmake/ | 0 | 0 | 1 | 0 / 0 / 0 |

**Suppressed (with rationale):**
- `lib/engine/foo.h:42` — InspectionId `UnusedInclude` — rationale: ...

**Most critical**: Memory leak in lib/Core.Foo.cppm:156 (Error)
```

---

## Constraints

- Review only files tracked by git; exclude `build/`, vcpkg install paths,
  and `out/` (mirror `.gitignore`).
- Every finding must cite a specific AGENTS.md rule or named C++ best
  practice; never present unvalidated findings (Step 4 is mandatory).
- The reviewer never edits code inline; remediation happens exclusively via
  delegated subagents (`@fixer` for fixes, `@oracle` for FP adjudication).
- Test files must use `PPR_TEST_ASSERT`; flag any `PPR_ASSERT`/`PPR_VERIFY`
  inside `PPR_UNIT_TEST` bodies.
- Suppression comments are exceptional, require oracle confirmation, and
  must cite the inspection ID + rationale. Errors are never suppressible.
- Resolution gate: max 3 rounds; leftovers are blocking ❌.
