[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('apc-promotion-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$names = @('AudioPlaybackConnector2_1.2.3.0_x64_ARM64.msixbundle', 'AudioPlaybackConnector2.appinstaller',
    'AudioPlaybackConnector2.cer', 'framework.appx', 'AudioPlaybackConnector2_SBOM.zip')
foreach ($name in $names) { Set-Content (Join-Path $root $name) "fixture $name" }
$assets = @(Get-ChildItem -LiteralPath $root -File | ForEach-Object {
    @{ name = $_.Name; size = $_.Length; state = 'uploaded'; digest = 'sha256:' + (Get-FileHash $_.FullName).Hash.ToLowerInvariant() }
})
$global:apcPromotionRelease = @{ tagName = 'v1.2.3'; isDraft = $true; isPrerelease = $false; assets = $assets }
function global:gh { $global:LASTEXITCODE = 0; $global:apcPromotionRelease | ConvertTo-Json -Depth 8 }
$verify = Join-Path $PSScriptRoot 'release/verify-release-promotion.ps1'
$parameters = @{ Repository = 'example/product'; Tag = 'v1.2.3'; PackageVersion = '1.2.3.0'; ArtifactDirectory = $root }
try {
    $result = & $verify @parameters
    if ($result.AssetCount -ne 5) { throw 'SBOM archive was not included in promotion verification.' }
    Set-Content (Join-Path $root 'AudioPlaybackConnector2_SBOM.zip') 'changed archive'
    $failed = $false
    try { & $verify @parameters | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'Modified SBOM archive was accepted.' }
    Remove-Item -LiteralPath (Join-Path $root 'AudioPlaybackConnector2_SBOM.zip')
    $global:apcPromotionRelease.assets = @($assets | Where-Object name -ne 'AudioPlaybackConnector2_SBOM.zip')
    $failed = $false
    try { & $verify @parameters | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'Missing SBOM archive was accepted.' }
    Write-Output 'Release promotion archive checks passed without network access.'
} finally {
    Remove-Item Function:/gh
    Remove-Variable -Scope Global -Name apcPromotionRelease
    $resolved = [IO.Path]::GetFullPath($root)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
