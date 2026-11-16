# Windows shell and toolchain recovery

Use this reference for Windows shell loss, first fresh configure/toolchain
verification, or suspected MSVC/vcpkg cache contamination. The host setup and
normal build commands remain in [build-system](../SKILL.md).

- A saved execution-session ID can expire. If the executor reports an unknown
  process, retain that evidence and replace the unavailable shell once through
  the same approved activation helper. Verify the new child directory and
  toolchain before resuming; an expired session is not evidence of toolchain
  contamination. Avoid creating an idle replacement far ahead of the batch.
- **A shell that skips Insiders vcvars fails fast.** The MSVC pre-project
  guard rejects the configure and names the required `vcvars64.bat` recovery.
  Never spawn a fresh shell per command unless it repeats the Insiders
  `vcvars64.bat` call before touching CMake.
- Toolchain inheritance in a generic/ad-hoc shell is UNVERIFIED: JetBrains
  documentation is silent on whether terminal tools inherit the IDE toolchain.
  Policy (not documented fact): an ad-hoc terminal is never a build path and
  never where toolchain discovery happens; builds run in the persistent vcvars
  shell defined in the entry skill.
- Observed: vcvars alone is insufficient if an inherited `PATH` or an existing
  CMake cache selects LLVM-MinGW `ld`/`ar`. MSVC-only presets pin `cl`, `link`,
  and `lib` on first configure (the configuration writer handles the preset
  values). Vcpkg detects its compiler separately and may select installed VS
  2022 even with CMake correctly pinned to VS 18; MSVC-only presets set
  `VCPKG_VISUAL_STUDIO_PATH` from the activated `VSINSTALLDIR` for initial
  configure. Ninja auto-regeneration on a separate `cmake --build` invokes
  CMake without preset environment; vcvars supplies `VSINSTALLDIR` but not
  `VCPKG_VISUAL_STUDIO_PATH`. When the override is absent, the helper derives
  the normalized vcpkg path from `VSINSTALLDIR` before vcpkg install: vcpkg
  rejected `.../18/Insiders/` while detecting `.../18/Insiders`. On the first
  fresh configure, check vcpkg's `Compiler found:` output points to VS 18.
  Verify `CMAKE_CXX_COMPILER`,
  `CMAKE_LINKER`, and `CMAKE_AR` in `out/build/<preset>/CMakeCache.txt` and
  the generated archive rule: they must use the MSVC tools, not LLVM-MinGW.
  For a contaminated *existing* `msvc-dev` tree, run
  `cmake --fresh --preset msvc-dev` in the same VS 18 Insiders vcvars shell,
  then recheck; `--fresh` is a one-time stale-cache
  recovery, not the normal configure route. Compilation errors from an active
  source refactor are separate from toolchain contamination.
