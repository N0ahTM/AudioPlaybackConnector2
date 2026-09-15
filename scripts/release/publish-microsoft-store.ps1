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
$packageDirectory = Split-Path -Path $resolvedPackage -Parent
& msstore publish . --inputDirectory $packageDirectory --appId $ProductId --noCommit
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft Store draft package upload failed with exit code $LASTEXITCODE."
}

$token = Invoke-RestMethod `
    -Method Post `
    -Uri "https://login.microsoftonline.com/$env:AZURE_AD_TENANT_ID/oauth2/v2.0/token" `
    -ContentType 'application/x-www-form-urlencoded' `
    -Body @{
        client_id = $env:AZURE_AD_APPLICATION_CLIENT_ID
        client_secret = $env:AZURE_AD_APPLICATION_SECRET
        grant_type = 'client_credentials'
        scope = 'https://api.store.microsoft.com/.default'
    }
if ([string]::IsNullOrWhiteSpace($token.access_token)) {
    throw 'Microsoft Entra returned no access token.'
}

$headers = @{
    Authorization = "Bearer $($token.access_token)"
    'X-Seller-Account-Id' = $env:SELLER_ID
}
$metadataUri = "https://api.store.microsoft.com/submission/v1/product/$ProductId/metadata?includelanguagelist=true"
$metadata = Invoke-RestMethod -Method Get -Uri $metadataUri -Headers $headers
$listings = @($metadata.responseData.listings)
if ($listings.Count -eq 0) {
    throw 'Microsoft Store returned no listings to update.'
}

$updatedLocales = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($listing in $listings) {
    $language = [string]$listing.language
    $locale = Resolve-ReleaseNotesLocale $language
    if (-not $whatsNew.locales.ContainsKey($locale)) {
        throw "No Store release notes are defined for listing language $language."
    }

    $body = @{
        listings = @{
            language = $language
            whatsNew = [string]$whatsNew.locales[$locale]
        }
    } | ConvertTo-Json -Depth 5 -Compress
    $response = Invoke-RestMethod -Method Patch -Uri $metadataUri -Headers $headers -ContentType 'application/json' -Body $body
    if ($null -ne $response -and $response.PSObject.Properties.Name -contains 'isSuccess' -and -not $response.isSuccess) {
        throw "Microsoft Store rejected release notes for $language."
    }
    [void]$updatedLocales.Add($locale)
}

$missingLocales = @($requiredLocales | Where-Object { -not $updatedLocales.Contains($_) })
if ($missingLocales.Count -gt 0) {
    throw "Microsoft Store has no matching listings for: $($missingLocales -join ', ')."
}

& msstore submission publish $ProductId
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft Store submission commit failed with exit code $LASTEXITCODE."
}

& msstore submission status $ProductId
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft Store status check failed with exit code $LASTEXITCODE."
}
