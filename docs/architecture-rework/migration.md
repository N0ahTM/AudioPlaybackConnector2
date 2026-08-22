# Migration

## Rules

- Replace incrementally; one new owner per phase.
- Every PR slice MUST compile and preserve unapproved behavior.
- Preferred slice: characterization/tests → implementation behind adapter → callers → legacy deletion → docs/boundary check.
- Temporary adapters/flags require owner and deletion criterion.
- A phase ends only after its legacy owner and duplicate pipeline are removed.
- Avoid unrelated formatting and project renames.
- Data changes require compatible read and rollback; protocol changes require explicit versioning.

## Phases

| Phase | Required change | Exit gate |
|---|---|---|
| 0 Baseline | Record metrics and [performance baseline](performance-baseline-2026-08-22.md). Inventory P01 commands/schema/security, P02 transitions/timing, P05 images, P06 races, P07 formats/failures, P09 behavior. Add missing characterization tests. Gate P10 separately. | Compatibility/race matrices exist; high-risk behavior has automated or named manual coverage; Debug/Release supported builds pass; language baseline is reported accurately. |
| 1 Models/API | Add `DeviceId`, snapshots/events, normalized results, typed `AppController`; route UI/CLI through adapters. | UI/CLI share one API; models contain no mutable service, XAML, or pipe types. |
| 2 Settings | Build `SettingsStore`; migrate all callers; implement P07 with one writer state machine. | No external settings locks; legacy formats/failures/change-during-write/suspend/shutdown tests pass; old settings coordination deleted. |
| 3 Devices | Add serialized `DeviceService`, `DeviceWatcher`, per-device `DeviceSession`, pure `ReconnectPolicy`; migrate discovery, P06 commands/policies/lifecycle. | One session-map owner; callbacks mutate only on device context; stale work cannot succeed/resurrect; race matrix passes; old managers/coordinators deleted. |
| 4 App | Move use cases, settings side effects, snapshots, presentation mapping, and P04 policy to `AppController`; replace the multi-event router with one controller subscription; reduce `AppRuntime`. | `AppRuntime` has no command switch/Bluetooth/policy; `AppController` has no XAML/pipe/JSON/pressure/toast XML; P04 test passes; old router is deleted. |
| 5 Resources | Wrap proven monitor/policy in `ResourceResidencyManager`; move all P02 coordination from host; keep narrow Tray contract. | Host has no pressure/timer/sequence/retry state; Tray makes no policy; full P02 parity tests pass. |
| 6 Tray | Build snapshot-driven `TrayUi`/rows and one flyout lifecycle state machine; preserve theme/DPI/clicks and resource facts; implement P03; remove direct settings/device access. | P03 starts/stops in every terminal/cancel/shutdown path with one owned timer and no leak; all tray states pass themes/DPI; picker and P02 UI scenarios pass. |
| 7 Control | Isolate protocol, `ControlServer`, handler, client; route mutations to `AppController`; implement all P01. | Compatibility/security/concurrency/deadline/cancellation/replay/startup/shutdown matrix passes; no duplicate CLI use cases. |
| 8 Notifications | Add normalized notification values; implement P04/P05 and retained actions; resolve G07. | Per-kind title/body/action/image matrix passes; P04 produces no toast/balloon; privacy/localization pass. |
| 9 Settings UI/services | Add `SettingsSession`; make XAML render/forward only; migrate settings, startup, updates, diagnostics, placement; resolve G04–G06. | UI owns no persistence/domain/transport/report collection; CLI↔open-window sync passes; all language keys exist. |
| 10 Diagnostics | Implement approved G01/G02; integrate approved recent logs; bound memory/disk and enforce privacy. | Logging cannot block/crash shutdown; crash path avoids recursion/unsafe dependencies; useful timing data remains; old pipelines deleted. |
| 11 Cleanup | Remove adapters/dead files; colocate source; update projects/docs; add dependency checks; record final performance and architecture exceptions. | No unowned bridge or duplicate owner; full [verification](verification.md) passes; source map matches [architecture](architecture.md). |

## Rollback

- Intermediate PRs may retain an adapter; completed phases may not.
- Short-lived feature flags need a removal phase.
- Never use the structural rewrite alone to justify data or P01 protocol breakage.
