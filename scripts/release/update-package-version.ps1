param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$RcPath = "AudioPlaybackConnector2/res/AudioPlaybackConnector2.rc",
    [string]$ManifestPath = "AudioPlaybackConnector2 (Package)/Package.appxmanifest"
)

$ErrorActionPreference = "Stop"

$quadParts = $Version.Split(".")
if ($quadParts.Count -ne 4 -or ($quadParts | Where-Object { $_ -notmatch "^\d+$" })) {
    throw "Version must be a four-part package version, for example 0.5.0.0."
}

$rcVersion = $Version -replace "\.", ","
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

$rcContent = [System.IO.File]::ReadAllText($RcPath)
$rcContent = $rcContent -replace "FILEVERSION \d+,\d+,\d+,\d+", "FILEVERSION $rcVersion"
$rcContent = $rcContent -replace "PRODUCTVERSION \d+,\d+,\d+,\d+", "PRODUCTVERSION $rcVersion"
$rcContent = $rcContent -replace '"FileVersion",\s*"[\d.]+"', "`"FileVersion`", `"$Version`""
$rcContent = $rcContent -replace '"ProductVersion",\s*"[\d.]+"', "`"ProductVersion`", `"$Version`""
[System.IO.File]::WriteAllText($RcPath, $rcContent, $utf8NoBom)

[xml]$manifest = [System.IO.File]::ReadAllText($ManifestPath)
$manifest.Package.Identity.Version = $Version

$writer = [System.IO.StreamWriter]::new($ManifestPath, $false, $utf8NoBom)
try {
    $manifest.Save($writer)
} finally {
    $writer.Close()
}
