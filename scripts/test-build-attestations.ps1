[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('apc-attestation-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $root '_manifest') | Out-Null
Set-Content (Join-Path $root 'application.msixbundle') 'package fixture'
Set-Content (Join-Path $root '_manifest/manifest.spdx.json') '{}'
$archive = Join-Path $root '_manifest/AudioPlaybackConnector2_SBOM.zip'
Set-Content $archive 'archive fixture'
$global:apcAttestationCalls = [Collections.Generic.List[object]]::new()
$global:apcAttestationFailPredicate = ''
$global:apcAttestationLocalFailure = $false
function global:gh {
    $global:apcAttestationCalls.Add(@($args))
    $global:LASTEXITCODE = if ($global:apcAttestationFailPredicate -and $args -contains $global:apcAttestationFailPredicate) { 1 } else { 0 }
}
# Local SBOM contents have their own fixture tests; this test exercises orchestration and policy arguments.
function global:python {
    if ($args[0] -notlike '*verify-sbom.py') { throw 'Unexpected Python command' }
    $global:LASTEXITCODE = if ($global:apcAttestationLocalFailure) { 1 } else { 0 }
}
$parameters = @{
    Repository = 'example/product'
    Tag = 'v1.2.3'
    ExpectedCommit = '0123456789012345678901234567890123456789'
    ArtifactDirectory = $root
}
$verify = Join-Path $PSScriptRoot 'release/verify-build-attestations.ps1'
try {
    & $verify @parameters
    if ($global:apcAttestationCalls.Count -ne 4) { throw 'Expected three provenance checks and one package SBOM check.' }
    foreach ($call in $global:apcAttestationCalls) {
        foreach ($pair in @(
            @('--repo', $parameters.Repository),
            @('--source-ref', 'refs/tags/v1.2.3'),
            @('--source-digest', $parameters.ExpectedCommit),
            @('--signer-digest', $parameters.ExpectedCommit),
            @('--signer-workflow', 'example/product/.github/workflows/build-release-artifacts.yml')
        )) {
            $index = [array]::IndexOf($call, $pair[0])
            if ($index -lt 0 -or $call[$index + 1] -cne $pair[1]) { throw "Missing policy argument: $($pair[0])" }
        }
        if ($call -notcontains '--deny-self-hosted-runners') { throw 'Runner policy missing.' }
    }
    foreach ($predicate in @('https://slsa.dev/provenance/v1', 'https://spdx.dev/Document')) {
        $global:apcAttestationFailPredicate = $predicate
        $failed = $false
        try { & $verify @parameters } catch { $failed = $true }
        if (-not $failed) { throw "Accepted failed attestation: $predicate" }
    }
    $global:apcAttestationFailPredicate = ''
    $global:apcAttestationLocalFailure = $true
    $global:apcAttestationCalls.Clear()
    $failed = $false
    try { & $verify @parameters } catch { $failed = $true }
    if (-not $failed -or $global:apcAttestationCalls.Count -ne 0) { throw 'Local verification did not stop remote lookup.' }
    $global:apcAttestationLocalFailure = $false
    Remove-Item -LiteralPath $archive
    $failed = $false
    try { & $verify @parameters } catch { $failed = $true }
    if (-not $failed -or $global:apcAttestationCalls.Count -ne 0) { throw 'Missing archive did not stop remote lookup.' }
    Write-Output 'Attestation policy and rejection tests passed without network access.'
} finally {
    Remove-Item Function:/gh, Function:/python
    Remove-Variable -Scope Global -Name apcAttestationCalls,apcAttestationFailPredicate,apcAttestationLocalFailure
    $resolved = [IO.Path]::GetFullPath($root)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
