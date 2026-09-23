[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'release-metadata.ps1')
$release = Get-MotionWallpaperReleaseMetadata (
    Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw
)
$version = $release.Version
$fileVersion = $release.FileVersion
$build = Join-Path $root 'build'
$artifacts = Join-Path $root 'artifacts'
$script = Join-Path $root 'installer\MotionWallpaper.iss'
$compiler = Join-Path $root '.tools\InnoSetup\ISCC.exe'
$installer = Join-Path $artifacts $release.InstallerFileName
$checksum = "$installer.sha256"

function Remove-InstallerArtifacts {
    foreach ($path in @($installer, $checksum)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force -ErrorAction Stop
        }
    }
}

New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
Remove-InstallerArtifacts

function Get-AssociatedIconHash([string]$Path) {
    $icon = [Drawing.Icon]::ExtractAssociatedIcon($Path)
    if ($null -eq $icon) { throw "无法读取程序图标：$Path" }
    $bitmap = $icon.ToBitmap()
    $stream = [IO.MemoryStream]::new()
    try {
        $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha256.ComputeHash($stream.ToArray()))).Replace('-', '')
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
        $bitmap.Dispose()
        $icon.Dispose()
    }
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-native.ps1')
    if ($LASTEXITCODE -ne 0) { throw "原生发布构建失败，退出码：$LASTEXITCODE" }
}

$required = @(
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
    'Tools\ffmpeg\ffmpeg.exe',
    'Tools\ffmpeg\ffprobe.exe',
    'Tools\ffmpeg\avformat-62.dll',
    'Tools\ffmpeg\avcodec-62.dll',
    'Tools\ffmpeg\avutil-60.dll',
    'Tools\ffmpeg\swscale-9.dll',
    'Tools\ffmpeg\FFmpeg-NOTICE.txt',
    'Tools\ffmpeg\LICENSE-FFmpeg.txt',
    'Tools\ffmpeg\LICENSE-OpenH264.txt'
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $name) -PathType Leaf)) {
        throw "安装负载缺少必需文件：$name"
    }
}
foreach ($name in @('MotionWallpaper.exe', 'motionwallpaper-agent.exe', 'motionwallpaper-renderer.exe')) {
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $build $name))
    $numericVersion = "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)"
    if ($info.FileVersion -ne $fileVersion -or $info.ProductVersion -ne $version -or $numericVersion -ne $fileVersion) {
        throw "程序版本资源不一致：$name（$($info.FileVersion) / $($info.ProductVersion)）"
    }
}
$appIconHash = Get-AssociatedIconHash (Join-Path $build 'MotionWallpaper.exe')
$agentIconHash = Get-AssociatedIconHash (Join-Path $build 'motionwallpaper-agent.exe')
if ($appIconHash -ne $agentIconHash) {
    throw '常驻 Agent 没有使用与主程序相同的托盘图标资源。'
}

$allowedLanguages = @('zh-CN', 'en-us')
$unexpectedLanguages = @(Get-ChildItem -LiteralPath $build -Directory | Where-Object {
    $files = @(Get-ChildItem -LiteralPath $_.FullName -Recurse -File -ErrorAction SilentlyContinue)
    $files.Count -gt 0 -and @($files | Where-Object { $_.Extension -ne '.mui' }).Count -eq 0 -and
        $_.Name -notin $allowedLanguages
})
if ($unexpectedLanguages) {
    throw "安装负载包含多余语言目录：$($unexpectedLanguages.Name -join ', ')"
}

if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'install-inno-tool.ps1')
}
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw '未找到 Inno Setup 命令行编译器。'
}

$artifactReady = $false
try {
    & $compiler "/DMyAppVersion=$version" "/DMyAppFileVersion=$fileVersion" `
        "/DMyInstallerBaseName=$($release.InstallerBaseName)" $script
    if ($LASTEXITCODE -ne 0) {
        throw "安装器编译失败，退出码：$LASTEXITCODE"
    }

    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
        throw "安装器未生成：$installer"
    }
    $installerInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($installer)
    $installerFileVersion = $installerInfo.FileVersion.Trim()
    $installerProductVersion = $installerInfo.ProductVersion.Trim()
    $installerNumericVersion = "$($installerInfo.FileMajorPart).$($installerInfo.FileMinorPart).$($installerInfo.FileBuildPart).$($installerInfo.FilePrivatePart)"
    if ($installerFileVersion -ne $fileVersion -or $installerProductVersion -ne $version -or
        $installerNumericVersion -ne $fileVersion) {
        throw "安装器版本资源不一致：$installerFileVersion / $installerProductVersion"
    }

    $hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        $checksum,
        "$hash  $([IO.Path]::GetFileName($installer))`r`n",
        [Text.UTF8Encoding]::new($false)
    )
    $artifactReady = $true
} finally {
    if (-not $artifactReady) {
        Remove-InstallerArtifacts
    }
}

Write-Host "安装器已生成：$installer"
Write-Host "SHA-256：$hash"
