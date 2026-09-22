# cmake/compiler/

## Responsibility

Compiler-specific CMake configuration for MSVC, clang-cl, Clang, and GCC. Applies toolchain flags, warning
sets, debug-info policy, sanitizer hooks, and the C++20-module synth-target consistency rules for the engine.

## Design

- **MSVC** (`MSVC.cmake`, primary toolchain): bootstraps the vcpkg toolchain from `VCPKG_ROOT` only when
  `CMAKE_TOOLCHAIN_FILE` is unset; debug info is per-object Embedded (`/Z7`, ccache-friendly, no PDB contention), shared-PDB
  (`/Zi`) on Release, and `/ZI` (`EditAndContinue`) under `PPR_EDIT_AND_CONTINUE` (Debug only) via
  `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT`; live EnC link set (all
  `PPR_EDIT_AND_CONTINUE`-scoped, one genex element per flag so Ninja quoting stays one-token-per-flag):
  `/DEBUG:FULL` + `/INCREMENTAL` + `/OPT:NOREF,NOICF` + `/LTCG:OFF` +
  `/PDBTMCACHE` (`/DEBUG:FASTLINK` forbidden — LNK4075 with `/INCREMENTAL`); the `/MDd` runtime requirement is
  enforced in `cmake/VCPkg.cmake` (dynamic triplet + DLL runtime; `/MT` fails configure, `LNK2038` otherwise) and the
  LNK4075 validators (`/INCREMENTAL:NO`, `/OPT:REF/ICF`, `/DEBUG:FASTLINK`, `/LTCG`) live in root `CMakeLists.txt`
  and fail at configure time, not build; `/EHsc` (required by `import std`), `/utf-8`
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
  `-Wno-reserved-module-identifier` so the synthesized `std`/`std.compat` BMI precompiles (which inherit these
  PRIVATE options) succeed under `-Werror` — project code never declares a module named `std`, so no project
  diagnostic changes. Version-agnostic `libc++.modules.json` probe (no pinned LLVM major): `llvm-config --libdir`,
  `$LLVM_DIR`, and `clang --print-resource-dir` hints first, then well-known versioned/multiarch fallbacks
  (`llvm-22`/`llvm-20`, `x86_64-linux-gnu`, `llvm/lib`); `CMAKE_CXX_STDLIB_MODULES_JSON` is set only when the
  file exists, otherwise left unset for CMake default lookup;
  **clang-cl** reuses it in MSVC-compat mode (`clang-cl-dev`/`clang-cl-rel` presets). **GCC** (`GCC.cmake`)
  extends Clang warnings — presets stay hidden: **no C++ modules support**.

## Flow

1. `Compilers.cmake` dispatches on `MSVC`, then `CMAKE_CXX_COMPILER_ID` (`.*Clang` including clang-cl,
  `GNU`), and includes the matching `compiler/*.cmake` file.
2. Global genex `add_compile_options` apply per-compiler/config flags to every target including synth targets.
3. `setup_ppr_project()` layers `cxx_std_23` / `CXX_MODULE_STD ON` / warning sets / link edges per target.

## Integration

- Included from root `CMakeLists.txt` via `include(Compilers)`; flags propagate via `add_compile_options`.
  Cache interplay: `/Z7` embedded debug info keeps the `Cache.cmake` launcher safe (no shared-PDB contention);
  every `setup_ppr_project()` target opts back out via `ppr_disable_compiler_cache()` (module BMIs uncacheable).
- Preset side: `msvc-live` sets `PPR_EDIT_AND_CONTINUE=ON` (→ `/ZI` path); `PPR_RELEASE_PERF_FLAGS=OFF` there.
  `msvc-rel` sets `BUILD_TESTING=OFF` (test targets + `:unit_test` excluded from shipping configs).
- Workaround refs: root-scope `@cmake_cxx_std.lib` LNK2001 (see `cmake/external/` + AGENTS.md "CMake Version
  Tracking") is why `imgui`/`imgui.base` pin `CXX_MODULE_STD OFF`.

## Key Files

- `MSVC.cmake` — vcpkg bootstrap (toolchain-file unset only), `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT`
  (`/Z7` vs `/Zi` vs `/ZI`), EnC link set (`/DEBUG:FULL`, `/INCREMENTAL`,
  `/OPT:NOREF,NOICF`, `/LTCG:OFF`, `/PDBTMCACHE`), release link set (`/LTCG`, `/OPT:REF,ICF`, `/INCREMENTAL:NO`,
  `/DEBUG`, `/PDBSTRIPPED`), `/EHsc`, `/utf-8`, `/bigobj`, `/O2` + `/Ob2` + `/GL` + `/Gw` + `/Zc:checkGwOdr`,
  opt-in `/arch:AVX2` (`PPR_ENABLE_AVX2`),
  `/Zc:__cplusplus`, `PPR_PROJECT_WARNINGS_CXX`, ASAN/`/WX` tails.
- `Clang.cmake` — `-stdlib=libc++`, warning family, `-Werror` gate (also serves clang-cl).
- `GCC.cmake` — Clang base + GCC-specific warnings (modules unsupported).
- `cmake/Compilers.cmake` — dispatcher + `setup_ppr_project()`.
