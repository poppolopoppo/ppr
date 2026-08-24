# Private local services — simplified two-container setup

This directory deploys **Crawl4AI** and **SearXNG** as two standalone Podman containers on the default WSL machine (`podman-machine-default`). No dedicated machine, no compose provider, no Valkey.

## Architecture

- **Machine**: `podman-machine-default` (rootless, created by `podman machine init --rootless` if absent). Reuses the host's working WSL machine; no `ppr-local-services` machine.
- **Containers** (both `--restart unless-stopped`):
  - `crawl4ai` — `docker.io/unclecode/crawl4ai:0.9.2` on `127.0.0.1:11235` (`--shm-size 1g`), auth via `CRAWL4AI_API_TOKEN` / `SECRET_KEY`
  - `searxng` — `ghcr.io/searxng/searxng:latest` on `127.0.0.1:8080`
- **Configs** (mounted read-only): `crawl4ai/config.yml` and `searxng/settings.yml` — both already contain the required browser args (`--no-sandbox`) and `limiter: false` / `formats: [html, json]`.
- **No Valkey** — SearXNG runs with `SEARXNG_LIMITER=false`; Crawl4AI uses no job queue.
- **Network**: loopback-only bindings (`127.0.0.1:11235`, `127.0.0.1:8080`). No internal backend bridge; no compose networks.
- **Secrets**: three 64-char lowercase-hex values generated at launch (`CRAWL4AI_API_TOKEN`, `SECRET_KEY`, `SEARXNG_SECRET`) via `RandomNumberGenerator`. `CRAWL4AI_API_TOKEN` is persisted to the User environment so OpenCode inherits it.

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

`launch.ps1` pulls the two images, generates secrets, starts the containers with `podman run`, persists `CRAWL4AI_API_TOKEN` to the User environment, and runs health/auth checks (SearXNG JSON `?format=json` must echo the query; Crawl4AI `POST /crawl` must reject without/wrong token and accept with the right token; `/mcp/sse` must return 200). Results are written redacted to `local-services/runtime-report.txt` (no secrets).

## Uninstall / cleanup

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\local-services\uninstall.ps1
# non-interactive:
pwsh -NoProfile -ExecutionPolicy Bypass -File .\local-services\uninstall.ps1 -Force
```

- Stops and removes `searxng` and `crawl4ai` containers on `podman-machine-default`
- Prunes unused networks
- Clears `CRAWL4AI_API_TOKEN` from the User environment (and current session)
- Deletes `local-services/.env` and `local-services/runtime-report.txt` if present
- Leaves `podman-machine-default` and the `podman-net-usermode` helper intact; prompts to stop the machine only if no other containers remain

## Validation loop

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\local-services\uninstall.ps1 -Force
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -LaunchServices
pwsh -NoProfile -ExecutionPolicy Bypass -File .\local-services\uninstall.ps1 -Force
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -LaunchServices
```

Each `install -LaunchServices` must end with `Stack launched and runtime checks passed.` and leave both containers `Up` on `127.0.0.1`.

## Manual checks

```powershell
podman --connection podman-machine-default ps --format '{{.Names}} {{.Status}} {{.Ports}}'
podman --connection podman-machine-default logs searxng --tail 50
podman --connection podman-machine-default logs crawl4ai --tail 50
Invoke-WebRequest http://127.0.0.1:8080/search?q=smoke&format=json -Headers @{'X-Real-IP'='127.0.0.1'}
Invoke-WebRequest http://127.0.0.1:11235/health
Get-Content .\local-services\runtime-report.txt
```

## OpenCode environment

OpenCode's Crawl4AI MCP reads `{env:CRAWL4AI_API_TOKEN}` and SearXNG MCP reads `SEARXNG_URL=http://127.0.0.1:8080` (`opencode.json`). Restart OpenCode after `CRAWL4AI_API_TOKEN` changes. Never place the token in tracked config.

## Legacy artifacts

The previous dedicated-machine compose stack (`ppr-local-services` machine, `compose.yml`, `podman-compose.requirements.txt` / `.venv-podman-compose`, `preflight.ps1`, `.env` / `.env.example`, Valkey service) is obsolete and not used. `compose.yml` and related files remain in-repo for reference but are not invoked. Remove `.venv-podman-compose` if present: `Remove-Item -Recurse -Force .\.venv-podman-compose`.
