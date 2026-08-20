#Requires -Version 5.1
# Install/uninstall worker for the Inno Setup bootstrapper.
# The wizard calls this script once per -Step so each step's console output can
# be shown on the progress page. Without -Step everything runs in one go
# (manual CLI use).
param(
    [string] $PackageDir,
    [ValidateSet('validate', 'cert', 'install', 'verify', 'uninstall')] [string] $Step,
    [ValidateSet('x86', 'x64', 'arm64')] [string] $PackageArchitecture = 'x64',
    [switch] $Launch
)

$ErrorActionPreference = 'Stop'
$logDir = Join-Path $env:LOCALAPPDATA 'AudioPlaybackConnector2'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
Start-Transcript -Path (Join-Path $logDir 'install.log') -Append | Out-Null

function Get-AppCertificate {
    param([string] $Dir)
    $cerPath = Join-Path $Dir 'AudioPlaybackConnector2.cer'
    if (-not (Test-Path -LiteralPath $cerPath -PathType Leaf)) { throw "Certificate not found: $cerPath" }
    # Not X509Certificate2::CreateFromCertFile: on .NET Framework it can yield a
    # handle-only object (RawData empty, Thumbprint null) and breaks store lookups.
    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($cerPath)
    if ($cert.Subject -ne 'CN=AudioPlaybackConnector2' -or -not $cert.Thumbprint) {
        throw 'The bundled signing certificate has an unexpected identity.'
    }
    return $cert
}

function Import-AppCertificate {
    param([string] $Dir)
    $cert = Get-AppCertificate -Dir $Dir
    # TrustedPeople (not Root): grants MSIX sideload trust per-user without admin rights or a security prompt.
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'CurrentUser')
    $store.Open('ReadWrite')
    try {
        $existing = $store.Certificates.Find('FindByThumbprint', $cert.Thumbprint, $false)
        if ($existing.Count -eq 0) {
            $store.Add($cert)
            Write-Host "Certificate $($cert.Thumbprint) added to CurrentUser\TrustedPeople."
        } else {
            Write-Host 'Certificate already trusted, skipping.'
        }
    } finally {
        $store.Close()
    }
}

function Get-AppPackage {
    param([string] $Dir)
    $packages = @(Get-ChildItem -Path $Dir -Filter "AudioPlaybackConnector2_*_$PackageArchitecture.msix" -File)
    if ($packages.Count -ne 1) {
        throw "Expected exactly one $PackageArchitecture app package in $Dir; found $($packages.Count)."
    }
    return $packages[0]
}

function Assert-AppPackageIdentity {
    param([string] $Path)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $archive.GetEntry('AppxManifest.xml')
        if (-not $entry) { throw 'AppxManifest.xml is missing from the app package.' }
        $reader = [IO.StreamReader]::new($entry.Open())
        try {
            [xml] $manifest = $reader.ReadToEnd()
        } finally {
            $reader.Dispose()
        }
        $identity = $manifest.Package.Identity
        if ($identity.Name -ne 'N0ahTM.AudioPlaybackConnector2' -or
            $identity.Publisher -ne 'CN=AudioPlaybackConnector2' -or
            $identity.ProcessorArchitecture -ne $PackageArchitecture) {
            throw "Unexpected package identity: $($identity.Name), $($identity.Publisher), $($identity.ProcessorArchitecture)."
        }
        Write-Host "Package identity verified: $($identity.Name) $($identity.Version) $($identity.ProcessorArchitecture)."
    } finally {
        $archive.Dispose()
    }
}

function Install-AppPackage {
    param([string] $Dir)
    $package = Get-AppPackage -Dir $Dir

    $depsRoot = Join-Path $Dir 'Dependencies'
    $deps = @()
    if (Test-Path $depsRoot) {
        $archDir = Join-Path $depsRoot $PackageArchitecture
        $searchDir = if (Test-Path $archDir) { $archDir } else { $depsRoot }
        $deps = @(Get-ChildItem -Path $searchDir -Recurse -File |
                  Where-Object { $_.Extension -in '.msix', '.appx' })
    } else {
        # Web mode downloads dependencies flat next to the main package.
        $deps = @(Get-ChildItem -Path $Dir -File | Where-Object {
            ($_.Extension -in '.msix', '.appx') -and ($_.Name -notmatch '^AudioPlaybackConnector2_') })
    }
    $addParams = @{ Path = $package.FullName; ErrorAction = 'Stop' }
    if ($deps.Count -gt 0) {
        $addParams['DependencyPath'] = $deps.FullName
        Write-Host "Dependencies: $($deps.Count) package(s) for $PackageArchitecture."
    }
    Assert-AppPackageIdentity -Path $package.FullName
    Write-Host "Installing $($package.Name) ..."
    Stop-AppProcess
    $addParams['ForceApplicationShutdown'] = $true
    try {
        Add-AppxPackage @addParams
    } catch {
        if ($_.Exception.Message -match '0x80073D06') {
            # A newer (or same) build already installed is not an installation failure for a bootstrapper.
            Write-Host 'A newer version is already installed; leaving it untouched.'
        } elseif ($_.Exception.Message -match '0x80073D02') {
            if ($_.Exception.Message -match 'N0ahTM\.AudioPlaybackConnector2') {
                # Resources in use by our own app (tray apps always run). Close and retry once.
                Write-Host 'App is running; closing it and retrying ...'
                Stop-AppProcess
                Start-Sleep -Seconds 2
                Add-AppxPackage @addParams
            } elseif ($deps.Count -gt 0) {
                # The bundled WindowsAppRuntime framework is in use by other apps
                # (Spotify, widgets, ...). Registering it again is unnecessary when
                # a framework is already present — retry without forcing the deps.
                Write-Host 'Shared framework in use by other apps; retrying without bundled dependencies ...'
                Add-AppxPackage -Path $package.FullName -ErrorAction Stop
            } else {
                throw
            }
        } else {
            throw
        }
    }
}

function Test-AppPayload {
    param([string] $Dir)
    $cert = Get-AppCertificate -Dir $Dir
    $package = Get-AppPackage -Dir $Dir
    Assert-AppPackageIdentity -Path $package.FullName
    Write-Host "Pinned certificate verified: $($cert.Thumbprint)."
}

function Stop-AppProcess {
    # Exact names only: a wildcard would also match the setup itself
    # (AudioPlaybackConnector2-WebSetup.exe) and kill our own parent.
    Get-Process -Name 'AudioPlaybackConnector2', 'AudioPlaybackConnector2.Control' `
        -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Test-AppInstalled {
    $installed = Get-AppxPackage -Name 'N0ahTM.AudioPlaybackConnector2'
    if (-not $installed) { throw 'Package is not registered after Add-AppxPackage.' }
    Write-Host "Installed: $($installed.PackageFullName)"
    if ($Launch) {
        Write-Host 'Launching app ...'
        Start-Process "shell:AppsFolder\$($installed.PackageFamilyName)!App"
    }
}

function Uninstall-App {
    param([string] $Dir)
    Stop-AppProcess
    $pkg = Get-AppxPackage -Name 'N0ahTM.AudioPlaybackConnector2'
    if ($pkg) {
        Write-Host "Removing $($pkg.PackageFullName) ..."
        Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction Stop
    } else {
        Write-Host 'Package not installed, nothing to remove.'
    }
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'CurrentUser')
    $store.Open('ReadWrite')
    try {
        $cerPath = Join-Path $Dir 'AudioPlaybackConnector2.cer'
        if (Test-Path -LiteralPath $cerPath -PathType Leaf) {
            $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($cerPath)
            $ours = $store.Certificates.Find('FindByThumbprint', $cert.Thumbprint, $false)
            foreach ($c in $ours) {
                $store.Remove($c)
                Write-Host "Certificate $($c.Thumbprint) removed from TrustedPeople."
            }
        }
    } finally {
        $store.Close()
    }
    Write-Host 'User settings under %LOCALAPPDATA% were kept.'
}

try {
    if (-not $Step) {
        Import-AppCertificate -Dir $PackageDir
        Install-AppPackage -Dir $PackageDir
        Test-AppInstalled
        exit 0
    }
    switch ($Step) {
        'validate'   { if (-not $PackageDir) { throw '-PackageDir required' }; Test-AppPayload -Dir $PackageDir }
        'cert'      { if (-not $PackageDir) { throw '-PackageDir required' }; Import-AppCertificate -Dir $PackageDir }
        'install'   { if (-not $PackageDir) { throw '-PackageDir required' }; Install-AppPackage -Dir $PackageDir }
        'verify'    { Test-AppInstalled }
        'uninstall' { if (-not $PackageDir) { throw '-PackageDir required' }; Uninstall-App -Dir $PackageDir }
    }
    Write-Host 'OK'
    exit 0
} catch {
    Write-Host "FAILED: $($_.Exception.Message)"
    exit 1
} finally {
    Stop-Transcript | Out-Null
}
