[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Source,
    [int]$Width = 1920,
    [int]$Height = 1080,
    [int]$Fps = 60,
    [string]$OutputDirectory,
    [switch]$RequireGpuScale
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$testExe = Join-Path $root 'native\x64\Release\MotionWallpaper.Tests\MotionWallpaper.Tests.exe'
$ffmpeg = Join-Path $root 'build\Tools\ffmpeg\ffmpeg.exe'
$ffprobe = Join-Path (Split-Path -Parent $ffmpeg) 'ffprobe.exe'
$Source = (Resolve-Path -LiteralPath $Source).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $root ('artifacts\gpu-scale-test-' + [guid]::NewGuid().ToString('N'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory to preserve previous evidence.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null

function Invoke-Bounded([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    # Windows CRT quoting also works on Windows PowerShell 5.1.
    $start.Arguments = ($Arguments | ForEach-Object {
        $quoted = [regex]::Replace($_, '(\\*)"', '$1$1\"')
        '"' + [regex]::Replace($quoted, '(\\+)$', '$1$1') + '"'
    }) -join ' '
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $child = [Diagnostics.Process]::Start($start)
    $stdout = $child.StandardOutput.ReadToEndAsync()
    $stderr = $child.StandardError.ReadToEndAsync()
    try {
        if (-not $child.WaitForExit(180000)) {
            # The native test host owns FFmpeg via a kill-on-close job.
            $child.Kill(); $child.WaitForExit()
            throw "$Name timed out"
        }
        $clock.Stop()
        [IO.File]::WriteAllText((Join-Path $OutputDirectory "$Name.stdout.log"), $stdout.Result)
        [IO.File]::WriteAllText((Join-Path $OutputDirectory "$Name.stderr.log"), $stderr.Result)
        if ($child.ExitCode) { throw "$Name exited $($child.ExitCode). $($stderr.Result) $($stdout.Result)" }
        [pscustomobject]@{ seconds = [math]::Round($clock.Elapsed.TotalSeconds, 3); text = $stdout.Result }
    } finally {
        if (-not $child.HasExited) { $child.Kill() }
        $child.Dispose()
    }
}

$sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
$results = @()
foreach ($case in @('gpu-preferred', 'reject-first-preview')) {
    $output = Join-Path $OutputDirectory "$case.mp4"
    $command = if ($case -eq 'gpu-preferred') { '--transcode-video' } else { '--transcode-reject-first-preview' }
    $run = Invoke-Bounded $case $testExe @($command, $ffmpeg, $Source, $output, "$Width", "$Height", "$Fps")
    $gpu = $run.text -match 'selected_gpu_scale=1'
    if ($RequireGpuScale -and $case -eq 'gpu-preferred' -and -not $gpu) { throw 'The expected GPU scaling route was not selected.' }
    if ($case -eq 'reject-first-preview' -and $gpu) { throw 'Rejected GPU preview was reused instead of falling back.' }
    $probe = Invoke-Bounded "$case-probe" $ffprobe @('-v', 'error', '-select_streams', 'v:0', '-count_frames', '-show_streams', '-of', 'json', $output)
    $stream = ($probe.text | ConvertFrom-Json).streams[0]
    $rateParts = $stream.avg_frame_rate -split '/'
    $rate = [double]$rateParts[0] / [double]$rateParts[1]
    if ($stream.width -ne $Width -or $stream.height -ne $Height -or $stream.codec_name -ne 'h264' -or
        $stream.pix_fmt -ne 'yuv420p' -or $stream.color_space -ne 'bt709' -or
        $stream.color_transfer -ne 'bt709' -or $stream.color_primaries -ne 'bt709' -or
        $stream.color_range -ne 'tv' -or [math]::Abs($rate - $Fps) -gt 0.01 -or [int]$stream.nb_read_frames -lt 1) {
        throw "$case output contract failed"
    }
    $null = Invoke-Bounded "$case-windows-first-frame" $testExe @('--probe-video-first-frame', $output)
    $results += [pscustomobject]@{ case = $case; seconds = $run.seconds; gpuScale = $gpu;
        width = $stream.width; height = $stream.height; fps = $rate; decodedFrames = $stream.nb_read_frames }
}
if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne $sourceHash) { throw 'The source file was modified.' }
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding UTF8
$results
