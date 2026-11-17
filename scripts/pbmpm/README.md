# PB-MPM offline upstream oracle

Deterministic, headless execution of the **upstream** Electronic Arts PB-MPM
WebGPU demo (particle simulation only), used on this project as an
**oracle**: it produces reference traces from unmodified upstream code so that
the PPR port can be compared against it.

Two entry points:

- **Stage 1** — one Liquid case, `iterationCount = 2`, one simulated substep:
  a single JSON trace.
- **Stage 2** — 4 material families × `iterationCount ∈ {2, 5, 10}` × 8 simulated
  substeps: a comparable trace set with per-substep particle and grid snapshots.

This is an **offline analysis tool**. It is never invoked by CMake, never built as
part of the engine, and no build or test gate depends on it. Nothing here is
linked into, imported by, or executed by PPR at build time or run time.

## Upstream provenance and licence

| Field | Value |
|---|---|
| Repository | <https://github.com/electronicarts/pbmpm> |
| Commit (pin) | `61749b6c2d2631ed243f86882752ba43196f2de7` |
| Commit subject | Fix rendering smoothstep function in new version of Dawn |
| Licence | BSD-3-Clause |

Upstream is licensed under the BSD-3-Clause licence. The full notice is vendored
verbatim at [`upstream/LICENSE.md`](upstream/LICENSE.md) — read it before
redistributing; it requires retaining the copyright notice, the conditions, and
the disclaimer. Per-file sha256 values, the list of vendored files, and the list
of deliberately excluded/replaced upstream files are in
[`upstream/README.md`](upstream/README.md).

The vendored upstream sources are byte-identical to the pinned commit and must
stay that way. Only [`src/gpu.js`](src/gpu.js) is ours (its header states that it
replaces upstream `gpu.js` and lists the differences), and
`upstream/src/gpu.js` is a one-line resolution shim so the verbatim `sim.js`
resolves its `./gpu.js` import.

## Dependency

One runtime dependency: [`webgpu`](https://www.npmjs.com/package/webgpu) — the
Dawn Node bindings — pinned exactly to `0.6.2` in
[`package.json`](package.json).

```bash
cd scripts/pbmpm
npm install          # first run only; needs network access
```

`webgpu`'s install script (`postinstall`) downloads a prebuilt Dawn binary for
the host platform. That download is why the first install needs network access;
afterwards the tool runs fully offline. Node 22.x was used for the recorded
traces.

`node_modules/` is **not** vendored and must not be committed (see
[`.gitignore`](.gitignore)). No lockfile is committed either: the exact version
pin in `package.json` fixes the direct `webgpu` package version only. It does not
lock transitive dependencies or the Dawn artifact downloaded by its install
script, so it does not guarantee an identical dependency installation.

## Invocation

Stage 1 — one Liquid trace, `iterationCount = 2`, one simulated substep:

```bash
cd scripts/pbmpm
node run.mjs --out <output-directory>/liquid-k2-s1.json
```

Stage 2 — the 4 × 3 × 8 case matrix:

```bash
cd scripts/pbmpm
node stage2.mjs --out-dir <output-directory>/stage2
```

Both scripts also work from any working directory using an absolute script path;
output paths are resolved against the current directory, so pass explicit paths.

Useful flags (see the header of each script for the full list):

| Script | Flag | Meaning |
|---|---|---|
| `run.mjs` | `--out FILE` | trace file (default `out/liquid-k2-s1.json`) |
| `run.mjs` | `--steps N` | frames to simulate (default `1`; diagnostics only) |
| `stage2.mjs` | `--out-dir DIR` | case directory (default `out/stage2`) |
| `stage2.mjs` | `--families a,b` | restrict to `liquid,elastic,sand,visco` |
| `stage2.mjs` | `--iterations 2,5` | restrict `iterationCount` |
| `stage2.mjs` | `--steps N` | simulated substeps per case (default `8`) |

`PBMPM_UPSTREAM=/path/to/pbmpm` overrides the vendored upstream tree with another
checkout, e.g. to re-derive the traces at a different commit. The default is the
vendored `upstream/` directory and needs no environment variable.

### Self-checks

```bash
cd scripts/pbmpm
bash verify.sh          # stage 1: two processes, byte comparison of the traces
bash verify-stage2.sh   # stage 2: emit twice, compare all cases, re-check stage 1
```

`verify.sh` writes `out/run-a.json` and `out/run-b.json` and `cmp`s them.
`verify-stage2.sh` regenerates `out/stage2` and `out/stage2-repeat`, compares every
file with `tools/compare-dirs.mjs` (`timings.json` is excluded by design: it holds
wall-clock durations), then re-runs stage 1 and compares it against the existing
`out/liquid-k2-s1.json`. Run `node run.mjs --out out/liquid-k2-s1.json` once
before the first `verify-stage2.sh`.

Both gates are POSIX shell scripts (`bash verify.sh`); on Windows run them from Git
Bash or WSL. The two `node` runners themselves are platform independent, so on
Windows the individual commands from the gates can be run directly.

## Output convention

Traces are generated artifacts: **write them outside the source tree**, e.g. into
the repository's ignored `out/` directory or a build/scratch directory, and never
commit them.

```bash
cd scripts/pbmpm
node stage2.mjs --out-dir ../../out/pbmpm/stage2
```

The default output directory of both scripts (`scripts/pbmpm/out/`) exists only as
script-local scratch for the two `verify*.sh` gates and is git-ignored. Stage 2
also writes `index.json` (case list with sha256 per case) and `timings.json`
(durations, excluded from byte comparisons) next to the case files.

## Determinism rule

The traces are **byte-identical across runs** of the same case on one host with
one GPU driver: two processes, same inputs, same bytes. Nothing is rounded,
reordered, or timestamped on the way out, and `verify.sh` /
`verify-stage2.sh` enforce exactly that rule.

The comparison is valid within one host and one driver/compiler pairing:

- moving to a different GPU, driver version, OS, or Dawn build may change
  floating-point results or dispatch shapes; a re-emitted trace then becomes the
  new reference for that host,
- **a driver or shader-compiler change is a trace-format bump**: the recorded
  traces for the old pairing must be regenerated and the version of the tool
  bumped, because existing traces are no longer comparable. Do not mix traces
  produced under two different driver/compiler pairings in one comparison,
- provenance that would vary per host or per checkout (absolute paths, host
  repository HEAD) is deliberately not recorded: the trace names the vendored
  tree as `scripts/pbmpm/upstream` and records the pinned commit, so traces stay
  comparable across checkouts of the same commit.

## Layout

| Path | Role |
|---|---|
| `README.md` | This file: provenance, invocation, output and determinism rules |
| `package.json` | Exact `webgpu` pin, npm script shortcuts, dependency notes |
| `.gitignore` | Excludes `node_modules/`, the default `out/`, and lockfiles |
| `run.mjs` | Stage-1 runner: headless device, seed, one substep, readback, JSON |
| `stage2.mjs` | Stage-2 runner: family/iteration/substep matrix and snapshots |
| `verify.sh` | Stage-1 determinism gate (two processes, byte compare) |
| `verify-stage2.sh` | Stage-2 gate (emit twice, compare, re-check stage 1) |
| `tools/compare-dirs.mjs` | Byte comparison of two stage-2 output directories |
| `src/gpu.js` | **Replacement** for upstream `gpu.js`, compute-only, plus trace hooks |
| `upstream/README.md` | Vendored-file record: pin, sha256 per file, exclusions |
| `upstream/LICENSE.md` | Upstream BSD-3-Clause notice, verbatim |
| `upstream/src/*.js` | Verbatim upstream `sim`, `shader`, `buffer_factory`, `v`, `time` |
| `upstream/src/gpu.js` | Resolution shim (not upstream): forwards to `../../src/gpu.js` |
| `upstream/shaders/*.wgsl` | All 14 verbatim upstream shaders |

## Repository conventions

What this repository does for tool scripts, and what this directory follows:

- Single-file tools live flat in `scripts/` (`format_cpp.py`,
  `check_semantic_style.py`, `generate_thermal_lut.py`, `profile_lighting.py`)
  with snake_case names; legacy PowerShell tools keep their PascalCase
  verb-noun names. A multi-file tool gets its own directory
  (`scripts/game-control/`) holding the entry point plus its dependency file.
- There is no central `scripts/README.md` and no `codemap.md` under `scripts/`;
  documentation lives next to the tool. This directory therefore keeps its
  documentation in `scripts/pbmpm/README.md`, and the third-party provenance
  record in `scripts/pbmpm/upstream/README.md` (the same "README next to the
  data" pattern as
  `scripts/tests/fixtures/lighting-profile/README.md`).
- Python tools declare their dependencies in a sibling `requirements.txt`
  (`scripts/game-control/requirements.txt`); the Node equivalent here is
  `package.json` with the exact pin.
- Generated output is not committed anywhere in `scripts/`; tools take an
  explicit output path or write to an ignored scratch directory, which is what
  the flags and `.gitignore` above do.

The `.mjs` extension is kept from the verified harness (Node ESM, no build step);
the tool name `pbmpm` is the upstream project name and already snake-free, so the
directory follows the `scripts/<tool>/` convention as-is.
