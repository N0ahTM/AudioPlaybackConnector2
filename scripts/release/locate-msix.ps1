param(
    [string]$PackageRoot = "AudioPlaybackConnector2 (Package)/AppPackages",
    [string]$BinRoot = "AudioPlaybackConnector2 (Package)/bin",
    [string]$Configuration = "Release",
    [string]$Architecture = "x64",
    [string]$OutputDirectory = $env:RUNNER_TEMP,
    [string]$AppName = "AudioPlaybackConnector2",
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-MsixPackageVersion {
    param([string]$Path)

    $zip = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $zip.GetEntry("AppxManifest.xml")
        if (-not $entry) {
            throw "No AppxManifest.xml found in $Path"
        }

        $stream = $entry.Open()
        $reader = [System.IO.StreamReader]::new($stream)
        try {
            [xml]$manifest = $reader.ReadToEnd()
            return [string]$manifest.Package.Identity.Version
        } finally {
            $reader.Dispose()
            $stream.Dispose()
        }
    } finally {
        $zip.Dispose()
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = [System.IO.Path]::GetTempPath()
}

# Prefer bin/<arch>/<config> (current build output), then fall back to AppPackages
$binPath = Join-Path $BinRoot "$Architecture/$Configuration"
$searchPaths = @()
if (Test-Path $binPath) {
    $searchPaths += $binPath
}
if (Test-Path $PackageRoot) {
    $searchPaths += $PackageRoot
}

$candidates = foreach ($path in $searchPaths) {
    Get-ChildItem -Path $path -Recurse -Filter "*.msix" |
        Where-Object { $_.FullName -notmatch "[\\/]Dependencies[\\/]" -and $_.FullName -notmatch "[\\/]Upload[\\/]" }
}

if (-not $candidates) {
    throw "MSIX not found under '$PackageRoot' or '$binPath'."
}

$msix = if ([string]::IsNullOrWhiteSpace($Version)) {
    $candidates | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
} else {
    $versionMatches = foreach ($candidate in $candidates) {
        try {
            if ((Get-MsixPackageVersion -Path $candidate.FullName) -eq $Version) {
                $candidate
            }
        } catch {
            Write-Warning "Skipping '$($candidate.FullName)': $($_.Exception.Message)"
        }
    }

    $versionMatches | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
}

if (-not $msix) {
    throw "MSIX version '$Version' not found under '$PackageRoot' or '$binPath'."
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $assetName = $msix.Name
} else {
    $assetName = "${AppName}_${Version}_${Architecture}.msix"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$assetPath = Join-Path $OutputDirectory $assetName
Copy-Item -LiteralPath $msix.FullName -Destination $assetPath -Force

# Determine the most likely dependencies directory relative to the found MSIX
$msixDir = Split-Path -Parent $msix.FullName
$depsDir = Join-Path $msixDir "Dependencies/$Architecture"
if (-not (Test-Path $depsDir)) {
    $depsDir = $null
}

if ($env:GITHUB_OUTPUT) {
    "PATH=$assetPath" >> $env:GITHUB_OUTPUT
    "NAME=$assetName" >> $env:GITHUB_OUTPUT
    "SOURCE_PATH=$($msix.FullName)" >> $env:GITHUB_OUTPUT
    if ($depsDir -and (Test-Path $depsDir)) {
        "DEPS_DIR=$depsDir" >> $env:GITHUB_OUTPUT
    }
}

[pscustomobject]@{
    Path = $assetPath
    Name = $assetName
    SourcePath = $msix.FullName
    DependenciesDirectory = if ($depsDir -and (Test-Path $depsDir)) { $depsDir } else { $null }
}
