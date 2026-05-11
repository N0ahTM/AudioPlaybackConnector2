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

<img width="600" alt="AudioPlaybackConnector2 device picker" src="https://github.com/user-attachments/assets/8eba7e86-4e53-46c8-b495-44aef7599460" />

The app is built with **WinUI 3 Desktop**, **C++/WinRT**, and packaged as an **MSIX**.

Inspired by the original [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector).

---

## Features

- System tray-only application
- Left-click tray device picker
- Right-click tray context menu
- Connect Bluetooth A2DP audio devices as playback sinks
- Disconnect connected devices directly from the picker
- Auto-reconnect on startup
- Per-device auto-reconnect settings
- Connected device names shown in the tray tooltip
- Animated tray icon while connecting
- Theme-aware tray icons for light and dark taskbars
- JSON-based settings persistence
- Multi-language support
- Optional startup with Windows (disabled by default)

---

## Requirements

- Windows 10 version 1809 / build 17763 or later
- Windows 11 supported
- Bluetooth adapter with A2DP support
- MSIX packaged installation
- `runFullTrust` capability

---

## Installation

There are exactly two supported ways to install and use AudioPlaybackConnector2.

### Option 1: Build from Source (Recommended for Developers)

1. Open `AudioPlaybackConnector2.slnx` in Visual Studio 2022.
2. Select `Release | x64`.
3. Build the solution or the packaging project.
4. Run the app directly from the build output or install the generated MSIX locally.

Local builds use a temporary developer certificate, so no manual certificate installation is required.

### Option 2: Install the Released MSIX Package (End Users)

Each GitHub Release contains two files: a `.cer` certificate and a `.msix` package.

**Step 1 — Install the certificate**

Right-click the `.cer` file and select **Install Certificate**, or run PowerShell as Administrator:

```powershell
Import-Certificate -FilePath ".\AudioPlaybackConnector2.cer" -CertStoreLocation "Cert:\LocalMachine\Root"
```

> **Note:** If you prefer to trust the certificate only for your user account, use `Cert:\CurrentUser\TrustedPeople` instead of `Cert:\LocalMachine\Root`.

**Step 2 — Install the MSIX package**

```powershell
Add-AppxPackage -Path ".\AudioPlaybackConnector2.msix"
```

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

Connected devices show a disconnect button directly in the picker.

### Open the tray menu

Right-click the tray icon.

Available actions:

- Open Settings
- Open Windows Bluetooth Settings
- Exit

### Auto-reconnect

Auto-reconnect can be enabled globally and per device.

When enabled, the app attempts to reconnect known devices automatically when they become available again.

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
- Windows App SDK
- Windows SDK 10.0.26100.0 or later
- NuGet package restore enabled

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
