function Get-MotionWallpaperReleaseMetadata {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$Version
    )

    $normalized = $Version.Trim()
    $match = [regex]::Match(
        $normalized,
        '^(?<major>0|[1-9]\d*)\.(?<minor>0|[1-9]\d*)\.(?<patch>0|[1-9]\d*)(?:-alpha\.(?<alpha>0|[1-9]\d*))?$'
    )
    if (-not $match.Success) {
        throw "VERSION '$normalized' must use x.y.z or x.y.z-alpha.n."
    }

    $components = @(
        [uint32]$match.Groups['major'].Value,
        [uint32]$match.Groups['minor'].Value,
        [uint32]$match.Groups['patch'].Value
    )
    $isPrerelease = $match.Groups['alpha'].Success
    $revision = if ($isPrerelease) { [uint32]$match.Groups['alpha'].Value } else { [uint32]65535 }
    if (@($components + $revision | Where-Object { $_ -gt 65535 }).Count -ne 0) {
        throw "VERSION '$normalized' cannot be represented by Windows four-part version resources."
    }
    if ($isPrerelease -and $revision -eq 65535) {
        throw "VERSION '$normalized' uses alpha revision 65535, which is reserved for the stable release."
    }

    $fileVersion = '{0}.{1}.{2}.{3}' -f $components[0], $components[1], $components[2], $revision
    $formal = -join @([char]0x6B63, [char]0x5F0F, [char]0x7248)
    $installerLabel = -join @([char]0x5B89, [char]0x88C5, [char]0x5305)
    $portableLabel = -join @([char]0x4FBF, [char]0x643A, [char]0x7248)
    $testLabel = -join @([char]0x6D4B, [char]0x8BD5, [char]0x7248)
    if ($isPrerelease) {
        $installerBaseName = "MotionWallpaper-v$normalized-setup-windows-x64"
        $archiveBaseName = "MotionWallpaper-v$normalized-windows-x64"
        $editionName = "Alpha $testLabel"
    } else {
        $installerBaseName = "MotionWallpaper-$formal-v$normalized-Windows-x64-$installerLabel"
        $archiveBaseName = "MotionWallpaper-$formal-v$normalized-Windows-x64-$portableLabel"
        $editionName = $formal
    }

    [pscustomobject]@{
        Version = $normalized
        FileVersion = $fileVersion
        IsPrerelease = $isPrerelease
        EditionName = $editionName
        InstallerBaseName = $installerBaseName
        InstallerFileName = "$installerBaseName.exe"
        ArchiveBaseName = $archiveBaseName
        ArchiveFileName = "$archiveBaseName.zip"
    }
}
