# Architecture

AudioPlaybackConnector2 is a per-user Windows tray application. WinUI 3 provides settings and picker surfaces, while Bluetooth connection state and command use cases are kept behind application and core interfaces so most behavior can be built and tested without constructing WinUI controls.

## Projects

- `AudioPlaybackConnector2`: packaged desktop executable, application coordination, Bluetooth services, tray UI, WinUI views, localization, settings, and diagnostics; Store or Windows App Installer owns updates outside the process
- `AudioPlaybackConnector2.Control`: `apc2ctl.exe` parser and trusted named-pipe client
- `AudioPlaybackConnector2.CoreRuntime`: runtime boundary used by non-UI core code
- `AudioPlaybackConnector2.CoreTests`: native regression tests, including application coordination, command handling, settings, device behavior, and runtime boundaries
- `AudioPlaybackConnector2 (Package)`: MSIX packaging for x64 and ARM64

## Runtime flow

1. `ApplicationHost` initializes settings, strings, services, command handling, and UI controllers for the current user.
2. `DeviceService` and its device/session types own Bluetooth discovery and connection facts.
3. `AppController` executes user-level commands and returns explicit result models.
4. Tray and WinUI presentation code consume snapshots and results rather than owning Bluetooth behavior.
5. `apc2ctl` validates command-line input, authenticates the same-user app endpoint, sends one typed request, and formats the typed response.

## Boundaries

- UI code calls named application methods and renders snapshots; it must not become a second owner of connection or settings state.
- Device-picker alias and default-device actions call the explicit `AppController` methods. Tray and picker retain weak controller references; each action acquires a temporary strong reference. The host owns the controller lifetime, and these actions have no separate command-wrapper or forwarding callback.
- Primary tray activation uses the same weak controller endpoint to request picker toggle with detached completion. It cannot wait for a picker event on its own UI dispatcher. A callback retained after controller destruction becomes a no-op.
- Tray connection actions and notification reconnect actions call named controller methods with detached completion. `ApplicationHost` no longer constructs general commands for these actions or passes them through a general tray command dispatcher; it retains the UI scheduling and visual refresh required by presentation.
- Core and application behavior must remain testable without WinUI construction. The CoreRuntime boundary verifier protects this constraint.
- Settings persistence owns schema validation and atomic storage. Callers use the current typed model rather than carrying legacy-format concerns through the application.
- Command transport is per-user and validates its peer. Protocol changes require compatibility consideration, bounded payloads, and regression tests.
- `ControlCommandAdapter` validates the wire grammar and directly selects an explicit controller method. Response formatting uses the original request and application result. The application model has no universal command object or controller dispatch switch.
- Each controller method selects its own owner calls and required inputs. Private admission handling owns the active-call lease, cancellation/deadline preflight and exception-to-result boundary. Only read-only queries may retry after a newer settings revision; actions with side effects execute once and reread committed values for their results.
- `SnapshotAndSubscribe` returns the initial application snapshot, an event-revision watermark and the owned subscription. Capture validates the settings and device-owner versions, then registers only if the publication revision stayed unchanged. The host serializes notification handling on its UI context and rejects older revisions. Settings and device changes use the same event stream.
- The adapter's stateless `FormatResponse` function owns response rendering. Golden output fixtures call that production function without constructing a controller; adapter integration tests verify delegation, localization and privacy options separately.
- Localization keys are defined by English resources. Selected locales overlay English at runtime.
- Long-running or asynchronous device actions have an explicit owner and cancellation/lifetime boundary.

## Dependency and code rules

- A source file includes the declarations it uses. `pch.h` is a compilation cache, not an undeclared dependency contract.
- Build in MSVC's C++26 draft mode (`/std:c++latest`, v145/14.51 or newer). Prefer the standard library when it expresses the requirement clearly; use Windows, WinRT, or WIL facilities for Windows-specific ownership and APIs.
- Introduce a helper, class, or interface only when it names a real responsibility, lifetime, invariant, or test boundary.
- Keep one primary non-trivial type per file when that improves navigation; tiny closely related value types may remain together.
- Avoid forwarding the same immutable context through many layers. Give stable shared state one clear owner and pass references, pointers, or focused views where lifetime is explicit.
- Choose static state only when there can be exactly one process-wide instance and test isolation remains clear. Prefer instances for mutable state, replaceable dependencies, and independently testable behavior.

This document describes the current high-level boundaries, not a second backlog. Planned changes belong in issues or the local untracked plan; accepted user-visible changes belong in the changelog.
