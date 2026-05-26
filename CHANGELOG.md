# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.4.2] - 2026-05-26

### Added
- Crash handling with structured crash report logging for unexpected process failures.
- Extended diagnostic logging around reconnect/state transitions to simplify root-cause analysis.
- Refreshed app icon and packaged tile/logo assets.

### Changed
- Documented the required Windows App SDK 2.0 runtime and WinAppRuntime.Singleton setup for MSIX installs and Visual Studio source builds.
- Updated release notes guidance to mention Windows App SDK runtime dependencies before MSIX installation.
- Updated source-build package manifest version handling and refreshed README media/docs details.
- Improved theme and notification handling during system power and session state transitions.

### Fixed
- Fixed a crash after unexpected Bluetooth disconnect followed by auto-reconnect by hardening reconnect/lifecycle handling (`#4`).
- Improved bitmap/HICON resource handling in tray icon rendering to avoid invalid memory/resource lifetime behavior.

## [0.4.1] - 2026-05-16

### Added
- Dedicated asynchronous logger with queued background writes, cached log path resolution, and log rotation.
- Shared connection failure reporting in `DeviceManager` so connection errors consistently emit retryable UI state and clean up failed connection objects.

### Changed
- Suppress the app-started notification when startup auto-reconnect is about to run.
- Shortened the app-started notification lifetime to reduce stale notification clutter.
- Start connection heartbeat logging only after a successful connection and stop it when the final active connection is removed.
- Reset tray click debounce state during tray teardown.
- Modernized internal container cleanup and lookup code.
- Cleaned up notification helper declarations and tray debounce ownership.

### Fixed
- Reduced startup and reconnect notification spam.
- Avoided unnecessary heartbeat logging while no device is connected.
- Made connection failure cleanup more consistent across timeout, system-denied, unknown, and exception paths.

## [0.4.0] - 2026-05-11

### Added
- Reusable self-signed certificate for all MSIX releases.
- Public `.cer` file attached to every GitHub Release so users can trust the certificate before installing the MSIX.
- Single-instance launch guard to prevent multiple tray app instances from running at the same time.
- Persistent diagnostic logging with rotating log files under the user's local app data directory.
- Periodic connection snapshot logging to help troubleshoot connection and auto-reconnect state.

### Changed
- Updated installation instructions to reflect the two supported paths: build from source, or install `.cer` then `.msix`.
- Updated `Package.appxmanifest` publisher identity to `CN=AudioPlaybackConnector2` to match the release signing certificate.
- Release pipeline now signs MSIX packages using the repository's self-signed certificate instead of building unsigned packages.
- Updated the GitHub Release body to include certificate installation instructions.
- Updated README presentation and installation guidance.
- Updated the bug report template to capture the installation method.
- Clarified in CI that regular builds intentionally skip MSIX signing and release signing happens in the release workflow.
- Updated `.gitignore` so the public release certificate can be committed while private signing keys stay ignored.
- Reconnect now closes the previous audio connection on a background thread before opening a replacement connection.
- Device connection callbacks now use weak references and attempt checks to avoid stale callbacks acting on replaced connections.
- Device events now snapshot arguments before invoking handlers.

### Fixed
- Fixed reconnect races where an old Bluetooth connection close could immediately close a newly opened connection.
- Fixed stale `Closed` callbacks triggering unexpected disconnect and auto-reconnect cycles.
- Fixed duplicate connection attempts by closing superseded connection objects.
- Fixed tray icon state so errors or disconnects do not show idle/error when another device is still connected.
- Fixed unknown exception paths in connect and reconnect flows so the picker shows a retryable error state.
- Fixed pending reconnect cancellation state when manual reconnect or watcher restart happens.

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

[Unreleased]: https://github.com/N0ahTM/AudioPlaybackConnector2/compare/v0.4.2...HEAD
[0.4.2]: https://github.com/N0ahTM/AudioPlaybackConnector2/compare/v0.4.1...v0.4.2
[0.4.1]: https://github.com/N0ahTM/AudioPlaybackConnector2/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/N0ahTM/AudioPlaybackConnector2/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/N0ahTM/AudioPlaybackConnector2/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/N0ahTM/AudioPlaybackConnector2/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/N0ahTM/AudioPlaybackConnector2/releases/tag/v0.1.0
