[CmdletBinding()]
param(
    [string]$Version,
    [string]$BuildDirectory,
    [string]$ArtifactDirectory
)

$ErrorActionPreference = 'Stop'
$arguments = @{}
if ($PSBoundParameters.ContainsKey('Version')) { $arguments.Version = $Version }
if ($PSBoundParameters.ContainsKey('BuildDirectory')) { $arguments.BuildDirectory = $BuildDirectory }
if ($PSBoundParameters.ContainsKey('ArtifactDirectory')) { $arguments.ArtifactDirectory = $ArtifactDirectory }

& (Join-Path $PSScriptRoot 'package-alpha.ps1') @arguments
