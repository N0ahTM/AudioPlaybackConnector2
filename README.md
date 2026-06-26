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

AudioPlaybackConnector2 is a small Windows tray app for connecting paired Bluetooth audio devices as Windows playback outputs. It uses the Windows `AudioPlaybackConnection` API so you can connect, disconnect, and reconnect A2DP devices without opening Windows Settings.

<img width="90%" alt="AudioPlaybackConnector2 device picker" src="https://github.com/user-attachments/assets/e0e2724a-82d3-40e7-849c-e6a870c0eeca" />

Built with **WinUI 3 Desktop**, **C++/WinRT**, and distributed as **MSIX**.

## Quick Start

1. Pair your Bluetooth speaker, headset, or other A2DP audio device in Windows.
2. Download the latest `.appinstaller` and `.cer` from [Releases](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest).
3. Trust the `.cer` certificate once.
4. Open the `.appinstaller` file to install the app with its update feed.
5. Launch AudioPlaybackConnector2 and use the tray icon to manage devices.

For commands and certificate-store options, see [Installation](docs/INSTALLATION.md).

## Features

- Tray-only workflow with no main window.
- Fast device picker on left-click.
- Connect, reconnect, and disconnect devices from the picker.
- Double-click the tray icon to toggle the most recently connected device.
- Disconnect or reconnect all active devices from the tray menu.
- Global and per-device auto-reconnect on startup.
- Guarded device actions so repeated clicks do not start overlapping connect/disconnect work.
- Animated, theme-aware tray icons for idle, connecting, connected, and error states.
- Toast notifications for connection events, failures, and available updates.
- Optional start with Windows.
- Manual update checks through GitHub Releases and the App Installer feed.
- Settings window placement persistence.
- Local crash reports and minidumps for troubleshooting.
- Localized UI in eight languages.

## Requirements

- Windows 10 version 2004 or newer.
- A Bluetooth adapter with A2DP support.
- A paired Bluetooth audio playback device.
- MSIX installation through a release package or local build.

## Usage

- **Open device picker:** Left-click the tray icon.
- **Open tray menu:** Right-click the tray icon.
- **Quick toggle:** Double-click the tray icon to connect or disconnect the last connected device.
- **Settings:** Open from the tray menu to configure language, startup behavior, reconnect behavior, notifications, and update checks.
- **Command line:** Use `apc2ctl.exe` from PowerShell, MacroPads, Stream Deck actions, or scripts:

```powershell
apc2ctl status
apc2ctl list
apc2ctl connect --name "Device Name"
apc2ctl connect --mac "A1:B2:C3:D4:E5:F6"
apc2ctl disconnect --id "<Windows device id>"
apc2ctl toggle --last
apc2ctl reconnect-all
```

Add `--json` to `status`, `list`, or device actions for machine-readable output.

## Installation Notes

The recommended install path is the `.appinstaller` file from the latest release. It preserves update-feed behavior and pulls in required framework packages such as VCLibs and the Windows App SDK runtime.

Direct `.msix` installation is still possible, but missing framework dependencies may need to be installed manually and update-feed behavior is not preserved.

Releases are currently signed with a self-signed certificate, so Windows must trust the release `.cer` before installation.

## Documentation

- [Installation](docs/INSTALLATION.md)
- [Developer setup and build](docs/DEV_SETUP.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Contributing](CONTRIBUTING.md)
- [Changelog](CHANGELOG.md)

## Privacy and Crash Reports

- Settings are stored locally in the current user profile.
- No telemetry is sent by the app.
- Update checks query GitHub release metadata.
- Crash reports and minidumps stay on your machine unless you choose to share them.
- Minidumps may contain sensitive memory; review them before attaching them to an issue.

## Supported Languages

- English
- German
- French
- Spanish
- Japanese
- Korean
- Chinese Simplified
- Chinese Traditional

## Credits

Inspired by [ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector).

## License

MIT License. See [LICENSE](LICENSE).
