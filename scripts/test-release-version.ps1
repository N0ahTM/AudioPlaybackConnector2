$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'release/ReleaseVersion.psm1') -Force

foreach ($tag in @('v0.9.1', 'v1.2.3', 'v65535.65535.65535')) {
    $actual = Resolve-ReleaseVersion -Tag $tag
    if ($actual.Tag -cne $tag -or $actual.SemVer -cne $tag.Substring(1) -or
        $actual.PackageVersion -cne ($tag.Substring(1) + '.0')) {
        throw "Incorrect release derivation for $tag."
    }
}
foreach ($tag in @('1.2.3', 'V1.2.3', 'vv1.2.3', 'v01.2.3', 'v1.02.3', 'v1.2.03',
        'v1.2', 'v1.2.3.4', 'v1.2.3-beta', 'v1.2.3+build', 'v65536.0.0',
        'v0.65536.0', 'v0.0.65536', 'v999999999999999999999.0.0',
        'v-1.2.3', ' v1.2.3', "v1.2.3`n", 'v1.2.3 ', 'v١.2.3')) {
    $rejected = $false
    try { $null = Resolve-ReleaseVersion -Tag $tag }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Invalid release tag accepted: '$tag'." }
}

# A disagreement must fail before any remote release lookup.
function gh { throw 'Unexpected remote lookup during version preflight.' }
$failure = $null
try {
    & (Join-Path $PSScriptRoot 'release/verify-release-promotion.ps1') `
        -Repository 'fixture/repository' -Tag 'v1.2.3' -PackageVersion '1.2.4.0'
} catch { $failure = $_.Exception.Message }
if ($failure -ne 'Promotion package version disagrees with the release tag.') {
    throw "Promotion failed to reject inconsistent versions before remote lookup: $failure"
}
Write-Host 'Release version derivation, invalid tags and promotion preflight passed.'
