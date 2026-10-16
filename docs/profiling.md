# Runtime profiling

[AGENTS.md](../AGENTS.md#runtime-cost) owns profiling coverage and cost policy.
The [PIX profiling skill](../.opencode/skills/pix-profiling/SKILL.md) owns capture
and analysis procedures. This guide explains the existing HAL markers and how
to choose useful boundaries.

## What the helpers measure

`pP::hal::profileBegin(name, category)` and `profileEnd()` bracket a CPU duration
event on the calling thread. `hal::ProfileScope` pairs them through construction
and destruction. Windows emits PIX events; Linux, Darwin, and Generic implement
no-op backends. See [the interface](../lib/engine/core/hal/Core.HAL.cppm) and
[Windows implementation](../lib/engine/core/hal/windows/Core.HAL.windows.Debugger.cpp).

On MSVC Release, `msvc-profile` enables `PPR_ENABLE_PROFILING`, which defines
`USE_PIX=1`; ordinary Release does not enable that flag by default. Debug is
unaffected by this Release-only flag and follows the PIX headers' Debug behavior.
Verify the effective build configuration. A disabled capture or no-op backend
does not prove that all caller/label construction costs disappear.

These scopes describe CPU elapsed time, including any waits inside the region.
A scope around draw encoding, queue submission, or presentation does not measure
GPU execution or prove GPU completion. Use GPU timing evidence for those claims.
Nested durations overlap; do not sum parent and child durations as independent
work, or sum parallel worker durations into wall time.

Names must be non-null/non-empty; categories must be null or non-empty. Both
strings are borrowed only during `profileBegin` and are not retained by the HAL.
Prefer stable descriptive literals such as `Sim.WorldGen.Terrain`. Categories
control color, not filtering or whether a marker is emitted. A null category
uses the first dotted name segment; `hal::profile_blocking_category` reserves red
for explicit CPU wait operations. Do not allocate or format per-item labels.

## Choose a boundary that answers a question

Keep a shallow hierarchy of phases that distinguishes likely causes of a
slowdown: frame update versus render execution, simulation systems, batch upload
versus encoding, world-generation phases, asset decode, and lifecycle operations.
A function is a suitable scope only when it is such an operation. Reuse an
existing parent if another scope would only repeat its interval.

```cpp
{
    const hal::ProfileScope scope{"Sim.WorldGen.Terrain"};

    for (const auto &chunk : chunks) {
        rasterizeChunk(chunk);
    }
}
```

The example marks the batch once. A scope inside that loop would multiply the
event rate by the chunk count; a per-cell scope would multiply it again. Getters,
math/noise helpers, container operations, and allocator primitives do not need
scopes by default. Temporary detail is appropriate for a concrete investigation;
remove it afterward unless its ongoing diagnostic value justifies its cost.

Texture loading follows the same rule: retain named RAII scopes around useful
aggregate phases such as batch loading, decoding, and upload preparation when
they distinguish the cause of a slowdown. Do not add an event for every texture
or helper call merely to increase coverage. Preserve existing useful phase names.

Definition loading is separate from texture preparation. `Content.Sources.Load`
measures owned text loading, while image worker stages use
`Image.Preparation.Base`, `Map`, `HashSource`, `Decode`, and `Mips` under the
`Image.Preparation` prefix. `Content.Presentation.Join` identifies composition of
completed image results. Shared request waits use `Asset.Preparation.Admit`,
`SettleCaptures`, and `Drain` under the `Asset.Preparation` prefix; polling and
worker execution are separate intervals. `Image.Mips.Resize` isolates stb resize
work. Existing encoding work is visible through `Core.Parallel.ForRange`;
inclusive and parallel intervals overlap. Historical `Content.Textures.Load`
and `Content.Texture.*` traces describe the removed synchronous loader and are
not directly comparable to the new worker phases.

Place scopes on the thread executing work. Submission, execution, and completion
are different boundaries; a submitter's scope cannot describe the worker's work.
Use separate scopes on each thread, never a begin on one and an end on another.
For parallel work, prefer an existing job or batch boundary over every element.
Production terrain generation uses `Sim.WorldGen.Terrain` for aggregate caller
elapsed time, `Sim.WorldGen.RasterJob` for each scheduled chunk job (a batch of
16,384 cells), and `Sim.WorldGen.WaitForRaster` for the caller's completion wait.
The serial path retains aggregate coverage. This bounded job-level detail
distinguishes worker execution and imbalance without marking individual cells.

Within raster work, `Sim.WorldGen.Raster.Sample` covers sampling and cell
preparation, `Sim.WorldGen.Raster.Relax` covers the complete pass/detail phase,
and `Sim.WorldGen.Raster.Output` covers the final cell/deposit copy.
Preliminary column/biome setup remains in aggregate
coverage. The direct no-pass `Sample` also includes output allocation; the general
path allocates buffers outside its child scopes. Interpret those boundaries
before comparing phases or adding temporary detail.

Each `ParallelScheduler` worker also emits `Core.Scheduler.ExecuteTask` around
task execution through its safety net. Idle signal/channel waits and completion
bookkeeping stay outside that interval. Domain-specific child scopes identify
the task's content; their durations overlap with the scheduler parent.
`Core.Parallel.ForRange` brackets the public `parallel::forRange` operation,
including caller participation and waiting for dispatched work. `parallel::forEach`
forwards through that boundary and needs no duplicate scope. Worker execution
remains visible through the separate scheduler scopes.

Nested `forRange` on a worker of the same pool executes inline, avoiding a worker
waiting on its own pool. The synchronous helper drains accepted work on exception
unwind before borrowed caller state is destroyed. This lifetime guarantee does
not make unhandled worker exceptions or shared mutable diagnostics appropriate;
fallible texture jobs retain private outcomes for ordered handling after drain.

Keep a scope around a wait call narrow, with preparation and subsequent useful
work outside it. A predicate wait can return immediately, and some native wait
APIs also execute callbacks: label and interpret the interval as wait-call elapsed
time rather than claiming it is entirely time asleep. Preserve existing useful
names when changing categories; check trace-analysis consumers before renaming.

## Pair scopes safely

Use a named `const hal::ProfileScope scope{"Domain.Operation"};` in the intended
lexical region. An unnamed temporary is destroyed at the end of its expression.
RAII closes the scope on success, early return, cancellation, and exception
unwinding. Fail-fast process termination does not unwind scopes.

Manual pairs are appropriate only when an established lifecycle cannot use one
lexical scope, as in the test runner's start/stop hooks. Prove same-thread,
balanced, properly nested pairing across every supported exit. Do not span
asynchronous handoffs or coroutine suspension, even if resumption might occur
on the same thread: unrelated work could interleave with the open scope.

## Verify usefulness and cost

For a changed subsystem, briefly record scopes added, retained, consolidated, or
omitted and the question that choice answers. This is part of the existing task
context, not a per-function inventory or a new tracking system.

Inspect a bounded representative timing capture for readable nesting, work on
the expected threads, and waits distinguishable from computation. CPU samples
can be disabled independently of PIX events; the
[Microsoft timing guide](https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/timing-captures/pix-timing-captures)
describes the options. The capture skill explains how to verify available
controls and what to report when tooling cannot enforce them.

Capture creation, the MCP response, and analysis are separate evidence. A response
error can occur after PIX writes an artifact; a nonempty artifact does not prove
that it opens or that CPU scopes were inspected. Record each outcome separately.
Native CPU timing queries are not exposed by the current MCP integration.
Microsoft's supported read-only timing-file queries can provide local evidence
and derived JSON without UI control; the capture skill owns that conditional
procedure and its coverage limits. Respect the user's chosen analysis mode and
report unavailable evidence rather than requiring UI inspection.

For test timing evidence, retain the exact binary, source revision/working-tree
state, effective configuration, loop/shuffle settings, and measured runtime.
A long-running suite's timeout does not establish a focused test's expected
runtime. The capture skill owns reproducible test setup and launch/attach steps.
For a focused capture, record the actual test process PID, exact leaf selector,
effective parameters and seed, and relevant output identity. Use a naturally
representative required run with its normal repetition count; do not add delays
or repeat a short leaf just to fill the capture interval. Select another required
workload if launch-and-attach cannot observe the phase of interest.

Before an algorithm change, retain deterministic baseline output checksums over
semantic fields, such as material IDs and temperature bit patterns, excluding
structure padding. Check the candidate against that baseline as well as meaningful
behavior assertions. Replaying the same implementation proves repeatability,
not equivalence between the previous and changed algorithms.

Numeric World identity excludes appearance and texture bytes. Joined presentation
identity includes numeric World identity, selected appearance, texture settings, source-byte identities
and processing revisions, but excludes generated mip bytes. Check owned mip bytes
independently when changing generation
or scheduling. The current mip path filters linear floats with stb's Mitchell
downsampling, clamp edges, and straight-alpha handling. A faster box/tiled path
would change output semantics; timing evidence alone does not authorize that
change or establish equivalent visual quality.

When granularity materially increases marker frequency, compare baseline and
candidate under matching workload, compiler/optimization, logging, warmup, and
capture settings. Retain event rate, trace bytes per second, runtime samples and
spread, and capture overhead. Compare capture enabled/disabled on the same
instrumented binary to assess collection perturbation; that alone does not measure
the cost of adding marker calls. Do not assume `msvc-rel` and `msvc-profile` differ
only in markers, since their logging settings also differ.

No universal event budget or duration threshold is imposed. A readable trace and
acceptable measured cost for the workload establish useful coverage. The HAL
no-fault test verifies callability, not trace quality or performance.
