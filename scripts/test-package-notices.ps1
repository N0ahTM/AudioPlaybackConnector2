$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'release/PackageVerification.psm1') -Force

function New-ZipBytes([object[]]$Entries) {
    $memory = [IO.MemoryStream]::new()
    try {
        $zip = [IO.Compression.ZipArchive]::new($memory, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($entry in $Entries) {
                $stream = $zip.CreateEntry($entry.Name).Open()
                try { $stream.Write($entry.Bytes, 0, $entry.Bytes.Length) }
                finally { $stream.Dispose() }
            }
        } finally { $zip.Dispose() }
        return ,$memory.ToArray()
    } finally { $memory.Dispose() }
}

$testPath = Join-Path ([IO.Path]::GetTempPath()) ('apc-notices-' + [guid]::NewGuid().ToString('N') + '.msixbundle')
try {
    $license = @{ Name = 'LICENSE'; Bytes = [IO.File]::ReadAllBytes((Join-Path $repository 'LICENSE')) }
    $notice = @{ Name = 'THIRD_PARTY_NOTICES.md'; Bytes = [IO.File]::ReadAllBytes((Join-Path $repository 'THIRD_PARTY_NOTICES.md')) }
    $validPackage = New-ZipBytes @($license, $notice)
    $manifest = @{
        Name = 'AppxMetadata/AppxBundleManifest.xml'
        Bytes = [Text.Encoding]::UTF8.GetBytes(@'
<Bundle><Identity Name="Test" Publisher="CN=Test" Version="1.2.3.0"/><Packages>
<Package Type="application" Architecture="x64" FileName="x64.msix"/>
<Package Type="application" Architecture="arm64" FileName="arm64 test.msix"/>
</Packages></Bundle>
'@)
    }
    foreach ($scenario in @('valid', 'missing-notice', 'changed-notice', 'duplicate-notice', 'missing-package', 'duplicate-package', 'encoded-alias')) {
        $armPackage = switch ($scenario) {
            'missing-notice' { New-ZipBytes @($license) }
            'changed-notice' { New-ZipBytes @($license, @{ Name = $notice.Name; Bytes = [byte[]]@(1, 2, 3) }) }
            'duplicate-notice' { New-ZipBytes @($license, $notice, $notice) }
            default { ,$validPackage }
        }
        $entries = @($manifest, @{ Name = 'x64.msix'; Bytes = $validPackage })
        if ($scenario -ne 'missing-package') { $entries += @{ Name = 'arm64%20test.msix'; Bytes = [byte[]]$armPackage } }
        if ($scenario -eq 'duplicate-package') { $entries += @{ Name = 'arm64%20test.msix'; Bytes = $validPackage } }
        if ($scenario -eq 'encoded-alias') { $entries += @{ Name = 'arm64 test.msix'; Bytes = $validPackage } }
        [IO.File]::WriteAllBytes($testPath, (New-ZipBytes $entries))
        $failure = $null
        try { Assert-AppBundleNotices -BundlePath $testPath -SourceDirectory $repository }
        catch { $failure = $_.Exception.Message }
        if ($scenario -eq 'valid') {
            if ($failure) { throw "Valid bundle rejected: $failure" }
        } else {
            $expected = if ($scenario -eq 'changed-notice') { 'differs from the release source' } else { 'must occur exactly once' }
            if (-not $failure -or $failure -notlike "*$expected*") { throw "Scenario '$scenario' failed to reject its intended defect: $failure" }
        }
    }
    $app = @{ Name = 'AudioPlaybackConnector2.exe'; Bytes = [byte[]]@(1, 2, 3) }
    $cli = @{ Name = 'AudioPlaybackConnector2.Control/AudioPlaybackConnector2.Control.exe'; Bytes = [byte[]]@(4, 5, 6) }
    $referencePackage = New-ZipBytes @($app, $cli)
    $referencePath = $testPath + '.reference'
    [IO.File]::WriteAllBytes($referencePath, (New-ZipBytes @($manifest,
        @{ Name = 'x64.msix'; Bytes = $referencePackage },
        @{ Name = 'arm64%20test.msix'; Bytes = $referencePackage })))
    try {
        foreach ($scenario in @('same', 'changed-app', 'missing-cli', 'duplicate-app')) {
            $candidate = switch ($scenario) {
                'changed-app' { New-ZipBytes @(@{ Name = $app.Name; Bytes = [byte[]]@(9) }, $cli) }
                'missing-cli' { New-ZipBytes @($app) }
                'duplicate-app' { New-ZipBytes @($app, $app, $cli) }
                default { ,$referencePackage }
            }
            [IO.File]::WriteAllBytes($testPath, (New-ZipBytes @($manifest,
                @{ Name = 'x64.msix'; Bytes = $referencePackage },
                @{ Name = 'arm64%20test.msix'; Bytes = [byte[]]$candidate })))
            $failure = $null
            try { Assert-AppBundleBinaries -BundlePath $testPath -ReferenceBundlePath $referencePath }
            catch { $failure = $_.Exception.Message }
            if ($scenario -eq 'same' -and $failure) { throw "Same binaries rejected: $failure" }
            if ($scenario -ne 'same' -and -not $failure) { throw "Invalid binaries accepted: $scenario" }
        }
    } finally { Remove-Item -LiteralPath $referencePath -Force }
} finally {
    Remove-Item -LiteralPath $testPath -Force -ErrorAction SilentlyContinue
}
Write-Host 'Package binary identity cases passed. Package notices: valid two-architecture bundle and all six negative cases passed.'
