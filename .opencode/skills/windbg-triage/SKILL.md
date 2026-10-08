---
name: windbg-triage
description: >
  Host-native WinDbg/TTD crash triage via the windbg-tool MCP (dump triage,
  TTD open/snapshot/navigation/memory/stack/exception). Use for .dmp analysis,
  .run replay, unexpected native exits, or death-test mismatches on Windows
  hosts.
---

# WinDbg Triage

For game-crash triage (WER LocalDumps capture through `windbg-tool dump
triage`), the entry point is `docs/debugging/crash-analysis.md` — follow that
runbook instead of improvising this skill's commands.

## When to use

Use this skill when triaging a native crash dump (`.dmp`/`.hdmp`/`.mdmp`), a
Time Travel Debugging trace (`.run`), an unexpected native process exit that
looks crash-like (fail-fast, abort, terminate, VEH-trapped code), or a
death-test mismatch where a child exit or stderr does not match the expected
death contract. Do not use it for managed-only exceptions better served by a
debugger breakpoint session, and do not route containerized workloads through
it: windbg-tool is host-exec only.

## Prerequisites

- Windows x64/arm64 host with `.NET 10 SDK` (`dotnet --version` major >= 10).
- `windbg-tool` global tool (`dotnet tool install -g Devolutions.WinDbg.Tool`).
- Daemon running: `windbg-tool daemon ensure` (named-pipe daemon).
- Symbols are local-only: image directory plus `--symbol-cache` dir, NO
  `srv*` entry. This windbg-tool build errors on DbgEng server paths, so
  never set an `srv*...msdl.microsoft.com` `_NT_SYMBOL_PATH`; the Rust-native
  prefetcher fed by `--image-path` / `--symbol-cache` downloads PDBs on first
  use. An inherited `_NT_SYMBOL_PATH` (especially any `srv*` entry) breaks
  triage — unset it before manual runs (`Remove-Item Env:_NT_SYMBOL_PATH`).
  TTD via `TTD_EXE` / `TTD_RUNTIME_DIR` when recording or replaying traces.
- opencode registration (repo `opencode.json`):
  `{"mcp":{"windbg-ttd":{"type":"local","command":["windbg-tool","mcp"],"enabled":true,"timeout":60}}}`.

## Daemon and discover

CLI-first before any MCP call:

```powershell
windbg-tool discover
windbg-tool daemon ensure
windbg-tool --compact --envelope discover
```

If `discover` fails, fix the install/daemon before opening dumps or traces.

## Route the event

| Observed | Route |
|---|---|
| Existing dump (`.dmp` in hand, or `out/crashes/` run with one) | Analyze it; do not reproduce the crash. |
| Crash-like exit, no dump | Read logs/exit status first, then provisioning check, then pending collection, then reproduce under the runner. |
| Death-test mismatch | Retain the child exit code, argv, PID, and stderr; distinguish intentional exits (`0xB1` fail-stop, ordinary `error_code` returns) from real deaths before triaging. |
| Existing trace (`.run` in hand) | Replay only; do not re-record. |
| `app.game.exe` reproduction | WER LocalDumps capture via the runbook. Explicitly NOT `trace record`: TTD recording of the game was observed to hang at finalize; repair is out of scope. |

WER LocalDumps keys are per-exe: the provisioned key covers `app.game.exe`
only. Test exes need their own key before WER will capture them.

## Elevation (TTD recording)

Elevation is mandatory for `trace record` and is used solely for launching
the TTD recorder — no other command in this skill elevates. It is unlocked
per invocation: run the record command under inline sudo
(`sudo windbg-tool trace record ...`) and validate the single Windows UAC
prompt yourself; the agent cannot approve it. New-Window sudo mode is
rejected. Dump triage, trace replay/navigation, and all other commands run
unelevated and never trigger UAC. Never point `trace record` at `app.game.exe`
(see routing above).

## Dump triage

Prefer the CLI shape first, then the matching MCP tools. Clear any inherited
`_NT_SYMBOL_PATH` first (see Prerequisites):

```powershell
Remove-Item Env:_NT_SYMBOL_PATH -ErrorAction SilentlyContinue
windbg-tool dump triage <path-to.dmp> --max-frames 64 --image-path <image-dir> --symbol-cache <cache-dir>
```

Use `dump_*` MCP tools for the same flow inside the agent session. Record the
faulting thread, top frames, and exception record; never paste full dump
contents into reports or logs.

## TTD open and navigation

```powershell
windbg-tool open <trace.run> --binary-path <path-to.exe>
```

Then navigate the recording: snapshot, disassembly, registers, position set /
step / replay-to, stack backtrace, memory dump, exception focus. Prefer the CLI
for live probing; use the `ttd_*` / `target_*` MCP tools for the same
operations inside the agent session. Existing traces replay; re-recording a
supplied trace is out of scope.

## Trace record flags

Pass `--ttd-exe` with the explicit recorder path when the elevated `trace
record` invocation lacks it on `PATH`. `--disable-user-shadow-stack` is
required with `--ring` / `--record-for-seconds`. `--max-file-mb` conflicts
with `--profile`: pass one or the other, never both.

## Triage JSON envelope

The `--envelope` result reports success with `ok:true`; no top-level `status`
field is required. The triage payload lives at `data.triage`. Under
`Set-StrictMode -Version Latest` every optional read is preceded by a PSObject
existence probe (`$x.PSObject.Properties['name']`); summaries render from the
unwrapped payload. Full contract and failure signals are in
`docs/debugging/crash-analysis.md` section 3.

## Live-probe note

Live managed break with `--allow-runtime-write` is test-VM-only. Default to
dump triage or TTD replay; do not attach to a live production process.

## MCP tool names

`windbg-tool mcp` exposes the `ttd_*`, `dump_*`, and `target_*` tools. Match
the CLI noun to the MCP prefix: `dump triage` to `dump_*`, `open` / snapshot /
navigation to `ttd_*`, process and exception focus to `target_*`.

## Artifact hygiene

Never commit traces or dumps: `.run/`, `.ttd/`, `*.dmp`, `*.hdmp`, `*.mdmp`,
`TTD/`, `dumps/`. Keep triage outputs (frame lists, exception summaries) in
redacted notes; full memory contents stay on the host.
