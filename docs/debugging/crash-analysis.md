# Game crash analysis runbook

> Status: WER capture was previously exercised on the abort path. Analysis now
> uses Microsoft's official WinDbg MCP; the prior CLI triage result is not
> validation of this integration. Verify a live MCP session and matching symbols
> before claiming an end-to-end diagnosis. The remaining fatal-path capture
> matrix and original binary/PDB matching checks remain outstanding.

All commands run from the repo root (`E:\Code\ppr`). Stock 5.1-compatible
syntax; invoke with either `powershell.exe` (Windows PowerShell 5.1) or `pwsh`
(PowerShell 7) — examples use `pwsh`. No script prompts, pauses, builds, or
configures. Never dot-source the scripts (`exit` sets the process exit code).

## 0. Route the event

| Observed | Route |
|---|---|
| Existing dump (`.dmp` in hand, or an `out/crashes/` run with one) | Analyze it (section 3); do not reproduce the crash. |
| Crash-like exit, no dump | Read `stdout.log` / `stderr.log` / `exit-code.txt` first, then provisioning check (section 1), then pending collection, then reproduce under the runner (section 2). |
| Death-test mismatch | Retain the child exit code, argv, PID, and stderr; distinguish intentional exits (ordinary `error_code` returns, `0xB1` fail-stop) from real deaths before triaging. |
| Existing trace (`.run` in hand) | Replay only; do not re-record. |
| `app.game.exe` reproduction | WER LocalDumps capture via this runbook. Explicitly NOT `trace record`: TTD recording of the game was observed to hang at finalize; repair is out of scope. |

WER LocalDumps keys are per-exe: section 1 provisions
`...\LocalDumps\app.game.exe` only. Test exes need their own key before WER
will capture them.

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
  demand — but the Check ACL target is wrong). Check reads `DumpFolder` via
  `Get-ItemProperty`, which returns the expanded path, so it validates the
  expanded value plus the `REG_EXPAND_SZ` kind. `Restore` puts the preserved
  values back (or removes the key when it did not exist before Install).

## 2. Run the game under capture

```powershell
pwsh -NoProfile -File scripts/Run-GameWithDiagnostics.ps1 -Preset <name> -- <game args>
```

Example: `pwsh -NoProfile -File scripts/Run-GameWithDiagnostics.ps1 -Preset msvc-dev -- --smoke`
(`--` separates runner args from game args; `GameArgs` is passed through to
`out\build\<Preset>\game\app.game.exe`). The runner never builds; a missing exe
is a hard error telling the operator to build first (`cmake --preset <name>`
configures; `cmake --build out/build/<name> --target app.game` builds). The runner first gates on
`Setup-CrashDiagnostics.ps1 -Mode Check` and exits 1 with the elevated Install
command when provisioning is absent.

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
  already have exited 1 before launch), or the process exited without crashing:
  an ordinary `std::error_code` return or the intentional `std::_Exit(0xB1)`
  fail-stop (section 4) produces no WER dump by design. A missing dump after an
  unexplained crash-like exit stays a capture investigation (provisioning check
  → pending collection → reproduce under the runner).
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

## 3. Analyze through official WinDbg MCP

Prepare the newest unanalyzed captured run, or select a run explicitly:

```powershell
pwsh -NoProfile -File scripts/Prepare-CrashAnalysis.ps1 -Pending
pwsh -NoProfile -File scripts/Prepare-CrashAnalysis.ps1 -RunDir out/crashes/<UTC>-<PID>
```

Exactly one selector is required. `-Pending` skips runs with an existing
`triage-summary.md` or legacy `triage.json`. The script writes
`analysis-request.json` containing dump paths, image/source locations derived
from the run manifest, a WinDbg symbol path, and the intended report location.
It does not launch a debugger or perform analysis. Exit 0 means preparation
completed; exit 1 means no dump/pending run, and exit 2 means usage/manifest error.
Existing reports are preserved. Multiple dumps are listed for explicit selection.
Direct-launch manifests can lack image identity; establish it from the dump before
claiming symbols match.

1. Open an empty WinDbg window and start MCP Service before opening the dump.
   Enable the server in MCP service settings if necessary. The configured
   `WinDbg` proxy connects the client to this service; registration alone does
   not create a debugging session.
2. Discover tools, use `list_sessions`, and select the intended WinDbg PID with
   `connect_session`. Verify `get_target_state`. One MCP client can connect to a
   session at a time; do not replace a target already under investigation.
3. Use `open_dump` with the selected full dump path and no active target. Use
   `show_output` with a debugger command and a short explanation for each query.
4. Set `.sympath` to the request's `symbolPath` and `.srcpath` to its `sourceRoot`,
   quoting paths as needed. The symbol path combines the original image directory
   with `srv*<cache>*https://msdl.microsoft.com/download/symbols`. Inspect `get_logs`
   when loading fails. Verify original image/PDB identity with module/symbol
   evidence; a rebuild does not recreate matching symbols for an old dump.
5. Inspect exception, context, stack, and modules as appropriate (`!analyze -v`,
   `.exr -1`, `.ecxr`, `kv`, `lm`). Test alternatives rather than treating a stack
   pattern as a cause. Save `triage-summary.md` with target/binary identity,
   verified symbols, commands/evidence, candidate cause, confidence, gaps, and
   next checks. Preserve prior reports and do not invent the old JSON envelope.

Follow `.opencode/skills/windbg-triage/SKILL.md` for live/hang/TTD investigations.
Microsoft documents Codex/custom clients as best effort; protected output requires
MCP sampling and may be blocked. Keep WinDbg security settings unchanged and
report the blocker. See [official setup](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/set-up-windbg-mcp)
and [security/client requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/windbg-mcp-overview).
No connected MCP session, missing/mismatched symbols, or insufficient dump data
must be reported as incomplete analysis, never a successful diagnosis.

## 4. Fatal-path contract

`hal::fatalExit(reason)` (declaration `lib/engine/core/hal/Core.HAL.cppm:380-386`;
Windows implementation `lib/engine/core/hal/windows/Core.HAL.windows.Debugger.cpp:77-91`)
is the shared fail-fast endpoint for all fatal paths. On Windows it calls
`RaiseFailFastException` (no handlers, no unwinding; WER captures the LocalDumps
dump); `TerminateProcess(..., 3)` below it is an unreachable backstop for the
`[[noreturn]]` contract. The `reason` literal is unused at runtime by design;
call sites are distinguished by return address in the dump. Live call sites:

- `assert` (`lib/engine/core/Core.Assert.cpp:33`, via the `PPR_ASSERT` default handler).
- `terminate` (`Core.HAL.windows.Debugger.cpp:178-186`, `std::set_terminate`
  handler) → `fatalExit("terminate")`. No logging inside: debug output can block
  under broken terminate invariants, so the dump stack identifies this path.
- `invalid-parameter` (`:116-167`, CRT invalid-parameter handler) →
  `fatalExit("invalid-parameter")`.
- `purecall` (`:169-176`, pure-virtual-call handler) → `fatalExit("purecall")`.
- SIGABRT (`game/main.cpp:3013-3020`, `diagSigAbrt`): dumps the stack via
  `diagDumpStack`, then `fatalExit("sigabrt")` — a WER dump IS captured. There is
  no silent `_exit(3)` on this path.
- Unexpected exceptions (`game/main.cpp:3286-3295`, `main()` catch-all around
  `app.run()`): `[fatal-ex] ...` to stderr, then
  `fatalExit("unexpected-std-exception" / "unexpected-unknown-exception")`.
- TEST-ONLY `--diag-crash=` (`game/main.cpp:3180-3183`, armed at `:3255-3275`):
  `abort` fires the real SIGABRT path above (`std::abort` at `:3268` → signal
  handler → `fatalExit`); `failfast` calls `fatalExit("diag-crash")` directly
  (`:3271`); an unknown mechanism prints usage and returns 2 without crashing.

UI suppression comes from `disableSystemErrorReporting()` (`:58-75`):
`WerSetFlags(WER_FAULT_REPORTING_NO_UI)` plus `SetErrorMode(SEM_FAILCRITICALERRORS |
SEM_NOOPENFILEERRORBOX)`. `SEM_NOGPFAULTERRORBOX` is deliberately absent — it
would suppress WER invocation entirely and block LocalDumps capture.

VEH (`diagVeh`, `game/main.cpp:3022-3121`) returns `EXCEPTION_CONTINUE_SEARCH`
(0) for fatal codes, so normal unwinding/termination proceeds and WER still
fires. Benign codes return `EXCEPTION_CONTINUE_EXECUTION` (-1), as a debugger
would: `0x406D1388` legacy thread-naming (`:3059-3062`) and `DBG_PRINTEXCEPTION_C`
(`0x40010006`, `:3105-3118`) continue execution; MSVC C++ exceptions
(`0xE06D7363`, `:3063-3104`) log `[cppthrow] <type>` and continue the search.
Benign prints never touch `diagDumpStack`'s once-guard, which is reserved for the
real trap.

Ordinary failures are NOT fatal: an `app.run()` error prints
`[fatal] app.run: ...` and returns the error value (`:3278-3285`); `--smoke`,
`--load100`, and `--time-trials` likewise return error values. These produce no
WER dump by design.

`0xB1` is a **separate intentional path, not a crash**:
`kUnknownGpuCompletionFatalExit{0xB1}`
(`lib/engine/app/renderer/App.Renderer.FrameFence.cppm:128`) via
`Application::failStopOnUnknownGpuTeardown_`
(`lib/engine/app/App.Application.cpp:1561-1576`, `std::_Exit` at `:1575`).
It produces no WER dump by design; a missing dump after an `0xB1` exit is
expected, not a capture failure.

Terminate-path dumps describe termination-time state, not the original throw
site: the `terminate` handler runs inside `noexcept` termination and fail-fasts
from there, so the dump stack identifies the terminate/fail-fast frames.

Pre-existing game diagnostics in `game/main.cpp` (informational only; the WER
dump is the artifact of record):

- stderr markers: `[trap] <reason> code=<n>` plus `  #<i> <addr> <sym>+<disp>`
  lines plus `[trap] end` from `diagDumpStack` (`:2968-3011`); `[cppthrow]
  <type>|<undecoded>` for in-flight C++ exceptions (`:3094-3102`); `[fatal-ex]
  ...` before the `fatalExit` catch-all (`:3286-3295`); `[dbgthread]` /
  `[dbgprint ...]` are benign swallows, not crashes. An ordinary `app.run()`
  error prints `[fatal] app.run: ...` and returns the error value without
  fail-fast (`:3278-3285`).

A missing dump therefore means one of: provisioning was absent, the process took
an expected no-dump exit (ordinary `error_code` return, intentional `0xB1`
fail-stop), or an unexplained crash-like exit produced nothing — which stays a
capture investigation (provisioning check → pending collection → reproduce under
the runner).

## 5. Validation status

The previous abort-path capture established that WER can collect a dump. Recheck
capture and diagnosis separately: preparation syntax/error paths, official proxy
startup, intended WinDbg session selection, target identity, original PDB match,
and evidence-backed analysis. The complete fatal-path capture matrix is still an
acceptance check; do not infer it from one abort-path observation.
