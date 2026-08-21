# Target Architecture

## Solution projects

The five-project solution remains because the projects have distinct build and deployment roles:

| Project | Responsibility | Dependency rule |
|---|---|---|
| `Apc.Core` | Device domain, settings, resource residency policy, application models, shared control protocol | Must not depend on WinUI/XAML |
| `Apc.App` | WinUI application, tray, windows, platform composition, control-server host | May depend on `Apc.Core` |
| `Apc.Control` | `apc2ctl.exe`, argument parsing, pipe client, console rendering | May depend on shared control protocol/client code, never on WinUI |
| `Apc.Tests` | Deterministic core, protocol, concurrency, and application-use-case tests | May depend on `Apc.Core`; platform integration is isolated |
| `Apc.Package` | MSIX manifest, assets, deployment and installer integration | References application and CLI outputs |

Names may initially remain compatible with the existing solution to avoid a noisy project rename. The boundaries are more important than the labels.

## Runtime overview

```text
TrayUi / SettingsWindow / ControlCommandHandler
                    |
                    | typed commands
                    v
              AppController
              /     |      \
             v      v       v
     DeviceService  SettingsStore  Update/Startup services
             |
             | DeviceEvent + DeviceSnapshot
             v
       AppController
             |
             | AppSnapshot
             v
      TrayUi / SettingsSession

ResourcePressureMonitor
             |
             v
ResourceResidencyManager ---- preload/release ----> TrayUi resource owner
```

There is no application-wide event bus. Each producer exposes one typed subscription surface, and `AppController` explicitly wires the few cross-feature reactions.

## Application layer

### `AppRuntime`

Owns:

- construction and destruction order;
- single-instance guard;
- hidden XAML host window and HWND subclass integration;
- taskbar recreation and Windows message forwarding;
- GDI+ and WinRT application lifetime;
- suspend/resume entry points;
- orderly shutdown and final settings flush.

Does not own device commands, settings mutations, control-protocol dispatch, update policy, notification policy, resource policy, or presentation mapping.

Target API:

```cpp
class AppRuntime final {
public:
    void Start();
    void Shutdown() noexcept;
};
```

### `AppController`

Owns application use cases and cross-feature policy:

- connect, disconnect, reconnect, toggle default, bulk actions;
- settings changes and their required device-policy side effects;
- mapping `DeviceEvent` to state refresh and selected notifications;
- update/startup requests;
- construction of immutable `AppSnapshot` and `TraySnapshot` values;
- the common command API used by UI and CLI.

It explicitly does not parse pipe frames, manipulate XAML controls, write JSON, call `AudioPlaybackConnection` directly, probe pressure, or render toast XML.

Normal disconnect handling updates state but does not call `NotificationService`. Error, reconnect-failed, update, and other retained notification kinds continue to select their dedicated image.

## Device module

### Public concepts

#### `DeviceService`

The single public facade for discovery and connection commands. It owns the device execution context, session map, watcher, and subscription list.

```cpp
class DeviceService final {
public:
    void Start();
    void Stop();
    DeviceTask Connect(DeviceId);
    DeviceTask Disconnect(DeviceId);
    DeviceTask Reconnect(DeviceId);
    DeviceTask DisconnectAll();
    DeviceTask ReconnectAll();
    void Configure(DevicePolicy);
    DeviceSnapshot Snapshot() const;
    DeviceSubscription Subscribe(DeviceEventHandler);
};
```

#### `DeviceWatcher`

A narrow C++/WinRT adapter that produces normalized discovery events. It owns watcher tokens and lifecycle generation but no connection, presentation, settings, or reconnect policy.

#### `DeviceSession`

One instance per active/busy device. It owns:

- `AudioPlaybackConnection` and state-changed token;
- state machine (`Idle`, `Connecting`, `Connected`, `Disconnecting`, `WaitingForReconnect`, `Failed`);
- current operation epoch and stale-result checks;
- close barrier and close-before-reconnect sequencing;
- incoming-connection state;
- reconnect timer/attempt state;
- user-action cancellation and error normalization.

#### `ReconnectPolicy`

Pure deterministic decision code for retry limits, backoff, manual-operation cancellation, startup policy, unexpected loss, and resume reconnect. It does not create timers or start connections.

### Internalization

The responsibilities currently represented by session stores, operation coordinators, latest-lifecycle holders, audio wrappers, close-barrier helpers, and reconnect tokens remain where needed, but single-use types become private implementation details of `DeviceService.cpp` or `DeviceSession.cpp`.

### Threading

All device mutations are serialized through one device execution context. Watcher callbacks and `AudioPlaybackConnection` callbacks post to that context before reading or changing session state. Per-session operation epochs remain necessary because WinRT operations may complete after cancellation.

## Resource residency module

The behavior is preserved, not replaced with simple idle loading.

### `ResourceResidencyManager`

The only public coordinator. It owns:

- pressure snapshot sequence and freshness;
- positive-authorization sequence checks;
- adaptive scheduling and both dispatcher/Win32 fallback paths;
- retry backoff for preload/release failures;
- current UI state and user-interaction hold;
- resource diagnostics snapshot;
- lifecycle of `ResourcePressureMonitor` and `ResidencyPolicy`.

```cpp
class ResourceResidencyManager final {
public:
    void Start(ResourceCallbacks);
    void Stop() noexcept;
    void ReportUiState(UiResourceState);
    void ReportUserInteraction();
    ResourceDiagnostics Snapshot() const;
};
```

### `ResourcePressureMonitor`

Owns Windows memory notifications, user activity, fullscreen/presentation detection, energy saver probing, polling intervals, and heartbeat snapshots.

### `ResidencyPolicy`

Pure, deterministic Cold/Warm/Hot evaluation with the existing pressure delay, recovery delay, preload delay, interaction hold, UI pinning, release deferral, clock rollback handling, and next reevaluation result.

`ScheduleState`, retry helpers, and diagnostic-name helpers are private or colocated unless another module genuinely consumes them.

## Settings module

### `SettingsStore`

The single owner of settings state and persistence:

- validated load and corrupt-file preservation;
- immutable snapshots;
- typed mutations;
- change notification;
- one dirty revision;
- one debounced writer;
- atomic temporary-file write, flush, and replace;
- bounded retry and synchronous suspend/shutdown flush.

The store keeps JSON. It does not introduce a database, registry storage, or a new persistence dependency.

```cpp
class SettingsStore final {
public:
    SettingsSnapshot Snapshot() const;
    UpdateResult Update(SettingsChange);
    void Flush();
    SettingsSubscription Subscribe(SettingsChangedHandler);
};
```

The existing guarantees against lost changes and partial files remain. Worker tokens, generations, timer fallbacks, and retry state are reduced to one private writer state machine owned by `SettingsStore`.

### `SettingsSession`

A window-scoped presentation model. It builds device rows, validates aliases, represents busy/update/startup states, creates the diagnostics text, and exposes typed user intents. It does not persist data itself.

### `SettingsWindow`

Owns XAML controls, navigation, accessibility, layout, and rendering. It forwards intent to `SettingsSession`/`AppController` and contains no update transport, startup coordination, device-domain interpretation, or report construction.

## Tray and picker

### `TrayUi`

Owns the notify icon, context menu, flyout, picker lifetime, theme, static state icons, tooltip, and UI resource residency hooks. It renders a `TraySnapshot` and emits user intents.

The connecting animation timer and animation frame state are removed. Static images remain for Idle, Connecting, Connected, and Error.

`TrayUi` reports these resource facts to `ResourceResidencyManager`:

- loaded;
- initialized;
- visible/transitioning;
- pinned;
- user interaction.

It receives only `Preload` and `Release` requests from the manager.

### `DevicePicker`

Receives ready-to-render `DeviceRow` values. It does not query settings, privacy policy, aliases, or `DeviceService` directly.

## Control module

CLI and named pipes are retained as a first-class subsystem.

### `ControlServer`

Owns pipe creation, security descriptor, server identity, concurrent instances, framing, deadlines, cancellation, request correlation/deduplication, response transfer, start retry, and shutdown. Pipe-instance and transfer types remain private to its implementation.

### `ControlCommandHandler`

Maps validated protocol requests to the same `AppController` methods used by UI and maps results/snapshots back to protocol responses. This removes command dispatch from `AppRuntime` without duplicating use cases.

### `ControlClient`

Owns client connection, trusted-server verification, request/response transport, timeout behavior, and cancellation. Console parsing and rendering remain in `Apc.Control`.

### Compatibility rule

The rework must preserve command names, arguments, exit behavior, JSON schema, privacy/raw behavior, deadlines, correlation semantics, and security properties unless a separately versioned protocol change is approved.

## Notifications

`NotificationService` accepts a normalized value:

```cpp
struct Notification {
    NotificationKind Kind;
    std::wstring Title;
    std::wstring Message;
    std::optional<NotificationAction> Action;
};
```

It maps `NotificationKind` to the existing distinct toast images. XML building and sanitization may be private helpers. A normal disconnect is deliberately absent from the notification-policy mapping; this does not remove disconnect state updates or error/reconnect notifications.

## Supporting services

- `UpdateService`: update transport, version comparison, cancellation/single-flight policy selected by the decision register.
- `StartupService`: startup-task state with latest-wins behavior internal to the service.
- `DiagnosticsService`: one privacy-aware report built from immutable snapshots.
- `Logger`: logging profile selected by the decision register.
- `CrashReporter`: crash profile selected by the decision register.
- `StringCatalog`: localization loading and lookup.
- `SingleInstance`: narrow Windows ownership primitive.

## Proposed source layout

```text
AudioPlaybackConnector2/
  app/
    AppRuntime.hpp/.cpp
    AppController.hpp/.cpp
    AppModels.hpp

  device/
    DeviceService.hpp/.cpp
    DeviceWatcher.hpp/.cpp
    DeviceSession.hpp/.cpp
    ReconnectPolicy.hpp/.cpp
    DeviceModels.hpp

  resource/
    ResourceResidencyManager.hpp/.cpp
    ResourcePressureMonitor.hpp/.cpp
    ResidencyPolicy.hpp/.cpp

  settings/
    SettingsStore.hpp/.cpp
    SettingsModels.hpp
    SettingsSession.hpp/.cpp

  control/
    ControlProtocol.hpp/.cpp
    ControlServer.hpp/.cpp
    ControlCommandHandler.hpp/.cpp
    ControlClient.hpp/.cpp
    PipeSecurity.cpp

  services/
    NotificationService.hpp/.cpp
    UpdateService.hpp/.cpp
    StartupService.hpp/.cpp
    DiagnosticsService.hpp/.cpp
    StringCatalog.hpp/.cpp

  ui/
    App.xaml/.h/.cpp
    shell/HostWindow.xaml/.h/.cpp
    tray/TrayUi.hpp/.cpp
    tray/TrayIcon.hpp/.cpp
    tray/TrayMenu.hpp/.cpp
    tray/DevicePicker.xaml/.h/.cpp
    settings/SettingsWindow.xaml/.h/.cpp

  platform/
    Logger.hpp/.cpp
    CrashReporter.hpp/.cpp
    WindowPlacement.hpp/.cpp
    SingleInstance.hpp/.cpp
```

Headers and implementations are colocated. The application does not maintain a public SDK, so a mirrored global `include/` tree is not required. A small exported include area may remain for protocol headers shared with the CLI.

## Dependency rules

- UI may depend on application models, never on device internals.
- `AppController` may depend on feature facades, never on XAML or pipe transport.
- Device and resource modules may depend on platform APIs, never on UI.
- `ControlServer` may depend on protocol and a command-handler function, never on XAML.
- Settings persistence never calls device or UI code; side effects of settings changes are handled by `AppController`.
- Diagnostics consumes snapshots and cannot reach into locks or mutable internals.
- No feature may include `AppRuntime.hpp`.
