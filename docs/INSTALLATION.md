# Installation

AudioPlaybackConnector2 requires Windows 10 version 2004 (build 19041) or newer. Pair the Bluetooth audio device in Windows before launching the app.

## Recommended: Web Setup

1. Open the [latest GitHub release](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest).
2. Download `AudioPlaybackConnector2-WebSetup.exe`.
3. Run the setup and select **Install**.

Web Setup downloads the current x64 package and its dependencies, trusts the pinned release certificate for the current user, installs the app, and launches it. Administrator rights are not required.

The setup executable is not yet code-signed, so Windows SmartScreen may show an unknown-publisher warning. Only continue when the file came from this repository's release page.

## Offline Setup

Use `AudioPlaybackConnector2-Setup-<version>.exe` when the target computer has no internet connection. It contains the application package, certificate, and framework dependencies. Installation is otherwise identical to Web Setup.

Both setup variants can update or repair an installation. Their uninstall option removes the package and trusted certificate but keeps user settings in `%LOCALAPPDATA%`.

## App Installer and MSIX

The release also includes the App Installer feed, raw `.msix`, `.cer`, and dependency packages for existing installations and advanced use.

To use the App Installer feed, download and open the local file:

```powershell
$path = Join-Path $env:TEMP "AudioPlaybackConnector2.appinstaller"
Invoke-WebRequest -Uri "https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller" -OutFile $path
Start-Process $path
```

If Windows reports an untrusted publisher, import the certificate from the same release for the current user:

```powershell
Import-Certificate -FilePath ".\AudioPlaybackConnector2.cer" -CertStoreLocation "Cert:\CurrentUser\TrustedPeople"
```

Installing the raw `.msix` is a fallback. Missing framework dependencies must then be installed manually, and a direct MSIX installation does not register the App Installer update feed.

## Build from Source

Source builds are intended for contributors. See [Contributing](../CONTRIBUTING.md) for prerequisites, build commands, certificates, and checks.

For installation errors, see [Troubleshooting](TROUBLESHOOTING.md).
