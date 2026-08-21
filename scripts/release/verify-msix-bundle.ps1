param(
    [Parameter(Mandatory = $true)]
    [string]$BundlePath,

    [string]$ExpectedPackageVersion = '',
    [string]$ExpectedName = '',
    [string]$ExpectedPublisher = '',
    [ValidateSet('x64', 'arm64')]
    [string[]]$ExpectedProcessorArchitectures = @('x64', 'arm64'),
    [string]$ExpectedCertificatePath = '',
    [switch]$RequireSignature,
    [switch]$WriteGitHubOutput
)

$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'PackageVerification.psm1') -Force

$bundle = Read-AppBundle -Path $BundlePath -RequireSignature:$RequireSignature
$metadata = $bundle.Metadata
$expectations = @(
    @{ Label = 'version'; Expected = $ExpectedPackageVersion; Actual = $metadata.Version },
    @{ Label = 'name'; Expected = $ExpectedName; Actual = $metadata.Name },
    @{ Label = 'publisher'; Expected = $ExpectedPublisher; Actual = $metadata.Publisher }
)
foreach ($expectation in $expectations) {
    if (-not [string]::IsNullOrWhiteSpace($expectation.Expected) -and
        -not [string]::Equals($expectation.Expected, $expectation.Actual,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "MSIX bundle $($expectation.Label) is '$($expectation.Actual)', expected '$($expectation.Expected)'."
    }
}

$expectedArchitectures = @($ExpectedProcessorArchitectures | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object -Unique)
$actualArchitectures = @($metadata.ApplicationArchitectures | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object -Unique)
if (($expectedArchitectures -join ',') -ne ($actualArchitectures -join ',')) {
    throw "MSIX bundle architectures are '$($actualArchitectures -join ',')', expected '$($expectedArchitectures -join ',')'."
}
if ($metadata.ApplicationPackages.Count -ne $expectedArchitectures.Count) {
    throw "MSIX bundle contains $($metadata.ApplicationPackages.Count) application packages; expected $($expectedArchitectures.Count)."
}

if ($RequireSignature) {
    if ([string]::IsNullOrWhiteSpace($ExpectedCertificatePath)) {
        throw 'ExpectedCertificatePath is required when RequireSignature is used.'
    }
    $signer = Test-AppPackageIntegrity `
        -PackagePath $metadata.Path `
        -SignatureBytes $bundle.SignatureBytes `
        -ExpectedCertificatePath $ExpectedCertificatePath `
        -ExpectedPublisher $metadata.Publisher
    $metadata | Add-Member -NotePropertyName SignerThumbprint -NotePropertyValue $signer.Thumbprint
}

$metadata | Add-Member -NotePropertyName ProcessorArchitectures -NotePropertyValue ($actualArchitectures -join ',')
if ($WriteGitHubOutput) {
    if ([string]::IsNullOrWhiteSpace($env:GITHUB_OUTPUT)) {
        throw 'GITHUB_OUTPUT is not available.'
    }
    "PATH=$($metadata.Path)" >> $env:GITHUB_OUTPUT
    "NAME=$($metadata.Name)" >> $env:GITHUB_OUTPUT
    "PUBLISHER=$($metadata.Publisher)" >> $env:GITHUB_OUTPUT
    "VERSION=$($metadata.Version)" >> $env:GITHUB_OUTPUT
    "ARCHITECTURES=$($metadata.ProcessorArchitectures)" >> $env:GITHUB_OUTPUT
    if ($metadata.PSObject.Properties.Name -contains 'SignerThumbprint') {
        "SIGNER_THUMBPRINT=$($metadata.SignerThumbprint)" >> $env:GITHUB_OUTPUT
    }
}

$metadata
