# Verification Plan

## Quality gates

Every implementation phase must pass the relevant automated tests, formatting/static analysis, and focused manual scenarios before the replaced code is deleted. Final verification covers all supported build targets and packaged behavior.

No document, pull request, or release note may claim a build, test, compatibility, or performance result that was not actually produced by the corresponding validation run.

## Build and static checks

Use the repository-supported restore and build commands for the active branch. At minimum:

```powershell
msbuild AudioPlaybackConnector2.slnx -t:restore -p:RestorePackagesConfig=true
msbuild AudioPlaybackConnector2.slnx /p:Configuration=Debug /p:Platform=x64
msbuild AudioPlaybackConnector2.slnx /p:Configuration=Release /p:Platform=x64 /t:Rebuild
clang-format --dry-run --Werror AudioPlaybackConnector2/src/**/*.cpp AudioPlaybackConnector2/include/**/*.hpp
cppcheck --enable=warning,performance,portability --std=c++20 --platform=win64 AudioPlaybackConnector2/src/
```

The final phase also builds every platform still declared supported by the solution/package and runs the repository's package-verification scripts. Update paths/globs when the new colocated source layout lands.

When the C++26 migration gate is reached, product and test projects must use the same selected language-standard mode. A local x64 compiler accepting a feature is not sufficient. The Windows SDK, C++/WinRT generation, dependencies, ARM64 compiler, tests, and packaging must pass before C++26 becomes the repository baseline.

## Automated test suites

### Device behavior

- discovery start/stop/restart and stale watcher callbacks;
- connect success/failure/cancellation;
- duplicate and overlapping connect/disconnect/reconnect;
- close-before-reconnect and close timeout/fallback;
- stale WinRT completion after cancellation or superseding command;
- unexpected loss and reconnect schedule;
- manual action cancels reconnect;
- reconnect exhaustion and later recovery;
- global and per-device reconnect combinations;
- incoming connection enable/disable and reconnect interactions;
- bulk actions;
- suspend/resume reconnect;
- shutdown with active operations.

### Settings

- current/legacy JSON load;
- missing, malformed, oversized, and partially invalid data;
- corrupt-file preservation;
- all supported settings mutations and bounds;
- change during active write;
- write failure and bounded retry;
- atomic replacement and temporary-file cleanup;
- suspend/shutdown flush;
- no lost revision;
- privacy, aliases, default/last device, and per-device policy persistence.

### Resource residency

- initial residency;
- pressure-to-Cold delay;
- immediate memory-pressure response;
- Cold-to-Warm and Warm-to-Hot transitions;
- energy saver and fullscreen/presentation constraints;
- UI visible/pinned behavior;
- user-interaction Hot hold;
- release deferral;
- preload authorization and stale authorization rejection;
- snapshot freshness/heartbeat;
- out-of-order sequence rejection;
- clock rollback;
- timer scheduling/fallback;
- preload/release failure backoff;
- shutdown cancellation;
- diagnostics snapshot accuracy.

### CLI and named pipes

- every existing command and argument combination;
- text and JSON rendering;
- raw/privacy behavior and redaction;
- success/failure exit codes;
- framing limits and malformed requests;
- trusted server/client identity checks;
- rejected untrusted peer;
- correlation IDs and duplicate/replay behavior;
- concurrent clients and pipe-instance recycling;
- operation deadline before UI execution;
- deadline after mutation began and indeterminate result handling;
- cancellation and shutdown;
- server startup retry;
- response cache bounds;
- CLI compatibility against a baseline build where practical.

### Application controller

- UI and CLI commands produce the same use case and result;
- device events update snapshots;
- settings changes apply required device side effects;
- default/last-device toggle behavior;
- notification-policy mapping;
- normal disconnect maps to no notification;
- retained notification kinds select the correct distinct image;
- UI refresh coalescing does not lose the final state.

### Startup, updates, notifications, diagnostics

- latest-wins startup requests;
- approved manual/automatic update profile;
- update cancellation and shutdown;
- toast XML escaping and action routing;
- distinct image URI per retained notification kind;
- privacy-aware diagnostics;
- logging/crash behavior according to the approved profile.

## Manual desktop matrix

Run on at least Windows 10 2004-compatible and current supported Windows 11 environments when available.

### Tray and picker

- first launch, second-instance attempt, Explorer/taskbar restart;
- left-click open/close, right-click menu, double-click toggle;
- picker open during discovery changes;
- static Idle, Connecting, Connected, and Error icons;
- verify no connecting animation or animation timer behavior remains;
- light/dark theme and 100/125/150/200% DPI;
- multi-monitor flyout placement;
- adaptive preload, release, and pinned-visible behavior.

### Bluetooth

- at least two paired A2DP devices;
- connect/disconnect/reconnect from picker, tray, and CLI;
- disconnect/reconnect all;
- rapid repeated actions;
- device powered off during connect and while connected;
- Bluetooth adapter disabled/re-enabled;
- incoming connection initiated from a phone/device;
- startup connect and unexpected-loss reconnect;
- sleep/resume and shutdown while busy.

### Notifications

- connected, error, reconnect, reconnect-failed, update, and any other retained kinds;
- correct dedicated artwork for every kind;
- action-button routing;
- localization and privacy redaction;
- ordinary disconnect produces no toast and no fallback balloon.

### Settings and support

- all pages and controls;
- aliases and device policies reflected in picker and CLI;
- language switch while UI is open;
- Privacy Mode across UI, CLI, notifications, diagnostics, and logs;
- startup task state;
- approved update flow;
- diagnostics copy/export;
- window placement if retained;
- corrupt settings recovery and persistence after restart.

### Packaging

- packaged launch and CLI installation;
- required framework deployment;
- startup task under packaged identity;
- toast activation;
- App Installer/update handoff according to approved profile;
- upgrade from the last released package with settings retained;
- uninstall/reinstall expectations documented.

## Performance and resource checks

Record before/after measurements rather than relying on line count:

- cold startup to tray registration;
- first picker open and preloaded picker open;
- steady-state private working set in Cold/Warm/Hot;
- CPU wakeups and polling intervals under normal and constrained states;
- handle/thread count at idle and after repeated picker/CLI use;
- connect-command latency from UI and CLI;
- settings-write frequency and duration;
- shutdown duration with and without active device operations;
- pipe-server memory/cache bounds under repeated requests.

The rework must not regress the adaptive-resource goals merely because ownership moved.

## Security checks

- named-pipe ACL is scoped to the intended user/session/package identities;
- client verifies the expected server process/identity;
- server rejects an unexpected client identity;
- deadlines and cancellation cannot cause a late unauthorized mutation;
- correlation/replay behavior cannot return another caller's sensitive response;
- protocol length limits prevent unbounded allocation;
- Privacy Mode redacts device identifiers/names in every external presentation;
- diagnostics and logs are bounded and local;
- update destinations remain trusted/validated according to current release policy.

## Final acceptance checklist

- [ ] All locked decisions have explicit tests.
- [ ] CLI compatibility matrix passes.
- [ ] Named-pipe security properties are unchanged or stronger.
- [ ] Adaptive-resource parity matrix passes.
- [ ] Normal disconnect never produces a notification.
- [ ] Distinct toast images remain for retained notification kinds.
- [ ] Connecting animation code and timers are removed.
- [ ] Settings writes are atomic and no update is lost.
- [ ] Suspend/resume and shutdown race scenarios pass.
- [ ] All user-visible strings use localized resources.
- [ ] No obsolete implementation or migration adapter remains.
- [ ] Architecture boundary checks pass.
- [ ] Supported Debug/Release/platform/package builds pass.
- [ ] Final architecture and troubleshooting documentation are updated.
