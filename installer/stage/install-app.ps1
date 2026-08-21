#Requires -Version 5.1
# Install/uninstall worker for the Inno Setup bootstrapper.
# The wizard calls this script once per -Step so each step's console output can
# be shown on the progress page. Without -Step everything runs in one go
# (manual CLI use).
param(
    [string] $PackageDir,
    [ValidateSet('validate', 'cert', 'install', 'verify', 'uninstall')] [string] $Step,
    [ValidateSet('x86', 'x64', 'arm64')] [string] $PackageArchitecture = 'x64',
    [string] $ExpectedPackageVersion,
    [switch] $Launch
)

$ErrorActionPreference = 'Stop'
$logDir = Join-Path $env:LOCALAPPDATA 'AudioPlaybackConnector2'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$logPath = Join-Path $logDir 'install.log'
if ((Test-Path -LiteralPath $logPath) -and (Get-Item -LiteralPath $logPath).Length -gt 2MB) {
    Move-Item -LiteralPath $logPath -Destination "$logPath.1" -Force
}
Start-Transcript -Path $logPath -Append | Out-Null

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
            Set-Content -LiteralPath (Join-Path $Dir '.certificate-added') -Value $cert.Thumbprint -Encoding ASCII
            Write-Host "Certificate $($cert.Thumbprint) added to CurrentUser\TrustedPeople."
        } else {
            Write-Host 'Certificate already trusted, skipping.'
        }
    } finally {
        $store.Close()
    }
}

function Remove-NewlyImportedCertificate {
    param([string] $Dir)
    $markerPath = Join-Path $Dir '.certificate-added'
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) { return }
    $thumbprint = (Get-Content -LiteralPath $markerPath -Raw).Trim()
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'CurrentUser')
    $store.Open('ReadWrite')
    try {
        $store.Certificates.Find('FindByThumbprint', $thumbprint, $false) | ForEach-Object { $store.Remove($_) }
    } finally {
        $store.Close()
        Remove-Item -LiteralPath $markerPath -Force -ErrorAction SilentlyContinue
    }
    Write-Host "Rolled back certificate $thumbprint after installation failure."
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
        if ($ExpectedPackageVersion -and $identity.Version -ne $ExpectedPackageVersion) {
            throw "Unexpected package version $($identity.Version); expected $ExpectedPackageVersion."
        }
        Write-Host "Package identity verified: $($identity.Name) $($identity.Version) $($identity.ProcessorArchitecture)."
    } finally {
        $archive.Dispose()
    }
}

function Assert-AppPackageSignature {
    param([string] $Path, $Certificate)
    Add-Type -AssemblyName System.Security
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $archive.GetEntry('AppxSignature.p7x')
        if (-not $entry) { throw 'AppxSignature.p7x is missing from the app package.' }
        $stream = $entry.Open()
        $memory = [System.IO.MemoryStream]::new()
        try {
            $stream.CopyTo($memory)
            $bytes = $memory.ToArray()
        } finally {
            $memory.Dispose()
            $stream.Dispose()
        }
    } finally {
        $archive.Dispose()
    }
    if ($bytes.Length -le 4 -or [System.Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'PKCX') {
        throw 'The app package signature has an invalid header.'
    }
    $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
    $cms.Decode($bytes[4..($bytes.Length - 1)])
    $cms.CheckSignature($true)
    $signers = @($cms.SignerInfos | ForEach-Object { $_.Certificate })
    if ($signers.Count -ne 1 -or $signers[0].Thumbprint -ne $Certificate.Thumbprint) {
        throw 'The app package signer does not match the bundled certificate.'
    }
    Write-Host "Package signer verified: $($Certificate.Thumbprint)."
}

function Get-AppDependencies {
    param([string] $Dir)
    $depsRoot = Join-Path $Dir 'Dependencies'
    if (Test-Path $depsRoot) {
        $archDir = Join-Path $depsRoot $PackageArchitecture
        $searchDir = if (Test-Path $archDir) { $archDir } else { $depsRoot }
        return @(Get-ChildItem -Path $searchDir -Recurse -File |
            Where-Object { $_.Extension -in '.msix', '.appx' })
    }
    return @(Get-ChildItem -Path $Dir -File | Where-Object {
            ($_.Extension -in '.msix', '.appx') -and ($_.Name -notmatch '^AudioPlaybackConnector2_') })
}

function Assert-AppDependencies {
    param([string] $Dir)
    $expectedPublisher = 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US'
    $allowedNames = @(
        'Microsoft.VCLibs.140.00',
        'Microsoft.VCLibs.140.00.UWPDesktop',
        'Microsoft.WindowsAppRuntime.2'
    )
    $deps = @(Get-AppDependencies -Dir $Dir)
    $identities = foreach ($dep in $deps) {
        $archive = [System.IO.Compression.ZipFile]::OpenRead($dep.FullName)
        try {
            $manifestEntry = $archive.GetEntry('AppxManifest.xml')
            $signatureEntry = $archive.GetEntry('AppxSignature.p7x')
            if (-not $manifestEntry -or -not $signatureEntry) {
                throw "Dependency '$($dep.Name)' is missing its manifest or signature."
            }
            $reader = [System.IO.StreamReader]::new($manifestEntry.Open())
            try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
            $stream = $signatureEntry.Open()
            $memory = [System.IO.MemoryStream]::new()
            try {
                $stream.CopyTo($memory)
                $signatureBytes = $memory.ToArray()
            } finally {
                $memory.Dispose()
                $stream.Dispose()
            }
        } finally {
            $archive.Dispose()
        }
        $identity = $manifest.Package.Identity
        if ($allowedNames -notcontains [string]$identity.Name -or
            [string]$identity.Publisher -ne $expectedPublisher -or
            [string]$identity.ProcessorArchitecture -ne $PackageArchitecture) {
            throw "Unexpected dependency identity in '$($dep.Name)'."
        }
        if ($signatureBytes.Length -le 4 -or
            [System.Text.Encoding]::ASCII.GetString($signatureBytes, 0, 4) -ne 'PKCX') {
            throw "Dependency '$($dep.Name)' has an invalid signature header."
        }
        $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
        $cms.Decode($signatureBytes[4..($signatureBytes.Length - 1)])
        $cms.CheckSignature($true)
        $signers = @($cms.SignerInfos | ForEach-Object { $_.Certificate })
        if ($signers.Count -ne 1 -or $signers[0].Subject -ne [string]$identity.Publisher) {
            throw "Dependency signer does not match its manifest publisher in '$($dep.Name)'."
        }
        [string]$identity.Name
    }
    $duplicates = @($identities | Group-Object | Where-Object Count -gt 1)
    if ($duplicates.Count -gt 0) {
        throw "Duplicate dependency identities found: $($duplicates.Name -join ', ')."
    }
    if ($deps.Count -gt 0) {
        Write-Host "$($deps.Count) Microsoft dependency package(s) verified."
    }
}

function Install-AppPackage {
    param([string] $Dir)
    $package = Get-AppPackage -Dir $Dir
    $certificate = Get-AppCertificate -Dir $Dir
    $deps = @(Get-AppDependencies -Dir $Dir)
    $addParams = @{ Path = $package.FullName; ErrorAction = 'Stop' }
    if ($deps.Count -gt 0) {
        $addParams['DependencyPath'] = $deps.FullName
        Write-Host "Dependencies: $($deps.Count) package(s) for $PackageArchitecture."
    }
    Assert-AppPackageIdentity -Path $package.FullName
    Assert-AppPackageSignature -Path $package.FullName -Certificate $certificate
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
    Assert-AppPackageSignature -Path $package.FullName -Certificate $cert
    Assert-AppDependencies -Dir $Dir
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
    if ($PackageDir -and $Step -in 'install', 'verify') {
        Remove-NewlyImportedCertificate -Dir $PackageDir
    }
    Write-Host "FAILED: $($_.Exception.Message)"
    exit 1
} finally {
    Stop-Transcript | Out-Null
}
