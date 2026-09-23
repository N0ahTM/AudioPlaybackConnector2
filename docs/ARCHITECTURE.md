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
- Host requests transport cancellation before controller drain. `UiDispatcher` owns general UI delivery and its native fallback queue; `UiRefreshScheduler` owns coalescing, retry timer and callback lifetime; host supplies Request/Stop and rendering.
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

## Reproducible source measurements

The immutable starting point is `f5404c597080f8e2d892e6b9787fee38bbb17113` (the original `main`). The checked-in [measurement record](source-measurements.json) compares it with `511ba95049b300dffab3d5d6c91f76cb53cd2605`; this is an intermediate observation, not final acceptance.

| Measure | Starting point | Measured revision |
| --- | ---: | ---: |
| Production C++ files | 136 | 127 |
| Physical lines | 26,239 | 22,913 |
| Nonblank lines | 23,297 | 20,333 |
| Stored source bytes | 1,140,260 | 1,014,669 |

Counts include comments, declarations and includes in tracked `.cpp`, `.hpp` and `.h` files under the app, Control and CoreRuntime projects. They exclude tests, benchmarks, generated build outputs, resources and project files. Bytes come from Git blobs, independent of checkout CRLF conversion. The record contains the ten largest files at each revision. Totals were independently checked against Git archives after normalizing exported CRLF to stored LF.

Reproduce the record from the repository root without checking out or building either revision:

```powershell
python scripts/measure-source.py --baseline f5404c597080f8e2d892e6b9787fee38bbb17113 --revision 511ba95049b300dffab3d5d6c91f76cb53cd2605 --output docs/source-measurements.json
```

For a later comparison, supply the exact accepted commit as `--revision` and update this table with the resulting record. Physical line counts do not establish simpler control flow, fewer public APIs, fewer helpers or preserved behavior. Those reviews, comparable fresh build timings and package-size measurements remain separate. The isolated dependency probes are historical evidence and remain until their results and the product comparison are fully retained.
