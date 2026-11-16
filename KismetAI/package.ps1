#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Package the KismetAI plugin for release and/or dev distribution.

.DESCRIPTION
    release: clean UAT BuildPlugin, written as two archives of the same build:
               ...-release.zip      binaries, config and notices; Source/ holds only
                                    precompiled Build.cs stubs. The default download,
                                    and what the installers and self-updater fetch.
               ...-release-src.zip  the same build plus the complete buildable source.
             .modules BuildId rewritten to -ReleaseBuildId (default: empty string).
    dev:     full snapshot of the plugin tree as-is: Binaries (with .pdb), Source,
             Config, Docs, Resources, Intermediate. Reuses whatever UBT last stamped.

    Output naming: KismetAI-<PluginVersion>-UE<EngineVersion>-<Platforms>-<variant>.zip

.PARAMETER EnginePath
    Root of a UE 5.x source or binary install (the directory containing Engine/).
    Required unless -PrebuiltPackageDir is given.

.PARAMETER PrebuiltPackageDir
    An existing UAT BuildPlugin output directory to package instead of running
    BuildPlugin here. Used for Linux, which is compiled inside Epic's Docker
    image (scripts/release.sh) where this script cannot run. Build it with
    KISMETAI_EMBED_CONFIG=1. Requires -EngineVersionOverride.

.PARAMETER EngineVersionOverride
    Engine Major.Minor (e.g. 5.9) for the archive name when there is no
    -EnginePath to read it from.

.PARAMETER KeepBuildId
    Leave the BuildId that the build stamped into *.modules. For builds whose
    engine has no Launcher install to copy an id from (Linux).

.PARAMETER Variant
    'release', 'dev', or 'both'. Default: both.

.PARAMETER TargetPlatforms
    One or more UE platform names (Win64, Mac, Linux, Android, ...). Default: Win64.

.PARAMETER OutputDir
    Where to drop the zip(s). Default: <plugin-parent>/releases.

.PARAMETER ReleaseBuildId
    Explicit GUID to stamp into Binaries/*/UnrealEditor.modules for the release
    archive. Overrides -LauncherEnginePath when both are given. Default: empty
    string (NOT recommended — UE refuses to load plugins with mismatched BuildId,
    and an empty value matches no real engine; only use for diagnostic builds).

.PARAMETER LauncherEnginePath
    Path to an installed (Launcher) UE engine of the target version. The script
    reads its Engine/Binaries/<Platform>/UnrealEditor.modules and stamps the
    BuildId from there into the release zip, so end users on the same Launcher
    version can load the binary directly. Use this when -EnginePath points at a
    source-built engine but the binary needs to ship for Launcher users.
    Ignored if -ReleaseBuildId is explicitly set.

.PARAMETER DevBuildId
    If specified, rewrite .modules BuildId in the dev archive too. Omit to keep
    whatever UBT stamped.

.EXAMPLE
    ./package.ps1 -EnginePath C:\UE_5.6
    # Builds both release and dev for Win64.

.EXAMPLE
    ./package.ps1 -EnginePath C:\UE_5.6 -Variant release -TargetPlatforms Win64,Linux
    # Multi-platform release only.

.EXAMPLE
    ./package.ps1 `
        -EnginePath         ~/UnrealEngine-5.6 `
        -LauncherEnginePath '/Users/Shared/Epic Games/UE_5.6' `
        -Variant release -TargetPlatforms Mac
    # Build with the local source engine but stamp the Launcher 5.6 BuildId,
    # so the resulting zip loads on end-user Launcher installs.
#>
[CmdletBinding()]
param(
    [string]$EnginePath,

    [string]$PrebuiltPackageDir,

    [string]$EngineVersionOverride,

    [switch]$KeepBuildId,

    [ValidateSet('release', 'dev', 'both')]
    [string]$Variant = 'both',

    [string[]]$TargetPlatforms = @('Win64'),

    [string]$OutputDir,

    [string]$ReleaseBuildId = '',

    [string]$LauncherEnginePath,

    [string]$DevBuildId,

    # Base URL prepended to release zip filenames in the generated manifest.
    # Example: "https://kismetai.example.com/releases/" produces URLs like
    # "https://kismetai.example.com/releases/KismetAI-0.1.7-UE5.6-Mac-release.zip".
    # Trailing slash is added if missing.
    [string]$ManifestBaseURL,

    # Free-form release notes shown to users in the update prompt.
    [string]$ReleaseNotes,

    # Path to a file whose contents become the release notes (overrides -ReleaseNotes).
    [string]$ReleaseNotesFile,

    # Skip manifest.json regeneration even on a release build.
    [switch]$SkipManifest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$PluginRoot  = $PSScriptRoot
$UPluginPath = Join-Path $PluginRoot 'KismetAI.uplugin'
if (-not (Test-Path $UPluginPath)) {
    throw "Can't find KismetAI.uplugin at $UPluginPath - place this script next to the .uplugin."
}

if (-not $OutputDir) {
    # Repo-root releases/ (PluginRoot = plugin/KismetAI -> up two levels), where
    # all historical zips and the published manifest live. Was plugin/releases/.
    $RepoRoot  = Split-Path (Split-Path $PluginRoot -Parent) -Parent
    $OutputDir = Join-Path $RepoRoot 'releases'
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

# Read plugin version
$UPluginJson = Get-Content $UPluginPath -Raw | ConvertFrom-Json
$PluginVersion = $UPluginJson.VersionName
if (-not $PluginVersion) { throw "KismetAI.uplugin has no VersionName" }

$IsWin = if ($PSVersionTable.PSVersion.Major -ge 6) { $IsWindows } else { $true }

if ($PrebuiltPackageDir) {
    # Packaging a build made elsewhere: no engine on this host is involved.
    if ($Variant -ne 'release') { throw '-PrebuiltPackageDir only applies to -Variant release.' }
    if (-not (Test-Path -LiteralPath (Join-Path $PrebuiltPackageDir 'KismetAI.uplugin'))) {
        throw "No KismetAI.uplugin in $PrebuiltPackageDir - is it a BuildPlugin output directory?"
    }
    if ($EngineVersionOverride -notmatch '^\d+\.\d+$') {
        throw '-PrebuiltPackageDir needs -EngineVersionOverride <Major.Minor>.'
    }
    $EngineVersion = $EngineVersionOverride
    $UATScript = $null
} else {
    if (-not $EnginePath) { throw '-EnginePath is required (or pass -PrebuiltPackageDir).' }

    # Read engine version
    $BuildVersionPath = Join-Path $EnginePath 'Engine/Build/Build.version'
    if (-not (Test-Path $BuildVersionPath)) {
        throw "Can't find $BuildVersionPath - is -EnginePath correct?"
    }
    $BuildVersionJson = Get-Content $BuildVersionPath -Raw | ConvertFrom-Json
    $EngineVersion = "$($BuildVersionJson.MajorVersion).$($BuildVersionJson.MinorVersion)"

    # Pick UAT script by host OS
    $UATScript = if ($IsWin) {
        Join-Path $EnginePath 'Engine/Build/BatchFiles/RunUAT.bat'
    } else {
        Join-Path $EnginePath 'Engine/Build/BatchFiles/RunUAT.sh'
    }
    if (-not (Test-Path $UATScript)) { throw "Can't find $UATScript" }
}

$PlatformsJoined  = $TargetPlatforms -join '+'
$PlatformsForName = ($TargetPlatforms | Sort-Object) -join '_'

function Write-Step([string]$Msg) { Write-Host "==> $Msg" -ForegroundColor Cyan }

function Update-ModulesBuildId {
    param([string]$ModulesFile, [string]$NewBuildId)
    $content = Get-Content $ModulesFile -Raw
    $updated = $content -replace '"BuildId"\s*:\s*"[^"]*"', "`"BuildId`": `"$NewBuildId`""
    Set-Content $ModulesFile -Value $updated -NoNewline
}

function Enable-PluginByDefault {
    param([string]$DescriptorPath)
    if (-not (Test-Path $DescriptorPath)) {
        throw "Plugin descriptor not found: $DescriptorPath"
    }

    $json = Get-Content $DescriptorPath -Raw | ConvertFrom-Json
    if ($json.PSObject.Properties.Name -contains 'EnabledByDefault') {
        $json.EnabledByDefault = $true
    } else {
        $json | Add-Member -NotePropertyName EnabledByDefault -NotePropertyValue $true
    }
    $json | ConvertTo-Json -Depth 20 | Set-Content -Path $DescriptorPath -Encoding UTF8
}

function Get-LauncherBuildId {
    param([string]$LauncherPath)
    if (-not (Test-Path $LauncherPath)) {
        throw "Launcher engine path does not exist: $LauncherPath"
    }
    # BuildId is identical across platform subdirs of a given engine install,
    # so probe whichever platform's manifest exists.
    $platforms = @('Mac', 'Win64', 'Linux')
    foreach ($plat in $platforms) {
        $modulesFile = Join-Path $LauncherPath "Engine/Binaries/$plat/UnrealEditor.modules"
        if (Test-Path $modulesFile) {
            $json = Get-Content $modulesFile -Raw | ConvertFrom-Json
            if ($json.BuildId) {
                Write-Host "  launcher BuildId: $($json.BuildId)" -ForegroundColor Cyan
                Write-Host "                    (from $modulesFile)" -ForegroundColor DarkGray
                return $json.BuildId
            }
        }
    }
    throw "No UnrealEditor.modules with a non-empty BuildId under $LauncherPath/Engine/Binaries/{Mac,Win64,Linux}"
}

function New-ZipFromStaging {
    # Compress-Archive in PowerShell 5.1 writes entry names with backslashes,
    # violating ZIP spec 4.4.17 — extracts as one literal filename on macOS/Linux.
    # Use System.IO.Compression directly and force forward slashes.
    param([string]$StagingDir, [string]$ZipPath)
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    Add-Type -AssemblyName System.IO.Compression -ErrorAction SilentlyContinue
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
    $base = (Resolve-Path -LiteralPath $StagingDir).Path
    $sep  = [IO.Path]::DirectorySeparatorChar
    $zip  = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        Get-ChildItem -LiteralPath $StagingDir -Recurse -File -Force | ForEach-Object {
            $rel = $_.FullName.Substring($base.Length).TrimStart($sep) -replace '\\', '/'
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $rel) | Out-Null
        }
    } finally {
        $zip.Dispose()
    }
}

# Replace each module's source with a Build.cs stub that links the shipped
# binary. Removing Source/ outright is not an option: UnrealBuildTool needs a
# Build.cs for every module named in KismetAI.uplugin whenever a consumer
# rebuilds their own C++ target, and fails with a RulesError without one.
# bUsePrecompiled tells it there is nothing to compile here. Dependency names
# are read from the real Build.cs so the module graph stays correct.
function Convert-SourceToPrecompiledStubs {
    param([string]$SourceRoot)
    $moduleDirs = @(Get-ChildItem -LiteralPath $SourceRoot -Directory -Force)
    if ($moduleDirs.Count -eq 0) { throw "No module directories under $SourceRoot - BuildPlugin layout changed?" }
    foreach ($moduleDir in $moduleDirs) {
        $module  = $moduleDir.Name
        $buildCs = Join-Path $moduleDir.FullName "$module.Build.cs"
        if (-not (Test-Path -LiteralPath $buildCs)) { throw "Expected $buildCs for module $module" }
        $rules = Get-Content -LiteralPath $buildCs -Raw

        $deps = @{ Public = @(); Private = @() }
        foreach ($kind in 'Public', 'Private') {
            $pattern = $kind + 'DependencyModuleNames\.AddRange\(\s*new\s+string\[\]\s*\{([^}]*)\}'
            foreach ($m in [regex]::Matches($rules, $pattern)) {
                foreach ($n in [regex]::Matches($m.Groups[1].Value, '"([^"]+)"')) {
                    $deps[$kind] += $n.Groups[1].Value
                }
            }
        }
        if ($deps.Public.Count -eq 0) { throw "Could not read dependencies from $buildCs" }

        Get-ChildItem -LiteralPath $moduleDir.FullName -Force |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }

        $list = { param($names) ($names | ForEach-Object { "            `"$_`"" }) -join ",`n" }
        $stub = @"
// Copyright (c) 2026 Artem Tarasov. Licensed under the Apache License, Version 2.0.
//
// RELEASE STUB - generated by package.ps1, not the real build rules.
//
// The $module module ships precompiled in Binaries/. This file exists only so
// UnrealBuildTool can resolve the module named in KismetAI.uplugin and link the
// prebuilt binary instead of compiling it. The complete source is in the
// "with source" archive of this release and in the source repository.

using UnrealBuildTool;

public class $module : ModuleRules
{
    public $module(ReadOnlyTargetRules Target) : base(Target)
    {
        bUsePrecompiled = true;
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
$(& $list $deps.Public)
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
$(& $list $deps.Private)
        });
    }
}
"@
        Set-Content -LiteralPath $buildCs -Value $stub -NoNewline
    }
}

# Run UAT BuildPlugin into $Staging with the Config files embedded.
function Invoke-UatBuildPlugin {
    param([string]$Staging)
    $uatArgs = @(
        'BuildPlugin',
        "-Plugin=$UPluginPath",
        "-Package=$Staging",
        "-TargetPlatforms=$PlatformsJoined",
        '-Unversioned'
    )

    # Apple Silicon only: BuildPlugin's Mac default is a universal arm64+x64
    # binary; we don't ship Intel Mac builds.
    if ($TargetPlatforms -contains 'Mac') {
        $uatArgs += '-Architecture_Mac=arm64'
    }
    Write-Host "    $UATScript $($uatArgs -join ' ')"

    # Force KismetAI.Build.cs to embed Config/ files into the DLL. UAT BuildPlugin
    # compiles at Development config, so without this override ShouldEmbed() returns
    # false and the zip would need Config/ on disk at runtime.
    $prevEmbed = $env:KISMETAI_EMBED_CONFIG
    $env:KISMETAI_EMBED_CONFIG = '1'

    # The .embed-config marker file (when present) outranks the env var in
    # ShouldEmbed(), so a developer-pinned "0" would silently produce a release
    # zip without baked configs — Config/ isn't shipped on disk either (UAT
    # BuildPlugin doesn't include it by default), so ui.html etc. would be
    # unreachable at runtime. Temporarily rewrite the marker to "1" for the
    # duration of this build, restore the original (or absence) on exit.
    $MarkerPath = Join-Path $PluginRoot '.embed-config'
    $prevMarkerExists = Test-Path $MarkerPath
    $prevMarkerContent = if ($prevMarkerExists) { Get-Content $MarkerPath -Raw } else { $null }
    Set-Content -LiteralPath $MarkerPath -Value '1' -NoNewline

    # embed_config.py needs a real Python. Windows often has only the Microsoft Store
    # stub on PATH; prepend UE's bundled Python so Build.cs can spawn it.
    $prevPath = $env:PATH
    $uePythonDir = $null
    if ($IsWin) {
        $uePythonDir = Join-Path $EnginePath 'Engine/Binaries/ThirdParty/Python3/Win64'
        if (Test-Path (Join-Path $uePythonDir 'python.exe')) {
            $env:PATH = "$uePythonDir;$env:PATH"
        } else {
            Write-Warning "UE python.exe not found at $uePythonDir - embed_config.py will depend on system Python."
        }
    }

    try {
        & $UATScript @uatArgs
        if ($LASTEXITCODE -ne 0) { throw "UAT BuildPlugin failed ($LASTEXITCODE)" }
    } finally {
        if ($null -eq $prevEmbed) {
            Remove-Item Env:\KISMETAI_EMBED_CONFIG -ErrorAction SilentlyContinue
        } else {
            $env:KISMETAI_EMBED_CONFIG = $prevEmbed
        }
        $env:PATH = $prevPath
        if ($prevMarkerExists) {
            Set-Content -LiteralPath $MarkerPath -Value $prevMarkerContent -NoNewline
        } else {
            Remove-Item -LiteralPath $MarkerPath -Force -ErrorAction SilentlyContinue
        }
    }
}

function Build-Release {
    $Staging = Join-Path $OutputDir '.staging-release'
    if (Test-Path $Staging) { Remove-Item $Staging -Recurse -Force }

    if ($PrebuiltPackageDir) {
        Write-Step "Packaging RELEASE variant from prebuilt $PrebuiltPackageDir"
        Copy-Item -LiteralPath $PrebuiltPackageDir -Destination $Staging -Recurse -Force
    } else {
        Write-Step "Building RELEASE variant via UAT BuildPlugin (clean rebuild)"
        Invoke-UatBuildPlugin -Staging $Staging
    }

    # Keep the complete buildable source and editable configuration in releases.
    # BuildPlugin filters auxiliary files, so copy these explicitly.
    foreach ($dir in 'Source', 'Config', 'Scripts', 'ThirdPartyNotices') {
        $src = Join-Path $PluginRoot $dir
        if (Test-Path $src) {
            $dst = Join-Path $Staging $dir
            if (Test-Path $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
            Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
        }
    }
    foreach ($dir in 'Intermediate', 'Docs') {
        $p = Join-Path $Staging $dir
        if (Test-Path $p) { Remove-Item -LiteralPath $p -Recurse -Force }
    }
    Get-ChildItem -LiteralPath $Staging -Recurse -File -Filter '*.pdb' -Force |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
    Get-ChildItem -LiteralPath $Staging -Recurse -Directory -Filter '__pycache__' -Force |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
    # Operating-system folder metadata (Finder, Explorer) never belongs in an archive.
    Get-ChildItem -LiteralPath $Staging -Recurse -File -Force |
        Where-Object { $_.Name -in '.DS_Store', 'Thumbs.db', 'desktop.ini' } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }

    # Patch .modules BuildId
    if (-not $KeepBuildId) {
        Get-ChildItem -LiteralPath $Staging -Recurse -Filter '*.modules' -Force |
            ForEach-Object { Update-ModulesBuildId -ModulesFile $_.FullName -NewBuildId $ReleaseBuildId }
    }

    # Published zips are also consumed by the self-updater. The updater replaces
    # the installed .uplugin with this staged descriptor, so the zip itself must
    # carry EnabledByDefault=true instead of relying on install-local/install-win.
    Enable-PluginByDefault -DescriptorPath (Join-Path $Staging 'KismetAI.uplugin')

    # Wrap the plugin contents under a top-level KismetAI/ folder so extraction
    # produces a drop-in directory for <Project>/Plugins/.
    $Wrapper = Join-Path $Staging 'KismetAI'
    New-Item -ItemType Directory -Path $Wrapper -Force | Out-Null
    Get-ChildItem -LiteralPath $Staging -Force |
        Where-Object { $_.FullName -ne $Wrapper } |
        ForEach-Object { Move-Item -LiteralPath $_.FullName -Destination $Wrapper -Force }
    # Install + legal docs ship inside the plugin folder so they follow it when
    # the user moves KismetAI/ into <Project>/Plugins/.
    # The contributor docs live at the repository root, one level above the
    # plugin folder. Ask git for it, since the plugin folder may be reached
    # through a link whose parent is somewhere else.
    $RepoDocsRoot = $null
    try { $RepoDocsRoot = (& git -C $PluginRoot rev-parse --show-toplevel 2>$null) } catch { }
    if (-not $RepoDocsRoot) { $RepoDocsRoot = Split-Path $PluginRoot -Parent }
    $WrapperDocs = @('README.md', 'INSTALL.md', 'LICENSE', 'SERVICE_TERMS.md', 'NOTICE.md', 'CONTRIBUTING.md', 'MAINTAINING.md', 'SECURITY.md')
    foreach ($doc in $WrapperDocs) {
        $src = Join-Path $PluginRoot $doc
        if (-not (Test-Path $src)) { $src = Join-Path $RepoDocsRoot $doc }
        if (Test-Path $src) {
            Copy-Item -LiteralPath $src -Destination $Wrapper -Force
        } else {
            Write-Warning "$doc not found at $src - archive will ship without it."
        }
    }

    $BaseName = "KismetAI-$PluginVersion-UE$EngineVersion-$PlatformsForName-release"

    # 1. With source: the staged tree as it stands.
    $SrcZipPath = Join-Path $OutputDir "$BaseName-src.zip"
    New-ZipFromStaging -StagingDir $Staging -ZipPath $SrcZipPath

    # 2. Default archive: the same binaries without the source. Scripts/ only
    #    serves source builds. Config/ and ThirdPartyNotices/ stay: the config is
    #    user-editable and the notices must accompany the binaries.
    Convert-SourceToPrecompiledStubs -SourceRoot (Join-Path $Wrapper 'Source')
    $StagedScripts = Join-Path $Wrapper 'Scripts'
    if (Test-Path $StagedScripts) { Remove-Item -LiteralPath $StagedScripts -Recurse -Force }
    $ZipPath = Join-Path $OutputDir "$BaseName.zip"
    New-ZipFromStaging -StagingDir $Staging -ZipPath $ZipPath
    Remove-Item $Staging -Recurse -Force

    foreach ($p in $ZipPath, $SrcZipPath) {
        $sizeMB = [math]::Round((Get-Item $p).Length / 1MB, 2)
        Write-Host "    -> $p ($sizeMB MB)" -ForegroundColor Green
    }
}

function Build-Dev {
    Write-Step "Packaging DEV variant (full snapshot of current plugin tree)"

    # Confirm at least one requested platform has a binary
    $BinDir = Join-Path $PluginRoot 'Binaries'
    $HasAnyBinary = $false
    foreach ($p in $TargetPlatforms) {
        $pf = Join-Path $BinDir $p
        if (Test-Path $pf) {
            $libs = Get-ChildItem $pf -File -ErrorAction SilentlyContinue |
                Where-Object { $_.Extension -in '.dll', '.so', '.dylib' }
            if ($libs) { $HasAnyBinary = $true }
        }
    }
    if (-not $HasAnyBinary) {
        throw "No binaries found in $BinDir for platforms [$PlatformsJoined]. Build the editor (or the plugin via UAT) first."
    }

    $Staging = Join-Path $OutputDir '.staging-dev'
    if (Test-Path $Staging) { Remove-Item $Staging -Recurse -Force }
    New-Item -ItemType Directory -Path $Staging -Force | Out-Null

    Copy-Item $UPluginPath $Staging
    Enable-PluginByDefault -DescriptorPath (Join-Path $Staging 'KismetAI.uplugin')
    foreach ($dir in 'Binaries', 'Config', 'Source', 'Docs', 'Resources', 'Intermediate') {
        $src = Join-Path $PluginRoot $dir
        if (Test-Path $src) {
            Copy-Item $src $Staging -Recurse -Force
        }
    }

    if ($PSBoundParameters.ContainsKey('DevBuildId')) {
        Get-ChildItem -LiteralPath $Staging -Recurse -Filter '*.modules' -Force |
            ForEach-Object { Update-ModulesBuildId -ModulesFile $_.FullName -NewBuildId $DevBuildId }
    }

    $ZipName = "KismetAI-$PluginVersion-UE$EngineVersion-$PlatformsForName-dev.zip"
    $ZipPath = Join-Path $OutputDir $ZipName
    New-ZipFromStaging -StagingDir $Staging -ZipPath $ZipPath
    Remove-Item $Staging -Recurse -Force

    $sizeMB = [math]::Round((Get-Item $ZipPath).Length / 1MB, 2)
    Write-Host "    -> $ZipPath ($sizeMB MB)" -ForegroundColor Green
}

Write-Host ""
Write-Host "KismetAI package" -ForegroundColor White
Write-Host "  plugin version : $PluginVersion"
Write-Host "  engine         : UE$EngineVersion   ($EnginePath)"
Write-Host "  platforms      : $PlatformsJoined"
Write-Host "  variant        : $Variant"
Write-Host "  output         : $OutputDir"

# Resolve the BuildId stamped into release zips. Explicit -ReleaseBuildId wins;
# otherwise pull it from -LauncherEnginePath. Warn when nothing is set, since
# the resulting zip will not load on any real engine.
if ($Variant -in 'release', 'both') {
    if (-not $PSBoundParameters.ContainsKey('ReleaseBuildId') -and $LauncherEnginePath) {
        $ReleaseBuildId = Get-LauncherBuildId -LauncherPath $LauncherEnginePath
    }
    if ($KeepBuildId) {
        Write-Host "  release BuildId: kept from the build" -ForegroundColor Cyan
    } elseif ([string]::IsNullOrEmpty($ReleaseBuildId)) {
        Write-Warning "No -ReleaseBuildId or -LauncherEnginePath given. Release zip will ship with empty BuildId; UE will refuse to load it (auto-update on end-user machines will leave the editor unable to load the plugin). Pass -LauncherEnginePath '/Users/Shared/Epic Games/UE_$EngineVersion' or -ReleaseBuildId <guid>."
    } else {
        Write-Host "  release BuildId: $ReleaseBuildId" -ForegroundColor Cyan
    }
}
Write-Host ""

if ($Variant -in 'dev', 'both')     { Build-Dev }
if ($Variant -in 'release', 'both') { Build-Release }

# Regenerate manifest.json from whatever release zips currently sit in OutputDir.
# Picks the highest semver as `latest`. Skipped if no release was built this run
# AND no release zips already exist in OutputDir.
function Update-Manifest {
    if ($SkipManifest) { return }

    $releaseZips = Get-ChildItem -LiteralPath $OutputDir -Filter '*-release.zip' -File -ErrorAction SilentlyContinue
    # Get-ChildItem returns a bare FileInfo (not an array) when exactly one zip
    # matches; @(...) forces array semantics so .Count works under StrictMode.
    $releaseZips = @($releaseZips)
    if ($releaseZips.Count -eq 0) {
        Write-Step "No release zips in $OutputDir - skipping manifest"
        return
    }

    Write-Step "Regenerating manifest.json from release zips in $OutputDir"

    if ($ReleaseNotesFile -and (Test-Path $ReleaseNotesFile)) {
        $Notes = Get-Content $ReleaseNotesFile -Raw
    } elseif ($ReleaseNotes) {
        $Notes = $ReleaseNotes
    } else {
        $Notes = ''
    }

    $BaseUrl = $ManifestBaseURL
    if (-not $BaseUrl) { $BaseUrl = 'https://YOUR-DOMAIN.example/kismetai/releases/' }
    if (-not $BaseUrl.EndsWith('/')) { $BaseUrl = $BaseUrl + '/' }

    # Filename pattern: KismetAI-<version>-UE<engine>-<platform>-release.zip
    $pattern = '^KismetAI-(?<ver>[\d.]+)-UE(?<eng>[\d.]+)-(?<plat>[A-Za-z0-9_]+)-release\.zip$'
    $entries = @()
    foreach ($zip in $releaseZips) {
        if ($zip.Name -notmatch $pattern) {
            Write-Warning "Skipping unrecognized zip name: $($zip.Name)"
            continue
        }
        $entries += [pscustomobject]@{
            Version  = $Matches['ver']
            Engine   = $Matches['eng']
            Platform = $Matches['plat']
            Url      = $BaseUrl + $zip.Name
            Size     = $zip.Length
        }
    }
    if ($entries.Count -eq 0) {
        Write-Warning "No zips matched the expected naming pattern; manifest not written"
        return
    }

    # Pick highest semver as `latest`.
    $latestVersion = ($entries | Sort-Object {
        $parts = $_.Version.Split('.') | ForEach-Object { [int]$_ }
        while ($parts.Count -lt 3) { $parts += 0 }
        ($parts[0] * 1000000) + ($parts[1] * 1000) + $parts[2]
    } -Descending | Select-Object -First 1).Version

    $latestAssets = @($entries | Where-Object { $_.Version -eq $latestVersion } | ForEach-Object {
        @{
            engine   = $_.Engine
            platform = $_.Platform
            url      = $_.Url
            size     = $_.Size
        }
    })

    $manifest = @{
        latest = @{
            version = $latestVersion
            notes   = $Notes
            assets  = $latestAssets
        }
    }

    $manifestPath = Join-Path $OutputDir 'manifest.json'
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Write-Host "    -> $manifestPath (latest: $latestVersion, $($latestAssets.Count) asset(s))" -ForegroundColor Green
    Write-Host "    Upload manifest.json + the zips to $BaseUrl" -ForegroundColor Cyan
}

if ($Variant -in 'release', 'both') { Update-Manifest }

Write-Step "Done"
