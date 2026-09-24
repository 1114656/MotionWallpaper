param(
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$buildRoot = Join-Path $projectRoot 'build'
$appPath = Join-Path $buildRoot 'MotionWallpaper.exe'
$sessionId = (Get-Process -Id $PID).SessionId

function Get-MotionProcesses {
    @(Get-Process -Name MotionWallpaper,motionwallpaper-agent,motionwallpaper-renderer -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -eq $sessionId })
}

$running = Get-MotionProcesses
$otherLocation = $false
foreach ($process in $running) {
    if (-not $process.Path) {
        throw "Cannot read Motion process $($process.Id). Exit Motion from its tray menu and retry."
    }
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($process.Path)) -ne $buildRoot) {
        $otherLocation = $true
    }
}

# All components use session-local IPC. Finish the running instance before
# switching folders or rebuilding, so it cannot receive the dev app's commands.
if ($running.Count -and ($Rebuild -or $otherLocation)) {
    Write-Host 'Closing the running Motion instance before starting development...'
    $exitEvent = [Threading.EventWaitHandle]::OpenExisting('Local\MotionWallpaper.ExitRequested')
    try {
        [void]$exitEvent.Set()
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        do {
            Start-Sleep -Milliseconds 200
            $remaining = Get-MotionProcesses
            if (-not $remaining.Count) { break }
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($remaining.Count) {
            throw 'Motion has not finished shutting down. No files were replaced; retry after it exits.'
        }
    } finally {
        $exitEvent.Dispose()
    }
}

if ($Rebuild -or -not (Test-Path -LiteralPath $appPath -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'build-native.ps1')
    if (-not $?) { throw 'The development build failed.' }
}
foreach ($required in @('MotionWallpaper.exe', 'motionwallpaper-agent.exe', 'motionwallpaper-renderer.exe', 'portable.mode')) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildRoot $required) -PathType Leaf)) {
        throw "Development payload is incomplete ($required). Run start-dev.ps1 -Rebuild."
    }
}

Write-Host "Starting development app: $appPath"
Write-Host "Persistent settings: $(Join-Path $buildRoot 'Config')"
Write-Host "Default wallpaper library: $(Join-Path $buildRoot 'Wallpapers')"
# This is the interactive settings window requested by the user. The app starts
# its own hidden Agent and activates the existing window on repeated launches.
Start-Process -FilePath $appPath -WorkingDirectory $buildRoot
