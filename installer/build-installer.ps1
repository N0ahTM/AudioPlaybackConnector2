#Requires -Version 5.1
# Stages the payload and compiles the Inno Setup bootstrapper.
# Bundle mode (default): embeds MSIX + .cer + Dependencies.
# Web mode: embeds the runtime script and pinned certificate; the MSIX and
# dependencies are downloaded from the latest GitHub release at install time.
# Run from the repo root:
#   .\installer\build-installer.ps1 -Mode Bundle -MsixPath <release.msix>
#   .\installer\build-installer.ps1 -Mode Web
[CmdletBinding()]
param(
    [ValidateSet('Bundle', 'Web')] [string] $Mode = 'Bundle',
    [string] $MsixPath,
    [string] $CertPath = 'certs\AudioPlaybackConnector2.cer',
    [string] $DependenciesDir,
    [string] $Version,
    [string] $OutputDir = 'dist\installer'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$stage = $null
Push-Location $repoRoot
try {
    if ($Mode -eq 'Web') {
        if (-not $Version) {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            $release = Invoke-RestMethod `
                -Uri 'https://api.github.com/repos/N0ahTM/AudioPlaybackConnector2/releases/latest' `
                -Headers @{ 'User-Agent' = 'AudioPlaybackConnector2-Setup' } -TimeoutSec 30
            $Version = $release.tag_name.TrimStart('v')
        }
        $CertPath = (Resolve-Path $CertPath).Path
        Write-Host "Building web bootstrapper version $Version"
    } else {
        if (-not $MsixPath) { throw '-MsixPath is required in Bundle mode.' }
        $MsixPath = (Resolve-Path $MsixPath).Path
        $CertPath = (Resolve-Path $CertPath).Path
        if (-not $DependenciesDir) {
            $candidate = Join-Path (Split-Path $MsixPath) 'Dependencies'
            if (Test-Path $candidate) { $DependenciesDir = $candidate }
        }
        $packageName = Split-Path $MsixPath -Leaf
        if ($packageName -match '_(\d+\.\d+\.\d+)(?:\.\d+)?_x64_ARM64\.msixbundle$') {
            $packageVersion = $Matches[1]
            $packageArchitectures = @('x64', 'arm64')
        } elseif ($packageName -match '_(\d+\.\d+\.\d+)(?:\.\d+)?_(x64|arm64)\.msix$') {
            $packageVersion = $Matches[1]
            $packageArchitectures = @($Matches[2])
        } else {
            throw 'Could not parse package version and architecture from MSIX bundle filename.'
        }
        if (-not $Version) {
            $Version = $packageVersion
        } elseif ($Version -ne $packageVersion) {
            throw "Installer version $Version does not match MSIX version $packageVersion."
        }
        Write-Host "Building bundle bootstrapper for version $Version"
    }
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must use SemVer format, for example 1.2.3: $Version" }

    $stageName = 'AudioPlaybackConnector2-installer-' + [guid]::NewGuid().ToString('N')
    $stage = Join-Path ([IO.Path]::GetTempPath()) $stageName
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'stage\install-app.ps1') (Join-Path $stage 'install-app.ps1')
    Copy-Item $CertPath (Join-Path $stage 'AudioPlaybackConnector2.cer')
    if ($Mode -eq 'Bundle') {
        Copy-Item $MsixPath $stage
        if ($DependenciesDir) {
            Copy-Item $DependenciesDir (Join-Path $stage 'Dependencies') -Recurse
        } else {
            Write-Warning 'No Dependencies directory found; MSIX must carry all dependencies.'
        }
        $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        foreach ($architecture in $packageArchitectures) {
            & $windowsPowerShell -NoProfile -ExecutionPolicy Bypass `
                -File (Join-Path $stage 'install-app.ps1') `
                -Step validate `
                -PackageDir $stage `
                -PackageArchitecture $architecture
            if ($LASTEXITCODE -ne 0) {
                throw "Staged installer payload validation failed for $architecture."
            }
        }
    }

    $iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if (-not $iscc) {
        $pf86 = [Environment]::GetFolderPath('ProgramFilesX86')
        $pf64 = [Environment]::GetFolderPath('ProgramFiles')
        $candidates = @(
            (Join-Path $pf86 'Inno Setup 6\ISCC.exe'),
            (Join-Path $pf64 'Inno Setup 6\ISCC.exe')
        ) | Where-Object { $_ -and (Test-Path $_) }
        if (-not $candidates) {
            throw 'ISCC.exe not found. Install Inno Setup 6: winget install JRSoftware.InnoSetup'
        }
        $iscc = @($candidates)[0]
    } else {
        $iscc = $iscc.Source
    }

    if (-not [IO.Path]::IsPathRooted($OutputDir)) { $OutputDir = Join-Path $repoRoot $OutputDir }
    $OutputDir = [IO.Path]::GetFullPath($OutputDir)
    $null = New-Item -ItemType Directory -Path $OutputDir -Force
    $isccArgs = @(
        "/DAppVersion=$Version",
        "/DStageDir=$stage",
        "/O$OutputDir"
    )
    if ($Mode -eq 'Web') {
        $isccArgs += '/DWEBBOOT=1'
    } else {
        # Embed exactly the payload validated above, including dual-architecture MSIX bundles.
        $isccArgs += "/DPackageFileName=$packageName"
    }
    & $iscc @isccArgs (Join-Path $repoRoot 'installer\setup.iss')
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

    $baseName = if ($Mode -eq 'Web') { 'AudioPlaybackConnector2-WebSetup' }
                else { "AudioPlaybackConnector2-Setup-$Version" }
    $setup = Join-Path $OutputDir "$baseName.exe"
    if (-not (Test-Path $setup)) { throw "Expected output missing: $setup" }
    Write-Host "Done: $((Resolve-Path $setup).Path)"
} finally {
    try {
        if ($stage -and (Test-Path -LiteralPath $stage)) {
            $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
            $resolvedStage = [IO.Path]::GetFullPath($stage)
            if (-not $resolvedStage.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
                [IO.Path]::GetFileName($resolvedStage) -notlike 'AudioPlaybackConnector2-installer-*') {
                throw "Refusing to remove unexpected staging path: $resolvedStage"
            }
            Remove-Item -LiteralPath $resolvedStage -Recurse -Force
        }
    } finally {
        Pop-Location
    }
}
