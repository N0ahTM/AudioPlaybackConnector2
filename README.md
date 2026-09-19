[![Build](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml)
[![CodeQL](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml)
[![GitHub release (latest by date)](https://img.shields.io/github/v/release/N0ahTM/AudioPlaybackConnector2)](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/N0ahTM/AudioPlaybackConnector2/total?label=downloads)](https://github.com/N0ahTM/AudioPlaybackConnector2/releases)
[![License](https://img.shields.io/github/license/N0ahTM/AudioPlaybackConnector2)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B)](https://en.cppreference.com/)

<a href="https://get.microsoft.com/installer/download/9n366pgkjz0k?referrer=appbadge" target="_self">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200" alt="Download from the Microsoft Store">
</a>

AudioPlaybackConnector2 is a small Windows tray app that lets you play audio from a paired Bluetooth source, such as a phone, through your PC. It uses the Windows [AudioPlaybackConnection API](https://learn.microsoft.com/windows/apps/develop/media-playback/enable-remote-audio-playback) to manage A2DP sink connections from the tray.

Built with **WinUI 3 Desktop** and **C++/WinRT** and distributed as a per-user Windows package.

![Tray device picker, compact device options, and settings](https://github.com/user-attachments/assets/b530f478-8266-496c-9686-61714ecf16ef)

## Quick Start

1. Pair your phone or another compatible Bluetooth audio source with your Windows PC.
2. Install AudioPlaybackConnector2 from the [Microsoft Store](https://apps.microsoft.com/detail/9n366pgkjz0k).
3. Launch AudioPlaybackConnector2 and use the tray icon to manage devices.

For commands and certificate-store options, see [Installation](docs/INSTALLATION.md).

## Features

- Tray-only workflow with no main window.
- Fast device picker on left-click.
- Connect, reconnect, and disconnect devices from the picker.
- Double-click the tray icon to toggle the configured default device, or the most recently connected device.
- Disconnect or reconnect all active devices from the picker when multiple devices are connected.
- Compact device options behind each device's **…** button, with an alias, default-device selection, and connection policies.
- Separate global and per-device policies for connecting on app startup and reconnecting after an unexpected connection loss.
- Optional incoming-connection mode that keeps the Windows A2DP sink ready, so a paired phone can connect and disconnect from its Bluetooth menu.
- Device aliases for cleaner picker, notification, and command-line labels.
- Privacy mode to redact real device names and IDs in UI, CLI output, and diagnostics.
- Support and diagnostics tools for Bluetooth settings, log folder access, and redacted bug-report details.
- Guarded device actions so repeated clicks do not start overlapping connect/disconnect work.
- Animated, theme-aware tray icons for idle, connecting, connected, and error states.
- Toast notifications for connection events and failures.
- Optional start with Windows.
- Microsoft Store or Windows App Installer manages updates, depending on the installation path; the app does not check for updates itself.
- Settings window placement persistence.
- Local crash reports and minidumps for troubleshooting.
- Localized UI in eight languages.

## Known limitations

- Audio can occasionally remain silent even while the device appears connected, including after a playback pause. Use the circular-arrow **Reconnect** action to restore playback. The cause is still under investigation in [issue #1](https://github.com/N0ahTM/AudioPlaybackConnector2/issues/1).
- Windows may keep A2DP sink audio on the default output despite a per-app choice in Volume Mixer. See [issue #13](https://github.com/N0ahTM/AudioPlaybackConnector2/issues/13) and [Troubleshooting](docs/TROUBLESHOOTING.md).

## Requirements

- Windows 10 version 2004 (build 19041) or newer.
- A Bluetooth adapter with A2DP support.
- A paired Bluetooth audio source that supports sending A2DP audio to Windows.

## Usage

- **Open device picker:** Left-click the tray icon.
- **Open tray menu:** Right-click the tray icon.
- **Connect or disconnect:** Click a device name. A connected device shows a disconnect symbol when hovered or focused with the keyboard.
- **Reconnect:** Use the circular-arrow button beside a connected device.
- **Device options:** Click **…** beside a device. Use the back arrow to return to the list.
- **Quick toggle:** Double-click the tray icon to toggle the default device, falling back to the last connected device.
- **Settings:** Use the gear in the picker or the tray menu for language, startup, connection policies, notifications, privacy, and window placement.
- **Help:** Choose **Help** in the tray menu or **? Help** in Settings. Diagnostics are grouped in an expandable section; the installed version is shown in Settings. Updates are managed by Microsoft Store or Windows App Installer, not by an in-app button.
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

The Microsoft Store is the recommended installation path. It provides Microsoft-signed packages and automatic updates for x64 and ARM64 devices.

GitHub releases provide an advanced `.appinstaller` path with automatic updates, a manual x64/ARM64 `.msixbundle` fallback, and the source code. These packages use a separate self-signed identity. Follow the [installation guide](docs/INSTALLATION.md) to trust the certificate under **Trusted People** and install the exact framework dependencies. The old Inno Setup executables are no longer produced.

## Documentation

- [Usage](docs/USAGE.md)
- [Command-line reference](docs/CLI.md)
- [Installation](docs/INSTALLATION.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Translating](docs/TRANSLATING.md)
- [Contributing](CONTRIBUTING.md)
- [Releasing](docs/RELEASING.md)
- [Security policy](SECURITY.md)
- [Changelog](CHANGELOG.md)

Use [GitHub Discussions](https://github.com/N0ahTM/AudioPlaybackConnector2/discussions) for questions, ideas, translation coordination, and automation recipes. Use [GitHub Issues](https://github.com/N0ahTM/AudioPlaybackConnector2/issues) for reproducible defects and accepted work.

## Privacy and Crash Reports

- Settings are stored locally in the current user profile.
- Settings support only the current file format. After an incompatible format change, preferences reset to defaults
  and must be configured again; the original file is retained as a backup. Old preferences are not migrated.
- No telemetry is sent by the app.
- The app does not query GitHub release metadata or perform update checks.
- Privacy mode can redact real Bluetooth device names and IDs from the UI, command-line output, and copied diagnostics.
- Crash reports and minidumps stay on your machine unless you choose to share them.
- Minidumps may contain sensitive memory; review them before attaching them to an issue.

## Supported Languages

Translation improvements are welcome; see the [translation guide](docs/TRANSLATING.md).

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
