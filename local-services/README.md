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
- Leaves Microsoft WinDbg and user-owned debugger sessions running independently
- Best-effort `pix-mcp.exe` binary cache removal (plus stale `pix-mcp-*.zip` archives and `_staging/`); `version.txt` pin and `captures/` are preserved, never fails uninstall
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

## Official WinDbg MCP (host-native)

WinDbg 1.2610.1001.0 or newer includes the MCP service and stdio proxy. The
`WinDbg` registration in `opencode.json` invokes the installed app alias
`DbgX.Mcp.Proxy.exe` directly with no arguments. Codex uses the same proxy.
No separate .NET tool or debugger daemon is installed by this stack.

`launch.ps1` validates that the configured proxy exists. It does not start a
WinDbg session, open a target, or alter security settings. In WinDbg, enable
MCP in Settings and start MCP Service before loading a target. Discover
`list_sessions`, select the intended session, and verify `get_target_state`.
One client can connect to each session. Uninstalling local services leaves
WinDbg and its sessions alone.

Follow `.opencode/skills/windbg-triage/SKILL.md` and
`docs/debugging/crash-analysis.md` for investigation. See
[Microsoft setup](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/set-up-windbg-mcp)
and [client limitations](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/windbg-mcp-overview).
Codex is a best-effort custom client; protected operations require MCP sampling.
Leave debugger protections unchanged unless an authorized operator decides otherwise.

## PIX MCP (host-native, not containerized)

`pix-mcp` (PozLabs, MIT, Rust, win-x64/arm64 only, stdio-only MCP, https://github.com/PozLabs/pix-mcp) cannot run in a container (needs host D3D12/`pixtool.exe`), so it runs host-exec alongside the containers: transport stdio, command `["local-services/pix/pix-mcp.exe"]`.

- Prereqs: Microsoft PIX installed (auto-discovers `C:\Program Files\Microsoft PIX\*\pixtool.exe`, override via `PIXTOOL_PATH`; tested 2603.25, https://devblogs.microsoft.com/pix/download/). `install.ps1` winget-installs `Microsoft.PIX` automatically when no `pixtool.exe` is found (manual fallback: install from https://devblogs.microsoft.com/pix/download/ or set `PIXTOOL_PATH`). Developer Mode for analysis verbs; admin only for `pix_timing_capture`.
- `launch.ps1` reads the pin in `local-services/pix/version.txt` (currently v0.1.1), selects the x86_64/aarch64 zip via `$env:PROCESSOR_ARCHITECTURE`, downloads it idempotently to `local-services/pix/`, verifies SHA256, extracts `pix-mcp.exe`, fails with an actionable message when no `pixtool.exe` is found, then smoke-tests the CLI (`--help`). Verification is a `--help` health check (a stdio server would block on stdin; `--help` exercises binary load without opening captures), recorded as `pix_mcp_ensure` / `pix_mcp_verify` in `runtime-report.txt`. No ports, no secrets, never capture contents. All `pix-mcp` env vars are optional (`PIXTOOL_PATH`, `PIX_MCP_CAPTURES_DIR`, `PIX_MCP_INPUT_ROOTS`, `PIX_MCP_OUTPUT_ROOTS`, `PIX_MCP_EXECUTABLE_ROOTS`, `PIX_MCP_MAX_CONCURRENT_TOOLS`, `RUST_LOG`); none are set by default.
- Verify manually: `.\local-services\pix\pix-mcp.exe --help`.
- Keep the pinned official release as the default. A source build is an explicit tooling experiment: verify its live schema and result compatibility in the intended client before adopting it. Compare deployed executable paths, hashes, and timestamps; the package version alone cannot distinguish a rebuild, and the idempotent installer can retain an existing executable at the pinned version. Building elsewhere does not update this deployed path.
- Follow the [PIX profiling skill](../.opencode/skills/pix-profiling/SKILL.md) for capture setup, elevation verification, and response-error triage.
- Named CPU scopes can also be inspected without UI through the [read-only timing-query procedure and helper](../.opencode/skills/pix-profiling/references/timing-queries.md), using the trusted installed `PixStorage.dll`. Preserve the original capture; marker JSON does not reconstruct sampled callstacks.
- opencode (`opencode.json`, native V2 shape): `{"mcp":{"servers":{"pix-mcp":{"type":"local","command":["local-services/pix/pix-mcp.exe"],"disabled":false,"timeout":{"catalog":60000,"execution":60000}}}}}`.
- Artifacts (never commit, gitignored): `local-services/pix/pix-mcp.exe`, `local-services/pix/*.zip`, `local-services/pix/_staging/`, `local-services/pix/captures/`.

## OpenCode environment

OpenCode's SearXNG MCP reads `SEARXNG_URL=http://127.0.0.1:8080` (`opencode.json`).
