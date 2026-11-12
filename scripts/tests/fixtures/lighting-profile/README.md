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
  eight per-frame samples. Vendor tag names (including
  `.avg.pct_of_peak_sustained_elapsed`, `.avg.per_cycle_elapsed` and `.sum`
  suffixes) and values are unmodified.
- `D3DPERF_EVENTS.xls` — the observed marker table: 28 rows with eight
  per-frame durations (milliseconds). It retains the production nesting
  (`scene.stage → scene.content → Scene.Materials/Scene.Gas → Particles.Draw`,
  `ppr → Present.Scene`, `ppr → Present.UI`) and every design-contract range:
  `WorldLighting.ResolveCells`, `.Visibility`, `.Jump`, `.Source`,
  `.Cascade.L5`–`.L1`, `.FineCH`, `Scene.Materials`, `Scene.Gas`,
  `Hable.Resolve`, `Post.Exposure`, `Post.Bloom`, `Post.AgX`, `Present.Scene`,
  `Present.UI`. `Scene.Materials` appears twice per frame: the opaque material
  draw, then (after `Scene.Gas`) the translucent material draw, matching the
  unchanged production order background → opaque → particles/gas → translucent.

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
around the first presented frames (1,024 API rows, 7 workload rows on the
current fixture). `StringIds` retains only the ids those rows reference. Heavy
event tables and unused schema tables were dropped. A Systems capture requests
no GPU metric set, so its analysis reports `collection.metric_set: null`.

Attribution: NVIDIA Nsight Systems/Graphics and the Nsight Graphics SDK are
NVIDIA products; see `third_party/README.md` for the SDK license record.
