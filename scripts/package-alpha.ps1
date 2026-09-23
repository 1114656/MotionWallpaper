param(
    [string]$Version,
    [string]$BuildDirectory,
    [string]$ArtifactDirectory
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'release-metadata.ps1')
if (-not $Version) {
    $Version = (Get-Content -LiteralPath (Join-Path $projectRoot 'VERSION') -Raw).Trim()
}
$release = Get-MotionWallpaperReleaseMetadata $Version
$Version = $release.Version

if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $projectRoot 'build'
}
if (-not $ArtifactDirectory) {
    $ArtifactDirectory = Join-Path $projectRoot 'artifacts'
}

$buildRoot = [IO.Path]::GetFullPath($BuildDirectory)
$artifactRoot = [IO.Path]::GetFullPath($ArtifactDirectory)
$entryPoint = Join-Path $buildRoot 'MotionWallpaper.exe'
$archiveName = $release.ArchiveFileName
$archivePath = Join-Path $artifactRoot $archiveName
$checksumPath = "$archivePath.sha256"
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$stagingRoot = Join-Path $temporaryBase ("MotionWallpaper-package-" + [guid]::NewGuid().ToString('N'))
$payloadRoot = Join-Path $stagingRoot $release.ArchiveBaseName
$fileVersion = $release.FileVersion
$artifactReady = $false

New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
foreach ($path in @($archivePath, $checksumPath)) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force -ErrorAction Stop
    }
}

function Get-RelativePath {
    param(
        [Parameter(Mandatory)][string]$BasePath,
        [Parameter(Mandatory)][string]$TargetPath
    )
    $baseFullPath = [IO.Path]::GetFullPath($BasePath).TrimEnd('\') + '\'
    $targetFullPath = [IO.Path]::GetFullPath($TargetPath)
    $relative = ([Uri]$baseFullPath).MakeRelativeUri([Uri]$targetFullPath).ToString()
    [Uri]::UnescapeDataString($relative).Replace('/', '\')
}

try {
    if (-not (Test-Path -LiteralPath $entryPoint -PathType Leaf)) {
        throw "Published application was not found at $entryPoint. Run scripts\build-native.ps1 first."
    }

    foreach ($name in @('MotionWallpaper.exe', 'motionwallpaper-agent.exe', 'motionwallpaper-renderer.exe')) {
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $buildRoot $name))
        $numericVersion = "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)"
        if ($info.FileVersion -ne $fileVersion -or $info.ProductVersion -ne $Version -or $numericVersion -ne $fileVersion) {
            throw "Published executable has inconsistent version metadata: $name ($($info.FileVersion) / $($info.ProductVersion))."
        }
    }

    New-Item -ItemType Directory -Path $payloadRoot -Force | Out-Null
    $excludedRoots = @('Wallpapers', 'Config')
    $excludedExtensions = @('.exp', '.iobj', '.ipdb', '.lib', '.pdb', '.log')

    Get-ChildItem -LiteralPath $buildRoot -Recurse -Force -File | ForEach-Object {
        $relativePath = Get-RelativePath $buildRoot $_.FullName
        $topLevel = ($relativePath -split '[\\/]', 2)[0]
        if ($topLevel -in $excludedRoots -or $_.Extension.ToLowerInvariant() -in $excludedExtensions) {
            return
        }
        $destination = Join-Path $payloadRoot $relativePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
    }

    Compress-Archive -LiteralPath $payloadRoot -DestinationPath $archivePath -CompressionLevel Optimal

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entries = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        $prefix = "$($release.ArchiveBaseName)/"
        $requiredEntries = @(
            'MotionWallpaper.exe',
            'motionwallpaper-agent.exe',
            'motionwallpaper-renderer.exe',
            'msvcp140.dll',
            'msvcp140_atomic_wait.dll',
            'vcruntime140.dll',
            'vcruntime140_1.dll',
            'portable.mode',
            'LICENSE.txt',
            'THIRD_PARTY_NOTICES.md',
            'Tools/ffmpeg/ffmpeg.exe',
            'Tools/ffmpeg/ffprobe.exe',
            'Tools/ffmpeg/FFmpeg-NOTICE.txt',
            'Tools/ffmpeg/avformat-62.dll',
            'Tools/ffmpeg/avcodec-62.dll',
            'Tools/ffmpeg/avutil-60.dll',
            'Tools/ffmpeg/swscale-9.dll',
            'Tools/ffmpeg/LICENSE-FFmpeg.txt',
            'Tools/ffmpeg/LICENSE-OpenH264.txt'
        )
        foreach ($required in $requiredEntries) {
            if (($prefix + $required) -notin $entries) {
                throw "Package is missing required entry: $required"
            }
        }

        $forbidden = @($entries | Where-Object {
            $_ -like ($prefix + 'Wallpapers/*') -or
            $_ -like ($prefix + 'Config/*') -or
            $_ -match '(?i)\.(pdb|lib|exp|log)$' -or
            $_ -match '(^|/)(tmp|output|artifacts)/'
        })
        if ($forbidden.Count -gt 0) {
            throw "Package contains forbidden entries: $($forbidden -join ', ')"
        }

        $supportedResourceLanguages = @('zh-CN', 'en-us')
        $unsupportedLanguageResources = @($entries | Where-Object { $_ -match '(?i)\.mui$' } | ForEach-Object {
            $relative = $_.Substring($prefix.Length)
            ($relative -split '/', 2)[0]
        } | Where-Object { $_ -notin $supportedResourceLanguages } | Select-Object -Unique)
        if ($unsupportedLanguageResources.Count -gt 0) {
            throw "Package contains unsupported language resources: $($unsupportedLanguageResources -join ', ')"
        }
    } finally {
        $archive.Dispose()
    }

    $hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText($checksumPath, "$hash  $archiveName`r`n", [Text.UTF8Encoding]::new($false))
    $artifactReady = $true
    Write-Host "Validated $($release.EditionName) archive: $archivePath"
    Write-Host "SHA-256: $hash"
} finally {
    if (-not $artifactReady) {
        foreach ($path in @($archivePath, $checksumPath)) {
            if (Test-Path -LiteralPath $path) {
                Remove-Item -LiteralPath $path -Force -ErrorAction Stop
            }
        }
    }
    $resolvedStaging = [IO.Path]::GetFullPath($stagingRoot)
    if ($resolvedStaging.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedStaging) -like 'MotionWallpaper-package-*') {
        Remove-Item -LiteralPath $resolvedStaging -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        throw "Refusing to remove unexpected staging path: $resolvedStaging"
    }
}
