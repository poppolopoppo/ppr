[CmdletBinding()]
param(
    [string] $BuildDir = "out/build/msvc-dev",
    [ValidateSet("summary", "target", "compiler", "deps", "timing")]
    [string] $View = "summary",
    [string] $Target,
    [switch] $Json
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$resolvedBuildDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else {
    Join-Path $repoRoot $BuildDir
}
$resolvedBuildDir = (Resolve-Path $resolvedBuildDir -ErrorAction SilentlyContinue)?.Path ?? $resolvedBuildDir
$replyRoot = Join-Path $resolvedBuildDir ".cmake/api/v1/reply"
$queryRoot = Join-Path $resolvedBuildDir ".cmake/api/v1/query"

function Get-JsonObject {
    param([string] $Path)
    Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -Depth 100
}

function Get-CollectionCount {
    param($Value)
    if ($null -eq $Value) {
        return 0
    }
    @($Value).Count
}

function Get-Status {
    if (-not (Test-Path -LiteralPath $resolvedBuildDir)) {
        return [pscustomobject]@{ state = "missing-build-tree"; detail = $resolvedBuildDir }
    }

    if (-not (Test-Path -LiteralPath $replyRoot)) {
        return [pscustomobject]@{ state = "unconfigured-build-tree"; detail = $replyRoot }
    }

    $index = Get-ChildItem -LiteralPath $replyRoot -Filter "index-*.json" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($null -eq $index) {
        return [pscustomobject]@{ state = "missing-reply-index"; detail = $replyRoot }
    }

    $queryNewest = Get-ChildItem -LiteralPath $queryRoot -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    $state = if ($null -ne $queryNewest -and $queryNewest.LastWriteTimeUtc -gt $index.LastWriteTimeUtc) {
        "query-newer-than-reply"
    } else {
        "fresh-enough"
    }

    [pscustomobject]@{
        state = $state
        detail = $index.FullName
        index = $index
        queryNewest = $queryNewest
    }
}

function Resolve-ReplyFile {
    param(
        [Parameter(Mandatory)] $IndexObject,
        [Parameter(Mandatory)] [string] $ReplyKey
    )

    $entry = $IndexObject.reply.$ReplyKey
    if ($null -eq $entry) {
        return $null
    }

    $path = Join-Path $replyRoot $entry.jsonFile
    if (-not (Test-Path -LiteralPath $path)) {
        throw "missing-object-reply: $($entry.jsonFile)"
    }
    $path
}

function Get-Model {
    $status = Get-Status
    if ($status.state -ne "fresh-enough" -and $status.state -ne "query-newer-than-reply") {
        return [pscustomobject]@{ status = $status }
    }

    $indexObject = Get-JsonObject -Path $status.index.FullName
    $codemodelPath = Resolve-ReplyFile -IndexObject $indexObject -ReplyKey "codemodel-v2"
    $toolchainsPath = Resolve-ReplyFile -IndexObject $indexObject -ReplyKey "toolchains-v1"
    $cachePath = Resolve-ReplyFile -IndexObject $indexObject -ReplyKey "cache-v2"

    $codemodel = Get-JsonObject -Path $codemodelPath
    $toolchains = Get-JsonObject -Path $toolchainsPath
    $cache = Get-JsonObject -Path $cachePath
    $configuration = $codemodel.configurations[0]

    $targetMap = @{}
    foreach ($entry in @($configuration.targets) + @($configuration.abstractTargets)) {
        if ($null -eq $entry) {
            continue
        }
        $targetMap[$entry.id] = $entry
        if (-not $targetMap.ContainsKey($entry.name)) {
            $targetMap[$entry.name] = $entry
        }
    }

    [pscustomobject]@{
        status = $status
        indexObject = $indexObject
        codemodel = $codemodel
        toolchains = $toolchains
        cache = $cache
        configuration = $configuration
        targetMap = $targetMap
    }
}

function Resolve-TargetEntry {
    param($Model, [string] $TargetName)

    if ([string]::IsNullOrWhiteSpace($TargetName)) {
        throw "target view requires -Target"
    }

    $match = @($Model.configuration.targets + $Model.configuration.abstractTargets) |
        Where-Object { $_.name -eq $TargetName } |
        Select-Object -First 1
    if ($null -eq $match) {
        throw "target not found: $TargetName"
    }
    $match
}

function Get-TargetObject {
    param($Model, [string] $TargetName)

    $targetEntry = Resolve-TargetEntry -Model $Model -TargetName $TargetName
    $targetPath = Join-Path $replyRoot $targetEntry.jsonFile
    if (-not (Test-Path -LiteralPath $targetPath)) {
        throw "missing-object-reply: $($targetEntry.jsonFile)"
    }

    $targetObject = Get-JsonObject -Path $targetPath
    $resolveName = {
        param([string] $Id)
        if ($Model.targetMap.ContainsKey($Id)) {
            return $Model.targetMap[$Id].name
        }
        $Id
    }

    $fileSetCounts = @{}
    foreach ($source in @($targetObject.interfaceSources)) {
        foreach ($index in @($source.fileSetIndexes)) {
            $key = [string] $index
            if (-not $fileSetCounts.ContainsKey($key)) {
                $fileSetCounts[$key] = 0
            }
            $fileSetCounts[$key]++
        }
    }

    $fileSets = @()
    for ($i = 0; $i -lt @($targetObject.fileSets).Count; $i++) {
        $fileSet = $targetObject.fileSets[$i]
        $fileSets += [pscustomobject]@{
            index = $i
            name = $fileSet.name
            type = $fileSet.type
            visibility = $fileSet.visibility
            baseDirectories = @($fileSet.baseDirectories)
            interfaceSourceCount = if ($fileSetCounts.ContainsKey([string] $i)) { $fileSetCounts[[string] $i] } else { 0 }
        }
    }

    [pscustomobject]@{
        name = $targetObject.name
        id = $targetObject.id
        type = $targetObject.type
        paths = $targetObject.paths
        artifacts = @($targetObject.artifacts | ForEach-Object { $_.path })
        dependencies = @($targetObject.dependencies | ForEach-Object { & $resolveName $_.id })
        compileDependencies = @($targetObject.compileDependencies | ForEach-Object { & $resolveName $_.id })
        linkLibraries = @($targetObject.linkLibraries | ForEach-Object { & $resolveName $_.id })
        interfaceLinkLibraries = @($targetObject.interfaceLinkLibraries | ForEach-Object { & $resolveName $_.id })
        compileGroups = @($targetObject.compileGroups | ForEach-Object {
                [pscustomobject]@{
                    language = $_.language
                    sourceIndexes = @($_.sourceIndexes)
                    includes = @($_.includes | ForEach-Object { $_.path })
                    defines = @($_.defines | ForEach-Object { $_.define })
                    fragments = @($_.compileCommandFragments | ForEach-Object { $_.fragment })
                }
            })
        sourceGroups = @($targetObject.sourceGroups | ForEach-Object {
                [pscustomobject]@{
                    name = $_.name
                    sourceCount = if ($_.PSObject.Properties.Name -contains "sourceIndexes") { Get-CollectionCount $_.sourceIndexes } else { 0 }
                    interfaceSourceCount = if ($_.PSObject.Properties.Name -contains "interfaceSourceIndexes") { Get-CollectionCount $_.interfaceSourceIndexes } else { 0 }
                }
            })
        fileSets = $fileSets
        interfaceSources = @($targetObject.interfaceSources | ForEach-Object { $_.path })
        sources = @($targetObject.sources | ForEach-Object { $_.path })
    }
}

function Get-SummaryObject {
    param($Model)

    $configuration = $Model.configuration
    $preferred = "engine.core", "engine.math", "engine.shader", "engine.rhi", "engine.image", "engine.mesh", "engine.sim", "engine.physics", "engine.app", "engine.tests", "engine.tests.core", "engine.tests.sim", "engine.tests.app", "engine.tests.asset", "game.tests", "app.game", "run-engine-tests"
    $present = foreach ($name in $preferred) {
        @($configuration.targets + $configuration.abstractTargets) |
            Where-Object { $_.name -eq $name } |
            Select-Object -First 1 |
            ForEach-Object { $_.name }
    }

    [pscustomobject]@{
        status = $Model.status.state
        buildDir = $resolvedBuildDir
        replyIndex = $Model.status.index.FullName
        replyIndexUtc = $Model.status.index.LastWriteTimeUtc
        queryNewestUtc = $Model.status.queryNewest?.LastWriteTimeUtc
        cmakeVersion = $Model.indexObject.cmake.version.string
        generator = $Model.indexObject.cmake.generator.name
        multiConfig = $Model.indexObject.cmake.generator.multiConfig
        projectCount = @($configuration.projects).Count
        directoryCount = @($configuration.directories).Count
        targetCount = @($configuration.targets).Count
        abstractTargetCount = @($configuration.abstractTargets).Count
        primaryTargets = @($present)
    }
}

function Get-CompilerObject {
    param($Model)

    [pscustomobject]@{
        status = $Model.status.state
        buildDir = $resolvedBuildDir
        toolchains = @($Model.toolchains.toolchains | ForEach-Object {
                [pscustomobject]@{
                    language = $_.language
                    compilerId = if ($_.compiler.PSObject.Properties.Name -contains "id") { $_.compiler.id } else { "unknown" }
                    compilerPath = $_.compiler.path
                    compilerVersion = if ($_.compiler.PSObject.Properties.Name -contains "version") { $_.compiler.version } else { "unknown" }
                    sourceFileExtensions = @($_.sourceFileExtensions)
                }
            })
    }
}

function Get-DepsObject {
    param($Model, [string] $TargetName)

    if ($TargetName) {
        $target = Get-TargetObject -Model $Model -TargetName $TargetName
        return [pscustomobject]@{
            status = $Model.status.state
            buildDir = $resolvedBuildDir
            target = $target.name
            dependencies = $target.dependencies
            compileDependencies = $target.compileDependencies
            linkLibraries = $target.linkLibraries
            interfaceLinkLibraries = $target.interfaceLinkLibraries
        }
    }

    [pscustomobject]@{
        status = $Model.status.state
        buildDir = $resolvedBuildDir
        targets = @($Model.configuration.targets | ForEach-Object {
                [pscustomobject]@{
                    name = $_.name
                    id = $_.id
                }
            })
    }
}

function Get-TimingObject {
    $artifacts = New-Object System.Collections.Generic.List[object]

    $ninjaLog = Join-Path $resolvedBuildDir ".ninja_log"
    if (Test-Path -LiteralPath $ninjaLog) {
        $entries = foreach ($line in Get-Content -LiteralPath $ninjaLog | Select-Object -Skip 1) {
            $parts = $line -split "`t"
            if ($parts.Count -lt 5) {
                continue
            }
            $start = [int] $parts[0]
            $end = [int] $parts[1]
            [pscustomobject]@{
                output = $parts[3]
                durationMs = $end - $start
            }
        }
        $artifacts.Add([pscustomobject]@{
                kind = "ninja-log"
                path = $ninjaLog
                topDurations = @($entries | Sort-Object durationMs -Descending | Select-Object -First 10)
            })
    }

    foreach ($candidate in @(
            (Join-Path $resolvedBuildDir "CMakeFiles/CMakeConfigureLog.yaml"),
            (Join-Path $resolvedBuildDir "CMakeFiles/CMakeOutput.log"),
            (Join-Path $resolvedBuildDir "CMakeFiles/CMakeError.log")
        )) {
        if (Test-Path -LiteralPath $candidate) {
            $item = Get-Item -LiteralPath $candidate
            $artifacts.Add([pscustomobject]@{
                    kind = "cmake-log"
                    path = $candidate
                    lastWriteTimeUtc = $item.LastWriteTimeUtc
                })
        }
    }

    $profileFiles = Get-ChildItem -LiteralPath $resolvedBuildDir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "(?i)(cmake|configure|profile|trace).*(json|trace)$" } |
        Select-Object -First 20
    foreach ($file in $profileFiles) {
        $artifacts.Add([pscustomobject]@{
                kind = "profile-or-trace"
                path = $file.FullName
                lastWriteTimeUtc = $file.LastWriteTimeUtc
            })
    }

    [pscustomobject]@{
        buildDir = $resolvedBuildDir
        available = $artifacts.Count -gt 0
        artifacts = @($artifacts)
    }
}

function Format-Text {
    param($Object, [string] $CurrentView)

    switch ($CurrentView) {
        "summary" {
            @(
                "status: $($Object.status)",
                "buildDir: $($Object.buildDir)",
                "replyIndexUtc: $($Object.replyIndexUtc)",
                "cmake: $($Object.cmakeVersion)",
                "generator: $($Object.generator) (multiConfig=$($Object.multiConfig))",
                "projects: $($Object.projectCount) directories: $($Object.directoryCount) targets: $($Object.targetCount) abstractTargets: $($Object.abstractTargetCount)",
                "primaryTargets: $([string]::Join(', ', $Object.primaryTargets))"
            ) -join [Environment]::NewLine
        }
        "target" {
            $group = $Object.compileGroups | Select-Object -First 1
            $moduleSets = $Object.fileSets | ForEach-Object {
                "  - [$($_.index)] $($_.name) type=$($_.type) visibility=$($_.visibility) interfaceSources=$($_.interfaceSourceCount)"
            }
            $moduleSample = @($Object.interfaceSources | Select-Object -First 12)
            $sourceSample = @($Object.sources | Select-Object -First 12)
            $sourceGroupSummary = [string]::Join('; ', @($Object.sourceGroups | ForEach-Object {
                        "$($_.name):src=$($_.sourceCount),iface=$($_.interfaceSourceCount)"
                    }))
            @(
                "name: $($Object.name)",
                "type: $($Object.type)",
                "artifact: $([string]::Join(', ', $Object.artifacts))",
                "sourcePath: $($Object.paths.source)",
                "buildPath: $($Object.paths.build)",
                "dependencies: $([string]::Join(', ', $Object.dependencies))",
                "compileDependencies: $([string]::Join(', ', $Object.compileDependencies))",
                "linkLibraries: $([string]::Join(', ', $Object.linkLibraries))",
                "interfaceLinkLibraries: $([string]::Join(', ', $Object.interfaceLinkLibraries))",
                "compileLanguage: $($group.language)",
                "compileFragments: $([string]::Join(' ', @($group.fragments)))",
                "sourceGroups: $sourceGroupSummary",
                "fileSets:",
                ($moduleSets -join [Environment]::NewLine),
                "interfaceSourcesCount: $(@($Object.interfaceSources).Count)",
                "interfaceSourcesSample:",
                (@($moduleSample | ForEach-Object { "  - $_" }) -join [Environment]::NewLine),
                "sourcesCount: $(@($Object.sources).Count)",
                "sourcesSample:",
                (@($sourceSample | ForEach-Object { "  - $_" }) -join [Environment]::NewLine),
                "note: use -Json for complete arrays"
            ) -join [Environment]::NewLine
        }
        "compiler" {
            @(
                "status: $($Object.status)",
                "buildDir: $($Object.buildDir)",
                (@($Object.toolchains | ForEach-Object { "  - $($_.language): $($_.compilerId) $($_.compilerVersion) @ $($_.compilerPath)" }) -join [Environment]::NewLine)
            ) -join [Environment]::NewLine
        }
        "deps" {
            if ($Object.PSObject.Properties.Name -contains "target") {
                @(
                    "status: $($Object.status)",
                    "target: $($Object.target)",
                    "dependencies: $([string]::Join(', ', $Object.dependencies))",
                    "compileDependencies: $([string]::Join(', ', $Object.compileDependencies))",
                    "linkLibraries: $([string]::Join(', ', $Object.linkLibraries))",
                    "interfaceLinkLibraries: $([string]::Join(', ', $Object.interfaceLinkLibraries))"
                ) -join [Environment]::NewLine
            } else {
                @(
                    "status: $($Object.status)",
                    "targets:",
                    (@($Object.targets | ForEach-Object { "  - $($_.name)" }) -join [Environment]::NewLine)
                ) -join [Environment]::NewLine
            }
        }
        "timing" {
            if (-not $Object.available) {
                return "timing unavailable in current build tree"
            }
            $lines = New-Object System.Collections.Generic.List[string]
            $lines.Add("buildDir: $($Object.buildDir)")
            foreach ($artifact in $Object.artifacts) {
                $lines.Add("- $($artifact.kind): $($artifact.path)")
                if ($artifact.PSObject.Properties.Name -contains "topDurations") {
                    foreach ($entry in @($artifact.topDurations)) {
                        $lines.Add("    $($entry.durationMs) ms  $($entry.output)")
                    }
                }
            }
            $lines -join [Environment]::NewLine
        }
    }
}

try {
    if ($View -eq "timing") {
        $result = Get-TimingObject
    } else {
        $model = Get-Model
        if ($model.status.state -ne "fresh-enough" -and $model.status.state -ne "query-newer-than-reply") {
            $result = [pscustomobject]@{
                status = $model.status.state
                detail = $model.status.detail
                buildDir = $resolvedBuildDir
            }
        } else {
            switch ($View) {
                "summary" { $result = Get-SummaryObject -Model $model }
                "target" { $result = Get-TargetObject -Model $model -TargetName $Target }
                "compiler" { $result = Get-CompilerObject -Model $model }
                "deps" { $result = Get-DepsObject -Model $model -TargetName $Target }
            }
        }
    }

    if ($Json) {
        $result | ConvertTo-Json -Depth 100
    } else {
        if ($result.PSObject.Properties.Name -contains "status" -and
            $result.PSObject.Properties.Name -contains "detail" -and
            @($result.PSObject.Properties.Name).Count -le 3) {
            "status: $($result.status)" + [Environment]::NewLine +
            "detail: $($result.detail)" + [Environment]::NewLine +
            "buildDir: $($result.buildDir)"
        } else {
            Format-Text -Object $result -CurrentView $View
        }
    }
} catch {
    if ($Json) {
        [pscustomobject]@{
            status = "error"
            message = $_.Exception.Message
            buildDir = $resolvedBuildDir
        } | ConvertTo-Json -Depth 20
    } else {
        "status: error" + [Environment]::NewLine +
        "message: $($_.Exception.Message)" + [Environment]::NewLine +
        "buildDir: $resolvedBuildDir"
    }
    exit 1
}
