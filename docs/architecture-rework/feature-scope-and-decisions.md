# Feature Scope and Decision Register

## How to use this register

Locked decisions are part of the migration scope. Proposed decisions are recommendations that require explicit product approval before behavior is removed. Until approval, migration work preserves the current behavior behind the new boundary.

## Locked decisions

| Area | Decision | Consequence |
|---|---|---|
| CLI/named pipes | Keep as first-class functionality | Preserve protocol, security, concurrency, cancellation, deadlines, privacy/raw output, and CLI compatibility |
| Tray animation | Remove | Replace connecting animation with a static Connecting icon; remove frame timer and animation state |
| Normal disconnect notification | Remove | State, tooltip, CLI, and logs still update; no toast is emitted for an ordinary disconnect |
| Toast artwork | Keep | Dedicated images remain for the retained notification kinds |
| Adaptive resources | Keep full behavior | Repackage into one manager; do not replace with simple idle release |
| Bluetooth correctness | Keep | Do not trade stale-result, close-barrier, reconnect, incoming, suspend/resume, or operation-order guarantees for line count |

## Current cost profile

These are approximate directly attributable production lines from the planning baseline. Cross-cutting code in `ApplicationHost` is not perfectly assignable.

| Feature | Approximate lines | Structural action |
|---|---:|---|
| Bluetooth/device core | 3,934 | Rebuild around service + per-device session; preserve behavior |
| Tray/device picker | 3,244 | Snapshot-driven UI; remove animation; retain resource hooks |
| CLI/control | 3,150 | Keep feature; isolate transport from command execution |
| Settings UI | 3,055 | Move non-UI work to `SettingsSession`; simplify code-behind |
| Settings persistence/controller | 1,503 | Consolidate into one `SettingsStore` and one writer state machine |
| Adaptive resources | 1,069 plus host integration | Preserve policy; contain coordination in one manager |
| Updates | 1,012 | Decision gate below |
| Notifications/toasts | 808 | Normalize notification model; preserve distinct artwork |
| Crash handling | 773 | Decision gate below |
| Logging | 661 | Decision gate below |
| Startup task | 500 | Keep, internalize latest-wins state |
| Diagnostics | 490 | Decision gate below |
| Suspend/resume | 399 | Keep correctness behavior |

## Recommended decision gates

### D1: Logging profile

Recommendation: adopt a bounded in-memory ring buffer with immediate persistence for warnings/errors and explicit flush on diagnostics export or crash.

Retain:

- timestamps, severity, thread/context where useful;
- recent history needed to diagnose Bluetooth timing;
- `OutputDebugString` in development builds;
- bounded file output and privacy rules;
- crash/diagnostics export of recent entries.

Remove or avoid:

- permanent background writer thread;
- general-purpose batch queue;
- dropped-message accounting unless measured load requires it;
- retry machinery for ordinary informational lines;
- stack capture for every caught exception.

Expected target: roughly 200-300 lines. This is a recommended behavior change and must be approved before implementation replaces persistent informational logging.

### D2: Crash-reporting profile

Recommendation: keep a minimal local crash reporter.

Retain:

- unhandled-exception capture;
- `MiniDumpWriteDump`;
- timestamp, app version, exception code, and recent log tail;
- a simple next-start indication that a dump exists.

Remove or avoid:

- vectored handling unless a demonstrated crash class requires it;
- C signal handling;
- generated GitHub issue URL;
- complex crash-time UI;
- multiple overlapping normal/emergency artifact pipelines;
- extensive marker/prompt state.

Expected target: roughly 180-300 lines.

### D3: Settings persistence profile

Recommendation: preserve data safety and simplify scheduling.

Retain:

- JSON format and compatibility migration;
- validation and bounded device/string counts;
- corrupt-file preservation;
- temporary write, `FlushFileBuffers`, and atomic replace;
- no lost update when changes arrive during a write;
- synchronous flush on suspend/shutdown.

Replace the current coordination with:

- one owner of the in-memory snapshot;
- one dirty revision;
- one debounced timer;
- at most one writer;
- bounded retry;
- a final synchronous flush.

Do not replace JSON with a database or registry storage. Expected target: roughly 700-900 lines for store, validation, mutation, and persistence, excluding Settings UI.

### D4: Update profile

Recommendation: use a manual check as the minimum implementation, with an explicit choice whether automatic checks remain.

Manual profile:

- Settings button starts one check;
- show up-to-date, available, or failed result;
- open `.appinstaller` or release destination;
- no background stability window, result cache, or update notification.

Full profile preserves current automatic checks and notification behavior but places single-flight and cancellation privately in `UpdateService`.

No automatic-update behavior is removed until this decision is approved.

### D5: Diagnostics profile

Recommendation: one privacy-aware report generated from immutable snapshots.

Include:

- application/Windows version;
- device and connection snapshot;
- resource-residency diagnostics;
- important settings;
- recent log ring;
- redaction according to Privacy Mode.

Avoid separate log-collection orchestration and report-builder layers unless required by a concrete export format. Target: roughly 150-250 lines.

### D6: Window placement

Recommendation: retain simple size/position persistence only if users value it. If retained, use one load/validate/apply path and one debounced save path. Avoid making exact multi-monitor restoration a dependency of general Settings behavior.

This feature is not removed by the current locked decisions.

### D7: Notification fallback

Recommendation: keep Windows App SDK toasts and distinct images. Decide separately whether the tray-balloon fallback is required on supported platforms.

The normal-disconnect notification is removed regardless of fallback choice. Error, reconnect, update, and other retained notification kinds continue using their designated artwork.

## Features that should not be reduced for code-size reasons

- named-pipe client/server trust validation;
- privacy/redaction for CLI and diagnostics;
- Bluetooth operation epochs and stale-completion checks;
- close-before-reconnect and connection-state token cleanup;
- manual-operation cancellation of reconnect work;
- suspend/resume state preservation;
- atomic settings replacement and shutdown flush;
- resource snapshot freshness and authorization sequences;
- Cold/Warm/Hot policy and UI pinning;
- XAML access from the UI thread only;
- localization of all user-visible strings.

## Expected profiles

| Profile | Product behavior | Directional size |
|---|---|---:|
| Full compatibility plus locked changes | All current features, CLI retained, animation and normal-disconnect toast removed | 16,000-19,000 lines |
| Recommended lean profile | CLI and adaptive resources retained; minimal logging/crash/diagnostics; manual update; simpler settings | 14,500-17,000 lines |

These are planning ranges, not quotas. Correctness, readability, and clear ownership take priority over a numerical target.
