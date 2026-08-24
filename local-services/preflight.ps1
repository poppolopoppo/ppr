$ErrorActionPreference = 'Stop'

$envPath = Join-Path $PSScriptRoot '.env'
if (-not (Test-Path -LiteralPath $envPath -PathType Leaf)) {
    throw 'Missing local-services/.env; copy .env.example and install reviewed values.'
}

$values = @{}
foreach ($line in Get-Content -LiteralPath $envPath) {
    if ($line -match '^\s*(?:#|$)') { continue }
    if ($line -notmatch '^\s*([A-Za-z_][A-Za-z0-9_]*)=(.*)\s*$') {
        throw 'Invalid .env line; expected KEY=VALUE.'
    }
    $values[$Matches[1]] = $Matches[2].Trim()
}

function Require-Value([string] $name) {
    if (-not $values.ContainsKey($name) -or [string]::IsNullOrWhiteSpace($values[$name])) {
        throw "$name is required and must not be blank."
    }
    if ($values[$name] -match '(?i)replace-with|example|default|changeme|set-a-long|<resolved|placeholder') {
        throw "$name contains an example/default value."
    }
}

foreach ($name in @('CRAWL4AI_IMAGE', 'SEARXNG_IMAGE', 'VALKEY_IMAGE', 'CRAWL4AI_API_TOKEN', 'SECRET_KEY', 'SEARXNG_SECRET')) {
    Require-Value $name
}

$digestPattern = '^(docker\.io|ghcr\.io)/[^\s:@/]+(?:/[^\s:@/]+)*:[^\s@]+@sha256:[0-9a-f]{64}$'
foreach ($name in @('CRAWL4AI_IMAGE', 'SEARXNG_IMAGE', 'VALKEY_IMAGE')) {
    if ($values[$name] -notmatch $digestPattern -or $values[$name] -match '(?i):latest@') {
        throw "$name must be a fully-qualified registry reference with a sha256 digest."
    }
}

foreach ($name in @('CRAWL4AI_API_TOKEN', 'SECRET_KEY', 'SEARXNG_SECRET')) {
    if ($values[$name] -notmatch '^[0-9a-f]{64}$') {
        throw "$name must be exactly 64 unquoted lowercase hexadecimal characters from 32 random bytes."
    }
}
if ($values['CRAWL4AI_API_TOKEN'] -eq $values['SECRET_KEY'] -or
    $values['CRAWL4AI_API_TOKEN'] -eq $values['SEARXNG_SECRET'] -or
    $values['SECRET_KEY'] -eq $values['SEARXNG_SECRET']) {
    throw 'Crawl4AI token, JWT secret, and SearXNG secret must be distinct.'
}

Write-Output 'Preflight passed: required values are present, image references are immutable, and secret policy checks passed.'
