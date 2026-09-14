param(
    [switch]$SkipPublish
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $projectRoot 'native\MotionWallpaper.Native.sln'
$outputDirectory = Join-Path $projectRoot 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$versionFile = Join-Path $projectRoot 'VERSION'
$releaseMetadataScript = Join-Path $PSScriptRoot 'release-metadata.ps1'
$versionResourceFile = Join-Path $projectRoot 'native\MotionWallpaper.Version.rcinc'
# VS 18.9 NuGet solution restore can recursively fan out worker nodes and fail
# without a diagnostic when /m is greater than one. One non-reused node keeps
# local and CI builds deterministic and avoids exhausting high-core systems.
$maximumBuildNodes = 1

function Get-PublishedProcesses {
    param([string[]]$Names)
    $publishedRoot = [IO.Path]::GetFullPath($outputDirectory).TrimEnd('\') + '\'
    Get-Process -Name $Names -ErrorAction SilentlyContinue | Where-Object {
        try {
            $processPath = [IO.Path]::GetFullPath($_.Path)
            $processPath.StartsWith($publishedRoot, [StringComparison]::OrdinalIgnoreCase)
        } catch { $false }
    }
}

function Get-PortableRelativePath {
    param(
        [Parameter(Mandatory)][string]$BasePath,
        [Parameter(Mandatory)][string]$TargetPath
    )
    # IO.Path.GetRelativePath is unavailable in Windows PowerShell 5.1.
    # Uri.MakeRelativeUri keeps the build entry point usable from both the
    # inbox shell and PowerShell 7 without maintaining two publish scripts.
    $baseFullPath = [IO.Path]::GetFullPath($BasePath).TrimEnd('\') + '\'
    $targetFullPath = [IO.Path]::GetFullPath($TargetPath)
    $relative = ([Uri]$baseFullPath).MakeRelativeUri([Uri]$targetFullPath).ToString()
    [Uri]::UnescapeDataString($relative).Replace('/', '\')
}

function Get-VcRuntimeDirectory {
    param([Parameter(Mandatory)][string]$VisualStudioPath)

    $redistRoot = Join-Path $VisualStudioPath 'VC\Redist\MSVC'
    $candidates = @(Get-ChildItem -Path (Join-Path $redistRoot '*\x64\Microsoft.VC*.CRT') -Directory -ErrorAction SilentlyContinue |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName 'msvcp140.dll') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'msvcp140_atomic_wait.dll') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'vcruntime140.dll') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'vcruntime140_1.dll') -PathType Leaf)
        })
    $selected = $candidates | Sort-Object {
        $folderVersion = $null
        if ([version]::TryParse($_.Parent.Parent.Name, [ref]$folderVersion)) { $folderVersion } else { [version]'0.0' }
    } -Descending | Select-Object -First 1
    if (-not $selected) {
        throw "The x64 Microsoft Visual C++ runtime was not found below $redistRoot. Install the MSVC v145 x64 build tools and redistributable components."
    }
    $selected.FullName
}

function Assert-PublishedVersionInfo {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$ExpectedFileVersion,
        [Parameter(Mandatory)][string]$ExpectedProductVersion
    )

    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    $numericFileVersion = "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)"
    $numericProductVersion = "$($info.ProductMajorPart).$($info.ProductMinorPart).$($info.ProductBuildPart).$($info.ProductPrivatePart)"
    if ($info.FileVersion -ne $ExpectedFileVersion -or $info.ProductVersion -ne $ExpectedProductVersion -or
        $numericFileVersion -ne $ExpectedFileVersion -or $numericProductVersion -ne $ExpectedFileVersion) {
        throw "Version metadata mismatch in $Path. Expected file/product $ExpectedFileVersion / $ExpectedProductVersion, found $($info.FileVersion) / $($info.ProductVersion)."
    }
}

$publishedProcesses = @(Get-PublishedProcesses @('MotionWallpaper', 'motionwallpaper-agent', 'motionwallpaper-renderer'))
$appWasRunning = [bool]($publishedProcesses | Where-Object ProcessName -EQ 'MotionWallpaper')
$agentWasRunning = [bool]($publishedProcesses | Where-Object ProcessName -EQ 'motionwallpaper-agent')

# Some portable Build Tools layouts leave non-existent optional ATL/VS entries
# in LIB. Roslyn reports those inherited paths as warnings even though this
# solution does not use them.
if ($env:LIB) {
    $env:LIB = (($env:LIB -split ';') | Where-Object { $_ -and (Test-Path -LiteralPath $_) }) -join ';'
}

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio with Desktop development with C++ and WinUI tools is required. vswhere.exe was not found.'
}

. $releaseMetadataScript
$release = Get-MotionWallpaperReleaseMetadata (
    Get-Content -LiteralPath $versionFile -Raw
)
$releaseVersion = $release.Version
$fileVersion = $release.FileVersion
$versionResource = Get-Content -LiteralPath $versionResourceFile -Raw
$resourceFileVersion = [regex]::Match($versionResource, '#define\s+MOTION_FILE_VERSION_TEXT\s+"([^"]+)"')
$resourceProductVersion = [regex]::Match($versionResource, '#define\s+MOTION_PRODUCT_VERSION_TEXT\s+"([^"]+)"')
if (-not $resourceFileVersion.Success -or $resourceFileVersion.Groups[1].Value -ne $fileVersion -or
    -not $resourceProductVersion.Success -or $resourceProductVersion.Groups[1].Value -ne $releaseVersion) {
    throw "VERSION and native\MotionWallpaper.Version.rcinc do not agree. Expected $fileVersion / $releaseVersion."
}

$requiredVisualStudioComponents = @(
    'Microsoft.Component.MSBuild',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    'Microsoft.VisualStudio.Component.VC.Redist.14.Latest'
)
$visualStudioPath = & $vswhere -latest -products * -requires $requiredVisualStudioComponents -property installationPath | Select-Object -First 1
$msbuild = & $vswhere -latest -products * -requires $requiredVisualStudioComponents -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $visualStudioPath -or -not $msbuild) {
    throw 'MSBuild, the x64 C++ toolset, and the Visual C++ redistributable files were not found in one Visual Studio installation. Install the Desktop development with C++ workload and its latest v14 redistributable component.'
}

# Minimal Build Tools layouts can omit the optional ATL and Visual Studio
# library folders while the VC targets still add those non-existent paths to
# LIB. Clear only the missing optional x64 paths so Roslyn inline build tasks do
# not emit CS1668; a complete installation keeps its normal search paths.
$msbuildLibraryOverrides = @()
$vcToolsDirectory = Get-ChildItem -Path (Join-Path $visualStudioPath 'VC\Tools\MSVC\*') -Directory -ErrorAction SilentlyContinue |
    Sort-Object {
        $folderVersion = $null
        if ([version]::TryParse($_.Name, [ref]$folderVersion)) { $folderVersion } else { [version]'0.0' }
    } -Descending | Select-Object -First 1
if ($vcToolsDirectory -and -not (Test-Path -LiteralPath (Join-Path $vcToolsDirectory.FullName 'atlmfc\lib\x64'))) {
    $msbuildLibraryOverrides += '-p:VC_LibraryPath_ATL_x64='
}
if (-not (Test-Path -LiteralPath (Join-Path $visualStudioPath 'VC\Auxiliary\VS\lib\x64'))) {
    $msbuildLibraryOverrides += '-p:VC_VS_LibraryPath_VC_VS_x64='
}

& $msbuild $solution -t:Restore "-m:$maximumBuildNodes" -nr:false @msbuildLibraryOverrides
if ($LASTEXITCODE -ne 0) { throw "NuGet restore failed with exit code $LASTEXITCODE" }

& $msbuild $solution -t:Build -p:Configuration=Release -p:Platform=x64 "-m:$maximumBuildNodes" -nr:false @msbuildLibraryOverrides
if ($LASTEXITCODE -ne 0) { throw "Native build failed with exit code $LASTEXITCODE" }

$testExecutable = Join-Path $projectRoot 'native\x64\Release\MotionWallpaper.Tests\MotionWallpaper.Tests.exe'
& $testExecutable
if ($LASTEXITCODE -ne 0) { throw "Native tests failed with exit code $LASTEXITCODE" }

$nativeOutput = Join-Path $projectRoot 'native\x64\Release\MotionWallpaper.App'
$nativeExecutable = Join-Path $nativeOutput 'MotionWallpaper.exe'
if (-not (Test-Path -LiteralPath $nativeExecutable)) {
    throw "Native executable was not found at $nativeExecutable"
}
foreach ($versionedExecutable in @('MotionWallpaper.exe', 'motionwallpaper-agent.exe', 'motionwallpaper-renderer.exe')) {
    Assert-PublishedVersionInfo (Join-Path $nativeOutput $versionedExecutable) $fileVersion $releaseVersion
}

if ($SkipPublish) {
    Write-Host 'Native build and tests completed successfully; publish was skipped.'
    return
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
@(Get-PublishedProcesses @('MotionWallpaper', 'motionwallpaper-agent', 'motionwallpaper-renderer')) |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
$preservedRoots = @('Wallpapers', 'Config')
$developmentOnlyExtensions = @(
    '.exp',
    '.iobj',
    '.ipdb',
    '.lib',
    '.pdb'
)
$payloadFiles = @(Get-ChildItem -LiteralPath $nativeOutput -Recurse -File | Where-Object {
    $relativePath = Get-PortableRelativePath $nativeOutput $_.FullName
    $topLevel = ($relativePath -split '[\\/]', 2)[0]
    $extension = $_.Extension.ToLowerInvariant()
    $topLevel -notin $preservedRoots -and $extension -notin $developmentOnlyExtensions
})
$payloadPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$payloadFiles | ForEach-Object {
    $relativePath = Get-PortableRelativePath $nativeOutput $_.FullName
    [void]$payloadPaths.Add($relativePath)
    $destination = Join-Path $outputDirectory $relativePath
    $existing = Get-Item -LiteralPath $destination -ErrorAction SilentlyContinue
    if (-not $existing -or $existing.Length -ne $_.Length -or $existing.LastWriteTimeUtc -ne $_.LastWriteTimeUtc) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
    }
}

# Mirror only application payload files. User media and settings are preserved.
Get-ChildItem -LiteralPath $outputDirectory -Recurse -File | ForEach-Object {
    $relativePath = Get-PortableRelativePath $outputDirectory $_.FullName
    $topLevel = ($relativePath -split '[\\/]', 2)[0]
    if ($topLevel -notin $preservedRoots -and -not $payloadPaths.Contains($relativePath)) {
        Remove-Item -LiteralPath $_.FullName -Force
    }
}
Get-ChildItem -LiteralPath $outputDirectory -Recurse -Directory |
    Sort-Object { $_.FullName.Length } -Descending |
    ForEach-Object {
        $relativePath = Get-PortableRelativePath $outputDirectory $_.FullName
        $topLevel = ($relativePath -split '[\\/]', 2)[0]
        if ($topLevel -notin $preservedRoots -and -not (Get-ChildItem -LiteralPath $_.FullName -Force)) {
            Remove-Item -LiteralPath $_.FullName -Force
        }
    }

# Windows App SDK self-contained output includes satellite resources for every
# translated WinUI language. MotionWallpaper currently supports Simplified
# Chinese with English fallback, so keep only those two resource directories.
$supportedResourceLanguages = @('zh-CN', 'en-us')
Get-ChildItem -LiteralPath $outputDirectory -Directory | ForEach-Object {
    $resourceFiles = @(Get-ChildItem -LiteralPath $_.FullName -Recurse -File)
    $isSatelliteLanguageDirectory = $resourceFiles.Count -gt 0 -and
        @($resourceFiles | Where-Object { $_.Extension -ine '.mui' }).Count -eq 0
    if ($isSatelliteLanguageDirectory -and $_.Name -notin $supportedResourceLanguages) {
        Remove-Item -LiteralPath $_.FullName -Recurse -Force
    }
}

# Both the ZIP and installer default to one-directory storage. The installer
# migrates legacy LocalAppData content; uninstall never follows an external
# media-library path outside the application directory.
[IO.File]::WriteAllText((Join-Path $outputDirectory 'portable.mode'), "portable`r`n", [Text.UTF8Encoding]::new($false))

# Keep the optional optimization backend inside the installed application.
# The pinned archive stays in the workspace cache; each verified extraction is
# isolated in a fresh temporary directory and never trusted across builds.
& (Join-Path $PSScriptRoot 'prepare-ffmpeg.ps1') -Destination (Join-Path $outputDirectory 'Tools\ffmpeg')

# Ship the project and third-party terms with every locally published payload.
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination (Join-Path $outputDirectory 'LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $outputDirectory 'THIRD_PARTY_NOTICES.md') -Force

# Normalize the user-facing executable name for Explorer.
$publishedApp = Get-ChildItem -LiteralPath $outputDirectory -File | Where-Object { $_.Name -ieq 'MotionWallpaper.exe' } | Select-Object -First 1
if ($publishedApp.Name -cne 'MotionWallpaper.exe') {
    $caseTemporary = Join-Path $outputDirectory 'MotionWallpaper.casefix.exe'
    Move-Item -LiteralPath $publishedApp.FullName -Destination $caseTemporary -Force
    Move-Item -LiteralPath $caseTemporary -Destination (Join-Path $outputDirectory 'MotionWallpaper.exe') -Force
}

# Release builds use the dynamic MSVC runtime. Deploy the official x64 CRT
# beside the executables so installation and portable use do not depend on a
# machine-wide redistributable or administrative privileges.
$vcRuntimeDirectory = Get-VcRuntimeDirectory $visualStudioPath
$vcRuntimeFiles = @(Get-ChildItem -LiteralPath $vcRuntimeDirectory -File -Filter '*.dll')
foreach ($runtimeFile in $vcRuntimeFiles) {
    Copy-Item -LiteralPath $runtimeFile.FullName -Destination (Join-Path $outputDirectory $runtimeFile.Name) -Force
}
foreach ($requiredRuntime in @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $outputDirectory $requiredRuntime) -PathType Leaf)) {
        throw "Published payload is missing the app-local Visual C++ runtime: $requiredRuntime"
    }
}

foreach ($versionedExecutable in @('MotionWallpaper.exe', 'motionwallpaper-agent.exe', 'motionwallpaper-renderer.exe')) {
    Assert-PublishedVersionInfo (Join-Path $outputDirectory $versionedExecutable) $fileVersion $releaseVersion
}

if ($appWasRunning) {
    Start-Process -FilePath (Join-Path $outputDirectory 'MotionWallpaper.exe')
} elseif ($agentWasRunning) {
    Start-Process -FilePath (Join-Path $outputDirectory 'motionwallpaper-agent.exe') -WindowStyle Hidden
}

Write-Host "Built native MotionWallpaper into $outputDirectory"
