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
  code", "audit my changes", "inspect the diff", "analyze the diff",
  "does this follow the conventions", or "code review please".
---

# Code Reviewer

Analyze unstaged and staged changes in tracked files, then produce a
structured review organized by zone (engine core, game, tests, build)
across ten dimensions.

## Contract

This skill performs static code review across 10 dimensions of C++ quality
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
oracle-approved-suppressed. Reviewers may additionally receive
orchestrator-derived *named suspects* as verification hints (see the Named
Suspects Protocol in Step 3) — they are inputs, never findings.

## Subagent routing

| Step | Delegate to | Why |
|------|-------------|-----|
| Diff context retrieval (`git diff HEAD`, `git diff --cached`, `git log`) | `@explorer` | Isolated read-only shell; keeps main lane free |
| Changed-file enumeration | `@explorer` | `clion_git_status` + `clion_get_repositories` for project-aware listing |
| Zone classification of changed files | orchestrator | Cheap; needs the full diff context |
| Dimension reviews (Step 3) | 4 background `oracle` subagents (grouped: Language & Formatting [Dims 1–3], Memory & Cache [Dims 4–5], Concurrency & Safety [Dims 6–8], Correctness & Design [Dims 9–10]) | Parallelizable; each reviewer loads this skill and runs its assigned checklists; findings tagged by original dimension number |
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
  (Step 4) as one background subagent per batch; reconcile all verdicts on
  the Background Job Board before the summary is presented. The IDE
  inspection sweep (Step 3.5) runs **concurrently with Step 3** since it
  depends only on the changed-file list from Step 1, not on dimension
  review results — this eliminates one sequential round-trip.
- **Session reuse** — the orchestrator SHOULD provide a cached `@explorer`
  session for diff retrieval if one was recently computed (same working
  tree, same HEAD). **Do NOT reuse dimension reviewer sessions across
  invocations** — the diff is the primary input and changes between
  reviews; dimension reviewers must always run fresh. Invalidate an
  `@explorer` session when its HEAD commit differs from the current diff's
  HEAD.
- **Large diffs** — for diffs spanning >10 changed files or >3 zones,
  consider delegating to `/deepwork` for phased remediation. Use `codemap`
  to understand module boundaries of changed files before launching
  dimension subagents.
- **`orchestratorPrompt` routing** — trigger on "review", "check my code",
  "audit my changes", "inspect my code", "analyze the code", "does this
  follow the conventions", or "code review please".

## Review Process

### Step 1 — Gather context

Delegate to `@explorer`: run `git diff HEAD`, `git diff --cached`,
`git log --oneline -8`, and `clion_git_status`/`clion_get_repositories`.
Request that `@explorer` return a **structured envelope** containing:
(1) the combined diff, (2) the list of changed tracked files, (3) a
per-zone file grouping using the zone classification rules from Step 2,
and (4) the recent commit log. The orchestrator validates the zone
classification and passes each zone's file list directly to the relevant
dimension reviewers — this eliminates redundant file enumeration across
the (now 4) dimension reviewers.

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

### Step 1.5 — Small-diff shortcut

Compute `git diff --numstat HEAD` and `git diff --name-only HEAD`. If
**all** of the following hold:

- total changed lines (additions + deletions) ≤ 50,
- changed-tracked file count ≤ 2,
- no changed file is a test (`*Tests*/` or `*Test*.cppm` / `*Test*.cpp`),
- no changed file is a module umbrella (filename matches `Core.cppm`,
  `Math.cppm`, `App.Application.cppm`, or any `.cppm` re-exporting
  multiple `import :…;` lines),

→ **shortcut** mode: skip the relevance-assessment heuristics and launch
all 4 dimension reviewers unconditionally. Emit the note
`Small-diff shortcut: <file_count> files, <line_count> total lines — full
dimension review` in the Step 6 preamble.

Otherwise → proceed to Step 2 (zone classification) and Step 3 (full
dimension review) as today.

**Why:** For trivial diffs, the cost of computing relevance heuristics
exceeds the savings. The shortcut is a gate on the *preamble*, not the
*reviewers* — when it fires, all 4 dimension reviewers still run, so
review coverage is preserved. Per-dimension skip heuristics are deferred
until empirical data from 50+ PRs motivates them.

---

### Step 2 — Classify each changed file by zone

| Zone | Path | Review depth |
|------|------|-------------|
| Engine code | `lib/*` | Full — all 10 dimensions |
| Game code | `game/*` | Full — all 10 dimensions |
| Tests | `*Tests*`, `*test*`, `*Test*` | Subset: 1, 4, 8, 9, 10 |
| Build system | `CMakeLists.txt`, `cmake/*.cmake` | Build correctness only |
| Third-party wrappers | `cmake/external/*.cmake` | Minimal — version pin, no engine patches |
| Config / docs | `*.md`, `*.json`, `.gitignore` | Skip |

---

### Step 3 — Review across all 10 dimensions

For each file in the diff, apply the relevant checklists below.

**Named Suspects Protocol (optional).** During recon (Steps 1–2) the
orchestrator MAY derive up to 12 named suspects from diff hunks, symbol
cross-references, and hand analysis of the structured envelope. Suspect
derivation is an orchestrator responsibility — `@explorer` returns evidence
only and never produces suspects. Each suspect specifies: id (`S<n>`), target
dimension(s), `file:symbol` or `file:line`, and a one-line hypothesis.
Suspects are attached under `SUSPECTS` to the matching reviewer prompt;
reviewers treat them as hints to verify, returning one verdict per suspect:
`Confirmed` (file:line + snippet) / `Refuted` (evidence) / `Cannot determine`.
A Confirmed suspect becomes a finding and follows the normal pipeline —
Step 4 fact-check still applies (no bypass). Refuted suspects are dropped.
`Cannot determine`: the orchestrator assigns a finding ID and re-dispatches
it as a single-finding Step 4 batch. Unverified suspects never appear in the
report as findings.

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

See also: AGENTS.md §Coding Standards (allowed macros, attributes,
`constexpr`/`noexcept`/`[[nodiscard]]` rules), `clion-tools` SKILL.md
(evidence gathering).

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

### Dimension 2 — Source formatting & conventions

See also: AGENTS.md §Source Formatting, `.clang-format` (auto-formatted
rules).

Check AGENTS.md §Source Formatting for all non-auto-formatted conventions
(section dividers, function prefix order, pointer/const style, branch
attributes, member order, comments, boolean negation, conditional
compilation, module file split). clang-format-enforced rules (indent,
braces, `*` spacing) are excluded.

Reminder: first divider after namespace has no blank line above;
subsequent dividers flanked by one blank line each side.

### Dimension 3 — Template complexity & compile-time impact

See also: `module-architect` SKILL.md (module partition naming, file
structure, CMake registration).

**Template complexity:**
- Concept definitions are concise — one `requires` clause, not nested
- Template parameter count ≤ 3 (flag deeply parameterized types)
- `static_assert` with clear messages for constraint violations
- No `auto` template parameters where a constrained concept would do
- No recursive template instantiation beyond reasonable depth

**Compile-time impact:**
- Module partitions minimize what is recompiled on change
- Module partition names (after `:`) match the source file name suffix
  (e.g. `:containers` → `Core.Containers.cppm`)
- Module partition hierarchy mirrors subdirectory layout (e.g.
  `lib/engine/core/strings/` → `engine.core:strings`)
- Every `.cppm` with non-trivial definitions has a corresponding `.cpp`
  implementation file
- No `export import` of entire partitions where a more selective
  `export { ... }` would suffice
- Module partitions imported by parent module only, not by unrelated
  consumers
- Template-heavy code isolated in dedicated `.cppm` partitions
- No unnecessary `import std;` in files that don't use standard types
- `.cppm` files kept minimal (declarations only), definitions in `.cpp`
- Unity build compatibility considered (`PPR_ANONYMIZE` usage)
- Avoid pulling large headers into module interfaces

### Dimension 4 — Memory patterns & safe_ptr lifetime

See also: `memory-allocator` SKILL.md (allocator selection, composition,
arena patterns, poison API, STL adapter, safe_ptr).

**Memory patterns:**
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

**safe_ptr lifetime correctness:**
- All `safe_ptr` instances pointing to an object must be released before
  the object is destroyed (all copies set to `nullptr` or go out of scope)
- `safe_ptr` is NOT a shared ownership pointer — it is a debug-only
  lifetime checker; treat as `T*` for ownership semantics
- `safe_ptr` acquired from `unique_ptr::get()` or `safe_ptr` `get()`
  requires the caller to ensure the source outlives the copy
- Local variables holding `safe_ptr` to a service-owned object must be
  non-`const` and nulled before removing the object from the service
- When storing `safe_ptr` as a class member, document the lifetime
  contract (who owns the source and how destruction ordering is enforced)
- Nested function calls that create temporary `safe_ptr` copies are safe
  as long as the pointed-to object lives until the return of the
  outermost call
- `addGamepadPlayer` / `getOrCreateKeyboardPlayer` return `safe_ptr` that
  must be released before the corresponding `removePlayer` call

### Dimension 5 — Cache behavior

See also: `hal::cacheline_size_v` is defined in the HAL platform headers
(`lib/engine/core/hal/<platform>/`).

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

### Dimension 6 — Threading model

See also: `concurrency-patterns` SKILL.md (RawChannel MPSC, IEvent/Signal,
IContext tree, thread safety, HAL I/O integration).

- Shared mutable state protected by atomics or explicit synchronization
- `thread_local` variables are POD or have trivial destructors (MSVC module issue)
- Lock-free algorithms use explicit `memory_order`, not `seq_cst` everywhere
- No data races detectable by thread sanitizer
- Per-thread caching (`LocalCache`) handles ownership and lifetime correctly
- Spin loops contain a compiler barrier (`PPR_COMPILER_READWRITE_BARRIER`)

### Dimension 7 — Exception safety & noexcept

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

### Dimension 9 — Design decisions & function design

See also: AGENTS.md §Function Design Principles.

**Design decisions:**
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

**Function design (honesty & abstraction):**
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

### Dimension 10 — Math & shader conventions

See also: AGENTS.md §Matrix Layout Conventions (authoritative handedness,
coordinate-system, and projection values, including shader session setup).
Do not hardcode convention values here; the orchestrator injects the current
AGENTS.md section text into reviewer prompts at runtime.

- Coordinate/handedness conventions hold at every boundary: world → view →
  NDC → framebuffer mappings agree with the documented source of truth across
  host math, GPU uploads, shader evaluation, and readback paths
- Matrix representation matches the documented storage/interpretation:
  session-level matrix layout mode, vector–matrix multiply order, translation
  placement, view–projection composition order, untransposed uploads, no
  undocumented backend-specific projections or axis flips
- Host↔shader data contracts: constant-buffer layouts match host structs
  (`static_assert` on size/alignment), binding slots consistent, GPU upload
  caches invalidate on any input change (identity/version discipline)
- Doc↔code consistency: if convention documentation changed in the same
  diff, validate the *new* doc against the *new* code (circularity risk)
- Tests: convention assertions pin documented behavior (coordinate
  directions, depth range, NDC mapping)

---

### Step 3.5 — IDE inspection sweep

Run a CLion inspection sweep on every changed file. This step starts
**concurrently with Step 3** (dimension reviews) since it depends only on
the changed-file list from Step 1, not on review results. The sweep
catches problems the dimension checklists miss (unused includes,
deprecated APIs, module-partition naming, etc.) and feeds the resolution gate.

**Procedure:**
1. Enumerate changed files (same list as Step 2).
2. Batch-call `clion_get_file_problems(filePath=<f>, errorsOnly=false,
   projectPath="E:/Code/ppr")` per file. Group 4–8 files per batch and
   adjust based on the orchestrator's per-message tool-call limit.
   See `clion-tools` SKILL.md §6 for the exact signature.
3. Each returned problem becomes a finding tagged **`[IDE]`** with
   provenance (inspection ID, severity, file:line). These findings enter
   Steps 4–6 like any other finding but are **exempt from Step 4's
   per-finding subagent fact-check** — IDE output is ground truth.

### Step 4 — Validate each finding (mandatory parallel fact-check)

After the global review pass (Step 3) and IDE inspection sweep (Step 3.5)
produce candidate findings, EVERY non-`[IDE]` finding must be individually
validated against the actual source BEFORE any result is presented to the
user. `[IDE]` findings are exempt (IDE = ground truth). This step is
mandatory — never skip it and never present unvalidated findings.

**Procedure:**
1. Collect the complete list of candidate findings (all zones, all severities)
   from Steps 3 and 3.5, excluding `[IDE]`-tagged findings.
2. **Batch findings** by `(source file, dimension group)`, with a maximum
   of 5 findings per batch. If a single file has more than 5 findings, split
   into multiple batches. Each finding in a batch gets a unique ID so the
   subagent cannot conflate them.
3. Spawn ONE parallel `oracle` subagent per batch (background, loading this
   `code-reviewer` skill). Each subagent receives its batch and is instructed
   to:
   - Read the ACTUAL cited source file(s) at each finding's cited location —
     **never the full diff**. (Reading the whole diff previously caused
     context exhaustion and wrong line numbers.)
   - Validate each finding independently: trace the real code path, confirm
     or refute the claim, verify the line number is correct.
   - Return one verdict per finding ID: `VERDICT: Confirmed | Partially
     correct | Incorrect | Cannot determine`, with `Evidence` (file:line +
     key snippet) and `Assessment` of whether the cited severity is
     over/under-stated.
4. **Orchestrator reconciliation:** Verify every finding ID from the batch
   appears in the response. If any ID is missing or returns `Cannot
   determine`, re-dispatch that finding individually as a single-finding
   batch. Drop or downgrade findings rated `Incorrect`; keep `Partially
   correct` only with stated nuance.
5. Present ONLY validated, reconciled results grouped by severity. Flag
   findings corroborated by ≥2 reviewers as high confidence.

**Why:** The global pass alone produced false positives — including two
fabricated ❌ Errors and several wrong line numbers. Per-item validation
against source catches misreadings before they reach the user. Batching
by `(file, dimension-group)` with a max of 5 findings per subagent cuts
validation subagent count by ~66% while preserving the safety guarantee
(every finding is still read against actual source, not the diff).

---

### Step 5 — Resolution gate (delegated, not inline)

Before the final report issues, every Error and Warning finding (human-found
or `[IDE]`-tagged) must be resolved. The reviewer never edits inline;
remediation happens exclusively through delegation.

**Loop** (max 3 rounds):
1. Dispatch `@fixer` per finding/batch to fix (bounded edits via
   `clion_apply_patch` / `clion_create_new_file`). Batch all findings on
   the same file into one `@fixer` call (unless they conflict) to reduce
   subagent launches.
2. **Re-inspection decision:** If `@fixer` reports that only suppression
   comments were added (no code changes), skip `clion_get_file_problems`
   for that file and instead verify the suppression comment format is
   correct (must cite the inspection ID and provide a rationale). If any
   code changes were made, re-run `clion_get_file_problems` on touched
   files only.
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

**Enforcement:** track round number explicitly. After round 3, stop
dispatching `@fixer`. Report remaining unresolved errors as blocking ❌ in
the Step 7 summary. Do not continue the loop. Early-exit: if a round
produces zero new fixes or suppressions, exit immediately.

**Suppression-comment policy:** suppression comments are exceptional and
reconcile with Dimension 9's no-comments rule by requiring oracle
confirmation + a cited rationale. Errors are never suppressible.

---

### Step 5.5 — Fix Plan Generation

After the resolution gate (Step 5) closes all errors and warnings, produce
a brief verification plan for each finding that was **fixed** or
**suppressed**. This step uses the `verification-planning` methodology
(claim → evidence path → budget → status) to give downstream `@fixer` or
`deepwork` consumers a concrete, machine-readable plan for confirming the
fix is correct or re-evaluating the suppression later.

**Scope:** Only `Error` findings that were fixed and `Warning` findings
that were suppressed require plans. `Suggestion` findings do not require
plans unless the user explicitly requests them.

**For each fixed `Error` or `Warning`:**

| Field | Content |
|---|---|
| **Claim** | What the fix asserts (e.g., "allocateRaw now calls poisonAllocated") |
| **Evidence path** | How to verify the claim holds (e.g., "Run engine.tests.core with ASAN; confirm no missing poison annotations in allocation paths") |
| **Budget** | Minimum work to establish the claim (e.g., "1 test file, compile + run engine.tests.core") |
| **Status** | `planned` (default) |

**For each suppressed `Warning`:**

| Field | Content |
|---|---|
| **Claim** | Why the suppression is justified |
| **Evidence path** | How a future reviewer can verify the suppression remains valid |
| **Re-evaluate** | Condition that would trigger re-evaluation (e.g., "when the surrounding code changes") |

Plans are included in the per-zone report (Step 6) under a "Verification
Plans" subsection and in the machine-readable JSON under
`"verification_plans"`. The `code-reviewer` skill does NOT need to load
the `verification-planning` skill directly — it produces the output
format that `verification-planning` expects as input. When the user later
invokes `verification-planning` on a specific finding, the pre-existing
plan serves as the starting point. This keeps the two skills decoupled.

---

### Step 6 — Generate per-zone report

Emit the preamble line first:

```
## Preamble

**Small-diff shortcut:** fired (<file_count> files, <line_count> total lines) — full dimension review.
**Small-diff shortcut:** not fired.
```

(Only one of the two lines is emitted per review.)

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

**Machine-readable summary** (append after the markdown table for
downstream agent consumption):

```json
{
  "zones": [
    {"zone": "lib/", "errors": 2, "warnings": 5, "suggestions": 8, "ide": {"fixed": 3, "suppressed": 1, "open": 0}},
    {"zone": "game/", "errors": 0, "warnings": 1, "suggestions": 3, "ide": {"fixed": 0, "suppressed": 0, "open": 0}},
    {"zone": "cmake/", "errors": 0, "warnings": 0, "suggestions": 1, "ide": {"fixed": 0, "suppressed": 0, "open": 0}}
  ],
  "suppressed": [
    {"file": "lib/engine/foo.h:42", "inspection": "UnusedInclude", "rationale": "..."}
  ],
  "verification_plans": [
    {
      "id": "vp-001",
      "finding_ref": "lib/Core.Foo.cppm:42",
      "type": "fix" | "suppression",
      "claim": "allocateRaw now calls poisonAllocated after allocation",
      "evidence_path": "Run engine.tests.core with ASAN; confirm no missing poison annotations in allocation paths",
      "budget": "1 test file, compile + run engine.tests.core",
      "status": "planned"
    }
  ],
  "gate": "green" | "red",
  "rounds_used": 1
}
```

