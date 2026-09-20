# Architecture

Per-user Windows Bluetooth A2DP sink tray app. WinUI is a presentation adapter; Store/App Installer owns updates. Detailed concurrency contracts and verification limits live in [INVARIANTS](INVARIANTS.md); build commands in [CONTRIBUTING](../CONTRIBUTING.md).

## Projects

| Project | Responsibility |
| --- | --- |
| `AudioPlaybackConnector2` | Desktop app, composition, WinUI/tray/resources |
| `AudioPlaybackConnector2.CoreRuntime` | Shared production application/core/platform behavior; no WinUI |
| `AudioPlaybackConnector2.CoreTests` | Native tests linking that same runtime |
| `AudioPlaybackConnector2.Control` | `apc2ctl.exe`: console I/O, correlation and trusted pipe transport |
| `AudioPlaybackConnector2 (Package)` | x64/ARM64 MSIX packaging |

## Runtime flow

`App → ApplicationHost → AppController → DeviceService / SettingsStore`.

- Host composes owners, handles lifecycle and marshals presentation to the UI context. AdaptiveResourceController owns pressure observation, picker residency, retry timers and the resource snapshot; its pure policies stay in CoreRuntime.
- Controller exposes named use cases, typed results and validated immutable snapshots. It coordinates owners without duplicating their mutable state.
- DeviceService owns discovery, sessions, connection/reconnect and operation completion. SettingsStore owns schema validation, revisioned settings and atomic persistence.
- Tray, picker and settings render snapshots and call the controller. Picker/options projections use one AppSnapshot and explicit localized privacy text. No UI device/store access, inventory cache or busy-operation owner.
- `ControlCommandAdapter` validates wire grammar and calls named controller methods; stateless `FormatResponse` formats their result. Universal commands stay at the external protocol boundary.

## Boundaries

- Each controller use case selects its dependencies; admission leases, cancellation/deadlines and exception conversion are private boundaries. Only read-only queries may retry a changed settings revision; side effects execute once.
- Snapshots validate settings/device/startup-owner versions. Queries format the same capture without fallback reads. Resource diagnostics are independently sampled.
- `SnapshotAndSubscribe` closes the capture/subscription gap and returns an event watermark. Consumers serialize initial state/events and reject old revisions. The controller owns its publication fence, not another device history map.
- Native connected transitions persist name, preferences and history before notification; settings policies reach the device owner by revision. Host performs neither persistence nor policy selection. `RestoreStartupConnections` uses one snapshot, ordered by recent history.
- Tray/picker callbacks hold weak controller references. Tray activation does not wait on its own UI dispatcher. Final settings-window placement commits before application admission closes.
- Host requests transport cancellation before controller drain. `UiRefreshScheduler` owns coalescing, retry timer and callback lifetime; host supplies Request/Stop and rendering.
- CLI authenticates the same-user endpoint. `CliParser` is pure wide-argument parsing in CoreRuntime; CLI11 stays in its implementation. Value/escape normalization preserves opaque UTF-16, ordered validation, diagnostic text and exit codes.
- Host owns StringResources; English is canonical with locale overlays. Read-only consumers retain text lifetime, not the application. Language publication precedes UI relocalization.

## Thread ownership

| Work | Context |
| --- | --- |
| Windows, tray, picker, settings, notification state | UI dispatcher |
| Settings persistence | One SettingsStore `jthread` |
| Logging | One worker in a private spdlog pool |
| Pipe I/O, monitoring, recovery/refresh timers, deferred cleanup | Shared Windows threadpool |
| Device start/open/close, discovery and diagnostics | WinRT background scheduling |

DeviceService, AppController and StartupTaskCoordinator serialize state without dedicated per-owner threads. Timer registrations and drainer IDs are not dedicated threads. Windows/WinUI/COM/Bluetooth may create additional threads; two workers plus UI is not a total process-thread count.

## Dependency and code rules

- Use C++26 draft `/std:c++latest`, v145/14.51+; direct includes, with PCH only as cache.
- Keep one mutable owner and explicit async cancellation/shutdown. No foreign calls or blocking work under owner locks; consult documented platform exceptions in INVARIANTS.
- Prefer supported standard facilities and WIL/WinRT at Windows boundaries. New abstractions need a real responsibility, invariant, lifetime or test/platform boundary.
- One primary substantial type per file; small related values may remain together. Pass focused values/references rather than unchanged context through forwarding layers.
- Mutable state and replaceable dependencies belong to instances; process-wide constants may be static.

Current contracts belong here/in INVARIANTS; remaining work belongs in the local plan, user-visible history in CHANGELOG.
