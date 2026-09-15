# cmake/compiler/

## Responsibility

Compiler-specific CMake configuration for MSVC, clang-cl, Clang, and GCC. Applies toolchain flags, warning
sets, debug-info policy, sanitizer hooks, and the C++20-module synth-target consistency rules for the engine.

## Design

- **MSVC** (`MSVC.cmake`, primary toolchain): bootstraps the vcpkg toolchain from `VCPKG_ROOT` when no
  toolchain is preset; debug info is per-object Embedded (`/Z7`, ccache-friendly, no PDB contention), shared-PDB
  (`/Zi`) on Release, and `/ZI` under `PPR_EDIT_AND_CONTINUE` (Debug only); live EnC link set (all
  `PPR_EDIT_AND_CONTINUE`-scoped): `/ZI` + `/DEBUG:FULL` + `/INCREMENTAL` + `/OPT:NOREF,NOICF` + `/LTCG:OFF` +
  `/PDBTMCACHE`; live-only `/MDd` asserts (dynamic vcpkg triplet + DLL runtime; `/MT` fails configure with
  `LNK2038`); LNK4075 validators (`/INCREMENTAL:NO`, `/OPT:REF/ICF`, `/DEBUG:FASTLINK`, `/LTCG`) fail at
  configure time, not build; `/EHsc` (required by `import std`), `/utf-8`
  and `/bigobj` applied globally via genex `add_compile_options`.
- **Module-synth consistency**: `/bigobj` stays global (per-target use forks `@cmake_cxx_std` synth targets →
  "Disagreement of the location of the 'std' module"); release `/O2` + `/Ob2` (pinned) + `/GL` + `/Gw` +
  `/Zc:checkGwOdr` sit behind `PPR_RELEASE_PERF_FLAGS` (default ON) as separate Release-only genex elements — one combined string is quoted
  as a single argv token and `cl` rejects it (D9002); the same flags must reach module synth targets so BMI
  location matching holds. `/arch:AVX2` is opt-in via `PPR_ENABLE_AVX2` (default OFF, min-spec unchanged).
  `/Zc:__cplusplus` fixes the `__cplusplus` macro value.
- **Release link contract** (shipping, Release-genexed): `/GL` + `/LTCG` pairing, `/OPT:REF,ICF`,
  `/INCREMENTAL:NO`, `/DEBUG` with bare `/PDBSTRIPPED` (shippable `app.game.stripped.pdb`, ~2MB, beside the
  full internal `app.game.pdb`, ~28MB).
- **Warnings** (`PPR_PROJECT_WARNINGS_CXX`): `/permissive-`, `/W4` baseline, targeted `/wd…`/`/w1…` suppressions
  (incl. `/wd5050` for `_UTF8`-in-command-line vs module-command-line mismatches on `import std`); `/WX` appended
  under `PPR_WARNINGS_AS_ERRORS`; `/D_ANNOTATE_STL` under `PPR_ENABLE_SANITIZER_ADDRESS`.
- **Clang** (`Clang.cmake`): `-stdlib=libc++`, `-Wall/-Wextra` family, `-Werror` under `PPR_WARNINGS_AS_ERRORS`;
  **clang-cl** reuses it in MSVC-compat mode (`clang-cl-dev`/`clang-cl-rel` presets). **GCC** (`GCC.cmake`)
  extends Clang warnings — presets stay hidden: **no C++ modules support**.

## Flow

1. `Compilers.cmake` detects `CMAKE_CXX_COMPILER_ID`, includes the matching file.
2. Global genex `add_compile_options` apply per-compiler/config flags to every target including synth targets.
3. `setup_ppr_project()` layers `cxx_std_23` / `CXX_MODULE_STD ON` / warning sets / link edges per target.

## Integration

- Included from root `CMakeLists.txt` via `include(Compilers)`; flags propagate via `add_compile_options`.
- Preset side: `msvc-live` sets `PPR_EDIT_AND_CONTINUE=ON` (→ `/ZI` path); `PPR_RELEASE_PERF_FLAGS=OFF` there.
  `msvc-rel` sets `BUILD_TESTING=OFF` (test targets + `:unit_test` excluded from shipping configs).
- Workaround refs: root-scope `@cmake_cxx_std.lib` LNK2001 (see `cmake/external/` + AGENTS.md "CMake Version
  Tracking") is why `imgui`/`imgui.base` pin `CXX_MODULE_STD OFF`.

## Key Files

- `MSVC.cmake` — vcpkg bootstrap, `/Z7` vs `/Zi` vs `/ZI`, EnC link set (`/DEBUG:FULL`, `/INCREMENTAL`,
  `/OPT:NOREF,NOICF`, `/LTCG:OFF`, `/PDBTMCACHE`), release link set (`/LTCG`, `/OPT:REF,ICF`, `/INCREMENTAL:NO`,
  `/DEBUG`, `/PDBSTRIPPED`), `/EHsc`, `/utf-8`, `/bigobj`, `/O2` + `/Ob2` + `/GL` + `/Gw` + `/Zc:checkGwOdr`,
  opt-in `/arch:AVX2` (`PPR_ENABLE_AVX2`),
  `/Zc:__cplusplus`, `PPR_PROJECT_WARNINGS_CXX`, ASAN/`/WX` tails.
- `Clang.cmake` — `-stdlib=libc++`, warning family, `-Werror` gate (also serves clang-cl).
- `GCC.cmake` — Clang base + GCC-specific warnings (modules unsupported).
- `cmake/Compilers.cmake` — dispatcher + `setup_ppr_project()`.
