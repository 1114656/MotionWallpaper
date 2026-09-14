[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$toolsRoot = Join-Path $root '.tools'
$innoRoot = Join-Path $toolsRoot 'InnoSetup'
$compiler = Join-Path $innoRoot 'ISCC.exe'
$language = Join-Path $innoRoot 'Languages\ChineseSimplified.isl'
$downloads = Join-Path $toolsRoot 'downloads'

$innoUri = 'https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe'
$innoHash = '9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732'
$languageUri = 'https://raw.githubusercontent.com/jrsoftware/issrc/1ae7bf81dc0d2013235dfe4bb0b6f4e4a0b6b25c/Files/Languages/ChineseSimplified.isl'
$languageHash = 'e0b0b350e2245f3c5e65586dfe43d574f6e7f06f2261149aba284954b3fc9a8d'

function Assert-FileHash([string]$Path, [string]$ExpectedHash) {
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $ExpectedHash) {
        throw "文件校验失败：$Path`n期望：$ExpectedHash`n实际：$actual"
    }
}

if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    New-Item -ItemType Directory -Path $downloads -Force | Out-Null
    $installer = Join-Path $downloads 'innosetup-6.7.3.exe'
    try {
        Invoke-WebRequest -Uri $innoUri -OutFile $installer
        Assert-FileHash $installer $innoHash
        $process = Start-Process -FilePath $installer -ArgumentList @(
            '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/CURRENTUSER', "/DIR=`"$innoRoot`""
        ) -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
            throw "Inno Setup 安装失败，退出码：$($process.ExitCode)"
        }
    } finally {
        Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
        if ((Test-Path -LiteralPath $downloads -PathType Container) -and
            -not (Get-ChildItem -LiteralPath $downloads -Force)) {
            Remove-Item -LiteralPath $downloads -Force
        }
    }
}

New-Item -ItemType Directory -Path (Split-Path -Parent $language) -Force | Out-Null
if (-not (Test-Path -LiteralPath $language -PathType Leaf) -or
    (Get-FileHash -LiteralPath $language -Algorithm SHA256).Hash.ToLowerInvariant() -ne $languageHash) {
    Invoke-WebRequest -Uri $languageUri -OutFile $language
}
Assert-FileHash $language $languageHash

Write-Host "Inno Setup 编译器已就绪：$compiler"
