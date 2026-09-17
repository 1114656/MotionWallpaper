param(
    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $projectRoot
$cacheRoot = Join-Path $workspaceRoot 'Codex\MotionWallpaper-ffmpeg'
$assetName = 'ffmpeg-n8.1.2-53-g1005b294ff-win64-lgpl-shared-8.1.zip'
$headers = @{ 'User-Agent' = 'MotionWallpaper-build' }
$expectedHash = 'a654407793b1caef118550de3b99e46299dcabc6649ccf9a3a325f41ff4ea414'
$downloadUrl = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-16-19-44/' + $assetName
$notice = Join-Path $projectRoot 'third_party\FFmpeg-NOTICE.txt'
$openH264License = Join-Path $projectRoot 'third_party\OpenH264-LICENSE.txt'
$expectedOpenH264LicenseHash = 'e7e7f1b027867f49b2a4731f2c317fe6572ff66fd0909d1469b0a7a328e8a293'

function Copy-FfmpegPackage {
    param(
        [Parameter(Mandatory)][string]$PackageRoot,
        [Parameter(Mandatory)][string]$OutputDirectory,
        [Parameter(Mandatory)][string]$NoticePath,
        [Parameter(Mandatory)][string]$OpenH264LicensePath,
        [Parameter(Mandatory)][string]$ExpectedOpenH264LicenseHash
    )
    $bin = Join-Path $PackageRoot 'bin'
    $license = Join-Path $PackageRoot 'LICENSE.txt'
    if (-not (Test-Path -LiteralPath (Join-Path $bin 'ffmpeg.exe')) -or -not (Test-Path -LiteralPath $license)) {
        throw 'The FFmpeg package is incomplete.'
    }
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    Get-ChildItem -LiteralPath $bin -File | Where-Object {
        $_.Extension -ieq '.dll' -or $_.Name -ieq 'ffmpeg.exe'
    } | Copy-Item -Destination $OutputDirectory -Force
    Copy-Item -LiteralPath $license -Destination (Join-Path $OutputDirectory 'LICENSE-FFmpeg.txt') -Force
    Copy-Item -LiteralPath $NoticePath -Destination $OutputDirectory -Force
    if (-not (Test-Path -LiteralPath $OpenH264LicensePath -PathType Leaf)) {
        throw "The pinned OpenH264 license text is missing: $OpenH264LicensePath"
    }
    $actualOpenH264LicenseHash = (Get-FileHash -LiteralPath $OpenH264LicensePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualOpenH264LicenseHash -ne $ExpectedOpenH264LicenseHash) {
        throw "The pinned OpenH264 license SHA-256 is $actualOpenH264LicenseHash; expected $ExpectedOpenH264LicenseHash. Ensure the checkout preserves LF line endings."
    }
    Copy-Item -LiteralPath $OpenH264LicensePath -Destination (Join-Path $OutputDirectory 'LICENSE-OpenH264.txt') -Force

    $publishedFfmpeg = Join-Path $OutputDirectory 'ffmpeg.exe'
    $encoders = (& $publishedFfmpeg -hide_banner -encoders 2>&1 | Out-String)
    foreach ($requiredEncoder in @(
        'h264_nvenc', 'h264_qsv', 'h264_amf',
        'hevc_nvenc', 'hevc_qsv', 'hevc_amf', 'libopenh264')) {
        if ($encoders -notmatch [Regex]::Escape($requiredEncoder)) {
            throw "The verified FFmpeg package does not provide required encoder '$requiredEncoder'."
        }
    }
    $filters = (& $publishedFfmpeg -hide_banner -filters 2>&1 | Out-String)
    if ($filters -notmatch 'scale_cuda') {
        throw "The verified FFmpeg package does not provide required filter 'scale_cuda'."
    }
}

New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null
$archive = Join-Path $cacheRoot $assetName
$validArchive = (Test-Path -LiteralPath $archive) -and
    ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expectedHash)
if (-not $validArchive) {
    $download = "$archive.download"
    Invoke-WebRequest -Uri $downloadUrl -OutFile $download -Headers $headers
    $actualHash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        [IO.File]::Delete($download)
        throw 'The downloaded FFmpeg archive failed SHA-256 verification.'
    }
    Move-Item -LiteralPath $download -Destination $archive -Force
}

# A hash-shaped directory is not an integrity proof: it can be stale or edited
# independently of the pinned zip. Re-check the archive on every invocation and
# extract into a fresh private directory before any bundled executable is run.
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw 'The cached FFmpeg archive changed after verification.'
}
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$expanded = Join-Path $temporaryBase ('MotionWallpaper-ffmpeg-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $expanded | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded
    $packages = @(Get-ChildItem -LiteralPath $expanded -Directory)
    if ($packages.Count -ne 1) { throw 'The FFmpeg archive did not contain exactly one package directory.' }
    Copy-FfmpegPackage `
        -PackageRoot $packages[0].FullName `
        -OutputDirectory $Destination `
        -NoticePath $notice `
        -OpenH264LicensePath $openH264License `
        -ExpectedOpenH264LicenseHash $expectedOpenH264LicenseHash
} finally {
    $resolvedExpanded = [IO.Path]::GetFullPath($expanded)
    if ($resolvedExpanded.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedExpanded).StartsWith('MotionWallpaper-ffmpeg-', [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $resolvedExpanded -Recurse -Force -ErrorAction SilentlyContinue
    }
}
