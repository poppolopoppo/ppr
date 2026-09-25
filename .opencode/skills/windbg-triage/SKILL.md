---
name: windbg-triage
description: >
  Host-native WinDbg/TTD crash triage via the windbg-tool MCP (dump triage,
  TTD open/snapshot/navigation/memory/stack/exception). Use for .dmp analysis
  or .run time-travel sessions on Windows hosts.
---

# WinDbg Triage

## When to use

Use this skill when triaging a native crash dump (`.dmp`/`.hdmp`/`.mdmp`) or a
Time Travel Debugging trace (`.run`) on a Windows host. Do not use it for
managed-only exceptions better served by a debugger breakpoint session, and do
not route containerized workloads through it: windbg-tool is host-exec only.

## Prerequisites

- Windows x64/arm64 host with `.NET 10 SDK` (`dotnet --version` major >= 10).
- `windbg-tool` global tool (`dotnet tool install -g Devolutions.WinDbg.Tool`).
- Daemon running: `windbg-tool daemon ensure` (named-pipe daemon).
- Symbols via `_NT_SYMBOL_PATH` / `_NT_ALT_SYMBOL_PATH` / `_NT_SYMCACHE_PATH`;
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

## Elevation (TTD recording)

Elevation is mandatory for `trace record` and is used solely for launching
the TTD recorder — no other command in this skill elevates. It is unlocked
per invocation: run the record command under inline sudo
(`sudo windbg-tool trace record ...`) and validate the single Windows UAC
prompt yourself; the agent cannot approve it. New-Window sudo mode is
rejected. Dump triage, trace replay/navigation, and all other commands run
unelevated and never trigger UAC.

## Dump triage

Prefer the CLI shape first, then the matching MCP tools:

```powershell
windbg-tool dump triage <path-to.dmp> --max-frames 32
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
operations inside the agent session.

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
