# Target Architecture

## Runtime flow

```text
TrayUi / SettingsWindow / ControlCommandHandler
                 | typed commands
                 v
            AppController
       / DeviceService | SettingsStore / supporting services
                 | typed facts + immutable snapshots
                 v
          UI / CLI presentation

ResourcePressureMonitor -> ResourceResidencyManager -> TrayUi preload/release
```

- MUST NOT add an application-wide event bus.
- Each producer exposes one typed subscription; `AppController` wires cross-feature reactions.
- UI and CLI use the same `AppController` commands.

## Projects

| Project | Owns | Dependency limit |
|---|---|---|
| `Apc.Core` | Domain, settings, resource policy, app models, shared protocol | No WinUI/XAML |
| `Apc.App` | WinUI, tray/windows, composition, control host | May depend on Core |
| `Apc.Control` | `apc2ctl`, parsing, pipe client, console output | Shared protocol only; no WinUI |
| `Apc.Tests` | Deterministic core/protocol/concurrency/use-case tests | Core; isolate platform integration |
| `Apc.Package` | MSIX, assets, deployment | App and CLI outputs |

Names may stay compatible during migration. Boundaries are mandatory.

## Owners

| Component | Owns | Must not own/depend on |
|---|---|---|
| `AppRuntime` | Composition/destruction order, single instance, hidden XAML host/HWND messages, taskbar recreation, GDI+/WinRT lifetime, suspend/resume entry, shutdown/final flush | Use cases, protocol dispatch, JSON, Bluetooth, notifications, updates, resource policy |
| `AppController` | Connect/disconnect/reconnect/bulk/default/last-device use cases, settings side effects, update/startup requests, selected notification policy, immutable `AppSnapshot`/`TraySnapshot`, common UI/CLI API | XAML, pipe frames/I/O, JSON persistence, `AudioPlaybackConnection`, pressure probes, toast XML |
| `DeviceService` | Serialized device context, session map, discovery watcher; Start/Stop, Connect/Disconnect/Reconnect/Bulk/Configure, Snapshot/Subscribe | UI, settings persistence, presentation |
| `DeviceWatcher` | Watcher lifecycle/tokens/generation; normalized discovery events | Connections, reconnect, settings, UI |
| `DeviceSession` | One device connection/token, lifecycle state, operation epoch, close barrier, incoming state, reconnect timer/attempt, cancellation, normalized errors | Cross-device/application policy |
| `ReconnectPolicy` | Pure retry/backoff/manual/startup/loss/resume decisions | Timers, I/O, connection start |
| `ResourceResidencyManager` | P02 coordination; Start/Stop, ReportUiState/Interaction, DiagnosticsSnapshot; monitor/policy lifecycle | Tray rendering |
| `ResourcePressureMonitor` | Windows memory/activity/fullscreen/energy probes, polling, heartbeat snapshots | Residency decisions |
| `ResidencyPolicy` | Pure P02 Cold/Warm/Hot decisions and next evaluation | Timers and UI operations |
| `SettingsStore` | P07 load; Snapshot/Update/Flush/Subscribe; one private writer state machine | Device/UI calls |
| `SettingsSession` | Window-scoped rows, validation, busy/startup/update state, diagnostics text, typed intents | Persistence and XAML controls |
| `SettingsWindow` | XAML, navigation, accessibility, layout, rendering, intent forwarding | Persistence, device/update coordination, diagnostics collection |
| `TrayUi` | Notify icon, menu, flyout/picker lifetime, theme/DPI, P03 icons, tooltip, UI resource owner, intents | Residency policy, settings/device internals |
| `DevicePicker` | Render `DeviceRow`; emit intent | Direct settings, alias/privacy policy, `DeviceService` access |
| `ControlServer` | P01 server pipe security/lifecycle/transport; private instances/transfers/cache/retry | XAML and use-case implementation |
| `ControlCommandHandler` | Validated protocol ↔ `AppController` mapping | Pipe lifecycle and duplicate use cases |
| `ControlClient` | P01 connection, trusted-server verification, transport, timeout/cancellation | UI |
| `NotificationService` | Render normalized `(kind, title, message, optional action)`, XML/sanitization, P05 images | P04 policy decision |

`DeviceSession` states: `Idle`, `Connecting`, `Connected`, `Disconnecting`, `WaitingForReconnect`, `Failed`. Watcher/WinRT callbacks post to the serialized device context before state access; operation epochs reject late completions.

`TrayUi` reports loaded, initialized, visible/transitioning, pinned, and interaction facts. It receives only `Preload` and `Release` requests from `ResourceResidencyManager`.

## Supporting services

| Service | Scope |
|---|---|
| `UpdateService` | Transport, version comparison, cancellation/single-flight; profile G04 |
| `StartupService` | Startup task with private latest-wins state |
| `DiagnosticsService` | Snapshot-only privacy-aware report; profile G05 |
| `Logger` | Profile G01 |
| `CrashReporter` | Profile G02 |
| `StringCatalog` | Localization load/lookup |
| `SingleInstance` | Narrow Windows ownership primitive |

## Source layout

```text
AudioPlaybackConnector2/
  app/       AppRuntime, AppController, AppModels
  device/    DeviceService, DeviceWatcher, DeviceSession, ReconnectPolicy, DeviceModels
  resource/  ResourceResidencyManager, ResourcePressureMonitor, ResidencyPolicy
  settings/  SettingsStore, SettingsModels, SettingsSession
  control/   ControlProtocol, ControlServer, ControlCommandHandler, ControlClient, PipeSecurity
  services/  NotificationService, UpdateService, StartupService, DiagnosticsService, StringCatalog
  ui/        App, shell/HostWindow, tray/*, settings/SettingsWindow
  platform/  Logger, CrashReporter, WindowPlacement, SingleInstance
```

- Colocate `.hpp`/`.cpp`; keep a small public area only for protocol headers shared with CLI.
- UI may depend on app models, never device internals.
- `AppController` may depend on feature facades, never XAML or pipe transport.
- Device/resource modules may use platform APIs, never UI.
- `ControlServer` depends on protocol plus a command callback, never XAML.
- Settings side effects run in `AppController`, not `SettingsStore`.
- Diagnostics reads snapshots and never feature locks.
- No feature includes `AppRuntime.hpp`.

## Language gate

- Current baseline: C++23 (`stdcpp23`).
- Target: C++26 after one dedicated change validates selected MSVC/Visual Studio, Windows SDK, C++/WinRT, WIL, Windows App SDK, tests, packaging, x64, and ARM64.
- Use only features supported consistently by the shipping toolchain; prefer a clearer stable C++23 form otherwise.
