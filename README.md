[![Build](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml)
[![CodeQL](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml)
[![GitHub release (latest by date)](https://img.shields.io/github/v/release/N0ahTM/AudioPlaybackConnector2)](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/N0ahTM/AudioPlaybackConnector2/total?label=downloads)](https://github.com/N0ahTM/AudioPlaybackConnector2/releases)
[![License](https://img.shields.io/github/license/N0ahTM/AudioPlaybackConnector2)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B)](https://en.cppreference.com/)

AudioPlaybackConnector2 is a small Windows tray app for connecting paired Bluetooth audio devices as Windows playback outputs. It uses the Windows `AudioPlaybackConnection` API so you can connect, disconnect, and reconnect A2DP devices without opening Windows Settings.

<img width="90%" alt="AudioPlaybackConnector2 device picker" src="https://github.com/user-attachments/assets/e0e2724a-82d3-40e7-849c-e6a870c0eeca" />

Built with **WinUI 3 Desktop** and **C++/WinRT** and distributed as a per-user Windows package.

## Quick Start

1. Pair your Bluetooth speaker, headset, or other A2DP audio device in Windows.
2. Download `AudioPlaybackConnector2-WebSetup.exe` from the [latest release](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest).
3. Run the setup. It installs the app and its signing certificate for the current user.
4. Launch AudioPlaybackConnector2 and use the tray icon to manage devices.

For commands and certificate-store options, see [Installation](docs/INSTALLATION.md).

## Features

- Tray-only workflow with no main window.
- Fast device picker on left-click.
- Connect, reconnect, and disconnect devices from the picker.
- Double-click the tray icon to toggle the configured default device, or the most recently connected device.
- Disconnect or reconnect all active devices from the tray menu.
- Separate global and per-device policies for connecting on app startup and reconnecting after an unexpected connection loss.
- Optional incoming-connection mode that keeps the Windows A2DP sink ready, so a paired phone can connect and disconnect from its Bluetooth menu.
- Device aliases for cleaner picker, notification, and command-line labels.
- Privacy mode to redact real device names and IDs in UI, CLI output, and diagnostics.
- Support and diagnostics tools for Bluetooth settings, log folder access, and redacted bug-report details.
- Guarded device actions so repeated clicks do not start overlapping connect/disconnect work.
- Animated, theme-aware tray icons for idle, connecting, connected, and error states.
- Toast notifications for connection events, failures, and available updates.
- Optional start with Windows.
- Manual update checks through GitHub Releases and the App Installer feed.
- Settings window placement persistence.
- Local crash reports and minidumps for troubleshooting.
- Localized UI in eight languages.

## Requirements

- Windows 10 version 2004 (build 19041) or newer.
- A Bluetooth adapter with A2DP support.
- A paired Bluetooth audio playback device.

## Usage

- **Open device picker:** Left-click the tray icon.
- **Open tray menu:** Right-click the tray icon.
- **Quick toggle:** Double-click the tray icon to connect or disconnect the last connected device.
- **Settings:** Open from the tray menu to configure language, startup behavior, reconnect behavior, notifications, and update checks.
- **Command line:** Use `apc2ctl.exe` from PowerShell, MacroPads, or scripts:

```powershell
apc2ctl status
apc2ctl list
apc2ctl show
apc2ctl settings
apc2ctl connect --name "Device Name"
apc2ctl disconnect --id "<Windows device id>"
apc2ctl toggle --default
apc2ctl default set --alias "Desk Speakers"
apc2ctl alias set --name "WH-1000XM5" --value "Headphones"
apc2ctl reconnect-all
```

Add `--json` for machine-readable output. Add `--raw` when you explicitly need real device IDs or names while privacy mode is enabled.

## Installation Notes

The recommended install path is `AudioPlaybackConnector2-WebSetup.exe` from the latest release. A versioned offline setup is also provided. Both bootstrapper variants install the signing certificate, app package, and required framework packages for the current user.

The `.appinstaller` feed and direct `.msix` package remain available for existing installations and manual fallback. Direct `.msix` installation may require framework dependencies to be installed manually and does not preserve update-feed behavior.

Releases are currently signed with a self-signed certificate. The setup installers establish per-user trust automatically; manual App Installer or MSIX installation requires importing the matching release `.cer` first.

## Documentation

- [Installation](docs/INSTALLATION.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Contributing](CONTRIBUTING.md)
- [Changelog](CHANGELOG.md)

## Privacy and Crash Reports

- Settings are stored locally in the current user profile.
- No telemetry is sent by the app.
- Update checks query GitHub release metadata.
- Privacy mode can redact real Bluetooth device names and IDs from the UI, command-line output, and copied diagnostics.
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
