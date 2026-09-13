[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[^/]+/[^/]+$')]
    [string]$Repository,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+$')]
    [string]$Tag,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')]
    [string]$PackageVersion,

    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$ArtifactDirectory,

    [ValidateSet('Draft', 'Published')]
    [string]$ExpectedReleaseState = 'Draft',

    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$ExpectedAssetFingerprint,

    [switch]$WriteGitHubOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$releaseJson = gh release view $Tag --repo $Repository --json tagName,isDraft,isPrerelease,assets
if ($LASTEXITCODE -ne 0) {
    throw "Could not read GitHub Release $Tag."
}
$release = $releaseJson | ConvertFrom-Json

if ($release.tagName -ne $Tag) {
    throw "Release tag $($release.tagName) does not match expected tag $Tag."
}
if ($release.isPrerelease) {
    throw 'A prerelease cannot be promoted as a stable release.'
}
$expectsDraft = $ExpectedReleaseState -eq 'Draft'
if ([bool]$release.isDraft -ne $expectsDraft) {
    throw "Release $Tag is not in the expected $ExpectedReleaseState state."
}

$canonicalAssets = @($release.assets | Sort-Object name | ForEach-Object {
        "$($_.name)`t$($_.size)`t$($_.digest)"
    }) -join "`n"
$sha256 = [Security.Cryptography.SHA256]::Create()
try {
    $fingerprintBytes = $sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($canonicalAssets))
} finally {
    $sha256.Dispose()
}
$assetFingerprint = ([Convert]::ToHexString($fingerprintBytes)).ToLowerInvariant()
if ($ExpectedAssetFingerprint -and $assetFingerprint -ne $ExpectedAssetFingerprint) {
    throw "Release assets changed after validation: $assetFingerprint does not match $ExpectedAssetFingerprint."
}
if ($WriteGitHubOutput) {
    if (-not $env:GITHUB_OUTPUT) {
        throw 'GITHUB_OUTPUT is not available.'
    }
    "ASSET_FINGERPRINT=$assetFingerprint" >> $env:GITHUB_OUTPUT
}

if (-not $ArtifactDirectory) {
    [pscustomobject]@{
        Tag = $Tag
        State = $ExpectedReleaseState
        AssetCount = @($release.assets).Count
        AssetFingerprint = $assetFingerprint
    }
    return
}

$publishableExtensions = @('.appx', '.msix', '.msixbundle', '.appinstaller', '.exe', '.cer')
$artifactFiles = @(Get-ChildItem -LiteralPath $ArtifactDirectory -Recurse -File |
    Where-Object { $publishableExtensions -contains $_.Extension.ToLowerInvariant() })
if ($artifactFiles.Count -eq 0) {
    throw "No publishable files were found in $ArtifactDirectory."
}

$duplicateArtifactNames = @($artifactFiles | Group-Object Name | Where-Object Count -gt 1)
if ($duplicateArtifactNames.Count -gt 0) {
    throw "The build artifact contains duplicate file names: $($duplicateArtifactNames.Name -join ', ')."
}

$releaseAssets = @($release.assets)
$duplicateReleaseNames = @($releaseAssets | Group-Object name | Where-Object Count -gt 1)
if ($duplicateReleaseNames.Count -gt 0) {
    throw "The GitHub Release contains duplicate asset names: $($duplicateReleaseNames.Name -join ', ')."
}

$artifactNames = @($artifactFiles | ForEach-Object Name | Sort-Object)
$releaseNames = @($releaseAssets | ForEach-Object name | Sort-Object)
$missingNames = @($artifactNames | Where-Object { $_ -notin $releaseNames })
$unexpectedNames = @($releaseNames | Where-Object { $_ -notin $artifactNames })
if ($missingNames.Count -gt 0) {
    throw "GitHub Release assets are missing files from the immutable build artifact: $($missingNames -join ', ')."
}
if ($unexpectedNames.Count -gt 0) {
    throw "GitHub Release contains files that are not in the immutable build artifact: $($unexpectedNames -join ', ')."
}

$semanticVersion = $Tag.TrimStart('v')
$requiredNames = @(
    "AudioPlaybackConnector2_${PackageVersion}_x64_ARM64.msixbundle",
    'AudioPlaybackConnector2-WebSetup.exe',
    "AudioPlaybackConnector2-Setup-${semanticVersion}.exe",
    'AudioPlaybackConnector2.appinstaller',
    'AudioPlaybackConnector2.cer'
)
foreach ($requiredName in $requiredNames) {
    if ($artifactNames -notcontains $requiredName) {
        throw "Required release asset is missing: $requiredName."
    }
}
if (@($artifactFiles | Where-Object { $_.Extension -in @('.appx', '.msix') }).Count -eq 0) {
    throw 'The release does not contain dependency packages.'
}

$assetsByName = @{}
foreach ($asset in $releaseAssets) {
    $assetsByName[$asset.name] = $asset
}

foreach ($file in $artifactFiles) {
    $asset = $assetsByName[$file.Name]
    if ($asset.state -ne 'uploaded') {
        throw "Release asset $($file.Name) is not fully uploaded."
    }
    if ([int64]$asset.size -ne $file.Length) {
        throw "Release asset $($file.Name) has size $($asset.size), expected $($file.Length)."
    }

    $expectedDigest = 'sha256:' + (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $actualDigest = [string]$asset.digest
    if ([string]::IsNullOrWhiteSpace($actualDigest)) {
        throw "GitHub did not provide a SHA-256 digest for release asset $($file.Name)."
    }
    if ($actualDigest.ToLowerInvariant() -ne $expectedDigest) {
        throw "Release asset $($file.Name) has digest $actualDigest, expected $expectedDigest."
    }
}

[pscustomobject]@{
    Tag = $Tag
    State = $ExpectedReleaseState
    AssetCount = $artifactFiles.Count
    VerifiedBytes = ($artifactFiles | Measure-Object Length -Sum).Sum
    AssetFingerprint = $assetFingerprint
}
