param(
    [Parameter(Mandatory = $true)]
    [string]$DependenciesDirectory,

    [ValidateSet('x86', 'x64', 'arm64')]
    [string]$ExpectedProcessorArchitecture = 'x64'
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Security

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

$identities = foreach ($package in $packages) {
    $zip = [System.IO.Compression.ZipFile]::OpenRead($package.FullName)
    try {
        $manifestEntry = $zip.GetEntry('AppxManifest.xml')
        $signatureEntry = $zip.GetEntry('AppxSignature.p7x')
        if (-not $manifestEntry -or -not $signatureEntry) {
            throw "Dependency '$($package.Name)' is missing its manifest or signature."
        }

        $manifestReader = [System.IO.StreamReader]::new($manifestEntry.Open())
        try {
            [xml]$manifest = $manifestReader.ReadToEnd()
        } finally {
            $manifestReader.Dispose()
        }

        $signatureStream = $signatureEntry.Open()
        $signatureMemory = [System.IO.MemoryStream]::new()
        try {
            $signatureStream.CopyTo($signatureMemory)
            $signatureBytes = $signatureMemory.ToArray()
        } finally {
            $signatureMemory.Dispose()
            $signatureStream.Dispose()
        }
    } finally {
        $zip.Dispose()
    }

    $identity = $manifest.Package.Identity
    if ($allowedNames -notcontains [string]$identity.Name) {
        throw "Unexpected dependency package identity '$($identity.Name)' in '$($package.Name)'."
    }
    if ([string]$identity.Publisher -ne $expectedPublisher) {
        throw "Unexpected dependency publisher in '$($package.Name)'."
    }
    if ([string]$identity.ProcessorArchitecture -ne $ExpectedProcessorArchitecture) {
        throw "Unexpected dependency architecture '$($identity.ProcessorArchitecture)' in '$($package.Name)'."
    }
    if ($signatureBytes.Length -le 4 -or
        [System.Text.Encoding]::ASCII.GetString($signatureBytes, 0, 4) -ne 'PKCX') {
        throw "Dependency '$($package.Name)' has an invalid signature header."
    }

    $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
    $cms.Decode($signatureBytes[4..($signatureBytes.Length - 1)])
    $cms.CheckSignature($true)
    $signers = @($cms.SignerInfos | ForEach-Object { $_.Certificate })
    if ($signers.Count -ne 1 -or $signers[0].Subject -ne [string]$identity.Publisher) {
        throw "Dependency signer does not match the manifest publisher in '$($package.Name)'."
    }

    [pscustomobject]@{
        FileName              = $package.Name
        Name                  = [string]$identity.Name
        Publisher             = [string]$identity.Publisher
        Version               = [string]$identity.Version
        ProcessorArchitecture = [string]$identity.ProcessorArchitecture
        SignerThumbprint      = $signers[0].Thumbprint
    }
}

$duplicateIdentities = @($identities | Group-Object Name | Where-Object Count -gt 1)
if ($duplicateIdentities.Count -gt 0) {
    throw "Duplicate dependency identities found: $($duplicateIdentities.Name -join ', ')."
}

$identities
