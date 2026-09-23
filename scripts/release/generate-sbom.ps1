[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DropPath,
    [Parameter(Mandatory)][string]$Tag,
    [ValidateSet('x64-windows-static-md', 'arm64-windows-static-md')]
    [string[]]$Triplets = @('x64-windows-static-md', 'arm64-windows-static-md'),
    [string]$ToolPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ReleaseVersion.psm1') -Force
$version = Resolve-ReleaseVersion -Tag $Tag
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$drop = (Resolve-Path -LiteralPath $DropPath).Path
if (Test-Path -LiteralPath (Join-Path $drop '_manifest')) { throw 'Drop already contains a manifest; use a fresh release staging directory.' }
if (Test-Path -LiteralPath (Join-Path $drop 'SBOM')) { throw 'Drop already contains SBOM metadata; use a fresh release staging directory.' }
$pin = Get-Content (Join-Path $root 'config/sbom-tool.json') -Raw | ConvertFrom-Json
if (-not $ToolPath) {
    $ToolPath = Join-Path ([IO.Path]::GetTempPath()) "apc-sbom-tool-$($pin.version).exe"
    if (-not (Test-Path -LiteralPath $ToolPath)) { Invoke-WebRequest -Uri $pin.url -OutFile $ToolPath }
}
if ((Get-FileHash -LiteralPath $ToolPath -Algorithm SHA256).Hash -ine $pin.sha256) { throw 'sbom-tool SHA256 does not match the reviewed pin.' }
$components = Join-Path $drop 'SBOM'
New-Item -ItemType Directory -Path $components | Out-Null
# Keep only actual project dependency metadata; benchmark manifests are not product inputs.
$configs = @(git -C $root ls-files '**/packages.config')
if ($LASTEXITCODE -ne 0 -or $configs.Count -eq 0) { throw 'Could not enumerate project NuGet metadata.' }
foreach ($relative in $configs) {
    $destination = Join-Path $components $relative
    New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $destination
}
$inventory = (Get-Content (Join-Path $root 'config/dependency-licenses.json') -Raw | ConvertFrom-Json).vcpkg
foreach ($triplet in $Triplets) {
    & python (Join-Path $root 'scripts/verify-dependency-licenses.py') --triplet $triplet --root $root
    if ($LASTEXITCODE -ne 0) { throw "Dependency review failed: $triplet" }
    foreach ($entry in $inventory) {
        $architecture = if ($entry.scope -eq 'product') { $triplet } else { 'x64-windows' }
        $relative = "SBOM/vcpkg/$triplet/$architecture/$($entry.name)/vcpkg.spdx.json"
        $source = Join-Path $root "vcpkg_installed/$triplet/$architecture/share/$($entry.name)/vcpkg.spdx.json"
        $destination = Join-Path $drop $relative
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination
    }
}
Copy-Item -LiteralPath (Join-Path $root 'config/dependency-licenses.json') -Destination (Join-Path $drop 'SBOM/dependency-licenses.json')
& $ToolPath generate -b $drop -bc $components -pn AudioPlaybackConnector2 -pv $version.SemVer `
    -ps 'Person: Noah Meyer' -nsb 'https://github.com/n0ahtm/AudioPlaybackConnector2' -mi SPDX:2.2 -V Warning
if ($LASTEXITCODE -ne 0) { throw 'SBOM generation failed.' }
& python (Join-Path $PSScriptRoot 'verify-sbom.py') --drop $drop
if ($LASTEXITCODE -ne 0) { throw 'SBOM contents or hashes do not match the release drop.' }
$archive = Join-Path $drop '_manifest/AudioPlaybackConnector2_SBOM.zip'
Compress-Archive -Path (Join-Path $drop 'SBOM'), (Join-Path $drop '_manifest/spdx_2.2') -DestinationPath $archive -CompressionLevel Optimal
Write-Output "SBOM verified: $drop/_manifest/spdx_2.2/manifest.spdx.json"
