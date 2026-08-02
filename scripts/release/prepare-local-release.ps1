# Builds a local Release x64 MSIX (unsigned) and copies release assets into ./dist/v0.7.0/
# For signed GitHub releases, push tag v0.7.0 and use the Release workflow instead.

param(
    [string]$Version = "0.7.0.0",
    [string]$SemVer = "0.7.0",
    [string]$Architecture = "x64",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "../..")

Push-Location $repoRoot
try {
    Write-Host "Updating package metadata to $Version ..."
    & (Join-Path $repoRoot "scripts/release/update-package-version.ps1") -Version $Version

    Write-Host "Restoring NuGet packages ..."
    msbuild AudioPlaybackConnector2.slnx -t:restore -p:RestorePackagesConfig=true | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Restore failed." }

    Write-Host "Building unsigned sideload MSIX ($Configuration, $Architecture) ..."
    msbuild AudioPlaybackConnector2.slnx `
        /p:Configuration=$Configuration `
        /p:Platform=$Architecture `
        /p:AppxPackageSigningEnabled=false `
        /p:UapAppxPackageBuildMode=SideloadOnly `
        /p:AppxBundle=Never `
        /t:Rebuild | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }

    $distRoot = Join-Path $repoRoot "dist/v$SemVer"
    New-Item -ItemType Directory -Path $distRoot -Force | Out-Null

    $msix = & (Join-Path $repoRoot "scripts/release/locate-msix.ps1") `
        -Version $Version `
        -OutputDirectory $distRoot `
        -Architecture $Architecture `
        -Configuration $Configuration

    # Copy dependencies into the dist folder so local .appinstaller can resolve them
    $depsTargetDir = Join-Path $distRoot "Dependencies/$Architecture"
    if ($msix.DependenciesDirectory -and (Test-Path $msix.DependenciesDirectory)) {
        New-Item -ItemType Directory -Path $depsTargetDir -Force | Out-Null
        Get-ChildItem -Path $msix.DependenciesDirectory -Recurse -Include @("*.appx", "*.msix") | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $depsTargetDir -Force
        }
    }

    $msixUrl = "https://github.com/N0ahTM/AudioPlaybackConnector2/releases/download/v$SemVer/$($msix.Name)"
    $appInstallerPath = Join-Path $distRoot "AudioPlaybackConnector2.appinstaller"
    $params = @{
        MsixUrl = $msixUrl
        AppInstallerUrl = "https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller"
        MsixPath = $msix.Path
        OutputPath = $appInstallerPath
        ExpectedPackageVersion = $Version
        ProcessorArchitecture = $Architecture
    }
    if (Test-Path $depsTargetDir) {
        $params['DependenciesDirectory'] = $depsTargetDir
        $params['DependencyBaseUrl'] = "Dependencies/$Architecture"
    }
    & (Join-Path $repoRoot "scripts/release/generate-appinstaller.ps1") @params | Out-Host

    $exeCandidates = @(
        (Join-Path $repoRoot "$Architecture/$Configuration/AudioPlaybackConnector2/AudioPlaybackConnector2.exe")
        (Join-Path $repoRoot "AudioPlaybackConnector2 (Package)/bin/$Architecture/$Configuration/AudioPlaybackConnector2/AudioPlaybackConnector2.exe")
        (Join-Path $repoRoot "AudioPlaybackConnector2/$Architecture/$Configuration/AudioPlaybackConnector2/AudioPlaybackConnector2.exe")
    )
    $exeSource = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($exeSource) {
        Copy-Item -LiteralPath $exeSource -Destination (Join-Path $distRoot "AudioPlaybackConnector2.exe") -Force
        Write-Host "Copied executable from $exeSource"
    } else {
        Write-Warning "Executable not found; MSIX is still the installable release artifact."
    }

    $cerSource = Join-Path $repoRoot "certs/AudioPlaybackConnector2.cer"
    if (Test-Path $cerSource) {
        Copy-Item -LiteralPath $cerSource -Destination (Join-Path $distRoot "AudioPlaybackConnector2.cer") -Force
    }

    $changelogSource = Join-Path $repoRoot "CHANGELOG.md"
    Copy-Item -LiteralPath $changelogSource -Destination (Join-Path $distRoot "CHANGELOG.md") -Force
    [string]$releaseNotes = & (Join-Path $repoRoot "scripts/release/extract-changelog.ps1") `
        -Version $SemVer `
        -ChangelogPath $changelogSource
    [System.IO.File]::WriteAllText(
        (Join-Path $distRoot "RELEASE_NOTES.md"),
        $releaseNotes.Trim() + [Environment]::NewLine
    )

    $streamDeckRoot = Join-Path $repoRoot "integrations/stream-deck"
    Push-Location $streamDeckRoot
    try {
        npm ci | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Stream Deck dependency restore failed." }
        npm audit --audit-level=high | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Stream Deck dependency audit failed." }
        npm run pack | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Stream Deck plugin build failed." }
    } finally {
        Pop-Location
    }
    $streamDeckPackage = Get-ChildItem -Path $streamDeckRoot -Filter "*.streamDeckPlugin" -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $streamDeckPackage) { throw "Stream Deck plugin package was not created." }
    Copy-Item -LiteralPath $streamDeckPackage.FullName -Destination $distRoot -Force

    Write-Host ""
    Write-Host "Local release bundle ready:" -ForegroundColor Green
    Get-ChildItem -LiteralPath $distRoot | Format-Table Name, Length, LastWriteTime
    Write-Host "Publish to GitHub: git tag v$SemVer && git push origin v$SemVer"
} finally {
    Pop-Location
}
