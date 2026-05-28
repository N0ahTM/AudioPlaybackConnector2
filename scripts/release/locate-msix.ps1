param(
    [string]$PackageRoot = "AudioPlaybackConnector2 (Package)/AppPackages",
    [string]$OutputDirectory = $env:RUNNER_TEMP,
    [string]$AppName = "AudioPlaybackConnector2",
    [string]$Version = "",
    [string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = [System.IO.Path]::GetTempPath()
}

$msix = Get-ChildItem -Path $PackageRoot -Recurse -Filter "*.msix" |
    Where-Object { $_.FullName -notmatch "[\\/]Dependencies[\\/]" } |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1

if (-not $msix) {
    throw "MSIX not found under '$PackageRoot'."
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $assetName = $msix.Name
} else {
    $assetName = "${AppName}_${Version}_${Architecture}.msix"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$assetPath = Join-Path $OutputDirectory $assetName
Copy-Item -LiteralPath $msix.FullName -Destination $assetPath -Force

if ($env:GITHUB_OUTPUT) {
    "PATH=$assetPath" >> $env:GITHUB_OUTPUT
    "NAME=$assetName" >> $env:GITHUB_OUTPUT
}

[pscustomobject]@{
    Path = $assetPath
    Name = $assetName
    SourcePath = $msix.FullName
}
