# Read-only CPU timing queries

Use this route for named CPU scopes when the user requests file/JSON analysis.
The [Microsoft query interface](https://devblogs.microsoft.com/pix/querying-the-pix-timing-capture-file-format/)
uses 64-bit SQLite and the trusted installed PIX storage extension, normally
`PixStorage.dll`, initialized through `sqlite3_batchexpand_init`. This is a
documented local interface; Developer Mode requirements for PIX replay verbs do
not apply to these read-only queries. Do not substitute an unknown DLL or private
interface. Keep the original `.wpix` authoritative and unchanged.

## Select the process before querying events

An OS PID can be reused within one capture. `Processes.Id` identifies the
captured process instance; `Processes.ProcessId` is the OS PID. Select both,
using the process image and start timestamp as corroborating metadata. The
helper requires `--pid`; it selects a unique matching instance or rejects
ambiguity until `--process-instance` is supplied. Never merge every process
with the same executable name. Threads are isolated by captured `Threads.Id`.
`Threads.ProcThreadId` is retained as opaque `pix_proc_thread_id` metadata, not
labelled or decoded as an OS thread ID. Chrome lanes use `Threads.Id`.

Start with selected-process/thread marker queries. Inspect the installed schema
before requesting `PixCpuExecutionTimes` for a specific question; avoid an
expensive all-event timing join. Keep its Duration, Execution, and Stall values
distinct and preserve their reported units. Marker wall duration alone is
neither CPU execution nor time stalled.

## Use the maintained marker helper

[query_timing_capture.py](../scripts/query_timing_capture.py) adapts the reviewed
`PixCpuExecution` marker query. Use a 64-bit Windows Python whose SQLite supports
extension loading. Supply the actual trusted installed DLL and a new per-capture
output directory. With verified values assigned to the variables:

```powershell
rtk proxy python .opencode/skills/pix-profiling/scripts/query_timing_capture.py `
  --capture $capture --storage-dll $storageDll --pid $targetPid `
  --process-instance $capturedInstance --output-dir $newOutput --chrome-trace
```

Omit `--chrome-trace` for an analysis summary alone. The helper validates runtime
and DLL architecture, required columns, process selection, and capture bounds.
Its current adapter uses Microsoft's documented `CaptureFacts` IDs
2 (`CaptureFirstReliableTime`) and 24 (`CaptureStopTime`) for start/end
nanoseconds; missing or invalid bounds stop the query rather than guessing.
Other installed formats require schema inspection before adapting that mapping.
Connections open with `mode=ro`, close on every path, and disable extension
loading immediately after the one explicit load.

Outputs are `analysis.json` and, when requested, `markers.trace.json`. A new
directory and atomic, no-clobber file publication prevent silently replacing
previous evidence. On failure, inspect any partial output before choosing a
fresh directory; do not delete or overwrite existing evidence to make a retry
succeed. Source and DLL paths/hashes/timestamps, selected process, window, units,
and coverage are retained in the analysis. Keep workload/source/build identity
in the surrounding validation record.

## Interpret the output

- Source timestamps and aggregate durations are integer nanoseconds. Chrome
  timestamps/durations are microseconds relative to capture-window start;
  `displayTimeUnit` changes presentation only.
- Missing/noninteger or inverted intervals are invalid. Zero-length, outside,
  and clipped intervals have separate counts. Integer negative/zero edges are
  accepted; no undocumented missing-timestamp sentinel is assumed.
- Counts describe intersecting intervals, including partial invocations at the
  window boundaries. Clipping does not reconstruct missing events or ancestors.
- Exclusive **wall** time subtracts direct-child interval unions relative to
  the observed valid named-marker topology within one captured thread, rather
  than reconstructing whole-call time. Recursion remains distinct during
  subtraction. Invalid intervals, unresolved marker names, duplicate intervals,
  crossings/overlapping siblings, or observed depth gaps make that
  thread's exclusive values `null`/`UNRELIABLE`; aggregates containing such values
  also remain unreliable. Invalid rows without a captured thread identity
  conservatively invalidate all threads. Rows provably outside the window do
  not taint it. Inclusive durations of retained named markers remain usable.
  The minimum observed level is the thread's root level; missing outer ancestry
  is not reconstructed.
- Inclusive nested durations and work on overlapping threads are not independent
  contributions to elapsed capture time. A low exclusive wall value is not a
  CPU-cost measurement. Summed parallel marker wall time can exceed the capture
  window; it neither measures CPU execution nor predicts whole-test elapsed time.

The query retains selected-process rows linked through `Threads`, including
unresolved marker-name metadata for invalid/incomplete accounting. The derived
Chrome JSON exports only positive named CPU marker intervals with resolvable
metadata. It does not export GPU timings, sampled callstacks, or a full-system
sample dump. Optional table availability is metadata, not proof that samples or
stacks are absent/present in the selected window. Preserve original stack data;
the public SQL article does not establish a `CpuSample`-to-`Stacks` mapping, so
do not invent one. If another needed query is unavailable, report that specific
limitation; UI inspection remains conditional on the user's permission.

The pure regression checks require only the standard library:

```powershell
rtk proxy python -m unittest discover -s .opencode/skills/pix-profiling/scripts -p test_query_timing_capture.py
```

They cover interval accounting, malformed nesting, process selection and output
preservation. They do not validate a particular installed DLL/capture; perform
that local integration check separately, outside timed workload comparisons.
