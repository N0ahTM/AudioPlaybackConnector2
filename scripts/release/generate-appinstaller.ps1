param(
    [Parameter(Mandatory = $true)]
    [string]$MsixUrl,

    [Parameter(Mandatory = $true)]
    [string]$AppInstallerUrl,

    [string]$ManifestPath = "AudioPlaybackConnector2 (Package)/Package.appxmanifest",
    [string]$PackageProjectPath = "AudioPlaybackConnector2 (Package)/AudioPlaybackConnector2 (Package).wapproj",
    [string]$OutputPath = (Join-Path ([System.IO.Path]::GetTempPath()) "AudioPlaybackConnector2.appinstaller"),
    [string]$ProcessorArchitecture = "x64"
)

$ErrorActionPreference = "Stop"

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

[xml]$manifest = [System.IO.File]::ReadAllText($ManifestPath)
$identity = $manifest.Package.Identity
$hoursBetweenUpdateChecks = Get-HoursBetweenUpdateChecks -Path $PackageProjectPath

$outputDirectory = Split-Path -Parent $OutputPath
if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

$appInstallerNs = "http://schemas.microsoft.com/appx/appinstaller/2017/2"
$s4Ns = "http://schemas.microsoft.com/appx/appinstaller/2018"
$settings = [System.Xml.XmlWriterSettings]::new()
$settings.Encoding = [System.Text.UTF8Encoding]::new($false)
$settings.Indent = $true

$writer = [System.Xml.XmlWriter]::Create($OutputPath, $settings)
try {
    $writer.WriteStartDocument()
    $writer.WriteStartElement("AppInstaller", $appInstallerNs)
    $writer.WriteAttributeString("xmlns", "s4", $null, $s4Ns)
    $writer.WriteAttributeString("Version", $identity.Version)
    $writer.WriteAttributeString("Uri", $AppInstallerUrl)

    $writer.WriteStartElement("MainPackage", $appInstallerNs)
    $writer.WriteAttributeString("Name", $identity.Name)
    $writer.WriteAttributeString("Publisher", $identity.Publisher)
    $writer.WriteAttributeString("Version", $identity.Version)
    $writer.WriteAttributeString("ProcessorArchitecture", $ProcessorArchitecture)
    $writer.WriteAttributeString("Uri", $MsixUrl)
    $writer.WriteEndElement()

    $writer.WriteStartElement("UpdateSettings", $appInstallerNs)
    $writer.WriteStartElement("OnLaunch", $appInstallerNs)
    $writer.WriteAttributeString("HoursBetweenUpdateChecks", $hoursBetweenUpdateChecks)
    $writer.WriteEndElement()
    $writer.WriteStartElement("s4", "AutomaticBackgroundTask", $s4Ns)
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
    Path = $OutputPath
    Version = $identity.Version
    Name = $identity.Name
    Publisher = $identity.Publisher
    HoursBetweenUpdateChecks = $hoursBetweenUpdateChecks
    MsixUrl = $MsixUrl
    AppInstallerUrl = $AppInstallerUrl
}
