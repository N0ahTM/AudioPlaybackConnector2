param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$ChangelogPath = "CHANGELOG.md",

    [string]$OutputPath = ''
)

$ErrorActionPreference = "Stop"

$searchVersion = $Version.TrimStart('v')
$content = [System.IO.File]::ReadAllText($ChangelogPath)

# Match from the version header to the next version header or end of file
$pattern = "(?ms)^## \[$([regex]::Escape($searchVersion))\][^\r\n]*(?:\r?\n)(.*?)(?=^## \[|\z)"
$match = [regex]::Match($content, $pattern)

if (-not $match.Success) {
    throw "Version [$searchVersion] not found in $ChangelogPath."
}

$body = $match.Groups[1].Value.Trim()

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $outputDirectory = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
        New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($OutputPath, $body, [System.Text.UTF8Encoding]::new($false))
}

if ($env:GITHUB_OUTPUT) {
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        "PATH=$([System.IO.Path]::GetFullPath($OutputPath))" >> $env:GITHUB_OUTPUT
    }
}

$body
