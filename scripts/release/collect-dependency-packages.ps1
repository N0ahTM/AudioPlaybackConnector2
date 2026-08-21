param(
    [Parameter(Mandatory = $true)]
    [string]$DependenciesDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateSet('x64', 'arm64')]
    [string[]]$Architectures = @('x64', 'arm64')
)

$ErrorActionPreference = 'Stop'

$packages = @(& (Join-Path $PSScriptRoot 'verify-dependency-packages.ps1') `
        -DependenciesDirectory $DependenciesDirectory `
        -ExpectedProcessorArchitectures $Architectures `
        -IgnoreOtherArchitectures)

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$staged = foreach ($package in $packages) {
    $architecture = $package.ProcessorArchitecture.ToLowerInvariant()
    $extension = [System.IO.Path]::GetExtension($package.FileName)
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($package.FileName)
    if ($baseName -notmatch "(?i)(^|[._-])$([regex]::Escape($architecture))([._-]|$)") {
        $baseName += ".$architecture"
    }
    $architectureDirectory = Join-Path $OutputDirectory $architecture
    New-Item -ItemType Directory -Path $architectureDirectory -Force | Out-Null
    $destination = Join-Path $architectureDirectory ($baseName + $extension)
    if (Test-Path -LiteralPath $destination) {
        throw "Dependency staging collision for '$destination'."
    }
    Copy-Item -LiteralPath $package.Path -Destination $destination
    [pscustomobject]@{
        Path                  = (Resolve-Path -LiteralPath $destination).Path
        FileName              = [System.IO.Path]::GetFileName($destination)
        Name                  = $package.Name
        Version               = $package.Version
        ProcessorArchitecture = $architecture
    }
}

$staged
