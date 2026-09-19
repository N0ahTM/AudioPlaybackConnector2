# Runtime invariants

This document records contracts verified against implementation and tests. It does not certify the whole
application as free of concurrency defects; owners outside the table below still require a complete audit.

## Device snapshot authority

Application snapshots obtain connection state and busy flags from the DeviceService session snapshot.
Presentation facts advance publication and describe events; they do not maintain a second runtime device map.
A delayed failed, connected or retry event cannot overwrite a newer session state, recreate a removed session,
or keep a terminal session busy. Tray busy state, connected labels and privacy come from one captured application snapshot. The tooltip
uses its prepared display names, in the same order as the device list; it performs no additional owner read.
An unavailable capture leaves the displayed tray state intact and uses the existing refresh retry.
Command admission may independently consult the current busy state; that read does not alter the snapshot.

After an awaited inventory refresh, session state is read again before merging discovery and saved labels.
The session read therefore cannot predate a disconnect that completed during enumeration. `Snapshot()` reads
settings and the complete device-owner snapshot, projects inventory, sessions and saved labels, then checks
the owner versions again, including the startup-task publication when present. A successful capture has an overlapping stable interval for these owners.
Bounded retries return an unavailable snapshot under continuous mutation, rather than a mixed usable value.
Presentation diagnostics are read at their own boundary; they are not part of this owner-version atomicity claim.

## Controller event delivery

The controller retains concrete `SettingsStore` and `DeviceService` owners. Commands and snapshots acquire
an admission lease before accessing them. `RequestStop` monotonically closes command and event admission,
invalidates queued recipients and cancels pending settings policy without waiting for calls or observers.
It is safe inside an admitted call's observer; that call may still finish and retain its committed result.
`Shutdown` requests stop, then drains outstanding leases, admitted device-fact processing and event delivery
without holding owner locks. A device fact admitted before stop may finish its settings commit; later facts
cannot start one. The device-fact counter uses the event mutex only for admission and completion, never for
store calls or observer delivery.
This lifecycle join must run outside admitted controller calls and owner/observer callbacks; such callbacks
use `RequestStop`. The caller retains the controller until admitted calls return. Tests cover a stop request
inside a settings commit callback, a foreign blocked callback and the subsequent lifecycle join. A separate
test blocks the settings observer of a native connection without an active controller command and verifies
that shutdown still waits for that admitted commit.
The host requests transport cancellation before
draining the controller. The presentation boundary is weakly held and contains only UI admission, window
acknowledgement and presentation diagnostics; it cannot mutate device or persistence state.

Connection commands await the native device completion record. Inventory enumeration uses a completion
notification with the caller's cancellation token and a capped deadline. Failed discovery falls back to saved
labels and current sessions; mutation admission rechecks the original caller context after resolution. The
controller tests exercise real owners with fake platform boundaries, including cancellation during enumeration,
session changes during enumeration, committed settings after reentrant writes and pipe-driven shutdown.

Each controller assigns monotonically increasing publication revisions under its event mutex. Publication
captures the current recipients and enters one FIFO queue. One drainer invokes handlers in registration order,
outside the mutex. Concurrent and reentrant publications enqueue without overlapping the current callback or
overtaking earlier events. A subscriber added after publication cannot receive that older queued event.

| State | Owner and synchronization | Lifetime boundary |
| --- | --- | --- |
| Next revision, recipients, pending queue and drainer identity | `AppController::EventState::Mutex` | Closing rejects registration and publication and discards queued delivery |
| Subscription activity and in-flight admission | The same event mutex | Reset disables admission before waiting for an active callback; reset on the drainer never waits on itself |
| Handler captures and current delivery | Shared registration and the one active drainer | Capture destruction happens outside the mutex; reentrant registration and reset are allowed |

Controller shutdown closes all registrations and drains a callback on another thread. For direct publication
outside an admitted command or native fact, destruction from the callback itself invalidates remaining
recipients and queued work without waiting for its own stack; the drainer retains the closed event state until
it returns. Callbacks of admitted work must use `RequestStop` and retain the controller until that work returns.
Observer exceptions do not interrupt subsequent recipients.
The tests exercise simultaneous and reentrant publication, admission timing, reset with queued work, both
destruction paths and reentrant capture destruction.

`SnapshotAndSubscribe` captures the publication revision, obtains a snapshot without holding the event mutex,
then atomically checks the revision and registers. An intervening publication retries the capture; a later
publication includes the new recipient. Exhausted retries, shutdown and invalid handlers return no subscription
and an unavailable snapshot. An already queued event cannot acquire the new recipient retroactively. The
returned `Observation::Revision` is an event watermark, distinct from the snapshot's presentation generation.
Callbacks may run before the method returns. Consumers apply the initial value and notifications on one context
and ignore revisions they have already applied; the host does this on its UI context.

Settings commits publish `SettingsChangedEvent` through the same ordered stream as device facts. Each carries
the store revision and requires no second settings cache. Regression tests mutate both owners during capture,
retain a blocked older delivery, force repeated capture invalidation, and overlap capture with shutdown.
Status, device listing, alias listing and default-device queries project their fields from the same validated
capture as `Snapshot()`. Device listing refreshes discovery first; subsequent capture uses the device owner's
inventory rather than a second WinRT enumeration result. Failed capture returns an unavailable query without
partial fields. The control adapter formats that result without taking a separate fallback snapshot.

Each successful snapshot records `SettingsRevision` and `DeviceGeneration`. The controller retains version
numbers, not a second copy of the owner data. An observed newer device version or picker acknowledgement
advances the presentation generation; stable rereads retain it. A reader overtaken by a newer capture retries
instead of stamping stale data with the newer generation. Application and tray projections share that generation.
Resource diagnostics remain an independently sampled presentation field, outside this generation guarantee.

The controller subscribes directly to the concrete `DeviceService` fact stream. Its private event state owns
`DeviceFactPublicationFence`; there is no separate router or device-state history map. The fence records only
connection/status publication history and identifies superseded presentation work. Session snapshots remain
the authority for application device state. Normalization runs on the device context without localization or
UI dispatch. Events carry typed failure reasons and disconnect notification intent.

Each actual transition into connected state records the native name, initial device preferences and recent
connection history in `SettingsStore` before publishing `DeviceConnectedEvent`. Repeated connected facts do
not repeat the mutation. An empty native name preserves a saved name. The store returns its ordinary mutation
result; presentation visibility and effective reconnect policy are not persistence results. Settings changes
reach the device owner through the existing revisioned policy subscription, without waiting on the native
fact drainer. The host only presents the resulting connection event. Tests verify headless persistence before
notification, repeated facts, callback stop requests and suppression of persistence after shutdown.

The host subscribes to the controller and marshals each notification once to its UI context. Before presenting
device work it validates the notification's fence token; a queued connected event cannot revive a disconnected
device's presentation. The controller unsubscribes from the source at destruction, and the source callback
captures only weak event state. Integration tests exercise opaque external IDs, loss versus manual disconnect,
reconnect entry, typed platform failure and callbacks after controller destruction.

## Settings persistence

Startup restoration is an admitted controller action. It reads one validated settings snapshot and submits
eligible saved devices exactly once per request, ordered by recent connection history and then saved order.
The global startup preference includes every saved device; otherwise each device's preference controls
eligibility. Unknown history entries do not become connection targets. `Submitted` describes handoff to the
device owner, not successful Bluetooth connection. Stop rejects later restoration calls; an admitted call is
drained with the other controller actions. Integration tests check empty and disabled settings, both policy
modes, recent ordering, inclusion of devices without history and stop rejection.

Settings UI mutations enter through named `AppController` methods and return the store's typed mutation result.
The application snapshot includes the settings value from its validated owner capture. There is no separate
settings controller or presentation callback slot. The shared settings event carries the language/backdrop
presentation inputs; the host applies these on its UI context. The host closes the settings window and commits
its final placement before closing application admission, then drains commands before shutting down the store.

`ApplySettingsPolicy` is the sole service entry point for incoming/reconnect configuration; there are no
unversioned setters. Device tests use this same contract, including policy changes during blocked delivery.
The application subscribes once to settings changes and submits the complete incoming/reconnect policy to
`DeviceService`. The device context applies only a newer settings revision. It stages the per-device set before
committing the version and flags, and does not republish session changes for semantically unchanged policy.
Submission never waits behind a foreign device observer. Controller shutdown cancels queued policy through an
owned stop token; the device context checks that token before admission. No owner mutex is held while calling
another component. Tests cover out-of-order revisions, duplicates, no-op policy, per-device overrides, blocked
delivery, cancelled queued work and settings actions after shutdown.

`SettingsStore` owns the current immutable `SettingsData` reference, revision and persisted revision. Changes stage
a private mutable candidate, allocate its immutable representation, and stage subscriber publication before the
no-throw commit. Allocation failure during staging cannot publish a partial revision. Storage never runs under an
owner lock.

An admitted writer captures the immutable data reference and matching revision under the store lock. Reference
capture cannot allocate or throw; there is no separate copying-failure/retry path. A later mutation replaces the
current reference while the old writer retains its original data until completion. Serialization and I/O happen
after unlocking. A failed write retries against the then-current committed revision, and an old successful write
cannot mark a newer revision as persisted. Public snapshots remain independent value objects: the reader captures
the data reference and revision under lock, then copies the immutable data after unlocking.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Data, revisions, load/write admission, persistence schedule and shutdown result | Public methods, admitted loader and persistence worker | `Impl::mutex` | None under the lock | First shutdown closes mutation, load and subscription admission; timeout fences subsequent load/write state commits |
| Publication queue and active publisher identity | Committing thread or the existing publication drainer | `Impl::publicationMutex` | Snapshot handlers run after unlocking | Shutdown disables future handler entry; an executing handler retains its publication until return |
| Subscription activity and executing handler identity | Publisher, reset caller and shutdown caller | `SubscriptionState::Mutex` | None under the lock | Reset disables future entry and drains an already executing handler; self-reset never waits for its own stack |
| Storage path, backend and worker lifetime | Immutable after construction; admitted load/flush and the persistence worker | Shared `Impl` lifetime; `writerActive` excludes overlapping I/O | Storage backend outside all locks | Final flush runs on the worker; a timed-out worker retains its own state until the admitted I/O returns |
| Persistence clock, wake version and platform wait | One worker waits; mutations, load/write completion and shutdown signal it | The system wakeup's own mutex/condition variable, never nested with store locks | No callbacks | Notify changes the version; Wait checks that version before sleeping, retaining notifications that arrive before wait entry |

Settings use exactly one format: `schemaVersion: 2`. Unversioned or differently versioned files are
not migrated, including individually recognizable preferences. The stateless codec validates the complete
candidate before the store commits it. Unknown keys, duplicate keys/identities, wrong types and invalid limits
reject the whole input. Missing optional fields use current defaults. The writer always emits version 2.
The JSON dependency stays in implementation files; no library DOM crosses the store boundary.

Only file/path-not-found means absent settings. An empty file or a failed open is a load failure, not permission
to overwrite the path with defaults. The storage boundary reports whether preservation succeeded. A failed
preservation sets `preservationFailed` under `Impl::mutex` before releasing load admission; all subsequent
mutations are rejected and flush returns failure for that store instance. No mutation can race this decision:
it waits for the active load first. A successfully preserved file allows normal default-based persistence.
Tests exercise both the injected backup failure and a real Windows sharing violation, then verify unchanged
original bytes after the external file lock is released.

The worker captures a wake version, inspects store state, and releases the store lock before entering the platform
wait. A blocked writer, active load or clean store produces an indefinite signal wait, not a deadline that has
already expired. Eligible dirty state waits until the debounce/retry deadline. `SettingsStoreWakeup` provides the
clock and versioned wait contract; every call to it occurs outside store/publication locks. Production and tests
link the same worker from `CoreRuntime`. The manual clock and wait barriers live solely in the test project.

Deterministic tests advance through the debounce boundary, park behind a synchronous writer after the deadline,
and notify between state inspection and wait entry. Shutdown always measures its budget with the real steady
clock, even when the persistence clock is frozen. A delayed platform wait retains the worker's state after a
shutdown timeout and releases it when the wait finally returns.

Where nesting is required the order is store mutex, publication mutex, subscription mutex. No code acquires an
earlier lock while holding a later one. Publication invokes handlers without any of these locks. Subscription
reset releases its subscription lock before erasing registration under the store lock. Shutdown moves retired
registrations out of the store before destroying callback captures.

`Shutdown(mode, maximumAttempts, timeBudget)` uses one steady-clock deadline for the worker, admitted load or
write, another shutdown executor, and publication drain. Attempts limit writes; they do not define the time
budget. The first caller determines flush versus discard and the write attempt limit. Later callers have their
own waiting budget and cannot change that policy. A callback may initiate shutdown without waiting on itself.

Successful shutdown means the final persistence operation and callbacks on other threads have drained. `false`
means persistence failed or some work did not drain within the budget. Admission stays closed in either case.
An already admitted synchronous filesystem operation cannot be forcibly unwound: it may finish its disk write,
but its late completion cannot change the public snapshot or start another write. An executing user handler also
cannot be forcibly unwound; captures must retain any dependencies it accesses until it returns. Subscription
reset remains a separate draining boundary and can wait for such a handler.

The facade and every admitted load/flush/publication retain `Impl` independently. The worker captures `Impl`,
never the facade. Normally shutdown joins the finished worker. At timeout it releases the join handle while the
worker retains its state; worker exit then releases that reference, without an ownership cycle or an implicit
`jthread` join in the facade destructor. A store whose persistence executor cannot initialize fails construction;
it does not silently switch to synchronous filesystem calls on UI or callback threads.

`SettingsStoreTests.cpp` covers blocked load, blocked final write, blocked publication, independent concurrent
shutdown budgets, late completion fences, callback reentrancy, callback-driven facade destruction and eventual
storage release after timeout. `FlushNow` is still an explicit synchronous I/O boundary: its retry count does not
bound platform I/O duration. Callers must not hold application locks when invoking it.

## Named-pipe response retention

Each pipe instance owns its pipe, threadpool I/O, work items and timers through WIL. Callback contexts retain the
instance address until Stop has cancelled I/O and drained the callback groups. The `*_nowait` owners deliberately
only close resources: waiting is explicit in Stop, slot recreation and the server destructor. Slot recreation
moves the old pipe and I/O owners out under the state lock, then cancels and drains them after unlocking.

`CommandLineControlServer` owns request records, pending handler deliveries and cache-byte accounting under its
request mutex. A complete request registers its pending delivery before its handler work is submitted. Pending
deliveries prevent eviction while that work is queued or waiting for another execution of the same request.

Selecting a completed response and acquiring its active delivery happen under the same request lock. A newly
executed response acquires that delivery when completion is committed. The handler callback then drops its
pending registration and transfers the active delivery to the pipe instance. Failed dispatch releases it directly;
normal dispatch releases it when the ACK arrives or the client disconnects. There is no second cache lookup to
establish ownership after returning the response. A conflicting request never owns the canonical delivery.

An active or pending delivery prevents both TTL pruning and eviction under cache pressure, including after another
client acknowledges the same result. The real-pipe cache-pressure test holds a duplicate's 64 KiB response behind
a 4 KiB pipe buffer, completes the other client's ACK, attempts another request, and then drains the retained body.
It verifies a Busy response under pressure, an intact duplicate response, exactly one execution of that request,
and restored cache capacity after both deliveries finish.

## Power recovery

| State | Execution context and callers | Synchronization | Cancellation and shutdown |
| --- | --- | --- | --- |
| Suspend flag, duplicate-resume time and owned timer cancellation handle | Host UI context, including destruction | No cross-thread access | Cancel invalidates recovery before disarming the schedule |
| Pending recovery targets, attempts, generation and active delivery sequence | Host lifecycle methods, timer callbacks and reconnect completions | `ResumeState::Mutex`, never nested | Only the current generation and its one outstanding delivery may record attempts |
| Native periodic timer and immutable delivery callback | Cancellation handle and admitted threadpool callbacks | Shared context lifetime; Windows drains callbacks before cancellation returns | Callback retains its context before disassociating; timer closes after the final reference is released |

Timer callbacks never capture the coordinator facade. The native timer uses one platform implementation and the
same injectable scheduling contract in production and tests. The coordinator tests link its `CoreRuntime` object
with the same pinned C++/WinRT projection instead of compiling a test variant. A failed schedule delivers once
immediately. A working schedule ticks every ten seconds, admits one reconnect delivery at a time and stops after pending targets
are acknowledged or each target has six actual attempts. Skipped targets retain their budget. Duplicate IDs in
one completion count once; duplicate completions cannot consume budget or unlock a later delivery.

Cancellation fences later state changes; a delivery already admitted may finish outside the state lock. Host
delivery rechecks the recovery generation on the UI context before starting a device operation. Late completions
and retained timer callbacks can outlive the facade and access only shared, invalidated recovery state. Callback
captures are retired outside the lock, including on suspend and cancellation. Logging also runs unlocked.

`AppWorkCoordinatorTests.cpp` covers scheduler failure, single-flight delivery, retry exhaustion, duplicate and
stale completions, cancellation during blocked delivery, capture-destructor reentrancy and callbacks retained
after facade destruction. A real native-timer test blocks an admitted delivery with semaphores, cancels the timer,
and releases its late completion under the process watchdog. Native callbacks initialize their own runtime
apartment. Cancellation disarms queued callbacks and drains their admission phase; it does not wait for foreign
delivery code that has already been disassociated.

## Device operation completion

Connect and reconnect return an epoch-bound completion alongside command admission. The serialized device owner
alone resolves that completion. Its first terminal outcome is immutable: subsequent disconnect, replacement or
shutdown cannot turn an already successful command into a failure. Pending completions retain only a weak owner
reference; the service retains only weak completion references. Detached calls therefore leave no pending waiter
ownership behind.

`WaitForCompletion` uses a predicate-protected condition variable with a stop token and a steady-clock deadline.
There is no timer polling or WinRT coroutine between connection commands and the application caller. A published
terminal result wins over a later cancellation or expired deadline. Waiting from the serialized publisher itself
is rejected. Completion notification precedes the foreign fact subscriber and runs without the queue or snapshot
lock held.

Only a caller whose command was accepted owns cancellation of the underlying epoch. A coalesced caller may stop
waiting without cancelling the shared operation. Cancellation queues an epoch-fenced mutation without waiting
behind another publisher; the mutation rechecks terminal completion before acting. If the context is idle, the
caller drains the queue as for other device commands, so this does not promise a bound on native platform calls.
Shutdown resolves all pending completions as cancelled. Deterministic tests cover retained success, shutdown,
coalesced cancellation, deadline cancellation, reentrant waits and cancellation behind a blocked subscriber.

## Startup task ownership

The UI reads `AppSnapshot::StartupTask` and sends `RefreshStartupTask` / `SetStartWithWindows` to the application
controller. An absent optional value means the composition has no startup integration. Accepted means queued or
started, not OS success; authoritative state, busy state and failure are observed in the snapshot and event stream.
The controller fences captures against the coordinator publication and includes changes in presentation generation.
The settings window queues each update once to its dispatcher and revokes its subscription on native window close.

| State | Owner and synchronization | Lifetime boundary |
| --- | --- | --- |
| Latest request, active operation and confirmed OS state | `StartupTaskCoordinator::State` serial drainer; request-state mutex is never nested under the owner mutex | Only queued owner work may start a set/query or accept a completion |
| Published snapshot, queue admission, registrations and active delivery | `State::Mutex`; short capture/update only | Shutdown closes admission and discards queued work; foreign draining is awaited without holding the mutex |
| WinRT operations | Shared internal State across coroutine suspension; no facade capture | Continuations post to the owner; after shutdown they cannot launch the next query, persist or notify |
| Persistence and observer callbacks | Serial drainer, outside all owner locks | Reentrant requests enqueue; reentrant shutdown invalidates remaining work without waiting on itself; foreign unsubscribe drains its active callback |

Only the latest completed request may persist a known OS state. Superseded results start the latest queued intent
without publishing their obsolete result. The commit callback can reenter or stop the owner. Foreign shutdown waits
for an already admitted commit, so none continues after it returns. Shutdown cannot undo a Windows call that already
started; its late completion only releases internal lifetime. Tests cover supersession, coalescing, failed queries,
reentrant commit/shutdown, foreign commit drain, facade destruction during a set and application-event integration.

## Device picker presentation

The device owner supplies inventory completion and availability together with its captured generation. Pure
`BuildDevicePickerViewState` / `BuildDeviceOptionsViewState` functions read only an immutable application snapshot
and an explicit localized privacy label. Saved unavailable devices and an orphaned persisted default remain
configurable; connected/busy devices remain visible independently of discovery. Projection tests exercise real
controller captures, privacy, rediscovery, unavailable captures and retained snapshot values.

The WinUI picker owns only presentation values and UI-thread state. Its discovery request runs the bounded,
cancellable controller query on the Windows threadpool; it creates no dedicated worker. Background work holds
an application reference and a stop token, never the view. Completion marshals once to the UI dispatcher and
checks cancellation before resolving the weak view. Closing/releasing the view requests cancellation before
resetting the application endpoint. An old cancelled completion cannot clear a new request's spinner or render
its result. WinUI rendering and dispatcher failure still require platform acceptance; core projection tests do
not prove the complete window lifecycle.

## Distribution

Store and Windows App Installer own application updates. The application performs no release lookup, update
download or installation. Build and analysis on a working branch must not publish a release, promote an App
Installer feed or submit a Store package. Release promotion is a separate operation.

## Native test process

Before invoking any suite the runner isolates `LOCALAPPDATA` in a unique temporary directory for that process.
Runtime logging uses its production implementation and retains diagnostic files there. Tests do not replace
`DebugTrace` or the startup controller's methods at link time; coordinator tests inject their backend through the
same explicit dependency contract used by the runtime.

The shared runner owns a Windows Runtime apartment for the complete suite run. Individual workers still
initialize their own apartments. A suite must not rely on another suite's temporary apartment keeping WinRT
factories alive. The shuffled-order regression seed `314159` exposed a stale JSON activation factory after
temporary apartment teardown; the same seed passes with process lifetime ownership, including AddressSanitizer.

Checks share an atomic failure counter within each suite and report source locations without interleaving
concurrent diagnostic lines. The runner executes suites sequentially; a seed changes only their order.
`scripts/test/run-concurrency-stress.ps1` preserves the seed and output, applies a per-run deadline and an overall
watchdog, and fails on either a nonzero process result or an expired deadline. This is additional evidence beside
the deterministic race scenarios, not exhaustive interleaving coverage.

## UI resource evaluation and snapshot reentrancy

The host's resource-authorization mutex protects the constrained-pressure sequence and published diagnostics.
Evaluation captures the current authorization in a short critical section, then releases it before logging,
querying tray state, preloading/releasing the picker or scheduling UI work. The capture admits that evaluation;
a pressure observation arriving afterward queues another UI evaluation. Published diagnostics recheck the fence
under the mutex, so a superseded evaluation cannot restore positive authorization.

This matters because picker preload reads `AppController::Snapshot`, which calls back into the host's
`ResourceStatus` and acquires the same mutex. Holding it across preload would wait on the same thread.
The UI call graph and lock scopes have been inspected; headless tests do not instantiate this WinUI path.
