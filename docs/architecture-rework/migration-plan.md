# Migration Plan

## Strategy

Use an incremental replacement rather than a single unreviewable rewrite. Each phase introduces one new owner, routes behavior through it, verifies compatibility, and removes the replaced implementation before the next broad phase. Temporary adapters are allowed only when they have an owner and deletion criterion recorded in the phase.

No phase may silently change an unapproved product behavior. The locked decisions in the plan overview are the only initial behavior changes.

## Phase 0: Baseline and characterization

Deliverables:

- record current project/file/line metrics;
- inventory every CLI command, argument, exit code, JSON response shape, timeout, and privacy/raw behavior;
- record named-pipe security and identity invariants;
- characterize connect, disconnect, reconnect, incoming, bulk actions, startup connect, loss reconnect, suspend/resume, and shutdown races;
- characterize Cold/Warm/Hot transitions, timings, stale snapshots, authorization sequences, preload/release failures, and timer fallbacks;
- characterize settings compatibility, bounds, corrupt file behavior, atomic replacement, and shutdown flush;
- inventory notification kind-to-image mappings and explicitly mark normal disconnect for removal;
- capture tray static states and current animation-only behavior;
- add missing deterministic tests before moving ownership;
- establish the supported Visual Studio/MSVC and Windows SDK versions for the C++26 target;
- change all product and test projects from `stdcpp23` to the selected C++26 mode in one dedicated change, then validate x64 and ARM64 before feature migrations use C++26 facilities.

Exit gate:

- a feature-compatibility matrix exists;
- all high-risk behavior has either an automated characterization test or a named manual scenario;
- the current language baseline is reported accurately; if the C++26 toolchain gate has not passed, migrations continue to build as C++23 without pretending otherwise;
- baseline Debug and Release builds succeed on the supported target platforms.

## Phase 1: Shared models and application command surface

Deliverables:

- introduce `DeviceId`, `DeviceSnapshot`, `DeviceEvent`, `SettingsSnapshot`, `AppSnapshot`, `TraySnapshot`, and normalized error/result values;
- introduce the typed `AppController` command surface without moving implementations yet;
- route existing UI actions and existing control-command dispatch through adapter methods on that surface;
- define subscription lifetime and UI-dispatch rules;
- prohibit XAML and pipe types from application/core models.

Exit gate:

- UI and CLI invoke one command API;
- existing behavior remains implemented by legacy services behind adapters;
- models contain no mutable service references.

## Phase 2: `SettingsStore`

Deliverables:

- implement validated load and format compatibility;
- implement typed settings changes and immutable snapshots;
- implement one debounced writer with dirty revision and bounded retry;
- preserve corrupt-file backup, size limits, temporary write, flush, atomic replace, and final flush;
- migrate app, UI, CLI, startup/update settings, aliases, privacy, reconnect policies, and window bounds;
- remove `SettingsController`, `DeferredSettingsSaver`, and `DeferredSaveCoordinator` after all callers move.

Exit gate:

- no caller takes a settings lock directly;
- tests cover change-during-write, write failure, retry, suspend flush, shutdown flush, corrupt load, and legacy data;
- old persistence coordination is deleted.

## Phase 3: Device execution model

Deliverables:

- introduce serialized device execution context;
- introduce `DeviceWatcher`, `DeviceSession`, `ReconnectPolicy`, and `DeviceService` facade;
- migrate discovery and inventory snapshots;
- migrate connect/disconnect/reconnect and bulk commands;
- migrate incoming connections and per-device/global reconnect policy;
- localize operation epochs, state tokens, close barriers, and timers per session;
- migrate suspend/resume and shutdown semantics;
- publish one consolidated `DeviceEvent` stream;
- delete replaced manager/store/coordinator/lifecycle abstractions.

Exit gate:

- race matrix from Phase 0 passes;
- no mutable connection/session map exists outside `DeviceService`;
- watcher and connection callbacks mutate state only on the device execution context;
- stale async completions cannot publish success or resurrect cancelled work.

## Phase 4: `AppController` becomes the use-case owner

Deliverables:

- move default/last-device selection, bulk actions, settings side effects, presentation mapping, and selected notification policy into `AppController`;
- map a normal disconnect to state refresh only, with no notification;
- route settings and tray state through immutable snapshots;
- reduce `AppRuntime` to composition/lifecycle and platform-message forwarding;
- replace the multi-event `DeviceEventRouter` with one explicit device subscription owned by `AppController`.

Exit gate:

- `AppRuntime` contains no control-command switch and no Bluetooth implementation;
- `AppController` contains no XAML, pipe I/O, JSON file I/O, pressure probes, or toast XML;
- normal disconnect is verified to produce no toast.

## Phase 5: Resource residency containment

Deliverables:

- introduce `ResourceResidencyManager` around the existing proven policy and monitor behavior;
- preserve Cold/Warm/Hot transitions and all timing configuration;
- move freshness, sequence authorization, scheduler fallback, retry, and diagnostics out of `AppRuntime`;
- define the narrow `TrayUi` resource contract (`state`, `interaction`, `preload`, `release`);
- colocate one-use schedule/backoff state privately;
- retain or port the current adaptive-resource tests.

Exit gate:

- `AppRuntime` contains no adaptive timers, sequences, backoff, or pressure values;
- `TrayUi` does not decide residency policy;
- parity tests cover memory pressure, energy saver, fullscreen/presentation, UI pinning, release deferral, clock rollback, stale snapshot, and failed preload/release retry.

## Phase 6: Tray and picker

Deliverables:

- introduce snapshot-driven `TrayUi` and `DevicePicker` rows;
- retain static Idle, Connecting, Connected, and Error visual states;
- retain theme-aware and DPI-correct icon creation;
- remove connecting-animation frames, animation timer, and associated scheduling;
- consolidate flyout lifecycle into one state machine;
- preserve left/right/double-click behavior unless separately changed;
- ensure resource manager receives accurate loaded/initialized/visible/pinned/interaction facts;
- remove direct settings and device-service access from picker controls.

Exit gate:

- no animation timer or frame state remains;
- all four static visual states render correctly under light/dark themes and supported DPI scales;
- picker behavior and adaptive preload/release scenarios pass.

## Phase 7: CLI and named-pipe isolation

Deliverables:

- preserve/version the existing protocol contract;
- isolate parsing/serialization in `ControlProtocol`;
- isolate pipe lifecycle/security/transport in `ControlServer` and `ControlClient`;
- move request-to-use-case mapping into `ControlCommandHandler`;
- route all mutations through `AppController`;
- keep deadlines, cancellation, identity verification, concurrent instance behavior, correlation IDs, replay/deduplication, JSON/raw/privacy behavior, and graceful shutdown;
- make pipe-instance, transfer, retry, and cache records private implementation details.

Exit gate:

- command compatibility matrix is green;
- untrusted client/server tests remain green;
- concurrent, duplicate, timed-out, cancelled, startup-retry, and shutdown requests are covered;
- no CLI-specific command implementation is duplicated in `AppController` or UI.

## Phase 8: Notifications

Deliverables:

- introduce normalized `Notification` and `NotificationKind` values;
- preserve the distinct image mapping for all retained kinds;
- preserve notification activation/actions that remain in scope;
- omit normal disconnect from application notification policy;
- keep XML sanitization private to notification rendering;
- resolve the tray-balloon fallback decision from the feature register.

Exit gate:

- notification matrix verifies title/body/action/image for every retained kind;
- normal disconnect produces no toast or fallback balloon;
- error and reconnect-failed notifications still work;
- privacy/localization are verified.

## Phase 9: Settings window and supporting services

Deliverables:

- introduce `SettingsSession` and reduce XAML code-behind to rendering/intent forwarding;
- migrate aliases, device policies, startup task, privacy, localization, notifications, appearance, update UI, support links, diagnostics, and window placement;
- internalize latest-wins startup behavior in `StartupService`;
- consolidate update behavior according to the approved decision gate;
- consolidate diagnostics according to the approved decision gate;
- preserve all unmodified settings and CLI settings commands.

Exit gate:

- Settings UI owns no persistence, device coordination, update transport, or diagnostics collection;
- settings changed from CLI are reflected in an open window and vice versa;
- localization keys exist in all supported languages.

## Phase 10: Logging and crash reporting

Deliverables:

- implement the approved logging profile;
- implement the approved crash-reporting profile;
- integrate recent log history with diagnostics/crash artifacts where approved;
- validate privacy and bounded disk/memory use;
- delete old logging/crash pipelines after artifact compatibility decisions are documented.

Exit gate:

- failures in logging cannot crash or block shutdown;
- crash path avoids unsafe dependencies and recursion;
- diagnostics remain useful for Bluetooth timing issues;
- no two logging or crash pipelines remain active.

## Phase 11: Boundary enforcement and cleanup

Deliverables:

- remove all migration adapters and dead files;
- colocate headers/implementations and retain only genuinely shared public headers;
- update project files and filters;
- add a build-time boundary check for Core-to-WinUI leakage and forbidden dependency directions;
- update architecture, contribution, troubleshooting, and release documentation;
- record final metrics and explain intentional exceptions to size targets.

Exit gate:

- no TODO compatibility bridge remains without an issue and owner;
- no old service and replacement service coexist;
- full verification plan passes;
- final source map matches the target-architecture document.

## Pull-request slicing

Do not implement one phase as one enormous PR when the phase can be separated safely. Preferred slices are:

1. tests/models;
2. new implementation behind adapter;
3. caller migration;
4. old implementation deletion;
5. documentation and boundary enforcement.

Every slice must compile. Avoid formatting unrelated legacy files during migration.

## Rollback policy

- A phase is not complete until the legacy owner is removed, but intermediate PRs may retain an adapter.
- Feature flags are permitted only for short-lived migration validation and must have a removal phase.
- Data format changes require backward-compatible read and an explicit rollback story.
- Control-protocol changes require versioning; the structural rewrite alone does not justify a protocol break.
