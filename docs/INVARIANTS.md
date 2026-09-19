# Runtime invariants

This document records contracts verified against implementation and tests. It does not certify the whole
application as free of concurrency defects; owners outside the table below still require a complete audit.

## Settings persistence

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
