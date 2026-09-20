[CmdletBinding()]
param(
    [switch]$Check,
    [string]$SourcePath = 'AudioPlaybackConnector2/src/control/CliParser.cpp',
    [string]$DocumentationPath = 'docs/CLI.md'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = Get-Content -LiteralPath $SourcePath -Raw -Encoding utf8
$helpMatch = [regex]::Match($source, '(?s)std::wstring HelpText\(\)\s*\{\s*return LR"\((.*?)\)";\s*\}')
if (-not $helpMatch.Success) { throw "Unable to extract HelpText from $SourcePath." }

$help = $helpMatch.Groups[1].Value.Trim().Replace("`r`n", "`n")
$begin = '<!-- BEGIN GENERATED APC2CTL HELP -->'
$end = '<!-- END GENERATED APC2CTL HELP -->'
$generated = $begin + "`n" + '```text' + "`n" + $help + "`n" + '```' + "`n" + $end

$documentation = Get-Content -LiteralPath $DocumentationPath -Raw -Encoding utf8
$regionPattern = "(?s)$([regex]::Escape($begin)).*?$([regex]::Escape($end))"
$match = [regex]::Match($documentation.Replace("`r`n", "`n"), $regionPattern)
if (-not $match.Success) { throw "Generated CLI help markers are missing from $DocumentationPath." }

if ($match.Value -cne $generated) {
    if ($Check) { throw "$DocumentationPath does not match the built-in apc2ctl help. Run scripts/update-cli-reference.ps1." }
    $updated = [regex]::Replace($documentation.Replace("`r`n", "`n"), $regionPattern, [System.Text.RegularExpressions.MatchEvaluator]{ param($unused) $generated })
    [IO.File]::WriteAllText((Resolve-Path $DocumentationPath), $updated, [Text.UTF8Encoding]::new($false))
    Write-Host "Updated $DocumentationPath."
    return
}

Write-Host 'CLI documentation matches the built-in help.'
