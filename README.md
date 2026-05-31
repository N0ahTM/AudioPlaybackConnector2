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

AudioPlaybackConnector2 is a Windows system-tray app that connects Bluetooth devices as A2DP playback sinks via `AudioPlaybackConnection`.

<img width="600" alt="AudioPlaybackConnector2 device picker" src="https://github.com/user-attachments/assets/e0e2724a-82d3-40e7-849c-e6a870c0eeca" />

Built with **WinUI 3 Desktop**, **C++/WinRT**, and distributed as **MSIX**.

## Quick Navigation

- [Installation](docs/INSTALLATION.md)
- [Developer setup and build](docs/DEV_SETUP.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Contributing](CONTRIBUTING.md)
- [Changelog](CHANGELOG.md)

## Features

- Tray-only workflow (no main window)
- Device picker on left-click
- Connect, reconnect, and disconnect from the picker
- Double-click tray icon to toggle the last connected device
- Global and per-device auto-reconnect
- Animated, theme-aware tray icons
- Multi-language UI
- Optional startup with Windows
- Manual update check via GitHub releases and App Installer feed

## Requirements

- Windows 10 version 1809 (build 17763) or newer
- Bluetooth adapter with A2DP support
- Windows App SDK Runtime 2.0.1 or newer
- Microsoft Windows App Runtime Singleton
- MSIX installation (release or local package)

## Usage

- **Open device picker:** Left-click the tray icon.
- **Open tray menu:** Right-click the tray icon.
- **Quick toggle:** Double-click the tray icon to toggle the last connected device.
- **Settings:** Open via tray menu for language, startup, reconnect, and update options.

## Privacy and Crash Reports

- Settings are stored locally in the current user profile.
- No telemetry is sent by the app.
- Update checks query GitHub release metadata.
- Crash dumps may contain sensitive memory; review before sharing.

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
