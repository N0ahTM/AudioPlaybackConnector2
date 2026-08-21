param(
    [Parameter(Mandatory = $true)]
    [Alias('MsixUrl')]
    [string]$PackageUrl,

    [Parameter(Mandatory = $true)]
    [string]$AppInstallerUrl,

    [Parameter(Mandatory = $true)]
    [Alias('MsixPath')]
    [string]$PackagePath,

    [string]$PackageProjectPath = "AudioPlaybackConnector2 (Package)/AudioPlaybackConnector2 (Package).wapproj",
    [string]$OutputPath = (Join-Path ([System.IO.Path]::GetTempPath()) "AudioPlaybackConnector2.appinstaller"),
    [string]$ExpectedPackageVersion = "",
    [string]$ProcessorArchitecture = "",

    [string]$DependenciesDirectory = "",
    [string]$DependencyBaseUrl = ""
)

$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot 'PackageVerification.psm1') -Force

function Get-HoursBetweenUpdateChecks {
    param([string]$Path)

    [xml]$project = [System.IO.File]::ReadAllText($Path)
    $namespaceManager = [System.Xml.XmlNamespaceManager]::new($project.NameTable)
    $namespaceManager.AddNamespace("msb", "http://schemas.microsoft.com/developer/msbuild/2003")

    $node = $project.SelectSingleNode("//msb:HoursBetweenUpdateChecks", $namespaceManager)
    if (-not $node) {
        $node = $project.SelectSingleNode("//*[local-name()='HoursBetweenUpdateChecks']")
    }

    if ($node -and -not [string]::IsNullOrWhiteSpace($node.InnerText)) {
        return $node.InnerText.Trim()
    }

    return "24"
}

$isBundle = [System.IO.Path]::GetExtension($PackagePath) -ieq '.msixbundle'
if ($isBundle) {
    $metadataParams = @{ BundlePath = $PackagePath }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPackageVersion)) {
        $metadataParams['ExpectedPackageVersion'] = $ExpectedPackageVersion
    }
    $identity = & (Join-Path $PSScriptRoot 'verify-msix-bundle.ps1') @metadataParams
} else {
    $metadataParams = @{ MsixPath = $PackagePath }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPackageVersion)) {
        $metadataParams['ExpectedPackageVersion'] = $ExpectedPackageVersion
    }
    if (-not [string]::IsNullOrWhiteSpace($ProcessorArchitecture)) {
        $metadataParams['ExpectedProcessorArchitecture'] = $ProcessorArchitecture
    }
    $identity = & (Join-Path $PSScriptRoot 'verify-msix-package.ps1') @metadataParams
}
$hoursBetweenUpdateChecks = Get-HoursBetweenUpdateChecks -Path $PackageProjectPath

$outputDirectory = Split-Path -Parent $OutputPath
if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

$appInstallerNs = "http://schemas.microsoft.com/appx/appinstaller/2017/2"
$settings = [System.Xml.XmlWriterSettings]::new()
$settings.Encoding = [System.Text.UTF8Encoding]::new($false)
$settings.Indent = $true

$writer = [System.Xml.XmlWriter]::Create($OutputPath, $settings)
try {
    $writer.WriteStartDocument()
    $writer.WriteStartElement("AppInstaller", $appInstallerNs)
    $writer.WriteAttributeString("Version", $identity.Version)
    $writer.WriteAttributeString("Uri", $AppInstallerUrl)

    $mainElementName = if ($isBundle) { 'MainBundle' } else { 'MainPackage' }
    $writer.WriteStartElement($mainElementName, $appInstallerNs)
    $writer.WriteAttributeString("Name", $identity.Name)
    $writer.WriteAttributeString("Publisher", $identity.Publisher)
    $writer.WriteAttributeString("Version", $identity.Version)
    if (-not $isBundle) {
        $writer.WriteAttributeString("ProcessorArchitecture", $identity.ProcessorArchitecture)
    }
    $writer.WriteAttributeString("Uri", $PackageUrl)
    $writer.WriteEndElement()

    if (-not [string]::IsNullOrWhiteSpace($DependenciesDirectory) -and (Test-Path $DependenciesDirectory)) {
        $deps = Get-ChildItem -Path $DependenciesDirectory -Recurse -Include @("*.appx", "*.msix") |
            Sort-Object Name
        if ($deps) {
            $writer.WriteStartElement("Dependencies", $appInstallerNs)
            foreach ($dep in $deps) {
                $meta = (Read-AppPackage -Path $dep.FullName).Metadata
                $depUrl = if (-not [string]::IsNullOrWhiteSpace($DependencyBaseUrl)) {
                    "$($DependencyBaseUrl.TrimEnd('/'))/$($dep.Name)"
                } else {
                    $dep.Name
                }
                $writer.WriteStartElement("Package", $appInstallerNs)
                $writer.WriteAttributeString("Name", $meta.Name)
                $writer.WriteAttributeString("Publisher", $meta.Publisher)
                $writer.WriteAttributeString("ProcessorArchitecture", $meta.ProcessorArchitecture)
                $writer.WriteAttributeString("Uri", $depUrl)
                $writer.WriteAttributeString("Version", $meta.Version)
                $writer.WriteEndElement()
            }
            $writer.WriteEndElement()
        }
    }

    $writer.WriteStartElement("UpdateSettings", $appInstallerNs)
    $writer.WriteStartElement("OnLaunch", $appInstallerNs)
    $writer.WriteAttributeString("HoursBetweenUpdateChecks", $hoursBetweenUpdateChecks)
    $writer.WriteEndElement()
    $writer.WriteEndElement()

    $writer.WriteEndElement()
    $writer.WriteEndDocument()
} finally {
    $writer.Close()
}

if ($env:GITHUB_OUTPUT) {
    "PATH=$OutputPath" >> $env:GITHUB_OUTPUT
}

[pscustomobject]@{
    Path                     = $OutputPath
    Version                  = $identity.Version
    PackageVersion           = $identity.Version
    Name                     = $identity.Name
    Publisher                = $identity.Publisher
    HoursBetweenUpdateChecks = $hoursBetweenUpdateChecks
    PackageUrl               = $PackageUrl
    PackageType              = if ($isBundle) { 'MainBundle' } else { 'MainPackage' }
    AppInstallerUrl          = $AppInstallerUrl
}
