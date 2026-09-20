[![Build](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/build.yml)
[![CodeQL](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml/badge.svg)](https://github.com/N0ahTM/AudioPlaybackConnector2/actions/workflows/codeql.yml)
[![GitHub release (latest by date)](https://img.shields.io/github/v/release/N0ahTM/AudioPlaybackConnector2)](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/N0ahTM/AudioPlaybackConnector2/total?label=downloads)](https://github.com/N0ahTM/AudioPlaybackConnector2/releases)
[![License](https://img.shields.io/github/license/N0ahTM/AudioPlaybackConnector2)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-26_draft-00599C?logo=c%2B%2B)](https://en.cppreference.com/)

<a href="https://get.microsoft.com/installer/download/9n366pgkjz0k?referrer=appbadge" target="_self">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200" alt="Download from the Microsoft Store">
</a>

AudioPlaybackConnector2 plays audio from a paired Bluetooth source through a Windows PC using the [AudioPlaybackConnection API](https://learn.microsoft.com/windows/apps/develop/media-playback/enable-remote-audio-playback). It is a per-user tray app built with WinUI 3 and C++/WinRT.

![Tray picker and settings](https://github.com/user-attachments/assets/b530f478-8266-496c-9686-61714ecf16ef)

## Quick Start

1. Pair your Bluetooth audio source with Windows.
2. Install from the [Microsoft Store](https://apps.microsoft.com/detail/9n366pgkjz0k).
3. Left-click the tray icon; select a device to connect or disconnect. Use the circular arrow to reconnect, **…** for device options, or the gear for Settings.

Requires Windows 10 2004 (build 19041) or newer, an A2DP-capable Bluetooth adapter and a paired A2DP source.

## Features

- Tray-only picker; double-click toggles the default or last connected device. Bulk reconnect/disconnect actions are available for multiple connections.
- Device aliases; global/per-device startup and reconnect policies; optional incoming connections initiated by the phone.
- Privacy redaction, diagnostics, local crash reports, connection notifications and animated theme-aware tray status.
- Start with Windows, saved settings-window placement and eight UI languages: English, German, French, Spanish, Japanese, Korean, Simplified and Traditional Chinese.
- Scriptable `apc2ctl` CLI; updates managed by Microsoft Store or Windows App Installer.

```powershell
apc2ctl list --json
apc2ctl connect --name "Device Name"
apc2ctl toggle --default
```

See [Usage](docs/USAGE.md) and the [CLI reference](docs/CLI.md) for controls, selectors, privacy and exit codes.

## Known limitations

- Connected devices may remain silent after a pause; collect diagnostics, then reconnect. Investigation: [#1](https://github.com/N0ahTM/AudioPlaybackConnector2/issues/1).
- Windows may ignore a per-app A2DP Sink output choice and use the default output. See [#13](https://github.com/N0ahTM/AudioPlaybackConnector2/issues/13) and [Troubleshooting](docs/TROUBLESHOOTING.md).

## Installation Notes

The Store provides signed x64/ARM64 packages, dependencies and automatic updates. GitHub offers a separately identified, self-signed `.appinstaller` with automatic updates or a manual `.msixbundle`. Follow [Installation](docs/INSTALLATION.md) for certificate trust and exact dependencies. Inno Setup installers are no longer produced.

## Privacy and Crash Reports

Settings stay in the current user profile. Only the current format is supported: incompatible files are backed up and preferences reset, without migration. The app sends no telemetry or update queries. Privacy mode redacts known device details; CLI `--raw` explicitly requests real identifiers. Reports and minidumps remain local unless shared; minidumps may contain sensitive memory.

## Documentation

[Documentation index](docs/README.md) · [Contributing](CONTRIBUTING.md) · [Security policy](SECURITY.md) · [Changelog](CHANGELOG.md)

Use [Discussions](https://github.com/N0ahTM/AudioPlaybackConnector2/discussions) for questions, ideas and translation coordination; [Issues](https://github.com/N0ahTM/AudioPlaybackConnector2/issues) for reproducible defects.

## Credits

Inspired by [ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector).

## License

[MIT](LICENSE).
