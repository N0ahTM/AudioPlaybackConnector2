Set-StrictMode -Version 3.0

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Security

function Read-AppPackage {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [switch]$RequireSignature
    )

    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $signatureBytes = $null
    $zip = [System.IO.Compression.ZipFile]::OpenRead($resolvedPath)
    try {
        $manifestEntry = $zip.GetEntry('AppxManifest.xml')
        if (-not $manifestEntry) {
            throw "No AppxManifest.xml found in '$resolvedPath'."
        }

        $reader = [System.IO.StreamReader]::new($manifestEntry.Open())
        try {
            [xml]$manifest = $reader.ReadToEnd()
        } finally {
            $reader.Dispose()
        }

        if ($RequireSignature) {
            $signatureEntry = $zip.GetEntry('AppxSignature.p7x')
            if (-not $signatureEntry) {
                throw "Package '$resolvedPath' is not signed."
            }
            $stream = $signatureEntry.Open()
            $memory = [System.IO.MemoryStream]::new()
            try {
                $stream.CopyTo($memory)
                $signatureBytes = $memory.ToArray()
            } finally {
                $memory.Dispose()
                $stream.Dispose()
            }
        }

        $entryNames = @($zip.Entries | ForEach-Object { $_.FullName })
    } finally {
        $zip.Dispose()
    }

    $identity = $manifest.Package.Identity
    $metadata = [ordered]@{
        Path                  = $resolvedPath
        Name                  = [string]$identity.Name
        Publisher             = [string]$identity.Publisher
        Version               = [string]$identity.Version
        ProcessorArchitecture = [string]$identity.ProcessorArchitecture
    }
    foreach ($propertyName in @('Name', 'Publisher', 'Version', 'ProcessorArchitecture')) {
        if ([string]::IsNullOrWhiteSpace([string]$metadata[$propertyName])) {
            throw "Package identity property '$propertyName' is missing in '$resolvedPath'."
        }
    }

    [pscustomobject]@{
        Metadata       = [pscustomobject]$metadata
        EntryNames     = $entryNames
        SignatureBytes = $signatureBytes
    }
}

function Read-AppBundle {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [switch]$RequireSignature
    )

    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $signatureBytes = $null
    $zip = [System.IO.Compression.ZipFile]::OpenRead($resolvedPath)
    try {
        $manifestEntry = $zip.GetEntry('AppxMetadata/AppxBundleManifest.xml')
        if (-not $manifestEntry) {
            throw "No AppxMetadata/AppxBundleManifest.xml found in '$resolvedPath'."
        }

        $reader = [System.IO.StreamReader]::new($manifestEntry.Open())
        try {
            [xml]$manifest = $reader.ReadToEnd()
        } finally {
            $reader.Dispose()
        }

        if ($RequireSignature) {
            $signatureEntry = $zip.GetEntry('AppxSignature.p7x')
            if (-not $signatureEntry) {
                throw "Bundle '$resolvedPath' is not signed."
            }
            $stream = $signatureEntry.Open()
            $memory = [System.IO.MemoryStream]::new()
            try {
                $stream.CopyTo($memory)
                $signatureBytes = $memory.ToArray()
            } finally {
                $memory.Dispose()
                $stream.Dispose()
            }
        }

        $entryNames = @($zip.Entries | ForEach-Object { $_.FullName })
    } finally {
        $zip.Dispose()
    }

    $identity = $manifest.Bundle.Identity
    $applicationPackages = @($manifest.Bundle.Packages.Package | Where-Object Type -eq 'application')
    $metadata = [ordered]@{
        Path                     = $resolvedPath
        Name                     = [string]$identity.Name
        Publisher                = [string]$identity.Publisher
        Version                  = [string]$identity.Version
        ApplicationArchitectures = @($applicationPackages | ForEach-Object { [string]$_.Architecture })
        ApplicationPackages      = @($applicationPackages | ForEach-Object { [string]$_.FileName })
    }
    foreach ($propertyName in @('Name', 'Publisher', 'Version')) {
        if ([string]::IsNullOrWhiteSpace([string]$metadata[$propertyName])) {
            throw "Bundle identity property '$propertyName' is missing in '$resolvedPath'."
        }
    }
    if ($metadata.ApplicationArchitectures.Count -eq 0) {
        throw "Bundle '$resolvedPath' contains no application packages."
    }

    [pscustomobject]@{
        Metadata       = [pscustomobject]$metadata
        EntryNames     = $entryNames
        SignatureBytes = $signatureBytes
    }
}

function Get-AppPackageSigner {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$SignatureBytes,

        [Parameter(Mandatory = $true)]
        [string]$PackagePath
    )

    if ($SignatureBytes.Length -le 4 -or
        [System.Text.Encoding]::ASCII.GetString($SignatureBytes, 0, 4) -ne 'PKCX') {
        throw "Package '$PackagePath' has an invalid signature header."
    }

    $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
    $cms.Decode($SignatureBytes[4..($SignatureBytes.Length - 1)])
    $cms.CheckSignature($true)
    $signers = @($cms.SignerInfos | ForEach-Object { $_.Certificate })
    if ($signers.Count -ne 1 -or -not $signers[0]) {
        throw "Package '$PackagePath' must have exactly one signer."
    }
    return $signers[0]
}

function Find-MakeAppx {
    [CmdletBinding()]
    param()

    $makeAppx = Get-Command makeappx.exe -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source -First 1
    if (-not $makeAppx) {
        $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
        $makeAppx = Get-ChildItem -Path $kitsRoot -Filter makeappx.exe -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object FullName -match '[\\/]x64[\\/]makeappx\.exe$' |
            Sort-Object FullName -Descending |
            Select-Object -ExpandProperty FullName -First 1
    }
    if (-not $makeAppx) {
        throw 'makeappx.exe was not found.'
    }
    return $makeAppx
}

function Test-AppPackageIntegrity {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackagePath,

        [Parameter(Mandatory = $true)]
        [byte[]]$SignatureBytes,

        [string]$ExpectedCertificatePath = '',
        [string]$ExpectedPublisher = ''
    )

    $signer = Get-AppPackageSigner -SignatureBytes $SignatureBytes -PackagePath $PackagePath
    $expectedCertificate = $null
    if (-not [string]::IsNullOrWhiteSpace($ExpectedCertificatePath)) {
        $expectedCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
            (Resolve-Path -LiteralPath $ExpectedCertificatePath).Path)
        if (-not [string]::Equals($signer.Thumbprint, $expectedCertificate.Thumbprint,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Package signer does not match certificate '$ExpectedCertificatePath'."
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPublisher) -and
        -not [string]::Equals($signer.Subject, $ExpectedPublisher,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Package signer '$($signer.Subject)' does not match publisher '$ExpectedPublisher'."
    }

    $makeAppx = Find-MakeAppx
    $validationDirectory = Join-Path ([System.IO.Path]::GetTempPath()) `
        ('apc2-package-validation-' + [guid]::NewGuid().ToString('N'))
    try {
        $operation = if ([System.IO.Path]::GetExtension($PackagePath) -in '.msixbundle', '.appxbundle') {
            'unbundle'
        } else {
            'unpack'
        }
        $makeAppxOutput = & $makeAppx $operation /p $PackagePath /d $validationDirectory /o 2>&1
        if ($LASTEXITCODE -ne 0) {
            $details = @($makeAppxOutput | Select-Object -Last 50) -join "`n"
            throw "MakeAppx integrity verification failed for '$PackagePath':`n$details"
        }
    } finally {
        if (Test-Path -LiteralPath $validationDirectory) {
            $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
            $resolvedValidationDirectory = [System.IO.Path]::GetFullPath($validationDirectory)
            if (-not $resolvedValidationDirectory.StartsWith($tempRoot,
                    [System.StringComparison]::OrdinalIgnoreCase) -or
                [System.IO.Path]::GetFileName($resolvedValidationDirectory) -notlike
                    'apc2-package-validation-*') {
                throw "Refusing to remove unexpected validation directory '$resolvedValidationDirectory'."
            }
            Remove-Item -LiteralPath $resolvedValidationDirectory -Recurse -Force
        }
    }

    [pscustomobject]@{
        Thumbprint = $signer.Thumbprint
        Subject    = $signer.Subject
    }
}

Export-ModuleMember -Function Read-AppPackage, Read-AppBundle, Test-AppPackageIntegrity
