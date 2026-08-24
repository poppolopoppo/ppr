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
$crawl4aiImage = 'docker.io/unclecode/crawl4ai:0.9.2'
$searxngImage = 'ghcr.io/searxng/searxng:latest'
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
    $mappings = @(Invoke-Podman @('port', 'searxng'); Invoke-Podman @('port', 'crawl4ai')) -join ' '
    if ($mappings -notmatch '127\.0\.0\.1:8080' -or $mappings -notmatch '127\.0\.0\.1:11235') {
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

Start-Sleep -Seconds 5

Verify-SearXNG
Verify-Crawl4AI $secrets
Assert-HostPorts

[Environment]::SetEnvironmentVariable('CRAWL4AI_API_TOKEN', $secrets['CRAWL4AI_API_TOKEN'], 'User')
Record 'opencode_user_environment' 'token_persisted_after_validation_without_output'

$results | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Output "Stack launched and runtime checks passed. Redacted report: $reportPath"
Write-Output 'Restart OpenCode so its process receives the persisted CRAWL4AI_API_TOKEN.'