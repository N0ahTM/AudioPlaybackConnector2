# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.9.0] - 2026-09-07

### Added
- Added compact per-device options in the picker for aliases, default selection, startup connection, and automatic reconnection.
- Added a direct Help entry to the tray menu.
- Added an updated animated README demonstration, hosted as a GitHub attachment outside the source repository.

### Changed
- Consolidated general preferences into a compact Settings page and retained Help in the same window.
- Kept device selection and device options at a shared localized width, with short transitions that respect Windows animation preferences.
- Added disconnect feedback on device hover and keyboard focus, and clarified the separate reconnect action.
- Open Settings after the picker has closed and focus the back button when entering device options.
- Made snapshot lifetimes explicit for static analysis while retaining owned string conversions.
- Reworked device session ownership, serialized operations, retry cancellation, and suspend/resume handling with native regression coverage.
- Consolidated the installation, troubleshooting, contributor, and installer documentation.
- Simplified the bootstrapper source layout and reused the application icon directly.

### Fixed
- Embed the validated MSIX bundle in the offline installer instead of requiring a single-architecture `.msix` file.
- Align the window-placement reset action with the Settings switches.
- Derive disconnect hover feedback from the button's actual pointer/focus state and remove the competing device-name tooltip.
- Handle Windows close/end-session messages before the hidden host window and XAML dispatcher are destroyed.
- Retain the settings-window state through synchronous close callbacks, fixing a null dereference reproduced during a package update with Settings open.
- Replace the narrow Settings navigation pane reported to truncate Chinese labels with the compact Settings/Help layout (issue #19).

### Removed
- Removed the duplicate device-management page from Settings and the outdated README preview.
- Removed repository-tracked performance benchmark harnesses, archived benchmark results, and Windows Sandbox installer helpers. Local experiments are now ignored.
- Removed unused developer helpers and generated documentation and installer image assets.

### Known limitations
- A connected device may occasionally produce no audio, including after a playback pause. Use the circular-arrow Reconnect action to recover. This remains under investigation; 0.9.0 does not claim to fix it ([#1](https://github.com/N0ahTM/AudioPlaybackConnector2/issues/1)).
- Windows may ignore per-app output routing for A2DP sink audio. The app does not provide an output-device selector ([#13](https://github.com/N0ahTM/AudioPlaybackConnector2/issues/13)).

## [0.8.1] - 2026-08-21

### Added
- Added per-user web and offline bootstrapper installers with adaptive install, update, repair, and uninstall flows.

### Changed
- Extended release and dry-run workflows to build and verify both setup variants alongside the existing MSIX, App Installer feed, certificate, and dependency assets.
- Made the web setup embed the pinned signing certificate and validate the selected x64 package identity before installation.

### Removed
- Removed the unmaintained Stream Deck integration and its release artifact. The command-line client remains available for external automation.

## [0.8.0] - 2026-08-16

### Added
- Added support for phone-initiated A2DP playback connections.
- Added adaptive Windows resource-pressure monitoring so picker residency, UI preloading, polling, and background work respond to memory, activity, and energy constraints.
- Added a native core runtime boundary, broad regression coverage, and reproducible resource-usage and picker-latency benchmark tooling.

### Changed
- Restored a hardened single-process architecture while keeping XAML dependencies out of the reusable core runtime.
- Made the device picker and tray paths faster through cached native snapshots, coalesced refreshes, and reduced invisible or unchanged UI work.
- Reduced background overhead by deferring settings persistence, diagnostics collection, and automatic update work, and by removing diagnostic-only connection heartbeats.
- Hardened command IPC with authenticated same-user endpoints, bounded concurrency and request retention, strict payload validation, and deterministic shutdown.

### Fixed
- Fixed connection, discovery, reconnect, suspend/resume, and shutdown races, including stale connection identities and failed coroutine launches.
- Fixed settings-window, tray-menu, startup-task, notification, and update-check lifecycle failures during partial initialization or teardown.
- Fixed an update-check coroutine lifetime issue that could access a destroyed coordinator, and bounded stale Stream Deck status and visual-generation state.
- Fixed disconnect-all handling for devices with pending reconnects and improved reconnect-all failure reporting.
- Hardened settings persistence, diagnostic retention, release packaging, signed payload verification, and App Installer feed publication.

## [0.7.0] - 2026-07-19

### Added
- Added a larger Preferences hub with Connection, Devices, Privacy, Stream Deck, Support, Diagnostics, Updates, and App sections.
- Added per-device aliases, default-device selection, and privacy-mode redaction for UI, notifications, command-line output, and copied diagnostics.
- Added support and diagnostics actions for opening Bluetooth settings, opening the log folder, copying redacted diagnostics, and preparing GitHub bug reports.
- Extended `apc2ctl.exe` with `show`, `settings`, `default`, and `alias` commands plus `--alias`, `--default`, and `--raw` targeting/output options.
- Added an installable Stream Deck SDK plugin (beta, version 0.1.0) with privacy-aware default-device, all-device, picker, and settings actions.

### Changed
- Reduced release logging noise while preserving detailed error, crash, and diagnostics context.
- Updated the README with the new CLI, privacy, diagnostics, and Stream Deck workflows.
- Made the Settings window more compact and adaptive across all supported languages, with acrylic backdrops and unclipped controls at supported window sizes.

### Fixed
- Kept unhandled XAML exceptions visible to the crash pipeline after recording their diagnostic context.

## [0.6.2] - 2026-07-03

### Changed
- Added extended diagnostics for tray icon updates, flyout refreshes, connection snapshots, and hidden WinUI anchor placement to make future state mismatches easier to diagnose.
- Documented the Windows A2DP sink routing workaround in the troubleshooting guide.

### Fixed
- Retried transient `AudioPlaybackConnection.OpenAsync` failures with cooldown/backoff when Windows returns recoverable failures such as `UnknownFailure` with `0x8007001F` (`#14`).
- Made the tray error icon transient and reconciled tray state from the actual device snapshot so tray, flyout, and connection state converge after errors (`#10`, `#14`).
- Hardened the hidden WinUI anchor window so the small `WinUI Desktop` window does not appear instead of the flyout.
- Kept the command-line control server responsive while long device operations run, and return a localized busy response for overlapping mutating commands.

## [0.6.1] - 2026-06-20

### Added
- Added `apc2ctl.exe` command line control for listing devices, status checks, connect/disconnect/reconnect actions, last-device toggle, and macro-friendly ID/name/MAC targeting (`#7`).

### Fixed
- Fixed the tray icon staying in a non-connected color while a playback connection is already open (`#10`).

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
