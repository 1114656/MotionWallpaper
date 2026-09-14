[CmdletBinding()]
param(
    [string]$InstallerPath
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
if (-not $InstallerPath) {
    $InstallerPath = Join-Path (Join-Path $root 'artifacts') $release.InstallerFileName
}
$InstallerPath = [IO.Path]::GetFullPath($InstallerPath)
if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw "安装器不存在：$InstallerPath"
}

$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$testRoot = Join-Path $temporaryBase ("MotionWallpaper-installer-test-" + [guid]::NewGuid().ToString('N'))
$installRoot = Join-Path $testRoot 'custom install path'
$crashInstallRoot = Join-Path $testRoot 'interrupted migration install path'
$raceInstallRoot = Join-Path $testRoot 'post-preflight race install path'
$retryInstallRoot = Join-Path $testRoot 'fallback retry install path'
$legacyRoot = Join-Path $testRoot 'legacy-data'
$externalMediaRoot = Join-Path $legacyRoot 'external-media-library'
$reparseTargetRoot = Join-Path $testRoot 'reparse-target'
$reparseLink = Join-Path $legacyRoot 'Wallpapers\linked-outside'
$installLog = Join-Path $testRoot 'install.log'
$crashLog = Join-Path $testRoot 'interrupted-migration.log'
$recoveryLog = Join-Path $testRoot 'migration-recovery.log'
$uninstaller = Join-Path $installRoot 'unins000.exe'
$crashUninstaller = Join-Path $crashInstallRoot 'unins000.exe'
$raceUninstaller = Join-Path $raceInstallRoot 'unins000.exe'
$retryUninstaller = Join-Path $retryInstallRoot 'unins000.exe'
$compiler = Join-Path $root '.tools\InnoSetup\ISCC.exe'
$installerScript = Join-Path $root 'installer\MotionWallpaper.iss'
$smokeInstaller = Join-Path $testRoot 'MotionWallpaper-installer-smoke.exe'
$faultSmokeInstaller = Join-Path $testRoot 'MotionWallpaper-installer-fault-smoke.exe'

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

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $legacyRoot 'Config') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Config\settings.json'), '{"version":1}')
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group\group.json'), '{"version":1}')

    & $compiler "/DMyAppVersion=$version" "/DMyAppFileVersion=$fileVersion" '/DMyAppName=MotionWallpaper-Installer-Smoke-Test' `
        '/DMyAppId={{1D39CB35-4E75-46D0-B117-934E57415E50}' `
        "/DLegacyDataRoot=$legacyRoot" '/DInstallerSmokeTest=1' "/O$testRoot" '/FMotionWallpaper-installer-smoke' $installerScript
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $smokeInstaller -PathType Leaf)) {
        throw "隔离测试安装器编译失败，退出码：$LASTEXITCODE"
    }
    & $compiler "/DMyAppVersion=$version" "/DMyAppFileVersion=$fileVersion" '/DMyAppName=MotionWallpaper-Installer-Smoke-Test' `
        '/DMyAppId={{1D39CB35-4E75-46D0-B117-934E57415E50}' `
        "/DLegacyDataRoot=$legacyRoot" '/DInstallerSmokeTest=1' '/DInstallerMigrationFaultAfterConfig=1' `
        "/O$testRoot" '/FMotionWallpaper-installer-fault-smoke' $installerScript
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $faultSmokeInstaller -PathType Leaf)) {
        throw "迁移故障注入安装器编译失败，退出码：$LASTEXITCODE"
    }

    $install = Start-Process -FilePath $smokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=chinesesimp',
        '/NOCLOSEAPPLICATIONS',
        "/DIR=`"$installRoot`"", "/LOG=`"$installLog`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($install.ExitCode -ne 0) {
        $installLogTail = if (Test-Path -LiteralPath $installLog -PathType Leaf) {
            $installLogLines = Get-Content -LiteralPath $installLog
            $failureLine = -1
            for ($lineIndex = 0; $lineIndex -lt $installLogLines.Count; $lineIndex++) {
                if ($installLogLines[$lineIndex] -match 'Fatal exception|Exception message') {
                    $failureLine = $lineIndex
                    break
                }
            }
            if ($failureLine -ge 0) {
                $firstDiagnosticLine = [Math]::Max(0, $failureLine - 30)
                $lastDiagnosticLine = [Math]::Min($installLogLines.Count - 1, $failureLine + 12)
                ($installLogLines[$firstDiagnosticLine..$lastDiagnosticLine]) -join "`n"
            } else {
                ($installLogLines | Select-Object -Last 40) -join "`n"
            }
        } else { '<安装日志不存在>' }
        throw "静默安装失败，退出码：$($install.ExitCode)`n$installLogTail"
    }

    $appRoot = Join-Path $installRoot 'App'
    foreach ($required in @(
        'MotionWallpaper.exe',
        'motionwallpaper-agent.exe',
        'motionwallpaper-renderer.exe',
        'msvcp140.dll',
        'msvcp140_atomic_wait.dll',
        'vcruntime140.dll',
        'vcruntime140_1.dll',
        'Tools\ffmpeg\FFmpeg-NOTICE.txt',
        'Tools\ffmpeg\LICENSE-FFmpeg.txt',
        'Tools\ffmpeg\LICENSE-OpenH264.txt'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $appRoot $required) -PathType Leaf)) {
            throw "安装后缺少必需文件：$required"
        }
    }
    foreach ($name in @('MotionWallpaper.exe', 'motionwallpaper-agent.exe', 'motionwallpaper-renderer.exe')) {
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $appRoot $name))
        $numericVersion = "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)"
        if ($info.FileVersion -ne $fileVersion -or $info.ProductVersion -ne $version -or $numericVersion -ne $fileVersion) {
            throw "安装后的程序版本资源不一致：$name（$($info.FileVersion) / $($info.ProductVersion)）"
        }
    }
    $appIconHash = Get-AssociatedIconHash (Join-Path $appRoot 'MotionWallpaper.exe')
    $agentIconHash = Get-AssociatedIconHash (Join-Path $appRoot 'motionwallpaper-agent.exe')
    if ($appIconHash -ne $agentIconHash) {
        throw '常驻 Agent 没有使用与主程序相同的托盘图标资源。'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $appRoot 'portable.mode') -PathType Leaf)) {
        throw '安装负载缺少单目录数据标记 portable.mode。'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $appRoot 'legacy-migration-complete.mode') -PathType Leaf)) {
        throw '安装器没有记录已经完整校验的旧数据迁移。'
    }
    foreach ($migrated in @('Config\settings.json', 'Wallpapers\Groups\legacy-group\group.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $appRoot $migrated) -PathType Leaf)) {
            throw "安装器没有迁移旧数据：$migrated"
        }
    }
    foreach ($preserved in @('Config\settings.json', 'Wallpapers\Groups\legacy-group\group.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $legacyRoot $preserved) -PathType Leaf)) {
            throw "安装器没有保留可回滚的旧数据：$preserved"
        }
    }

    $muiRoots = @(Get-ChildItem -LiteralPath $appRoot -Recurse -File -Filter '*.mui' | ForEach-Object {
        $_.FullName.Substring($appRoot.Length + 1).Split('\')[0]
    } | Sort-Object -Unique)
    $unexpectedLanguages = @($muiRoots | Where-Object { $_ -notin @('zh-CN', 'en-us') })
    if ($unexpectedLanguages) {
        throw "安装负载包含多余语言：$($unexpectedLanguages -join ', ')"
    }
    if (@($muiRoots | Where-Object { $_ -in @('zh-CN', 'en-us') }).Count -ne 2) {
        throw "安装负载缺少中英文资源：$($muiRoots -join ', ')"
    }

    if (-not (Test-Path -LiteralPath $uninstaller -PathType Leaf)) {
        throw '安装后缺少卸载程序。'
    }
    New-Item -ItemType Directory -Path (Join-Path $appRoot 'Config') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $appRoot 'Wallpapers\Groups\installer-smoke-test') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $externalMediaRoot 'Groups\external-test') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $appRoot 'Config\agent.log'), "smoke test`r`n")
    $externalSettings = [ordered]@{ version = 9; mediaLibraryPath = $externalMediaRoot } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText((Join-Path $appRoot 'Config\settings.json'), $externalSettings)
    [IO.File]::WriteAllText((Join-Path $appRoot 'Wallpapers\Groups\installer-smoke-test\group.json'), '{}')
    [IO.File]::WriteAllText((Join-Path $externalMediaRoot 'Groups\external-test\group.json'), '{"external":true}')
    $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    ) -WindowStyle Hidden -Wait -PassThru
    if ($uninstall.ExitCode -ne 0) { throw "静默卸载失败，退出码：$($uninstall.ExitCode)" }
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $installRoot) {
        throw "卸载后仍残留安装目录：$installRoot"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $externalMediaRoot 'Groups\external-test\group.json') -PathType Leaf)) {
        throw '卸载器越界删除了安装目录外的用户媒体库。'
    }

    # Force a process-level interruption after Config has acquired its final
    # name but before Wallpapers is published. The next ordinary install must
    # see the already durable fallback marker, treat the legacy source as
    # authoritative, archive the partial target, and finish both directories.
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Config\settings.json'), '{"version":1,"beforeCrash":true}')
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group\group.json'), '{"version":1,"beforeCrash":true}')
    $faultInstall = Start-Process -FilePath $faultSmokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=english',
        '/NOCLOSEAPPLICATIONS', "/DIR=`"$crashInstallRoot`"", "/LOG=`"$crashLog`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($faultInstall.ExitCode -ne 197) {
        throw "迁移故障注入没有在预期位置硬退出，退出码：$($faultInstall.ExitCode)"
    }

    $crashAppRoot = Join-Path $crashInstallRoot 'App'
    if (-not (Test-Path -LiteralPath (Join-Path $crashAppRoot 'legacy-data-fallback.mode') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $crashAppRoot 'Config\settings.json') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $crashAppRoot 'Config\.legacy-migration-owner.mode') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $crashAppRoot 'Wallpapers') -PathType Container) -or
        (Test-Path -LiteralPath (Join-Path $crashAppRoot 'legacy-migration-complete.mode') -PathType Leaf)) {
        throw '两次目录发布之间中断后，没有留下 fallback + 仅 Config 的可恢复状态。'
    }
    if ([IO.File]::ReadAllText((Join-Path $crashAppRoot 'Config\settings.json')) -ne
        '{"version":1,"beforeCrash":true}') {
        throw '故障注入后的 Config 目标不是已验证发布的第一份副本。'
    }
    foreach ($preserved in @('Config\settings.json', 'Wallpapers\Groups\legacy-group\group.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $legacyRoot $preserved) -PathType Leaf)) {
            throw "故障注入删除了权威旧数据：$preserved"
        }
    }

    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Config\settings.json'), '{"version":2,"recoveredAfterCrash":true}')
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group\group.json'), '{"version":2,"recoveredAfterCrash":true}')
    $recoveryInstall = Start-Process -FilePath $smokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=english',
        '/NOCLOSEAPPLICATIONS', "/DIR=`"$crashInstallRoot`"", "/LOG=`"$recoveryLog`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($recoveryInstall.ExitCode -ne 0) {
        throw "迁移中断恢复安装失败，退出码：$($recoveryInstall.ExitCode)"
    }
    if ([IO.File]::ReadAllText((Join-Path $crashAppRoot 'Config\settings.json')) -ne
            '{"version":2,"recoveredAfterCrash":true}' -or
        [IO.File]::ReadAllText((Join-Path $crashAppRoot 'Wallpapers\Groups\legacy-group\group.json')) -ne
            '{"version":2,"recoveredAfterCrash":true}') {
        throw '恢复安装没有从 fallback 指定的旧数据根重新发布两个目录。'
    }
    $crashConfigRecovery = @(Get-ChildItem -LiteralPath $crashAppRoot -Directory -Filter 'Config.legacy-recovery-*')
    $crashWallpaperRecovery = @(Get-ChildItem -LiteralPath $crashAppRoot -Directory -Filter 'Wallpapers.legacy-recovery-*')
    if ($crashConfigRecovery.Count -ne 1 -or $crashWallpaperRecovery.Count -ne 0 -or
        [IO.File]::ReadAllText((Join-Path $crashConfigRecovery[0].FullName 'settings.json')) -ne
            '{"version":1,"beforeCrash":true}') {
        throw '恢复安装没有保留中断时已经发布的 Config 副本。'
    }
    if ((Test-Path -LiteralPath (Join-Path $crashAppRoot 'legacy-data-fallback.mode') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $crashAppRoot 'legacy-migration-complete.mode') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $crashAppRoot 'legacy-data-conflict.mode') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $crashAppRoot 'Config\.legacy-migration-owner.mode') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $crashAppRoot 'Wallpapers\.legacy-migration-owner.mode') -PathType Leaf)) {
        throw '中断恢复完成后的 fallback/complete 状态优先级不正确。'
    }
    $crashUninstall = Start-Process -FilePath $crashUninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    ) -WindowStyle Hidden -Wait -PassThru
    if ($crashUninstall.ExitCode -ne 0) { throw "中断恢复场景卸载失败，退出码：$($crashUninstall.ExitCode)" }
    if (-not (Test-Path -LiteralPath (Join-Path $legacyRoot 'Config\settings.json') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group\group.json') -PathType Leaf) -or
        -not (Test-Path -LiteralPath $crashConfigRecovery[0].FullName -PathType Container)) {
        throw '中断恢复场景卸载时删除了旧库或恢复副本。'
    }

    # Race a new unowned target into the gap after the all-directory preflight
    # and durable fallback prearm. The first pass must stop, and the retry must
    # refuse to archive that target because it carries no matching transaction
    # ownership marker.
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Config\settings.json'), '{"version":3,"raceSource":true}')
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group\group.json'), '{"version":3,"raceSource":true}')
    $raceInstall = Start-Process -FilePath $smokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=english',
        '/NOCLOSEAPPLICATIONS', '/MIGRATIONRACETEST', "/DIR=`"$raceInstallRoot`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($raceInstall.ExitCode -ne 0) { throw "迁移抢占注入安装失败，退出码：$($raceInstall.ExitCode)" }
    $raceAppRoot = Join-Path $raceInstallRoot 'App'
    $raceFallback = Join-Path $raceAppRoot 'legacy-data-fallback.mode'
    if ([IO.File]::ReadAllText((Join-Path $raceAppRoot 'Config\settings.json')) -ne '{"injectedAfterPreflight":true}' -or
        -not (Test-Path -LiteralPath $raceFallback -PathType Leaf) -or
        [IO.File]::ReadAllText($raceFallback) -notmatch '^MotionWallpaper\.LegacyFallback/v2\r?\n[0-9a-fA-F]{64}\r?\n$' -or
        -not (Test-Path -LiteralPath (Join-Path $raceAppRoot 'legacy-data-conflict.mode') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $raceAppRoot 'Config\.legacy-migration-owner.mode') -PathType Leaf)) {
        throw '迁移抢占注入没有留下带 token fallback 的未认领冲突目标。'
    }
    $raceRetry = Start-Process -FilePath $smokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=english',
        '/NOCLOSEAPPLICATIONS', "/DIR=`"$raceInstallRoot`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($raceRetry.ExitCode -ne 0) { throw "迁移抢占恢复安装失败，退出码：$($raceRetry.ExitCode)" }
    $raceRecoveries = @(Get-ChildItem -LiteralPath $raceAppRoot -Directory -Filter '*.legacy-recovery-*')
    if ([IO.File]::ReadAllText((Join-Path $raceAppRoot 'Config\settings.json')) -ne '{"injectedAfterPreflight":true}' -or
        $raceRecoveries.Count -ne 0 -or
        -not (Test-Path -LiteralPath $raceFallback -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $raceAppRoot 'legacy-data-conflict.mode') -PathType Leaf)) {
        throw '无 ownership token 的抢占目标在重试时被错误归档、覆盖或采纳。'
    }
    $raceUninstall = Start-Process -FilePath $raceUninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    ) -WindowStyle Hidden -Wait -PassThru
    if ($raceUninstall.ExitCode -ne 0) { throw "迁移抢占场景卸载失败，退出码：$($raceUninstall.ExitCode)" }

    # If a completed pass could not clear its fallback marker, both markers can
    # coexist. The legacy side remains authoritative and may have changed after
    # a manual launch; retry must re-stage it instead of trusting a stale target.
    $retryAppRoot = Join-Path $retryInstallRoot 'App'
    New-Item -ItemType Directory -Path (Join-Path $retryAppRoot 'Config') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $retryAppRoot 'Wallpapers\Groups\legacy-group') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Config\settings.json'), '{"version":1,"retryAuthoritative":true}')
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group\group.json'), '{"version":1,"retryAuthoritative":true}')
    [IO.File]::WriteAllText((Join-Path $retryAppRoot 'Config\settings.json'), '{"staleTarget":true}')
    [IO.File]::WriteAllText((Join-Path $retryAppRoot 'Wallpapers\Groups\legacy-group\group.json'), '{"staleTarget":true}')
    $retryToken = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
    [IO.File]::WriteAllText((Join-Path $retryAppRoot 'legacy-data-fallback.mode'), "MotionWallpaper.LegacyFallback/v2`r`n$retryToken`r`n")
    [IO.File]::WriteAllText((Join-Path $retryAppRoot 'legacy-migration-complete.mode'), "MotionWallpaper.LegacyMigration/v1`r`n")
    [IO.File]::WriteAllText((Join-Path $retryAppRoot 'Config\.legacy-migration-owner.mode'), "MotionWallpaper.LegacyMigrationOwner/v1`r`nConfig`r`n$retryToken`r`n")
    [IO.File]::WriteAllText((Join-Path $retryAppRoot 'Wallpapers\.legacy-migration-owner.mode'), "MotionWallpaper.LegacyMigrationOwner/v1`r`nWallpapers`r`n$retryToken`r`n")

    $retryInstall = Start-Process -FilePath $smokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=english',
        '/NOCLOSEAPPLICATIONS', "/DIR=`"$retryInstallRoot`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($retryInstall.ExitCode -ne 0) { throw "双标记重试场景静默安装失败，退出码：$($retryInstall.ExitCode)" }
    if ([IO.File]::ReadAllText((Join-Path $retryAppRoot 'Config\settings.json')) -ne
            '{"version":1,"retryAuthoritative":true}' -or
        [IO.File]::ReadAllText((Join-Path $retryAppRoot 'Wallpapers\Groups\legacy-group\group.json')) -ne
            '{"version":1,"retryAuthoritative":true}') {
        throw 'fallback 与 complete 同时存在时，安装器没有以旧数据根为权威重新迁移。'
    }
    $configRecovery = @(Get-ChildItem -LiteralPath $retryAppRoot -Directory -Filter 'Config.legacy-recovery-*')
    $wallpaperRecovery = @(Get-ChildItem -LiteralPath $retryAppRoot -Directory -Filter 'Wallpapers.legacy-recovery-*')
    if ($configRecovery.Count -ne 1 -or $wallpaperRecovery.Count -ne 1 -or
        [IO.File]::ReadAllText((Join-Path $configRecovery[0].FullName 'settings.json')) -ne '{"staleTarget":true}' -or
        [IO.File]::ReadAllText((Join-Path $wallpaperRecovery[0].FullName 'Groups\legacy-group\group.json')) -ne '{"staleTarget":true}') {
        throw '双标记重试没有完整保留旧的目标副本。'
    }
    if ((Test-Path -LiteralPath (Join-Path $retryAppRoot 'legacy-data-fallback.mode') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $retryAppRoot 'legacy-migration-complete.mode') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $retryAppRoot 'legacy-data-conflict.mode') -PathType Leaf)) {
        throw '双标记重试完成后的迁移状态标记不正确。'
    }
    $retryUninstall = Start-Process -FilePath $retryUninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    ) -WindowStyle Hidden -Wait -PassThru
    if ($retryUninstall.ExitCode -ne 0) { throw "双标记重试场景静默卸载失败，退出码：$($retryUninstall.ExitCode)" }
    if (-not (Test-Path -LiteralPath $configRecovery[0].FullName -PathType Container) -or
        -not (Test-Path -LiteralPath $wallpaperRecovery[0].FullName -PathType Container)) {
        throw '卸载器删除了为恢复而保留的旧目标副本。'
    }

    # Restore the isolated legacy fixture for the independent conflict case.
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Config\settings.json'), '{"version":1}')
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group\group.json'), '{"version":1}')

    # An unmarked, non-matching authoritative target can be residue from an
    # older interrupted migration, or it can contain newer portable data. The
    # installer must preserve both sides, record a conflict and not guess.
    New-Item -ItemType Directory -Path (Join-Path $installRoot 'App\Config') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $installRoot 'App\Config\settings.json'), '{"partialOrNewer":true}')
    $conflictInstall = Start-Process -FilePath $smokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=english',
        '/NOCLOSEAPPLICATIONS', "/DIR=`"$installRoot`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($conflictInstall.ExitCode -ne 0) { throw "旧数据冲突场景静默安装失败，退出码：$($conflictInstall.ExitCode)" }
    if ([IO.File]::ReadAllText((Join-Path $installRoot 'App\Config\settings.json')) -ne '{"partialOrNewer":true}' -or
        [IO.File]::ReadAllText((Join-Path $legacyRoot 'Config\settings.json')) -ne '{"version":1}') {
        throw '安装器覆盖了无法判定权威侧的旧数据冲突。'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $installRoot 'App\legacy-data-conflict.mode') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $installRoot 'App\legacy-data-fallback.mode') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $installRoot 'App\legacy-migration-complete.mode') -PathType Leaf)) {
        throw '安装器没有把无标记的非一致目标识别为迁移冲突。'
    }
    $conflictRetry = Start-Process -FilePath $smokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=english',
        '/NOCLOSEAPPLICATIONS', "/DIR=`"$installRoot`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($conflictRetry.ExitCode -ne 0) { throw "旧数据冲突二次安装失败，退出码：$($conflictRetry.ExitCode)" }
    $unexpectedConflictRecovery = @(Get-ChildItem -LiteralPath (Join-Path $installRoot 'App') `
        -Directory -Filter '*.legacy-recovery-*')
    if ([IO.File]::ReadAllText((Join-Path $installRoot 'App\Config\settings.json')) -ne '{"partialOrNewer":true}' -or
        $unexpectedConflictRecovery.Count -ne 0 -or
        (Test-Path -LiteralPath (Join-Path $installRoot 'App\legacy-data-fallback.mode') -PathType Leaf)) {
        throw '二次安装把无 fallback 的冲突目标错误归档、覆盖或升级成了重试迁移。'
    }
    $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    ) -WindowStyle Hidden -Wait -PassThru
    if ($uninstall.ExitCode -ne 0) { throw "冲突场景静默卸载失败，退出码：$($uninstall.ExitCode)" }

    # A third isolated install verifies that legacy migration never follows a
    # junction. Both the original tree and the junction target must survive.
    New-Item -ItemType Directory -Path (Join-Path $legacyRoot 'Config') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $legacyRoot 'Wallpapers') -Force | Out-Null
    New-Item -ItemType Directory -Path $reparseTargetRoot -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Config\junction-settings.json'), '{"junctionTest":true}')
    [IO.File]::WriteAllText((Join-Path $reparseTargetRoot 'outside-sentinel.txt'), 'must survive')
    New-Item -ItemType Junction -Path $reparseLink -Target $reparseTargetRoot | Out-Null
    if (([IO.File]::GetAttributes($reparseLink) -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
        throw '无法创建用于迁移安全测试的 junction。'
    }

    $reinstall = Start-Process -FilePath $smokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=english',
        '/NOCLOSEAPPLICATIONS', "/DIR=`"$installRoot`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($reinstall.ExitCode -ne 0) { throw "junction 场景静默安装失败，退出码：$($reinstall.ExitCode)" }
    if (-not (Test-Path -LiteralPath (Join-Path $legacyRoot 'Config\junction-settings.json') -PathType Leaf)) {
        throw '安装器在检测到 junction 后仍删除了旧数据。'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $reparseTargetRoot 'outside-sentinel.txt') -PathType Leaf)) {
        throw '安装器沿 junction 修改或删除了外部数据。'
    }
    if (Test-Path -LiteralPath (Join-Path $installRoot 'App\Config\junction-settings.json') -PathType Leaf) {
        throw '安装器在检测到 junction 后仍迁移了旧数据。'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $installRoot 'App\legacy-data-conflict.mode') -PathType Leaf)) {
        throw '安装器拒绝不安全迁移后没有记录需人工处理的冲突状态。'
    }

    $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    ) -WindowStyle Hidden -Wait -PassThru
    if ($uninstall.ExitCode -ne 0) { throw "junction 场景静默卸载失败，退出码：$($uninstall.ExitCode)" }
    if (-not (Test-Path -LiteralPath (Join-Path $reparseTargetRoot 'outside-sentinel.txt') -PathType Leaf)) {
        throw '卸载器沿 junction 删除了外部数据。'
    }

    Write-Host "安装器冒烟测试通过：中英文资源、App-local VC++ Runtime、版本资源、OpenH264 许可、单目录数据、原子 staging、双目录发布中断恢复、transaction ownership 抢占保护、双标记重试、旧数据回滚与失败回退、自定义路径、junction 拒绝、外置媒体保留和卸载清理均正常。"
} finally {
    foreach ($remainingUninstaller in @($uninstaller, $crashUninstaller, $raceUninstaller, $retryUninstaller)) {
        if (Test-Path -LiteralPath $remainingUninstaller -PathType Leaf) {
            Start-Process -FilePath $remainingUninstaller -ArgumentList @(
                '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
            ) -WindowStyle Hidden -Wait | Out-Null
        }
    }
    if (Test-Path -LiteralPath $reparseLink) {
        $linkAttributes = [IO.File]::GetAttributes($reparseLink)
        if (($linkAttributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
            throw "拒绝把非 reparse 目录当作测试 junction 清理：$reparseLink"
        }
        [IO.Directory]::Delete($reparseLink, $false)
    }
    $resolved = [IO.Path]::GetFullPath($testRoot)
    if ($resolved.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolved) -like 'MotionWallpaper-installer-test-*') {
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        throw "拒绝清理意外的测试目录：$resolved"
    }
}
