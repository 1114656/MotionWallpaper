[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path ([IO.Path]::GetTempPath()) ('MotionWallpaper-fill-geometry-' + [Guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ffmpeg = Join-Path $root 'build\Tools\ffmpeg\ffmpeg.exe'
$testExe = Join-Path $root 'native\x64\Release\MotionWallpaper.Tests\MotionWallpaper.Tests.exe'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
function Run-Bounded([string]$name, [string]$exe, [string[]]$values) {
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
    foreach($value in $values){$info.ArgumentList.Add($value)}
    $child=[Diagnostics.Process]::new(); $child.StartInfo=$info
    $null=$child.Start(); $stdout=$child.StandardOutput.ReadToEndAsync(); $stderr=$child.StandardError.ReadToEndAsync()
    try {
        if(-not $child.WaitForExit(90000)){ $child.Kill($true); $null=$child.WaitForExit(5000); throw "Timeout: $name" }
        $stdout.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $OutputDirectory "$name.stdout.log")
        $stderr.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $OutputDirectory "$name.stderr.log")
        if($child.ExitCode -ne 0){ throw "Failed: $name ($($child.ExitCode))" }
    } finally { if(-not $child.HasExited){$child.Kill($true)}; $child.Dispose() }
}
$source=Join-Path $OutputDirectory 'square-source.mov'
Run-Bounded 'generate' $ffmpeg @('-v','error','-nostdin','-y','-f','lavfi','-i',
    'color=c=black:s=640x360:r=30,drawbox=x=280:y=140:w=80:h=80:color=white:t=fill',
    '-t','1','-an','-c:v','v210','-pix_fmt','yuv422p10le','-color_primaries','bt709','-color_trc','bt709','-colorspace','bt709',$source)
$hash=(Get-FileHash -LiteralPath $source).Hash
$results=foreach($case in @(@('portrait',180,320,68,76),@('ultrawide',480,200,57,63))) {
    $name=[string]$case[0]; $width=[int]$case[1]; $height=[int]$case[2]
    $target=Join-Path $OutputDirectory "$name.mp4"
    Run-Bounded "$name-transcode" $testExe @('--transcode-cpu-video',$ffmpeg,$source,$target,[string]$width,[string]$height,'30')
    $raw=Join-Path $OutputDirectory "$name.gray"
    Run-Bounded "$name-frame" $ffmpeg @('-v','error','-nostdin','-y','-i',$target,'-frames:v','1','-pix_fmt','gray','-f','rawvideo',$raw)
    $bytes=[IO.File]::ReadAllBytes($raw)
    if($bytes.Length -ne $width*$height){throw "Invalid dimensions: $name"}
    $minX=$width; $minY=$height; $maxX=-1; $maxY=-1
    for($y=0;$y -lt $height;$y++){for($x=0;$x -lt $width;$x++){
        if($bytes[$y*$width+$x] -gt 200){$minX=[Math]::Min($minX,$x);$maxX=[Math]::Max($maxX,$x);$minY=[Math]::Min($minY,$y);$maxY=[Math]::Max($maxY,$y)}
    }}
    $boxWidth=$maxX-$minX+1; $boxHeight=$maxY-$minY+1
    if([Math]::Abs($boxWidth-$boxHeight) -gt 2 -or $boxWidth -lt $case[3] -or $boxWidth -gt $case[4]){throw "Center square was distorted or lost detail: $name $boxWidth x $boxHeight"}
    if([Math]::Abs(($minX+$maxX+1)-$width) -gt 4 -or [Math]::Abs(($minY+$maxY+1)-$height) -gt 4){throw "Crop was not centered: $name"}
    Run-Bounded "$name-full-decode" $ffmpeg @('-v','error','-nostdin','-i',$target,'-f','null','-')
    [ordered]@{Case=$name;Width=$width;Height=$height;VisibleSquareWidth=$boxWidth;VisibleSquareHeight=$boxHeight;Result='PASS'}
}
if((Get-FileHash -LiteralPath $source).Hash -ne $hash){throw 'Source changed'}
$results | ConvertTo-Json -Depth 4 | Tee-Object -FilePath (Join-Path $OutputDirectory 'summary.json')
