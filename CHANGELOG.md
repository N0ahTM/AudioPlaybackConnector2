# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Added `apc2ctl.exe` command line control for listing devices, status checks, connect/disconnect/reconnect actions, last-device toggle, and macro-friendly ID/name/MAC targeting (`#7`).

## [0.6.0] - 2026-06-17

### Added
- Persisted Settings window placement so size and position restore between sessions.
- Added a minidump stack helper for local crash diagnostics.
- Added release changelog extraction and dependency-aware App Installer feed generation for release packaging.

### Changed
- Polished the Settings window, notification actions, and device picker layouts, including adaptive device picker width and a flatter connection settings section.
- Refactored reconnect planning, device diagnostics, startup task handling, toast construction, flyout styling, and shared UI button helpers.
- Improved tray icon connecting animation timing and cursor behavior over the tray icon.

### Fixed
- Hardened async lifecycle and teardown paths, recoverable exception logging, and crash-report exception handling.
- Made settings and update persistence more robust, including pending startup task state in Settings.
- Prevented duplicate/pending device picker actions from racing while connect/disconnect work is in progress.
- Tightened release asset discovery and local release bundle generation.

## [0.5.4] - 2026-06-08

### Changed
- Removed the legacy tray balloon notification fallback; notifications now use Windows App SDK app notifications only.
- Kept crash report and minidump files after the startup crash prompt so users can still attach them to GitHub issues.

### Fixed
- Fixed cascade restore waiting on its own reconnect busy marker before restoring a second device after a user-action cascade (`#6`).
- Restored catch-all protection in device event dispatch so unknown UI callback exceptions are logged instead of escaping the dispatcher.
- Restored clang-format compliance for the release branch.

## [0.5.3] - 2026-06-03

### Added
- In-memory diagnostic log tail on fatal crashes to simplify field analysis without verbose regular logging (`#6`).

### Changed
- Updated installation and troubleshooting docs to explain that `ms-appinstaller:` is disabled by default on consumer Windows devices and should not be required for GitHub release updates.

### Fixed
- Fixed multi-device disconnect and reconnect crashes (`#6`) by caching device metadata as plain strings instead of using `DeviceInformation` across threads, clearing reconnect state after a successful open, restoring cascade victims with `ConnectAsync`, deferring settings persistence off the UI hot path, and catching exceptions in UI event dispatch.
- Fixed the update installer button on Windows systems where the `ms-appinstaller:` protocol is disabled by downloading and opening the `.appinstaller` file locally.

## [0.5.2] - 2026-06-01

### Changed
- Improved Settings window placement, DPI-aware sizing, and Mica backdrop handling.

### Fixed
- Further hardened multi-device disconnect and reconnect state handling for the crash path reported in `#6`.
- Hardened shutdown, tray, theme-change, and notification teardown paths against stale callbacks.

## [0.5.1] - 2026-06-01

### Added
- Explicit logging for recoverable exceptions and UI async failures to improve field diagnostics.
- Privacy and crash report documentation, with README content split into focused docs.

### Changed
- Refactored core device, reconnect, discovery, application host, settings, and update-check responsibilities into dedicated components.
- Hardened release App Installer feed generation and verification in CI.
- Stabilized parallel compilation settings for local and CI builds.

### Fixed
- Fixed a multi-device disconnect cascade where manually disconnecting or reconnecting one device could cause a second connected device to be treated as an unexpected failure and exit the app (`#6`).
- Fixed a device picker flyout refresh race during reconnect and disconnect actions.
- Hardened reconnect async flow and discovery watcher lifetimes during teardown.
- Bounded logger queue memory under write backpressure.
- Hardened update release checks and App Installer feed version handling.
- Ensured generated crash issue templates stay in English.

## [0.5.0] - 2026-05-28

### Added
- Manual update checks in Settings using the GitHub Releases API, with WinUI status feedback and App Installer launch support.
- Cached startup update checks with an update-available notification that opens the App Installer feed.
- App Installer feed generation and GitHub Pages publishing in the release workflow so MSIX installs can receive App Installer updates.
- Device busy/activity state tracking so the tray icon and device picker can reflect ongoing connect, reconnect, and disconnect operations.
- Double-click tray icon behavior to toggle the most recently connected device.

### Changed
- Release assets now include a stable `.appinstaller` feed alongside the signed MSIX and public certificate.
- Release publishing now keeps the GitHub Release as a draft until the GitHub Pages App Installer feed has been deployed and verified.
- Improved notification status management so stale status notifications are replaced more consistently and tray balloon fallback remains available.
- Device picker rows now refresh busy state more reliably and avoid enabling actions while a device operation is already running.

### Fixed
- Replaced cached `DeviceInformation` objects with ID-only string storage to avoid lifetime issues after device changes.
- Improved device cache compatibility by storing device IDs as `std::wstring`.
- Fixed tray visual refresh and busy-state checks so the tray icon returns to the correct state after background device operations.

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
