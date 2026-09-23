Set-StrictMode -Version Latest

function Resolve-ReleaseVersion {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)] [string]$Tag)

    if ($Tag -cnotmatch '\Av(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\z') {
        throw 'Release tag must be canonical vX.Y.Z with three MSIX-compatible numeric components.'
    }
    $semver = $Tag.Substring(1)
    foreach ($component in $semver.Split('.')) {
        if ([uint32]$component -gt [uint16]::MaxValue) {
            throw 'A release version component exceeds 65535.'
        }
    }
    [pscustomobject]@{ Tag = $Tag; SemVer = $semver; PackageVersion = "$semver.0" }
}

Export-ModuleMember -Function Resolve-ReleaseVersion
