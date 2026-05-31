# AudioPlaybackConnector2

[![Build](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml)
[![CodeQL](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml)
[![GitHub release (latest by date)](https://img.shields.io/github/v/release/N0ahTM/AudioPlaybackConnector2)](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest)
[![License](https://img.shields.io/github/license/N0ahTM/AudioPlaybackConnector2)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B)](https://en.cppreference.com/)
<p align="center">
  <a href="https://github.com/N0ahTM/AudioPlaybackConnector2">
    <img src="https://img.shields.io/badge/⭐_Star_this_project-2d333b?style=for-the-badge&logo=github&logoColor=white&labelColor=1f2328" alt="Star this project">
  </a>
</p>

---

AudioPlaybackConnector2 is a Windows system-tray application for connecting Bluetooth audio devices as A2DP audio sinks using the Win32 `AudioPlaybackConnection` API.

<img width="600" alt="AudioPlaybackConnector2 device picker" src="https://github.com/user-attachments/assets/e0e2724a-82d3-40e7-849c-e6a870c0eeca" />

The app is built with **WinUI 3 Desktop**, **C++/WinRT**, and packaged as an **MSIX**.

Inspired by the original [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector).

---

## Features

- System tray-only application
- Left-click tray device picker
- Double-click the tray icon to toggle the last connected device
- Right-click tray context menu
- Connect Bluetooth A2DP audio devices as playback sinks
- Disconnect connected devices directly from the picker
- Reconnect connected devices directly from the picker
- Auto-reconnect on startup
- Per-device auto-reconnect settings
- Connected device names shown in the tray tooltip
- Animated tray icon while connecting
- Theme-aware tray icons for light and dark taskbars
- JSON-based settings persistence
- Multi-language support
- Optional startup with Windows (disabled by default)
- Manual GitHub release update check in Settings
- App Installer update feed for MSIX installs

---

## Requirements

- Windows 10 version 1809 / build 17763 or later
- Windows 11 supported
- Bluetooth adapter with A2DP support
- Windows App SDK Runtime 2.0.1 or later
- Microsoft Windows App Runtime Singleton
- MSIX packaged installation
- `runFullTrust` capability

---

## Windows App SDK Runtime

AudioPlaybackConnector2 currently uses **Windows App SDK Runtime 2.0.1** and **WinUI 2.0.12**.

If launching the installed MSIX package or running the packaging project from Visual Studio fails with `DEP0840` and mentions missing `MicrosoftCorporationII.WinAppRuntime.Main.2` or `MicrosoftCorporationII.WinAppRuntime.Singleton` packages, install these runtime dependencies once:

1. Install [WinAppRuntime.Singleton](https://apps.microsoft.com/detail/9p5z076k079h) from Microsoft Store.
2. Install the [Windows App SDK 2.0 runtime](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/downloads#windows-app-sdk-20).

After both runtime components are installed, install or launch AudioPlaybackConnector2 again.

---

## Installation

There are exactly two supported ways to install and use AudioPlaybackConnector2.

### Option 1: Build from Source (Recommended for Developers)

1. Open `AudioPlaybackConnector2.slnx` in Visual Studio 2022.
2. Select `Release | x64`.
3. Make sure the Windows App SDK runtime dependencies listed above are installed.
4. Build the solution or the packaging project.
5. Run the app directly from the build output or install the generated MSIX locally.

Local builds use a temporary developer certificate, so no manual certificate installation is required.

### Option 2: Install the Released MSIX Package (End Users)

Each GitHub Release contains a `.cer` certificate, an `.appinstaller` file, and a direct `.msix` package.

**Step 1 — Install the Windows App SDK runtime dependencies**

Install [WinAppRuntime.Singleton](https://apps.microsoft.com/detail/9p5z076k079h), then install the [Windows App SDK 2.0 runtime](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/downloads#windows-app-sdk-20).

**Step 2 — Install the certificate**

Right-click the `.cer` file and select **Install Certificate**, or run PowerShell as Administrator:

```powershell
Import-Certificate -FilePath ".\AudioPlaybackConnector2.cer" -CertStoreLocation "Cert:\LocalMachine\Root"
```

> **Note:** If you prefer to trust the certificate only for your user account, use `Cert:\CurrentUser\TrustedPeople` instead of `Cert:\LocalMachine\Root`.

**Step 3 — Install through App Installer**

```powershell
Start-Process "ms-appinstaller:?source=https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller"
```

Installing through the `.appinstaller` feed lets Windows App Installer check for future updates. If Windows does not open App Installer from the protocol link, download and open the `.appinstaller` asset directly. Direct MSIX sideloading with `Add-AppxPackage` remains available as a fallback, but direct MSIX installs do not remember the update feed.

After installation, start **AudioPlaybackConnector2** from the Start Menu.

The app runs in the system tray and does not open a normal main window.

---

### Signing & Certificates

Releases are currently signed with a self-signed certificate. This is why you must install the `.cer` file before the `.msix` — Windows blocks installation of packages from untrusted publishers.

> **Disclaimer:** I have applied for a free open-source code-signing certificate. Approval and provisioning may take some time. Until then, the self-signed certificate is the only way to distribute installable MSIX packages.

---

## Usage

### Open the device picker

Left-click the tray icon.

The device picker shows available Bluetooth A2DP audio devices. Select a device to connect it as an audio playback sink.

Connected devices show reconnect and disconnect buttons directly in the picker.

While a device is connecting, reconnecting, or disconnecting, the row is shown as busy and actions for that device are disabled.

Double-clicking the tray icon toggles the most recently connected device. If that device is already connected, the app disconnects it; otherwise, the app connects it again.

### Open the tray menu

Right-click the tray icon.

Available actions:

- Open Settings
- Open Windows Bluetooth Settings
- Exit

### Auto-reconnect

Auto-reconnect can be enabled globally and per device.

When enabled, the app attempts to reconnect known devices automatically when they become available again.

### Updates

Use **Settings → Updates → Check for updates** to compare the installed app version with the latest GitHub Release. If a newer version is available, the app opens the App Installer feed so Windows can install the update.

When installed through `AudioPlaybackConnector2.appinstaller`, Windows also checks the update feed on app launch at the configured interval.

---

## Privacy and Crash Reports

AudioPlaybackConnector2 stores settings locally in the current user's profile. This includes paired device names, device IDs, reconnect preferences, language, startup preference, and update notification state.

The app does not send telemetry. Update checks only request GitHub release metadata, and App Installer updates use the fixed appinstaller feed URL shown above.

If the app crashes, it may create a local crash report and minidump. Minidumps can contain sensitive process memory, local paths, device names, or other private data. Do not upload dump files publicly unless you have reviewed them and are comfortable sharing their contents.

---

## Supported Languages

AudioPlaybackConnector2 currently includes string resources for:

- English
- German
- French
- Spanish
- Japanese
- Korean
- Chinese Simplified
- Chinese Traditional

---

## Tray Behavior

The tray icon reflects the current connection state:

| State | Behavior |
|------|----------|
| Idle | No active connection |
| Connecting | Animated icon |
| Connected | Connected-state icon |
| Error | Error-state icon and tooltip message |

The icon is rendered programmatically and adapts to light and dark Windows taskbar themes.

---

## Building from Source

### Prerequisites

- Visual Studio 2022 (17.14+ recommended)
- Desktop development with C++
- Windows application development workload for WinUI / Windows App SDK
- Windows App SDK Runtime 2.0.1 or later
- Microsoft Windows App Runtime Singleton
- Windows SDK 10.0.26100.0 or later
- NuGet package restore enabled

> **Visual Studio 2026 note:** If the project does not compile because C++23 language features are unavailable, set the C++ language standard to `/std:c++latest`. If the packaging project fails because the certificate thumbprint is not valid on your machine, open `Package.appxmanifest`, go to **Packaging**, choose or create a local test certificate, then update `PackageCertificateThumbprint` in `AudioPlaybackConnector2 (Package)`.

### Build in Visual Studio

Open:

```txt
AudioPlaybackConnector2.slnx
```

Select:

```txt
Configuration: Release
Platform: x64
```

Then build the solution or the packaging project.

### Command line build

```powershell
msbuild AudioPlaybackConnector2.slnx /p:Configuration=Release /p:Platform=x64 /t:Rebuild
```

The MSIX package is generated in the packaging project output folder under:

```txt
AppPackages/
```

### Continuous Integration

Every push and pull request is checked by GitHub Actions for:

- **clang-format** compliance (`--dry-run --Werror`)
- **CppCheck** static analysis (`--enable=warning,performance,portability`)
- Clean **Release/x64** build with `/W4 /WX`
- **CodeQL** security analysis

---

## Credits

Inspired by the original project:

[ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector)

---

## License

MIT License. See [LICENSE](LICENSE) for details.
