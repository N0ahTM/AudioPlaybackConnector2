[CmdletBinding()]
param(
    [string]$StringsDirectory = 'AudioPlaybackConnector2/res/strings'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expectedFiles = @('en.json', 'de.json', 'fr.json', 'es.json', 'ja.json', 'ko.json', 'zh_hans.json', 'zh_hant.json')
$resolvedDirectory = (Resolve-Path -LiteralPath $StringsDirectory).Path

function Read-StringTable([string]$Path) {
    try {
        $table = Get-Content -LiteralPath $Path -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
    } catch {
        throw "Invalid localization JSON in $Path`: $($_.Exception.Message)"
    }
    if ($table -isnot [System.Collections.IDictionary]) {
        throw "Localization file must contain one JSON object: $Path"
    }
    foreach ($entry in $table.GetEnumerator()) {
        if ($entry.Value -isnot [string] -or [string]::IsNullOrWhiteSpace($entry.Value)) {
            throw "Localization value '$($entry.Key)' must be a non-empty string in $Path."
        }
    }
    return $table
}

function Get-Placeholders([string]$Text) {
    return @([regex]::Matches($Text, '\{\d+(?::[^{}]+)?\}') | ForEach-Object Value | Sort-Object)
}

$actualFiles = @(Get-ChildItem -LiteralPath $resolvedDirectory -File -Filter '*.json' | ForEach-Object Name | Sort-Object)
$missingFiles = @($expectedFiles | Where-Object { $_ -notin $actualFiles })
$unexpectedFiles = @($actualFiles | Where-Object { $_ -notin $expectedFiles })
if ($missingFiles.Count -gt 0 -or $unexpectedFiles.Count -gt 0) {
    throw "Localization file set differs from the supported locales. Missing: $($missingFiles -join ', '); unexpected: $($unexpectedFiles -join ', ')."
}

$english = Read-StringTable (Join-Path $resolvedDirectory 'en.json')
$failures = [System.Collections.Generic.List[string]]::new()
$summary = [System.Collections.Generic.List[string]]::new()

foreach ($fileName in $expectedFiles | Where-Object { $_ -ne 'en.json' }) {
    $path = Join-Path $resolvedDirectory $fileName
    $table = Read-StringTable $path
    $missingKeys = @($english.Keys | Where-Object { -not $table.ContainsKey($_) } | Sort-Object)
    $unknownKeys = @($table.Keys | Where-Object { -not $english.ContainsKey($_) } | Sort-Object)

    foreach ($key in $unknownKeys) {
        $failures.Add("$fileName contains unknown key '$key'.")
    }
    foreach ($key in $english.Keys | Where-Object { $table.ContainsKey($_) }) {
        $sourcePlaceholders = @(Get-Placeholders ([string]$english[$key]))
        $translatedPlaceholders = @(Get-Placeholders ([string]$table[$key]))
        if (($sourcePlaceholders -join "`n") -cne ($translatedPlaceholders -join "`n")) {
            $failures.Add("$fileName key '$key' has placeholders [$($translatedPlaceholders -join ', ')] but English requires [$($sourcePlaceholders -join ', ')].")
        }
    }

    $translatedCount = $english.Count - $missingKeys.Count
    $summary.Add("$fileName`: $translatedCount/$($english.Count) keys; English fallback for $($missingKeys.Count).")
}

if ($failures.Count -gt 0) {
    throw ($failures -join [Environment]::NewLine)
}

$summary | ForEach-Object { Write-Host $_ }
Write-Host "Localization validation passed. English defines $($english.Count) keys."
