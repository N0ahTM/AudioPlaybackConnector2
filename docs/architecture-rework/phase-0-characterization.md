# Phase 0 characterization matrix

Status: characterization evidence for product/test execution ref
`fb653f1c0a1abf26c2abb2f0784daf81fde4f34e` (2026-08-22). This artifact is reviewed
in a subsequent docs-only commit with equivalent product/test inputs; that docs-only
commit is not a product/test execution ref and does not declare Phase 0 complete.

The matrix records what is implemented, what an existing test proves, and what still
needs a named manual scenario or future deterministic automation. A primitive unit test
is not treated as proof of the corresponding WinRT, shell, pipe, or persisted-file
integration path.

## Evidence conventions

* `AUTOMATED` names an existing test function exactly as it appears in
  `AudioPlaybackConnector2.CoreTests/src`.
* `MANUAL` is a uniquely named scenario. The scenario has not been run by this
  documentation-only slice unless a command is explicitly listed in the validation
  section.
* `GAP` identifies missing coverage or a required future automation seam. A gap is not
  an approval to change behavior.
* Source references use `path:line` and describe the current legacy owner, not the
  target owner from `architecture.md`.

## Baseline and language facts

The performance baseline was measured at source commit
`d2a4587831e8d7155a2fe8b331a38f07d0b4c428`. Product/test execution ref
`fb653f1c0a1abf26c2abb2f0784daf81fde4f34e` is no longer source/test-equivalent to
that baseline: its focused Settings persistence characterization changes are:

```text
M AudioPlaybackConnector2.CoreTests/AudioPlaybackConnector2.CoreTests.vcxproj
M AudioPlaybackConnector2.CoreTests/src/ReconnectControllerTests.cpp
A AudioPlaybackConnector2.CoreTests/src/SettingsPersistenceTests.cpp
M AudioPlaybackConnector2/include/core/Settings.hpp
M AudioPlaybackConnector2/src/core/Settings.cpp
```

The earlier `c2dfd87...` ref added architecture-rework guidance and agent
configuration only. Commit `b295c6c...` adds the focused production/test changes above;
the PCH compatibility fix developed as `764205d...` was integrated before execution ref
`fb653f1...` and keeps the persisted schema and default application-data path unchanged.
The subsequent docs-only commit changes only this artifact, so the product/test input
for this evidence remains equivalent to execution ref `fb653f1...`. The repository
contains
`performance-baseline-2026-08-22.md`; this slice did not rerun its measurements. The
existing baseline evidence is x64 Release-only, so it must not be read as Debug or
ARM64 performance evidence. Separately, the root-provided execution-ref evidence below
records successful Debug and Release build/package outputs for x64 and ARM64, plus
passing x64 CoreTests. Those build artifacts do not imply that ARM64 native execution,
manual desktop scenarios, security review, or package-verification gates were run;
the final explicit clang-format and cppcheck passes are recorded below.

All four native project files explicitly set `<LanguageStandard>stdcpp23</LanguageStandard>`:

* `AudioPlaybackConnector2/AudioPlaybackConnector2.vcxproj:92`
* `AudioPlaybackConnector2.Control/AudioPlaybackConnector2.Control.vcxproj:57`
* `AudioPlaybackConnector2.CoreRuntime/AudioPlaybackConnector2.CoreRuntime.vcxproj:54`
* `AudioPlaybackConnector2.CoreTests/AudioPlaybackConnector2.CoreTests.vcxproj:54`

The accurate language baseline is therefore C++23 (`stdcpp23`), with v143 projects;
there is no C++26 claim. P10 remains gated on the dedicated C++26 x64/ARM64 toolchain,
packaging, and CI validation; that gate has not been attempted. The execution-ref evidence
below records Debug and Release build/package outputs for both supported architectures,
but ARM64 native CoreTests, manual desktop scenarios, security/package-verification
checks, and the C++26 gate remain unrun or unattempted.

## Legacy owner map used by this matrix

| Decision area | Current owner and evidence | Boundary risk to preserve |
|---|---|---|
| P01 control | `CommandLineControlServer` owns pipe transport in `AudioPlaybackConnector2/include/services/CommandLineControlServer.hpp:21-129` and `src/services/CommandLineControlServer.cpp`; `ApplicationHost::HandleControlCommand` owns mapping/use cases at `src/app/ApplicationHost.cpp:1494-2205`; `AudioPlaybackConnector2.Control/src/main.cpp:122-825` owns CLI parsing/presentation. | Protocol transport, command mapping, and presentation are presently intertwined across three components. |
| P02 resources | `ApplicationHost::InitializeAdaptiveResources`, `HandleResourcePressureSnapshot`, `EvaluateAdaptiveResources`, and `ScheduleAdaptiveResourceEvaluation` at `src/app/ApplicationHost.cpp:709-910`; `AdaptiveResourcePolicy` and `ResourcePressureMonitor` are tested primitives. | Sequence/freshness/authorization, timers, retries, and Tray preload/release calls must remain coupled in behavior even when ownership moves. |
| P03/P09 tray | `TrayController` owns flyout/menu and direct UI/device/settings references; `TrayIcon` owns frames and rendering; `ApplicationHost` owns the connecting timer at `include/app/ApplicationHost.hpp:154-176` and `src/app/ApplicationHost.cpp:1248-1300,2430-2434`. | Connecting animation, click suppression, theme/DPI, and shell re-registration are high-risk UI behavior. |
| P05/P04 notifications | `NotificationService` renders six retained kinds at `src/services/NotificationService.cpp:319-400`; `ApplicationHost` chooses policy and calls it at `src/app/ApplicationHost.cpp:2309-2378`. | Normal disconnect currently calls `ShowDeviceDisconnected`, which conflicts with locked P04. G07 fallback remains unresolved. |
| P06 devices | `DeviceManager` owns sessions, operation epochs, close barriers, discovery, incoming state, reconnect scheduling, and power/shutdown at `include/core/DeviceManager.hpp:26-230` and `src/core/DeviceManager.cpp:123-2260`; helpers are `DeviceOperationCoordinator`, `DeviceSessionStore`, `DeviceDiscoveryService`, and `ReconnectController`. | WinRT callbacks, connection tokens, close-before-reconnect, and stale completion protection are not covered by an end-to-end fake. |
| P07 settings | `Settings` owns mutable data and exposed locks at `include/core/Settings.hpp:77-121`; JSON load/save is `src/core/Settings.cpp:146+`; debounce/retry/final flush is split between `DeferredSettingsSaver` and `DeferredSaveCoordinator`. Commit `b295c6c...` adds a scoped persistence-directory seam and direct file characterization; the PCH/CoreRuntime include boundary is restored before execution ref `fb653f1...`. | Direct temp-file coverage now exercises round-trip, corrupt preservation, validation, replacement failure, retry, and a later final save; oversized-input rejection remains a known bounded manual defect; host lifecycle and concurrent real-write behavior remain gaps. |

## P01 — CLI, named pipe, and control compatibility

The wire constants and schema are in `AudioPlaybackConnector2/include/control/CommandProtocol.hpp:15-391`:
protocol version 2; request/response/ack magic values; 64 KiB maximum UTF-16 payload;
4 KiB pipe buffers; four pipe instances; `CommandType` values 1–16; target kinds
`Id`, `Name`, `Mac`, `Last`, `Auto`, `Alias`, and `Default`; flags `Json` and `Raw`;
exit codes `0`, `3`–`9`; and a non-empty 128-bit correlation ID. `ReadRequest` and
`ReadResponse` validate headers before allocation, transfer exact bytes with overlapped
I/O, and return typed `IoStatus` values.

| ID / required behavior | Legacy evidence | Existing automated evidence or named scenario | Status and required future automation |
|---|---|---|---|
| P01-01 command names and arguments: `show`, `settings`, `status`, `list`, `connect`, `disconnect`, `reconnect`, `toggle`, `disconnect-all`, `reconnect-all`, `default show/set/clear`, and `alias list/set/clear`; target selectors and `--` handling | CLI grammar/help at `AudioPlaybackConnector2.Control/src/main.cpp:122-148`; parser at `:245-518`; enum at `CommandProtocol.hpp:25-45`. | `MANUAL P01-CLI-Command-Argument-Matrix`: execute every help form, every selector (`--id`, `--name`, `--mac`, `--alias`, `--last`, `--default`, positional), alias value forms, duplicate selectors, missing values, leading-dash values, case variants, and unknown options. | `GAP`: no CoreTests target the CLI parser or `wmain`; add parser-table tests before changing it. |
| P01-02 exit behavior and local failures | CLI returns help/0, invalid syntax/3, unavailable/7, indeterminate/9, or server response codes at `main.cpp:711-825`; protocol values at `CommandProtocol.hpp:49-58`; host maps target/operation outcomes at `ApplicationHost.cpp:1515-2205`. | `MANUAL P01-CLI-Exit-Code-Matrix`: assert process exit and stdout/stderr for success 0, invalid request 3, not found 4, ambiguous 5, operation failure 6, unavailable 7, busy 8, and indeterminate 9, including server-not-running and late UI action cases. | `GAP`: no executable-level exit assertions; add CLI integration tests with a fake transport and fake controller result. |
| P01-03 text, JSON, raw, and privacy output | `ApplicationHost.cpp:1494-1540,1640-1720,1738-2205` emits text or JSON; `ResponseId`, `ResponseName`, `InsertDeviceJson`, and `redactOutput` are at `:199-220`; CLI error JSON is at `main.cpp:219-229,711-825`. Privacy mode redacts IDs/names unless `--raw`. | `MANUAL P01-CLI-Output-Schema-Privacy`: capture `list`, `status`, `default`, and `alias` in text/JSON with privacy off/on and raw off/on; validate keys, `ok`, `exitCode`, `action`, `id`, `name`, `displayName`, `connected`, `known`, `privacyRedacted`, and adaptive-resource status fields; verify no device identity leaks to stderr. | `GAP`: no parser/presentation schema snapshot tests, and no privacy-output test invokes the production handler. Add golden JSON/text tests without depending on localized prose. |
| P01-04 framing, bounds, malformed input, and response schema | `CommandProtocol.hpp:21-23,71-120,145-205,227-391` defines fixed headers, UTF-16 even-byte payloads, maximum payload, command/target/flag validation, response exit validation, and correlation-preserving acknowledgements. | `AUTOMATED`: `TestCommandProtocolRoundTrip`, `TestCommandProtocolDelayedResponseReader`, `TestCommandProtocolStrictValidation`, `TestCommandProtocolRejectsInvalidHeader` (`AudioPlaybackConnector2.CoreTests/src/ReconnectControllerTests.cpp:271-423`); `TestProductionRoundTripFragmentationAndRearm`, `TestMalformedTimeoutOversizeAndRecovery`, `TestMaximumRequestAndIdleCachePruning` (`CommandLineControlServerTests.cpp:170-229,403-465,813-870`). | Covered at protocol/server primitive level. `GAP`: add negative response-header cases and a cross-process compatibility fixture for version/magic/odd-byte payloads. |
| P01-05 identity, ACL, and rogue endpoint rejection | Server creates current-user-only protected security attributes and validates the client at `CommandLineControlServer.cpp:173-190,273-325`; client captures live server PID/creation time and executable/package identity at `AudioPlaybackConnector2.Control/src/main.cpp:550-647`. | `AUTOMATED`: `TestStartupSquattingAndSecurityDescriptor` (`CommandLineControlServerTests.cpp:613-776`); `TestRejectedEndpointMayLaunchTrustedApp`, `TestIndeterminateReplaysOnlyToSameServer`, `TestLaunchAndReplayBoundaries` (`CommandClientTests.cpp:142-233`); security helper checks are included in the startup-squatting test. | Covered for same-user fixtures and current-process identity. `MANUAL P01-Control-Identity-Deadline-Retry`: packaged/unpackaged client, another-user process, rogue same-user server, and server replacement. `GAP`: no multi-user/packaged deployment test in CoreTests. |
| P01-06 concurrency, mutation serialization, duplicate requests, cache/replay, and correlation conflicts | `ApplicationHost.cpp:1528-1555` uses `m_controlMutationMutex` and returns Busy for concurrent mutators; server request records/cache/pending deliveries are declared at `CommandLineControlServer.hpp:29-45,111-128` and implemented in `CommandLineControlServer.cpp`. | `AUTOMATED`: `TestParallelDuplicatesAndCorrelationConflict`, `TestDisconnectBeforeResponseRetriesExactlyOnce`, `TestDisconnectAfterResponseBeforeAckRetriesExactlyOnce`, `TestMaximumRequestAndIdleCachePruning` (`CommandLineControlServerTests.cpp:230-402,813-870`); `TestIndeterminateReplaysOnlyToSameServer`, `TestOverallDeadlineBoundsReplay` (`CommandClientTests.cpp:120-174`); `TestBeginCancelRace` (`ControlUiActionGateTests.cpp:49-67`). | Primitive coverage exists for deduplication and one mutation lock. `GAP`: no test asserts every mutating command is serialized through the production host while a late UI action is pending; add controller/handler integration coverage. |
| P01-07 deadlines, cancellation, and late mutation reporting | Server options default to request 5 s, response 5 s, acknowledgement 1.5 s, startup retry 250 ms at `CommandLineControlServer.hpp:27-38`; protocol transfer checks stop event and absolute deadline at `CommandProtocol.hpp:227-277`; host forwards `stop_token`/deadline and classifies UI timeout as indeterminate at `ApplicationHost.cpp:56-82,1165-1210,1930-1970`. | `AUTOMATED`: `TestCommandProtocolTimeoutAndCancellation` (`ReconnectControllerTests.cpp:374-403`); `TestOverallDeadlineBoundsReplay` (`CommandClientTests.cpp:153-174`); `TestCancellationBeforeDispatch`, `TestTimeoutAfterDispatchStarted`, `TestBeginCancelRace` (`ControlUiActionGateTests.cpp:17-67`); `TestMalformedTimeoutOversizeAndRecovery` (`CommandLineControlServerTests.cpp:403-465`). | Covered for transport and UI-action gate. `MANUAL P01-Control-Late-Mutation-Indeterminate`: force timeout before and after dispatch, cancel a refresh/connect, then verify no unreported mutation and correct 7/9 outcome. Add handler integration tests with a deterministic clock/executor. |
| P01-08 startup retry, rearm, graceful stop, and handle lifetime | `CommandLineControlServer::Start/TryStart/Stop` and retry timer at `CommandLineControlServer.cpp:247-420`; host starts/stops server at `ApplicationHost.cpp:694-706,351-352`; client retries four instances and launches the packaged app at `main.cpp:589-647,711-825`. | `AUTOMATED`: `TestStopLifecycleAndRearmRetry`, `TestStartStopHandleStability` (`CommandLineControlServerTests.cpp:466-612,777-812`); `TestLaunchAndComplete`, `TestRejectedEndpointMayLaunchTrustedApp`, `TestLaunchAndReplayBoundaries` (`CommandClientTests.cpp:108-233`). | Primitive lifecycle is covered. `MANUAL P01-Control-Startup-Shutdown`: start with occupied pipe slot(s), recover, invoke all command classes during shutdown, and verify no post-stop callback/response. Add a packaged app startup/shutdown test. |

## P02 — adaptive resource residency

The policy defaults are `BackgroundPressureToColdDelay=5 s`, `ColdToWarmDelay=45 s`,
`WarmToHotDelay=20 s`, and `InteractionHotHold=10 s` in
`AudioPlaybackConnector2/include/app/AdaptiveResourcePolicy.hpp:16-42`. The monitor
polls every 5 s, uses 30 s constrained polling, and publishes a 30 s heartbeat from
`ResourcePressureMonitor.hpp:18-27`. `ApplicationHost` treats pressure snapshots older
than 75 s as stale (`src/app/ApplicationHost.cpp:47,775-825`), tracks constrained
sequence authorization, schedules a Win32 timer with a DispatcherQueue fallback, and
retries failed preload/release actions with bounded backoff (`:855-910`).

| ID / required behavior | Legacy evidence | Existing automated evidence or named scenario | Status and required future automation |
|---|---|---|---|
| P02-01 Cold/Warm/Hot startup, transitions, configured delays, and actions | `AdaptiveResourcePolicy` state/input/decision types at `include/app/AdaptiveResourcePolicy.hpp:14-67`; host evaluation at `ApplicationHost.cpp:775-849`; Tray preload/release calls at `:814-815`. | `AUTOMATED`: `TestWarmupAndActions`, `TestImmediateMemoryPressureAndStagedRecovery`, `TestBackgroundPressureGraceAndHysteresis`, `TestNeutralMemoryMaintainsButNeverCreatesHotState` (`AdaptiveResourcePolicyTests.cpp:31-159`). | Policy timing/action behavior is covered with a fake clock. `GAP`: no host-to-Tray integration test verifies the same transitions with real resource state. |
| P02-02 memory pressure, energy saver, fullscreen/presentation, visibility, pinning, and interaction hold | `ResourcePressureValues` states/probes at `include/app/ResourcePressureState.hpp:7-65`; host maps pressure and UI facts into policy input at `ApplicationHost.cpp:785-807`; Tray reports only `IsDevicePickerVisibleOrTransitioning()` and host hardcodes `.UiPinned = false`. | `AUTOMATED`: `TestBackgroundPressureGraceAndHysteresis`, `TestVisibleUiPinsAColdBackgroundDecision`, `TestInteractionTemporarilyOverridesCold`, `TestPreloadPermissionMustRemainStableForFullPromotionDelay` (`AdaptiveResourcePolicyTests.cpp:91-234`). `AUTOMATED`: `TestReducerPreservesIndependentSignals` (`ResourcePressureMonitorTests.cpp:62-91`). | Policy-level coverage exists. `GAP`: production host never supplies a true explicit pin fact; add a Tray/UI-fact integration test and preserve pin/interaction semantics before moving ownership. `MANUAL P02-Residency-Desktop-Pressure-Integration`: pressure, battery saver, fullscreen/presentation, picker visible/pinned/interaction, and release deferral. |
| P02-03 freshness, sequence ordering, positive authorization, and clock rollback | Freshness/authorization helpers at `include/app/ResourcePressureState.hpp:57-65`; host rejects stale/out-of-order snapshots at `ApplicationHost.cpp:737-754,775-787`; policy resets temporal state on rollback. | `AUTOMATED`: `TestSnapshotFreshnessFailsClosed`, `TestPositiveAuthorizationAndAdaptivePollCadence`, `TestMonitorPublishesPeriodicLivenessHeartbeat` (`ResourcePressureMonitorTests.cpp:142-197,348-367`); `TestClockRollbackRestartsStabilityWindows` (`AdaptiveResourcePolicyTests.cpp:235-248`); `RunAdaptiveResourceDiagnosticsTests` (`AdaptiveResourceDiagnosticsTests.cpp:18-25`). | Helper/monitor/policy behavior is covered. `GAP`: no test sends out-of-order snapshots through `ApplicationHost::HandleResourcePressureSnapshot`; add a sequence/freshness integration test. |
| P02-04 failed and partial probes, constrained cadence, and heartbeat/liveness | Probe/reducer definitions at `ResourcePressureState.hpp:27-65`; monitor lifecycle/probe scheduling at `src/app/ResourcePressureMonitor.cpp:24-305`. | `AUTOMATED`: `TestReducerHandlesMemoryTransitionsAndPartialFailures`, `TestFailedProbesRevokePositiveAuthorization`, `TestPartialMemoryProbeFailuresRemainFailClosed`, `TestIncompleteMemoryProbesPauseSignalWaits`, `TestPositiveAuthorizationAndAdaptivePollCadence` (`ResourcePressureMonitorTests.cpp:28-197`). | Covered at monitor/reducer level. `GAP`: no long-running packaged/desktop heartbeat observation or diagnostic export assertion. |
| P02-05 release deferral and UI resource facts | `TrayController::ShowDevicePicker`, `TryHideDevicePicker`, `ReleaseDevicePicker`, and `ReleaseDevicePickerOnUIThread` at `src/services/TrayController.cpp:273-414,744-827`; host uses loaded/initialized facts at `ApplicationHost.cpp:825-845`. | `AUTOMATED`: `TestVisibleUiPinsAColdBackgroundDecision`, `TestInteractionTemporarilyOverridesCold` (`AdaptiveResourcePolicyTests.cpp:187-234`); `TestInventoryFreshnessAndInvalidation`, `TestLateInventoryCallbackAfterRelease` (`DevicePickerSnapshotTests.cpp:27-71`). | Policy and snapshot-cache pieces are covered. `MANUAL P02-Residency-Preload-Release-Lifecycle`: preload, open, interaction hold, close, deferred release, explicit release, reopen, and late callback. `GAP`: no actual XAML resource release test. |
| P02-06 retry, scheduler fallback, supersession, and safe shutdown | `AdaptiveActionRetryBackoff` is used at `ApplicationHost.cpp:769,829-851`; adaptive schedule state/timer/fallback is `:855-910`; teardown stops monitor/timers at `:296-324`. | `AUTOMATED`: `TestAdaptiveActionRetryBackoffIsBoundedAndResettable`, `TestAdaptiveScheduleRejectsSupersededAndEarlyCallbacks` (`AdaptiveResourcePolicyTests.cpp:273-312`); `TestMonitorLifecycleAndLateCallbackBarrier`, `TestMonitorCanStopFromItsOwnCallback`, `TestMonitorCanBeDestroyedFromItsOwnCallback`, `TestExternalStopWaitsForSelfStoppedCallback`, `TestConcurrentStartAndStopRemainSafe`, `TestRepeatedLifecycleDoesNotLeakHandles` (`ResourcePressureMonitorTests.cpp:198-467`). | Timer/monitor primitives are covered. `GAP`: no host scheduling-failure test proves exactly one active fallback and no wakeup after teardown; add deterministic scheduler injection. |
| P02-07 diagnostics and retained energy/pressure state | Status JSON includes adaptive residency, freshness, authorization, preload facts, pressure, activity, and energy at `ApplicationHost.cpp:1653-1688`; names are in `AdaptiveResourceDiagnostics.hpp:12-54`. | `AUTOMATED`: `RunAdaptiveResourceDiagnosticsTests` (`AdaptiveResourceDiagnosticsTests.cpp:18-25`). `MANUAL P02-Status-Diagnostics`: compare `status --json` before/after stale, constrained, recovery, and shutdown states. | `GAP`: no production status-schema snapshot test; add one without exposing mutable policy internals. |

## P03 — Connecting animation invariant

P03 is part of Phase 0 even though its timer currently lives in `ApplicationHost`.
`RefreshTrayVisualState` starts a 75 ms timer only for `TrayIconState::Connecting` and
kills it for Connected, Error, and Idle (`src/app/ApplicationHost.cpp:1248-1300`).
`WM_TIMER` advances an eight-frame, four-size `TrayIcon` animation and kills the timer
when the frame update fails (`src/app/ApplicationHost.cpp:2430-2434`; frames are
declared at `include/ui/TrayIcon.hpp:53-70`). Teardown kills the animation timer
(`ApplicationHost.cpp:351-359`).

| ID / required behavior | Existing automated evidence or named scenario | Status and gap |
|---|---|---|
| P03-01 start only for active connect/reconnect work; prompt stop on Idle, Connected, Error, cancellation, and shutdown | `MANUAL P03-Tray-Connecting-Animation-Terminals`: begin connect and reconnect, observe progress; drive each terminal/cancel/shutdown path; assert no stale frame/wakeup after terminal state. | `GAP`: no TrayIcon/ApplicationHost animation test; add a fake timer/clock test proving one bounded owner and every terminal cleanup path. |
| P03-02 eight frames, theme/DPI-correct icon selection, and no orphan timer after shell re-registration | `MANUAL P03-Tray-Connecting-Animation-Theme-Dpi`: light/dark at 100/125/150/200% DPI, Explorer restart/taskbar recreation, and state restoration. | `GAP`: no automated frame/timer/shell test; preserve the current eight-frame and four-size assets. |

## P04/P05 — notification policy, image mapping, and actions

`NotificationService` currently renders these six kinds:

| Retained kind | Current image | Current action and arguments | Current renderer |
|---|---|---|---|
| App started | `ms-appx:///Images/ToastInfo.png` | None; silent; 7 s expiry | `NotificationService::ShowAppStarted` (`src/services/NotificationService.cpp:319-330`) |
| Device connected | `ToastConnected.png` | `Reconnect`, action `reconnect`, `deviceId=id`; long sound; 1 min expiry | `ShowDeviceConnected` (`:332-345`) |
| Device disconnected | `ToastWarning.png` | None; silent; 1 min expiry | `ShowDeviceDisconnected` (`:347-358`) |
| Auto reconnect started | `ToastReconnect.png` | None; silent; 1 min expiry | `ShowAutoReconnect` (`:360-371`) |
| Auto reconnect failed | `ToastError.png` | `Retry`, action `retry`, `deviceId=id`; looping alarm; 1 h expiry | `ShowAutoReconnectFailed` (`:373-385`) |
| Update available | `ToastInfo.png` | localized update action, action `openUpdate`; silent; 6 h expiry | `ShowUpdateAvailable` (`:387-400`) |

Every retained kind has an explicit mapping, but AppStarted and UpdateAvailable
currently share `ToastInfo.png`; the P05 acceptance check must confirm that this shared
asset is intentional or record a product decision. P05 does not authorize inventing a
new image. `ToastXmlSanitization` protects XML escaping and invalid UTF-16 handling at
`src/services/ToastXmlSanitizer.hpp` and `ReconnectControllerTests.cpp:425-443`.

| ID / required behavior | Legacy policy evidence | Existing automated evidence or named scenario | Status and required future automation |
|---|---|---|---|
| P04-01 normal disconnect updates state/tooltip/CLI/diagnostics but never emits toast or balloon | `ApplicationHost::OnDeviceDisconnected` calls `ShowDeviceDisconnected` unconditionally when the notification service exists (`src/app/ApplicationHost.cpp:2321-2339`); state refresh is also scheduled. | `MANUAL P04-Normal-Disconnect-No-Notification`: disconnect a normally connected device with notifications enabled and inspect toast history, fallback-balloon history, tooltip, CLI status, and diagnostics. | `GAP` and current behavior conflict: the call site violates locked P04. Add a deterministic event-to-policy test before moving notification policy; do not infer G07 fallback approval. |
| P05-01 AppStarted image and payload | Renderer mapping above. Startup call is `ApplicationHost.cpp:553-555`. | `MANUAL P05-Notification-Kind-Image-Action-Matrix` (AppStarted row): record XML image, title/body, sound, expiry, and absence of action. | `GAP`: no NotificationService mapping test; add normalized kind→image/action table assertions. |
| P05-02 DeviceConnected image/action | `ShowDeviceConnected` at `NotificationService.cpp:332-345`; callback parsing is `:407-444`. | `MANUAL P05-Notification-Kind-Image-Action-Matrix` (Connected row): invoke reconnect action and verify the correct device only. | `GAP`: no image/action or invocation test; add action parsing and target routing tests. |
| P05-03 DeviceDisconnected mapping retained for non-normal policy decisions | `ShowDeviceDisconnected` at `NotificationService.cpp:347-358`; normal event currently invokes it at `ApplicationHost.cpp:2321-2339`. | `MANUAL P05-Notification-Kind-Image-Action-Matrix` (Disconnected row), combined with `P04-Normal-Disconnect-No-Notification`. | `GAP`: map exists but policy is wrong for a normal disconnect; add a negative no-toast assertion and retain mapping only if a separately approved exceptional kind needs it. |
| P05-04 AutoReconnect image/action | `ShowAutoReconnect` at `NotificationService.cpp:360-371`; trigger call at `ApplicationHost.cpp:2346-2357`. | `MANUAL P05-Notification-Kind-Image-Action-Matrix` (AutoReconnect row): assert `ToastReconnect.png`, no action, and redaction behavior. | `GAP`: no renderer/policy test. |
| P05-05 AutoReconnectFailed image/action | `ShowAutoReconnectFailed` at `NotificationService.cpp:373-385`; trigger call at `ApplicationHost.cpp:2361-2378`. | `MANUAL P05-Notification-Kind-Image-Action-Matrix` (Failed row): assert `ToastError.png`, `retry`, and device target. | `GAP`: no renderer/policy/action test. |
| P05-06 UpdateAvailable image/action | `ShowUpdateAvailable` at `NotificationService.cpp:387-400`; startup/update orchestration is `StartupUpdateCoordinator` and `ApplicationHost`. | `MANUAL P05-Notification-Kind-Image-Action-Matrix` (Update row): assert `ToastInfo.png`, trusted update action, no raw version/identity leak in privacy mode, and expiry. | `GAP`: no mapping test; G04 automatic/manual behavior remains gated and must not be changed here. |
| P05-07 fallback, localization, privacy, and teardown | Toast initialization/teardown and callback lifetime are `NotificationService.cpp:49-180,258-317,407-444`; `ToastXmlSanitization` only covers XML. Changelog records the old balloon fallback removed; G07 is still unresolved. | `AUTOMATED`: `TestToastXmlSanitization` (`ReconnectControllerTests.cpp:425-443`). `MANUAL P05-Notification-Privacy-Localization-Teardown`: all eight languages, privacy mode, unavailable AppNotificationManager, callback during teardown, and action after teardown. | `GAP`: no image mapping, localization, privacy, fallback-decision, or teardown integration tests. Do not claim fallback is approved or required. |

## P06 — Bluetooth operations, races, and lifecycle

`DeviceManager` serializes mutable state under `wil::srwlock`, tracks operation tokens
and epochs in `DeviceOperationCoordinator`, stores active connection/token state in
`DeviceSessionStore`, and coordinates reconnect timers through `ReconnectController`.
The high-risk paths are visible at `DeviceManager.cpp:337-706` (connect/reconnect),
`:706-820` (bulk/policy), `:982-1210` (disconnect/close barriers), `:1211-1528`
(WinRT connection setup), `:1544-1789` (operation/close-barrier cleanup),
`:1829-2024` (watcher/callback/loss), and `:2024-2247` (retry attempts).

| ID / required behavior | Existing automated evidence or named scenario | Status and required future automation |
|---|---|---|
| P06-01 watcher start/stop/restart, lifecycle coalescing, generation, and stale watcher callback | `AUTOMATED`: `TestStartCompletesToIdle`, `TestReentrantStopIsAppliedByCurrentExecutor`, `TestConcurrentPermanentStopWaitsForSettlement`, `TestDuplicateRequestsCoalesceOnlyWhileInFlight`, `TestStaleCompletionCannotReleaseNewerWork`, `TestConcurrentRequestStormUsesSingleExecutor` (`LatestServiceLifecycleStateTests.cpp:22-137`); device-specific watcher ownership is `DeviceManager.cpp:123-139,1829-1890`. | `MANUAL P06-Watcher-Restart-Stale-Callback`: start/stop/restart discovery, inject add/remove/inventory callbacks after stop/release, and assert no stale mutation. `GAP`: add a fake DeviceDiscoveryService integration test. |
| P06-02 connect success/failure/cancel, duplicate/overlap, and stale completion | `AUTOMATED`: `TestDuplicateBeginDoesNotInvalidateOwner`, `TestTransitionAndStaleCompletion`, `TestFailureReportCanBeClaimedExactlyOnce`, `TestInvalidInputsAreNoOps`, `TestTokenExhaustionFailsClosed` (`DeviceOperationCoordinatorTests.cpp:17-113`); `TestCancellationBeforeDispatch`, `TestTimeoutAfterDispatchStarted` (`ControlUiActionGateTests.cpp:17-48`) cover analogous UI action gates only. | `MANUAL P06-Device-Connect-Overlap`: two devices, duplicate same-device connect, connect vs disconnect/reconnect overlap, cancellation before/after WinRT start, failure after stale callback. `GAP`: no fake AudioPlaybackConnection or DeviceManager integration test. |
| P06-03 disconnect ordering, close-before-reconnect barrier, connection-token cleanup, and incoming state | Cleanup/barrier implementation is `DeviceManager.cpp:982-1210,1544-1789`; incoming enable/disable is `:307-335,495-559`. | `MANUAL P06-Disconnect-Close-Token`: power off/close a connected device, reconnect immediately, verify old StateChanged token is revoked before new connection and no zombie callback can close the new connection. `MANUAL P06-Incoming-Connection`: enable/disable incoming, discovered device, and cleanup restore. `GAP`: add deterministic close-barrier timeout and token-cleanup tests. |
| P06-04 reconnect backoff, retry/exhaustion/recovery, timer claim/deferral/abort, and stale timer tokens | `AUTOMATED`: `TestFullBackoffSequence`, `TestSuccessAndStaleTokens`, `TestCancellationAndTimerCreationFailure`, `TestObservedConnectionInvalidatesAttempt`, `TestUnknownConnectionSuccessDoesNotCreateState`, `TestBlockedTimerDoesNotRemainPending`, `TestBusyTimerDeferralPreservesReconnect`, `TestAbortReleasesClaimedAttempt`, `TestAbortDoesNotMutateNewerTimer` (`ReconnectControllerTests.cpp:22-186`). | `AUTOMATED`: `TestReconnectPolicyDoesNotBecomeUserCancellation`, `TestReconnectPolicyDoesNotClearUserCancellation` (`ReconnectControllerTests.cpp:187-216`). `MANUAL P06-Reconnect-Loss-Retry-Recovery`: unexpected loss, retries through exhaustion, manual cancel, policy disable/enable, and successful recovery. `GAP`: no DeviceManager timer/WinRT integration test. |
| P06-05 manual cancellation, global/per-device policy, bulk operations, and cascade ordering | `DeviceManager.cpp:706-820,1789-1828`; reconnect helper policy tests above; planner tests below. | `AUTOMATED`: `TestReconnectPolicyDoesNotBecomeUserCancellation`, `TestReconnectPolicyDoesNotClearUserCancellation`; `TestMostRecentlyConnectedPromotionIsIdempotent`, `TestMostRecentlyConnectedPromotionRemovesDuplicates`, `TestReconnectPlanHonorsAllSavedDevicePolicies`, `TestReconnectPlanHonorsPerDevicePolicyWithoutMruHistory`, `TestReconnectPlanRejectsInvalidAndExcessState` (`AutoReconnectPlannerTests.cpp:24-103`). `MANUAL P06-Bulk-Cascade`: disconnect-all/reconnect-all with pending timers and two connected devices; assert no victim is treated as an unexpected loss. | `GAP`: planner/controller tests do not prove DeviceManager cascade behavior; add a serialized multi-device fake. |
| P06-06 incoming connection, startup connect, and saved policy application | Startup planner is `AutoReconnectPlanner`; DeviceManager incoming path is `DeviceManager.cpp:307-559`; host applies settings and starts watcher at `ApplicationHost.cpp:513-549`. | `MANUAL P06-Startup-Incoming`: launch with two saved startup devices, per-device/global policy, incoming enabled, and one unavailable device; assert order, bounded retries, and no duplicate session. `GAP`: no startup-to-DeviceManager integration test. |
| P06-07 suspend/resume and reconnect-attempt accounting | `PowerTransitionCoordinator` invokes `SuspendForPowerTransition`/`ResumeAfterPowerTransition` at `src/app/PowerTransitionCoordinator.cpp:80-107`; DeviceManager suspends/clears connections at `DeviceManager.cpp:203-295`. | `AUTOMATED`: `TestResumeReconnectCountsOnlyActuallyStartedAttempts`, `TestResumeReconnectPreservesPendingTargetsAcrossSuspendCycles` (`AppWorkCoordinatorTests.cpp:459-499`). `MANUAL P06-Suspend-Resume`: sleep/resume during connect, connected, pending reconnect, and incoming enable; verify watcher restart and no lost target. | `GAP`: existing tests cover resume bookkeeping only, not WinRT close/reopen. |
| P06-08 safe shutdown while busy, callback lifetime, and no resurrection | `ShutdownForProcessExit` cancels operations/timers, signals barriers, revokes tokens, clears sessions, and detaches connections at `DeviceManager.cpp:141-201`; host teardown order is `ApplicationHost.cpp:296-390`. | `AUTOMATED`: `TestConcurrentPermanentStopWaitsForSettlement`, `TestStaleCompletionCannotReleaseNewerWork` (`LatestServiceLifecycleStateTests.cpp:48-107`); `TestMonitorCanBeDestroyedFromItsOwnCallback` is analogous callback lifetime coverage. `MANUAL P06-Shutdown-Busy`: exit during connect/disconnect/reconnect/close barrier and late StateChanged/discovery callback; assert no crash, no new timer, and no session resurrection. | `GAP`: no DeviceManager shutdown integration test; add fake callback/lifetime harness. |

## P07 — JSON settings compatibility, atomicity, and persistence races

The persisted JSON schema is emitted by `Settings::Save` at
`AudioPlaybackConnector2/src/core/Settings.cpp:312+`:

```text
globalConnectOnStartup, globalReconnectOnConnectionLoss,
allowIncomingConnections, startWithWindows, showNotifications,
useSystemBackdropEffects, privacyModeEnabled, language,
lastUpdateCheckUnixSeconds, lastNotifiedUpdateVersion,
defaultDeviceMode, defaultDeviceId,
settingsWindowBounds { x, y, width, height, dpi },
devices [{ id, name, alias, connectOnStartup, reconnectOnConnectionLoss }],
lastConnectedIds
```

Load retains the legacy `globalAutoReconnect` and per-device `autoReconnect` values
(`Settings.cpp:199,261`), parses into locals before taking the settings lock,
deduplicates IDs, bounds arrays and strings, and moves an unreadable file to
`.corrupt.bak`, then probes `.corrupt.1.bak` through `.corrupt.99.bak` while those
paths exist (`:116-137,146+`). At saturation, the loop leaves `.corrupt.99.bak`
selected and `MOVEFILE_REPLACE_EXISTING` may overwrite that existing backup. Save
snapshots a revision, validates persistability, writes `AudioPlaybackConnector2.json.tmp`,
calls `FlushFileBuffers` at `:411`, then atomically replaces the destination with
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` at `:415` and removes the temp
file on failure (`:312+`). The limits
are 4 MiB file size, 384 devices/IDs, 512 ID characters, 256 name characters, 128 alias
and version characters, and DPI 48–960 (`include/core/SettingsLimits.hpp:11-28`).

Commit `b295c6c...` adds a constructor-injected persistence directory
(`include/core/Settings.hpp:83-84,120`) so direct Windows file tests can use a scoped
temporary directory while the empty default still resolves the normal application-data
path (`Settings.cpp:433+`). The PCH compatibility fix developed as `764205d...` and
integrated before execution ref `fb653f1...` makes `Settings.cpp` include `<pch.h>` unconditionally
(`Settings.cpp:1`) while the CoreRuntime project provides the unconditional `pch.h`
PCH (`AudioPlaybackConnector2.CoreRuntime.vcxproj:47-49`); the CoreTests Settings
compilation resolves the CoreRuntime include/res directories
(`AudioPlaybackConnector2.CoreTests.vcxproj:125-126`).
The CoreTests project includes the new source and `RunSettingsPersistenceTests` is
defined and registered through its runner at
(`AudioPlaybackConnector2.CoreTests.vcxproj:94`, `SettingsPersistenceTests.cpp:226,231-236`);
the aggregate runner invokes it at `ReconnectControllerTests.cpp:458,500`.

The externally bounded >4 MiB sparse-fixture path is a known legacy defect, not
automated persistence coverage: the file-size rejection can reach legacy
`std::exception` recovery logging and stall before the corrupt backup is moved.
`MANUAL P07-Settings-Oversized-Rejection-Bounded`: create a sparse file just over the
4 MiB limit through an externally bounded fixture, invoke `Settings::Load`, impose an
external timeout, and record rejection, logging stall, and whether `.corrupt.bak` was
created. At execution ref `fb653f1`, the oversized direct-load automation was removed
from `SettingsPersistenceTests` and its runner; this documents that completed removal,
not an outstanding remediation. No production fix is approved in Phase 0. The retained
persistence-test inventory is six behaviors—`TestMissingCurrentAndRoundTrip`,
`TestLegacyAndPartialInputNormalization`, `TestMalformedAndCorruptInputPreservation`,
`TestValidationFailureLeavesRevisionDirty`, `TestFailedReplacePreservesExistingFileAndCanRetry`,
and `TestFinalSaveCapturesLaterMutation`; `TestOversizedInputIsPreserved` is no longer
present in the execution-ref test source.

| ID / required behavior | Existing automated evidence or named scenario | Status and required future automation |
|---|---|---|
| P07-01 current/legacy JSON formats and field compatibility | Load/save evidence above; `SettingsData` is `include/core/SettingsData.hpp:7-44`. | `AUTOMATED`: `TestMissingCurrentAndRoundTrip` covers missing-file defaults, current-schema save/load, clean revisions, temp cleanup, and every populated persisted field (`SettingsPersistenceTests.cpp:82-102`); `TestLegacyAndPartialInputNormalization` covers legacy global/per-device `autoReconnect`, unsupported language, partial bounds, duplicate/invalid device entries, and recent-ID normalization (`:103-129`). The final x64 Debug/Release CoreTests runs below include these registered tests. `MANUAL P07-Settings-Compatibility-Formats`: unknown/omitted fields, old default mode, aliases, placement, all supported language IDs, and upgrade from a shipped file. | Direct temp-path parser/round-trip coverage now exists. `GAP`: no production ApplicationData/package-upgrade or UI/controller round-trip; unknown-field and full legacy-file matrix remain manual. |
| P07-02 malformed, partial, corrupt, oversized, invalid UTF-16, duplicate, and out-of-bounds data | `Settings.cpp:146+`; limits at `SettingsLimits.hpp:31-67`; corrupt-file backup search/saturation at `:116-137`. | `AUTOMATED`: `TestMalformedAndCorruptInputPreservation` preserves malformed bytes in `.corrupt.bak` and leaves defaults (`SettingsPersistenceTests.cpp:130-145`); `TestLegacyAndPartialInputNormalization` checks invalid types, duplicate IDs, and invalid entries (`:103-129`). Existing primitive checks are `TestUtf16Validation`, `TestBoundedStrings`, `TestSafeTruncation`, `TestSupportedLanguages` (`SettingsLimitsTests.cpp:16-51`). `MANUAL P07-Settings-Oversized-Rejection-Bounded`: externally bounded sparse >4 MiB fixture, rejection/logging-stall timeout, and corrupt-backup observation; `MANUAL P07-Settings-Corrupt-Backup-Saturation`: precreate `.corrupt.bak` through `.corrupt.99.bak`, load malformed input, and record the legacy `.99` overwrite behavior. | `GAP`/known defect: oversized rejection is not automated because legacy std-exception logging can stall before backup; no production fix is approved in Phase 0. Parser coverage still lacks invalid UTF-16/surrogate and every array/string/DPI bound; add those cases and explicit saturation assertions only after an owner is approved. |
| P07-03 all mutations, validation, bounds, privacy/alias/default/last-device persistence | Mutations are routed through `SettingsController` and direct lock callers; data fields are in `SettingsData.hpp`; validation is `IsPersistable` at `Settings.cpp:17+`. | `AUTOMATED`: `TestMissingCurrentAndRoundTrip` mutates and round-trips privacy, alias, default, last-connected, per-device policies, placement, language, update fields, and global flags (`SettingsPersistenceTests.cpp:82-102`). `TestValidationFailureLeavesRevisionDirty` rejects an invalid language, preserves dirty state/no files, then saves the corrected revision (`:146-165`). Existing revision/presentation tests remain `TestSettingsRevisionTracksOnlyCommittedMutations`, `TestPrivacyAndMissingSettings`, and `TestPrivacyAndSnapshotGeneration`. The final x64 Debug/Release CoreTests results include the registered persistence suite. | Real direct persistence coverage now exists. `GAP`: no SettingsController/UI/CLI end-to-end round-trip, no exhaustive invalid bounds matrix, and no restart/package migration scenario. |
| P07-04 debounce, one writer, dirty revision, change during write, and no lost update | `DeferredSaveCoordinator` state machine at `include/app/DeferredSaveCoordinator.hpp:9-140`; `DeferredSettingsSaver` scheduling/retry at `src/app/DeferredSettingsSaver.cpp:17-180`. | `AUTOMATED`: `TestDeferredSaveCoalescesDirtyGenerations`, `TestDeferredSaveRetainsMutationsDuringAnAttempt`, `TestDeferredSavePersistsMutationMadeAfterSnapshot`, `TestDeferredSaveRequestCompletionRaceHasNoLostWakeup`, `TestDeferredSaveConcurrentRequestsStartExactlyOneWorker`, `TestDeferredSaveExternalFlushAcknowledgesOnlyItsGeneration`, `TestSettingsRevisionTracksOnlyCommittedMutations` (`AppWorkCoordinatorTests.cpp:26-209,210-298,441-515`); direct-file `TestFinalSaveCapturesLaterMutation` verifies a later revision remains dirty and is present after a second synchronous save (`SettingsPersistenceTests.cpp:202-225`). The final x64 Debug/Release CoreTests runs include these tests. | Coordinator coverage is strong and deterministic, and sequential final-save persistence is now characterized. `GAP`: no test changes data during an in-progress real `Settings::Save` or connects the deferred coordinator to file bytes; add a writer-boundary fixture and assert newest revision wins. |
| P07-05 write failure, bounded retry, scheduler fallback, temp cleanup, and atomic replace | `DeferredSettingsSaver::RunAttempt` retries 1 s exponential backoff capped at 5 min and falls back to synchronous flush at `DeferredSettingsSaver.cpp:121-178`; `Settings::Save` temp/flush/replace at `Settings.cpp:312+,411,415`. | `AUTOMATED`: `TestValidationFailureLeavesRevisionDirty` covers rejected data/no files (`SettingsPersistenceTests.cpp:146-165`); `TestFailedReplacePreservesExistingFileAndCanRetry` locks the destination, asserts failure/old-file preservation/temp cleanup, unlocks, retries, and reloads the new value (`:166-201`). Existing coordinator checks are `TestDeferredSaveRetriesFailuresWithoutLosingDirtyState` and `TestDeferredSaveCanRecoverFromSchedulerFailure` (`AppWorkCoordinatorTests.cpp:90-156`). The final x64 Debug/Release CoreTests runs include these tests. `MANUAL P07-Settings-Atomic-Write-Failure`: deny destination/temp access, interrupt write, fill/lock destination, inspect cleanup and old-file preservation; verify `FlushFileBuffers` precedes replace and retries are bounded. | Direct validation/replacement failure and retry coverage now exists. `GAP`: no injected `FlushFileBuffers` ordering/failure check, filesystem fault matrix, or host-level bounded-retry observation. |
| P07-06 synchronous suspend/shutdown final flush and cancellation | Host calls `m_settingsSaver.FlushNow()` on suspend and `FlushNow(3)` during teardown at `ApplicationHost.cpp:985-1050,373-380`; saver cancellation waits timer callbacks at `DeferredSettingsSaver.cpp:72-96`. | `AUTOMATED`: `TestDeferredSaveExternalFlushAcknowledgesOnlyItsGeneration`, `TestDeferredSaveCancellationInvalidatesOutstandingWork` (`AppWorkCoordinatorTests.cpp:115-137,441-458`); added `TestFinalSaveCapturesLaterMutation` covers a direct synchronous final-save sequence (`SettingsPersistenceTests.cpp:202-225`). `MANUAL P07-Settings-Suspend-Shutdown-Flush`: mutate immediately before suspend and exit while a delayed save is pending; restart and verify latest data is present or failure is visible. | Direct final-save behavior is characterized, but host lifecycle is not. `GAP`: no suspend/shutdown/Settings::Save file integration test, cancellation while real I/O is active, or failure visibility check. |

## P08 — privacy, localization, accessibility, theme, DPI, and UI-thread facts

P08 is not a separate migration slice, but its invariants constrain every matrix row.
Privacy-aware immutable presentation is exercised by `DevicePickerSnapshot` and
`TrayTooltipBuilder`; `DiagnosticsLogCollector` redacts exported diagnostic lines.
Main WinUI user-visible strings use `StringResources`, but the control CLI currently
hard-codes English help, parser, and transport/error text in
`AudioPlaybackConnector2.Control/src/main.cpp:122-146,264-499,718-741`. This is a
current P08 localization violation. Consolidated Phase 4B Control should route
CLI help/parser/transport errors through the approved localized catalog
boundary, with consolidated Phase 6B catalog-parity work completing the shared
resource inventory; no behavior change is approved by this characterization.

Local logging also violates the P08 privacy expectation: raw device IDs/names are
written through `DebugTrace` in `TrayController.cpp:646-660`,
`DeviceManager.cpp:344-437,567-660,1073,1362-1490,1908-2113`, and
`ApplicationHost.cpp:940-980,2214-2367`, even though exported log collection redacts
lines. Consolidated Phase 6C under G01 owns the later privacy-aware logging remediation and must
retain the required bounded diagnostic tail; this assignment does not infer G01
approval or authorize a Phase 0 logging change.

| ID / required behavior | Existing automated evidence or named scenario | Status and gap |
|---|---|---|
| P08-01 privacy/redaction in tooltip, picker, diagnostics, CLI, notifications, and local logs | `AUTOMATED`: `TestPrivacyAndMissingSettings` (`TrayTooltipBuilderTests.cpp:34-44`); `TestPrivacyAndSnapshotGeneration` (`DevicePickerSnapshotTests.cpp:108-143`); `TestSensitiveValuesAreRedactedAndControlsNeutralized`, `TestCustomTemporaryDirectoryIsRedactedInsideLogLines` (`DiagnosticsLogCollectorTests.cpp:164-210`) cover exported diagnostics. Raw local IDs/names remain in `DebugTrace` call sites listed above. | `MANUAL P08-Privacy-All-Presentations`: enable privacy mode and inspect tray, picker, status/list/default/alias CLI, diagnostics export, notifications, local log, and error paths; `GAP`: consolidated Phase 6C/G01 must provide privacy-aware local-log policy and production CLI/notification redaction tests. No G01 approval is inferred. |
| P08-02 all eight language catalogs and localized actions/errors | Language IDs are bounded by `SettingsLimits.hpp:48-51`; main WinUI lookup is `StringResources`, but `Control/src/main.cpp:122-146,264-499,718-741` hard-codes English help/parser/transport errors. | `AUTOMATED`: `TestSupportedLanguages` only validates accepted identifiers. `MANUAL P08-Localization-Catalog-Matrix`: switch `system`, en, de, fr, es, ja, ko, zh_hans, zh_hant; exercise every tray, picker, settings, command, notification, and error action. `GAP`: current CLI literals violate P08; consolidated Phase 4B Control and Phase 6B catalog owners must route them through the localized catalog and add key-parity/UI coverage. |
| P08-03 accessibility, keyboard behavior, XAML UI thread, theme, and DPI | XAML and dispatcher marshaling are in `TrayController.cpp:273-414,537-618,744-827`; theme/reregister is `TrayController.cpp:490-526` and `ApplicationHost.cpp:2463-2466`. | `MANUAL P08-UI-Accessibility-Theme-Dpi`: keyboard navigation, screen reader names, focus, light/dark, 100/125/150/200% DPI, and non-UI-thread callbacks. `GAP`: no automated XAML/UI-thread test in CoreTests. |

## P09 — tray interactions, actions, theme/DPI, and placement

`TrayController::HandleTrayMessage` recognizes left select/up, double-click, context
menu, and suppresses/debounces mouse events at `src/services/TrayController.cpp:537-618`:
left click opens/toggles the picker; right/context click opens the menu; double-click
hides an open picker and invokes the toggle callback; left/right down/move and balloon
messages are ignored. Click debounce is 200 ms and double-click suppression is 450 ms;
light-dismiss reopen suppression is 2 s (`:569-600, CreatePickerFlyout at :839-929`).
The menu actions are Settings, Bluetooth settings, and Exit
(`TrayController.cpp:40-88,233-271`); the picker exposes connect/disconnect/reconnect
and bulk actions (`ui/DevicePickerView/DevicePickerView.xaml.cpp:93-105,775-795`).

| ID / required behavior | Existing automated evidence or named scenario | Status and required future automation |
|---|---|---|
| P09-01 left click/select opens picker; repeated click toggles; light-dismiss suppression | `MANUAL P09-Tray-Left-Click-Picker-State-Machine`: left select/up, click while opening/open/closing, light dismiss over icon, and click again within/outside 2 s; assert one flyout generation per open and correct resource facts. | `GAP`: no TrayController/XAML interaction test; add a fake flyout state-machine test. |
| P09-02 right/context click opens menu and Settings/Bluetooth/Exit actions | `MANUAL P09-Tray-Context-Menu-Actions`: context/right click, each menu item, menu close, and failure to show; assert callback exactly once and no action after teardown. | `GAP`: no automated shell/menu callback test. |
| P09-03 double click toggles last/default device and suppresses the preceding single-click action | `ApplicationHost::ToggleLastConnectedDeviceFromTray` at `src/app/ApplicationHost.cpp:936-964`; `TrayController.cpp:580-586` records double-click and calls toggle. `MANUAL P09-Tray-Double-Click-Toggle`: connected/disconnected/default-missing/busy cases, two devices, and rapid click sequences. | `GAP`: no message-routing or toggle integration test; add deterministic tick/clock tests. |
| P09-04 picker connect/disconnect/reconnect/bulk actions and discovery refresh | Picker callbacks at `DevicePickerView.xaml.cpp:775-795`; controller callback wiring at `TrayController.cpp:611-700`; snapshots at `DevicePickerSnapshot`. | `AUTOMATED`: `TestConsistentPresentationSnapshot`, `TestInventoryFreshnessAndInvalidation`, `TestLateInventoryCallbackAfterRelease` (`DevicePickerSnapshotTests.cpp:27-107`). `MANUAL P09-Picker-Action-Refresh`: discovery add/remove/change while open, each row action, bulk action, and stale callback. `GAP`: automated tests cover snapshots, not XAML intents/DeviceManager effects. |
| P09-05 tooltip ordering, aliases, privacy, and fallback names | Tooltip builder at `include/core/TrayTooltipBuilder.hpp` and `src/core/TrayTooltipBuilder.cpp`. | `AUTOMATED`: `TestEmptyTooltip`, `TestNamePrecedenceAndFallbacks`, `TestPrivacyAndMissingSettings`, `TestConnectionOrderAndDuplicates` (`TrayTooltipBuilderTests.cpp:16-54`). | Covered for pure presentation; `GAP`: no shell tooltip update/reregister test. |
| P09-06 Idle/Connecting/Connected/Error states, frame timing, theme, DPI, and Explorer/taskbar recreation | `TrayIcon` state/frame/rendering at `include/ui/TrayIcon.hpp:9-70` and `src/ui/TrayIcon.cpp:194-279`; host selects state/timer at `ApplicationHost.cpp:1248-1300`; taskbar-created handling at `ApplicationHost.cpp:2463-2466`. | `MANUAL P09-Tray-States-Theme-Dpi`: all states and eight Connecting frames in light/dark at 100/125/150/200% DPI, then Explorer restart/taskbar recreation; assert state/tooltip/icon re-register and no animation leak. | `GAP`: no icon rendering or shell integration test; P03 timer ownership remains in host until its migration. |
| P09-07 settings-window placement and multi-monitor DPI | Placement calculation is `TrayController.cpp:945-963` and `ui/WindowPlacement`; persisted bounds schema is `Settings.cpp:369-383`. | `MANUAL P09-Placement-Dpi-MultiMonitor`: first launch, saved bounds at each DPI, monitor removal, off-screen bounds, and multi-monitor tray anchor. | `GAP`: no placement test; G06 remains a gate and current behavior must be preserved. |
| P09-08 teardown and UI-thread-only XAML access | `TrayController::Teardown` marshals to the dispatcher and releases flyout/view/icon at `TrayController.cpp:111-200`; release guards UI thread at `:744-827`. | `MANUAL P09-Tray-Teardown-Thread`: teardown from UI/background thread during opening/closing/preload and callback after teardown. | `GAP`: no deterministic XAML lifetime test; add a dispatcher/flyout fake. |

## Locked-decision checklist (P01–P10)

| Decision | Characterized invariant | Current Phase 0 evidence | Open gap / gate |
|---|---|---|---|
| P01 | CLI/named pipes, schema, security, framing, concurrency, deadlines, cancellation, correlation, replay, startup, shutdown | Protocol/server/client unit/integration tests listed in P01 | CLI/host/packaging integration matrix remains. |
| P02 | Adaptive Cold/Warm/Hot, delays, pressure, energy, fullscreen, UI facts, freshness, sequence, rollback, fallback, retry, heartbeat, diagnostics | Policy/reducer/monitor tests listed in P02 | Host/Tray integration and explicit pin fact remain. |
| P03 | Connecting animation is visible progress, theme/DPI correct, bounded timer, all terminal cleanup | Source trace at host/TrayIcon; named manual scenarios | No deterministic animation/timer test; timer is still host-owned. |
| P04 | Normal disconnect must never toast or balloon | Current event call site identified | Current call violates P04; policy test and fix required in its owning phase. |
| P05 | Every retained notification kind has an image/action mapping | Six-kind source mapping table | No mapping test; AppStarted/Update share image; manual matrix required. |
| P06 | Bluetooth ordering, serialized mutation, epochs, close barrier, token cleanup, incoming, retry, bulk, startup, suspend/resume, shutdown | Coordinator/reconnect/planner/lifecycle tests | No WinRT DeviceManager integration/race harness. |
| P07 | JSON compatibility, validation, corrupt preservation, no lost update, temp/flush/atomic replace, retry, final flush | Six retained `SettingsPersistenceTests` cover current/legacy round-trip, malformed/corrupt preservation, validation/retry, replacement failure, and later final-save behavior; the final x64 Debug/Release suites passed; existing limits/deferred-save tests and source trace | Oversized rejection/logging-stall and corrupt-backup saturation are named manual defects/scenarios; no concurrent real-write, flush-order instrumentation, suspend/shutdown, package-upgrade, or full parser-bound matrix. |
| P08 | Privacy/localization/accessibility/theme/DPI/UI thread | Pure tooltip/picker/export-redaction tests; main WinUI uses StringResources | CLI help/parser/transport errors are hard-coded English and local DebugTrace contains raw device IDs/names; consolidated Phase 4B/6B catalog and Phase 6C/G01 privacy remediation remain required. |
| P09 | Tray clicks/actions/theme/DPI/placement and retained notification actions | Pure tooltip/snapshot tests; source trace | No shell/XAML interaction test. |
| P10 | C++23 remains current until dedicated C++26 gate; x64/ARM64 evidence required | Four vcxproj `stdcpp23` declarations; execution-ref evidence records Debug/Release outputs for x64/ARM64, x64 CoreTests passes, clang-format, and explicit cppcheck passes | ARM64 native CoreTests, manual desktop/security/package-verification checks, and the dedicated C++26 gate remain unrun or unattempted; C++26 not approved. |

## Required future automation before deleting legacy owners

These are the cheapest deterministic seams indicated by the gaps above; they are
characterization requirements, not permission to alter behavior:

1. Add a CLI parser/output fixture that invokes every command/selector/flag and asserts
   text, JSON, raw/privacy, and exit behavior independently of WinRT.
2. Add a control integration fixture with a fake clock, scheduler, handler, and server
   identity to exercise late mutation, replay/correlation, startup retry, and shutdown.
3. Add a resource manager fixture with fake pressure snapshots, a sequence clock, a
   scheduler, and a Tray resource fact sink. Include `UiPinned=true`, stale and
   out-of-order snapshots, rollback, fallback scheduling, action failure, retry, and
   heartbeat diagnostics.
4. Add a normalized notification mapping/policy test for all six kinds, P04 normal
   disconnect negative behavior, P08 redaction/localization, action arguments, and
   teardown. Resolve G07 only by explicit approval.
5. Add a fake AudioPlaybackConnection/DeviceDiscoveryService harness covering every
   P06 row, including callback epochs, token revocation, close-barrier timeout,
   incoming connections, multi-device cascade, suspend/resume, and busy shutdown.
6. Extend `SettingsPersistenceTests` with parser-boundary UTF-16/count/string/DPI cases,
   explicit `.corrupt.99.bak` saturation handling, a writer-boundary mutation fixture, injected
   `FlushFileBuffers`/atomic-failure ordering, and host suspend/shutdown final-flush
   coverage; retain the current direct-file cases as the baseline.
7. Add pure TrayController/TrayIcon message and timer tests with an injected tick,
   dispatcher, shell, DPI, and flyout seam; keep an explicit manual matrix for actual
   Explorer/taskbar, accessibility, theme, and multi-monitor behavior.
8. Complete ARM64 native test and package-verification evidence (the final x64
   Debug/Release suites and both architecture build/package outputs are recorded
   above), then run the P10 gate before any C++26 language-mode change.

## Locally run execution-ref evidence (root-provided)

Root reports the following locally run evidence for product/test execution ref
`fb653f1c0a1abf26c2abb2f0784daf81fde4f34e`. A subsequent docs-only commit contains
this evidence artifact with equivalent product/test inputs and is not an execution ref:

| Check | Reported result |
|---|---|
| `msbuild AudioPlaybackConnector2.slnx -t:restore -p:RestorePackagesConfig=true /v:minimal /m:1` | Exit 0. |
| `msbuild AudioPlaybackConnector2.slnx /p:Configuration=Debug /p:Platform=x64 /v:minimal /m:1` | Exit 0; CoreRuntime boundary verification passed; the solution configuration produced x64 and ARM64 Debug app/package/control/core-tests artifacts. |
| x64 Debug CoreTests | Exit 0; output `All core tests passed`. This suite includes the registered `SettingsPersistenceTests`. |
| x64 Release rebuild with `AppxBundle=Never`, `AppxBundlePlatforms=x64`, and signing disabled | Exit 0; app, MSIX, and CoreTests built. |
| x64 Release CoreTests | Exit 0; output `All core tests passed`. |
| ARM64 Release rebuild with `AppxBundle=Never`, `AppxBundlePlatforms=ARM64`, and signing disabled | Exit 0; app, MSIX, and cross-compiled CoreTests built. |
| ARM64 CoreTests execution | Not executed on the x64 host. |
| clang-format 19.1.3 over all product `.cpp`/`.hpp` files with the required dry-run/error check | Exit 0. |
| `C:\Program Files\Cppcheck\cppcheck.exe` 2.20.0 with warning/performance/portability, `--std=c++20`, and `--platform=win64` over product `src` | Exit 0. The unrelated PATH Strawberry cppcheck 2.14 executable is broken; it is not the recorded check. |
| `git diff --check c2dfd87..fb653f1` and execution-ref repository status | Root reported clean. |

This evidence is supplied by root for the product/test execution ref. It establishes
the x64 Debug/Release build and CoreTests results, both architecture build/package
outputs, the CoreRuntime boundary check, and the explicit clang-format/cppcheck passes.
It does not establish ARM64 native test execution, manual desktop scenarios, security
review, package-verification scenarios, or a performance rerun. The dedicated C++26
toolchain gate was not attempted.

## Validation record for this artifact

Read-only inspection and evidence collection used `rg`, `Get-Content`, project-file
inspection, and Git history/diff commands. The following checks were run for the
documentation change:

```text
git diff --check
```

The root-provided evidence above includes the retained six-test persistence inventory
in the passing x64 Debug and Release CoreTests runs. This docs-only review update did
not create a new product/test execution ref; its validation is the diff check recorded
above and the status check after this documentation commit. Manual desktop scenarios,
ARM64 native tests, security review, package-verification scenarios, and performance
rerun remain unprovided/unrun. The dedicated C++26 toolchain gate was not attempted.
Phase 0 still requires the applicable checks and scenarios listed in
`docs/architecture-rework/verification.md`.
