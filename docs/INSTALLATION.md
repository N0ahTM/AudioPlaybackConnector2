# Installation

AudioPlaybackConnector2 supports two installation paths:

- Build from source (developer workflow)
- Install the published MSIX release (end-user workflow)

For local development, see [Developer setup and build](DEV_SETUP.md).

## Install Released MSIX

Each GitHub release provides:

- `.appinstaller`
- `.msix`
- `.cer` certificate

### 1) Install runtime dependencies (once)

1. Install [WinAppRuntime.Singleton](https://apps.microsoft.com/detail/9p5z076k079h).
2. Install the [Windows App SDK 2.0 runtime](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/downloads#windows-app-sdk-20).

### 2) Trust the release certificate

Right-click the `.cer` file and choose **Install Certificate**, or use PowerShell:

```powershell
Import-Certificate -FilePath ".\AudioPlaybackConnector2.cer" -CertStoreLocation "Cert:\LocalMachine\Root"
```

If you only want per-user trust, use:

```powershell
Import-Certificate -FilePath ".\AudioPlaybackConnector2.cer" -CertStoreLocation "Cert:\CurrentUser\TrustedPeople"
```

### 3) Install via App Installer

```powershell
Start-Process "ms-appinstaller:?source=https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller"
```

If protocol launch does not work, open the downloaded `.appinstaller` file directly.

### Optional fallback: direct MSIX install

`Add-AppxPackage` works as a fallback, but it does not preserve update-feed behavior from `.appinstaller`.

## Build and run locally

Open `AudioPlaybackConnector2.slnx`, choose `Release | x64`, build, then run/install the generated package.

Local builds use a temporary developer certificate, so manual certificate installation is usually not needed.

## Notes on signing

Releases are currently signed with a self-signed certificate. Installing the `.cer` is required before the `.msix` can be trusted by Windows.
