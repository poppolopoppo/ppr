# Lighting profiling fixtures

These fixtures are retained **real vendor exports** trimmed to the rows the
pinned adapters read. They exist so the offline parser tests never depend on a
live GPU, a licensed tool session, or a synthetic stand-in for the vendor
schema.

## `graphics/BASE_UNLOCKED/`

Source: an Nsight Graphics 2026.4.0.0 (`build 39228586`) GPU Trace Profiler
export of the real `app.game` on this host (RTX 2080 / TU104, driver
620.70.06), requested with `--set-gpu-clocks=unaltered`, `--limit-to-frames=8`,
`--max-duration-ms=2000`, metric set `Throughput Metrics`.

- `REPRO_INFO.xls` — tab-separated key/value export metadata. Only the records
  used for identity/collection are retained; all values are unmodified.
- `FRAME.xls` — the observed `GPU frame time` row with its eight per-frame
  values (milliseconds).
- `GPUTRACE_FRAME.xls` — the first three observed metric rows verbatim, each with
  eight per-frame samples. Vendor tag names (including the
  `.avg.pct_of_peak_sustained_elapsed`/`.sum` suffixes) and values are
  unmodified.
- `D3DPERF_EVENTS.xls` — the observed marker table: `ppr` and its nested
  `scene`/`ui.detached` rows with eight per-frame durations (milliseconds).

Not retained (present in a real capture, not read by the adapter and too large
to commit): `ReportGeneratorTags.txt`, `GPUTRACE_REGIMES.xls`, the
`.ngfx-gputrace` binary report.

## `graphics-comma/BASE_UNLOCKED/`

Synthetic locale variant of the fixture above: every numeric field uses a
decimal comma and the same tab-separated structure. It exists only to prove the
adapter parses a **declared** decimal-comma export and rejects an ambiguous one;
it is not a claim about any vendor's locale default. No real decimal-comma
vendor export was produced on this host.

## `systems/systems.sqlite`

Source: an Nsight Systems 2026.5.1.161 SQLite export
(`EXPORT_SCHEMA_VERSION` 3.31.0) of the real `app.game`. The schema of
`META_DATA_EXPORT`, `StringIds`, `PROCESSES`, `ThreadNames`, `VULKAN_API` and
`VULKAN_WORKLOAD` is copied verbatim; `META_DATA_EXPORT`/`PROCESSES`/
`ThreadNames` are the real rows, and the API/workload tables keep a real window
around the first presented frames. `StringIds` retains only the ids those rows
reference. Heavy event tables and unused schema tables were dropped.

Attribution: NVIDIA Nsight Systems/Graphics and the Nsight Graphics SDK are
NVIDIA products; see `third_party/README.md` for the SDK license record.
