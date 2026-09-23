[CmdletBinding()]
param(
    [string]$TestExecutable,
    [string]$Ffmpeg,
    [string]$OutputDirectory,
    [ValidateSet('sdr-v210-mov', 'pq-v210-mov', 'hlg-v210-mov', 'sdr-v210-rotate90-mov')]
    [string[]]$CaseName = @(),
    [ValidateRange(10, 600)]
    [int]$TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $TestExecutable) {
    $TestExecutable = Join-Path $projectRoot 'native\x64\Release\MotionWallpaper.Tests\MotionWallpaper.Tests.exe'
}
$TestExecutable = [IO.Path]::GetFullPath($TestExecutable)
if (-not (Test-Path -LiteralPath $TestExecutable -PathType Leaf)) {
    throw "Build the native test executable first: $TestExecutable"
}
if (-not $Ffmpeg) {
    $candidates = @(
        (Join-Path (Split-Path -Parent $TestExecutable) 'Tools\ffmpeg\ffmpeg.exe'),
        (Join-Path $projectRoot 'native\x64\Release\Tools\ffmpeg\ffmpeg.exe'),
        (Join-Path $projectRoot 'build\Tools\ffmpeg\ffmpeg.exe')
    )
    $Ffmpeg = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (-not $Ffmpeg) { throw 'Specify -Ffmpeg or build the bundled FFmpeg tools first.' }
$Ffmpeg = [IO.Path]::GetFullPath($Ffmpeg)
$ffprobe = Join-Path (Split-Path -Parent $Ffmpeg) 'ffprobe.exe'
foreach ($tool in @($Ffmpeg, $ffprobe)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing video tool: $tool" }
}
if (-not $OutputDirectory) {
    $name = 'video-pipeline-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $OutputDirectory = Join-Path (Join-Path (Split-Path -Parent $projectRoot) '.tmp') $name
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $OutputDirectory) -and
    @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count -ne 0) {
    throw "Use a new or empty output directory; existing results will not be overwritten: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$logsDirectory = Join-Path $OutputDirectory 'logs'
New-Item -ItemType Directory -Path $logsDirectory | Out-Null

function ConvertTo-WindowsArgument {
    param([AllowEmptyString()][string]$Value)
    # Start-Process joins ArgumentList rather than preserving argv boundaries.
    # Quote according to the Windows CRT rules, including trailing backslashes.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function Invoke-BoundedVideoProcess {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string[]]$Arguments
    )
    $stdoutPath = Join-Path $logsDirectory ($Name + '.stdout.log')
    $stderrPath = Join-Path $logsDirectory ($Name + '.stderr.log')
    $argumentLine = ($Arguments | ForEach-Object { ConvertTo-WindowsArgument $_ }) -join ' '
    [ordered]@{ executable = $Executable; arguments = $Arguments; timeoutSeconds = $TimeoutSeconds } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $logsDirectory ($Name + '.command.json')) -Encoding UTF8
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $Executable -ArgumentList $argumentLine -WindowStyle Hidden `
        -WorkingDirectory (Split-Path -Parent $Executable) -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    try {
        $null = $process.Handle
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            # Kill the test process together with FFmpeg/probe descendants, not
            # unrelated copies of those executables running on the machine.
            $taskkill = Join-Path $env:SystemRoot 'System32\taskkill.exe'
            $killer = Start-Process -FilePath $taskkill -ArgumentList @('/PID', "$($process.Id)", '/T', '/F') `
                -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput (Join-Path $logsDirectory ($Name + '.timeout.stdout.log')) `
                -RedirectStandardError (Join-Path $logsDirectory ($Name + '.timeout.stderr.log'))
            try {
                if (-not $killer.WaitForExit(10000)) { $killer.Kill() }
            } finally { $killer.Dispose() }
            if (-not $process.WaitForExit(10000)) { $process.Kill() }
            throw "$Name exceeded ${TimeoutSeconds}s; its process tree was stopped. Logs: $stderrPath"
        }
        # Refresh the cached Process state before reading ExitCode in Windows
        # PowerShell 5.1 as well as PowerShell 7.
        $process.Refresh()
        $exitCode = $process.ExitCode
        if ($exitCode -ne 0) {
            $tail = Get-Content -LiteralPath $stderrPath -Tail 12 -ErrorAction SilentlyContinue | Out-String
            throw "$Name exited with code $exitCode. $tail Logs: $stdoutPath / $stderrPath"
        }
        return [pscustomobject]@{
            elapsedSeconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
            stdout = $stdoutPath
            stderr = $stderrPath
        }
    } finally {
        $timer.Stop()
        $process.Dispose()
    }
}

function Read-VideoProbe {
    param([string]$Name, [string]$Path)
    $result = Invoke-BoundedVideoProcess -Name $Name -Executable $ffprobe -Arguments @(
        '-v', 'error', '-select_streams', 'v:0', '-show_entries',
        'stream=codec_name,pix_fmt,width,height,r_frame_rate,avg_frame_rate,color_range,color_space,color_transfer,color_primaries,bits_per_raw_sample:stream_side_data=side_data_type,rotation:stream_tags=rotate:format=duration',
        '-of', 'json', $Path
    )
    return Get-Content -LiteralPath $result.stdout -Raw | ConvertFrom-Json
}

function ConvertFrom-Rational {
    param([string]$Value)
    $parts = $Value.Split('/')
    if ($parts.Length -ne 2) { throw "Invalid frame rate: $Value" }
    $numerator = [double]::Parse($parts[0], [Globalization.CultureInfo]::InvariantCulture)
    $denominator = [double]::Parse($parts[1], [Globalization.CultureInfo]::InvariantCulture)
    if ($denominator -eq 0) { throw "Unknown frame rate: $Value" }
    return $numerator / $denominator
}

function Get-DisplayMatrices {
    param([Parameter(Mandatory)]$Stream)
    $sideData = $Stream.PSObject.Properties['side_data_list']
    if ($null -ne $sideData) {
        return @($sideData.Value | Where-Object {
            $kind = $_.PSObject.Properties['side_data_type']
            $null -ne $kind -and $kind.Value -eq 'Display Matrix'
        })
    }
}

$baseColor = 'setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709'
$cases = @(
    [pscustomobject]@{
        name = 'sdr-v210-mov'; primaries = 'bt709'; transfer = 'bt709'; matrix = 'bt709'
        rotation = 0; targetWidth = 640; targetHeight = 360
        filter = $baseColor + ',format=yuv422p10le'
    },
    [pscustomobject]@{
        name = 'pq-v210-mov'; primaries = 'bt2020'; transfer = 'smpte2084'; matrix = 'bt2020nc'
        rotation = 0; targetWidth = 640; targetHeight = 360
        filter = $baseColor + ',zscale=transferin=bt709:primariesin=bt709:matrixin=bt709:rangein=limited:transfer=smpte2084:primaries=bt2020:matrix=bt2020nc:range=limited:npl=100,format=yuv422p10le'
    },
    [pscustomobject]@{
        name = 'hlg-v210-mov'; primaries = 'bt2020'; transfer = 'arib-std-b67'; matrix = 'bt2020nc'
        rotation = 0; targetWidth = 640; targetHeight = 360
        filter = $baseColor + ',zscale=transferin=bt709:primariesin=bt709:matrixin=bt709:rangein=limited:transfer=arib-std-b67:primaries=bt2020:matrix=bt2020nc:range=limited:npl=100,format=yuv422p10le'
    },
    [pscustomobject]@{
        name = 'sdr-v210-rotate90-mov'; primaries = 'bt709'; transfer = 'bt709'; matrix = 'bt709'
        rotation = 90; targetWidth = 360; targetHeight = 640
        filter = $baseColor + ',format=yuv422p10le'
    }
)
if ($CaseName.Count) { $cases = @($cases | Where-Object { $CaseName -contains $_.name }) }
$results = [Collections.Generic.List[object]]::new()
foreach ($case in $cases) {
    $source = Join-Path $OutputDirectory ($case.name + '.mov')
    $destination = Join-Path $OutputDirectory ($case.name + '-sdr-h264.mp4')
    $result = [ordered]@{
        name = $case.name; passed = $false; source = $source; destination = $destination
        sourceSha256Before = ''; sourceSha256After = ''; sourceRotationDegrees = $case.rotation; transcodeSeconds = $null
        output = $null; lumaMean = $null; error = ''
    }
    try {
        Write-Host "[$($case.name)] Generate a two-second 10-bit MOV fixture."
        $generatedSource = if ($case.rotation) {
            Join-Path $OutputDirectory ($case.name + '-unrotated.mov')
        } else { $source }
        $null = Invoke-BoundedVideoProcess -Name ($case.name + '-generate') -Executable $Ffmpeg -Arguments @(
            '-hide_banner', '-loglevel', 'error', '-nostdin', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30',
            '-t', '2', '-an', '-vf', $case.filter, '-c:v', 'v210', '-pix_fmt', 'yuv422p10le',
            '-color_range', 'tv', '-color_primaries', $case.primaries, '-color_trc', $case.transfer,
            '-colorspace', $case.matrix, '-movflags', '+write_colr', $generatedSource
        )
        if ($case.rotation) {
            # Set a container Display Matrix through stream-copy remuxing.
            # The coded pixels stay 640x360; only visual orientation changes.
            $null = Invoke-BoundedVideoProcess -Name ($case.name + '-set-display-matrix') -Executable $Ffmpeg -Arguments @(
                '-hide_banner', '-v', 'error', '-nostdin', '-y',
                '-display_rotation:v:0', [string]$case.rotation, '-i', $generatedSource,
                '-map', '0:v:0', '-c', 'copy', $source
            )
        }
        $result.sourceSha256Before = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        $sourceProbe = Read-VideoProbe -Name ($case.name + '-probe-source') -Path $source
        $sourceStream = @($sourceProbe.streams)[0]
        if ($sourceStream.codec_name -ne 'v210' -or $sourceStream.pix_fmt -ne 'yuv422p10le' -or
            $sourceStream.color_primaries -ne $case.primaries -or $sourceStream.color_transfer -ne $case.transfer -or
            $sourceStream.color_space -ne $case.matrix) {
            throw 'The generated fixture did not preserve its requested 10-bit format and color metadata.'
        }
        if ($case.rotation) {
            $matrices = @(Get-DisplayMatrices $sourceStream)
            if ($matrices.Count -ne 1 -or [Math]::Abs([double]$matrices[0].rotation) -ne 90 -or
                $sourceStream.width -ne 640 -or $sourceStream.height -ne 360) {
                throw 'The fixture must contain a 90-degree Display Matrix over unchanged 640x360 coded pixels.'
            }
        }
        Write-Host "[$($case.name)] Run the real application transcoder (hardware attempts may fall back to CPU)."
        $transcode = Invoke-BoundedVideoProcess -Name ($case.name + '-transcode') -Executable $TestExecutable -Arguments @(
            '--transcode-video', $Ffmpeg, $source, $destination,
            [string]$case.targetWidth, [string]$case.targetHeight, '30'
        )
        $result.transcodeSeconds = $transcode.elapsedSeconds
        $outputProbe = Read-VideoProbe -Name ($case.name + '-probe-output') -Path $destination
        $stream = @($outputProbe.streams)[0]
        $duration = [double]::Parse($outputProbe.format.duration, [Globalization.CultureInfo]::InvariantCulture)
        $sourceDuration = [double]::Parse($sourceProbe.format.duration, [Globalization.CultureInfo]::InvariantCulture)
        $frameRate = ConvertFrom-Rational $stream.avg_frame_rate
        if ($stream.codec_name -ne 'h264' -or $stream.pix_fmt -ne 'yuv420p' -or
            $stream.width -ne $case.targetWidth -or $stream.height -ne $case.targetHeight -or
            [Math]::Abs($frameRate - 30) -gt 0.01 -or
            [Math]::Abs((ConvertFrom-Rational $stream.r_frame_rate) - 30) -gt 0.01 -or
            [Math]::Abs($duration - $sourceDuration) -gt 0.15 -or $duration -lt 1.8 -or
            $stream.color_range -ne 'tv' -or $stream.color_primaries -ne 'bt709' -or
            $stream.color_transfer -ne 'bt709' -or $stream.color_space -ne 'bt709') {
            throw "The output failed the H.264 / 8-bit SDR BT.709 / limited range / $($case.targetWidth)x$($case.targetHeight) / 30 FPS / duration contract."
        }
        $outputMatrices = @(Get-DisplayMatrices $stream)
        $outputTags = $stream.PSObject.Properties['tags']
        $rotationTag = if ($null -ne $outputTags) { $outputTags.Value.PSObject.Properties['rotate'] } else { $null }
        if ($outputMatrices.Count -ne 0 -or $null -ne $rotationTag) {
            throw 'The output retained a container rotation marker instead of baking visual orientation into its pixels.'
        }
        $result.output = [ordered]@{
            codec = $stream.codec_name; pixelFormat = $stream.pix_fmt
            width = $stream.width; height = $stream.height; fps = $frameRate; durationSeconds = $duration
            range = $stream.color_range; primaries = $stream.color_primaries
            transfer = $stream.color_transfer; matrix = $stream.color_space
            rotationMetadataAbsent = $true
        }
        $decode = Invoke-BoundedVideoProcess -Name ($case.name + '-decode-all-frames') -Executable $Ffmpeg -Arguments @(
            '-hide_banner', '-v', 'error', '-nostdin', '-xerror', '-err_detect', 'explode', '-i', $destination,
            '-map', '0:v:0', '-an', '-vf', 'signalstats,metadata=mode=print:key=lavfi.signalstats.YAVG:file=-',
            '-f', 'null', '-'
        )
        $decodeErrors = [string](Get-Content -LiteralPath $decode.stderr -Raw)
        if (-not [string]::IsNullOrWhiteSpace($decodeErrors)) {
            throw 'Full output decoding reported errors; inspect the decode stderr log.'
        }
        $luma = @([regex]::Matches((Get-Content -LiteralPath $decode.stdout -Raw), 'lavfi\.signalstats\.YAVG=([0-9.]+)') |
            ForEach-Object { [double]::Parse($_.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) })
        if ($luma.Count -lt 55) { throw 'The full decode did not produce enough luma measurements for this two-second clip.' }
        $mean = ($luma | Measure-Object -Average).Average
        if ($mean -le 20 -or $mean -ge 230) { throw "The decoded fixture is nearly all black or all white (mean luma $mean)." }
        $result.lumaMean = [Math]::Round($mean, 3)
        $result.passed = $true
    } catch {
        $result.error = $_.Exception.Message
    } finally {
        if ($result.sourceSha256Before) {
            try {
                $result.sourceSha256After = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
                if ($result.sourceSha256After -ne $result.sourceSha256Before) {
                    $result.passed = $false
                    $result.error += ' Source SHA-256 changed during transcoding.'
                }
            } catch {
                $result.passed = $false
                $result.error += ' Could not verify the retained source: ' + $_.Exception.Message
            }
        }
    }
    $results.Add([pscustomobject]$result)
    if ($result.passed) { Write-Host "[$($case.name)] PASS ($($result.transcodeSeconds)s; mean luma $($result.lumaMean))." }
    else { Write-Warning "[$($case.name)] FAIL: $($result.error)" }
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
[ordered]@{
    testExecutable = $TestExecutable; ffmpeg = $Ffmpeg; timeoutSeconds = $TimeoutSeconds
    note = 'Synthetic SDR/PQ/HLG and container-rotation conversion, metadata, unchanged-source and full-decode regression; luma sanity is not a visual color certification.'
    cases = @($results.ToArray())
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
Write-Host "Results and retained fixtures: $summaryPath"
if (@($results | Where-Object { -not $_.passed }).Count) {
    throw "One or more real video pipeline regressions failed. See $summaryPath"
}
