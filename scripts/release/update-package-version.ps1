param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$RcPath = "AudioPlaybackConnector2/res/AudioPlaybackConnector2.rc",
    [string]$ManifestPath = "AudioPlaybackConnector2 (Package)/Package.appxmanifest"
)

$ErrorActionPreference = "Stop"

$quadParts = $Version.Split(".")
if ($quadParts.Count -ne 4 -or ($quadParts | Where-Object { $_ -notmatch "^\d+$" })) {
    throw "Version must be a four-part package version, for example 1.2.3.0."
}
foreach ($part in $quadParts) {
    [uint64]$numericPart = 0
    if (-not [uint64]::TryParse($part, [ref]$numericPart) -or $numericPart -gt [uint16]::MaxValue) {
        throw "Every package version component must be between 0 and 65535."
    }
}

$rcVersion = $Version -replace "\.", ","
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

$rcContent = [System.IO.File]::ReadAllText($RcPath)
$patterns = @(
    'FILEVERSION \d+,\d+,\d+,\d+',
    'PRODUCTVERSION \d+,\d+,\d+,\d+',
    '"FileVersion",\s*"[\d.]+"',
    '"ProductVersion",\s*"[\d.]+"'
)
foreach ($pattern in $patterns) {
    if ([regex]::Matches($rcContent, $pattern).Count -ne 1) {
        throw "Expected exactly one RC version field matching '$pattern'."
    }
}
$rcContent = $rcContent -replace "FILEVERSION \d+,\d+,\d+,\d+", "FILEVERSION $rcVersion"
$rcContent = $rcContent -replace "PRODUCTVERSION \d+,\d+,\d+,\d+", "PRODUCTVERSION $rcVersion"
$rcContent = $rcContent -replace '"FileVersion",\s*"[\d.]+"', "`"FileVersion`", `"$Version`""
$rcContent = $rcContent -replace '"ProductVersion",\s*"[\d.]+"', "`"ProductVersion`", `"$Version`""
[System.IO.File]::WriteAllText($RcPath, $rcContent, $utf8NoBom)
foreach ($expected in @("FILEVERSION $rcVersion", "PRODUCTVERSION $rcVersion", "`"FileVersion`", `"$Version`"", "`"ProductVersion`", `"$Version`"")) {
    if (-not $rcContent.Contains($expected)) { throw "RC version update verification failed for '$expected'." }
}

[xml]$manifest = [System.IO.File]::ReadAllText($ManifestPath)
$manifest.Package.Identity.Version = $Version

$writer = [System.IO.StreamWriter]::new($ManifestPath, $false, $utf8NoBom)
try {
    $manifest.Save($writer)
} finally {
    $writer.Close()
}
[xml]$writtenManifest = [System.IO.File]::ReadAllText($ManifestPath)
if ($writtenManifest.Package.Identity.Version -ne $Version) {
    throw 'Package manifest version update verification failed.'
}
