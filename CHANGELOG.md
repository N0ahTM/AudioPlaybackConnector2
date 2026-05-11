# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Reusable self-signed certificate for all MSIX releases.
- Public `.cer` file attached to every GitHub Release so users can trust the certificate before installing the MSIX.

### Changed
- Updated installation instructions to reflect the two supported paths: build from source, or install `.cer` then `.msix`.
- Updated `Package.appxmanifest` publisher identity to `CN=AudioPlaybackConnector2` to match the release signing certificate.
- Release pipeline now signs MSIX packages using the repository's self-signed certificate instead of building unsigned packages.

## [0.3.0] - 2026-05-09

### Added
- Windows App SDK toast notifications for app start, connection, disconnection, auto-reconnect, and reconnect failure events.
- Notification actions to retry or reconnect devices directly from supported toast notifications.
- Status-specific toast icon assets in all packaged scale variants.
- Localized toast notification strings for all supported languages.

### Fixed
- Handle unexpected Bluetooth device removals in `DeviceManager::OnDeviceRemoved`.
- Derive the About dialog version from package metadata instead of hard-coded localized text.
- Prevent duplicate reconnect flows when a reconnect is already running for the same device.

### Changed
- Fall back to tray balloon notifications when Windows App SDK toast notifications are unavailable.
- Updated package notification metadata and third-party notices for toast notification support.
- Updated tray icon image color-matrix initialization for clearer structured initialization.
- Refactored `ThemeHelper` to use a strongly-typed `Theme` enum instead of `bool`.
- Tray icons now dynamically query system brush colors (`TextFillColorPrimaryBrush`, `SystemFillColorSuccessBrush`, etc.) instead of hardcoded light/dark palettes.
- Reduced tray icon storage from a `[theme][size]` matrix to `[size]` by removing the static light/dark dichotomy.
- Added deduplication in `ThemeHelper::OnSettingChange` so theme-changed handlers only fire when the theme actually changes.

## [0.2.0] - 2026-05-08

### Added
- Auto-reconnect backoff with retry limits to avoid repeated immediate reconnect attempts.
- Immediate settings persistence for settings changes to reduce data loss after crashes or forced exits.
- Project documentation files: `CONTRIBUTING.md`, `LICENSE`, and `THIRD_PARTY_NOTICES.md`.
- Initial `CHANGELOG.md`.
- Localized reconnect failure messages for all supported languages.

### Changed
- Improved code quality and maintainability in App, Device Picker, Settings, and Device Manager code.
- Updated packaging metadata, Store association files, and build configuration for release packaging.
- Refactored device enumeration UI handling to reduce duplicated async completion logic.

### Fixed
- Resolved CodeQL warnings in `DevicePickerView`.
- Fixed CI build, MSIX release, clang-format, CppCheck, and CodeQL workflow issues.
- Unified the Visual Studio platform toolset to `v143`.
- Added missing application manifest configuration required by CI/package builds.

## pre-release [0.1.0] - 2025-05-08

### Added
- Initial release of AudioPlaybackConnector2.
- System tray-only WinUI 3 Desktop application packaged as MSIX.
- Bluetooth A2DP audio sink connection via Win32 `AudioPlaybackConnection` API.
- Left-click device picker with connect / disconnect actions.
- Right-click tray context menu (Settings, Bluetooth Settings, Exit).
- Auto-reconnect on startup with per-device granularity.
- Animated tray icon during connection and theme-aware icons for light / dark taskbars.
- JSON-based settings persistence.
- Multi-language support for 8 locales: English, German, French, Spanish, Japanese, Korean, Chinese Simplified, Chinese Traditional.
- CI/CD workflows: build (with clang-format and CppCheck), CodeQL analysis, and automated MSIX releases on version tags.

[Unreleased]: https://github.com/N0ahTM/AudioPlaybackConnector2/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/N0ahTM/AudioPlaybackConnector2/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/N0ahTM/AudioPlaybackConnector2/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/N0ahTM/AudioPlaybackConnector2/releases/tag/v0.1.0
