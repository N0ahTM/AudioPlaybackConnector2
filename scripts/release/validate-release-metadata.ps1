[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$packageVersion = "$Version.0"
$changelog = [IO.File]::ReadAllText((Resolve-Path 'CHANGELOG.md'))
if ($changelog -notmatch "(?m)^## \[$([regex]::Escape($Version))\] - \d{4}-\d{2}-\d{2}$") {
    throw "CHANGELOG.md has no dated [$Version] release section."
}

[xml]$props = Get-Content -LiteralPath 'Directory.Build.props' -Raw
$defaultPackageVersion = [string]$props.Project.PropertyGroup.PackageVersion.'#text'
if ([string]::IsNullOrWhiteSpace($defaultPackageVersion)) {
    $defaultPackageVersion = [string]$props.Project.PropertyGroup.PackageVersion
}
if ($defaultPackageVersion -ne $packageVersion) {
    throw "Directory.Build.props defaults to $defaultPackageVersion, expected $packageVersion."
}

[xml]$manifest = Get-Content -LiteralPath 'AudioPlaybackConnector2 (Package)/Package.appxmanifest' -Raw
$manifestIdentity = $manifest.SelectSingleNode("/*[local-name()='Package']/*[local-name()='Identity']")
if ($null -eq $manifestIdentity -or $manifestIdentity.Version -ne $packageVersion) {
    throw "Package.appxmanifest must contain version $packageVersion."
}

$whatsNew = Get-Content -LiteralPath 'store/whats-new.json' -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
if ([string]$whatsNew.version -ne $Version) {
    throw "store/whats-new.json targets version $($whatsNew.version), expected $Version."
}
$requiredLocales = @('en', 'de', 'fr', 'es', 'ja', 'ko', 'zh-Hans', 'zh-Hant')
foreach ($locale in $requiredLocales) {
    $text = [string]$whatsNew.locales[$locale]
    if ([string]::IsNullOrWhiteSpace($text)) { throw "Store release notes are missing locale $locale." }
    if ($text.Length -gt 1500) { throw "Store release notes for $locale exceed 1500 characters." }
}

Write-Host "Release metadata is consistent for $Version ($packageVersion)."
