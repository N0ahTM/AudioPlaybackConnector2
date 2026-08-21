param(
    [Parameter(Mandatory = $true)]
    [string]$DependenciesDirectory,

    [ValidateSet('x86', 'x64', 'arm64')]
    [string]$ExpectedProcessorArchitecture = 'x64'
)

$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'PackageVerification.psm1') -Force

$expectedPublisher = 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US'
$allowedNames = @(
    'Microsoft.VCLibs.140.00',
    'Microsoft.VCLibs.140.00.UWPDesktop',
    'Microsoft.WindowsAppRuntime.2'
)

$resolvedDirectory = (Resolve-Path -LiteralPath $DependenciesDirectory).Path
$packages = @(Get-ChildItem -LiteralPath $resolvedDirectory -Recurse -File |
    Where-Object { $_.Extension -in '.appx', '.msix' })
if ($packages.Count -eq 0) {
    throw "No dependency packages found in '$resolvedDirectory'."
}

$duplicateFileNames = @($packages | Group-Object Name | Where-Object Count -gt 1)
if ($duplicateFileNames.Count -gt 0) {
    throw "Duplicate dependency filenames found: $($duplicateFileNames.Name -join ', ')."
}

$identities = foreach ($packagePath in $packages) {
    $package = Read-AppPackage -Path $packagePath.FullName -RequireSignature
    $identity = $package.Metadata
    if ($allowedNames -notcontains $identity.Name) {
        throw "Unexpected dependency package identity '$($identity.Name)' in '$($packagePath.Name)'."
    }
    if ($identity.Publisher -ne $expectedPublisher) {
        throw "Unexpected dependency publisher in '$($packagePath.Name)'."
    }
    if ($identity.ProcessorArchitecture -ne $ExpectedProcessorArchitecture) {
        throw "Unexpected dependency architecture '$($identity.ProcessorArchitecture)' in '$($packagePath.Name)'."
    }

    $signer = Test-AppPackageIntegrity `
        -PackagePath $identity.Path `
        -SignatureBytes $package.SignatureBytes `
        -ExpectedPublisher $identity.Publisher

    [pscustomobject]@{
        FileName              = $packagePath.Name
        Name                  = $identity.Name
        Publisher             = $identity.Publisher
        Version               = $identity.Version
        ProcessorArchitecture = $identity.ProcessorArchitecture
        SignerThumbprint      = $signer.Thumbprint
    }
}

$duplicateIdentities = @($identities | Group-Object Name | Where-Object Count -gt 1)
if ($duplicateIdentities.Count -gt 0) {
    throw "Duplicate dependency identities found: $($duplicateIdentities.Name -join ', ')."
}

$identities
