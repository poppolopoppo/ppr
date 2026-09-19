---
name: unit-test-updater
description: >
  Analyzes local git modifications and updates or adds C++ unit tests
  for changed observable behavior. Designed for the pP engine's PPR_UNIT_TEST
  framework. Use this skill whenever the user says "update tests",
  "add tests for my changes", or "test the new code".
---

# Unit Test Updater

Analyze unstaged/staged changes and produce matching unit tests for each changed
observable behavior contract in the pP engine's `PPR_UNIT_TEST` framework.

## Contract

This skill analyzes unstaged/staged git changes and produces matching unit tests
for changed observable behavior contracts in the pP engine's
 `PPR_UNIT_TEST` framework. It does
NOT modify production code. The orchestrator plans coverage and
delegates all file edits to `@fixer`. It never writes test files
directly.

## Mandatory pre-implementation / review checklist

- Define the changed observable behavior contract first: inputs, boundary/error
  result, state change, ownership transfer, and post-shutdown behavior.
- Test public or otherwise observable boundaries, not private representation or
  call sequencing. Cover internal regressions through their public boundary.
  Drive time-dependent behavior with an injected clock/tick rather than sleeps
  or the wall clock.
- Keep test fixtures explicit about lifetime: use values, RAII handles, engine allocators, or owning smart pointers; detach observers/callbacks before their owner is destroyed.
- For asynchronous code, test cancellation, callback detachment, and shutdown
  ordering: detach or disable callbacks/listeners/submissions and prevent new
  work; cancel, wake, drain, or join; then release resources while preserving
  the first cleanup error. Avoid timing-dependent assumptions.
- Use `PPR_TEST_ASSERT` for test assertions in every build configuration; do not use raw `new`/`delete` where engine allocator or lifetime rules apply.
- Follow the test group convention (thematic private `.cpp` group files in
  `lib/engine/tests/core/` or `lib/engine/tests/app/`, never new `.cppm`
  partitions), umbrella registration, and matching CMake source registration.
  Refer to `AGENTS.md`, `module-architect`, and `code-reviewer` for shared policy.

---

## Step 1 — Gather context

Run both commands:

```bash
git diff HEAD                  # show all changes
git diff --cached              # staged-only (if user already staged)
```

Also load the AGENTS.md for coding conventions, and read the test
group file(s) corresponding to the modified source:

- Source `lib/engine/core/Core.Foo.cppm` → leaves in the matching group file,
  e.g. `lib/engine/tests/core/Core.Allocator.Tests.cpp` (allocator group)
- Source `lib/engine/app/...` → leaves in the matching group file,
  e.g. `lib/engine/tests/app/App.Player.Tests.cpp` (player group)
- Test layout: each suite keeps ONE exported root (`core` / `app`) and a set of
  thematic private group `.cpp` files (`module engine.tests.<suite>;` impl units,
  `namespace detail` leaves, one TU-local `const UnitTest` plus one
  non-exported `const UnitTest &<node>Tests() noexcept` accessor per
  root-visible node; file-local sub-groups need no accessor; `memory`/`containers`
  assembled in `Core.Tests.cpp`). A new leaf joins its thematic file; a
  new file only for a new thematic area.
  If no group file covers the area, note that a new group file must be created
  (PRIVATE SOURCES + 1 accessor forward-declare + 1 recurse call per root-visible node).

---

## Step 2 — Analyse the diff

For each changed observable behavior, classify the required test action:

| Change type | Testing action |
|---|---|
| Changed observable behavior | Add or update a focused test at the public boundary |
| Bug fix | Add a public-boundary regression test that reproduces the prior behavior and asserts the fix |
| Internal/private change | Test only when it changes observable behavior; exercise it through the public boundary |
| Deleted API | Remove or update tests for the affected public contract |

---

## Step 3 — Write tests following project conventions

### Framework rules (from AGENTS.md):

1. **File:** the existing thematic group file, e.g.
   `lib/engine/tests/core/Core.Allocator.Tests.cpp` (never a new `.cppm`
   partition — per-test partitions were collapsed; do not create
   `export module engine.tests.<suite>:<part>`).
2. **Module declaration:** `module engine.tests.core;` (plain impl unit, no
   `export`, no partition name).
3. **Includes:** `module;` + `#include "pP/UnitTest.h"` (always) then `module engine.tests.core;` + `import engine.core;` (`engine.app` for app tests) + `import std;`. Add `"pP/Macros.h"` or third-party headers to the global fragment only when the test body needs them (e.g. `App.Camera.Tests.cpp` includes both `Macros.h` and `UnitTest.h`; `App.Render.Tests.cpp` includes `slang.h`)
4. **Namespace:** Leaves in `namespace pP::tests::detail`; group/root nodes in `namespace pP::tests`
5. **Nested grouping:** Use inner namespaces under `detail` for sub-grouping
6. **Leaf tests:** `PPR_UNIT_TEST(descriptive_name) { PPR_TEST_ASSERT(...); };` (non-exported, in `detail`)
7. **Parent tests:** one TU-local group plus one accessor per root-visible node: `const UnitTest <group> = UnitTest::Named("<group>") / [](UnitTest::IRun &_) -> void { _.recurse({detail::...}); };` then `const UnitTest &<group>Tests() noexcept { return <group>; }` — file-local sub-groups need no accessor; direct-root singleton leaves use `const UnitTest <leaf> = detail::<leaf>;` + `const UnitTest &<leaf>Tests() noexcept { return <leaf>; }`; never spell `extern` except for the `core`/`app` roots
8. **Top-level registration:** In the suite root `.cpp` (`Core.Tests.cpp` / `App.Tests.cpp`), forward-declare the accessor (`const UnitTest &<group>Tests() noexcept;`) and call it in the root `_.recurse({...})` in fixed order; register a new group `.cpp` in CMake PRIVATE SOURCES (never FILE_SET) in the same change
9. **Assertions:** Use `PPR_TEST_ASSERT()` only — it throws in ALL build configs, including release (engine `PPR_ASSERT`/`PPR_VERIFY` compile to `[[assume]]` in release and are unusable in tests)
10. **Code style:** Follow `AGENTS.md`; this skill owns test behavior and registration, not repository-wide style policy.
11. **Expected-fail tests:** `PPR_UNIT_TEST(name, UnitTest::expect_fail) { ... };` — test body is expected to throw an assertion or exception. If it throws, the test passes; if it returns normally, the test fails. Use for precondition/guard validation.
12. **Expected-crash tests:** `PPR_UNIT_TEST(name, UnitTest::expect_crash) { ... };` — test body is expected to crash/terminate the process (e.g., ASAN violation, segfault). Runs in a forked child process; non-zero exit = pass, zero exit = fail.
13. **Fork-only tests:** `PPR_UNIT_TEST(name, UnitTest::fork) { ... };` — runs in a child process but expects success (zero exit). Use when the test must be isolated from the parent process state.

### Behavioral test selection

- Select normal, boundary, error, lifecycle, and teardown cases that prove the
  changed contract; do not prescribe tests per function, type, branch, or
  incidental implementation detail.
- Use `expect_fail` only for active precondition guards and `expect_crash` only
  for process-level failures. Do not turn assertion behavior into a release
  runtime contract.

### Guarded edge cases

Test only observable preconditions. Engine `PPR_ASSERT`/`PPR_VERIFY` may become
assumptions in release, so an `expect_fail` case for such a guard must be
configuration-scoped; do not represent it as a portable runtime error contract.

```cpp
// Debug/profile guard — expected to throw only where that guard is active.
PPR_UNIT_TEST(null_parameter_triggers_assertion, UnitTest::expect_fail) {
    some_function(nullptr);  // triggers PPR_ASSERT(arg != nullptr)
};

// Memory safety violation — expected to crash the process
PPR_UNIT_TEST(use_after_free_triggers_asan, UnitTest::expect_crash) {
    int *p{};
    {
        mem::Allocation<int, mem::GPA> allocation;
        p = allocation.create(42);
    }
    volatile auto x = *p;  // ASAN use-after-free
};
```

Rules:
- Use `expect_fail` for **active precondition guards** — the test body throws via the guard, the runner catches the exception and records a pass. Do not use it to claim a release runtime error contract for `PPR_ASSERT`/`PPR_VERIFY`.
- Use `expect_crash` for **process-level failures** — the test spawns in a child process; a non-zero exit (crash) is a pass, a clean exit is a failure.
- Guarded-edge-case tests live alongside the happy-path tests in the same test file and parent group.

---

## Step 4 — Register the new tests

### In the group file:

Ensure every leaf test is aggregated via its group node:

```cpp
const UnitTest subsystem = UnitTest::Named("subsystem") / [](UnitTest::IRun &_) -> void {
    _.recurse({
        detail::Group::test_a,
        detail::Group::test_b,
    });
};

const UnitTest &subsystemTests() noexcept { return subsystem; }
```

New leaf in an existing group file = edit that file only. New thematic
group = new `.cpp` (CMake PRIVATE SOURCES in the same change) + 1 accessor
forward-declare + 1 recurse call per root-visible node in the suite root `.cpp`.
Direct-root singleton leaves use a TU-local copy plus accessor
(`const UnitTest <leaf> = detail::<leaf>;` + `const UnitTest &<leaf>Tests() noexcept { return <leaf>; }`),
called as `<leaf>Tests()` — never wrap one in a `Named` group (that would add a
tree level and change its path); see `module-architect`.

### In the suite root (e.g. `lib/engine/tests/core/Core.Tests.cpp`):

1. Forward-declare the accessor: `const UnitTest &<subsystem>Tests() noexcept;`
2. Call `<subsystem>Tests()` in the root `_.recurse({...})` in fixed order (test
   paths `core/<...>` / `app/<...>` derive from these names — keep them stable)

---

## Step 5 — Verify

Build and run the test executable to confirm all tests pass.

Prefer CLion MCP tools (see `clion-tools` skill and AGENTS.md §Debugging):

```
clion_execute_run_configuration(configurationName="engine.tests.core", programArguments="--shuffle")
```

If CLion is unavailable, use CMake presets directly:

```powershell
cmake --build --preset msvc-dev --target engine.tests.core
ctest --test-dir out/build/msvc-dev --output-on-failure
```

Use `msvc-dev` or `clang-dev` as applicable; do not use a `gcc-*` preset for
module-build verification.

If any test fails, treat the failure as a bug — do not weaken assertions.

### Expected-fail/crash validation:

- For `expect_fail` tests: verify they pass (i.e., the assertion fires and the runner flips the result to pass). If an `expect_fail` test returns without throwing, that is a *failure* — the precondition wasn't enforced.
- For `expect_crash` tests: verify they pass (i.e., the child process exits non-zero). If an `expect_crash` test exits cleanly, that is a *failure* — the crash wasn't triggered.
- Run all tests together; expected-fail/crash tests should produce green (pass) output, not red (fail).

---

## Constraints

- **Never modify production code** to make tests pass. If a test reveals a design issue, flag it to the user.
- **Do not test private/implementation details.** Test through public API only.
- **Keep tests self-contained.** Each `PPR_UNIT_TEST` should be independent.
- **If the diff has no public API changes** (e.g., internal refactor, comment fix), output "No test changes required" and stop.
- **Large diffs:** Group related changes and ask the user which area to test first.

---

## Coordination

| Step | Delegate to | Why |
|------|-------------|-----|
| Locate corresponding test files / naming | `@explorer` | Fast codebase recon |
| Classify diff into required test actions | `@oracle` (or orchestrator) | Judgment on API change type |
| Write `PPR_UNIT_TEST` bodies + umbrella registration | `@fixer` | Bounded implementation |
| Verify build/test of new tests | background build subagent | Reuse validation lane |
| Convention check | `code-reviewer` skill | AGENTS.md compliance |

---

## Execution handoff

This skill defines test-analysis and test-update procedure only. The active OMO
preset is the sole authority for actor selection, permissions, tool access,
parallel work, validation execution, and session reuse. Apply the procedure only
through the active configuration; this document grants or routes none of them.
