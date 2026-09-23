$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repository = Split-Path -Parent $PSScriptRoot
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$testRoot = Join-Path $temporaryRoot ('apc-store-tag-notes-' + [guid]::NewGuid().ToString('N'))
$tagRoot = Join-Path $testRoot 'release-tag'
$automationRoot = Join-Path $testRoot 'store-automation'
$packagePath = Join-Path $testRoot 'test_0.9.1.0.msixupload'
$credentialNames = @('AZURE_AD_TENANT_ID', 'SELLER_ID', 'AZURE_AD_APPLICATION_CLIENT_ID', 'AZURE_AD_APPLICATION_SECRET')
$originalCredentials = @{}

try {
    New-Item -ItemType Directory -Path (Join-Path $tagRoot 'store'), (Join-Path $automationRoot 'scripts/release') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repository 'store/whats-new.json') -Destination (Join-Path $tagRoot 'store/whats-new.json')
    foreach ($name in @('publish-microsoft-store.ps1', 'ReleaseVersion.psm1')) {
        Copy-Item -LiteralPath (Join-Path $repository "scripts/release/$name") -Destination (Join-Path $automationRoot "scripts/release/$name")
    }
    Set-Content -LiteralPath $packagePath -Value 'test package'

    $tagNotes = Get-Content -LiteralPath (Join-Path $tagRoot 'store/whats-new.json') -Raw | ConvertFrom-Json -AsHashtable
    $branchNotes = $tagNotes | ConvertTo-Json -Depth 10 | ConvertFrom-Json -AsHashtable
    foreach ($locale in @($branchNotes.locales.Keys)) {
        $branchNotes.locales[$locale] = "Different branch notes for $locale"
    }
    New-Item -ItemType Directory -Path (Join-Path $automationRoot 'store') | Out-Null
    $branchNotes | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $automationRoot 'store/whats-new.json') -Encoding utf8

    $listings = @{}
    foreach ($locale in @($tagNotes.locales.Keys)) {
        $listings[$locale] = @{ BaseListing = @{ ReleaseNotes = 'old notes' } }
    }
    $global:apcStoreSubmission = @{
        ApplicationPackages = @(
            @{ FileName = 'test_0.9.1.0.msixupload'; Version = '0.9.1.0'; FileStatus = 'PendingUpload' },
            @{ FileName = 'test_0.9.0.0.msixupload'; Version = '0.9.0.0'; FileStatus = 'Published' }
        )
        Listings = $listings
    }
    $global:apcStorePublished = $false

    function msstore {
        if ($args[0] -eq 'publish') {
            if ($args[2] -ne '--appId' -or $args[4] -ne '--noCommit') { throw 'Unexpected draft upload arguments.' }
            $global:LASTEXITCODE = 0
            return
        }
        switch ("$($args[0]) $($args[1])") {
            'submission get' {
                $global:apcStoreSubmission | ConvertTo-Json -Depth 100
            }
            'submission update' {
                $global:apcStoreSubmission = Get-Content -LiteralPath $args[3] -Raw | ConvertFrom-Json -AsHashtable
            }
            'submission publish' { $global:apcStorePublished = $true }
            'submission status' { 'mock status' }
            default { throw "Unexpected Microsoft Store operation: $($args -join ' ')" }
        }
        $global:LASTEXITCODE = 0
    }

    foreach ($name in $credentialNames) {
        $originalCredentials[$name] = [Environment]::GetEnvironmentVariable($name)
        [Environment]::SetEnvironmentVariable($name, 'local-test-only')
    }

    Push-Location $tagRoot
    try {
        & (Join-Path $automationRoot 'scripts/release/publish-microsoft-store.ps1') `
            -ProductId 9N366PGKJZ0K `
            -PackagePath $packagePath `
            -Version '0.9.1' `
            -WhatsNewPath './store/whats-new.json'
    } finally {
        Pop-Location
    }

    if (-not $global:apcStorePublished) { throw 'The mocked submission was not committed.' }
    foreach ($locale in @($tagNotes.locales.Keys)) {
        $actual = $global:apcStoreSubmission.Listings[$locale].BaseListing.ReleaseNotes
        if ($actual -cne $tagNotes.locales[$locale] -or $actual -ceq $branchNotes.locales[$locale]) {
            throw "Store listing $locale did not use the release-tag notes."
        }
    }
    Write-Host 'Store tag-note isolation test passed.'
} finally {
    foreach ($name in $credentialNames) {
        [Environment]::SetEnvironmentVariable($name, $originalCredentials[$name])
    }
    Remove-Variable -Name apcStoreSubmission, apcStorePublished -Scope Global -ErrorAction SilentlyContinue
    $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
    if ([IO.Path]::GetDirectoryName($resolvedRoot) -ne $temporaryRoot -or
        [IO.Path]::GetFileName($resolvedRoot) -notlike 'apc-store-tag-notes-*') {
        throw 'Refusing to remove a test directory outside its temporary root.'
    }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
}
