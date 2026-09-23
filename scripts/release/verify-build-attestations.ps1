[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repository,
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')][string]$ExpectedCommit,
    [Parameter(Mandatory)][string]$ArtifactDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ReleaseVersion.psm1') -Force
$null = Resolve-ReleaseVersion -Tag $Tag
$drop = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
& python (Join-Path $PSScriptRoot 'verify-sbom.py') --drop $drop
if ($LASTEXITCODE -ne 0) { throw 'Local SBOM verification failed before attestation lookup.' }
$archive = Join-Path $drop '_manifest/AudioPlaybackConnector2_SBOM.zip'
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) { throw 'Release SBOM archive is missing.' }
$policy = @(
    '--repo', $Repository,
    '--signer-workflow', "$Repository/.github/workflows/build-release-artifacts.yml",
    '--signer-digest', $ExpectedCommit,
    '--source-digest', $ExpectedCommit,
    '--source-ref', "refs/tags/$Tag",
    '--deny-self-hosted-runners'
)
$files = @(Get-ChildItem -LiteralPath $drop -Recurse -File | Sort-Object FullName)
$packageCount = 0
foreach ($file in $files) {
    & gh attestation verify $file.FullName @policy --predicate-type 'https://slsa.dev/provenance/v1'
    if ($LASTEXITCODE -ne 0) { throw "Build provenance verification failed: $($file.Name)" }
    if ($file.Extension -in @('.msixbundle', '.msixupload', '.msix', '.appx', '.appinstaller')) {
        ++$packageCount
        & gh attestation verify $file.FullName @policy --predicate-type 'https://spdx.dev/Document'
        if ($LASTEXITCODE -ne 0) { throw "Package SBOM attestation verification failed: $($file.Name)" }
    }
}
if ($packageCount -eq 0) { throw 'Release drop contains no attested packages.' }
Write-Output "Verified provenance of $($files.Count) files and SBOM attestations of $packageCount packages."
