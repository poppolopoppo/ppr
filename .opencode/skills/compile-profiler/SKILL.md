---
name: compile-profiler
description: >
  Profiles compilation times of the PPR engine using /Bt+ or vcperf
  timetrace, parses per-file front/back-end timings, generates structured
  reports, and compares against saved baselines. Use this skill whenever
  the user says "profile the build", "measure compile times", "check
  compilation performance", "regress compile times", "is the build
  slower", or "run the profiler".
---

# Compile Profiler

Profile per-file compilation times, generate reports, and compare against
baselines to detect regressions. Two collection methods:

| Method | Data | Admin needed | Speed |
|--------|------|-------------|-------|
| `/Bt+` | Per-file front/back/total | No | Fast |
| `vcperf timetrace` | Chrome-trace JSON with full event graph | Yes (admin+VS shell) | Slow |

---

## Step 1 — Profile

Run the collection script from the repo root:

```powershell
# Quick: /Bt+ per-file timing (no admin)
.opencode/skills/compile-profiler/profile.ps1 `
    -Target EngineCore -Label my_run_label

# Full: vcperf timetrace (opens elevated window)
.opencode/skills/compile-profiler/profile.ps1 `
    -Target EngineCore -Collect timetrace -Elevated -Label my_run_label
```

**Key flags:**

| Flag | Purpose |
|------|---------|
| `-Target` | Build target: EngineCore, EngineTests, EngineApp, VideoGameApp |
| `-Label` | Short name for the run (used for baselines) |
| `-Clean` | Clean build (default: true). Pass `-Clean:$false` for incremental |
| `-Jobs 1` | Serial builds give reproducible timing. Increase for speed runs |
| `-Collect timetrace` | Full vcperf trace (JSON). Requires `-Elevated` on this system |
| `-Elevated` | Launch an elevated window (required for vcperf) |

**Profiling data is saved to:** `.opencode/profile/runs/<label>_<timestamp>/`

---

## Step 2 — Analyze

Parse the collected data and generate a report:

```powershell
# Parse /Bt+ output
python .opencode/skills/compile-profiler/analyze.py `
    --btplus .opencode/profile/runs/<run_dir>/btplus_output.txt `
    --label my_run

# Parse vcperf timetrace JSON
python .opencode/skills/compile-profiler/analyze.py `
    --timetrace .opencode/profile/runs/<run_dir>/timetrace.json `
    --label my_run
```

**Output includes:**

- **Category breakdown** — std modules, partition interfaces (.cppm),
  implementation files (.cpp), test modules, and dependency scanning
- **Top 20 slowest files** — sorted by total compile time with
  front/back-end split
- **Totals** — overall, front-end, back-end, and category percentages

**Key flags:**

| Flag | Purpose |
|------|---------|
| `--btplus <file>` | Parse /Bt+ build log |
| `--timetrace <file>` | Parse vcperf timetrace JSON |
| `--label <name>` | Label for this run |
| `--save <name>` | Save as a named baseline for future comparison |
| `--compare <name>` | Compare with a previously saved baseline |
| `--list` | List all saved baselines |
| `--json` | Raw JSON output (for machine consumption) |

---

## Step 3 — Save Baselines

After analyzing a run, save it as a named baseline:

```powershell
python .opencode/skills/compile-profiler/analyze.py `
    --btplus .opencode/profile/runs/<run_dir>/btplus_output.txt `
    --label "post-refactor" --save "post-refactor"
```

Baselines are stored in `.opencode/profile/baselines.json`. Each contains
total time, category breakdown, and top-20 slowest files.

---

## Step 4 — Compare & Regress

Compare a new run against a saved baseline:

```powershell
python .opencode/skills/compile-profiler/analyze.py `
    --btplus .opencode/profile/runs/<new_run>/btplus_output.txt `
    --label "after-optimization" --compare "post-refactor"
```

Output shows per-category deltas with ▲/▼ indicators. Use this to detect
whether changes improved or regressed compile times.

List all available baselines:

```powershell
python .opencode/skills/compile-profiler/analyze.py --list
```

---

## Interpretation Guide

### What the numbers mean

- **Front-end time**: Parsing, template instantiation, name lookup, BMI
  loading. Dominates C++ module builds (~89% of project code time in
  profiling). High front-end usually means expensive includes, excessive
  templates, or large BMIs.
- **Back-end time**: Code generation, optimization, and emission. Low
  relative to front-end in module builds (~4%).
- **I/O / overhead**: File reading, dependency scanning, dyndep processing
  (~7%).

### Red flags

| Flag | What it means |
|------|---------------|
| A `.cppm` file in top 10 slowest | Partition interface is too heavy; move bodies to `.cpp` |
| Std modules >8s | Consider whether all TUs need `import std;` vs. targeted imports |
| A test `.Tests.cppm` file >5s | Consider splitting the mega-import aggregator |
| File count in "dependency_scanning" high | Too many small TUs; consider unity builds |
| Baseline delta >+10% | Regression introduced — bisect recent changes |

### Typical baseline values (msvc-dev, clean serial build)

| Target | Total | Std modules | Partition interfaces | Implementations |
|--------|-------|-------------|---------------------|-----------------|
| EngineCore | ~39s | ~7s | ~4.5s | ~18s |
| EngineTests | ~47s | ~7s | ~14s (test modules) | ~4s |

---

## Workflow Example: Regression Check

```powershell
# 1. Profile
.opencode/skills/compile-profiler/profile.ps1 -Target EngineCore -Label regression_check

# 2. Analyze + compare against saved baseline
python .opencode/skills/compile-profiler/analyze.py `
    --btplus .opencode/profile/runs/regression_check_*/btplus_output.txt `
    --label "regression_check" --compare "post-refactor"
```

If no baseline exists yet, run the profile once with `--save` to create one.

---

## Tips & Known Issues

- **Serial builds (`-Jobs 1`)** give reproducible, comparable timings.
  Parallel builds are noisy due to resource contention.
- **Clean builds** measure the full compilation cost. Incremental builds
  measure only changed files — useful for iterating, not for baselines.
- **vcperf** on this system must run in an elevated window. The
  `-Elevated` flag handles this by spawning `RunAs`.
- Lingering ETW sessions block vcperf. The profile script cleans them
  with `vcperf /stop PPR_Core 2>$null`. If that fails, run:
  `tracelog -stop MSVC_BUILD_INSIGHTS_SESSION_PPR_Core`
- The `/Bt+` output includes lines for every CL invocation, including
  dependency scanning passes. The analyzer classifies these by file
  extension and name patterns.
- Paths in the timetrace JSON use `/` separators. The analyzer
  normalizes backslashes to forward slashes for cross-platform
  consistency.
