$ErrorActionPreference = 'Stop'
$validator = Join-Path $PSScriptRoot 'validate-localizations.ps1'
$fixture = Join-Path ([System.IO.Path]::GetTempPath()) ('apc-localizations-' + [guid]::NewGuid().ToString('N'))
$locales = @('en', 'de', 'fr', 'es', 'ja', 'ko', 'zh_hans', 'zh_hant')
New-Item -ItemType Directory -Path $fixture | Out-Null

function Write-Table([string]$Locale, [hashtable]$Table) {
    $Table | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixture "$Locale.json") -Encoding utf8
}

function Assert-Rejected([string]$Expected) {
    $failure = $null
    try { & $validator -StringsDirectory $fixture *> $null }
    catch { $failure = $_.Exception.Message }
    if (-not $failure -or -not $failure.Contains($Expected)) {
        throw "Expected localization rejection '$Expected', got '$failure'."
    }
}

try {
    foreach ($locale in $locales) { Write-Table $locale @{ Greeting = 'Hello {0}'; Title = 'App' } }
    & $validator -StringsDirectory $fixture

    # A missing translation must fail even though runtime English fallback still exists.
    Write-Table 'de' @{ Greeting = 'Hallo {0}' }
    Assert-Rejected "de.json is missing key 'Title'"
    Write-Table 'de' @{ Greeting = 'Hallo {0}'; Title = 'App'; Obsolete = 'Old' }
    Assert-Rejected "de.json contains unknown key 'Obsolete'"
    Write-Table 'de' @{ Greeting = 'Hallo {1}'; Title = 'App' }
    Assert-Rejected "de.json key 'Greeting' has placeholders"
    Write-Table 'de' @{ Greeting = 'Hallo {0} {0}'; Title = 'App' }
    Assert-Rejected "de.json key 'Greeting' has placeholders"
    Write-Table 'de' @{ Greeting = 'Hallo {0}'; Title = '' }
    Assert-Rejected "must be a non-empty string"
    Write-Table 'de' @{ Greeting = 'Hallo {0}'; Title = 7 }
    Assert-Rejected "must be a non-empty string"
    Write-Table 'de' @{ Greeting = 'Hallo {0}'; Title = 'App' }
    Write-Table 'unsupported' @{ Greeting = 'Hello {0}'; Title = 'App' }
    Assert-Rejected 'unexpected: unsupported.json'
} finally {
    # Only this invocation's directory, directly below the system temporary directory.
    $resolvedFixture = [System.IO.Path]::GetFullPath($fixture)
    $temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    if (-not $resolvedFixture.StartsWith($temporaryRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'Localization fixture escaped the temporary directory.'
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
Write-Host 'Localization gate: complete tables accepted; missing/unknown keys, placeholders, values and locales rejected.'
