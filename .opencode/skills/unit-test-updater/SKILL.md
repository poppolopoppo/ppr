---
name: unit-test-updater
description: >
  Analyzes local git modifications and updates or adds C++ unit tests
  to reflect API changes. Designed for the pP engine's PPR_UNIT_TEST
  framework. Use this skill whenever the user says "update tests",
  "add tests for my changes", "get 100% coverage", or "test the new code".
---

# Unit Test Updater

Analyze unstaged/staged changes and produce matching unit tests that
cover every new or changed function, type, branch, and edge case.

---

## Step 1 — Gather context

Run both commands:

```bash
git diff HEAD                  # show all changes
git diff --cached              # staged-only (if user already staged)
```

Also load the AGENTS.md for coding conventions, and read the test file(s)
corresponding to the modified source:

- Source `lib/engine/core/Core.Foo.cppm` → tests `lib/engine/tests/core/Core.Foo.Tests.cppm`
- Source `lib/engine/core/Core.Foo.Bar.cppm` → tests `lib/engine/tests/core/Core.Foo.Bar.Tests.cppm`
- If no test file exists, note that one must be created.

---

## Step 2 — Analyse the diff

For each changed function, type, or constant, classify the nature of the change:

| Change type | Testing action |
|---|---|
| New public function | Add a `PPR_UNIT_TEST` block exercising all paths |
| New type/class | Add a namespace + `PPR_UNIT_TEST` per key operation |
| Modified signature | Update existing test to match; cover new parameters |
| New overload | Add test for new overload signatures |
| New enum/constant | Test value, bitwise ops for bitmask enums, `hashValue` |
| Bug fix | Add a test that reproduces the bug; assert fix |
| Internal/private change | No test needed (test through public API) |
| Deleted API | Remove/update corresponding test |

---

## Step 3 — Write tests following project conventions

### Framework rules (from AGENTS.md):

1. **File:** `lib/engine/tests/core/Core.<Subsystem>.Tests.cppm`
2. **Module declaration:** `export module engine.tests:core_<subsystem>;`
3. **Includes:** `module;` + `#include "pP/Macros.h"` then `export module ...;` + `import engine.core;` + `import std;`
4. **Namespace:** All tests in `namespace pP::tests`
5. **Nested grouping:** Use inner namespaces for sub-grouping
6. **Leaf tests:** `PPR_UNIT_TEST(descriptive_name) { PPR_ASSERT(...); };`
7. **Parent tests:** `PPR_UNIT_TEST(subsystem) { _.recurse(Group::sub_test); };`
8. **Top-level registration:** In `Core.Tests.cppm`, add `import :core_<subsystem>;` and call `_.recurse(mySubsystem);` inside the appropriate parent
9. **Assertions:** Use `PPR_ASSERT()` only
10. **Code style:** No comments, `constexpr` everywhere, `[[nodiscard]]`, no raw loops, prefer algorithms/ranges

### Test coverage checklist for each function under test:

- [ ] Normal/expected inputs (happy path)
- [ ] Boundary values (empty containers, zero, max, min, null)
- [ ] Edge cases (single element, full capacity, aliasing)
- [ ] Error conditions (overflow, invalid state, null pointer)
- [ ] Constexpr evaluation (mark tests `constexpr` where possible)
- [ ] `noexcept` guarantee (verify if function is marked noexcept)
- [ ] State mutation (check before/after for mutating functions)
- [ ] Iterator validity (where iterators are involved)
- [ ] Equality/comparison (if types define `==`, `<=>`, `hashValue`)
- [ ] Relocatability trait (if type should be relocatable)

---

## Step 4 — Register the new tests

### In the test partition file:

Ensure every leaf test is aggregated via its parent:

```cpp
PPR_UNIT_TEST(subsystem) {
    _.recurse(Group::test_a);
    _.recurse(Group::test_b);
};
```

### In `lib/engine/tests/core/Core.Tests.cppm`:

1. Add `import :core_<subsystem>;` at the top
2. Add `_.recurse(<subsystem>);` inside the appropriate parent test (e.g., `containers`, `memory`, `strings`)

---

## Step 5 — Verify

Build and run the test executable to confirm all tests pass:

```bash
cmake --build build --target VideoGameApp
./build/game/VideoGameApp
```

If any test fails, treat the failure as a bug — do not weaken assertions.

---

## Constraints

- **Never modify production code** to make tests pass. If a test reveals a design issue, flag it to the user.
- **Do not test private/implementation details.** Test through public API only.
- **Keep tests self-contained.** Each `PPR_UNIT_TEST` should be independent.
- **If the diff has no public API changes** (e.g., internal refactor, comment fix), output "No test changes required" and stop.
- **Large diffs:** Group related changes and ask the user which area to test first.
