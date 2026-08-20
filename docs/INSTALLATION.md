# Installation

AudioPlaybackConnector2 supports three installation paths:

- Build from source (developer workflow)
- Install with the web or offline bootstrapper (recommended end-user workflow)
- Install the published MSIX release manually (advanced workflow)

For local development, see [Developer setup and build](DEV_SETUP.md).

## Install with Setup

Each GitHub release provides:

- `AudioPlaybackConnector2-WebSetup.exe`, which downloads the current release payload
- `AudioPlaybackConnector2-Setup-<version>.exe`, which contains the complete offline payload

Both variants install per-user without administrator rights. They import the pinned release certificate into
`Cert:\CurrentUser\TrustedPeople`, install the MSIX and its framework dependencies, and launch the app after an
interactive installation. The setup executable itself is not yet code-signed, so Windows SmartScreen may display
an unknown-publisher warning.

## Manual MSIX Installation

Each GitHub release provides:

- `.appinstaller`
- `.msix`
- `.cer` certificate
- framework dependency packages

### 1) Trust the release certificate

Right-click the `.cer` file and choose **Install Certificate**, or use PowerShell:

```powershell
Import-Certificate -FilePath ".\AudioPlaybackConnector2.cer" -CertStoreLocation "Cert:\CurrentUser\TrustedPeople"
```

Machine-wide trust is optional and requires an elevated PowerShell session:

```powershell
Import-Certificate -FilePath ".\AudioPlaybackConnector2.cer" -CertStoreLocation "Cert:\LocalMachine\Root"
```

### 2) Install via App Installer

Download the App Installer feed and open the local `.appinstaller` file:

```powershell
$installer = Join-Path $env:TEMP "AudioPlaybackConnector2.appinstaller"
Invoke-WebRequest -Uri "https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller" -OutFile $installer
Start-Process $installer
```

Opening a downloaded `.appinstaller` avoids the `ms-appinstaller:` web protocol, which Microsoft has disabled by
default on consumer devices since December 2023. The protocol can be re-enabled by enterprise policy, but the local
`.appinstaller` flow is the supported path for general GitHub release distribution.

The `.appinstaller` automatically pulls in required framework dependencies (VCLibs and Windows App SDK runtime).
Because these are system framework packages, Windows may prompt for administrator approval during installation.

See:

- [Installing Windows apps from a web page](https://learn.microsoft.com/windows/msix/app-installer/installing-windows10-apps-web)
- [DesktopAppInstaller policy CSP](https://learn.microsoft.com/windows/client-management/mdm/policy-csp-desktopappinstaller)

### Optional fallback: direct MSIX install

`Add-AppxPackage` works as a fallback, but it does not preserve update-feed behavior from `.appinstaller`.
If you install the `.msix` directly, any missing framework dependencies will be reported and must be resolved manually.

## Build and run locally

Open `AudioPlaybackConnector2.slnx`, choose `Release | x64`, build, then run/install the generated package.

Local builds use a temporary developer certificate, so manual certificate installation is usually not needed.

## Notes on signing

Releases are currently signed with a self-signed certificate. Installing the `.cer` is required before the `.msix` can be trusted by Windows.
