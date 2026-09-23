# Runtime invariants

This file records owner, concurrency, persistence and shutdown contracts. Read the affected section before changing asynchronous work or locks. [Architecture](ARCHITECTURE.md) maps components; [Contributing](../CONTRIBUTING.md) defines checks. These contracts do not certify owners that have not been audited.

## Shared rules

Each mutable state has one owner. A lock protects only its stated state; owner calls, application callbacks, logging, I/O and capture destruction run after unlocking unless an exception is named below. A condition-variable wait releases its lock. Admitted asynchronous work retains every dependency it uses, has cancellation or a generation fence, and cannot publish after shutdown. A reentrant stop never waits for its own callback; an external stop drains admitted foreign callbacks before releasing their state.

Host composes owners and controls lifecycle. Controller exposes named actions and validated immutable snapshots. DeviceService owns devices and reconnect; SettingsStore owns persistence. UI renders snapshots and calls controller without another device or settings cache. CoreTests link production CoreRuntime; Control owns CLI and pipe; WinUI stays outside headless projects.

## Device state and snapshots

DeviceService serializes sessions, policy, generations and facts through its task drainer. QueueMutex protects queue bookkeeping only. The drainer publishes value snapshots under SnapshotMutex and invokes subscribers after unlocking. Shutdown is an owner task: it stops the watcher, closes sessions, publishes a terminal snapshot/fact, resolves pending operations and retires the subscriber. Posted work checks shutdown before mutation.

DeviceWatcher inventory changes only on its serialized executor. Native callbacks and awaited refreshes post back through stop and generation gates. RefreshDevicesAsync retains shared service State across suspension; the watcher remains alive until that State is destroyed. After discovery, reread sessions before merging inventory and saved labels. Stale results cannot publish.

A controller capture copies settings, complete device state and optional startup state, then validates their versions over an overlapping stable interval. Exhausted retries return unavailable, never mixed usable data. Status, list, alias, default, tray and picker projections use that capture; resource diagnostics are sampled separately. Delayed presentation facts cannot overwrite newer sessions, recreate removed sessions or leave terminal sessions busy. Unavailable UI captures retain the display and retry. Command admission may recheck busy state independently.

Connect and reconnect return admission plus an epoch completion resolved only by the device drainer. The first terminal result remains final. A predicate wait uses a stop token and steady deadline; terminal publication wins over later cancellation or timeout. Do not wait on the publishing drainer. Only the accepted command caller may cancel an epoch; coalesced callers may stop waiting without cancelling it. Cancellation posts an epoch-fenced mutation. Notify completion before foreign fact subscribers and outside queue/snapshot locks. Shutdown resolves pending completions. Native work drained by an idle caller can exceed the wait deadline.

## Controller actions and events

Commands and captures lease concrete SettingsStore and DeviceService owners. RequestStop closes command/event admission, invalidates queued recipients and cancels queued policy without waiting. Admitted work may finish with its committed result. Shutdown drains leases, device facts and foreign callbacks outside locks; Host cancels transport first. An admitted callback may call RequestStop, but cannot join its own lifecycle.

EventState::Mutex protects revisions, recipient admission, the FIFO queue and one drainer identity. Publication captures recipients in registration order. Handlers and capture destruction run unlocked; concurrent or reentrant publication cannot overlap or overtake delivery. Reset disables admission before draining foreign delivery and never waits for its own drainer. Exceptions do not prevent later recipients.

SnapshotAndSubscribe captures an event watermark, reads owners unlocked, then validates the revision and registers atomically. Intervening events retry; stop, bad handlers or exhausted retries return unavailable. Callbacks can arrive before return, so consumers serialize initial/event application by revision. SettingsChangedEvent includes the store revision. Owner versions are validation tokens, not copies; presentation generations advance for new device versions and picker acknowledgement. Overtaken readers retry.

Device facts normalize on the device context without UI or localization. DeviceFactPublicationFence invalidates superseded UI presentation, not device state. A real connected transition persists native name, initial preferences and history before DeviceConnectedEvent; duplicate facts do not rewrite and empty names preserve saved names. Versioned policy reaches DeviceService without waiting for observers. Facts admitted before stop may finish persistence; later facts cannot begin it. Host marshals once to UI and checks the presentation fence before rendering.

RestoreStartupConnections is an admitted controller action using one validated settings capture. Each eligible saved device is submitted once, in recent-history order followed by saved order. Global startup enables all saved devices; otherwise per-device policy applies. Unknown history is ignored. Submitted means handoff, not connection success.

## Settings and persistence

Settings UI uses controller actions and snapshot/events. Host applies language and backdrop on UI, commits final window placement, then closes admission; commands drain before the store. ApplySettingsPolicy is the sole device reconnect-configuration entry. It sends a complete versioned policy per settings change. DeviceService stages it on its context, ignores older/duplicate revisions and avoids republishing semantic no-ops. Queued submissions check the controller stop token.

SettingsStore owns immutable SettingsData, current revision and persisted revision. Stage allocation and publication before a no-throw commit. Public snapshots copy a captured immutable reference outside Impl::mutex. Writers capture reference and revision under that mutex, then serialize and write unlocked. Older success cannot mark a newer revision persisted; failure retries the then-current revision.

SchemaVersion 2 is emitted and validated strictly. The unversioned 0.9.1 document is the sole migration input: accept only known legacy fields, translate reconnect fallbacks, validate the normalized document with the schema-2 decoder and schedule an atomic rewrite. A failed rewrite leaves the original file for retry. Unknown/duplicate keys or identities, invalid types and limits reject the input. JSON DOM remains private. Only file/path-not-found means absent. Empty or unreadable files fail preservation. A failed preservation closes mutation admission before load releases it; mutations reject and flush fails for that instance. Mutations wait for load before deciding. Successful preservation permits defaults.

The persistence worker captures a wake version, inspects state, unlocks and waits for a signal or eligible debounce/retry. Versioned Wait retains notifications arriving before entry. SettingsStoreWakeup is called outside store/publication locks. Shutdown deadlines use real steady time even when test persistence time is frozen.

Lock order is store → publication → subscription; callbacks and capture destruction run unlocked. PublicationMutex protects queue/drainer admission; SubscriptionState::Mutex protects handler admission and active delivery. Subscribe stages callback state and unsubscribe handle before registration; allocation failure destroys external captures after the store lock unwinds. Subscription reset releases its own lock before erasing under the store lock, disables admission and drains foreign execution, never its own stack. Shutdown retires registrations before destroying captures.

The first Shutdown caller chooses flush/discard and attempt count. Every caller has its own steady deadline across worker, I/O and publication drain; attempts bound writes, not elapsed I/O. Success means persisted data and foreign callbacks drained. Timeout closes admission but admitted synchronous I/O may finish on disk; it cannot change public state or start another write. Facade, load, flush, publication and worker retain Impl independently. A timed-out worker retains state until exit without an implicit destructor join. Executor initialization failure fails construction. FlushNow remains synchronous, and callers hold no application locks.

## Control pipe and response retention

ControlCommandAdapter uses one atomic mutation gate through response formatting; concurrent mutations return Busy while reads remain concurrent. Controller and localization callbacks run without an adapter lock. Each handled command initializes its own apartment on its threadpool thread.

Pipe lifecycle admission and stop state use m_lifecycleMutex. TryStart reserves starting, then prepares security and handles unlocked. Stop closes admission, waits with the mutex released for preparation, requests stop outside the mutex, drains handlers and instances, and destroys retired handler captures unlocked. Same-thread reentrant Stop/Start does not self-wait. WIL owns pipe instances, I/O, work and timers; nowait handles only close, so Stop, recreation and destruction explicitly cancel and drain. Recreation detaches old owners under the slot lock and drains them unlocked.

Platform exceptions: Options::ConnectPipe and DisconnectNamedPipe run under a pipe slot's StateMutex to keep native connection and OVERLAPPED reservation together. Initial connection arms also hold m_lifecycleMutex while startup iterates instances. ConnectPipe must return promptly, never throw or reenter, support concurrent slots and preserve ConnectNamedPipe errors. The server never flushes while holding the slot lock; Stop cancels and drains I/O before disconnect. A slot deadline timer is cancelled and drained under StateMutex before OVERLAPPED reuse; its callback only calls CancelIoEx and takes no slot lock. Stop disarms timers under slot locks, then drains them unlocked.

m_requestMutex owns correlation records, pending/active deliveries and cache bytes. Register pending delivery before dispatch; acquire a completed response and active delivery in one commit. The handler transfers active ownership to the instance before releasing pending registration. Failure releases directly; ACK or disconnect ends normal delivery. Active delivery prevents TTL and pressure eviction even if another client ACKs. A request has one canonical delivery owner; no second ownership lookup or conflicting claimant is allowed. Slot cleanup may take the request mutex, so request admission releases it before any slot cleanup.

Sample monotonic CacheNow outside request and slot locks. SetCacheTimer is the named exception under m_requestMutex: it must not wait, throw or invoke callbacks inline. The server owns and drains the native prune timer. An in-process handler that reenters Stop defers it; destruction on its own active handler terminates rather than double-draining. CLI endpoint discovery owns a waitable timer per attempt; connection and overall deadlines bound waits, and replay observes the retained server process handle.

## Power recovery and startup task

Power recovery retains shared state rather than the facade. ResumeState::Mutex protects ordered targets, attempt counters and generation; reconnect callbacks, logging and capture destruction run unlocked. Schedule failure delivers once immediately. Otherwise a ten-second tick admits one delivery at a time, up to six actual attempts per target. Skips do not spend attempts; duplicate IDs/completions do not double-count. Suspend retains targets and resets budgets. Cancel fences later mutation and disarms native work; admitted delivery may finish. Host checks generation on UI before device work. Native callbacks initialize an apartment and retain context before disassociation.

StartupTaskCoordinator serializes request intent, active WinRT operation and confirmed OS state. UI reads AppSnapshot::StartupTask and invokes RefreshStartupTask/SetStartWithWindows; an absent optional value means no integration. Accepted means queued/started, not OS success. Only the latest completed request persists confirmed state; a superseded completion starts current intent without publishing obsolete state. Snapshot/event versions fence publication. Native completion posts to retained internal State, never a facade. Settings UI queues each update once and revokes its subscription on native close. Shutdown rejects further owner work and drains foreign commits/observers outside the publication mutex; an already-started OS operation cannot be undone, but a late completion cannot persist or notify.

## UI presentation and delivery

Device picker and options projections consume one immutable AppSnapshot plus localized privacy text. Saved unavailable devices and an orphaned default remain configurable; connected or busy rows survive absent discovery. Picker owns UI state only. Discovery retains application and stop token, holds the view weakly, posts one completion and rejects stale generation or closed-view results. Close cancels before releasing the endpoint. Language, navigation and saved-device expansion rerender even without a device generation change. Picker-open waiters use generation predicates, stop token and deadline; UI never waits for its own Opened event.

UiDispatcher runs UI callers inline. Workers try asynchronous dispatch, then a private HWND message. Its State::Mutex protects fallback queue claims and active PostMessage count; PostMessage, work and capture destruction run unlocked. A claimed entry cannot be rejected by a later failed post. Stop on UI closes admission, drops queued work and waits only for admitted nonblocking posts before HWND destruction. Queued delegates hold weak state. RunAndWait uses completion, cancellation and shutdown events with the remaining deadline, without polling; cancellation before execution differs from an indeterminate running action. Host cancels pipe and stops dispatcher before controller drain.

UiRefreshScheduler owns coalesced flags, one reservation, delivery tickets and render retry. Requests merge while queued, rendering or retrying. Dispatch is asynchronous and false means nothing was accepted. Duplicate or stale callbacks cannot consume a newer pass. UiRefreshCoalescer::m_mutex and TimerMutex are leaf locks; enqueue, render, logging and foreign callbacks run unlocked. The timer lock only arms/disarms native timer. Dispatch failure retains reservation for a 100 ms retry; render failures back off from 100 to 3200 ms. Host excludes the transient-error bit from this retry mask. Stop invalidates tickets, clears flags and disarms, then drains native admission only. Native callbacks retain State and disassociate before posting; UI callbacks hold the scheduler weakly. Admitted render may finish, subject to Host exit checks.

TrayController owns animation, transient error, theme and HWND timers on UI. It renders one AppSnapshot, and Host retains tray across reentrant rendering. Teardown stops timers before releasing flyout; late messages cannot restart them. Host forwards WM_SETTINGCHANGE on UI; theme changes and taskbar recreation refresh presentation. Picker-open acknowledgement uses a separate generation wait mutex and wakes waiters at teardown.

SettingsWindowPresenter is a single noncopyable UI owner. Shared PresenterState retains WindowState across synchronous Close/Closed reentrancy; callbacks hold weak owner/window references. Closed clears only its matching window and revokes its handler. Destruction tries Close, then revokes and abandons a window that cannot close.

## Resource pressure and adaptive policy

ResourcePressureMonitor owns notification handles and native wait/timer/work through WIL. It disarms and drains callbacks before resetting nowait owners, including partial initialization. ControlMutex protects reducer and native arming; native measurements and public handler run unlocked. Probe tickets reject an older slow sample. SetThreadpoolWait/SetThreadpoolTimer under ControlMutex only schedule callbacks and never invoke the handler inline.

Platform exception: LifecycleMutex spans native drain and the public-callback barrier so Start cannot replace a context before Stop finishes. Native callbacks never enter it and disassociate before public delivery. An on-thread callback returns from Start/Stop without self-wait; external Stop drains admitted delivery. RequestProbe retains the active context atomically without LifecycleMutex. Handler captures are moved to one shared object at construction, never copied or destroyed under LifecycleMutex.

AdaptiveResourceController owns pressure observation, pure policy, retry/schedule and ResourceStatusSnapshot. Start/Stop and window messages run on UI. Its snapshot mutex protects authorization sequence and snapshot copy only. Capture authorization briefly, then unlock before logging, tray query, picker preload/release or scheduling; preload can synchronously reenter Snapshot → ResourceStatus. Publication rechecks the fence so stale evaluation cannot restore permission. Stop closes admission, drains the monitor, resets its WIL-owned power notification, invalidates timers and releases tray/dispatcher references. The notification handle uses unique_hpowernotify for explicit Stop and destruction. Native picker calls retain tray and recheck admission afterward. Weak callbacks cannot retain Host. Convert pressure/policy once at publication.

| Input or state | Required transition |
| --- | --- |
| First evaluation | Keep initial residency; seed timing and apply ordinary transitions, including zero delays. |
| Clock moves backward | Preserve residency; reset pressure, healthy, preload and interaction time marks. |
| Memory pressure | Background becomes Cold immediately. |
| Fullscreen, presentation or energy saving | Background becomes Cold after continuous BackgroundPressureToColdDelay. |
| Cold without pressure | Become Warm after ColdToWarmDelay; start preload timing if allowed. |
| Warm with continuous preload permission | Become Hot after WarmToHotDelay; loss of permission restarts timing. |
| Hot without preload permission | Become Warm if resources are absent; otherwise retain loaded Hot until pressure changes residency. |
| Visible/pinned UI or interaction hold | Effective residency is Hot while background transitions continue; release waits for foreground demand to end. |
| Effective Hot with absent resources | Preload for foreground demand, or allowed background preload without pressure. |
| Effective Cold/Warm with loaded resources | Release. |

Schedule generations reject superseded callbacks; failed platform actions use separate bounded retry state.

## Logging, localization and notifications

App constructs Logger → CrashHandlers → ApplicationHost and tears down in reverse. Logger closes admission once, drains admitted calls and its private queue; overflow replaces queued entries and counts drops. A deadline cannot destroy live state: preallocated work retains and joins the worker. EmergencyLog owns immutable path, fixed tail and preallocated dump scratch. TailLock never spans I/O; Dump rejects concurrent entry and try-locks the tail. CrashHandlers alone owns the process registration slot. Native entry increments a seq-cst counter before reading the seq-cst slot; close removes the slot, restores handlers, drains observed entries and releases context. Fatal processing terminates; minidumps remain best effort under heap/stack corruption.

Host publishes StringResources before localized services. Consumers retain shared immutable resources rather than Host. Parse and build candidates unlocked; an invalid English baseline preserves the old map, while missing/invalid regional data falls back to English. Empty keys or non-string values reject the resource. m_lock protects a complete map swap and each individual Get copy, not a multi-lookup transaction. Publish language before tray/settings relocalization on UI; unloaded windows use the current language at Loaded.

NotificationService is constructed and used on UI. Native activation copies arguments and posts through weak service, dispatcher and generation without acquiring the service on the native thread. UI delivery checks teardown/current event and calls the controller once. One AppSnapshot supplies preferences and labels; the service never reads SettingsStore directly. Teardown invalidates generation and detaches manager before native revoke. Initialize/Show retain UI owner across reentrant Windows calls; nested Show declines. Only successful Show commits a status tag, and unique tags prevent stale removal. Cleanup coroutines retain only manager/tag/group/log values.

Only the Store package enables StoreRatingPrompt.enabled. The prompt runs inside a successful picker/settings UI action. SettingsStore marks it asked in one mutation after Windows accepts the notification and stops later usage recording; its own notification group protects it from device-status replacement. Acceptance does not establish visibility.

## Distribution and verification limits

Store/App Installer owns updates; the app performs no release lookup, download or installation. Distribution authorization is in [Releasing](RELEASING.md).

Native test runner isolates LOCALAPPDATA and owns a process-wide WinRT apartment; worker threads initialize their own. Suites link production CoreRuntime and run sequentially. Seeded order and watchdog stress do not enumerate every interleaving. Headless tests cannot establish WinUI rendering, picker/tray/theme behavior, packaged notification activation, Bluetooth hardware transitions or native resource allocation/release. Those require the relevant platform acceptance checks.
