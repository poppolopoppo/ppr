# MSVC C1001 in module translation units

Status: resolved for the October 2026 tree. This document records the observed
failure, the evidence that separated its two causes, the changes that removed
them, and the procedure to use if the same signature returns after a toolset or
source change.

## Scope

The evidence applies to:

- MSVC `14.52.37025` (Visual Studio 18 Insiders).
- C++23 modules with `-std:c++latest`, `/permissive-`, and `/bigobj`.
- The `msvc-dev` preset: Debug and AddressSanitizer
  (`PPR_ENABLE_SANITIZER_ADDRESS`).
- The working tree of October 2026.

A `C1001` is a compiler defect, but the construct that reaches it lives in our
source. Treat the guidance here as an evidence-backed procedure, not as a claim
that every `C1001` has this cause. Compiler ICEs are version-specific; revalidate
after a toolset update.

## Symptom

`cl.exe` terminated with:

```
fatal error C1001: Internal compiler error.
(compiler file 'function-signature.cpp', line 213)
```

The failure was deterministic per translation unit, not intermittent. Rebuilding
the same object with the same sources failed identically. Rebuilding only the
failing object was therefore a reliable and fast reproducer; a full preset build
was not required to test a hypothesis.

The diagnostic note chain was the most valuable signal:

- When the ICE notes named a template instantiation, that instantiation was the
  cause.
- When no notes appeared, the crash came from cumulative front-end load rather
  than one named construct.

## Minimal failing shape

In debug builds, `safe_ptr<T>` enforces the `safe_object` contract through a
concept built on `std::is_base_of_v`:

```cpp
concept TSafeObject = std::is_base_of_v<safe_object, T>;
```

That concept is evaluated by the ref-count helpers and by the destructor:

```cpp
~safe_ptr() noexcept {
    static_assert(details::TSafeObject<T>, "safe_ptr requires safe_object base");
    reset();
}
```

With `T = ParallelScheduler`, the instantiation crashed the compiler while it
built function signatures in a translation unit importing the owning module.
When the crash was reported with notes, they named `safe_ptr<ParallelScheduler>`.

`PPR_ENABLE_SAFE_OBJECT_TRACKING` gates the check. In release, `safe_ptr` is a
raw-pointer wrapper and `safe_object` contributes no state, so the same source
did not fail outside the debug preset.

## Two failure modes

The investigation initially treated every `C1001` as the same `safe_ptr`
instantiation. That was wrong: removing the instantiation still left one test
translation unit failing.

### A. Named instantiation trigger

`safe_ptr<ParallelScheduler>` appeared in three places:

- `sim::GenerationExecution::m_scheduler` in `Sim.WorldGen.cppm`.
- `asset::preparation_detail::State::m_pool` in `Asset.Preparation.cppm`.
- `ParallelScheduler::WorkerState_::m_owner` (`safe_ptr<const ParallelScheduler>`)
  in `Core.Concurrency.Scheduler.cppm`.

Each site forced the `is_base_of_v<safe_object, ParallelScheduler>` instantiation
into its importing translation units. The sim and asset modules failed
independently; fixing one did not fix the other.

### B. Cumulative front-end threshold

After every `safe_ptr<ParallelScheduler>` use was removed, the
`Asset.Image.Preparation.Tests.cpp` translation unit still crashed, and now
without an ICE note chain. Bisecting the test content with temporary probe
translation units produced this evidence:

| Configuration | Result |
|---|---|
| Test groups T1–T9 | compiles |
| T1–T10 | `C1001` |
| T10 alone | compiles |
| T7–T11 | compiles |
| T1–T10 without the `find_if` / `count_if` block | compiles |
| Full file with the manual loop and raw pointers | compiles |

T1–T11 are temporary session probe groups, not a committed test structure. The
conclusion is what matters: the failing unit was over a cumulative front-end
threshold, and the `std::ranges::find_if` plus `count_if` over
`Array<Diagnostic>` was the tipping construct. The same construct compiled in
isolation. Any sufficiently large addition could have crossed the same line.

This is why "the last thing edited" is not automatically the cause. The last
edit only reveals the margin that already existed.

### C. A second cumulative threshold, in the checkpoint suite

The checkpoint tests reproduced mode B again after further test growth, this time
in `game/colony/Checkpoint.Tests.cpp`:

```
game/colony/Checkpoint.Tests.cpp(1839): fatal error C1001
(compiler file 'function-signature.cpp', line 213)
note: IFC import detected.
```

The source was newer than the last good object, and the crash was at the closing
brace of a test lambda, with no instantiation notes. Emptying test bodies moved
the crash forward instead of removing it:

| Configuration | Crash location |
|---|---|
| Original file | 1839 |
| `round_trip_restores_edited_cells_actor_identity_and_frozen_full_capture` body emptied | 2099 |
| That body and `malformed_identity_metric_counts_and_payload_are_rejected_before_restore` emptied | 2281 |

The file never passed while the remaining bodies were present. Two emptied tests
were not enough: the translation unit was well over the threshold, not one
construct over it. The checkpoint file is 2321 lines and had reached the same
cumulative limit as the asset test unit.

## Resolution

The durable fix removes the problematic instantiations instead of moving them:

- `sim::GenerationExecution::m_scheduler` is a plain borrowed
  `ParallelScheduler *`.
- `asset::preparation_detail::State::m_pool` is a plain borrowed
  `ParallelScheduler *`.
- `ParallelScheduler::WorkerState_::m_owner` is a `const ParallelScheduler *`.
- `ParallelScheduler` no longer derives from `safe_object`, so no
  `is_base_of_v<safe_object, ParallelScheduler>` can be formed anywhere.
- The asset test's range algorithms were replaced by an equivalent manual loop
  that records the first matching location and the match count.

The pointer changes preserve the existing contracts: all three references are
non-owning, and their owners already outlive them by construction or by
documented dependency injection. `GenerationExecution` remains an aggregate, so
designated initializers in tests are unchanged.

Follow-up call sites that still treated the member as a `safe_ptr` were updated:

- `game/colony/WorldGen.Fixture.cpp` reads `execution.m_scheduler` directly.
- `game/colony/ColonyDriver.cpp` reads `execution->m_scheduler` directly.

`ParallelScheduler`'s debug-only self-reference count in `WorkerState_` was
redundant: `~ParallelScheduler` calls `join()` and destroys `m_workers` before
the scheduler object goes away. The comment on the deleted move operations
already described `WorkerState_`'s owner as a non-owning pointer.

A separate real error surfaced once the build advanced past the ICEs:
`Checkpoint.Tests.cpp` used the `GameMatter` log category, which is defined only
inside `MatterRuntime.cpp` with no exported declaration, and the file's own
`details::log` namespace shadowed it. Those four checks now use the file's
existing `CheckpointTests` category. This was a normal compile error, not an
ICE.

### Checkpoint test translation unit split

Repeated shaving is not durable for a file that keeps growing. The checkpoint
translation unit was split along its existing suite boundary:

- `Checkpoint.Tests.Support.h` holds the shared `Fixture` and
  `editedCheckpoint` fixture.
- `Checkpoint.Tests.cpp` keeps the CPU checkpoint tests and defines the
  `CheckpointTests` log category.
- `Checkpoint.Gpu.Tests.cpp` holds `GpuOwner`, the four GPU checkpoint tests,
  and `gameCheckpointGpuTests()`.
- `game/CMakeLists.txt` registers the new source once.

The split reduced the CPU unit to 930 lines and the GPU unit to 1106 lines
(plus the 343-line shared header), both below the observed threshold. Test names
and registration names are unchanged.

A shared textual header included after the imports in two implementation units
of the same named module produced strong definitions for the non-template member
functions defined inside `Fixture`; the first link failed with `LNK2005` for
`Fixture::Fixture`, `~Fixture`, `write`, and `presentation`. Marking those
members explicitly `inline` restored COMDAT deduplication. In-class definitions
are already implicitly inline by the language rules, but explicit `inline` was
required for MSVC to emit them as selectable symbols across the two units.

## What did not work

| Attempt | Outcome |
|---|---|
| Out-of-line `~GenerationExecution()` | Fixed the sim translation unit but not the asset test unit. Other inline `safe_ptr` uses (`operator->`, `operator*`, ref-count helpers) still instantiate the concept. |
| `SchedulerRef` wrapper with out-of-line destructor and assignment | Reduced the surface but did not fix the asset test unit. |
| Removing `safe_ptr<ParallelScheduler>` alone | Still failed the asset test unit, which exposed the cumulative failure mode. |
| Out-of-line relocation in general | Moves the instantiation rather than eliminating it and leaves the same landmine in other translation units. |
| Emptying individual checkpoint test bodies | Moved the crash from 1839 to 2099 and then 2281, but never passed. The unit was over the cumulative threshold, not one construct over it. |

## Diagnostic procedure

1. Read the full build log and find the **first** `C1001`, not the last.
2. Extract the note chain. If it names a template instantiation, treat that
   instantiation as the target.
3. Build only the failing object:

   ```powershell
   ninja -C out\build\msvc-dev '<target-dir>/CMakeFiles/<target>.dir/<file>.obj'
   ```

4. Test one hypothesis at a time. Remove the construct and compile; add it back
   alone and compile. Distinguish a necessary condition from a sufficient one.
5. For a large test translation unit, bisect content with generated probe units.
   In this investigation, the T1–T10 versus T10-alone comparison separated the
   threshold effect from the construct effect.
6. After changing a member type, search the whole tree for every use and every
   `.get()` call before rebuilding.
7. Rebuild each consuming preset and run the affected suites. Compilation alone
   does not establish behavior.
8. If a file restore leaves misleading `C7621` module errors, check file
   timestamps before chasing the module graph. Restoring with `Copy-Item` can
   preserve stale timestamps; touch the file or bump `LastWriteTime`.

## Decision guidance

- **Prefer eliminating the instantiation over relocating it.** Out-of-line
  definitions are useful evidence and occasionally a temporary fix, but they do
  not remove the instantiation from other translation units.
- **Use a raw borrowed pointer when ownership is not transferred and the
  outlive relationship is already guaranteed.** This is the right fit for
  globals, injected dependencies with a documented lifetime, and self-references
  ordered by `join()` and member destruction.
- **Keep `safe_ptr` where it provides real observed-lifetime checking** of an
  object whose destruction order is not otherwise obvious.
- **When no ICE notes appear, suspect cumulative load.** For a localized tipping
  construct, prefer a small semantic rewrite. When shaving one construct only
  moves the crash to the next test, the unit is over the threshold: split along
  an existing suite boundary instead of continuing to shave tests. Keep test and
  registration names stable so the suite layout does not change.
- **A shared textual header across module implementation units needs explicit
  `inline` on non-template in-class definitions.** Otherwise MSVC can emit
  strong duplicates and fail the link with `LNK2005`.
- **Keep module interfaces lean.** Heavy template instantiation in exported
  inline bodies, especially in aggregates used with designated initializers,
  increases the front-end margin consumed by every importer.
- **Treat debug-only lifetime instrumentation as a cost/risk trade.** A check
  that crashes the debug toolchain repeatedly has negative value even when its
  intent is sound.

## Verification record

The resolved tree was verified with:

- Full `ninja -C out\build\msvc-dev`: green, 509 steps, no `C1001`.
- `ninja -C out\build\msvc-profile app.game.exe`: green.
- `ctest --test-dir out\build\msvc-dev -R '^(engine.tests.core|engine.tests.sim|engine.tests.asset.cpu|game.tests)$'`:

  | Suite | Result | Time |
  |---|---|---|
  | `engine.tests.core` | Passed | 52.70 s |
  | `engine.tests.sim` | Passed | 52.08 s |
  | `engine.tests.asset.cpu` | Passed | 2.26 s |
  | `game.tests` | Passed | 143.07 s |

- `scripts/Format-Cpp.ps1` Check: PASS on every touched file.
- `scripts/Check-SemanticStyle.ps1`: no blocking findings.

### Checkpoint split verification

After the checkpoint translation unit split:

- `game/CMakeFiles/game.tests.dir/colony/Checkpoint.Tests.cpp.obj`: compiled, no
  `C1001`.
- `game/CMakeFiles/game.tests.dir/colony/Checkpoint.Gpu.Tests.cpp.obj`:
  compiled, no `C1001`.
- `game.tests.exe`: linked after marking the shared `Fixture` members `inline`.
- `ctest --test-dir out\build\msvc-dev -R '^game.tests$'`: Passed, 203.92 s.
- `scripts/Format-Cpp.ps1` Check on the three checkpoint files: PASS.
- `scripts/Check-SemanticStyle.ps1` on the three checkpoint files: no blocking
  findings; the reported B-CMP advisories are in the moved expressions.

The engine suite timings above predate this test-only split; those translation
units were not changed by it.

## Limitations

- No standalone minimal reproducer was filed with Microsoft. The exact
  front-end defect behind `function-signature.cpp:213` remains unknown.
- The evidence is tied to MSVC `14.52.37025`; a later toolset may fix, move, or
  replace the failure.
- `msvc-rel` was not rebuilt in this investigation. The changed code paths are
  debug-instrumentation removals and pointer substitutions, so no release
  behavior change is expected, but that preset is not part of the recorded
  verification.
- Removing `safe_object` from `ParallelScheduler` removes its debug lifetime
  self-check. The real invariant is enforced by `join()` before worker
  destruction, not by reference counting.

## Key files

- [Core.Memory.Pointer.cppm](../lib/engine/core/memory/Core.Memory.Pointer.cppm):
  `safe_object`, `safe_ptr`, and `TSafeObject`.
- [Core.Concurrency.Scheduler.cppm](../lib/engine/core/concurrency/Core.Concurrency.Scheduler.cppm):
  `ParallelScheduler`, `WorkerState_`, and the removed base class.
- [Sim.WorldGen.cppm](../lib/engine/sim/worldgen/Sim.WorldGen.cppm):
  `GenerationExecution`.
- [Asset.Preparation.cppm](../lib/engine/asset/Asset.Preparation.cppm):
  `preparation_detail::State`.
- [Asset.Image.Preparation.Tests.cpp](../lib/engine/tests/asset/Asset.Image.Preparation.Tests.cpp):
  the rewritten T10 assertions.
- [WorldGen.Fixture.cpp](../game/colony/WorldGen.Fixture.cpp) and
  [ColonyDriver.cpp](../game/colony/ColonyDriver.cpp): follow-up call sites.
- [Checkpoint.Tests.cpp](../game/colony/Checkpoint.Tests.cpp),
  [Checkpoint.Tests.Support.h](../game/colony/Checkpoint.Tests.Support.h), and
  [Checkpoint.Gpu.Tests.cpp](../game/colony/Checkpoint.Gpu.Tests.cpp): the split
  checkpoint suite and the `GameMatter` category correction.
