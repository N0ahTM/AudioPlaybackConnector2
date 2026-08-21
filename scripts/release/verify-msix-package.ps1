param(
    [Parameter(Mandatory = $true)]
    [string]$MsixPath,

    [string]$ExpectedPackageVersion = "",
    [string]$ExpectedProcessorArchitecture = "",
    [string]$ExpectedName = "",
    [string]$ExpectedPublisher = "",
    [string[]]$RequiredEntries = @(),
    [string[]]$UniqueEntries = @(),
    [string]$ExpectedCertificatePath = "",
    [switch]$RequireSignature,
    [switch]$WriteGitHubOutput
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Security

$resolvedPath = (Resolve-Path -LiteralPath $MsixPath).Path
$signatureBytes = $null
$zip = [System.IO.Compression.ZipFile]::OpenRead($resolvedPath)
try {
    $manifestEntry = $zip.GetEntry("AppxManifest.xml")
    if (-not $manifestEntry) {
        throw "No AppxManifest.xml found in '$resolvedPath'."
    }

    $stream = $manifestEntry.Open()
    $reader = [System.IO.StreamReader]::new($stream)
    try {
        [xml]$manifest = $reader.ReadToEnd()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }

    $identity = $manifest.Package.Identity
    $metadata = [pscustomobject]@{
        Path                  = $resolvedPath
        Name                  = [string]$identity.Name
        Publisher             = [string]$identity.Publisher
        Version               = [string]$identity.Version
        ProcessorArchitecture = [string]$identity.ProcessorArchitecture
    }

    foreach ($propertyName in @("Name", "Publisher", "Version", "ProcessorArchitecture")) {
        if ([string]::IsNullOrWhiteSpace([string]$metadata.$propertyName)) {
            throw "MSIX identity property '$propertyName' is missing in '$resolvedPath'."
        }
    }

    $expectations = @(
        @{ Label = "version"; Expected = $ExpectedPackageVersion; Actual = $metadata.Version },
        @{ Label = "architecture"; Expected = $ExpectedProcessorArchitecture; Actual = $metadata.ProcessorArchitecture },
        @{ Label = "name"; Expected = $ExpectedName; Actual = $metadata.Name },
        @{ Label = "publisher"; Expected = $ExpectedPublisher; Actual = $metadata.Publisher }
    )
    foreach ($expectation in $expectations) {
        if (-not [string]::IsNullOrWhiteSpace($expectation.Expected) -and
            -not [string]::Equals($expectation.Expected, $expectation.Actual, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "MSIX $($expectation.Label) is '$($expectation.Actual)', expected '$($expectation.Expected)'."
        }
    }

    foreach ($entryName in $RequiredEntries) {
        $count = @($zip.Entries | Where-Object {
                [string]::Equals($_.FullName, $entryName, [System.StringComparison]::OrdinalIgnoreCase)
            }).Count
        if ($count -eq 0) {
            throw "Required MSIX payload '$entryName' is missing."
        }
    }

    foreach ($entryName in $UniqueEntries) {
        $count = @($zip.Entries | Where-Object {
                [string]::Equals($_.FullName, $entryName, [System.StringComparison]::OrdinalIgnoreCase)
            }).Count
        if ($count -ne 1) {
            throw "MSIX payload '$entryName' occurs $count times; expected exactly once."
        }
    }

    if ($RequireSignature) {
        $signatureEntry = $zip.GetEntry("AppxSignature.p7x")
        if (-not $signatureEntry) {
            throw "MSIX package '$resolvedPath' is not signed."
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
    }
} finally {
    $zip.Dispose()
}

if ($RequireSignature) {
    if ([string]::IsNullOrWhiteSpace($ExpectedCertificatePath)) {
        throw "ExpectedCertificatePath is required when RequireSignature is used."
    }
    if ($signatureBytes.Length -le 4 -or
        [System.Text.Encoding]::ASCII.GetString($signatureBytes, 0, 4) -ne "PKCX") {
        throw "MSIX package '$resolvedPath' has an invalid signature header."
    }

    $expectedCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        (Resolve-Path -LiteralPath $ExpectedCertificatePath).Path)
    $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
    $cms.Decode($signatureBytes[4..($signatureBytes.Length - 1)])
    $cms.CheckSignature($true)
    $signerCertificates = @($cms.SignerInfos | ForEach-Object { $_.Certificate })
    if ($signerCertificates.Count -ne 1 -or
        $signerCertificates[0].Thumbprint -ne $expectedCertificate.Thumbprint) {
        throw "MSIX signer does not match certificate '$ExpectedCertificatePath'."
    }

    $signTool = Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
    if (-not $signTool) {
        $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
        $signTool = Get-ChildItem -Path $kitsRoot -Filter signtool.exe -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object FullName -match '[\\/]x64[\\/]signtool\.exe$' |
            Sort-Object FullName -Descending |
            Select-Object -ExpandProperty FullName -First 1
    }
    if (-not $signTool) { throw 'signtool.exe was not found.' }

    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'CurrentUser')
    $certificateAdded = $false
    $store.Open('ReadWrite')
    try {
        if ($store.Certificates.Find('FindByThumbprint', $expectedCertificate.Thumbprint, $false).Count -eq 0) {
            $store.Add($expectedCertificate)
            $certificateAdded = $true
        }
        $signToolOutput = & $signTool verify /pa /all /v $resolvedPath 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "SignTool verification failed for '$resolvedPath':`n$($signToolOutput -join "`n")"
        }
    } finally {
        if ($certificateAdded) {
            $store.Certificates.Find('FindByThumbprint', $expectedCertificate.Thumbprint, $false) |
                ForEach-Object { $store.Remove($_) }
        }
        $store.Close()
    }
    $metadata | Add-Member -NotePropertyName SignerThumbprint -NotePropertyValue $expectedCertificate.Thumbprint
}

if ($WriteGitHubOutput) {
    if ([string]::IsNullOrWhiteSpace($env:GITHUB_OUTPUT)) {
        throw "GITHUB_OUTPUT is not available."
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
