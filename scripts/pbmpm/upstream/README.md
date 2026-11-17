# Vendored upstream: Electronic Arts PB-MPM

Provenance record for everything under `scripts/pbmpm/upstream/`. The tool that
runs this code is documented in [`../README.md`](../README.md).

## Upstream

| Field | Value |
|---|---|
| Repository | <https://github.com/electronicarts/pbmpm> (Electronic Arts PB-MPM WebGPU demo) |
| Commit (pin) | `61749b6c2d2631ed243f86882752ba43196f2de7` |
| Commit subject | Fix rendering smoothstep function in new version of Dawn |
| Commit date | 2024-10-17 |
| Licence | BSD-3-Clause, see [`LICENSE.md`](LICENSE.md) |

Every file below is a byte-identical copy of that commit. They were taken from a
clean checkout whose working tree matched `HEAD`, and the sha256 values were
recorded before and after the copy (they agree, and agree with
`git show <pin>:<path> | sha256sum`). Do not edit them: a local edit would
silently invalidate the oracle.

## Vendored files and sha256

| Path | sha256 |
|---|---|
| `LICENSE.md` | `7bb46d23e3920a130d94a23a5a134a870ba6087b76802e91a9fd61ee673fd5f3` |
| `src/buffer_factory.js` | `c4df6b4b5658c41e0e775ec8b46c6ad1ea6f460dc2a8b2221a5187ca6a9b8e11` |
| `src/shader.js` | `fece72d1b7aed5cf2038e1b59fd703ec4ab85a980492cc15d5f74939905f2ec0` |
| `src/sim.js` | `72c8da0f79ad92a2084fd1039eb5827d1d975d4ed78142c3eb5225806493caf0` |
| `src/time.js` | `7029079e168861fe11d29ed76b9a1d5d2f9d14618057b48a5fd21b821161e66e` |
| `src/v.js` | `04f9263dc8d819e84a87e063de06f6c70431ed55821ee1cbc74fe6b894887374` |
| `shaders/bukkitAllocate.wgsl` | `e2616f593d5b04675e1207fb05796436b694f671102e16b3bd17b661239bac07` |
| `shaders/bukkitCount.wgsl` | `c34821fc6138f79c6fbe9bdd0ab42652104221fb2113844a36bebbe9da0af54b` |
| `shaders/bukkit.inc.wgsl` | `62d7b48c06799446aa6cdac29e61064b9525c7e4770db57ec7021a807a152671` |
| `shaders/bukkitInsert.wgsl` | `528a2767592633a86be0828a3bcae7004c228a9a728ef61a005912e7e8428e1e` |
| `shaders/dispatch.inc.wgsl` | `e384a6bb3866ade7d40035cc3c831b4f2daa66b25faac7b1195bbb282833de18` |
| `shaders/g2p2g.wgsl` | `8dc2105ad520988bcc27ca435aac1364d83cfd77b6530b8a3f8071c1f1995250` |
| `shaders/matrix.inc.wgsl` | `ac289b3d85ed2e880bcbddc34580f5953107339850380019027d3373c711ec0b` |
| `shaders/particleEmit.wgsl` | `53adbc12c24f42481f0fa43e54d292eb28604f5d5a673729b64d681f50c67a3c` |
| `shaders/particle.inc.wgsl` | `116c5a333fe9d685da2a992447fe3102c0860e12b685ffe45095ad349a3442e2` |
| `shaders/particleRender.wgsl` | `7559316286abc107dee5dfac29fd2d13a5c5feb3384b3051ced10de90240a732` |
| `shaders/random.inc.wgsl` | `9c133cfc09ae2fc72161c4147a2dd142dec98a8ffee925d53e3cc6661c0396d9` |
| `shaders/setIndirectArgs.wgsl` | `2c27f6e911cd4f6668d7b302b793a793ae96d70697ea0bd934b0390b99662fe7` |
| `shaders/shapes.inc.wgsl` | `eb151f603f4d72a8413ed00e663de8c4e649682d1e2d8e537c37bb4f2477d393` |
| `shaders/simConstants.inc.wgsl` | `31eb3252d58ca8a0c17665023e12b47660faf231a0cf254f7a146bebc5138b38` |

`shaders/` is vendored in full (all 14 `.wgsl` files, including the three
reference-only ones the simulation never dispatches: `particleRender.wgsl` is
compiled by `shader.js` but never rendered, and the `*.inc.wgsl` files are
preprocessor includes reached through `//!include`). The trace records the sha256
of every one of them, so the whole set has to be present and unmodified.

Verify the record from the repository root:

```bash
cd scripts/pbmpm/upstream && sha256sum LICENSE.md src/*.js shaders/*.wgsl
```

## What was not vendored

| Upstream path | Reason |
|---|---|
| `src/gpu.js` | Replaced. Upstream's version is DOM/canvas/render-pipeline bound; the compute-only replacement lives at `scripts/pbmpm/src/gpu.js`. |
| `src/main.js` | Browser driver: canvas, `requestAnimationFrame`, pointer input, UI wiring. |
| `src/ui.js` | Browser control panel. |
| `src/render.js` | Particle render pass and `RenderConstants` producer. |
| `index.html`, `style.css`, `_config.yml`, `Gemfile`, `*.bat`, `VERSION.md` | Site/host scaffolding with no effect on the simulation. |
| `data/`, `scenes/` | Demo content for the interactive scene loader; the trace seeds its own particle block. |
| upstream `README.md` | Its prose describes the browser demo and its hosting; this file replaces it as the provenance record. |

## What was replaced

One upstream module has a substitute, and one file here is not upstream at all.

- **Replacement:** `scripts/pbmpm/src/gpu.js` provides the compute-only subset
  that `src/sim.js` consumes (`divUp`, `construct4IntBuffer`, `computeDispatch`,
  `beginFrame`, `endFrame`, `getGpuContext`) plus the trace hooks (snapshot copy
  list, indirect-args probes, dispatch log). Its header lists every difference
  from upstream. `sim.js` itself is untouched.
- **Shim:** `upstream/src/gpu.js` is **not** an upstream file. The verbatim
  `sim.js` imports `./gpu.js`, so this one-line `export * from
  "../../src/gpu.js"` keeps that specifier resolvable and keeps the replacement
  and the runners on the same module instance. It is listed in the layout of
  `../README.md` and must not be confused with upstream's `gpu.js`.

Shaders are unmodified and are still loaded through the `fetch` shim in the
runners (`fetch('shaders/<name>.wgsl')` → `readFileSync`), so upstream's own
`//!include` / `//!insert` preprocessing runs exactly as it does in the browser.
