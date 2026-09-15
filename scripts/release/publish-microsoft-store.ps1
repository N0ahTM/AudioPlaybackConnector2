[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Z0-9]+$')]
    [string]$ProductId,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$PackagePath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$WhatsNewPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

foreach ($name in @('AZURE_AD_TENANT_ID', 'SELLER_ID', 'AZURE_AD_APPLICATION_CLIENT_ID', 'AZURE_AD_APPLICATION_SECRET')) {
    if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
        throw "Required environment variable $name is not set."
    }
}

$whatsNew = Get-Content -LiteralPath $WhatsNewPath -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
if ($whatsNew.version -ne $Version) {
    throw "Store release notes target version $($whatsNew.version), expected $Version."
}

$requiredLocales = @('en', 'de', 'fr', 'es', 'ja', 'ko', 'zh-Hans', 'zh-Hant')
foreach ($locale in $requiredLocales) {
    $text = [string]$whatsNew.locales[$locale]
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "Store release notes are missing locale $locale."
    }
    if ($text.Length -gt 1500) {
        throw "Store release notes for $locale exceed 1500 characters."
    }
}

function Resolve-ReleaseNotesLocale([string]$Language) {
    $normalized = $Language.Trim().Replace('_', '-').ToLowerInvariant()
    if ($normalized -match '^zh-(hans|cn|sg)($|-)') { return 'zh-Hans' }
    if ($normalized -match '^zh-(hant|tw|hk|mo)($|-)') { return 'zh-Hant' }
    return $normalized.Split('-')[0]
}

$resolvedPackage = (Resolve-Path -LiteralPath $PackagePath).Path
& msstore publish $resolvedPackage --appId $ProductId --noCommit
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft Store draft package upload failed with exit code $LASTEXITCODE."
}

$submissionPath = Join-Path ([IO.Path]::GetTempPath()) "msstore-submission-$PID.json"
$expectedPackageVersion = "$Version.0"
$submission = $null
$uploadedPackage = $null

# Partner Center can briefly return the previous package after msstore reports a
# successful upload. Never update or commit that stale representation: doing so
# would submit only the listing changes and leave the old package in place.
for ($attempt = 1; $attempt -le 30; $attempt++) {
    & msstore submission get $ProductId | Out-File -LiteralPath $submissionPath -Encoding utf8
    if ($LASTEXITCODE -ne 0) {
        throw "Microsoft Store draft retrieval failed with exit code $LASTEXITCODE."
    }

    $submission = Get-Content -LiteralPath $submissionPath -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
    $uploadedPackage = @($submission['ApplicationPackages']) | Where-Object {
        if ($null -eq $_) { return $false }
        $packageVersion = [string]$_['Version']
        $packageFileName = [string]$_['FileName']
        $packageVersion -eq $expectedPackageVersion -or
            $packageFileName -match "_$([regex]::Escape($expectedPackageVersion))_"
    } | Select-Object -First 1

    if ($null -ne $uploadedPackage) {
        break
    }

    if ($attempt -lt 30) {
        Write-Host "Waiting for Partner Center to expose package version $expectedPackageVersion (attempt $attempt of 30)..."
        Start-Sleep -Seconds 10
    }
}

if ($null -eq $uploadedPackage) {
    $visiblePackages = @($submission['ApplicationPackages']) | ForEach-Object {
        "{0} (version {1})" -f [string]$_['FileName'], [string]$_['Version']
    }
    throw "Partner Center did not expose uploaded package version $expectedPackageVersion. Visible packages: $($visiblePackages -join ', '). The submission was not updated or committed."
}

$listings = $submission['Listings']
if ($null -eq $listings -or $listings.Count -eq 0) {
    throw 'Microsoft Store returned no listings to update.'
}

$updatedLocales = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($language in @($listings.Keys)) {
    $locale = Resolve-ReleaseNotesLocale $language
    if (-not $whatsNew.locales.ContainsKey($locale)) {
        throw "No Store release notes are defined for listing language $language."
    }

    $listing = $listings[$language]
    $baseListing = $listing['BaseListing']
    if ($null -eq $baseListing) {
        throw "Microsoft Store listing $language has no base listing."
    }
    $baseListing['ReleaseNotes'] = [string]$whatsNew.locales[$locale]
    [void]$updatedLocales.Add($locale)
}

$missingLocales = @($requiredLocales | Where-Object { -not $updatedLocales.Contains($_) })
if ($missingLocales.Count -gt 0) {
    throw "Microsoft Store has no matching listings for: $($missingLocales -join ', ')."
}

$submission | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $submissionPath -Encoding utf8
$null = & msstore submission update $ProductId $submissionPath
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft Store release-note update failed with exit code $LASTEXITCODE."
}

& msstore submission publish $ProductId
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft Store submission commit failed with exit code $LASTEXITCODE."
}

& msstore submission status $ProductId
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft Store status check failed with exit code $LASTEXITCODE."
}
