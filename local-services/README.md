# Private local services — single-container setup

This directory deploys **SearXNG** as a standalone Podman container on the default WSL machine (`podman-machine-default`). No dedicated machine, no compose provider, no Valkey.

## Architecture

- **Machine**: `podman-machine-default` (rootless, created by `podman machine init --rootless` if absent). Reuses the host's working WSL machine; no `ppr-local-services` machine.
- **Containers** (all `--restart unless-stopped`):
  - `searxng` — `ghcr.io/searxng/searxng:latest` on `127.0.0.1:8080`
- **Configs** (mounted read-only): `searxng/settings.yml` — already contains the required `limiter: false` / `formats: [html, json]`.
- **No Valkey** — SearXNG runs with `SEARXNG_LIMITER=false`.
- **Network**: loopback-only binding (`127.0.0.1:8080`). No internal backend bridge; no compose networks.
- **Secrets**: one 64-char lowercase-hex value generated at launch (`SEARXNG_SECRET`) via `RandomNumberGenerator`.

## Fresh-machine setup

From a new PowerShell shell at the repository root:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

Validates Windows, WinGet, WSL2, Podman, Python 3.12+, Node.js 22+, repository layout, and ensures `podman-machine-default` is running rootless. By default does **not** launch services; opt in with:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -LaunchServices
```

`install.ps1` installs missing Podman / Node.js 22 / Python 3.12 via WinGet, ensures the default machine exists and is running, then invokes `local-services/launch.ps1`. It never installs Podman Desktop, enables WSL/virtualization, elevates, or reboots. Use `-SkipPrerequisiteInstall` to fail rather than install via WinGet.

`launch.ps1` pulls the image, generates secrets, starts the container with `podman run`, and runs a health check (SearXNG JSON `?format=json` must echo the query). Results are written redacted to `local-services/runtime-report.txt` (no secrets).

## Uninstall / cleanup

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\local-services\uninstall.ps1
# non-interactive:
pwsh -NoProfile -ExecutionPolicy Bypass -File .\local-services\uninstall.ps1 -Force
```

- Stops and removes the `searxng` container on `podman-machine-default`
- Best-effort `windbg-tool daemon stop`; skipped silently when the tool is absent, never fails uninstall
- Prunes unused networks
- Deletes `local-services/.env` and `local-services/runtime-report.txt` if present
- Leaves `podman-machine-default` and the `podman-net-usermode` helper intact; prompts to stop the machine only if no other containers remain

## Validation loop

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\local-services\uninstall.ps1 -Force
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -LaunchServices
pwsh -NoProfile -ExecutionPolicy Bypass -File .\local-services\uninstall.ps1 -Force
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -LaunchServices
```

Each `install -LaunchServices` must end with `Stack launched and runtime checks passed.` and leave the container `Up` on `127.0.0.1`.

## Manual checks

```powershell
podman --connection podman-machine-default ps --format '{{.Names}} {{.Status}} {{.Ports}}'
podman --connection podman-machine-default logs searxng --tail 50
Invoke-WebRequest http://127.0.0.1:8080/search?q=smoke&format=json -Headers @{'X-Real-IP'='127.0.0.1'}
Get-Content .\local-services\runtime-report.txt
```

## WinDbg/TTD (host-native, not containerized)

`windbg-tool` (Devolutions, Rust + C++, win-x64/arm64 only, stdio-only MCP) cannot run in a container (needs host kernel/TTD driver/elevation/DbgEng/host PIDs/named pipes), so it runs host-exec alongside the four containers: transport stdio, command `["windbg-tool", "mcp"]`.

- Prereqs: .NET 10 SDK. `install.ps1` gates `dotnet --version >= 10` and installs `Microsoft.DotNet.SDK.10` via WinGet unless `-SkipPrerequisiteInstall`.
- `launch.ps1` runs `dotnet tool install -g Devolutions.WinDbg.Tool` when `windbg-tool` is missing (otherwise `dotnet tool update -g`, tolerating already-current failure), refreshes `%USERPROFILE%\.dotnet\tools` on `PATH`, then `windbg-tool discover` plus `windbg-tool daemon ensure` (named-pipe daemon). Verification is `windbg-tool --compact --envelope discover` with a 2-minute retry loop, recorded as `windbg_tool_ensure` / `windbg_tool_discover` in `runtime-report.txt`. No ports, no secrets, never dump/trace contents.
- Verify manually: `windbg-tool --compact --envelope discover`.
- opencode (`opencode.json`, native V2 shape): `{"mcp":{"servers":{"windbg-ttd":{"type":"local","command":["windbg-tool","mcp"],"disabled":false,"timeout":{"catalog":60000,"execution":60000}}}}}`.
- Triage: `windbg-tool dump triage <dmp> --max-frames 32`; `windbg-tool open <trace.run> --binary-path <exe>` then snapshot/disasm/registers/position set/step/replay-to/backtrace/memory dump/exception focus. `trace record` requires elevation via inline `sudo windbg-tool trace record ...` (New-Window sudo mode is rejected). Live managed-break `--allow-runtime-write` is test-VM-only. See `.opencode/skills/windbg-triage/SKILL.md`.
- Artifacts (never commit, gitignored): `*.dmp`, `*.hdmp`, `*.mdmp`, `*.run/`, `*.ttd/`, `TTD/`, `dumps/`.

## OpenCode environment

OpenCode's SearXNG MCP reads `SEARXNG_URL=http://127.0.0.1:8080` (`opencode.json`).
