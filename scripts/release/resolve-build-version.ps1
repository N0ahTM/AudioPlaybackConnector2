param([Parameter(Mandatory = $true)] [string]$RepositoryPath)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ReleaseVersion.psm1') -Force
$tag = $env:APC_BUILD_RELEASE_TAG
if ([string]::IsNullOrEmpty($tag)) {
    $tag = & git -C $RepositoryPath describe --tags --abbrev=0
    if ($LASTEXITCODE -ne 0) { throw 'No release tag is available. Fetch tags or build with /p:ReleaseTag=vX.Y.Z.' }
    $tag = $tag.Trim()
}
$version = Resolve-ReleaseVersion -Tag $tag
if (-not [string]::IsNullOrEmpty($env:APC_BUILD_PACKAGE_VERSION) -and
    $env:APC_BUILD_PACKAGE_VERSION -cne $version.PackageVersion) {
    throw 'PackageVersion disagrees with ReleaseTag. Specify the release tag as the version source.'
}
$version.PackageVersion
