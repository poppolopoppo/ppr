[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$reportPath = Join-Path $PSScriptRoot 'runtime-report.txt'
$machineName = 'podman-machine-default'
$connectionName = 'podman-machine-default'
$crawl4aiConfig = Join-Path $PSScriptRoot 'crawl4ai\config.yml'
$searxngSettings = Join-Path $PSScriptRoot 'searxng\settings.yml'
# Floating tags are intentional per user policy for crawl4ai/searxng: images track :latest, no digest pins.
# Headroom is the Phase 1 exception: pinned to the tested 0.37.x tag (no floating :latest).
$crawl4aiImage = 'docker.io/unclecode/crawl4ai:latest'
$searxngImage = 'ghcr.io/searxng/searxng:latest'
# Empty HEADROOM_IMAGE falls back to the pinned 0.37.x tag (Phase 1 posture, not :latest).
$headroomImage = if (-not [string]::IsNullOrWhiteSpace($env:HEADROOM_IMAGE)) { $env:HEADROOM_IMAGE } else { 'ghcr.io/headroomlabs-ai/headroom:0.37.0' }
$secretNames = @('CRAWL4AI_API_TOKEN', 'SECRET_KEY', 'SEARXNG_SECRET')

$results = [System.Collections.Generic.List[string]]::new()

function Record([string] $name, [string] $status) {
    $results.Add("$name=$status")
}

function Stop-WithMessage([string] $message) {
    Record 'overall' 'blocked'
    $results | Set-Content -LiteralPath $reportPath -Encoding utf8
    throw $message
}

function Invoke-Safe([string] $name, [scriptblock] $operation) {
    try {
        & $operation
        Record $name 'passed'
    } catch {
        Record $name 'failed'
        throw
    }
}

function New-Secret {
    $bytes = [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
    return [Convert]::ToHexString($bytes).ToLowerInvariant()
}

function Invoke-Podman([string[]] $arguments) {
    & podman --connection $connectionName @arguments
}

function Ensure-MachineAndConnection {
    $inspection = "$(& podman machine inspect $machineName --format '{{.State}} {{.Rootful}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($inspection)) {
        Write-Output "Initializing default Podman machine '$machineName'."
        & podman machine init --rootful=false $machineName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Default Podman machine initialization failed.' }
    }

    $rootful = "$(& podman machine inspect $machineName --format '{{.Rootful}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $rootful -eq 'true') {
        throw "Default Podman machine $machineName must be rootless."
    }

    $state = "$(& podman machine inspect $machineName --format '{{.State}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Default Podman machine inspection failed.' }
    if ($state -ne 'running') {
        Write-Output "Starting default Podman machine '$machineName'."
        & podman machine start $machineName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Default Podman machine start failed.' }
    }

    $connectionJson = (& podman system connection list --format json 2>$null) -join "`n"
    $connections = @()
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($connectionJson)) {
        try { $connections = @($connectionJson | ConvertFrom-Json) } catch { $connections = @() }
    }
    $connection = @($connections | Where-Object { $_.Name -eq $connectionName })
    if ($connection.Count -eq 0) {
        # machine init/start normally creates this entry. Restarting the machine is
        # safe and lets Podman regenerate its rootless connection after metadata loss.
        & podman machine stop $machineName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Podman machine stop failed while restoring its connection.' }
        & podman machine start $machineName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Podman machine start failed while restoring its connection.' }
    }
}

function Assert-Connection {
    $connectionJson = (& podman system connection list --format json 2>$null) -join "`n"
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($connectionJson)) {
        throw 'The Podman connection listing failed.'
    }
    try { $connections = @($connectionJson | ConvertFrom-Json) } catch { throw 'The Podman connection listing was not valid JSON.' }
    $matches = @($connections | Where-Object { $_.Name -eq $connectionName })
    if ($matches.Count -ne 1) {
        throw "The $connectionName Podman connection could not be established."
    }
    $uri = [string]$matches[0].URI
    if ($uri -notmatch '/run/user/\d+/podman/podman\.sock$') {
        throw 'The Podman connection is not the expected rootless user socket.'
    }
    $rootless = "$(Invoke-Podman @('info', '--format', '{{.Host.Security.Rootless}}') 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $rootless -ne 'true') { throw 'The Podman connection is not rootless.' }
    Record 'podman_connection' 'default_rootless_machine_verified'
}

function Assert-Machine {
    $machine = "$(& podman machine inspect $machineName --format '{{.State}} {{.Rootful}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $machine -notmatch '^running\s+False$') {
        throw "Default Podman machine must be running and rootless. Current: $machine"
    }
    Record 'podman_machine' 'running_rootless'
}

function Generate-Secrets {
    $secrets = @{}
    foreach ($name in $secretNames) {
        $secrets[$name] = New-Secret
    }
    if (($secretNames | ForEach-Object { $secrets[$_] } | Select-Object -Unique).Count -ne 3) {
        throw 'Generated secrets must be distinct.'
    }
    return $secrets
}

function Get-Crawl4AIToken {
    $candidates = @(
        $env:CRAWL4AI_API_TOKEN,
        [Environment]::GetEnvironmentVariable('CRAWL4AI_API_TOKEN', 'User')
    )
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and $candidate -match '^[0-9a-f]{64}$') {
            return $candidate.ToLowerInvariant()
        }
    }
    return New-Secret
}

function Start-SearXNG([hashtable] $secrets) {
    $containerName = 'searxng'
    $exists = "$(Invoke-Podman @('ps', '-a', '--filter', "name=^$containerName$", '--format', '{{.Names}}') 2>$null)".Trim()
    if ($exists) {
        Invoke-Podman @('rm', '-f', $containerName) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed to remove the existing SearXNG container." }
    }
    Invoke-Podman @(
        'run', '-d',
        '--name', $containerName,
        '--restart', 'unless-stopped',
        '-p', '127.0.0.1:8080:8080',
        '-e', "SEARXNG_SECRET=$($secrets['SEARXNG_SECRET'])",
        '-e', 'SEARXNG_LIMITER=false',
        '-e', 'SEARXNG_BASE_URL=http://127.0.0.1:8080/',
        '-v', "${searxngSettings}:/etc/searxng/settings.yml:ro",
        $searxngImage
    )
    if ($LASTEXITCODE -ne 0) { throw "Failed to start SearXNG container." }
    Record 'searxng_started' 'passed'
}

function Start-Crawl4AI([hashtable] $secrets) {
    $containerName = 'crawl4ai'
    $exists = "$(Invoke-Podman @('ps', '-a', '--filter', "name=^$containerName$", '--format', '{{.Names}}') 2>$null)".Trim()
    if ($exists) {
        Invoke-Podman @('rm', '-f', $containerName) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed to remove the existing Crawl4AI container." }
    }
    Invoke-Podman @(
        'run', '-d',
        '--name', $containerName,
        '--restart', 'unless-stopped',
        '-p', '127.0.0.1:11235:11235',
        '--shm-size', '1g',
        '-e', "CRAWL4AI_API_TOKEN=$($secrets['CRAWL4AI_API_TOKEN'])",
        '-e', "SECRET_KEY=$($secrets['SECRET_KEY'])",
        '-v', "${crawl4aiConfig}:/app/config.yml:ro",
        $crawl4aiImage
    )
    if ($LASTEXITCODE -ne 0) { throw "Failed to start Crawl4AI container." }
    Record 'crawl4ai_started' 'passed'
}

function Start-Headroom {
    $containerName = 'headroom'
    $exists = "$(Invoke-Podman @('ps', '-a', '--filter', "name=^$containerName$", '--format', '{{.Names}}') 2>$null)".Trim()
    if ($exists) {
        Invoke-Podman @('rm', '-f', $containerName) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed to remove the existing Headroom container." }
    }
    # Keyless by default: no -e SECRET args. Provider keys stay on the host opencode
    # process; the HTTP transport preserves the client Authorization header.
    # Phase 1 posture (non-secret only): cache mode + coding profile, auto
    # Kompress backend, Zen upstream hosts allowlisted. Values come from the
    # host environment with the pinned defaults below; never from secrets.
    # No workspace mount: pass-through proxy needs no host filesystem access.
    $headroomMode = if (-not [string]::IsNullOrWhiteSpace($env:HEADROOM_MODE)) { $env:HEADROOM_MODE } else { 'cache' }
    $headroomSavingsProfile = if (-not [string]::IsNullOrWhiteSpace($env:HEADROOM_SAVINGS_PROFILE)) { $env:HEADROOM_SAVINGS_PROFILE } else { 'coding' }
    $headroomKompressBackend = if (-not [string]::IsNullOrWhiteSpace($env:HEADROOM_KOMPRESS_BACKEND)) { $env:HEADROOM_KOMPRESS_BACKEND } else { 'auto' }
    $headroomUpstreamHosts = if (-not [string]::IsNullOrWhiteSpace($env:HEADROOM_UPSTREAM_ALLOWED_HOSTS)) { $env:HEADROOM_UPSTREAM_ALLOWED_HOSTS } else { 'opencode.ai' }
    Invoke-Podman @(
        'run', '-d',
        '--name', $containerName,
        '--restart', 'unless-stopped',
        '-p', '127.0.0.1:8787:8787',
        '-e', "HEADROOM_MODE=$headroomMode",
        '-e', "HEADROOM_SAVINGS_PROFILE=$headroomSavingsProfile",
        '-e', "HEADROOM_KOMPRESS_BACKEND=$headroomKompressBackend",
        '-e', "HEADROOM_UPSTREAM_ALLOWED_HOSTS=$headroomUpstreamHosts",
        '-v', 'headroom-state:/home/nonroot/.headroom',
        $headroomImage
    )
    if ($LASTEXITCODE -ne 0) { throw "Failed to start Headroom container." }
    Record 'headroom_started' 'passed'
}

function Start-HeadroomMcp {
    $containerName = 'headroom-mcp'
    $exists = "$(Invoke-Podman @('ps', '-a', '--filter', "name=^$containerName$", '--format', '{{.Names}}') 2>$null)".Trim()
    if ($exists) {
        Invoke-Podman @('rm', '-f', $containerName) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed to remove the existing Headroom MCP container." }
    }
    # Stateless MCP bridge: --entrypoint override to `mcp serve` (flags confirmed
    # via `mcp serve --help`: --transport http --host 127.0.0.1 --port 8788
    # --path /mcp --proxy-url http://127.0.0.1:8787). No volume, no secrets.
    # Proxy-URL uses container-name DNS (headroom:8787); 127.0.0.1 would be this
    # container's own loopback, not the headroom container.
    Invoke-Podman @(
        'run', '-d',
        '--name', $containerName,
        '--restart', 'unless-stopped',
        '-p', '127.0.0.1:8788:8788',
        '--entrypoint', 'headroom',
        $headroomImage,
        'mcp', 'serve',
        '--transport', 'http',
        '--host', '0.0.0.0',
        '--port', '8788',
        '--path', '/mcp',
        '--proxy-url', 'http://headroom:8787'
    )
    if ($LASTEXITCODE -ne 0) { throw "Failed to start Headroom MCP container." }
    Record 'headroom_mcp_started' 'passed'
}

function Verify-SearXNG {
    $deadline = (Get-Date).AddMinutes(2)
    $search = $null
    do {
        try {
            $search = Invoke-WebRequest -Uri 'http://127.0.0.1:8080/search?q=smoke&format=json' -Headers @{ 'X-Real-IP' = '127.0.0.1' } -TimeoutSec 20
        } catch {
            $search = $null
        }
        if ($null -eq $search -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 3 }
    } while ($null -eq $search -and (Get-Date) -lt $deadline)
    if ($null -eq $search -or $search.StatusCode -ne 200 -or ($search.Content | ConvertFrom-Json).query -ne 'smoke') {
        throw 'SearXNG JSON check failed.'
    }
    Record 'searxng_json' 'passed'
}

function Verify-Crawl4AI([hashtable] $secrets) {
    $deadline = (Get-Date).AddMinutes(2)
    $health = $null
    do {
        try {
            $health = Invoke-WebRequest -Uri 'http://127.0.0.1:11235/health' -TimeoutSec 15
        } catch {
            $health = $null
        }
        if ($null -eq $health -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 3 }
    } while ($null -eq $health -and (Get-Date) -lt $deadline)
    if ($null -eq $health -or $health.StatusCode -ne 200) {
        throw 'Crawl4AI health check failed.'
    }
    Record 'crawl4ai_health' 'passed'

    $noToken = Request-Status 'http://127.0.0.1:11235/crawl' $null 'Post' '{"urls":["https://example.com"]}'
    $wrongToken = Request-Status 'http://127.0.0.1:11235/crawl' ('0' * 64) 'Post' '{"urls":["https://example.com"]}'
    $rightToken = Request-Status 'http://127.0.0.1:11235/crawl' $secrets['CRAWL4AI_API_TOKEN'] 'Post' '{"urls":["https://example.com"]}'
    if ($noToken -notin @(401, 403) -or $wrongToken -notin @(401, 403) -or $rightToken -notin @(200, 201, 202)) {
        throw 'Crawl4AI authentication check failed.'
    }
    Record 'crawl4ai_authentication' 'missing_wrong_rejected_right_accepted'

    $sse = Request-SseStatus 'http://127.0.0.1:11235/mcp/sse' $secrets['CRAWL4AI_API_TOKEN']
    if ($sse -notin @(200, 201)) {
        throw 'Crawl4AI SSE endpoint check failed.'
    }
    Record 'crawl4ai_sse' 'passed'
}

function Verify-Headroom {
    # Keyless reality: the proxy gates /v1/* behind auth (401 without a token),
    # so the primary gate is the unauthenticated readiness surface.
    $deadline = (Get-Date).AddMinutes(2)
    $ready = 0
    $health = 0
    do {
        $ready = Request-Status 'http://127.0.0.1:8787/readyz' $null
        $health = Request-Status 'http://127.0.0.1:8787/health' $null
        if (($ready -ne 200 -or $health -ne 200) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 3 }
    } while (($ready -ne 200 -or $health -ne 200) -and (Get-Date) -lt $deadline)
    if ($ready -ne 200 -or $health -ne 200) {
        throw 'Headroom readiness check failed.'
    }
    Record 'headroom_ready' 'passed'

    # The keyless proxy must expose the models route and enforce authentication.
    $models = Request-Status 'http://127.0.0.1:8787/v1/models' $null
    if ($models -ne 401) {
        throw "Headroom models endpoint check failed; expected HTTP 401 without credentials, received $models."
    }
    Record 'headroom_models' 'keyless_auth_enforced_401'
}

function Invoke-McpPost([string] $uri, [string] $sessionId, [string] $body) {
    $client = $null
    $response = $null
    try {
        $client = [System.Net.Http.HttpClient]::new()
        $client.Timeout = [TimeSpan]::FromSeconds(15)
        $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $uri)
        $request.Headers.Accept.ParseAdd('application/json, text/event-stream')
        if (-not [string]::IsNullOrWhiteSpace($sessionId)) {
            $request.Headers.Add('Mcp-Session-Id', $sessionId)
        }
        $request.Content = [System.Net.Http.StringContent]::new($body, [System.Text.Encoding]::UTF8, 'application/json')
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        $content = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        $session = ''
        try {
            $session = [string](@($response.Headers.GetValues('Mcp-Session-Id'))[0])
        } catch {
            $session = ''
        }
        return @{ Status = [int]$response.StatusCode; Body = [string]$content; Session = [string]$session }
    } finally {
        if ($null -ne $response) { $response.Dispose() }
        if ($null -ne $client) { $client.Dispose() }
    }
}

function Verify-HeadroomMcp {
    # Streamable HTTP: initialize POST to /mcp (exact --path) establishes the
    # session (Mcp-Session-Id response header); tools/list must then expose the
    # headroom compress/retrieve/stats tools.
    $mcpUri = 'http://127.0.0.1:8788/mcp'
    $deadline = (Get-Date).AddMinutes(2)
    $sessionId = ''
    do {
        try {
            $init = Invoke-McpPost $mcpUri '' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"launch-check","version":"1.0"}}}'
            if ($init.Status -eq 200 -and $init.Body -match '"result"' -and -not [string]::IsNullOrWhiteSpace($init.Session)) {
                $sessionId = $init.Session
            }
        } catch {
            $sessionId = ''
        }
        if ([string]::IsNullOrWhiteSpace($sessionId) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 3 }
    } while ([string]::IsNullOrWhiteSpace($sessionId) -and (Get-Date) -lt $deadline)
    if ([string]::IsNullOrWhiteSpace($sessionId)) {
        throw 'Headroom MCP initialize check failed.'
    }
    Record 'headroom_mcp_initialize' 'session_established'

    try {
        Invoke-McpPost $mcpUri $sessionId '{"jsonrpc":"2.0","method":"notifications/initialized"}' | Out-Null
    } catch {
    }
    $tools = Invoke-McpPost $mcpUri $sessionId '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
    if ($tools.Status -ne 200 -or $tools.Body -notmatch 'headroom_compress' -or $tools.Body -notmatch 'headroom_retrieve' -or $tools.Body -notmatch 'headroom_stats') {
        throw 'Headroom MCP tools/list check failed.'
    }
    Record 'headroom_mcp_tools' 'compress_retrieve_stats_present'
}

function Request-Status([string] $uri, [string] $token, [string] $method = 'Get', [string] $body = '') {
    try {
        $headers = @{}
        if ($null -ne $token) { $headers.Authorization = "Bearer $token" }
        $response = Invoke-WebRequest -Uri $uri -Method $method -Headers $headers -Body $body -ContentType 'application/json' -TimeoutSec 15
        return [int]$response.StatusCode
    } catch {
        if ($_.Exception.Response) { return [int]$_.Exception.Response.StatusCode.value__ }
        return 0
    }
}

function Request-SseStatus([string] $uri, [string] $token) {
    $client = $null
    $response = $null
    try {
        $client = [System.Net.Http.HttpClient]::new()
        $client.Timeout = [TimeSpan]::FromSeconds(15)
        $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Get, $uri)
        if ($null -ne $token) { $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token) }
        $response = $client.SendAsync($request, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
        return [int]$response.StatusCode
    } catch {
        if ($_.Exception.Response) { return [int]$_.Exception.Response.StatusCode.value__ }
        return 0
    } finally {
        if ($null -ne $response) { $response.Dispose() }
        if ($null -ne $client) { $client.Dispose() }
    }
}

function Assert-HostPorts {
    $mappings = @(Invoke-Podman @('port', 'searxng'); Invoke-Podman @('port', 'crawl4ai'); Invoke-Podman @('port', 'headroom'); Invoke-Podman @('port', 'headroom-mcp')) -join ' '
    if ($mappings -notmatch '127\.0\.0\.1:8080' -or $mappings -notmatch '127\.0\.0\.1:11235' -or $mappings -notmatch '127\.0\.0\.1:8787' -or $mappings -notmatch '127\.0\.0\.1:8788') {
        throw 'Port mapping policy check failed.'
    }
    Record 'port_mappings' 'loopback_only'
}

New-Item -ItemType Directory -Path $PSScriptRoot -Force | Out-Null
'' | Set-Content -LiteralPath $reportPath -Encoding utf8

if (-not (Get-Command podman -ErrorAction SilentlyContinue)) { Stop-WithMessage 'Podman is required.' }
Record 'podman' 'available'

Ensure-MachineAndConnection
Record 'podman_machine' 'ensured_running_rootless'
Assert-Connection
Assert-Machine

$secrets = Generate-Secrets
$secrets['CRAWL4AI_API_TOKEN'] = Get-Crawl4AIToken
$env:CRAWL4AI_API_TOKEN = $secrets['CRAWL4AI_API_TOKEN']
Record 'crawl4ai_token' 'reused_or_generated_without_output'

Start-SearXNG $secrets
Start-Crawl4AI $secrets
Start-Headroom
Start-HeadroomMcp

Start-Sleep -Seconds 5

try {
    Verify-SearXNG
    Verify-Crawl4AI $secrets
    Verify-Headroom
    Verify-HeadroomMcp
    Assert-HostPorts
} catch {
    $results | Set-Content -LiteralPath $reportPath -Encoding utf8
    throw
}

[Environment]::SetEnvironmentVariable('CRAWL4AI_API_TOKEN', $secrets['CRAWL4AI_API_TOKEN'], 'User')
Record 'opencode_user_environment' 'token_persisted_after_validation_without_output'

$results | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Output "Stack launched and runtime checks passed. Redacted report: $reportPath"
Write-Output 'Restart OpenCode so its process receives the persisted CRAWL4AI_API_TOKEN.'