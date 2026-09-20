param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory
)

$sourcePath = Join-Path $BuildDirectory 'compile_commands.json'
$outputPath = Join-Path $BuildDirectory 'compile_commands.vscode.json'

if (-not (Test-Path -LiteralPath $sourcePath)) {
    throw "Missing compile database: $sourcePath"
}

$entries = Get-Content -Raw -LiteralPath $sourcePath | ConvertFrom-Json
foreach ($entry in $entries) {
    $entry.command = [regex]::Replace($entry.command, '@(?<path>[^\s]+\.modmap)', {
        param($match)

        $modmapPath = $match.Groups['path'].Value.Replace('\', [IO.Path]::DirectorySeparatorChar)
        $fullPath = Join-Path $entry.directory $modmapPath
        if (-not (Test-Path -LiteralPath $fullPath)) {
            return $match.Value
        }

        $references = Get-Content -LiteralPath $fullPath |
            Where-Object { $_ -match '^\s*-reference\s+"([^"]+)"' } |
            ForEach-Object { '/reference ' + $_.Trim().Substring(11) }

        if ($references.Count -eq 0) {
            return ''
        }

        return $references -join ' '
    })
}

$entries | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $outputPath -Encoding utf8
Write-Output "Wrote $outputPath"
