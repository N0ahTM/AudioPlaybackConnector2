# Migration

## Rules

- Replace incrementally; one new owner per phase.
- Every PR slice MUST compile and preserve unapproved behavior.
- Preferred slice: characterization/tests → implementation behind adapter → callers → legacy deletion → docs/boundary check.
- Temporary adapters/flags require owner and deletion criterion.
- A phase ends only after its legacy owner and duplicate pipeline are removed.
- Avoid unrelated formatting and project renames.
- Data changes require compatible read and rollback; protocol changes require explicit versioning.

## Consolidation

The previous eleven-phase plan is organizationally consolidated below. No
requirement, invariant, exit gate, test matrix, manual scenario, security
requirement, decision gate, or final acceptance condition is removed. The
dependency order inside each combined phase is preserved.

| Previous phase(s) | Consolidated phase | Preserved order |
|---|---|---|
| 0 | 0 Baseline | Characterization and baseline evidence first. |
| 1 | 1 Models and API | Models and normalized contracts precede owner migration. |
| 2 | 2 Settings | SettingsStore and P07 ownership remain before device/application cutover. |
| 3 | 3 Devices | Device watcher, sessions, policy, and lifecycle remain together. |
| 4, 7 | 4 Application and Control | 4A Application, then 4B Control. |
| 5, 6 | 5 Resources and Tray | 5A Resources, then 5B Tray. |
| 8, 9, 10 | 6 UI and Supporting Services | 6A Notifications, 6B Settings UI/services, then 6C Logging/crash. |
| 11 | 7 Cleanup and Final Verification | Deletion, alignment, complete verification, and final acceptance last. |

The decision gates remain attached to their owning consolidated phase: G03's
single snapshot/writer contract remains in Phase 2; G04-G06 remain current and
unmodified until their explicit Phase 6B decisions; G07 remains unresolved
until the Phase 6A fallback decision; and G01/G02 may change only through the
explicitly approved profiles in Phase 6C. No Gxx approval is inferred by this
organizational consolidation.

## Consolidated phases

### Phase 0: Baseline

Record the metrics and [performance baseline](performance-baseline-2026-08-22.md).
Inventory P01 commands/schema/security, P02 transitions/timing, P05 images,
P06 races, P07 formats/failures, and P09 behavior. Add missing
characterization tests and keep named manual scenarios for behavior that cannot
be automated. Gate P10 separately; C++26 is not the baseline until its
dedicated toolchain gate passes.

Exit gate: compatibility and race matrices exist; high-risk behavior has
automated or named manual coverage; supported Debug/Release builds pass; and
the language baseline is reported accurately.

### Phase 1: Models and API

Add `DeviceId`, immutable snapshots and facts, normalized results, typed
`AppController` contracts, and the adapters needed to route UI and CLI through
the common API. Preserve all P01-P10 behavior while the old owners remain
behind temporary boundaries.

Exit gate: UI and CLI share one API, and models contain no mutable service,
XAML, or pipe types.

### Phase 2: Settings

Build `SettingsStore`, migrate every caller, and implement the complete P07
contract with one writer state machine. Preserve JSON current/legacy
compatibility, validation and bounds, corrupt-file preservation, no-lost-update
revision ordering, temporary write, `FlushFileBuffers`, atomic replacement,
bounded retry, and synchronous suspend/shutdown flush.

Exit gate: no external settings locks remain; legacy format, failure,
change-during-write, suspend, and shutdown tests pass; the old settings
coordination and duplicate persistence owners are deleted.

### Phase 3: Devices

Implement `DeviceService`, `DeviceWatcher`, one `DeviceSession` per device,
pure `ReconnectPolicy`, normalized device snapshots, normalized results, and
typed facts. Preserve all P06 behavior: serialized mutations; watcher
lifecycle and stale-watcher rejection; per-device operation epochs;
close-before-reconnect barriers; connection-token cleanup; incoming
connections; manual reconnect cancellation; bounded retry and backoff; bulk
operations; startup connection; suspend/resume; shutdown while busy; and the
rule that no stale completion, callback, or timer can resurrect a session.

Exit gate: one session-map owner; callbacks mutate state only on the serialized
device context; the old `DeviceManager`/coordinator duplicate pipeline is
gone; all applicable P06 race and lifecycle tests and required builds pass;
and a fresh final reviewer returns `APPROVED`.

### Phase 4: Application and Control

#### 4A Application

Make `AppController` the concrete use-case owner for connect, disconnect,
reconnect, toggle, bulk commands, default/last-device behavior, settings side
effects, startup and update requests, notification policy selection, immutable
App/Tray snapshots, and normalized presentation mapping.

UI and CLI use the same `AppController` commands. `AppRuntime` owns only
composition, platform lifetime, message entry, suspend/resume entry, and
shutdown order. Replace the multi-event router with one typed controller
subscription and delete `LegacyAppUseCaseBridge` at the end of this phase.
`AppController` is a concrete use-case owner, not a callback wrapper or
generic operation table, and it must not depend on XAML, pipe frames, JSON
persistence, `AudioPlaybackConnection`, pressure probes, or toast XML.

#### 4B Control

Implement and isolate `ControlServer`, `ControlCommandHandler`, `ControlClient`,
the shared protocol, and pipe-security boundaries. Preserve every P01 command,
argument, selector, exit behavior, text/JSON/raw/privacy output, JSON schema,
ACL and identity check, framing and response bound, concurrency and mutation
serialization, deadlines and cancellation, indeterminate outcomes,
correlation IDs, replay/deduplication, cache bounds, startup retry, and
graceful shutdown. Pipe transport must not enter `AppController`; use cases
must not be duplicated in the control layer. Timeout or cancellation must not
permit an unreported late mutation. Delete old control adapters and duplicate
command switches assigned to this phase.

Exit gate: `AppRuntime` has no command switch, Bluetooth, or policy ownership;
`AppController` has no UI, pipe, persistence, pressure, or toast-XML
dependency; the old router/bridge and duplicate CLI use cases are deleted;
P04 behavior and the complete positive/negative P01 protocol/security matrix
pass; required builds pass; and a fresh final reviewer returns `APPROVED`.

### Phase 5: Resources and Tray

#### 5A Resources

Implement `ResourceResidencyManager`, `ResourcePressureMonitor`, and pure
`ResidencyPolicy`. Preserve complete P02 behavior: Cold/Warm/Hot states;
configured pressure, recovery, and preload delays; memory pressure; energy
saver; fullscreen/presentation awareness; UI visibility and pinning;
interaction hold; release deferral; freshness, sequence, and positive
authorization; clock rollback; scheduler fallback; bounded failure retry;
heartbeat and diagnostics; and safe shutdown. Do not replace adaptive
resources with simple lazy loading or idle release.

#### 5B Tray

Implement snapshot-driven `TrayUi` and picker rows. Preserve P03 Connecting
animation, one bounded Tray-owned animation timer, prompt stop on success,
error, cancellation, idle, and shutdown, no orphan timer or wakeup, light/dark
themes, DPI-correct frames/icons, left/right/double-click behavior,
Explorer/taskbar recreation, picker preload/release and resource facts,
accessibility, and UI-thread-only XAML access. Tray must not own resource
policy; `DevicePicker` must not access settings or device internals directly;
the host must have no resource-pressure, timer, sequence, or retry state; and
the old resource/tray coordination pipelines must be deleted.

Exit gate: P03 starts/stops in every terminal, cancellation, and shutdown path
with one owned timer and no leak; all tray states pass theme/DPI checks; picker
and P02 UI scenarios pass; all applicable P02, P03, P08, and P09 tests and
named scenarios pass; the host and Tray have the required ownership
boundaries; required builds pass; and a fresh final reviewer returns
`APPROVED`.

### Phase 6: UI and Supporting Services

Before changing behavior governed by G01-G07, inspect `decisions.md`. If any
required Gxx decision remains unapproved, stop this phase, present the smallest
concrete decision request with current behavior, recommendation, compatibility
requirement, and consequences, wait for explicit approval, and record it in
`decisions.md`. Never infer approval from an architectural recommendation.

#### 6A Notifications

Implement normalized notification values and rendering. Preserve P04 normal
disconnect behavior (state, tooltip, CLI, and diagnostics/logging may update,
but no toast or fallback balloon), every distinct retained P05 notification
image mapping, retained actions, localization, Privacy Mode redaction, and
teardown/callback safety. Resolve G07 before changing fallback behavior.

#### 6B Settings UI and services

Implement `SettingsSession` so XAML renders prepared models and forwards typed
intents. Migrate settings UI, startup, update, diagnostics, and placement
behavior. Resolve G04, G05, and G06 before changing their current behavior.
G05 remains one privacy-aware report built from immutable snapshots; avoid
collector/builder layers without a concrete format need. UI owns no
persistence, device coordination, update transport, or report collection.
CLI-to-open-window synchronization, accessibility, keyboard, theme, DPI, and
UI-thread rules must pass. Every user-visible string uses localized resources
in every supported language, and all language keys exist.

#### 6C Logging and crash reporting

Implement only explicitly approved G01 and G02 profiles. Preserve
privacy-aware bounded diagnostics, no secret or unredacted device logging in
Privacy Mode, non-blocking/non-crashing shutdown, non-recursive crash handling,
required Bluetooth timing history, and a useful diagnostic tail. Remove old
duplicate logging/crash pipelines only after replacement validation.

Exit gate: approved service behavior is implemented; the per-kind
title/body/action/image matrix passes; normal disconnect produces no toast or
balloon; P04/P05/P08 matrices pass; no unresolved service/UI owner assigned to
this phase remains; and a fresh final reviewer returns `APPROVED`.

### Phase 7: Cleanup and Final Verification

Remove every temporary bridge, adapter, feature flag, compatibility overload,
and duplicate pipeline whose deletion criterion has been reached. Remove dead
files and project registrations, obsolete includes and dependency edges, and
completed migration-only tests that no longer test retained behavior. Retain
valuable characterization and regression tests. Align source layout and
documentation with the shipping architecture; do not rename or relocate files
for aesthetic or count targets.

Verify one owner per mutable state, no application-wide event bus or service
locator, no pass-through `AppController`, no UI dependency in Core, no pipe
transport in `AppController`, no settings persistence calling device or UI
code, no diagnostics feature locks, no obsolete owner or duplicate pipeline,
and documentation matching the exact shipping code.

C++ language gate: keep C++23 unless the dedicated P10 toolchain change passes
every required x64/ARM64 build, test, package, dependency, and CI-equivalent
check. Do not declare C++26 mandatory based only on one local compiler build.

Exit gate: the complete [verification](verification.md) matrix, package and
upgrade scenarios, dependency checks, comparable final performance evidence,
architecture-exception record, source map alignment, and documentation review
pass; a fresh final reviewer approves the complete branch against the original
architecture-rework base; and
`rework/architecture` is clean and locally PR-ready.

## Phase gate workflow

Complete phases sequentially. A later phase may not begin until the preceding
phase is implemented, its assigned legacy owners are removed, applicable
validation passes, the result is committed with a clean worktree, and a fresh
independent final reviewer returns `APPROVED`. Capture the exact phase base and
candidate commit, commands, results, applicable Pxx/Gxx decisions, exit gate,
and reviewer evidence. Fix every blocking finding, rerun affected and phase
validation, and obtain another fresh review.

For every implementation slice, compile the directly affected project/configuration,
run focused deterministic tests, run `git diff --check`, and run formatting
checks over changed source. At every phase gate run the applicable restore,
x64 Debug, x64 Release, x64 Release CoreTests, clang-format, cppcheck,
protocol/security/dependency checks, and named manual scenarios. Run ARM64
Release rebuilds when shared production code, project files, platform
contracts, or public headers are affected. Never mark an unavailable manual or
hardware scenario as passed; record the exact environment or user action and
keep it as an acceptance blocker.

## Rollback

- Intermediate PRs may retain an adapter; completed phases may not.
- Short-lived feature flags need a removal phase.
- Never use the structural rewrite alone to justify data or P01 protocol breakage.
