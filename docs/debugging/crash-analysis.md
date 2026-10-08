# Game crash analysis runbook

> Status: **TESTED end to end 2026-10-08** (rev 33a2d89 + smoke-test fixes):
> armed `--diag-crash=abort` via Run-GameWithDiagnostics (msvc-live) →
> WER full dump (338MB, exit 0xC0000602) → Invoke-CrashAnalysis SUCCESS
> with symbolized stack (fatalExit ← diagSigAbrt ← abort ← main).
> Symbol-cache warm-up downloads PDBs on first use; rerun is offline.
> Remaining validation gate: capture matrix across the other fatal paths
> (terminate, invalid-param, purecall, stack overflow, fail-fast directly),
> PDB matching per preset, zero dialogs. The three scripts are tested on
> the abort path; treat other paths' artifacts as contract, not observed.

All commands run from the repo root (`E:\Code\ppr`). Stock `pwsh 5.1`
compatible. No script prompts, pauses, builds, or configures. Never dot-source
the scripts (`exit` sets the process exit code).

## 1. Provision the machine (once, elevated)

Run this from an **elevated** prompt (the script never self-elevates; an
unelevated Install/Restore exits 2 with the elevated re-run command):

```powershell
pwsh -NoProfile -File scripts/Setup-CrashDiagnostics.ps1 -Mode Install
```

Expect this artifact:

- Registry key `HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\app.game.exe`
  with `DumpFolder` = `%LOCALAPPDATA%\PPR\CrashDumps\app.game.exe`
  (`REG_EXPAND_SZ`, stored unexpanded), `DumpCount` = 4 (`REG_DWORD`), `DumpType` = 2
  (`REG_DWORD`, full dump).
- Inbox directory `%LOCALAPPDATA%\PPR\CrashDumps\app.game.exe` with the default
  user-private ACL (do not loosen it; dumps contain process memory).
- Backup of any pre-existing values at
  `%LOCALAPPDATA%\PPR\CrashDumps\.localdumps-backup-app.game.json` (written once;
  re-installing keeps the originals `Restore` must put back).

Verify with (runs unelevated):

```powershell
pwsh -NoProfile -File scripts/Setup-CrashDiagnostics.ps1 -Mode Check
```

- Expect: `Check PASSED: ... values correct and inbox '...' writable.` Exit 0.
- Failure means: exit 1 with `DRIFT <detail>` per problem (key missing, value or
  kind mismatch, inbox missing or not writable) — run Install elevated.
  Exit 2 means usage, internal, not-elevated (Install/Restore), or missing-backup
  (Restore) error.
- Notes: only the HKLM key is honored by WER; an HKCU key is silently ignored
  and this script never writes one. PPR ships no in-app crash reporter, and none
  must be added — a custom unhandled-exception reporter would silently exclude
  `app.game.exe` from LocalDumps collection. Install elevated as the same
  interactive user that will run the game; otherwise the pre-created directory
  lands in the wrong profile (capture still works — WER creates the folder on
  demand — but the Check ACL target is wrong). `Restore` puts the preserved
  values back (or removes the key when it did not exist before Install).

## 2. Run the game under capture

```powershell
pwsh -NoProfile -File scripts/Run-GameWithDiagnostics.ps1 -Preset <name> -- <game args>
```

Example: `pwsh -NoProfile -File scripts/Run-GameWithDiagnostics.ps1 -Preset msvc-dev -- --smoke`
(`--` separates runner args from game args; `GameArgs` is passed through to
`out\build\<Preset>\game\app.game.exe`). The runner never builds; a missing exe
is a hard error telling the operator to build first (e.g. `cmake --preset <name>`).
The runner first gates on `Setup-CrashDiagnostics.ps1 -Mode Check` and exits 1
with the elevated Install command when provisioning is absent.

Expect this artifact: `out/crashes/<UTC>-<PID>/` (UTC `yyyyMMdd-HHmmss`, plus a
`-<N>` suffix on collision), containing:

- `run-manifest.json` with fields `schemaVersion` (1), `utc`, `directLaunch`
  (`false`), `pid`, `processCreationTime`, `exePath`, `preset`, `repoRoot`,
  `gitRevision`, `gitDirty`, `argv` (secret values redacted), `exitCode`,
  `collectedDumps` (array of `{sourceFile, runFile}`).
- `stdout.log`, `stderr.log`, `exit-code.txt`.
- Zero or more collected `*.dmp` files, copied from the inbox after a bounded
  wait (60 s) for WER to close each file. Only inbox dumps with `LastWriteTime`
  within one minute before process start are candidates.

Failure means:

- No `.dmp` in the run directory: either provisioning was absent (runner would
  already have exited 1 before launch) or the process did not crash (clean exit,
  `std::error_code` return, `_exit(3)`/`_Exit(0xB1)` paths — see section 4; none
  of these produce WER dumps).
- `Timed out waiting for WER to close ...`: WER still held the file after 60 s;
  retry collection; the inbox original is untouched (the runner copies, never moves).
- Runner exit code is the game exit code on a launched run; 1 means runner
  failure (provisioning, missing exe, dump-collection failure); 2 means usage or
  internal error.

Orphaned dumps (direct exe launches outside this runner still land in the inbox)
are gathered without launching the game:

```powershell
pwsh -NoProfile -File scripts/Run-GameWithDiagnostics.ps1 -CollectPending
```

Expect: one `out/crashes/<UTC>-pending[/-<N>]/` directory per previously
unclaimed inbox dump, each with a manifest (`directLaunch: true`, `pid`/`exePath`/
`preset`/`argv`/`exitCode` null) and the copied dump. `No pending dumps in inbox.`
means there is nothing unclaimed (claimed = named in any existing manifest or
already present as `*.dmp` under `out/crashes/`).

## 3. Analyze the dump

```powershell
pwsh -NoProfile -File scripts/Invoke-CrashAnalysis.ps1 -Pending
```

or target one run explicitly:

```powershell
pwsh -NoProfile -File scripts/Invoke-CrashAnalysis.ps1 -RunDir out/crashes/<UTC>-<PID>
```

`-Pending` selects the newest run directory that has `run-manifest.json` plus a
collected dump but no `triage.json` yet. Exactly one of `-RunDir`/`-Pending` is
required. The script derives the dump path, image directory, source root, and
symbol cache from `run-manifest.json` (plus `build-manifest.json` beside the exe
when present) — never from operator input — sets `_NT_SYMBOL_PATH` to
`srv*<repo>/out/crashes/symbol-cache*https://msdl.microsoft.com/download/symbols;<image-dir>`
and `_NT_SOURCE_PATH` to the source root, runs `windbg-tool discover` then
`windbg-tool --compact --envelope dump triage <dump> --max-frames 64
--image-path <images> --symbol-cache <cache>`, staging raw bytes in
`analysis-scratch/triage.raw.json` before validating and publishing.

Expect these artifacts in the run directory:

- `triage.json` (machine-readable triage result, published only after validation).
- `triage-summary.md` (human-readable: dump, exe/preset/rev, command, outcome,
  exception record, top frames, modules/symbols).
- `analysis-scratch/triage.raw.json` (raw envelope bytes), `triage.stderr.log`
  (tool stderr).

Failure means (exit 1; `triage-summary.md` records `FAILED: ...`):

- No dump in the run directory: `FAILED: no dump was collected for this run (the
  process may not have crashed).` See section 4 for non-crash exits.
- PDB missing beside the exe (`<image-dir>/app.game.pdb`): `FAILED: symbols
  missing ... Rebuild the preset so PDBs sit beside the exe, then re-run
  analysis.` The tool is not invoked.
- `windbg-tool` absent from `PATH` or `discover` failing: install/daemon problem
  (exit 2 for missing tool); fix before opening dumps.
- Tool non-zero exit, empty output, non-JSON output, symbol-mismatch text
  (`pdb ... mismatch`, `symbol ... could not be loaded`, `failed to load
  symbols`, `symbol file not found`), `status` error/failed/failure, or a
  non-empty `errors` array: triage failure. An empty triage is **never** written
  as success — `windbg-tool produced no output; refusing to write an empty
  success report.` Exit 0 means triage completed with frames/symbols recorded.

## 4. Fatal-path contract

`hal::fatalExit(reason)` (`lib/engine/core/hal/Core.HAL.cppm:380-386`,
`hal/windows/Core.HAL.windows.Debugger.cpp:77-91`) is the shared fail-fast
endpoint for all fatal paths. On Windows it calls `RaiseFailFastException`
(no handlers, no unwinding; WER captures the LocalDumps dump) with
`TerminateProcess(..., 3)` as an unreachable backstop. The `reason` literal is
unused at runtime; call sites are distinguished by return address in the dump.
Known reasons:

- `assert` (`lib/engine/core/Core.Assert.cpp:33`, via `PPR_ASSERT` default handler).
- `terminate` (`Core.HAL.windows.Debugger.cpp:185`, `std::set_terminate` handler).
- `invalid-parameter` (`:166`, CRT invalid-parameter handler).
- `purecall` (`:175`, pure-virtual-call handler).
- `unexpected-std-exception` / `unexpected-unknown-exception`
  (`game/main.cpp:3267,3270`, `main()` catch-all around `app.run()`).

UI suppression comes from `disableSystemErrorReporting()` (`:58-75`):
`WerSetFlags(WER_FAULT_REPORTING_NO_UI)` plus `SetErrorMode(SEM_FAILCRITICALERRORS |
SEM_NOOPENFILEERRORBOX)`. `SEM_NOGPFAULTERRORBOX` is deliberately absent — it
would suppress WER invocation entirely and block LocalDumps capture.

`0xB1` is a **separate intentional path, not a crash**:
`kUnknownGpuCompletionFatalExit{0xB1}`
(`lib/engine/app/renderer/App.Renderer.FrameFence.cppm:128`) via
`Application::failStopOnUnknownGpuTeardown_` (`lib/engine/app/App.Application.cpp:1575`)
calling `std::_Exit`. It produces no WER dump by design; a missing dump after an
`0xB1` exit is expected, not a capture failure.

Terminate-path dumps describe termination-time state, not the original throw
site: the `terminate` handler runs inside `noexcept` termination and fail-fasts
from there, so the dump stack identifies the terminate/fail-fast frames.

Pre-existing game diagnostics in `game/main.cpp` (informational only; the WER
dump is the artifact of record):

- `diagVeh` (`:3018-3117`) returns `EXCEPTION_CONTINUE_SEARCH` (0) for fatal
  codes, so normal unwinding/termination proceeds and WER still fires. Benign
  codes are swallowed without touching the once-guarded stack dump: `0x406D1388`
  (legacy thread-naming) and `DBG_PRINTEXCEPTION_C` (`0x40010006`) continue
  execution; MSVC C++ exceptions (`0xE06D7363`) log and continue the search.
- `diagSigAbrt` (`:3013-3016`) dumps the stack then calls `_exit(3)`. `_exit`
  bypasses WER dump capture: a SIGABRT run leaves stderr evidence but no dump.
- stderr markers: `[trap] <reason> code=<n>` plus `  #<i> <addr> <sym>+<disp>`
  lines plus `[trap] end` from `diagDumpStack` (`:2968-3011`); `[cppthrow]
  <type>|<undecoded>` for in-flight C++ exceptions (`:3090-3098`); `[fatal-ex]
  ...` before the `fatalExit` catch-all (`:3262,3269`); `[dbgthread]` /
  `[dbgprint ...]` are benign swallows, not crashes. An ordinary `app.run()`
  error prints `[fatal] app.run: ...` and returns the error value without
  fail-fast (`:3254-3260`).

## 5. Validation gate (pending)

The following have **NOT** been validated on any machine yet (scripts are
syntax-checked only):

- End-to-end WER dump capture for each fatal path (`assert`, `terminate`,
  `invalid-parameter`, `purecall`, unexpected exceptions) via
  `Run-GameWithDiagnostics.ps1`.
- Symbolized-stack recovery (`triage.json` top frames resolving to PDB symbols)
  via `Invoke-CrashAnalysis.ps1`, including PDB matching for the preset that
  built the crashing exe.
- Zero-dialog behavior on a real crash (no Abort/Retry/Ignore, no WER UI).

Until this gate passes, treat every artifact expectation in sections 1–3 as
contract, not accomplishment.
