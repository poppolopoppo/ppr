# C++ logical grouping examples

[AGENTS.md](../AGENTS.md#semantic-control-flow) owns the rules. These examples
calibrate them against the user's whitespace-only working-tree corrections to
[Layout.cpp](../lib/engine/sim/worldgen/Sim.WorldGen.Layout.cpp) and
[Layout.cppm](../lib/engine/sim/worldgen/Sim.WorldGen.Layout.cppm), compared with
their staged versions, and the section headers added by commit `6fc03a2`.
Read this reference when a grouping decision is unclear; it is not an additional
per-edit checklist or a requirement to re-read those source files.

## Logical steps inside one responsibility

In `assignRequired`, counting candidates, rejecting an impossible demand, and
recording feasible demands all serve assignment, but are three distinct steps.
The authored correction adds only the two blank lines:

```cpp
u32 candidates = 0u;
for (std::size_t s = 1u; s < sites.size(); ++s) {
    if (eligible(constraint, sites[s], sites[0u], plan)) {
        ++candidates;
    }
}

if (count > candidates) {
    PPR_RETURN_ERROR_ON_FAIL(WorldLayout, make_error_code(EGenerationErrc::constraints_exhausted));
}

for (u32 i = 0u; i < count; ++i) {
    demands.push_back({c, candidates, constraint.m_key});
}
```

Removing these blank lines is a readability defect even though the tokens,
behavior, nesting, and formatter acceptance may be identical. Apply the same
reasoning inside the outer loop, not just between top-level function phases.

Other boundaries in the same authored implementation:

| Location | Steps separated by blank lines |
|---|---|
| `assignRequired` | Build demands; sort; allocate search scratch; register deferred peak accounting; search; publish assignments. |
| Nested augmenting search | Reject ineligible sites; record a parent/check a free site; handle an occupied site; validate search success; reassign the path. |
| `BiomeLayout::append` | Define the local bin helper; use it to update the index and sites. |
| `BiomeLayout::nearest` | Check input invariants; define bin conversion; initialize search state; define visitor; traverse rings; check final result. |
| Ring traversal | Top row; side columns; bottom row; calculate unvisited bounds; test termination. Independent bounds checks are also visually separated. |
| `restoreBiomeLayout` | Check collection shape; validate entries; construct the layout; return the completed result. |
| `generationBiomeAt` | Fast paths; start-reservation lookup; warped-field lookup. |
| Weighted assignment | Skip assigned sites; sum weights; reject an empty choice; draw/select; check the selected biome. |

A single statement can be a paragraph: sorting demands or calling
`assignRequired` marks a phase transition. Even the nested weighted-choice loop
separates skipping an ineligible constraint, updating `cumulative`, and testing
the draw. Statement count is not the criterion.

Conversely, `distanceSquared` keeps its coordinate deltas with the return;
`compileBiomeSampleLayout` keeps appending, setting the sample biome, and returning
the layout together. An assignment and its `break` stay together. Do not insert
a blank after every guard, declaration, closing brace, or before every return.
Keep statements together when they form one step, not merely because they share
variables or contribute to the same function's purpose.

## Declaration groups and attached documentation

The `BiomeLayout` interface separates its constant, related storage groups,
constructor, mutator, friends, move pair, deleted-copy pair, and query groups.
This is an excerpt of the authored public declarations:

```cpp
BiomeLayout(BiomeLayout &&) noexcept = default;
BiomeLayout &operator=(BiomeLayout &&) noexcept = default;

BiomeLayout(const BiomeLayout &) = delete;
BiomeLayout &operator=(const BiomeLayout &) = delete;

[[nodiscard]] std::span<const GenerationSite> sites() const noexcept;
[[nodiscard]] std::optional<u32> sampleBiome() const noexcept;

/// Owned vector capacities and the inline spatial-bin head table.
[[nodiscard]] u64 retainedPayloadBytes() const noexcept;

/// Finite positions may lie outside the world after domain warp, within
/// twice kNoiseCoordinateLimitMetres on each axis.
[[nodiscard]] u32 nearest(double x_m, double y_m) const noexcept;
```

Do not separate each member mechanically: the move pair and the simple queries
remain grouped. Separate a new documented operation before its comment, keeping
the comment attached to the declaration. Separate adjacent definitions even when
their bodies are tiny. In `GenerationSite`, state and the comparison member are
separate groups.

## Named sections for navigation

Commit `6fc03a2` adds the established dashed headers across interfaces,
implementations, and tests. Its Layout interface names `generation errors`,
`biome layout`, and `layout compilation`; its application implementation names
`headless worldgen validation` and `interactive editor`. Tests receive named
suite sections such as `config` and `world gen`, including at the start of a
file's only suite. These are required navigation structure, not optional
decorations reserved for unusually long files.

Use the existing [Input.Listener reference](../lib/engine/app/input/App.Input.Listener.cppm)
for the shape and hierarchy:

```cpp
// ------------------------------------------------------------------
// input context - route input messages to subscribed listeners
// ------------------------------------------------------------------

class InputContext final : public safe_object {
```

Within that class, `// listeners:` and `// input events:` label member subgroups;
they do not need full banners. A section header precedes the section's attached
API documentation; it never splits a `///` block from its declaration. Keep a
blank line around a major banner. Use names that describe the area's role, and
extend the section structure when adding a new area. Ordinary local algorithm
steps use paragraph breaks; a header per function or statement would hide the
hierarchy. Tiny umbrella/forwarding files need no invented section.

## What validation establishes

The author supplies paragraphs, declaration groups, and headers in the first
implementation. `format_cpp.py` handles mechanical layout; the lexical style
checker catches its defined token patterns. Neither can infer an algorithm's
logical steps or certify meaningful section names. A missing boundary or header
remains a defect after both scripts pass. Inspect complete touched bodies and
declaration blocks after formatting, using the existing author/reviewer workflow
in [validation](../.opencode/skills/validation/SKILL.md) and
[code-reviewer](../.opencode/skills/code-reviewer/SKILL.md); no separate review
agent per formatting concern or repeated build is needed.
