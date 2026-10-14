---
description: Optimize a bounded batch of member layouts using compiled layout evidence.
---

Perform one bounded optimization pass under
[AGENTS.md's data-member layout contract](../../AGENTS.md#data-member-layout).
Use the existing `build-system`, `validation`, and `code-reviewer` skills for
their responsibilities. This command adds no agent, permission, or permanent gate.

{optional scope: targets, types, build configuration, or candidate limit}

## Establish coverage

Preserve existing user changes. Record the source state, build configuration,
toolchain, and selected artifacts in the task's existing evidence record.
Read CMake File API metadata through `build-introspection` to identify production
targets and their full PDBs. Use generated reply records directly if the helper
cannot read an optional field; do not reconstruct the graph by scanning source.

Start with the Release `app.game` full PDB and its linked production types.
Reuse it only when build evidence establishes its source/configuration identity;
timestamps alone do not prove freshness. Otherwise obtain one current baseline
build through `build-system`. Inspect additional existing target PDBs only to
cover an identified gap. Keep configurations separate, and never use stripped
PDBs as evidence of complete type coverage. Unbuilt types, missing private symbols,
and uninstantiated templates remain explicitly uncovered.

## Collect candidates mechanically

Use `llvm-pdbutil pretty` to inspect compiled types without asking an agent to
read repository source. Verify tool availability, then save the full report under
`out/layout-audit/` rather than returning it to model context:

```text
rtk proxy llvm-pdbutil pretty --classes --include-types="^pP::" --class-definitions=layout --class-recurse-depth=1 --class-order=padding-imm --min-class-padding-imm=1 "<full-pdb>"
```

Summarize the report mechanically into rows containing artifact/configuration,
qualified type, size, member offsets/sizes, and immediate padding. Exclude forward
declarations, compiler-generated/local closure types, and non-project types.
Deduplicate identical type/layout records; flag conflicting layouts for the same
type instead of combining them. Keep raw dumps on disk and pass only selected
rows to agents. If extraction is unavailable or incomplete, report that condition;
do not substitute a repository-wide agent scan.

Immediate padding identifies candidates, not removable bytes. Do not add nested
padding to a parent's immediate padding or treat required tail padding as waste.
Prioritize known repeated storage using existing population evidence; otherwise
mark population unknown. Account for rebuild fan-out when selecting a batch.
Freeze at most 20 unique candidate types before source inspection unless the
caller supplied another limit. This is a run budget, not an engine standard.
Retain the remaining candidates as deferred; do not replenish the batch.

## Resolve and edit only the selected types

Use CodeGraph with exact candidate names to locate current declarations and
directly affected consumers. Read only their owning codemaps and relevant source.
Confirm each candidate against the current build and declarations before editing.
Record its source location, proposed order, expected size change, constraints,
and affected target. Do not infer field alignment solely from PDB field size or
offset; resolve unknown requirements with targeted compiler evidence.

Check initialization/destruction dependencies, declaration-order behavior,
external layouts, and intentional false-sharing separation. Defer candidates
whose safe reorder is unclear or requires a broader design change. Reject a
reorder that does not reduce actual size/stride unless a separate measured
performance requirement justifies it.

Use one implementation owner for the selected batch. Each edit scope consists
only of the listed types and consumers that require corresponding updates.
Reorder members and update affected initialization; preserve types, ownership,
behavior, and required alignment. Do not introduce packing, representation
changes, unrelated refactors, or a new optimization subsystem. Keep the candidate
evidence with the handoff so implementation and review do not repeat discovery.

## Validate once and finish

Batch edits by affected build closure. Run applicable formatting/style checks,
then rebuild and run the affected suites through the existing skills. Reuse
passed evidence and rerun only checks invalidated by fixes. Obtain one independent
review for the coherent batch, including any corresponding initializer and
consumer changes. Follow `validation` for additional compatibility gates.

Re-extract only the selected layouts from current artifacts and compare actual
before/after size, alignment evidence, and relevant offsets. Report per-type
results, skipped/deferred/uncovered types, and validation status. Calculate total
storage savings only from evidenced populations without double-counting embedded
objects; a size reduction alone is not a runtime-speed improvement.

Finish when the fixed batch is resolved. Do not expand the search or start another
optimization round. Commit only when the user requests it, using
`git-commit-planner` within the approved scope.
