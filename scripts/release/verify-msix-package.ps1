param(
    [Parameter(Mandatory = $true)]
    [string]$MsixPath,

    [string]$ExpectedPackageVersion = '',
    [string]$ExpectedProcessorArchitecture = '',
    [string]$ExpectedName = '',
    [string]$ExpectedPublisher = '',
    [string[]]$RequiredEntries = @(),
    [string[]]$UniqueEntries = @(),
    [string]$ExpectedCertificatePath = '',
    [switch]$RequireSignature,
    [switch]$WriteGitHubOutput
)

$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'PackageVerification.psm1') -Force

$package = Read-AppPackage -Path $MsixPath -RequireSignature:$RequireSignature
$metadata = $package.Metadata
$expectations = @(
    @{ Label = 'version'; Expected = $ExpectedPackageVersion; Actual = $metadata.Version },
    @{ Label = 'architecture'; Expected = $ExpectedProcessorArchitecture; Actual = $metadata.ProcessorArchitecture },
    @{ Label = 'name'; Expected = $ExpectedName; Actual = $metadata.Name },
    @{ Label = 'publisher'; Expected = $ExpectedPublisher; Actual = $metadata.Publisher }
)
foreach ($expectation in $expectations) {
    if (-not [string]::IsNullOrWhiteSpace($expectation.Expected) -and
        -not [string]::Equals($expectation.Expected, $expectation.Actual,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "MSIX $($expectation.Label) is '$($expectation.Actual)', expected '$($expectation.Expected)'."
    }
}

foreach ($entryName in $RequiredEntries) {
    $count = @($package.EntryNames | Where-Object {
            [string]::Equals($_, $entryName, [System.StringComparison]::OrdinalIgnoreCase)
        }).Count
    if ($count -eq 0) {
        throw "Required MSIX payload '$entryName' is missing."
    }
}

foreach ($entryName in $UniqueEntries) {
    $count = @($package.EntryNames | Where-Object {
            [string]::Equals($_, $entryName, [System.StringComparison]::OrdinalIgnoreCase)
        }).Count
    if ($count -ne 1) {
        throw "MSIX payload '$entryName' occurs $count times; expected exactly once."
    }
}

if ($RequireSignature) {
    if ([string]::IsNullOrWhiteSpace($ExpectedCertificatePath)) {
        throw 'ExpectedCertificatePath is required when RequireSignature is used.'
    }
    $signer = Test-AppPackageIntegrity `
        -PackagePath $metadata.Path `
        -SignatureBytes $package.SignatureBytes `
        -ExpectedCertificatePath $ExpectedCertificatePath `
        -ExpectedPublisher $metadata.Publisher
    $metadata | Add-Member -NotePropertyName SignerThumbprint -NotePropertyValue $signer.Thumbprint
}

if ($WriteGitHubOutput) {
    if ([string]::IsNullOrWhiteSpace($env:GITHUB_OUTPUT)) {
        throw 'GITHUB_OUTPUT is not available.'
    }
    "PATH=$($metadata.Path)" >> $env:GITHUB_OUTPUT
    "NAME=$($metadata.Name)" >> $env:GITHUB_OUTPUT
    "PUBLISHER=$($metadata.Publisher)" >> $env:GITHUB_OUTPUT
    "VERSION=$($metadata.Version)" >> $env:GITHUB_OUTPUT
    "ARCHITECTURE=$($metadata.ProcessorArchitecture)" >> $env:GITHUB_OUTPUT
    if ($metadata.PSObject.Properties.Name -contains 'SignerThumbprint') {
        "SIGNER_THUMBPRINT=$($metadata.SignerThumbprint)" >> $env:GITHUB_OUTPUT
    }
}

$metadata
