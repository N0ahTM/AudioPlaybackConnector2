# Runtime invariants

Owner contracts and verification limits. This is not a whole-application concurrency certification; owners outside the recorded tables still need audit. Read only the affected owner; [Architecture](ARCHITECTURE.md) provides the map.

## Device snapshot authority

DeviceService session snapshots are authoritative for connection/busy state. Delayed presentation events cannot overwrite newer sessions, recreate removed sessions or keep terminal sessions busy. Tray labels, order and privacy use one AppSnapshot without another owner read; unavailable captures retain the display and retry. Command admission may independently recheck busy state.

RefreshDevicesAsync retains the shared service State across the co_await; the DeviceWatcher member is created once and destroyed only with that State, never nulled by StopAndReleaseSessions. DeviceWatcher::RefreshAsync fences post-shutdown entry through IsShutdown and a captured watcher generation, and inventory mutation publishes only through the watcher's serialized executor.

After awaited discovery, reread sessions before merging inventory/saved labels. Snapshot captures settings, complete device state and optional startup publication, then rechecks owner versions: success requires an overlapping stable interval. Bounded retries yield unavailable, never a mixed usable snapshot. Presentation diagnostics are independently sampled.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| DeviceService task queue and drainer identity | Public commands and watcher/timer callbacks post work; the first poster drains on its own thread | `State::QueueMutex` protects only queue bookkeeping | Tasks, platform calls and fact publication run after unlocking | Shutdown is serialized as an owner task; queued work observes `IsShutdown` before changing sessions |
| Sessions, policy, subscription and device generation | Only the serialized DeviceService drainer mutates them | Serial execution, without a second session mutex | Facts reach the subscriber from the drainer, without `QueueMutex` or `SnapshotMutex` | Shutdown stops the watcher, closes sessions, publishes the terminal fact and retires the subscriber |
| Published device snapshot | Drainer publishes; any thread may read | `State::SnapshotMutex` protects the copy and replacement | No callback under this mutex | The terminal snapshot records shutdown; readers receive a value copy |
| Device operation and posted-task completions | Serialized drainer resolves; callers wait or cancel | Each completion's own mutex and condition variable; waits release that mutex | Notifications occur after unlocking | First terminal result wins; shutdown resolves pending operations and stop/deadline can end a caller's wait |
| Watcher inventory, registration and generation | DeviceService's serialized context applies watcher facts; native callbacks and awaited refreshes post back to it | Serialized executor for inventory; atomic generation and stop flags fence cross-thread entry | Fact sink runs from the owner context, not from the native callback | Stop invalidates the generation and registration; shutdown closes admission; stale refresh results cannot publish |
| Device fact presentation fence | Controller fact normalization writes; queued UI presentation checks tokens | `DeviceFactPublicationFence::m_mutex` protects per-device connection/status generations | No callback under the mutex | Changed facts invalidate older UI tokens; exhausted generations reject presentation |

## Controller event delivery

Commands/snapshots lease the concrete SettingsStore and DeviceService. RequestStop monotonically closes command/event admission, invalidates queued recipients and cancels queued policy without waiting; admitted work may finish and retain its committed result. Shutdown requests stop and drains leases, admitted device facts and foreign callbacks outside locks. Device-fact counters use the event mutex only for admission/completion, never store calls. Host cancels transport before controller drain.

Callbacks of admitted work use RequestStop, not the lifecycle join; retain the controller until that work returns. Direct-publication callback destruction invalidates remaining delivery without self-wait; the drainer retains closed event state. Foreign shutdown/reset drains active callbacks. Handler-capture destruction is outside locks; exceptions do not prevent later recipients.

Event revisions increase under the event mutex. Capture recipients at publication and enqueue FIFO; one drainer invokes registration order outside locks. Concurrent/reentrant publication cannot overlap/overtake delivery; later subscribers never receive earlier queued events.

SnapshotAndSubscribe captures a watermark, reads owners unlocked, then atomically validates revision and registers. Intervening publication retries; exhausted retries, stop or invalid handlers return unavailable/no subscription. Callbacks may precede return: consumers serialize initial/events and reject applied revisions. Observation::Revision is an event watermark, not presentation generation.

SettingsChangedEvent carries store revision on the same stream. Status/list/alias/default queries project one validated capture; list refreshes discovery first. Failed capture has no partial fields or adapter fallback read. SettingsRevision/DeviceGeneration record owner versions, not data copies. New device versions/picker acknowledgements advance presentation generation; overtaken readers retry. Resource diagnostics are outside this guarantee.

Controller subscribes directly through weak event state. DeviceFactPublicationFence tracks superseded presentation, not device state. Normalize on the device context without UI/localization. Every actual connected transition persists native name, initial preferences and history before DeviceConnectedEvent; duplicates do not rewrite and empty names preserve saved names. Policy delivery is revisioned and does not wait for the native fact drainer. Facts admitted before stop may finish persistence; later facts cannot start it.

Host marshals once to UI and rechecks the fence before presenting. Its weak presentation boundary only admits UI, acknowledges windows and samples diagnostics. Connection commands await epoch completion; enumeration is completion-notified, cancellable and deadline-capped. Failed discovery falls back to saved labels/current sessions; mutation rechecks original caller context.

Tests cover ordered/reentrant delivery, capture/reset/destruction races, capture invalidation, blocked observers, native-fact persistence without commands, saved-name handling, late callbacks and stop/drain.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Next revision, recipients, pending queue and drainer identity | Controller publishers and the one active drainer | `AppController::EventState::Mutex` | Recipient handlers run after unlocking | Closing rejects registration/publication and discards queued delivery |
| Subscription activity and in-flight admission | Subscriber reset, publisher and shutdown caller | The same event mutex; callback waits release it | No handler under the mutex | Reset disables admission before waiting; reset on the drainer never waits on itself |
| Handler captures and current delivery | Active drainer and subscription owners | Shared registration lifetime | Capture destruction runs unlocked; reentrant registration and reset are allowed | Shutdown drains foreign delivery before releasing captures |
| Controller command admission and presentation watermarks | Public commands, owner facts and snapshot readers | `AppController::m_stateMutex`; snapshots of SettingsStore/DeviceService are captured unlocked and version-checked afterward | No owner call or subscriber callback under this mutex | RequestStop closes admission; Shutdown waits with `m_noActiveCalls` while the mutex is released |
| ApplicationHost composition and teardown | UI thread owns windows and presentation; native callbacks enter through weak/generation gates | UI-thread confinement plus atomic start/exit/result flags, without a host owner mutex | Pipe, controller, WinUI and logger calls follow the explicit teardown order, outside host locks | Exiting closes pipe/UI admission before controller drain, then stops timer/subscription owners and releases windows |

`UiRefreshCoalescer::m_mutex` and the emergency logger's `TailLock` are leaf locks: they protect only local state and are not held across callbacks. SAL marks their guarded fields; the emergency ring methods also require that the caller does not hold `TailLock`. The crash scratch buffer is separately owned by the `Dumping` gate. A focused MSVC ConcurrencyCheck probe reports C26130 for an intentionally unguarded write while the annotated production owners pass; this does not replace the full lock/callback audit.

## Settings persistence

RestoreStartupConnections is an admitted controller action: one validated settings capture, each eligible saved device once, recent-history order then saved order. Global startup enables all saved devices; otherwise use per-device policy. Unknown history is not a target; Submitted means handoff, not connection success. Stop rejects later calls and drains admitted work.

Settings UI uses named controller methods and typed store results. Snapshot/event data replaces extra settings controllers/caches. Host applies language/backdrop on UI and commits final window placement before closing admission; commands drain before the store.

ApplySettingsPolicy is the only incoming/reconnect configuration entry. Submit the complete versioned policy once per settings change; device context stages per-device data before committing a newer revision. Duplicate/older revisions are ignored; semantic no-ops do not republish sessions. Submission does not wait behind foreign observers; queued policy checks the controller stop token before admission. Never call another component under an owner mutex.

SettingsStore owns immutable SettingsData, revision and persisted revision. Stage the candidate, immutable allocation and publication before no-throw commit; allocation failure cannot partially publish. Writer captures reference+revision under lock without allocation, then serializes/writes unlocked. Old writers retain their data; old success cannot mark a newer revision persisted. Failure retries the then-current revision. Public snapshots copy a captured immutable reference outside the lock.

Only schemaVersion 2 is accepted/emitted; no historical migration. Whole-input validation rejects unknown/duplicate keys or identities, wrong types and invalid limits; missing optional fields use defaults. JSON DOM stays private. Only file/path-not-found means absent. Empty/unreadable files are failures, not permission to overwrite. Failed preservation sets preservationFailed before releasing load admission: mutations reject, flush fails for that instance; mutations wait for load before deciding. Successful preservation permits defaults.

Worker captures wake version, inspects state, unlocks then waits. Active load/writer or clean state waits for a signal; eligible dirty state waits for debounce/retry. Versioned Wait retains pre-entry notifications. SettingsStoreWakeup is always called outside store/publication locks; manual clock/barriers stay in tests. Shutdown uses real steady time even if persistence time is frozen.

Required nesting order: store → publication → subscription mutex. Callbacks run without these locks. Subscribe stages the callback state and unsubscribe handle before registering; allocation failure destroys external captures after the store lock unwinds. Reset releases subscription lock before erasing under store lock; shutdown retires registrations before destroying captures.

Shutdown(mode, maximumAttempts, timeBudget) has one steady deadline across worker, load/write, another executor and publication drain. Attempts bound writes, not time. First caller chooses flush/discard and attempts; later callers have independent waiting budgets without changing policy. Callback shutdown never self-waits. Success means persistence and foreign callbacks drained; false means persistence failure or incomplete drain. Admission remains closed either way.

Admitted synchronous I/O may finish on disk after timeout, but cannot mutate public state or start another write. Executing handlers retain accessed dependencies; subscription reset remains a separate potentially waiting drain. Facade/load/flush/publication/worker retain Impl independently; worker never captures facade. Normal shutdown joins the finished worker; timeout releases its join handle while state survives until worker exit, without cycle or implicit destructor join. Executor initialization failure fails construction, never falls back to UI-thread I/O. FlushNow remains synchronous: retry count does not bound I/O, and callers hold no application locks.

SettingsStoreTests cover staging/versions, backup failure and real sharing violation, debounce/lost wakeups, blocked load/write/publication/wait, concurrent budgets, late fences, callback reentrancy/destruction and eventual state/storage release. Controller/device tests cover startup ordering, settings events and out-of-order/cancelled policy.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Data, revisions, load/write admission, persistence schedule and shutdown result | Public methods, admitted loader and persistence worker | `Impl::mutex` | None under the lock | First shutdown closes mutation, load and subscription admission; timeout fences subsequent load/write state commits |
| Publication queue and active publisher identity | Committing thread or the existing publication drainer | `Impl::publicationMutex` | Snapshot handlers run after unlocking | Shutdown disables future handler entry; an executing handler retains its publication until return |
| Subscription activity and executing handler identity | Publisher, reset caller and shutdown caller | `SubscriptionState::Mutex` | None under the lock | Reset disables future entry and drains an already executing handler; self-reset never waits for its own stack |
| Storage path, backend and worker lifetime | Immutable after construction; admitted load/flush and the persistence worker | Shared `Impl` lifetime; `writerActive` excludes overlapping I/O | Storage backend outside all locks | Final flush runs on the worker; a timed-out worker retains its own state until the admitted I/O returns |
| Persistence clock, wake version and platform wait | One worker waits; mutations, load/write completion and shutdown signal it | The system wakeup's own mutex/condition variable, never nested with store locks | No callbacks | Notify changes the version; Wait checks that version before sleeping, retaining notifications that arrive before wait entry |

## Named-pipe response retention

ControlCommandAdapter admits one mutating command with an atomic flag and returns Busy to concurrent mutations. The flag spans dispatch and response formatting; no adapter lock is held while the controller or localization callbacks run. Read-only commands remain concurrent.

WIL owns each pipe instance, I/O, work and timers. Callback contexts retain the instance until Stop cancels I/O and drains groups. *_nowait owners only close; Stop/recreation/destructor explicitly drain. Recreation moves old owners out under slot lock, then cancels/drains unlocked.

The lifecycle mutex protects handler admission and stop state. TryStart reserves `m_starting`, then prepares security and native pipe/threadpool handles unlocked. Stop closes admission and waits with the mutex released until preparation has either published its instances or destroyed unpublished handles. RequestStop and Stop copy the stop source under the mutex, then notify stop callbacks unlocked. Stop drains in-flight handlers, retires the stored handler under the mutex and destroys its captures unlocked while other starters still wait. Reentrant Stop/Start from that stopping thread returns without self-wait; other callers wait until capture destruction finishes.

Platform exception: Options::ConnectPipe and DisconnectNamedPipe run under the slot lock to keep the native connection state and OVERLAPPED/I/O reservation together. The server never flushes a pipe while holding that lock; Stop cancels and drains I/O before disconnecting. Initial connection arms also hold the lifecycle lock so Stop cannot detach the instance vector while startup iterates it. ConnectPipe must support concurrent slots, return immediately, never throw/reenter, and preserve ConnectNamedPipe error semantics. Tests close a client before native admission and inject eight ERROR_RETRY results through ordinary balancing/backoff/recreation.

The per-slot deadline timer is cancelled and drained under `StateMutex` before a failed or completed transfer can reuse its OVERLAPPED state. Its callback only calls `CancelIoEx`; it never takes `StateMutex`, invokes a handler or waits. The slot lock prevents another transfer from rearming the timer during that drain. `Stop` cancels timers under the slot locks and drains them after releasing those locks. Silent-client deadline, malformed-request recovery and concurrent-stop tests exercise these paths.

Request mutex owns request records, pending/active deliveries and byte accounting. Register pending delivery before dispatch; acquire completed response+active delivery atomically under that same mutex. Newly executed responses acquire delivery at completion commit. Handler transfers active ownership to the instance, then drops pending registration; failed dispatch releases directly, normal dispatch on ACK/disconnect. No second ownership lookup; conflicting requests never own canonical delivery.

The pipe slot may acquire the request mutex during cleanup; request admission releases the request mutex before any slot cleanup, including counter saturation. This keeps the slot → request lock order consistent.

Pending/active delivery prevents TTL/pressure eviction even after another client's ACK. Real-pipe pressure test blocks a duplicate 64 KiB response behind 4 KiB buffering, ACKs the first, observes Busy for another request, then verifies intact response, single execution and restored capacity.

CacheNow is monotonic/concurrent and sampled outside request and pipe-slot locks; completion paths pass the sampled time through locked state transitions. SetCacheTimer runs under the request lock and must not wait, throw or invoke callbacks inline. Defaults are steady_clock/SetThreadpoolTimer. Server owns/drains the native timer. Tests freeze time across real ticks, advance both retention TTLs, require timer idle before another request/Stop, then verify the entire byte budget and reexecution after expiry. No private cache-reading hook.

CoreRuntime compiles the server once; tests link identical production layout without test macros.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Start/stop admission, handler and pipe-instance collection | Public lifecycle calls and retry/deferred-stop callbacks | `m_lifecycleMutex`; native start preparation and handler capture destruction happen unlocked | No handler or log callback under this mutex | Stop closes admission, requests stop unlocked and drains all detached instances before retiring the handler |
| Per-instance pipe, OVERLAPPED phase and deadlines | Native I/O, timer and handler-work callbacks | Each `PipeInstance::StateMutex`; slot cleanup may acquire `m_requestMutex` afterward | Handler and trust check run unlocked; the documented native connect/disconnect and deadline-timer operations stay under the slot lock | Stop disarms under slot locks, then drains callbacks and I/O unlocked before destroying instances |
| Correlation records, pending deliveries and cache byte budget | Handler work, acknowledgement/I/O completion and prune timer | `m_requestMutex`; no path acquires a slot lock while holding it | No handler, clock or log call under the mutex; the documented nonblocking timer submission is the platform exception | Active deliveries cannot be evicted; Stop clears pending ownership after handlers drain, and the server destructor drains the prune timer |
| Control adapter mutation admission | Concurrent validated CLI requests | `ControlCommandAdapter::m_mutationActive` atomic gate; read-only calls need no gate | Controller and localization calls run after admission, without an adapter lock | The gate is released after response formatting; server stop token/deadline reaches the controller action |

Each handled command initializes its own util::RuntimeApartment on the arriving threadpool thread; apartments are never shared across commands. The thread_local active-server pointer is the reentrancy key for in-process handler callbacks: a handler that reenters Stop defers it, and a destructor meeting itself terminates instead of double-draining.

CLI endpoint discovery uses one locally owned waitable timer per attempt, with the connection and
overall deadlines bounding every wait. A replay also waits on its retained server process handle;
process exit returns ServerChanged. No callback or extra worker owns the timer, and destruction closes it.

## Power recovery

Native callbacks retain shared state, never the facade; production/tests share the scheduling contract and CoreRuntime object with pinned WinRT. Schedule failure delivers once immediately. Otherwise tick every ten seconds, allow one delivery, stop after acknowledgement or six actual attempts per target. Skips retain budget; duplicate IDs/completions neither double-count nor unlock later work. Ordered private vector owns targets/counters; suspend cycles retain targets and reset budgets.

Cancellation fences later mutations; admitted delivery may finish unlocked. Host rechecks generation on UI before device work. Retained callbacks/late completion access only invalidated shared state. Logging/capture destruction run unlocked. Native callbacks initialize an apartment and retain context before disassociating; cancellation disarms queued work and drains admission, not already-disassociated foreign delivery.

PowerTransitionCoordinatorTests cover schedule failure, single-flight, attempt/skip/ACK accounting, duplicates, stale completion, cancellation, suspend retention, capture reentrancy and post-destruction callbacks. A real-timer semaphore test cancels blocked delivery and releases late completion under watchdog. Coalescer tests cover merge/cancel/scheduling/lost-wakeup.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Suspend flag, duplicate-resume time and owned timer cancellation handle | Host UI context, including destruction | UI-thread confinement, without a coordinator lock | Resume-device callback and scheduling run outside `ResumeState::Mutex` | Cancel invalidates recovery before disarming the schedule |
| Pending recovery targets, attempts, generation and active delivery sequence | Host lifecycle methods, timer callbacks and reconnect completions | `ResumeState::Mutex`, never nested | Reconnect callback and logging run after unlocking | Only the current generation and its one outstanding delivery may record attempts |
| Native periodic timer and immutable delivery callback | Cancellation handle and admitted threadpool callbacks | Shared context lifetime; Windows drains callbacks before cancellation returns | Callback retains context and disassociates before delivering | Timer closes after the final reference is released; stale delivery cannot mutate recovery |

## Device operation completion

Connect/reconnect return admission plus epoch completion, resolved only by the serial device owner. First terminal result is immutable through later disconnect/replacement/shutdown. Pending completions and service hold only weak cross-references, leaving no waiter ownership for detached calls.

WaitForCompletion uses a predicate condition variable, stop token and steady deadline; no polling/coroutine. Published terminal result wins over later cancellation/deadline. Reject waits from the serialized publisher. Notify completion before foreign fact subscribers and outside queue/snapshot locks.

Only an accepted command caller owns epoch cancellation; coalesced callers may stop waiting without cancelling it. Cancellation enqueues an epoch-fenced mutation and rechecks terminal state, without waiting behind a publisher. An idle caller may drain native work, so native-call duration is not bounded. Shutdown cancels pending completion.

Tests cover retained success, shutdown, coalesced/deadline cancellation, reentrant waits and cancellation behind blocked subscribers.

## Startup task ownership

UI reads AppSnapshot::StartupTask and sends RefreshStartupTask/SetStartWithWindows. Missing optional value means no startup integration. Accepted means queued/started, not OS success. Snapshot/events carry authoritative busy/result state; captures fence coordinator publication and advance presentation generation. Settings UI queues each update once and revokes subscription on native close.

Only the latest completed request persists known OS state. Superseded completion starts latest intent without obsolete publication. Commit may reenter/stop; foreign shutdown drains admitted commits. An already-started OS operation cannot be undone; late completion only releases lifetime. Tests cover supersession/coalescing, query failures, reentrant commit/shutdown, foreign drain, destruction during set and application events.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Latest request, active operation and confirmed OS state | `StartupTaskCoordinator::State` serial drainer | Serial execution, without a second policy mutex | Query, set and persistence callbacks run from the drainer | Only queued owner work may start an OS operation or accept completion; stop rejects the next step |
| Published snapshot, queue admission, registrations and active delivery | Request posters, subscribers and the drainer | `State::Mutex`; short capture/update only | Observer callbacks and capture destruction run unlocked | Shutdown closes admission, discards queued work and waits for foreign delivery without holding the mutex |
| WinRT operations | Coroutine continuations post to the owner | Shared internal State across suspension; no facade capture | Native completion only submits owner work | After shutdown, late completion cannot launch the next query, persist or notify |
| Persistence and observer delivery | Serial drainer and admitted handlers | Owner ordering; no lock held across foreign calls | Commit may reenter and observers may unsubscribe themselves | Reentrant shutdown invalidates remaining work without self-wait; foreign unsubscribe drains its active callback |

## Device picker presentation

Device inventory completion/availability and generation come from the device owner. Pure BuildDevicePickerViewState/BuildDeviceOptionsViewState consume immutable AppSnapshot plus localized privacy text. Saved unavailable devices/orphaned default stay configurable; connected/busy rows survive absent discovery. Projection tests use real controller captures and cover privacy, rediscovery, unavailable/retained values.

Picker owns only presentation/UI state. Threadpool discovery holds application+stop token, never view; bounded cancellable query dispatches once, checks cancellation, then resolves weak view. Close cancels before releasing endpoint. Old completion cannot clear a new spinner or render old results. No dedicated worker, pending-device map, global reservation or expiry timer.

Commands return after device admission; bulk actions immediately render snapshot. Native failure stays busy through close/cooldown, then clears with new generation. Language/navigation/saved-device expansion rerender even at unchanged generation. Real controller/projection tests cover admission and cooldown.

Picker-open control callers use generation condition-variable predicates, mutated under wait mutex and notified afterward. Stop/deadline wake timed wait; teardown before acknowledgement yields indeterminate. UI callers never wait for their own Opened dispatcher event; detached tray activation stays nonblocking. Actual WinUI rendering/dispatch/flyout lifecycle still requires platform acceptance; headless tests do not prove it.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Picker presentation, expansion and busy rendering | UI event handlers consume one AppSnapshot | UI-thread confinement; no copied device owner state | Controller actions and WinUI rendering run on UI | Close cancels discovery; generation/weak-view checks discard old completion |
| In-flight picker discovery | Threadpool query and UI completion | Application/stop token retained across query; UI endpoint held weakly | Completed discovery posts once to UI without owning the view | Cancellation/deadline bounds the query; late result cannot mutate a closed or newer picker |

## Distribution

Store/App Installer owns updates; the app performs no release lookup, download or installation. Working-branch builds/analysis do not authorize public releases, feed promotion or Store submission. See [Releasing](RELEASING.md).

## Native test process

Before suites, runner isolates LOCALAPPDATA in a unique process temp directory; real logger diagnostics remain there. Tests do not replace DebugTrace/startup methods at link time; backends use production dependency contracts.

Runner owns a WinRT apartment for all suites; individual workers initialize theirs. Suite-local apartment lifetime cannot keep factories alive for later suites. Seed 314159 reproduced stale JSON factory lifetime; process ownership passed that seed including ASan.

DeviceServiceTests cover commands, completions, facts and settings policy; DeviceReconnectTests cover retry/close barriers; DeviceIncomingTests cover listener coexistence; DevicePowerTests cover suspend/resume recovery. They share DeviceTestFixture rather than copies of backend setup. Controller normalization and device-event lifetime tests belong to AppControllerTests and use the existing AppTestFixture. Every suite links the production CoreRuntime.

Suites run sequentially; seed shuffles suite order, not all interleavings. Atomic failure counters and serialized source-location output support concurrent assertions. scripts/test/run-concurrency-stress.ps1 retains seed/output and enforces per-process/global watchdog; timeout or nonzero exit fails. Deterministic race tests remain necessary.

## UI resource evaluation and snapshot reentrancy

ResourcePressureMonitor owns memory-notification handles and threadpool wait/timer/work objects with WIL. It first disarms and explicitly drains native callbacks, then resets the nowait threadpool owners and notification handles. The nowait deleters only close resources; public callback draining remains a separate lifetime boundary. Partial initialization follows the same shutdown path. RequestProbe retains the active context atomically without taking the lifecycle mutex: a public callback can observe stopped admission while an external Stop waits for it to finish.

The external handler is moved into one shared callback object at construction. Run contexts retain that object by shared ownership, so Start/Stop never copy user captures or run their destructors while holding `LifecycleMutex`. A focused lifecycle test rejects callback copies after construction.

Native pressure, activity and power reads run outside `ControlMutex`. Probe tickets prevent an older, slower read from overwriting a later-started sample. The mutex protects reducer state and native wait/timer arming; `SetThreadpoolWait` and `SetThreadpoolTimer` only schedule callbacks and do not invoke the public handler inline. Shutdown disarms under this mutex and drains callbacks after unlocking.

Platform exception: `LifecycleMutex` spans native drain and the public-callback barrier to prevent a new Start from replacing a context before Stop finishes. Native callbacks do not enter this mutex; they disassociate before public delivery. A callback on its own thread returns from Start/Stop without waiting for itself, and RequestProbe uses the atomic context instead. The self-stop, external-stop and callback-probe tests exercise these transitions.

AdaptiveResourceController owns pressure observation, policy, retry/schedule and the published ResourceStatusSnapshot. Its snapshot mutex protects only pressure authorization sequence and snapshot copies. The host retains the owner from construction through final controller reads; Start/Stop and window messages run on UI. Stop closes admission, drains the monitor, resets the WIL-owned power notification, invalidates timers and releases tray/dispatcher references. The notification handle uses unique_hpowernotify for both explicit Stop and destruction. Weak callbacks cannot retain the host or form a tray cycle. Evaluation retains the tray across native picker calls, which can reenter Stop; it checks admission again before publishing or scheduling. Convert policy/pressure once on publication; readers copy the same value. Pure policy has no host retry/scheduling dependencies.

Capture authorization briefly, then unlock before logging, tray query, picker preload/release or UI scheduling. Later pressure queues another evaluation; publication rechecks fence so stale evaluation cannot restore positive authorization. Preload → AppController::Snapshot → ResourceStatus takes the same mutex, so holding it across preload self-deadlocks. Evidence is source/build inspection; headless tests do not instantiate this UI path.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Resource monitor lifecycle and active run context | Start/Stop and deferred native stop | `LifecycleMutex` serializes replacement/drain; atomic active context admits probes | Public handler runs outside the mutex; documented native drain is the exception | Stop invalidates the epoch, disarms and drains the retiring context before replacement |
| Probe reduction, pending snapshots and native arm state | Native wait/timer callbacks | Run-context `ControlMutex`; probe tickets reject older samples | Native measurements and public handler run unlocked | Shutdown disarms under lock, then drains native callbacks after unlocking |
| Public callback count | Admitted monitor delivery and external Stop | `PublicCallbackMutex`; the wait releases it | Handler runs without this mutex | External Stop waits for admitted delivery; self-stop defers its own drain |
| Resource authorization and published status | Monitor callback, UI evaluation and snapshot readers | `AdaptiveResourceController::m_snapshotMutex` protects authorization sequence and snapshot copy | Logging, tray/picker and dispatch calls run unlocked | Stop closes admission and invalidates timers; stale evaluations cannot restore authorization |

## Logging ownership and native crash registration

App constructs Logger → CrashHandlers → ApplicationHost; teardown reverses order. Weak LogSink owns neither worker nor app. Shutdown closes admission once, admitted calls finish, then private spdlog queue drains. Overflow replaces queued entries and counts drops; queued flush is best effort. Successful Shutdown acknowledges drain.

Deadline never destroys live state: preallocated Windows work retains it, joins worker and releases sink. Emergency path/buffer survive until cleanup even after App's bounded wait. EmergencyLog owns immutable path and fixed 100-record tail; Dump uses preallocated scratch, rejects concurrent dumps and try-locks snapshot. No tail lock spans I/O; no async logger/queue access.

Only CrashHandlers and native callbacks access the process registration slot. Construction/destruction follow serialized App lifetime. Callback increments entry count before reading slot; both and owner close are seq_cst. Owner closes slot, restores previous handlers, drains entries, then releases context. Closed-slot callbacks cannot access it; observed-context callbacks retain protection. Fatal processing terminates; an active fatal dump may delay teardown until exit. DbgHelp/minidump is best effort, not guaranteed under corrupted heap/stack.

LoggerTests cover overflow, concurrent shutdown, late calls, rotation/failure/Unicode, emergency output after destruction, and real exclusive-oplock blocked rotation with both retained/destroyed owners followed by cleanup. No fake product sink. CrashHandlerTests run 25 registration/restoration cycles and five fatal child paths (terminate, abort, invalid parameter, unhandled SEH, vectored) after Logger destruction; assert exit, tail, minidump signature and exception code. Corrupt-heap/exhausted-stack behavior is not simulated.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Logger admission, active calls and shutdown result | Application and worker log callers; owner shutdown | Logger state `Mutex` and condition variable; wait releases the mutex | spdlog/I/O runs after admission, without the mutex | Shutdown closes admission, drains active calls and retains state if the bounded wait expires |
| Emergency log tail and dump scratch | Any emergency caller and fatal dump path | `TailLock` protects ring entries; atomic `Dumping` owns scratch | File I/O runs after the tail lock is released | Emergency state survives until the dump/cleanup work completes |
| Process crash registration | Serialized application owner and native exception callbacks | Seq-cst slot and entry counter, without an owner mutex | Fatal processing runs after retaining the context | Owner closes the slot, restores handlers and drains observed entries before release |

## Tray theme delivery

TrayController owns the connecting animation, transient-error deadline/text and their two HWND timers on UI. It derives each refresh from one AppSnapshot and the existing tooltip builder; no device-status cache or extra worker is introduced. Host forwards timer messages and coalesces requested refreshes, retaining the tray owner across reentrant rendering. Teardown closes admission and stops both timers before hiding the flyout; late timer messages cannot restart them. Native animation/error-expiry acceptance remains open.

Host forwards WM_SETTINGCHANGE directly to TrayController on UI. Tray owns initial/last theme; ImmersiveColorSet changes refresh it. Delivery/teardown share UI; no global registry, mutex, callback lease or cross-thread unsubscribe. Taskbar recreation forces refresh. GetSystemTheme is stateless. Source/build verification exists; interactive theme switching remains a runtime check.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Tray animation, error state, theme and HWND timers | UI messages and host presentation calls | UI-thread confinement; one shared atomic backdrop preference is retained by the menu's Opened handler | Controller reads and native UI calls occur on UI | Teardown stops timers and closes presentation before the flyout is released |
| Picker-open acknowledgement | UI Opened event publishes; control callers wait | `m_pickerOpenedMutex` and generation/teardown atomics; wait releases the mutex | No UI call under the wait mutex | Teardown wakes waiters; stop/deadline bounds each wait |

## Localization ownership

Host owns/publishes StringResources, loading English+overlay before localized services. Consumers hold shared_ptr<const StringResources>; command adapter retains text, not host/app/logger. Instances are independent.

Parse/convert/build candidates outside publication lock. Invalid baseline preserves old map; missing/invalid regional data keeps English. Empty keys/non-string values reject the resource. Short swap publishes complete map; Get copies under reader lock. Guarantee is per lookup, not multiple background reads.

Publish language before tray/settings relocalization on UI. Selection handler does not relocalize against old resources; unloaded windows update pending language and use current resources at Loaded. Tests embed all eight real resources and cover fallback, concurrent replacement, independence and retained lifetime. WinUI language/layout remains interactive acceptance.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Current localized string map | UI Initialize publishes; UI/worker consumers call Get | `StringResources::m_lock` swaps a complete candidate and protects individual lookup copies | Parsing, logging and UI relocalization run unlocked | Consumers retain the resource instance; publication replaces the map without exposing partial contents |

## Notification ownership

Host constructs/calls/destroys NotificationService on UI. Native activation captures dispatcher, weak service, generation and weak log sink; copies arguments to UI without acquiring service on native thread. Queued delivery checks generation/teardown before calling the controller directly; no service mutex or second dispatcher hop. NotificationService holds a weak controller reference, reads preferences and device labels from one AppSnapshot per event, and never reads SettingsStore or calls host policy callbacks. The host checks event currency before delivery.

The Store package alone includes `StoreRatingPrompt.enabled`; the shared executable checks that explicit package feature instead of inferring a distribution channel from its identity. The rating prompt runs within a successful picker/settings UI action, never on the command worker after the UI dispatcher returns. SettingsStore marks it asked in one mutation after Windows accepts the notification, preserving concurrent usage updates; subsequent connections stop recording rating-prompt usage. The prompt uses its own notification group so later device status toasts cannot remove it. Acceptance by Windows does not prove the notification was visible to the user.

Teardown invalidates generation and detaches manager before native revoke. Initialize/Show retain UI owner across Windows calls that may pump messages; late registration cannot republish after teardown. Nested Show is declined. Last status tag commits only on success; unique tags prevent stale removal of newer notifications. Cleanup coroutines retain manager/tag/group/log values only.

ToastContentBuilderTests link the production CoreRuntime builder. They cover Unicode/delimiter round trips, a fixed 1,000-case generated corpus (seed 920040), exactly-once percent decoding, malformed percent input, missing targets and the existing XML sanitizer cases. Windows XmlDocument independently parses generated toasts to verify element counts, attribute/text recovery, action argument round trips and optional/audio replacement behavior. These checks do not instantiate AppNotificationManager. Packaged/unpackaged activation, shutdown during delivery and interactive notification behavior remain runtime checks.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Notification manager, status tags and presentation admission | Host and queued activation handling on UI | UI-thread confinement and generation checks; no service mutex | Controller snapshot/actions and native toast APIs run on UI | Teardown invalidates generation, detaches manager and revokes native registration |
| Native activation payload | Windows callback copies arguments before UI dispatch | Weak service and generation token, without a service lock | One queued UI delivery performs the controller action | Late activation loses its weak owner or fails the generation fence |

## Settings window ownership

ApplicationHost owns one noncopyable SettingsWindowPresenter on UI. The facade has one shared PresenterState solely to retain the current window across synchronous Close/Closed reentrancy. Show and Close retain their WindowState while Windows may invoke Closed; the event callback holds weak owner/window references, so it cannot keep either alive. Closed clears only the matching current window and revokes its handler. Destruction tries Close, then revokes and abandons a window that cannot close. ShowHelp and language updates use the same UI context. Standalone header compilation verifies that neither copying nor moving the facade is allowed; actual WinUI close/reentrancy remains a native acceptance check.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Current settings window and close registration | Host and WinUI Closed callback on UI | UI-thread confinement; PresenterState retains WindowState across reentrant close | WinUI close/help/language calls occur with a retained window, without an owner mutex | Closed clears only the matching window; destruction revokes and abandons one that cannot close |

## UI refresh scheduling

UiDispatcher owns general UI delivery. UI callers run inline; workers try the asynchronous dispatcher, then a private HWND message. Native fallback entries have a claim bit so a failed post cannot reject work already taken by UI. Queue claims and posting count are mutex-protected; PostMessage, callbacks and capture destruction run unlocked. Stop runs on UI, closes admission, drops queued work and waits only for admitted nonblocking PostMessage calls (the condition-variable wait releases the mutex), so the host can then destroy its HWND. Already queued dispatcher delegates hold weak state and do nothing after Stop/destruction. The host requests pipe cancellation and stops the dispatcher before controller draining; this wakes admitted UI-action waiters without waiting for queued UI work. Producers arriving later are rejected. Native message-window tests cover concurrent posts, post failure, stop racing producers, late dispatch and reentrant capture destruction. RunAndWait uses completion, caller-cancellation and persistent shutdown events with the remaining deadline, without periodic polling. The gate distinguishes cancellation before execution from an indeterminate running action; UI admission independently checks cancellation/deadline. Tests cover inline failure, deferred success, deadline, cancellation, reentrant shutdown during execution and multiple shutdown waiters.

Host constructs UiRefreshScheduler with asynchronous serial UI dispatcher and weak render callback; then only Request/Stop. CoreRuntime implementation has one native threadpool timer, no dedicated worker/HWND fallback. Timer allocation failure fails construction. Dispatch rejection/exception keeps reservation, retrying after 100 ms; render retry backs off 100–3200 ms. Host retry mask excludes transient-error bit.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| UiDispatcher fallback queue, claims and active PostMessage calls | Worker producers post; UI thread claims and runs work | `State::Mutex`; waits release it | Dispatcher, PostMessage, work and capture destruction run unlocked | Stop rejects producers, discards queued work and waits for admitted nonblocking posts before HWND teardown |
| UI refresh coalescing, tickets and render retries | Any requester, timer callback and serial UI drain | `UiRefreshCoalescer::m_mutex` for pending flags; atomic tickets fence superseded dispatches; failure count belongs to UI | Enqueue and render run outside the coalescer and timer locks | Stop cancels pending flags/tickets; weak dispatched work cannot render after state release |
| UI refresh native timer | Request/retry and timer callback | `State::TimerMutex` serializes native arm/disarm only | Timer callback retains state, disassociates and posts to UI after unlocking | Stop disarms under the mutex, then drains the timer outside it |
| One pending UI control action | UI claimant and cancelling/deadline caller | `ControlUiActionGate` atomics; Pending-to-Running has one winner | Action executes after admission without a gate lock | Cancellation wins only before execution; a running action is reported indeterminate until completion |

Coalescer owns flags+one reservation through Request/BeginDrain/CompleteDrain/Cancel. Requests merge while queued/rendering/retrying. Delivery tickets admit once; duplicate/old callbacks cannot consume newer passes. UI context owns rendering/failure count. Dispatch must enqueue asynchronously; false means nothing accepted. Native timer lock protects arm/disarm only; no dispatch/render/logging/foreign callback under timer/coalescer locks.

Stop closes admission, invalidates ticket, clears flags and disarms. Drain only native admission, never UI or admitted rendering. Native callbacks retain state before disassociating, so blocked dispatch may outlive facade; late failures cannot rearm. UI callbacks hold weak scheduler. Admitted rendering may finish; host independently checks exit state.

Tests use production timer/injected UI queue for merge, render-time requests, duplicates, dispatch/render retries, exceptions, reentrant Stop, Request/Stop races, queued post-destruction work and destruction during blocked native dispatch. Interactive WinUI/tray acceptance remains separate.

## Adaptive resource policy transitions

AdaptiveResourceController owns and serializes the pure policy, retry backoff and schedule state on UI. Policy evaluation alone seeds missing time marks; startup needs no separate initialization path. Schedule generations reject superseded callbacks, and failed platform actions use the separate bounded retry state.

| Input / current state | Transition and timing |
| --- | --- |
| First evaluation | Keep configured initial residency; begin pressure or healthy timing from this evaluation. Apply ordinary transitions, including zero delays. |
| Clock moves backward | Preserve residency; discard pressure, healthy, preload and interaction time marks. Evaluate current input with fresh timing. |
| Memory pressure | Background becomes Cold immediately. |
| Fullscreen/presentation or energy saving | Background becomes Cold after continuous BackgroundPressureToColdDelay. |
| Cold, pressure absent | Become Warm after ColdToWarmDelay; begin preload timing at that transition if allowed. |
| Warm, preload allowed continuously | Become Hot after WarmToHotDelay. Loss of permission restarts preload timing. |
| Hot, preload disallowed and resources absent/uninitialized | Become Warm; otherwise retain loaded Hot resources until pressure changes residency. |
| Visible/pinned UI or interaction hold | Effective residency is Hot; background transitions still proceed. Release is deferred until foreground demand ends. |
| Effective Hot, resources absent/uninitialized | Request preload for foreground demand, or when background preload is allowed without pressure. |
| Effective Cold/Warm, resources loaded | Request release. |

Existing AdaptiveResourcePolicy tests exercise startup, delays, pressure, foreground holds, resource actions and backward-clock timing. Native allocation/release and timer delivery are separate controller acceptance checks.
