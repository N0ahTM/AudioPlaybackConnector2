param(
    [Parameter(Mandatory = $true)]
    [string]$MsixPath,

    [string]$ExpectedPackageVersion = "",
    [string]$ExpectedProcessorArchitecture = "",
    [string]$ExpectedName = "",
    [string]$ExpectedPublisher = "",
    [string[]]$RequiredEntries = @(),
    [string[]]$UniqueEntries = @(),
    [switch]$RequireSignature,
    [switch]$WriteGitHubOutput
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.IO.Compression.FileSystem

$resolvedPath = (Resolve-Path -LiteralPath $MsixPath).Path
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

    if ($RequireSignature -and -not $zip.GetEntry("AppxSignature.p7x")) {
        throw "MSIX package '$resolvedPath' is not signed."
    }
} finally {
    $zip.Dispose()
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
}

$metadata
