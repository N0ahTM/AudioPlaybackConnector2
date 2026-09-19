# Runtime invariants

This document records contracts verified against implementation and tests. It does not certify the whole
application as free of concurrency defects; owners outside the table below still require a complete audit.

## Settings persistence

`SettingsStore` owns the only mutable `SettingsData`, revision and persisted revision. Changes stage a candidate
before committing it, and publish immutable snapshots in revision order. Storage never runs under an owner lock.

| State | Execution context and callers | Synchronization | Outgoing callbacks | Cancellation and shutdown |
| --- | --- | --- | --- | --- |
| Data, revisions, load/write admission, persistence schedule and shutdown result | Public methods, admitted loader and persistence worker | `Impl::mutex` | None under the lock | First shutdown closes mutation, load and subscription admission; timeout fences subsequent load/write state commits |
| Publication queue and active publisher identity | Committing thread or the existing publication drainer | `Impl::publicationMutex` | Snapshot handlers run after unlocking | Shutdown disables future handler entry; an executing handler retains its publication until return |
| Subscription activity and executing handler identity | Publisher, reset caller and shutdown caller | `SubscriptionState::Mutex` | None under the lock | Reset disables future entry and drains an already executing handler; self-reset never waits for its own stack |
| Storage path, backend and worker lifetime | Immutable after construction; admitted load/flush and the persistence worker | Shared `Impl` lifetime; `writerActive` excludes overlapping I/O | Storage backend outside all locks | Final flush runs on the worker; a timed-out worker retains its own state until the admitted I/O returns |

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

The shared runner owns a Windows Runtime apartment for the complete suite run. Individual workers still
initialize their own apartments. A suite must not rely on another suite's temporary apartment keeping WinRT
factories alive. The shuffled-order regression seed `314159` exposed a stale JSON activation factory after
temporary apartment teardown; the same seed passes with process lifetime ownership, including AddressSanitizer.

Checks share an atomic failure counter within each suite and report source locations without interleaving
concurrent diagnostic lines. The runner executes suites sequentially; a seed changes only their order.
`scripts/test/run-concurrency-stress.ps1` preserves the seed and output, applies a per-run deadline and an overall
watchdog, and fails on either a nonzero process result or an expired deadline. This is additional evidence beside
the deterministic race scenarios, not exhaustive interleaving coverage.
