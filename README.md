# AudioPlaybackConnector2

[![Build](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml)
[![CodeQL](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml)
[![GitHub release (latest by date)](https://img.shields.io/github/v/release/N0ahTM/AudioPlaybackConnector2)](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest)
[![GitHub issues](https://img.shields.io/github/issues/N0ahTM/AudioPlaybackConnector2)](https://github.com/N0ahTM/AudioPlaybackConnector2/issues)
[![License](https://img.shields.io/github/license/N0ahTM/AudioPlaybackConnector2)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B)](https://en.cppreference.com/)

AudioPlaybackConnector2 is a Windows system-tray application for connecting Bluetooth audio devices as A2DP audio sinks using the Win32 `AudioPlaybackConnection` API.

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

---

## Requirements

- Windows 10 version 1809 / build 17763 or later
- Windows 11 supported
- Bluetooth adapter with A2DP support
- MSIX packaged installation
- `runFullTrust` capability

---

## Installation

Download the latest MSIX package from the GitHub Releases page.

Install it with PowerShell:

```powershell
Add-AppxPackage -Path ".\AudioPlaybackConnector2.msix"
```

After installation, start **AudioPlaybackConnector2** from the Start Menu.

The app runs in the system tray and does not open a normal main window.

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

---

## Credits

Inspired by the original project:

[ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector)

---

## License

Copyright (C) 2025 Noah Meyer.

MIT License. See [LICENSE](LICENSE) for details.